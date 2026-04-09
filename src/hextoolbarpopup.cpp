#include "hextoolbarpopup.h"
#include "themes/thememanager.h"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QScreen>
#include <QLineEdit>
#include <QIcon>
#include <cmath>

namespace rcx {

static constexpr NodeKind kHexSizes[] = {
    NodeKind::Hex8, NodeKind::Hex16, NodeKind::Hex32,
    NodeKind::Hex64, NodeKind::Hex128
};
static constexpr const char* kSizeLabels[] = {"8","16","32","64","128"};
static constexpr int kNumSizes = 5;

// action codes for hit rects
enum HitAction { HA_Size = 0, HA_Pin = 1, HA_Suggest = 2,
                 HA_InsAbove = 3, HA_InsBelow = 4, HA_JoinSel = 5, HA_FillGo = 6 };

HexToolbarPopup::HexToolbarPopup(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    m_offsetEdit = new QLineEdit(this);
    m_offsetEdit->setPlaceholderText(QStringLiteral("0x"));
    m_offsetEdit->setFixedWidth(60);
    m_offsetEdit->setFixedHeight(18);
    m_offsetEdit->hide();
}

void HexToolbarPopup::setFont(const QFont& font) {
    m_font = font;
    QFont small = font;
    small.setPointSize(qMax(7, font.pointSize() - 1));
    m_offsetEdit->setFont(small);
}

void HexToolbarPopup::setContext(const HexPopupContext& ctx) {
    m_ctx = ctx;
    m_hoveredBtn = -1;
    update();
}

void HexToolbarPopup::popup(const QPoint& globalPos) {
    m_lastPos = globalPos;
    QSize sz = computeSize();
    setFixedSize(sz);
    QRect screenRect = QApplication::screenAt(globalPos)->availableGeometry();
    int x = qBound(screenRect.left(), globalPos.x(), screenRect.right() - sz.width());
    int y = globalPos.y() + 2;
    if (y + sz.height() > screenRect.bottom())
        y = globalPos.y() - sz.height() - 2;
    move(x, y);
    show();
    setFocus();
}

void HexToolbarPopup::togglePin() {
    m_pinned = !m_pinned;
    hide();
    // Change window flags: Tool stays open, Popup auto-dismisses
    setWindowFlags(m_pinned
        ? (Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowStaysOnTopHint)
        : (Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint));
    m_offsetEdit->setVisible(m_pinned);
    QSize sz = computeSize();
    setFixedSize(sz);
    // Re-show at last position
    QRect screenRect = QApplication::screenAt(m_lastPos)->availableGeometry();
    int x = qBound(screenRect.left(), m_lastPos.x(), screenRect.right() - sz.width());
    int y = m_lastPos.y() + 2;
    if (y + sz.height() > screenRect.bottom())
        y = m_lastPos.y() - sz.height() - 2;
    move(x, y);
    show();
    if (m_pinned) setFocus();
}

int HexToolbarPopup::maxJoinableBytes() const {
    int total = sizeForKind(m_ctx.currentKind);
    for (const auto& adj : m_ctx.nexts) {
        if (!adj.exists || !isHexNode(adj.kind) || adj.kind != m_ctx.currentKind)
            break;
        total += sizeForKind(adj.kind);
        if (total >= 16) break;
    }
    return total;
}

QString HexToolbarPopup::hexLine(const char* typeName, const QByteArray& bytes) {
    QString type = QString::fromLatin1(typeName).leftJustified(7, ' ');
    QString ascii;
    for (int i = 0; i < bytes.size(); i++) {
        uint8_t c = (uint8_t)bytes[i];
        ascii += (c >= 0x20 && c < 0x7f) ? QChar(c) : QChar('.');
    }
    QString hex;
    for (int i = 0; i < bytes.size(); i++) {
        if (i > 0) hex += ' ';
        hex += QStringLiteral("%1").arg((uint8_t)bytes[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return QStringLiteral("%1 %2  %3").arg(type, ascii, hex);
}

QString HexToolbarPopup::previewForKind(NodeKind target) const {
    int curSz = sizeForKind(m_ctx.currentKind);
    int tgtSz = sizeForKind(target);
    const char* tgtName = kindMeta(target)->typeName;

    if (tgtSz == curSz)
        return hexLine(tgtName, m_ctx.data);

    if (tgtSz < curSz) {
        int count = curSz / tgtSz;
        QStringList lines;
        for (int i = 0; i < count; i++)
            lines << hexLine(tgtName, m_ctx.data.mid(i * tgtSz, tgtSz));
        return lines.join('\n');
    }

    QByteArray merged = m_ctx.data;
    for (const auto& adj : m_ctx.nexts) {
        if (!adj.exists || adj.kind != m_ctx.currentKind) break;
        merged.append(adj.data);
        if (merged.size() >= tgtSz) break;
    }
    merged = merged.left(tgtSz);
    if (merged.size() < tgtSz)
        merged.append(QByteArray(tgtSz - merged.size(), '\0'));
    return hexLine(tgtName, merged);
}

QString HexToolbarPopup::infoForKind(NodeKind target) const {
    int curSz = sizeForKind(m_ctx.currentKind);
    int tgtSz = sizeForKind(target);
    const char* curName = kindMeta(m_ctx.currentKind)->typeName;
    const char* tgtName = kindMeta(target)->typeName;

    if (tgtSz == curSz) return QStringLiteral("current size");
    if (tgtSz < curSz)
        return QStringLiteral("splits 1 %1 \u2192 %2 %3")
            .arg(curName).arg(curSz / tgtSz).arg(tgtName);
    int needed = tgtSz / curSz;
    if (maxJoinableBytes() >= tgtSz)
        return QStringLiteral("joins %1 %2 \u2192 1 %3")
            .arg(needed).arg(curName).arg(tgtName);
    return QStringLiteral("need %1 adjacent %2 to join")
        .arg(needed - 1).arg(curName);
}

QSize HexToolbarPopup::computeSize() const {
    QFontMetrics fm(m_font);
    int lineH = fm.height() + 2;
    int pad = 4;

    int btnRowW = pad;
    for (int i = 0; i < kNumSizes; i++)
        btnRowW += fm.horizontalAdvance(QString::fromLatin1(kSizeLabels[i])) + 8 + 2;
    btnRowW += 18 + pad;  // pin icon space

    int curSz = sizeForKind(m_ctx.currentKind);
    int previewChars = 8 + curSz + 2 + curSz * 3;
    int previewW = pad + fm.horizontalAdvance(QString(previewChars, QChar('0'))) + pad;

    int w = qMax(btnRowW, previewW);

    // Height: buttons + sep + preview + info
    int previewLines = 1;
    if (m_hoveredBtn >= 0 && m_hoveredBtn < (int)m_hits.size()) {
        for (const auto& h : m_hits) {
            if (h.rect.contains(QPoint(0, 0)) || true) {} // just use hovered kind
        }
        // Find hovered size kind
        if (m_hoveredBtn >= 0 && m_hoveredBtn < kNumSizes) {
            int tgtSz = sizeForKind(kHexSizes[m_hoveredBtn]);
            if (tgtSz < curSz)
                previewLines = qMin(curSz / tgtSz, 8);
        }
    }
    int h = pad + lineH + 4 + previewLines * lineH + lineH + pad;

    // Pinned extras
    if (m_pinned) {
        bool hasSuggestions = m_ctx.hasPtr || m_ctx.hasFloat || m_ctx.hasString;
        if (hasSuggestions) h += lineH + 2;         // suggestion row
        h += lineH + 2;                              // insert above/below row
        if (m_ctx.multiSelectCount > 1) h += lineH + 2;  // join selected row
        h += lineH + 4;                              // fill-to-offset row
    }

    return QSize(qMax(w, 200), h);
}

void HexToolbarPopup::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const auto& t = ThemeManager::instance().current();
    QFontMetrics fm(m_font);
    p.setFont(m_font);

    int lineH = fm.height() + 2;
    int pad = 4;

    // Resize to fit
    QSize needed = computeSize();
    if (size() != needed)
        const_cast<HexToolbarPopup*>(this)->setFixedSize(needed);

    // Background + border
    p.fillRect(rect(), t.backgroundAlt);
    p.fillRect(0, 0, width(), 1, t.border);
    p.fillRect(0, height() - 1, width(), 1, t.border);
    p.fillRect(0, 0, 1, height(), t.border);
    p.fillRect(width() - 1, 0, 1, height(), t.border);

    int x = pad, y = pad;
    m_hits.clear();

    // ── Size buttons ──
    int joinable = maxJoinableBytes();
    for (int i = 0; i < kNumSizes; i++) {
        QString label = QString::fromLatin1(kSizeLabels[i]);
        int btnW = fm.horizontalAdvance(label) + 8;
        QRect r(x, y, btnW, lineH);

        bool isCurrent = (kHexSizes[i] == m_ctx.currentKind);
        bool canDo = (sizeForKind(kHexSizes[i]) <= sizeForKind(m_ctx.currentKind))
                  || (sizeForKind(kHexSizes[i]) <= joinable);
        bool isHovered = false;
        for (const auto& h : m_hits) { (void)h; } // hits not built yet, check via m_hoveredBtn
        // Check if mouse is over this button
        QPoint mp = mapFromGlobal(QCursor::pos());
        isHovered = r.contains(mp) && canDo;

        QColor bg = isCurrent ? t.selected : (isHovered ? t.hover : t.surface);
        p.fillRect(r, bg);

        QColor bd = isCurrent ? t.indHoverSpan : t.border;
        p.fillRect(r.x(), r.y(), r.width(), 1, bd);
        p.fillRect(r.x(), r.bottom(), r.width(), 1, bd);
        p.fillRect(r.x(), r.y(), 1, r.height(), bd);
        p.fillRect(r.right(), r.y(), 1, r.height(), bd);

        p.setPen(isCurrent ? t.indHoverSpan : (canDo ? t.text : t.textFaint));
        p.drawText(r, Qt::AlignCenter, label);

        m_hits.append({r, HA_Size, canDo, kHexSizes[i]});
        x += btnW + 1;
    }

    // ── Pin icon (top-right) ──
    {
        int pinSz = lineH;
        QRect pinR(width() - pad - pinSz, y, pinSz, pinSz);
        QPoint mp = mapFromGlobal(QCursor::pos());
        bool pinHov = pinR.contains(mp);
        p.fillRect(pinR, pinHov ? t.hover : t.backgroundAlt);

        QIcon pinIcon(m_pinned ? QStringLiteral(":/vsicons/pinned.svg")
                               : QStringLiteral(":/vsicons/pin.svg"));
        pinIcon.paint(&p, pinR.adjusted(2, 2, -2, -2));
        m_hits.append({pinR, HA_Pin, true, NodeKind::Hex8});
    }

    y += lineH + 2;
    p.fillRect(pad, y, width() - 2 * pad, 1, t.border);
    y += 3;

    // ── Preview ──
    NodeKind previewKind = m_ctx.currentKind;
    // Find hovered size button
    {
        QPoint mp = mapFromGlobal(QCursor::pos());
        for (const auto& h : m_hits) {
            if (h.action == HA_Size && h.rect.contains(mp) && h.enabled)
                previewKind = h.kind;
        }
    }

    QString preview = previewForKind(previewKind);
    QStringList previewLines = preview.split('\n');
    int maxShow = qMin(previewLines.size(), 8);
    for (int i = 0; i < maxShow; i++) {
        p.setPen(t.text);
        p.drawText(pad, y, width() - 2 * pad, lineH, Qt::AlignVCenter | Qt::AlignLeft, previewLines[i]);
        y += lineH;
    }
    if (previewLines.size() > maxShow) {
        p.setPen(t.textFaint);
        p.drawText(pad, y, width() - 2 * pad, lineH, Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("... +%1 more").arg(previewLines.size() - maxShow));
        y += lineH;
    }

    // Info text
    p.setPen(t.textDim);
    p.drawText(pad, y, width() - 2 * pad, lineH, Qt::AlignVCenter | Qt::AlignLeft, infoForKind(previewKind));
    y += lineH;

    // ── Pinned extras ──
    if (!m_pinned) return;

    y += 2;

    // Smart suggestions
    bool hasSuggestions = m_ctx.hasPtr || m_ctx.hasFloat || m_ctx.hasString;
    if (hasSuggestions) {
        int sx = pad;
        auto drawSugBtn = [&](const QString& label, NodeKind kind) {
            int bw = fm.horizontalAdvance(label) + 8;
            QRect r(sx, y, bw, lineH);
            QPoint mp = mapFromGlobal(QCursor::pos());
            bool hov = r.contains(mp);
            p.fillRect(r, hov ? t.hover : t.surface);
            p.fillRect(r.x(), r.y(), r.width(), 1, t.border);
            p.fillRect(r.x(), r.bottom(), r.width(), 1, t.border);
            p.fillRect(r.x(), r.y(), 1, r.height(), t.border);
            p.fillRect(r.right(), r.y(), 1, r.height(), t.border);
            p.setPen(t.indHoverSpan);
            p.drawText(r, Qt::AlignCenter, label);
            m_hits.append({r, HA_Suggest, true, kind});
            sx += bw + 2;
        };

        if (m_ctx.hasPtr) {
            QString label = m_ctx.ptrSymbol.isEmpty()
                ? QStringLiteral("ptr*")
                : QStringLiteral("ptr* %1").arg(m_ctx.ptrSymbol.left(12));
            drawSugBtn(label, NodeKind::Pointer64);
        }
        if (m_ctx.hasFloat) {
            drawSugBtn(QStringLiteral("float %1f").arg(m_ctx.floatVal, 0, 'g', 4), NodeKind::Float);
        }
        if (m_ctx.hasString) {
            drawSugBtn(QStringLiteral("utf8 \"%1\"").arg(m_ctx.stringPreview.left(6)), NodeKind::UTF8);
        }
        y += lineH + 2;
    }

    // Insert above/below
    {
        int sx = pad;
        auto drawActBtn = [&](const QString& label, int action) {
            int bw = fm.horizontalAdvance(label) + 8;
            QRect r(sx, y, bw, lineH);
            QPoint mp = mapFromGlobal(QCursor::pos());
            bool hov = r.contains(mp);
            p.fillRect(r, hov ? t.hover : t.surface);
            p.fillRect(r.x(), r.y(), r.width(), 1, t.border);
            p.fillRect(r.x(), r.bottom(), r.width(), 1, t.border);
            p.fillRect(r.x(), r.y(), 1, r.height(), t.border);
            p.fillRect(r.right(), r.y(), 1, r.height(), t.border);
            p.setPen(t.text);
            p.drawText(r, Qt::AlignCenter, label);
            m_hits.append({r, action, true, NodeKind::Hex64});
            sx += bw + 2;
        };
        drawActBtn(QStringLiteral("+ hex64 above"), HA_InsAbove);
        drawActBtn(QStringLiteral("+ hex64 below"), HA_InsBelow);
        y += lineH + 2;
    }

    // Multi-select join
    if (m_ctx.multiSelectCount > 1) {
        NodeKind joinKind = NodeKind::Hex8;
        int total = m_ctx.multiSelectBytes;
        if      (total == 16) joinKind = NodeKind::Hex128;
        else if (total == 8)  joinKind = NodeKind::Hex64;
        else if (total == 4)  joinKind = NodeKind::Hex32;
        else if (total == 2)  joinKind = NodeKind::Hex16;

        bool valid = m_ctx.multiSelectContiguous && (total == 2 || total == 4 || total == 8 || total == 16);
        QString label = valid
            ? QStringLiteral("join %1 \u2192 %2").arg(m_ctx.multiSelectCount).arg(kindMeta(joinKind)->typeName)
            : QStringLiteral("join %1 (%2B, not power of 2)").arg(m_ctx.multiSelectCount).arg(total);

        int bw = fm.horizontalAdvance(label) + 8;
        QRect r(pad, y, bw, lineH);
        QPoint mp = mapFromGlobal(QCursor::pos());
        bool hov = r.contains(mp) && valid;
        p.fillRect(r, hov ? t.hover : t.surface);
        p.fillRect(r.x(), r.y(), r.width(), 1, t.border);
        p.fillRect(r.x(), r.bottom(), r.width(), 1, t.border);
        p.fillRect(r.x(), r.y(), 1, r.height(), t.border);
        p.fillRect(r.right(), r.y(), 1, r.height(), t.border);
        p.setPen(valid ? t.text : t.textFaint);
        p.drawText(r, Qt::AlignCenter, label);
        m_hits.append({r, HA_JoinSel, valid, joinKind});
        y += lineH + 2;
    }

    // Fill-to-offset
    {
        p.setPen(t.textDim);
        QString label = QStringLiteral("fill to:");
        int lw = fm.horizontalAdvance(label);
        p.drawText(pad, y, lw, lineH, Qt::AlignVCenter | Qt::AlignLeft, label);

        // Position the line edit
        m_offsetEdit->move(pad + lw + 4, y + (lineH - m_offsetEdit->height()) / 2);
        const auto& theme = t;
        m_offsetEdit->setStyleSheet(QStringLiteral(
            "QLineEdit { background: %1; color: %2; border: 1px solid %3; padding: 0 2px; }")
            .arg(theme.background.name(), theme.text.name(), theme.border.name()));
        m_offsetEdit->show();

        // Go button
        int goX = pad + lw + 4 + m_offsetEdit->width() + 4;
        QRect goR(goX, y, fm.horizontalAdvance("Go") + 8, lineH);
        QPoint mp = mapFromGlobal(QCursor::pos());
        bool hov = goR.contains(mp);
        p.fillRect(goR, hov ? t.hover : t.surface);
        p.fillRect(goR.x(), goR.y(), goR.width(), 1, t.border);
        p.fillRect(goR.x(), goR.bottom(), goR.width(), 1, t.border);
        p.fillRect(goR.x(), goR.y(), 1, goR.height(), t.border);
        p.fillRect(goR.right(), goR.y(), 1, goR.height(), t.border);
        p.setPen(t.text);
        p.drawText(goR, Qt::AlignCenter, QStringLiteral("Go"));
        m_hits.append({goR, HA_FillGo, true, NodeKind::Hex8});
    }
}

void HexToolbarPopup::mouseMoveEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    update();  // repaint to update hover states
}

void HexToolbarPopup::mousePressEvent(QMouseEvent* event) {
    QPoint pos = event->pos();
    for (const auto& h : m_hits) {
        if (!h.rect.contains(pos) || !h.enabled) continue;
        switch (h.action) {
        case HA_Pin:
            togglePin();
            return;
        case HA_Size:
            if (h.kind != m_ctx.currentKind) {
                emit sizeSelected(m_ctx.nodeId, h.kind);
                if (!m_pinned) hide();
            }
            return;
        case HA_Suggest:
            emit sizeSelected(m_ctx.nodeId, h.kind);
            if (!m_pinned) hide();
            return;
        case HA_InsAbove:
            emit insertAbove(m_ctx.nodeId);
            return;
        case HA_InsBelow:
            emit insertBelow(m_ctx.nodeId);
            return;
        case HA_JoinSel:
            emit joinSelected();
            return;
        case HA_FillGo: {
            bool ok;
            int offset = m_offsetEdit->text().toInt(&ok, 16);
            if (ok) emit fillToOffset(m_ctx.nodeId, offset);
            return;
        }
        }
    }
    // Click outside any button: dismiss if not pinned
    if (!m_pinned) {
        hide();
        emit dismissed();
    }
}

void HexToolbarPopup::leaveEvent(QEvent*) {
    update();
}

void HexToolbarPopup::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (m_pinned) togglePin();  // unpin first
        else { hide(); emit dismissed(); }
        return;
    }
    QFrame::keyPressEvent(event);
}

} // namespace rcx
