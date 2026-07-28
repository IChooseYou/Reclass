#include "typeselectorpopup.h"
#include "profiler.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QToolButton>
#include <QButtonGroup>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QIcon>
#include <QApplication>
#include <QScreen>
#include <QIntValidator>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QToolTip>
#include <QHelpEvent>
#include "themes/thememanager.h"
#include "widgets/category_chip.h"
#include "widgets/fuzzy_match.h"
#include "widgets/dialog_button.h"
#include "rcxtooltip.h"
#include "fontutil.h"
#include <QSettings>

namespace rcx {

// ── parseTypeSpec ──

TypeSpec parseTypeSpec(const QString& text) {
    TypeSpec spec;
    QString s = text.trimmed();
    if (s.isEmpty()) return spec;

    // Check for pointer suffix: "Ball*", "Ball**", "Ball***".
    // Chop EVERY trailing '*' (tolerating whitespace between stars, e.g.
    // "Ball * *") so a stray star is never left dangling in the base name —
    // "int***" must yield baseName "int", not "int*". Depth is capped at 2.
    if (s.endsWith('*')) {
        int depth = 0;
        for (;;) {
            QString t = s.trimmed();
            if (!t.endsWith('*')) { s = t; break; }
            t.chop(1);
            s = t;
            ++depth;
        }
        spec.isPointer = true;
        spec.ptrDepth = qBound(1, depth, 2);
        spec.baseName = s.trimmed();
        return spec;
    }

    // Check for array suffix: "int32_t[10]"
    int bracket = s.indexOf('[');
    if (bracket > 0 && s.endsWith(']')) {
        spec.baseName = s.left(bracket).trimmed();
        // Trim so "int32_t[ 10 ]" parses; clamp absurd counts so a typo like
        // "[2000000000]" can't request a multi-GB array span downstream.
        QString countStr = s.mid(bracket + 1, s.size() - bracket - 2).trimmed();
        bool ok;
        int count = countStr.toInt(&ok);
        if (ok && count > 0)
            spec.arrayCount = qMin(count, 1 << 20);
        return spec;
    }

    spec.baseName = s;
    return spec;
}

// Fuzzy scorer lives in widgets/fuzzy_match.h (strict substring + word-start
// initials, replaces an older loose subsequence matcher that produced
// hundreds of false-positive matches in big symbol lists).

// resolvedPointSize() lives in fontutil.h (shared with scannerpanel.cpp) — it
// guards against pixel-sized base fonts collapsing every derived size to the
// 7pt floor. See the include at the top of this file.

// ── Per-group accent colors (derived from theme semantic colors) ──

QColor kindGroupColor(const QString& group) {
    const auto& t = ThemeManager::instance().current();
    if (group == QStringLiteral("Hex"))   return t.indHoverSpan;   // purple
    if (group == QStringLiteral("Int"))   return t.syntaxKeyword;  // blue
    if (group == QStringLiteral("Float")) return t.markerCycle;    // amber
    if (group == QStringLiteral("Ptr"))   return t.markerPtr;      // red
    if (group == QStringLiteral("Vec"))   return t.syntaxType;     // teal
    if (group == QStringLiteral("Str"))   return t.syntaxString;   // salmon
    if (group == QStringLiteral("Ctr"))   return t.indDataChanged; // green
    if (group == QStringLiteral("Common")) return t.syntaxPreproc;  // grey
    return t.text;
}

QColor kindGroupDimColor(const QString& group) {
    QColor c = kindGroupColor(group);
    // Blend 18% accent + 82% theme background so the muted variant
    // tracks the active palette instead of being baked against a dark
    // #1e1e1e. The old constant looked right under VS2022 dark, but on
    // a light theme it baked a dirty dark tint into every group chip.
    const QColor bg = ThemeManager::instance().current().background;
    return QColor(
        (c.red()   * 18 + bg.red()   * 82) / 100,
        (c.green() * 18 + bg.green() * 82) / 100,
        (c.blue()  * 18 + bg.blue()  * 82) / 100);
}

QString kindGroupFor(NodeKind k) {
    if (isHexNode(k))                     return QStringLiteral("Hex");
    if (k == NodeKind::Int8  || k == NodeKind::Int16  ||
        k == NodeKind::Int32 || k == NodeKind::Int64  || k == NodeKind::Int128 ||
        k == NodeKind::UInt8  || k == NodeKind::UInt16 ||
        k == NodeKind::UInt32 || k == NodeKind::UInt64 || k == NodeKind::UInt128 ||
        k == NodeKind::Bool)              return QStringLiteral("Int");
    if (k == NodeKind::Float16 || k == NodeKind::Float || k == NodeKind::Double)  return QStringLiteral("Float");
    if (isPointerKind(k) || isFuncPtr(k)) return QStringLiteral("Ptr");
    if (isVectorKind(k) || isMatrixKind(k)) return QStringLiteral("Vec");
    if (isStringKind(k))                  return QStringLiteral("Str");
    if (isContainerKind(k))               return QStringLiteral("Ctr");
    return QStringLiteral("Hex");
}

// ── Custom delegate: group-colored rows + size bar + fuzzy highlight ──

class TypeSelectorDelegate : public QStyledItemDelegate {
public:
    explicit TypeSelectorDelegate(TypeSelectorPopup* popup, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_popup(popup) {}

    // Hide the field-summary tooltip if it's currently visible. The
    // tooltip is a top-level Qt::ToolTip window owned by the delegate,
    // not by the popup's list view, so closing the popup leaves it
    // stranded on screen until we explicitly dismiss it here.
    void hideTip() {
        sharedRcxTooltip()->hide();
    }

    void setFont(const QFont& f) {
        m_font = f;
        m_fm = QFontMetrics(f);
        m_smallFont = f;
        m_smallFont.setPointSize(qMax(7, resolvedPointSize(f) - 2));
        m_sfm = QFontMetrics(m_smallFont);
        recomputeLayout();
        updateCachedSizeHint();
    }
    void setLoading(bool v) { m_isLoading = v; }
    void setCompact(bool v) { m_compact = v; updateCachedSizeHint(); }
    void setFilteredTypes(const QVector<TypeEntry>* filtered) {
        m_filtered = filtered;
    }
    void setMatchPositions(const QVector<QVector<int>>* mp) {
        m_matchPositions = mp;
    }
    void setCurrentEntry(const TypeEntry* entry, bool hasCurrent) {
        m_currentEntry = entry;
        m_hasCurrent = hasCurrent;
    }

    // Column layout, font-scaled. Earlier rev had these as static constexpr
    // — fine at the default 10 pt font, but at high zoom the left side grew
    // (icon + name + composite suffix all use fm.height() / fm.advance) while
    // these stayed fixed at their 540-px-popup baseline. Result: names elided
    // into the bar/size text on the right. Deriving from m_fm.height() means
    // every pixel constant scales in lockstep with the font.
    int m_barW   = 32;   // size-bar width
    int m_maxSz  = 64;   // size-bar normalization ceiling (logical bytes)
    int m_szCol  = 38;   // size-text column width
    int m_accent = 2;    // left accent stripe width
    int m_padL   = 4;    // left inset before icon
    int m_pad    = 4;    // tight gap (icon ↔ name)
    int m_gap    = 6;    // loose gap (between right-side columns: bar / size / pad)
    int m_indent = 16;   // item inset under a section header (the "tab")
    void recomputeLayout() {
        const int h = m_fm.height();
        // Scale linearly off the default 10 pt JetBrains Mono row
        // (h≈14). At zoom 0 every value here equals its old constant;
        // at higher zoom each grows proportionally, keeping the popup's
        // visual rhythm consistent instead of letting the font outrun
        // the layout.
        m_accent = qMax(2, h * 2 / 14);
        m_padL   = qMax(4, h * 4 / 14);
        m_pad    = qMax(4, h * 4 / 14);
        m_gap    = qMax(6, h * 6 / 14);
        m_indent = qMax(16, h * 16 / 14);
        m_barW   = qMax(32, h * 32 / 14);
        m_szCol  = m_sfm.horizontalAdvance(QStringLiteral("9999B")) + 4;
        // m_maxSz is a logical normalization value, not a pixel; keep at 64
        // so the bar still represents 0..64 bytes regardless of zoom.
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        const auto& t = ThemeManager::instance().current();
        const int row = index.row();

        // ── Loading skeleton ──
        if (m_isLoading) {
            int barH = m_fm.height() - 2;
            int x0 = option.rect.x() + m_fm.height() + 6;
            int y0 = option.rect.y() + (option.rect.height() - barH) / 2;
            int barW = 40 + ((row * 73 + 29) % 100);
            painter->setPen(Qt::NoPen);
            painter->setBrush(t.surface);
            painter->drawRoundedRect(x0, y0, barW, barH, 2, 2);
            painter->restore();
            return;
        }

        const TypeEntry* entry = (m_filtered && row >= 0 && row < m_filtered->size())
                                     ? &(*m_filtered)[row] : nullptr;
        const bool isSection  = entry && entry->entryKind == TypeEntry::Section;
        const bool isDisabled = entry && !entry->enabled && !isSection;
        const bool isSel      = (option.state & QStyle::State_Selected) && !isSection;
        const bool isHov      = (option.state & QStyle::State_MouseOver) && !isSection;

        const QColor groupCol = entry ? kindGroupColor(entry->kindGroup) : t.text;

        const QRect r = option.rect;
        const int y = r.y(), h = r.height();
        const int baseline = y + (h + m_fm.ascent() - m_fm.descent()) / 2;  // true text baseline

        // ── Background ──
        if (isSel) {
            painter->fillRect(r, t.selected);
            painter->fillRect(r.x(), y, m_accent, h, groupCol);
        } else if (isHov && !isSection && !isDisabled) {
            painter->fillRect(r, t.hover);
        }

        // ── Section header: chevron + pip + bold title, with a separator
        // rule above so groups read as distinct bands. The title uses the
        // MAIN font (bold) — not the small font — so headers anchor the
        // hierarchy instead of looking smaller than the items beneath them.
        if (isSection) {
            const bool collap = entry && entry->sectionCollapsible;
            // Separator rule above every header except the very first row, so
            // sections are clearly divided.
            if (row > 0)
                painter->fillRect(r.x(), y, r.width(), 1, t.border);
            // Collapsible headers are interactive — selection / hover feedback.
            if (collap) {
                if (option.state & QStyle::State_Selected)
                    painter->fillRect(r, t.selected);
                else if (option.state & QStyle::State_MouseOver)
                    painter->fillRect(r, t.hover);
            }
            QFont hf = m_font;
            hf.setBold(true);
            painter->setFont(hf);
            const int hbl = y + (h + m_fm.ascent() - m_fm.descent()) / 2;
            int px = r.x() + m_accent + m_padL + 1;
            // ▾ expanded / ▸ collapsed chevron (collapsible only).
            if (collap) {
                painter->setPen(t.textDim);
                painter->drawText(px, hbl, entry->sectionExpanded
                    ? QStringLiteral("▾") : QStringLiteral("▸"));
                px += m_fm.horizontalAdvance(QStringLiteral("▸")) + m_pad;
            }
            const int pipSz = qMax(4, m_fm.height() / 3);
            painter->fillRect(px, y + (h - pipSz) / 2, pipSz, pipSz, groupCol);
            painter->setPen(t.textDim);
            const int tx = px + pipSz + m_pad;
            painter->drawText(tx, hbl, index.data().toString());
            painter->restore();
            return;
        }

        // ── "Create new" action row: green ＋ class icon + label, no size /
        // keyword columns. The selection/hover background above already ran
        // (it's not a section), so the row highlights like any other. ──
        if (entry && entry->isCreateNew) {
            const QColor accent = t.indHintGreen;
            int xx = r.x() + m_accent + m_padL + m_indent;
            const int iconSz = m_fm.height();
            static QIcon createI(QStringLiteral(":/vsicons/symbol-class.svg"));
            QPixmap pm = createI.pixmap(iconSz, iconSz);
            if (!pm.isNull()) {
                QPainter pp(&pm);
                pp.setCompositionMode(QPainter::CompositionMode_SourceIn);
                pp.fillRect(pm.rect(), accent);
                pp.end();
                painter->drawPixmap(xx, y + (h - iconSz) / 2, pm);
            }
            xx += iconSz + m_pad;
            painter->setFont(m_font);
            painter->setPen(accent);
            const QString label = index.data().toString();
            painter->drawText(xx, baseline,
                m_fm.elidedText(label, Qt::ElideRight, r.right() - xx - m_pad));
            painter->restore();
            return;
        }

        // ── Columns ── items are indented one "tab" so they read as children
        // of the section header above them.
        int x = r.x() + m_accent + m_padL + m_indent;
        const int rightW = m_gap + m_barW + m_gap + m_szCol + m_gap;
        const int nameEnd = r.right() - rightW;

        // ── Icon ── tinted with the row's group color so a "Hex" entry
        // reads as purple end-to-end (accent stripe, section pip, AND
        // icon), an "Int" entry as blue, etc. Without tinting, every row
        // got the same neutral SVG fill and lost its group cue past the
        // 2 px accent stripe.
        const int iconSz = m_fm.height();
        if (entry) {
            static QIcon sI(QStringLiteral(":/vsicons/symbol-class.svg"));
            static QIcon eI(QStringLiteral(":/vsicons/symbol-enum.svg"));
            static QIcon pI(QStringLiteral(":/vsicons/symbol-variable.svg"));
            const QIcon& ico = (entry->entryKind == TypeEntry::Composite)
                ? (entry->category == TypeEntry::CatEnum ? eI : sI) : pI;
            const int iy = y + (h - iconSz) / 2;
            QPixmap pm = ico.pixmap(iconSz, iconSz);
            if (!pm.isNull()) {
                QPainter pp(&pm);
                pp.setCompositionMode(QPainter::CompositionMode_SourceIn);
                pp.fillRect(pm.rect(), groupCol);
                pp.end();
            }
            painter->setOpacity(isDisabled ? 0.25 : 1.0);
            painter->drawPixmap(x, iy, pm);
            painter->setOpacity(1.0);
        }
        x += iconSz + m_pad;

        // ── Name: baseline-aligned for pixel-perfect text ──
        const QColor nameColor = isDisabled ? t.textMuted
                               : isSel      ? t.text
                                            : t.text;
        painter->setFont(m_font);
        const QString fullText = index.data().toString();
        // The name segment is the entry's displayName; the model string also
        // carries a " - <size>B" suffix. Take the base straight from the entry
        // when we have it — reconstructing via lastIndexOf(" - ") mis-split any
        // displayName containing " - " (e.g. a zero-size type), throwing off the
        // fuzzy-highlight character offsets which index into displayName.
        QString namePart;
        if (entry) {
            namePart = entry->displayName;
        } else {
            const int dashIdx = fullText.lastIndexOf(QStringLiteral(" - "));
            namePart = dashIdx > 0 ? fullText.left(dashIdx) : fullText;
        }
        const int nameW = nameEnd - x;

        // Fuzzy highlight
        QVector<bool> hlFlags;
        if (m_matchPositions && row >= 0 && row < m_matchPositions->size()
            && !(*m_matchPositions)[row].isEmpty()) {
            const int nameLen = namePart.size();
            hlFlags.resize(nameLen, false);
            for (int p : (*m_matchPositions)[row])
                if (p < nameLen) hlFlags[p] = true;
        }

        if (hlFlags.isEmpty()) {
            // Single drawText at baseline (no QRect VCenter — avoids half-pixel jitter)
            painter->setPen(nameColor);
            const QString elided = m_fm.elidedText(namePart, Qt::ElideRight, nameW);
            painter->drawText(x, baseline, elided);
        } else {
            int xp = x;
            for (int i = 0; i < namePart.size(); ) {
                const bool hl = i < hlFlags.size() && hlFlags[i];
                const int s = i;
                while (i < namePart.size() && (i < hlFlags.size() && hlFlags[i]) == hl) i++;
                const QString seg = namePart.mid(s, i - s);
                const int segW = m_fm.horizontalAdvance(seg);
                if (hl) {
                    painter->fillRect(xp, y + 1, segW, h - 2, t.selection);
                    painter->setPen(t.text);
                } else {
                    painter->setPen(nameColor);
                }
                painter->drawText(xp, baseline, seg);
                xp += segW;
            }
        }

        // ── Composite keyword suffix (dim, same baseline) ──
        if (entry && entry->entryKind == TypeEntry::Composite) {
            const int nameTextW = m_fm.horizontalAdvance(namePart);
            const int kwX = x + qMin(nameTextW + 5, nameW);
            const QString kw = entry->classKeyword.isEmpty()
                ? QStringLiteral("struct") : entry->classKeyword;
            const int kwW = m_sfm.horizontalAdvance(kw);
            if (kwX + kwW < nameEnd) {
                painter->setFont(m_smallFont);
                painter->setPen(t.textFaint);
                const int sbl = y + (h + m_sfm.ascent() - m_sfm.descent()) / 2;
                painter->drawText(kwX, sbl, kw);
            }
        }

        // ── Size bar + size text ──
        if (entry) {
            // Size text first (right-most) — measure to position bar correctly
            painter->setFont(m_smallFont);
            const bool isDyn = (entry->sizeBytes == 0 && !isSection);
            const QString szText = entry->sizeBytes > 0
                ? QStringLiteral("%1B").arg(entry->sizeBytes)
                : (isDyn ? QStringLiteral("dyn") : QString());
            const int szTextW = szText.isEmpty() ? 0 : m_sfm.horizontalAdvance(szText);
            const int szTextX = r.right() - m_gap - szTextW;  // right-aligned in right pad

            // Size bar: positioned to the left of size text
            const int barX = szTextX - m_gap - m_barW;
            const int barH = qMax(4, m_fm.height() / 3);
            const int barY = y + (h - barH) / 2;

            painter->fillRect(barX, barY, m_barW, barH, t.surface);
            if (entry->sizeBytes > 0) {
                // 64-bit math: a huge composite sizeBytes * m_barW can overflow int.
                const int fillW = qBound<int>(1,
                    int(qint64(entry->sizeBytes) * m_barW / m_maxSz), m_barW);
                QColor fc = groupCol;
                fc.setAlpha(isSel ? 200 : 140);
                painter->fillRect(barX, barY, fillW, barH, fc);
            } else if (isDyn) {
                const int stripe = qMax(2, m_fm.height() / 6);
                for (int dx = 0; dx < m_barW; dx += stripe * 2)
                    painter->fillRect(barX + dx, barY, stripe, barH, t.border);
            }

            // Draw size text
            if (!szText.isEmpty()) {
                const int sbl = y + (h + m_sfm.ascent() - m_sfm.descent()) / 2;
                painter->setPen(isDyn ? t.textFaint : t.textDim);
                painter->drawText(szTextX, sbl, szText);
            }
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& /*option*/,
                   const QModelIndex& index) const override {
        // Section headers get extra height: room for the separator rule above
        // plus breathing space, so groups read as distinct bands.
        if (m_filtered) {
            int row = index.row();
            if (row >= 0 && row < m_filtered->size()
                && (*m_filtered)[row].entryKind == TypeEntry::Section)
                return QSize(m_cachedSizeHint.width(),
                             m_fm.height() + (m_compact ? 7 : 11));
        }
        return m_cachedSizeHint;
    }

    void updateCachedSizeHint() {
        m_cachedSizeHint = QSize(200, m_fm.height() + (m_compact ? 3 : 6));
    }

    bool helpEvent(QHelpEvent* event, QAbstractItemView* view,
                   const QStyleOptionViewItem& option,
                   const QModelIndex& index) override {
        if (event->type() == QEvent::ToolTip && m_filtered) {
            int row = index.row();
            if (row >= 0 && row < m_filtered->size()) {
                const auto& e = (*m_filtered)[row];
                if (e.entryKind == TypeEntry::Composite && !e.fieldSummary.isEmpty()) {
                    // Themed RcxTooltip — matches the rest of the app's
                    // visual language (rounded body, arrow callout, same
                    // mono font as the chooser, theme palette colors).
                    // Default QToolTip used system fonts/colors and looked
                    // out of place against the chooser's dark chrome.
                    QString title = QStringLiteral("%1 (0x%2 bytes, %3 fields)")
                        .arg(e.displayName,
                             QString::number(e.sizeBytes, 16).toUpper())
                        .arg(e.fieldCount);
                    QString body = e.fieldSummary.join(QChar('\n'));
                    if (e.fieldCount > e.fieldSummary.size())
                        body += QStringLiteral("\n...");
                    auto* tip = sharedRcxTooltip();
                    const auto& t = ThemeManager::instance().current();
                    tip->setTheme(t.backgroundAlt, t.border,
                                  t.text, t.text, t.border);
                    tip->populate(title, body, m_font);
                    // Anchor at the cursor position. The earlier fixed-X
                    // variant was for the editor's row-anchored callouts
                    // (which always want the same X relative to the row);
                    // here the tooltip is mouse-driven, so it should
                    // follow the cursor exactly.
                    tip->showAt(event->globalPos());
                    return true;
                }
            }
            sharedRcxTooltip()->hide();
            return true;
        }
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }

private:
    TypeSelectorPopup* m_popup = nullptr;
    QFont m_font;
    QFont m_smallFont;
    QFontMetrics m_fm{QFont()};
    QFontMetrics m_sfm{QFont()};
    QSize m_cachedSizeHint{200, 20};
    bool m_isLoading = false;
    bool m_compact = false;
    const QVector<TypeEntry>* m_filtered = nullptr;
    const QVector<QVector<int>>* m_matchPositions = nullptr;
    const TypeEntry* m_currentEntry = nullptr;
    bool m_hasCurrent = false;
};

// ── TypeSelectorPopup ──

TypeSelectorPopup::TypeSelectorPopup(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_DeleteOnClose, false);

    const auto& theme = ThemeManager::instance().current();
    QPalette pal;
    pal.setColor(QPalette::Window,          theme.backgroundAlt);
    pal.setColor(QPalette::WindowText,      theme.text);
    pal.setColor(QPalette::Base,            theme.background);
    pal.setColor(QPalette::AlternateBase,   theme.surface);
    pal.setColor(QPalette::Text,            theme.text);
    pal.setColor(QPalette::Button,          theme.button);
    pal.setColor(QPalette::ButtonText,      theme.text);
    pal.setColor(QPalette::Highlight,       theme.hover);
    pal.setColor(QPalette::HighlightedText, theme.text);
    setPalette(pal);
    setAutoFillBackground(true);

    setFrameShape(QFrame::NoFrame);
    setLineWidth(0);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 5, 6, 5);
    layout->setSpacing(3);

    // ── Top: Filter + close ──
    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);

        m_filterEdit = new QLineEdit;
        m_filterEdit->setPlaceholderText(
            QStringLiteral("Filter types..  (try int*, Ball[10])"));
        m_filterEdit->setClearButtonEnabled(true);
        m_filterEdit->setPalette(pal);
        m_filterEdit->setStyleSheet(QStringLiteral(
            "QLineEdit { border: 1px solid %1; padding: 2px 4px; border-radius: 0px; }")
            .arg(theme.border.name()));
        m_filterEdit->setAccessibleName(QStringLiteral("Filter types"));
        m_filterEdit->installEventFilter(this);
        connect(m_filterEdit, &QLineEdit::textChanged,
                this, &TypeSelectorPopup::applyFilter);
        row->addWidget(m_filterEdit);

        m_escLabel = new QToolButton;
        m_escLabel->setText(QStringLiteral("\u2715"));
        m_escLabel->setAutoRaise(true);
        m_escLabel->setCursor(Qt::PointingHandCursor);
        m_escLabel->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: none; padding: 2px 4px; }"
            "QToolButton:hover { color: %2; }")
            .arg(theme.textDim.name(), theme.indHoverSpan.name()));
        connect(m_escLabel, &QToolButton::clicked, this, [this]() { hide(); });
        row->addWidget(m_escLabel);

        layout->addLayout(row);
    }

    // ── Kind-group chips (Hex, Int, Float, Ptr) ──
    {
        m_chipRow = new QWidget;
        auto* chipLayout = new QHBoxLayout(m_chipRow);
        chipLayout->setContentsMargins(0, 0, 0, 0);
        chipLayout->setSpacing(2);

        // Consolidated filter chips: Hex, Int (signed+unsigned+bool), Float, Ptr
        // Vec/Str/Ctr/Common always visible — too niche to need a toggle
        static const char* groups[] = {"Hex","Int","Float","Ptr"};
        static const char* labels[] = {"Hex","Int","Float","Ptr"};
        auto refilter = [this]() { applyFilter(m_filterEdit->text()); };
        for (int i = 0; i < 4; i++) {
            auto* btn = new CategoryChip(QString::fromLatin1(labels[i]));
            btn->setChecked(true);
            btn->setGroupColor(kindGroupColor(QString::fromLatin1(groups[i])));
            chipLayout->addWidget(btn);
            m_groupChips[QString::fromLatin1(groups[i])] = btn;
            connect(btn, &QToolButton::toggled, this, refilter);
        }
        chipLayout->addStretch();

        // all / none quick toggles
        auto makeQuick = [&](const QString& label) {
            auto* btn = new QToolButton;
            btn->setText(label);
            btn->setAutoRaise(true);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(QStringLiteral(
                "QToolButton { color: %1; border: 1px solid %2; padding: 1px 5px; }"
                "QToolButton:hover { background: %3; color: %4; }")
                .arg(theme.textMuted.name(), theme.border.name(),
                     theme.hover.name(), theme.text.name()));
            return btn;
        };
        auto* allBtn = makeQuick(QStringLiteral("all"));
        auto* noneBtn = makeQuick(QStringLiteral("none"));
        connect(allBtn, &QToolButton::clicked, this, [this, refilter]() {
            // Block per-chip toggled() during the bulk change so we refilter
            // ONCE at the end instead of N times (one per chip).
            for (auto* c : m_groupChips) { QSignalBlocker b(c); c->setChecked(true); }
            refilter();
        });
        connect(noneBtn, &QToolButton::clicked, this, [this, refilter]() {
            // Keep exactly one group on, deterministically (Hex) — the prior
            // code kept "the first" in QHash iteration order, so which group
            // survived was effectively random across runs/builds.
            for (auto it = m_groupChips.begin(); it != m_groupChips.end(); ++it) {
                QSignalBlocker b(it.value());
                it.value()->setChecked(it.key() == QStringLiteral("Hex"));
            }
            refilter();
        });
        chipLayout->addWidget(allBtn);
        chipLayout->addWidget(noneBtn);

        m_statusLabel = new QLabel;
        m_statusLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; padding: 1px 4px; }").arg(theme.textDim.name()));
        chipLayout->addWidget(m_statusLabel);

        layout->addWidget(m_chipRow);
    }

//TODO-DELETE(sort toolbar block (m_sortRow / m_sortBtns / density + detail buttons))     // ── Sort toolbar ──
//    {
//        m_sortRow = new QWidget;
//        m_sortRow->setFixedHeight(22);
//        auto* slay = new QHBoxLayout(m_sortRow);
//        slay->setContentsMargins(0, 0, 0, 0);
//        slay->setSpacing(0);
//
//        struct SortDef { const char* label; SortMode mode; };
//        static const SortDef defs[] = {
//            {"group", SortGroup}, {"name", SortName},
//            {"size", SortSize}
//        };
//        for (const auto& d : defs) {
//            auto* btn = new QToolButton;
//            btn->setText(QString::fromLatin1(d.label));
//            btn->setCheckable(true);
//            btn->setCursor(Qt::PointingHandCursor);
//            btn->setFixedHeight(22);
//            btn->setStyleSheet(QStringLiteral(
//                "QToolButton { color: %1; background: transparent; border: none;"
//                " border-right: 1px solid %2; padding: 0 8px; }"
//                "QToolButton:checked { color: %3; background: %4; }"
//                "QToolButton:hover:!checked { color: %5; background: %6; }")
//                .arg(theme.textMuted.name(), theme.border.name(),
//                     theme.syntaxKeyword.name(), theme.selected.name(),
//                     theme.text.name(), theme.hover.name()));
//            if (d.mode == SortGroup) btn->setChecked(true);
//            SortMode sm = d.mode;
//            connect(btn, &QToolButton::clicked, this, [this, btn, sm]() {
//                if (m_sortMode == sm) {
//                    m_sortDir *= -1;
//                } else {
//                    m_sortMode = sm;
//                    m_sortDir = 1;
//                }
//                // Update button labels
//                for (auto* b : m_sortBtns) b->setChecked(false);
//                btn->setChecked(true);
//                // Show direction arrow on active sort
//                static const char* labels[] = {"group", "name", "size"};
//                for (int i = 0; i < m_sortBtns.size(); i++) {
//                    if (m_sortBtns[i] == btn)
//                        m_sortBtns[i]->setText(
//                            QString::fromLatin1(labels[i])
//                            + (m_sortDir == 1 ? QStringLiteral(" \u2191")
//                                              : QStringLiteral(" \u2193")));
//                    else
//                        m_sortBtns[i]->setText(QString::fromLatin1(labels[i]));
//                }
//                applyFilter(m_filterEdit->text());
//            });
//            slay->addWidget(btn);
//            m_sortBtns.append(btn);
//        }
//        slay->addStretch();
//
//        // Density toggle: normal / compact
//        QString vbtnStyle = QStringLiteral(
//            "QToolButton { color: %1; border: none; border-left: 1px solid %2;"
//            " padding: 2px; width: 22px; height: 22px; }"
//            "QToolButton:checked { color: %3; }"
//            "QToolButton:hover { color: %4; background: %5; }")
//            .arg(theme.textMuted.name(), theme.border.name(),
//                 theme.syntaxKeyword.name(), theme.text.name(), theme.hover.name());
//        auto* normBtn = new QToolButton;
//        normBtn->setCheckable(true);
//        normBtn->setChecked(true);
//        normBtn->setText(QStringLiteral("\u2630")); // \u2630 (looser = normal)
//        normBtn->setToolTip(QStringLiteral("Normal density"));
//        normBtn->setStyleSheet(vbtnStyle);
//        auto* compBtn = new QToolButton;
//        compBtn->setCheckable(true);
//        compBtn->setText(QStringLiteral("\u2261")); // \u2261 (tighter = compact)
//        compBtn->setToolTip(QStringLiteral("Compact density"));
//        compBtn->setStyleSheet(vbtnStyle);
//        slay->addWidget(normBtn);
//        slay->addWidget(compBtn);
//        connect(normBtn, &QToolButton::clicked, this, [this, normBtn, compBtn]() {
//            m_compact = false;
//            normBtn->setChecked(true); compBtn->setChecked(false);
//            auto* d = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
//            if (d) { d->setCompact(false); }
//            m_listView->doItemsLayout();
//        });
//        connect(compBtn, &QToolButton::clicked, this, [this, normBtn, compBtn]() {
//            m_compact = true;
//            compBtn->setChecked(true); normBtn->setChecked(false);
//            auto* d = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
//            if (d) { d->setCompact(true); }
//            m_listView->doItemsLayout();
//        });
//
//        // Detail pane toggle
//        m_detailBtn = new QToolButton;
//        m_detailBtn->setCheckable(true);
//        m_detailBtn->setChecked(false);
//        m_detailBtn->setText(QStringLiteral("\u25E8")); // ◨
//        m_detailBtn->setToolTip(QStringLiteral("Toggle detail pane"));
//        m_detailBtn->setStyleSheet(vbtnStyle);
//        slay->addWidget(m_detailBtn);
//        connect(m_detailBtn, &QToolButton::clicked, this, [this]() {
//            m_showDetail = m_detailBtn->isChecked();
//            if (m_detailPane) m_detailPane->setVisible(m_showDetail);
//            if (m_showDetail) updateDetailPane();
//        });
//
//        layout->addWidget(m_sortRow);
//    }

    // ── List view + detail pane (horizontal split) ──
    {
        auto* bodyRow = new QHBoxLayout;
        bodyRow->setContentsMargins(0, 0, 0, 0);
        bodyRow->setSpacing(0);

        m_model = new QStringListModel(this);
        m_listView = new QListView;
        m_listView->setModel(m_model);
        m_listView->setPalette(pal);
        m_listView->setFrameShape(QFrame::NoFrame);
        m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_listView->setMouseTracking(true);
        m_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_listView->viewport()->setAttribute(Qt::WA_Hover, true);
        m_listView->setAccessibleName(QStringLiteral("Type list"));
        m_listView->setUniformItemSizes(false);
        m_listView->setLayoutMode(QListView::Batched);
        m_listView->setBatchSize(50);
        m_listView->installEventFilter(this);

        auto* delegate = new TypeSelectorDelegate(this, m_listView);
        m_listView->setItemDelegate(delegate);

        bodyRow->addWidget(m_listView, 1);

        // Detail pane (hidden by default)
        m_detailPane = new QWidget;
        m_detailPane->setFixedWidth(220);
        m_detailPane->setAutoFillBackground(true);
        {
            QPalette dp = pal;
            dp.setColor(QPalette::Window, theme.background);
            m_detailPane->setPalette(dp);
        }
        auto* dpLayout = new QVBoxLayout(m_detailPane);
        dpLayout->setContentsMargins(0, 0, 0, 0);
        dpLayout->setSpacing(0);

        auto* dpScroll = new QScrollArea;
        dpScroll->setWidgetResizable(true);
        dpScroll->setFrameShape(QFrame::NoFrame);
        dpScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_detailContent = new QLabel;
        m_detailContent->setTextFormat(Qt::RichText);
        m_detailContent->setWordWrap(true);
        m_detailContent->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        m_detailContent->setContentsMargins(0, 0, 0, 4);
        m_detailContent->setOpenExternalLinks(false);
        connect(m_detailContent, &QLabel::linkActivated,
                this, [this](const QString& link) {
            if (link == QStringLiteral("action:select")) {
                setModifier(0);          // plain
                acceptCurrent();
            } else if (link == QStringLiteral("action:ptr")) {
                setModifier(1);          // pointer
                acceptCurrent();
            } else if (link == QStringLiteral("action:arr")) {
                setModifier(3, 1);       // array, count 1
                acceptCurrent();
            }
        });
        dpScroll->setWidget(m_detailContent);
        dpLayout->addWidget(dpScroll, 1);

        m_detailPane->setVisible(false);
        bodyRow->addWidget(m_detailPane);

        layout->addLayout(bodyRow, 1);

        connect(m_listView, &QListView::doubleClicked,
                this, [this](const QModelIndex& index) {
            acceptIndex(index.row());
        });
        // A single click on a collapsible section header expands/collapses it
        // (everywhere else a single click just selects; double-click accepts).
        connect(m_listView, &QListView::clicked,
                this, [this](const QModelIndex& index) {
            int r = index.row();
            if (r >= 0 && r < m_filteredTypes.size()) {
                const auto& e = m_filteredTypes[r];
                if (e.entryKind == TypeEntry::Section && e.sectionCollapsible)
                    acceptIndex(r);
            }
        });
        connect(m_listView->selectionModel(), &QItemSelectionModel::currentChanged,
                this, [this]() { updateModifierPreview(); updateDetailPane(); });
    }

    // ── Modifier row: "Apply as: [ Value | Pointer | Array ]" on its OWN
    // full-width line. Previously this shared one row with the preview banner
    // and the New/OK buttons, which crushed the segments below their text
    // width on a narrow popup so they rendered as empty boxes. A dedicated row
    // gives the segments their natural size. Hidden in simple mode
    // (updateModeChrome); the C4 text path (typing "int*") still applies
    // modifiers there. ──
    {
        m_modRow = new QWidget;
        auto* modLayout = new QHBoxLayout(m_modRow);
        modLayout->setContentsMargins(0, 0, 0, 0);
        modLayout->setSpacing(4);

        m_modLabel = new QLabel(QStringLiteral("Apply as:"));
        m_modLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; }")
            .arg(theme.textDim.name()));
        modLayout->addWidget(m_modLabel);

        m_modGroup = new QButtonGroup(this);
        m_modGroup->setExclusive(true);  // exactly one segment selected

        // The three segments live in their OWN Fixed-size container widget.
        // A bare sub-layout still let the joined group stretch to fill the row
        // (eliding "Pointer" → "Po…er" once ×2 appeared); a Fixed-policy
        // QWidget sizes exactly to its contents and can never stretch, so the
        // segments always show full text regardless of how much row is free.
        auto* segBox = new QWidget;
        auto* segWrap = new QHBoxLayout(segBox);
        segWrap->setContentsMargins(0, 0, 0, 0);
        segWrap->setSpacing(0);  // joined; border-right divides segments
        segBox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        auto makeSeg = [&](const QString& label, int id) -> QToolButton* {
            auto* btn = new QToolButton;
            btn->setText(label);
            btn->setCheckable(true);
            btn->setCursor(Qt::PointingHandCursor);
            m_modGroup->addButton(btn, id);
            segWrap->addWidget(btn);
            return btn;
        };
        m_segValue   = makeSeg(QStringLiteral("Value"),   0);
        m_segPointer = makeSeg(QStringLiteral("Pointer"), 1);
        m_segArray   = makeSeg(QStringLiteral("Array"),   2);
        m_segValue->setChecked(true);
        modLayout->addWidget(segBox);

        // ×2 depth toggle — only meaningful (and shown) when Pointer active.
        m_ptrDouble = new QToolButton;
        m_ptrDouble->setText(QStringLiteral("×2"));
        m_ptrDouble->setCheckable(true);
        m_ptrDouble->setCursor(Qt::PointingHandCursor);
        m_ptrDouble->setToolTip(QStringLiteral("Pointer-to-pointer (**)"));
        m_ptrDouble->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_ptrDouble->hide();
        modLayout->addWidget(m_ptrDouble);

        // Square, joined segmented styling (matches the sort toolbar).
        const QString segStyle = QStringLiteral(
            "QToolButton { color: %1; background: %2; border: 1px solid %3;"
            "  border-radius: 0px; padding: 2px 12px; }"
            "QToolButton:checked { color: %4; background: %5; border-color: %5; }"
            "QToolButton:hover:!checked { background: %6; }")
            .arg(theme.textDim.name(), theme.background.name(), theme.border.name(),
                 theme.text.name(), theme.selected.name(), theme.hover.name());
        m_segValue->setStyleSheet(segStyle);
        m_segPointer->setStyleSheet(segStyle);
        m_segArray->setStyleSheet(segStyle);
        m_ptrDouble->setStyleSheet(segStyle);

        m_arrayCountEdit = new QLineEdit;
        m_arrayCountEdit->setPlaceholderText(QStringLiteral("n"));
        m_arrayCountEdit->setValidator(new QIntValidator(1, 99999, m_arrayCountEdit));
        m_arrayCountEdit->setFixedWidth(50);
        m_arrayCountEdit->setPalette(pal);
        m_arrayCountEdit->hide();
        modLayout->addWidget(m_arrayCountEdit);

        // Stretch pins the controls left at natural size — they never expand
        // to fill, and never shrink below their text.
        modLayout->addStretch();

        connect(m_modGroup, &QButtonGroup::idClicked, this, [this](int) {
            syncModifierExtras(/*focusCount=*/true);
            updateModifierPreview();
        });
        connect(m_ptrDouble, &QToolButton::toggled,
                this, [this]() { updateModifierPreview(); });
        connect(m_arrayCountEdit, &QLineEdit::textChanged,
                this, [this]() { updateModifierPreview(); });

        layout->addWidget(m_modRow);
    }

    // ── Action row: live preview banner (left) + "＋ New" / "OK" (right) ──
    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);

        m_titleLabel = new QLabel;
        m_titleLabel->setPalette(pal);
        m_titleLabel->setAlignment(Qt::AlignVCenter);
        m_titleLabel->setTextFormat(Qt::RichText);
        m_titleLabel->setContentsMargins(0, 0, 0, 0);
        row->addWidget(m_titleLabel);

        row->addStretch();

        // Outline-only square buttons via the shared DialogButton (self-themes
        // on themeChanged) — matches the project's validated dialog button
        // language and removes the per-button stylesheet drift.
        m_createBtn = new DialogButton(QStringLiteral("＋ New"),
                                       DialogButton::Secondary);
        m_createBtn->setCursor(Qt::PointingHandCursor);
        m_createBtn->setAccessibleName(QStringLiteral("Create new type"));
        connect(m_createBtn, &QPushButton::clicked, this, [this]() {
            int modId = currentModId();
            int arrCount = 0;
            if (modId == 3 && m_arrayCountEdit) {
                arrCount = m_arrayCountEdit->text().trimmed().toInt();
                if (arrCount < 1) arrCount = 1;  // array toggle on → never 0
            }
            m_accepted = true;
            hide();
            // Corner "+ New" button: auto-named struct (no typed name/keyword).
            emit createNewTypeRequested(modId, arrCount, QString(), QString());
        });
        row->addWidget(m_createBtn);

        m_saveBtn = new DialogButton(QStringLiteral("OK"), DialogButton::Primary);
        m_saveBtn->setCursor(Qt::PointingHandCursor);
        m_saveBtn->setAccessibleName(QStringLiteral("OK"));
        connect(m_saveBtn, &QPushButton::clicked, this, [this]() {
            acceptCurrent();
        });
        row->addWidget(m_saveBtn);

        layout->addLayout(row);
    }

    // (Footer crumb removed — the action row above the OK button already
    // shows "<type> → <size>" right where the user is about to click,
    // so the dim "<type> · <size> · <group>" line duplicated the same
    // info one band lower without adding anything. Less visual noise,
    // same actionable information.)
//TODO-DELETE(m_footerLabel = nullptr)     m_footerLabel = nullptr;

    // Apply the initial chrome gating for the default (simple/Common) mode.
    updateModeChrome();
}

// Static once-per-process primer: absorbs the ~300ms DLL/style/font init
// cost that Qt charges on the first-ever popup show.
static bool s_primerDone = false;

static void runPrimerOnce() {
    if (s_primerDone) return;
    s_primerDone = true;

    auto* primer = new QFrame(nullptr, Qt::Popup | Qt::FramelessWindowHint);
    primer->resize(300, 400);
    auto* lay = new QVBoxLayout(primer);
    lay->addWidget(new QLabel(QStringLiteral("x")));
    lay->addWidget(new QLineEdit);
    auto* model = new QStringListModel(primer);
    QStringList items; for (int i = 0; i < 10; i++) items << QStringLiteral("x");
    model->setStringList(items);
    auto* lv = new QListView;
    lv->setModel(model);
    lay->addWidget(lv);
    primer->show();
    QApplication::processEvents();
    primer->hide();
    QApplication::processEvents();
    delete primer;
}

void TypeSelectorPopup::preload() {
    PROFILE_SCOPE("TypeSelectorPopup::preload");
    runPrimerOnce();
}

void TypeSelectorPopup::warmUp() {
    // Phase 1: one-time per-process primer (absorbs ~300ms DLL init)
    runPrimerOnce();

    // Phase 2: show/hide ourselves to pre-create native window handle
    TypeEntry dummy;
    dummy.entryKind = TypeEntry::Primitive;
    dummy.primitiveKind = NodeKind::Hex8;
    dummy.displayName = QStringLiteral("warmup");
    setTypes({dummy});
    popup(QPoint(-9999, -9999));
    hide();
    QApplication::processEvents();
}

void TypeSelectorPopup::placeOnScreen(const QPoint& globalPos, int w, int h) {
    QScreen* screen = QApplication::screenAt(globalPos);
    if (!screen) screen = QApplication::primaryScreen();
    QPoint origin = globalPos;
    if (screen) {
        const QRect avail = screen->availableGeometry();
        // Cap to the work area but never below a usable minimum — the old code
        // computed height as `avail.bottom() - globalPos.y()`, which goes to
        // zero or negative when opened near the bottom/right edge, leaving a
        // degenerate 1px window at the wrong spot.
        w = qBound(120, w, avail.width());
        h = qBound(120, h, avail.height());
        // Overflow → shift the origin back onto the screen (flip up/left)
        // instead of shrinking the popup.
        if (origin.x() + w > avail.right())  origin.setX(avail.right()  - w + 1);
        if (origin.y() + h > avail.bottom()) origin.setY(avail.bottom() - h + 1);
        origin.setX(qMax(avail.left(), origin.x()));
        origin.setY(qMax(avail.top(),  origin.y()));
    }
    setFixedSize(w, h);
    move(origin);
    show();
    raise();
    activateWindow();
    m_filterEdit->setFocus();
}

void TypeSelectorPopup::popupLoading(const QPoint& globalPos) {
    m_loading = true;
    m_accepted = false;
    auto* delegate = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
    if (delegate) delegate->setLoading(true);

    // Clear stale selection from previous use
    m_listView->selectionModel()->clearSelection();
    m_listView->selectionModel()->clearCurrentIndex();

    // Fill model with dummy rows for skeleton bars
    QStringList dummy;
    for (int i = 0; i < 12; i++) dummy << QString();
    m_model->setStringList(dummy);

    // Reset UI to empty state
    m_titleLabel->clear();
    if (m_statusLabel) m_statusLabel->clear();

    // Default popup size — scales with the current font so high zoom
    // produces a proportionally wider popup. 540 px = 54 chars of 'M' at
    // the default 10 pt JetBrains Mono, so the constant `* 54` keeps the
    // baseline identical at zoom 0 and grows it linearly when the user
    // Ctrl+scrolls.
    QFontMetrics fm(m_font);
    const int charW = qMax(6, fm.horizontalAdvance(QStringLiteral("M")));
    int rowH = fm.height() + 6;
    int popupW = charW * 54;
    int popupH = qMax(400, rowH * 14 + rowH * 2 + 20);

    placeOnScreen(globalPos, popupW, popupH);
}

void TypeSelectorPopup::setFont(const QFont& font) {
    m_font = font;

    {
        // C5 — the live "type+modifier → size" preview is the centerpiece:
        // larger + bold so the user always sees what they'll get.
        QFont tf = font;
        tf.setPointSize(qMax(8, resolvedPointSize(font) + 1));
        tf.setBold(true);
        m_titleLabel->setFont(tf);
    }
    m_escLabel->setFont(font);
    m_filterEdit->setFont(font);
    m_listView->setFont(font);

    QFont smallFont = font;
    smallFont.setPointSize(qMax(7, resolvedPointSize(font) - 1));
    m_segValue->setFont(smallFont);
    m_segPointer->setFont(smallFont);
    m_segArray->setFont(smallFont);
    m_ptrDouble->setFont(smallFont);
    if (m_modLabel) m_modLabel->setFont(smallFont);
    m_arrayCountEdit->setFont(smallFont);

    // Pin each segment to exactly its text width + padding (the stylesheet's
    // 12px horizontal padding + 1px borders ≈ 28px of chrome). A styled
    // QToolButton otherwise reports a fat, roughly-equal minimum width, so the
    // joined group filled the row and middle-elided "Pointer" → "Po…er" once
    // the ×2 toggle appeared. A hard per-text width can't stretch or clip.
    {
        const QFontMetrics segFm(smallFont);
        auto fitSeg = [&](QToolButton* b) {
            if (b) b->setFixedWidth(segFm.horizontalAdvance(b->text()) + 30);
        };
        fitSeg(m_segValue);
        fitSeg(m_segPointer);
        fitSeg(m_segArray);
        fitSeg(m_ptrDouble);
    }

    m_createBtn->setFont(smallFont);
    m_saveBtn->setFont(smallFont);

    QFont chipFont = font;
    chipFont.setPointSize(qMax(7, (int)(resolvedPointSize(font) * 0.75)));
    for (auto* c : m_groupChips) c->setFont(chipFont);
    if (m_statusLabel) m_statusLabel->setFont(chipFont);
    if (m_footerLabel) m_footerLabel->setFont(chipFont);
    if (m_detailContent) m_detailContent->setFont(smallFont);
    for (auto* btn : m_sortBtns) btn->setFont(chipFont);

    auto* delegate = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
    if (delegate)
        delegate->setFont(font);
}

void TypeSelectorPopup::applyTheme(const Theme& theme) {
    QPalette pal;
    pal.setColor(QPalette::Window,          theme.backgroundAlt);
    pal.setColor(QPalette::WindowText,      theme.text);
    pal.setColor(QPalette::Base,            theme.background);
    pal.setColor(QPalette::AlternateBase,   theme.surface);
    pal.setColor(QPalette::Text,            theme.text);
    pal.setColor(QPalette::Button,          theme.button);
    pal.setColor(QPalette::ButtonText,      theme.text);
    pal.setColor(QPalette::Highlight,       theme.hover);
    pal.setColor(QPalette::HighlightedText, theme.text);
    pal.setColor(QPalette::Dark,            theme.border);  // 1px frame border
    setPalette(pal);

    m_titleLabel->setPalette(pal);
    m_filterEdit->setPalette(pal);
    m_listView->setPalette(pal);
    m_listView->viewport()->setPalette(pal);
    m_arrayCountEdit->setPalette(pal);

    // Esc button (snapped to corner)
    m_escLabel->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; border: none; padding: 2px 4px; }"
        "QToolButton:hover { color: %2; }")
        .arg(theme.textDim.name(), theme.indHoverSpan.name()));

    // Create / OK buttons are DialogButtons — they self-theme on
    // themeChanged, so no per-button stylesheet here (removes the old
    // square-vs-rounded constructor/applyTheme drift).

    // Filter (no focus accent)
    m_filterEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { border: 1px solid %1; padding: 2px 4px; border-radius: 0px; }")
        .arg(theme.border.name()));

    // Segmented modifier control (square, joined)
    QString segStyle = QStringLiteral(
        "QToolButton { color: %1; background: %2; border: 1px solid %3;"
        "  border-radius: 0px; padding: 2px 10px; }"
        "QToolButton:checked { color: %4; background: %5; border-color: %5; }"
        "QToolButton:hover:!checked { background: %6; }")
        .arg(theme.textDim.name(), theme.background.name(), theme.border.name(),
             theme.text.name(), theme.selected.name(), theme.hover.name());
    m_segValue->setStyleSheet(segStyle);
    m_segPointer->setStyleSheet(segStyle);
    m_segArray->setStyleSheet(segStyle);
    m_ptrDouble->setStyleSheet(segStyle);
    if (m_modLabel)
        m_modLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; }")
            .arg(theme.textDim.name()));

    // Kind-group chips — custom painted, theme read at paint time, no stylesheet needed
    for (auto* c : m_groupChips) c->update();

    // Status label
    if (m_statusLabel) {
        m_statusLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; padding: 1px 2px; }").arg(theme.textDim.name()));
    }

    // Sort toolbar buttons
    for (auto* btn : m_sortBtns) {
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; background: transparent; border: none;"
            " border-right: 1px solid %2; padding: 0 8px; }"
            "QToolButton:checked { color: %3; background: %4; }"
            "QToolButton:hover:!checked { color: %5; background: %6; }")
            .arg(theme.textMuted.name(), theme.border.name(),
                 theme.syntaxKeyword.name(), theme.selected.name(),
                 theme.text.name(), theme.hover.name()));
    }

    // Footer crumb
    if (m_footerLabel) {
        m_footerLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; padding: 0 3px; border-top: 1px solid %2; }")
            .arg(theme.textFaint.name(), theme.border.name()));
    }

    // Detail pane
    if (m_detailPane) {
        QPalette dp;
        dp.setColor(QPalette::Window, theme.background);
        m_detailPane->setPalette(dp);
    }

}

//TODO-DELETE(TypeSelectorPopup::setTitle) void TypeSelectorPopup::setTitle(const QString& /*title*/) {
//    // Title is now dynamic — set by updateModifierPreview()
//}

void TypeSelectorPopup::setMode(TypePopupMode mode) {
    m_mode = mode;
    // Option (a): the modifier row is always available when the mode allows it
    // (no longer gated on the simple/full split). updateModeChrome() owns the
    // same rule.
    bool showMods = (mode == TypePopupMode::FieldType
                     || mode == TypePopupMode::ArrayElement);
    m_modRow->setVisible(showMods);
    // Reset to the default "Value" segment (plain).
    if (m_segValue) { QSignalBlocker b(m_modGroup); m_segValue->setChecked(true); }
    if (m_ptrDouble) { m_ptrDouble->setChecked(false); m_ptrDouble->hide(); }
    m_arrayCountEdit->clear();
    m_arrayCountEdit->hide();
}

void TypeSelectorPopup::setCurrentNodeSize(int bytes) {
    m_currentNodeSize = bytes;
}

void TypeSelectorPopup::setPointerSize(int bytes) {
    m_pointerSize = bytes;
}

void TypeSelectorPopup::setModifier(int modId, int arrayCount) {
    if (!m_modGroup) return;
    QSignalBlocker bg(m_modGroup);
    if (modId == 1 || modId == 2) {
        m_segPointer->setChecked(true);
        QSignalBlocker bd(m_ptrDouble);
        m_ptrDouble->setChecked(modId == 2);
    } else if (modId == 3) {
        m_segArray->setChecked(true);
        // Clamp to >=1: arrayCount 0/negative would defeat the QIntValidator
        // and later be dropped as "not an array", silently committing a plain
        // type while the segment reads as Array.
        m_arrayCountEdit->setText(QString::number(qMax(1, arrayCount)));
    } else {
        m_segValue->setChecked(true);
    }
    syncModifierExtras();
    updateModifierPreview();
}

int TypeSelectorPopup::currentModId() const {
    if (!m_modGroup) return 0;
    const int seg = m_modGroup->checkedId();  // 0=Value 1=Pointer 2=Array
    if (seg == 1) return (m_ptrDouble && m_ptrDouble->isChecked()) ? 2 : 1;
    if (seg == 2) return 3;
    return 0;  // Value / plain
}

void TypeSelectorPopup::syncModifierExtras(bool focusCount) {
    if (!m_modGroup) return;
    const int seg = m_modGroup->checkedId();
    if (m_ptrDouble) m_ptrDouble->setVisible(seg == 1);
    if (m_arrayCountEdit) {
        const bool arr = (seg == 2);
        m_arrayCountEdit->setVisible(arr);
        if (arr) {
            if (m_arrayCountEdit->text().trimmed().isEmpty())
                m_arrayCountEdit->setText(QStringLiteral("1"));
            if (focusCount) {
                m_arrayCountEdit->setFocus();
                m_arrayCountEdit->selectAll();
            }
        }
    }
}

void TypeSelectorPopup::setTypes(const QVector<TypeEntry>& types, const TypeEntry* current) {
    m_loading = false;
    auto* delegate = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
    if (delegate) delegate->setLoading(false);

    m_allTypes = types;
    // Normalize kindGroup ONCE here — setTypes is the single owner of the type
    // list, so the filter/query path never has to mutate m_allTypes as a side
    // effect (it used to auto-assign in 3 duplicate spots inside applyFilter).
    // Empty kindGroup means "auto": primitives map by primitiveKind, everything
    // else is a container ("Ctr").
    for (auto& t : m_allTypes) {
        if (t.entryKind == TypeEntry::Section) continue;
        if (t.kindGroup.isEmpty())
            t.kindGroup = (t.entryKind == TypeEntry::Primitive)
                ? kindGroupFor(t.primitiveKind) : QStringLiteral("Ctr");
    }
    // Cache max display name length for popup width calculation
    m_cachedMaxNameLen = 0;
    for (const auto& t : m_allTypes) {
        if (t.entryKind != TypeEntry::Section)
            m_cachedMaxNameLen = qMax(m_cachedMaxNameLen, (int)t.displayName.size());
    }
    if (current) {
        m_currentEntry = *current;
        m_hasCurrent = true;
    } else {
        m_currentEntry = TypeEntry{};
        m_hasCurrent = false;
    }
    // Pass current entry to delegate for checkmark rendering
    if (delegate)
        delegate->setCurrentEntry(m_hasCurrent ? &m_currentEntry : nullptr, m_hasCurrent);
    // Don't reset modifier buttons here — setMode() already resets to plain,
    // and setModifier() may have preselected a button between setMode/setTypes.

    // Dynamic placeholder with type count
    int typeCount = 0;
    for (const auto& t : m_allTypes)
        if (t.entryKind != TypeEntry::Section) typeCount++;
    QString placeholder;
    switch (m_mode) {
    case TypePopupMode::Root:           placeholder = QStringLiteral("Filter %1 structs..."); break;
    case TypePopupMode::ArrayElement:   placeholder = QStringLiteral("Filter %1 element types..."); break;
    case TypePopupMode::PointerTarget:  placeholder = QStringLiteral("Filter %1 targets..."); break;
    default:                            placeholder = QStringLiteral("Filter %1 types..  (try int*, Ball[10])"); break;
    }
    m_filterEdit->setPlaceholderText(placeholder.arg(typeCount));

    m_filterEdit->clear();
    // Drop any stale selection from a prior open so applyFilter's restore
    // logic doesn't carry it over; the current-type pre-select below sets
    // the real selection for this open.
    m_listView->setCurrentIndex(QModelIndex());
    applyFilter(QString());

    // Pre-select current type in list
    if (m_hasCurrent) {
        for (int i = 0; i < m_filteredTypes.size(); i++) {
            const auto& entry = m_filteredTypes[i];
            if (entry.entryKind == TypeEntry::Section) continue;
            bool match = false;
            if (m_currentEntry.entryKind == TypeEntry::Primitive && entry.entryKind == TypeEntry::Primitive)
                match = (entry.primitiveKind == m_currentEntry.primitiveKind);
            else if (m_currentEntry.entryKind == TypeEntry::Composite && entry.entryKind == TypeEntry::Composite)
                match = (entry.structId == m_currentEntry.structId);
            if (match) {
                m_listView->setCurrentIndex(m_model->index(i));
                break;
            }
        }
    }
}

void TypeSelectorPopup::setShowAllTypesForTest(bool all) {
    m_showAllTypes = all;
    updateModeChrome();
    applyFilter(m_filterEdit ? m_filterEdit->text() : QString());
}

void TypeSelectorPopup::updateModeChrome() {
    // Option (a) — decluttered default: the kind-group chips and the sort
    // toolbar (density / detail toggles included) are permanently hidden, so
    // the chooser is just filter + list + Apply-as + create. Typing the filter
    // already searches every type, so browsing chrome isn't needed to reach
    // anything; m_showAllTypes now controls ONLY the list content (common vs
    // all types) via the bottom "Show all types" row, not chrome. The modifier
    // row is always available (it's a core action) whenever the mode allows it.
    if (m_chipRow) m_chipRow->setVisible(false);
    if (m_sortRow) m_sortRow->setVisible(false);
    const bool modeAllowsMods = (m_mode == TypePopupMode::FieldType
                                 || m_mode == TypePopupMode::ArrayElement);
    if (m_modRow)  m_modRow->setVisible(modeAllowsMods);
}

void TypeSelectorPopup::popup(const QPoint& globalPos) {
    m_accepted = false;
    QFontMetrics fm(m_font);
    // Popup width scales with the font. The earlier hard 540-px cap was the
    // right size at the default 10 pt font but it never grew when the user
    // zoomed in; names elided into the size bar / size text on the right.
    // The new cap is `charW * 54` — equal to ~540 px at default zoom and
    // proportionally larger at higher zoom levels.
    const int charW = qMax(6, fm.horizontalAdvance(QStringLiteral("M")));
    const int maxPopupW = charW * 54;
    int iconColW = fm.height() + 6;
    int estMaxW = iconColW + fm.horizontalAdvance(QChar('W')) * m_cachedMaxNameLen + 16;
    int maxTextW = qMax(fm.horizontalAdvance(QStringLiteral("Choose element type        ")), estMaxW);
    int popupW = qBound(charW * 46, maxTextW + 24, maxPopupW);
    int rowH = fm.height() + 6;
    int headerH = rowH * 2 + 10;
    int footerH = rowH + 6;
    int listH = qBound(rowH * 3, rowH * (int)m_filteredTypes.size(), rowH * 16);
    int popupH = qMax(400, headerH + listH + footerH);

    placeOnScreen(globalPos, popupW, popupH);
}

void TypeSelectorPopup::updateModifierPreview() {
    const auto& t = ThemeManager::instance().current();
    QModelIndex idx = m_listView->currentIndex();
    int row = idx.isValid() ? idx.row() : -1;

    if (row < 0 || row >= m_filteredTypes.size()
        || m_filteredTypes[row].entryKind == TypeEntry::Section) {
        m_titleLabel->setText(QStringLiteral("<span style='color:%1'>Select a type</span>")
            .arg(t.textDim.name()));
        if (m_footerLabel) m_footerLabel->setText(QStringLiteral(
            "<span style='color:%1'>\u2191\u2193 navigate \u00B7 Enter select \u00B7 Esc dismiss \u00B7 Ctrl+F filter</span>")
            .arg(t.textFaint.name()));
        return;
    }

    const TypeEntry& entry = m_filteredTypes[row];

    // Disabled entry
    if (!entry.enabled) {
        m_titleLabel->setText(QStringLiteral("<span style='color:%1'>Not selectable</span>")
            .arg(t.textDim.name()));
        if (m_footerLabel) m_footerLabel->clear();
        return;
    }

    // Footer crumb: name · size · group (all dim, no garish colors)
    if (m_footerLabel) {
        QString szText = entry.sizeBytes > 0
            ? QStringLiteral("%1B").arg(entry.sizeBytes)
            : QStringLiteral("dyn");
        m_footerLabel->setText(QStringLiteral(
            "<span style='color:%1'>%2 \u00B7 %3 \u00B7 %4</span>")
            .arg(t.textFaint.name(), entry.displayName.toHtmlEscaped(), szText, entry.kindGroup));
    }

    int modId = currentModId();

    // Build modifier suffix
    QString suffix;
    if (modId == 1) suffix = QStringLiteral("*");
    else if (modId == 2) suffix = QStringLiteral("**");
    else if (modId == 3) {
        QString c = m_arrayCountEdit->text().trimmed();
        suffix = c.isEmpty() ? QStringLiteral("[n]") : QStringLiteral("[%1]").arg(c);
    }

    // Compute resulting size
    int newSize = entry.sizeBytes;
    if (modId == 1 || modId == 2)
        newSize = m_pointerSize;
    else if (modId == 3 && newSize > 0) {
        QString c = m_arrayCountEdit->text().trimmed();
        bool ok; int count = c.toInt(&ok);
        if (ok && count > 0) newSize *= count;
    }

    // Format: "type+modifier → size (+diff)"
    QString label = QStringLiteral("<span style='color:%1'>%2%3</span>")
        .arg(t.text.name(), entry.displayName.toHtmlEscaped(), suffix);

    if (newSize > 0) {
        label += QStringLiteral("<span style='color:%1'> \u2192 %2B</span>")
            .arg(t.textFaint.name()).arg(newSize);

        if (m_currentNodeSize > 0 && newSize != m_currentNodeSize) {
            int diff = newSize - m_currentNodeSize;
            QString sign = diff > 0 ? QStringLiteral("+") : QString();
            label += QStringLiteral("<span style='color:%1'> (%2%3)</span>")
                .arg(t.textFaint.name(), sign, QString::number(diff));
        }
    }

    m_titleLabel->setText(label);
}

void TypeSelectorPopup::updateDetailPane() {
    if (!m_showDetail || !m_detailContent) return;

    const auto& t = ThemeManager::instance().current();
    QModelIndex idx = m_listView->currentIndex();
    int row = idx.isValid() ? idx.row() : -1;

    if (row < 0 || row >= m_filteredTypes.size()
        || m_filteredTypes[row].entryKind == TypeEntry::Section
        || m_filteredTypes[row].isExpandToggle) {
        m_detailContent->setText(QStringLiteral(
            "<div style='color:%1;padding:6px'>No selection</div>").arg(t.textFaint.name()));
        return;
    }

    const TypeEntry& entry = m_filteredTypes[row];
    const QColor gc = kindGroupColor(entry.kindGroup);
    const int modId = currentModId();

    // Use the actual font point size — all HTML sizes are relative to this
    const int pt = resolvedPointSize(m_font);
    const int ptS = qMax(7, pt - 2);   // small text
    const int ptXS = qMax(7, pt - 3);  // section labels

    QString html;

    // ── Section label helper ──
    auto secLabel = [&](const QString& label) {
        return QStringLiteral(
            "<div style='font-size:%1pt;color:%2;text-transform:uppercase;"
            "padding:0 0 2px 0;margin:0 0 4px 0;"
            "border-bottom:1px solid %3'>%4</div>")
            .arg(ptXS).arg(t.textFaint.name(), t.border.name(), label);
    };
    // Key-value row (table)
    auto kv = [&](const QString& k, const QString& v, const QColor& vc) {
        return QStringLiteral(
            "<tr><td style='font-size:%1pt;color:%2;padding:1px 6px 1px 0'>%3</td>"
            "<td style='font-size:%1pt;color:%4;text-align:right'>%5</td></tr>")
            .arg(ptS).arg(t.textMuted.name(), k, vc.name(), v);
    };

    // ── Header ──
    html += QStringLiteral(
        "<div style='padding:7px 8px 5px 8px;border-bottom:1px solid %1;background:%2'>"
        "<div style='font-size:%3pt;font-weight:bold;color:%4'>%5</div>"
        "<div style='font-size:%6pt;color:%7'>%8</div></div>")
        .arg(t.border.name(), t.backgroundAlt.name())
        .arg(pt).arg(t.text.name(), entry.displayName.toHtmlEscaped())
        .arg(ptXS).arg(t.textMuted.name(), entry.kindGroup);

    // ── Layout ──
    html += QStringLiteral("<div style='padding:5px 8px;border-bottom:1px solid %1'>").arg(t.border.name());
    html += secLabel(QStringLiteral("layout"));
    html += QStringLiteral("<table style='width:100%;border-collapse:collapse'>");
    html += kv(QStringLiteral("size"),
               entry.sizeBytes > 0 ? QStringLiteral("%1 bytes").arg(entry.sizeBytes)
                                    : QStringLiteral("dynamic"),
               entry.sizeBytes > 0 ? t.syntaxNumber : t.textMuted);
    html += kv(QStringLiteral("alignment"), QStringLiteral("\u00D7%1").arg(entry.alignment), t.textDim);
    if (entry.sizeBytes > 0)
        html += kv(QStringLiteral("bits"), QString::number(entry.sizeBytes * 8), t.textDim);
    html += QStringLiteral("</table></div>");

    // ── Memory grid (types <= 16 bytes) ──
    if (entry.sizeBytes > 0 && entry.sizeBytes <= 16) {
        html += QStringLiteral("<div style='padding:5px 8px;border-bottom:1px solid %1'>").arg(t.border.name());
        html += secLabel(QStringLiteral("memory"));
        html += QStringLiteral("<table style='border-collapse:collapse'><tr>");
        QColor cellBg = kindGroupDimColor(entry.kindGroup);
        for (int i = 0; i < entry.sizeBytes; i++) {
            if (i == 8)
                html += QStringLiteral("</tr><tr>");  // wrap at 8 bytes
            html += QStringLiteral(
                "<td style='width:14px;height:14px;text-align:center;"
                "font-size:7pt;background:%1;border:1px solid %2;"
                "color:%3;padding:0'>%4</td>")
                .arg(cellBg.name(), t.border.name(), t.textFaint.name()).arg(i);
        }
        html += QStringLiteral("</tr></table></div>");
    }

    // ── Properties ──
    {
        html += QStringLiteral("<div style='padding:5px 8px;border-bottom:1px solid %1'>").arg(t.border.name());
        html += secLabel(QStringLiteral("properties"));
        html += QStringLiteral("<table style='width:100%;border-collapse:collapse'>");
        if (entry.entryKind == TypeEntry::Composite) {
            QString kw = entry.classKeyword.isEmpty() ? QStringLiteral("struct") : entry.classKeyword;
            html += kv(QStringLiteral("keyword"), kw, t.syntaxKeyword);
            if (entry.fieldCount > 0)
                html += kv(QStringLiteral("fields"), QString::number(entry.fieldCount), t.syntaxNumber);
        } else {
            html += kv(QStringLiteral("group"), entry.kindGroup, gc);
            if (isValidPrimitivePtrTarget(entry.primitiveKind))
                html += kv(QStringLiteral("ptr target"), QStringLiteral("yes"), t.syntaxType);
            else
                html += kv(QStringLiteral("ptr target"), QStringLiteral("no"), t.textFaint);
        }
        html += QStringLiteral("</table></div>");
    }

    // ── C declaration ──
    {
        html += QStringLiteral("<div style='padding:5px 8px;border-bottom:1px solid %1'>").arg(t.border.name());
        html += secLabel(QStringLiteral("C declaration"));
        html += QStringLiteral("<div style='font-size:%1pt;padding:4px 6px;background:%2;"
            "border:1px solid %3;line-height:1.6'>")
            .arg(ptS).arg(t.background.name(), t.border.name());

        QString tn = entry.displayName.toHtmlEscaped();
        if (modId == 1 || modId == 2) {
            QString stars = (modId == 2) ? QStringLiteral("**") : QStringLiteral("*");
            html += QStringLiteral("<span style='color:%1'>%2</span>"
                "<span style='color:%3'>%4</span>"
                " <span style='color:%5'>fieldName</span>"
                "<span style='color:%6'>;</span>")
                .arg(t.syntaxType.name(), tn, t.markerPtr.name(), stars,
                     t.text.name(), t.textMuted.name());
        } else if (modId == 3) {
            QString c = m_arrayCountEdit ? m_arrayCountEdit->text().trimmed() : QString();
            if (c.isEmpty()) c = QStringLiteral("n");
            html += QStringLiteral("<span style='color:%1'>%2</span>"
                " <span style='color:%3'>fieldName</span>"
                "<span style='color:%4'>[</span>"
                "<span style='color:%5'>%6</span>"
                "<span style='color:%4'>];</span>")
                .arg(t.syntaxType.name(), tn, t.text.name(),
                     t.textMuted.name(), t.syntaxNumber.name(), c);
        } else {
            html += QStringLiteral("<span style='color:%1'>%2</span>"
                " <span style='color:%3'>fieldName</span>"
                "<span style='color:%4'>;</span>")
                .arg(t.syntaxType.name(), tn, t.text.name(), t.textMuted.name());
        }

        int sz = entry.sizeBytes;
        if (modId == 1 || modId == 2) sz = m_pointerSize;
        else if (modId == 3 && sz > 0) {
            QString c = m_arrayCountEdit ? m_arrayCountEdit->text().trimmed() : QString();
            bool ok; int n = c.toInt(&ok);
            if (ok && n > 0) sz *= n;
        }
        if (sz > 0)
            html += QStringLiteral(" <span style='color:%1'>// %2B</span>")
                .arg(t.syntaxComment.name()).arg(sz);
        html += QStringLiteral("</div></div>");
    }

    // ── Fields (composites only) ──
    if (entry.entryKind == TypeEntry::Composite && !entry.fieldSummary.isEmpty()) {
        html += QStringLiteral("<div style='padding:5px 8px;border-bottom:1px solid %1'>").arg(t.border.name());
        html += secLabel(QStringLiteral("fields (%1)").arg(entry.fieldCount));
        for (const auto& f : entry.fieldSummary)
            html += QStringLiteral("<div style='font-size:%1pt;color:%2;padding:1px 0'>%3</div>")
                .arg(ptS).arg(t.textDim.name(), f.toHtmlEscaped());
        if (entry.fieldCount > entry.fieldSummary.size())
            html += QStringLiteral("<div style='font-size:%1pt;color:%2'>\u2026</div>")
                .arg(ptS).arg(t.textFaint.name());
        html += QStringLiteral("</div>");
    }

    // ── Actions ──
    {
        html += QStringLiteral("<div style='padding:5px 8px;border-top:1px solid %1'>").arg(t.border.name());
        html += QStringLiteral(
            "<div style='padding:3px 6px;margin-bottom:3px;background:%1;"
            "border:1px solid %2;font-size:%3pt'>"
            "<a href='action:select' style='color:%4;text-decoration:none'>"
            "\u2713 Select %5</a></div>")
            .arg(t.selection.name(), t.syntaxKeyword.name())
            .arg(ptS).arg(t.text.name(), entry.displayName.toHtmlEscaped());

        bool canPtr = entry.entryKind == TypeEntry::Composite
            || isValidPrimitivePtrTarget(entry.primitiveKind);
        if (canPtr)
            html += QStringLiteral(
                "<div style='padding:3px 6px;margin-bottom:2px;background:%1;"
                "border:1px solid %2;font-size:%3pt'>"
                "<a href='action:ptr' style='color:%4;text-decoration:none'>"
                "* Pointer \u2192 %5</a></div>")
                .arg(t.surface.name(), t.border.name())
                .arg(ptS).arg(t.textDim.name(), entry.displayName.toHtmlEscaped());

        html += QStringLiteral(
            "<div style='padding:3px 6px;background:%1;"
            "border:1px solid %2;font-size:%3pt'>"
            "<a href='action:arr' style='color:%4;text-decoration:none'>"
            "[ ] Array of %5</a></div>")
            .arg(t.surface.name(), t.border.name())
            .arg(ptS).arg(t.textDim.name(), entry.displayName.toHtmlEscaped());
        html += QStringLiteral("</div>");
    }

    m_detailContent->setText(html);
}

void TypeSelectorPopup::applyFilter(const QString& text) {
    // Remember the currently-selected entry's identity so a re-filter / chip
    // toggle / re-sort can RESTORE it after the rebuild, instead of snapping
    // the selection back to the first row. The old behavior meant typing one
    // more character (or toggling a chip) silently moved the selection, so a
    // following Enter accepted the wrong type. (setTypes() clears the current
    // index before calling us, so this never carries a selection across opens
    // — its own current-type pre-select still wins on open.)
    bool selHad = false;
    QString selName; TypeEntry::Kind selKind = TypeEntry::Primitive;
    NodeKind selPrim = NodeKind::Hex8; bool selRel = false; uint64_t selStruct = 0;
    if (QModelIndex ci = m_listView->currentIndex(); ci.isValid()) {
        int r = ci.row();
        if (r >= 0 && r < m_filteredTypes.size()
            && m_filteredTypes[r].entryKind != TypeEntry::Section) {
            const TypeEntry& s = m_filteredTypes[r];
            selHad = true; selName = s.displayName; selKind = s.entryKind;
            selPrim = s.primitiveKind; selRel = s.isRelative; selStruct = s.structId;
        }
    }

    m_filteredTypes.clear();
    m_matchPositions.clear();
    QStringList displayStrings;

    QString filterBase = text.trimmed();

    // C4 — text path: typing "int*", "int**", or "Ball[10]" reflects the
    // suffix into the segmented modifier and filters on the base name. The
    // base type still matches by fuzzy search, so "Ball[10]" finds Ball and
    // pre-arms the Array modifier. setModifier blocks signals + doesn't steal
    // focus, so it can't re-enter applyFilter or interrupt typing.
    {
        TypeSpec spec = parseTypeSpec(text);
        if (spec.isPointer || spec.arrayCount > 0) {
            int modId = spec.arrayCount > 0 ? 3 : (spec.ptrDepth >= 2 ? 2 : 1);
            setModifier(modId, spec.arrayCount);
            filterBase = spec.baseName.trimmed();
        }
    }

    // Build set of enabled groups from chips
    QSet<QString> enabledGroups;
    for (auto it = m_groupChips.begin(); it != m_groupChips.end(); ++it)
        if (it.value()->isChecked()) enabledGroups.insert(it.key());

    auto catAllowed = [&](const TypeEntry& t) {
        // Entries without a kindGroup always pass
        if (t.kindGroup.isEmpty()) return true;
        // Groups without a chip toggle (Vec/Str/Ctr) always visible
        if (!m_groupChips.contains(t.kindGroup)) return true;
        return enabledGroups.contains(t.kindGroup);
    };

    auto makeLabel = [](const TypeEntry& e) {
        QString label = e.displayName;
        if (e.sizeBytes > 0) label += QStringLiteral(" - %1B").arg(e.sizeBytes);
        return label;
    };

    // A C-identifier the user could legally name a new class (B1 gate).
    auto isValidIdent = [](const QString& s) {
        if (s.isEmpty()) return false;
        if (!(s.at(0).isLetter() || s.at(0) == QLatin1Char('_'))) return false;
        for (const QChar& c : s)
            if (!(c.isLetterOrNumber() || c == QLatin1Char('_'))) return false;
        return true;
    };

    // Build a synthetic "＋ Create/New" row. The label tracks the active
    // modifier (computed here, so the C4 text path keeps it live since typing
    // re-runs applyFilter). name empty → "＋ New struct/class"; else
    // "＋ Create class “Foo*”". The row stores createName/createKeyword so
    // acceptIndex can emit createNewTypeRequested in one step.
    auto makeCreateEntry = [this](const QString& name, const QString& keyword) {
        TypeEntry e;
        e.entryKind    = TypeEntry::Primitive;   // selectable (not a Section)
        e.isCreateNew  = true;
        e.enabled      = true;
        e.createName   = name;
        e.createKeyword = keyword;
        int modId = currentModId();
        QString suffix;
        if (modId == 1) suffix = QStringLiteral("*");
        else if (modId == 2) suffix = QStringLiteral("**");
        else if (modId == 3) {
            QString c = m_arrayCountEdit ? m_arrayCountEdit->text().trimmed() : QString();
            suffix = QStringLiteral("[%1]").arg(c.isEmpty() ? QStringLiteral("n") : c);
        }
        const QString kw = keyword.isEmpty() ? QStringLiteral("struct") : keyword;
        e.displayName = name.isEmpty()
            ? QStringLiteral("＋ New %1").arg(kw)
            : QStringLiteral("＋ Create %1 “%2%3”").arg(kw, name, suffix);
        return e;
    };

    QHash<QString, int> groupCounts;       // counts of matched/visible types per group
    QHash<QString, int> totalGroupCounts;  // counts of ALL types per group (for chip totals)
    const int totalTypes = m_allTypes.size();
    int totalNonSection = 0;

    // Pre-compute total counts per group (independent of filter)
    for (const auto& t : m_allTypes) {
        if (t.entryKind == TypeEntry::Section) continue;
        totalNonSection++;
        totalGroupCounts[t.kindGroup]++;  // kindGroup normalized in setTypes
    }

    // Pre-reserve to avoid realloc churn
    m_filteredTypes.reserve(totalTypes);
    m_matchPositions.reserve(totalTypes);
    displayStrings.reserve(totalTypes);

    bool hasCreateRow = false;

    if (!filterBase.isEmpty()) {
        // ── Fuzzy search: flat ranked list, no section headers ──
        // Use index + score to avoid deep-copying TypeEntry structs
        struct Scored { int idx; int score; QVector<int> pos; };
        QVector<Scored> scored;
        scored.reserve(totalTypes);

        bool exactMatch = false;
        for (int i = 0; i < totalTypes; i++) {
            const auto& t = m_allTypes[i];
            if (t.entryKind == TypeEntry::Section) continue;
            if (t.displayName.compare(filterBase, Qt::CaseInsensitive) == 0)
                exactMatch = true;
            QVector<int> pos;
            int sc = fuzzyScore(filterBase, t.displayName, &pos);
            if (sc <= 0) continue;
            groupCounts[t.kindGroup]++;
            if (catAllowed(t))
                scored.push_back(Scored{i, sc, std::move(pos)});
        }
        std::stable_sort(scored.begin(), scored.end(),
                  [](const Scored& a, const Scored& b) { return a.score > b.score; });

        for (const auto& s : scored) {
            m_filteredTypes.append(m_allTypes[s.idx]);
            m_matchPositions.append(s.pos);
            displayStrings << makeLabel(m_allTypes[s.idx]);
        }

        // B1 — one-step "＋ Create class “Foo”" when the typed base is a fresh
        // identifier with no exact match. Appended (not prepended) so a strong
        // match like "int" still auto-selects on Enter; only when there are no
        // matches at all does the create row become the default selection.
        if (isValidIdent(filterBase) && !exactMatch) {
            TypeEntry ce = makeCreateEntry(filterBase, QStringLiteral("class"));
            m_filteredTypes.append(ce);
            m_matchPositions.append(QVector<int>());
            displayStrings << ce.displayName;
            hasCreateRow = true;
        }
    } else {
        // ── No filter: build list with current sort mode ──
        // Bucket by kindGroup, count all, filter by enabled groups
        static const char* kGroupOrder[] = {"Hex","Int","Float","Ptr","Vec","Str","Ctr","Common"};
        static const char* kGroupLabels[] = {"Hex","Int / Bool","Float","Pointer / FuncPtr","Vec / Mat","String","Type","Common Types"};
        QHash<QString, QVector<TypeEntry>> buckets;
        for (const auto& t : m_allTypes) {
            if (t.entryKind == TypeEntry::Section) continue;
            groupCounts[t.kindGroup]++;  // kindGroup normalized in setTypes
            if (catAllowed(t))
                buckets[t.kindGroup].append(t);
        }

        // In grouped view, each section sorts largest → smallest by byte size
        // (dynamic-size entries with sizeBytes==0 sink to the bottom); ties
        // break alphabetically.
        auto bySizeDesc = [](const TypeEntry& a, const TypeEntry& b) {
            if (a.sizeBytes != b.sizeBytes) return a.sizeBytes > b.sizeBytes;
            return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
        };
        auto alphabetical = [](const TypeEntry& a, const TypeEntry& b) {
            return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
        };

        auto appendSection = [&](const QString& title, const QString& groupKey,
                                 const QVector<TypeEntry>& items,
                                 bool collapsible = false,
                                 bool defaultExpanded = true) {
            if (items.isEmpty()) return;
            // Collapsible sections honour the user's per-section toggle
            // (m_sectionExpanded), falling back to the built-in default;
            // m_showAllTypes (test/harness hook) force-expands everything.
            const bool expanded = !collapsible ? true
                : (m_showAllTypes
                   || m_sectionExpanded.value(groupKey, defaultExpanded));
            TypeEntry sec;
            sec.entryKind = TypeEntry::Section;
            // Bake the member count into a collapsible header so the size is
            // visible before expanding ("Your classes (1003)").
            sec.displayName = collapsible
                ? QStringLiteral("%1 (%2)").arg(title).arg(items.size())
                : title;
            // Use the EXACT group key for the pip color. Deriving it from the
            // label's first word produced "Pointer"/"String"/"Type" — none of
            // which kindGroupColor() knows ("Ptr"/"Str"/"Ctr"), so those pips
            // fell through to the neutral default instead of red/salmon/green.
            sec.kindGroup = groupKey;
            sec.enabled = false;
            sec.sectionCollapsible = collapsible;
            sec.sectionExpanded = expanded;
            sec.sectionKey = groupKey;
            m_filteredTypes.append(sec);
            m_matchPositions.append(QVector<int>());
            displayStrings << sec.displayName;
            if (!expanded) return;   // collapsed: header only
            for (const auto& c : items) {
                m_filteredTypes.append(c);
                m_matchPositions.append(QVector<int>());
                displayStrings << makeLabel(c);
            }
        };

        if (m_sortMode == SortGroup) {
            // Collapsible-section model: common primitive groups (Hex / Int /
            // Float / Ptr) are what people reach for, so they're listed first
            // and expanded by default; "Your classes" and the std-lib group are
            // ordinary collapsible sections, COLLAPSED by default — so a large
            // SDK (1000s of classes) never floods the view or buries the
            // primitives. Click a header (or Enter) to expand/collapse it; the
            // filter still fuzzy-searches every type regardless of collapse.

            // Recent section — entries whose displayName matches a recent
            // pick. Listed first so common selections are one chord away.
            // Items still appear in their normal kindGroup section below.
            if (!m_recentNames.isEmpty()) {
                QVector<TypeEntry> recents;
                QHash<QString, TypeEntry> byName;
                for (const auto& items : buckets)
                    for (const auto& p : items) byName.insert(p.displayName, p);
                for (const QString& nm : m_recentNames) {
                    auto it = byName.constFind(nm);
                    if (it != byName.constEnd() && it->enabled)
                        recents.append(*it);
                }
                if (!recents.isEmpty())
                    appendSection(QStringLiteral("Recent"), QStringLiteral("Common"), recents);
            }
            // Group view: per-kindGroup collapsible sections. Common primitive
            // groups expand by default; "Your classes" (Ctr) and the std-lib
            // "Common types" group collapse by default so they never flood.
            for (int gi = 0; gi < 8; gi++) {
                QString g = QString::fromLatin1(kGroupOrder[gi]);
                auto& items = buckets[g];
                if (items.isEmpty()) continue;
                // Same-size-first sorting for the matching group — except
                // the Hex group, where the user reads it as a fixed size
                // ladder (hex128 → hex64 → hex32 → hex16 → hex8). When the
                // current node was an 8-byte hex64, "same-size first" put
                // hex64 above hex128, which broke the size-descending
                // expectation. Always sort Hex by size desc.
                if (g == QStringLiteral("Hex")) {
                    std::stable_sort(items.begin(), items.end(), bySizeDesc);
                } else if (m_mode != TypePopupMode::Root && m_currentNodeSize > 0) {
                    QVector<TypeEntry> sameSize, other;
                    for (const auto& p : items) {
                        if (p.sizeBytes == m_currentNodeSize) sameSize.append(p);
                        else other.append(p);
                    }
                    std::stable_sort(sameSize.begin(), sameSize.end(), alphabetical);
                    std::stable_sort(other.begin(), other.end(), bySizeDesc);
                    items = sameSize + other;
                } else {
                    std::stable_sort(items.begin(), items.end(), bySizeDesc);
                }
                // Default expand state:
                //  • hex/int/float/pointer — what people reach for → expanded
                //  • Your classes (Ctr) — expanded only for a SMALL SDK; a big
                //    one (1000s) stays collapsed so it can't flood / bury the
                //    primitives. Either way it's one click to toggle.
                //  • everything else (Vec/Str/std-lib) → collapsed
                constexpr int kAutoExpandClassCap = 8;
                bool defExpand;
                if (g == QStringLiteral("Hex") || g == QStringLiteral("Int")
                    || g == QStringLiteral("Float") || g == QStringLiteral("Ptr"))
                    defExpand = true;
                else if (g == QStringLiteral("Ctr"))
                    defExpand = (items.size() <= kAutoExpandClassCap);
                else
                    defExpand = false;
                const QString title =
                      g == QStringLiteral("Ctr")    ? QStringLiteral("Your classes")
                    : g == QStringLiteral("Common") ? QStringLiteral("Common types")
                    : QString::fromLatin1(kGroupLabels[gi]);
                appendSection(title, g, items, /*collapsible=*/true, defExpand);
            }
            // Pinned one-click "＋ New struct / ＋ New class" creators at the
            // BOTTOM: people reach for primitives/their classes first, and the
            // primary create path is typing a fresh name (the inline "＋ Create
            // class" row), so these stay as an out-of-the-way fallback rather
            // than pushing the common primitives down. Auto-named (empty
            // createName → controller's uniqueStructName).
            {
                QVector<TypeEntry> creators;
                creators.append(makeCreateEntry(QString(), QString()));
                creators.append(makeCreateEntry(QString(), QStringLiteral("class")));
                appendSection(QStringLiteral("Create"), QStringLiteral("Ctr"), creators);
            }
        } else {
            // Flat sorted list (no sections) — name/size/align
            QVector<TypeEntry> all;
            for (auto& items : buckets) all += items;
            // Simple mode applies in the flat sorts too (else switching to
            // name/size/align would silently reveal everything with no way
            // back): drop the std-lib "Common Types" + long-tail primitives,
            // exactly as the grouped view does.
            if (!m_showAllTypes) {
                QVector<TypeEntry> keep;
                keep.reserve(all.size());
                for (const auto& it : all)
                    if (it.kindGroup != QStringLiteral("Common")
                        && (it.entryKind != TypeEntry::Primitive || isCommonKind(it.primitiveKind)))
                        keep.append(it);
                all = keep;
            }
            int dir = m_sortDir;
            switch (m_sortMode) {
            case SortName:
                std::stable_sort(all.begin(), all.end(), [dir](const TypeEntry& a, const TypeEntry& b) {
                    return dir * a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
                });
                break;
            case SortSize:
                std::stable_sort(all.begin(), all.end(), [dir](const TypeEntry& a, const TypeEntry& b) {
                    if (a.sizeBytes != b.sizeBytes) return dir * (a.sizeBytes - b.sizeBytes) < 0;
                    return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
                });
                break;
//TODO-DELETE(case SortAlign in flat-sort switch)             case SortAlign:
//                std::stable_sort(all.begin(), all.end(), [dir](const TypeEntry& a, const TypeEntry& b) {
//                    if (a.alignment != b.alignment) return dir * (a.alignment - b.alignment) < 0;
//                    return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
//                });
//                break;
            default: break;
            }
            for (const auto& c : all) {
                m_filteredTypes.append(c);
                m_matchPositions.append(QVector<int>());
                displayStrings << makeLabel(c);
            }
        }
        // (The old global "Show all types" toggle row is gone — expanding the
        // per-section headers replaces it.)
    }

    // Empty state — the expand/collapse toggle row is not a real type, so
    // it must not inflate the "N types" status count or suppress the
    // empty-state placeholder.
    int resultCount = 0;
    for (const auto& f : m_filteredTypes)
        if (f.entryKind != TypeEntry::Section && !f.isExpandToggle && !f.isCreateNew)
            resultCount++;

    // A lone "＋ Create" row (no matches but a valid fresh name) is the answer,
    // so suppress the "No types match" placeholder in that case.
    if (resultCount == 0 && !hasCreateRow) {
        TypeEntry empty;
        empty.entryKind = TypeEntry::Section;
        empty.displayName = filterBase.isEmpty()
            ? QStringLiteral("No types available")
            : QStringLiteral("No types match \u2018%1\u2019").arg(filterBase);
        empty.enabled = false;
        m_filteredTypes.append(empty);
        m_matchPositions.append(QVector<int>());
        displayStrings << empty.displayName;
    }

    // Point the delegate at the freshly-rebuilt vectors BEFORE setStringList:
    // setStringList can trigger a synchronous repaint, and if the delegate's
    // pointers were still unset (first open) that paint rendered rows with no
    // entry/highlight data. The vectors are fully populated by here, so this is
    // safe — and it closes the transient mismatch window.
    if (auto* delegate = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate())) {
        delegate->setFilteredTypes(&m_filteredTypes);
        delegate->setMatchPositions(&m_matchPositions);
    }

    m_model->setStringList(displayStrings);

    for (auto it = m_groupChips.begin(); it != m_groupChips.end(); ++it) {
        int visible = groupCounts.value(it.key(), 0);
        int total = totalGroupCounts.value(it.key(), 0);
        auto* chip = static_cast<CategoryChip*>(it.value());
        if (!filterBase.isEmpty())
            chip->setCount(visible, total);
        else
            chip->setCount(total);
    }
    // Equalize chip widths to the widest natural size so the row reads as a
    // uniform strip. Without this, "Hex (4)" and "Int (11)" end up different
    // widths, and with a 2px QHBoxLayout spacing between them the row looks
    // ragged. Chips with shorter content simply gain extra symmetric padding
    // (centring in paintEvent keeps their pip+text block visually balanced).
    {
        int maxW = 0;
        for (auto* chip : m_groupChips)
            maxW = qMax(maxW, chip->sizeHint().width());
        for (auto* chip : m_groupChips)
            chip->setFixedWidth(maxW);
    }

    if (m_statusLabel) {
        if (!filterBase.isEmpty())
            m_statusLabel->setText(QStringLiteral("%1 of %2").arg(resultCount).arg(totalNonSection));
        else
            m_statusLabel->setText(QStringLiteral("%1 types").arg(resultCount));
    }

    // While a text filter is active the result list is fuzzy-rank ordered and
    // the sort mode is ignored, so grey out the sort buttons to signal they
    // have no effect. (The group chips DO still filter the fuzzy results, so
    // they stay enabled.)
    {
        const bool filtering = !filterBase.isEmpty();
        for (auto* b : m_sortBtns)
            if (b) b->setEnabled(!filtering);
    }

    // Restore the previously-selected entry if it survived the rebuild;
    // otherwise fall back to the first selectable row.
    int target = -1;
    if (selHad) {
        for (int i = 0; i < m_filteredTypes.size(); ++i) {
            const TypeEntry& e = m_filteredTypes[i];
            if (e.entryKind == TypeEntry::Section || !e.enabled) continue;
            if (e.displayName != selName || e.entryKind != selKind) continue;
            bool same = (selKind == TypeEntry::Composite)
                ? (e.structId == selStruct)
                : (e.primitiveKind == selPrim && e.isRelative == selRel);
            if (same) { target = i; break; }
        }
    }
    if (target < 0)
        target = nextSelectableRow(0, 1);
    if (target >= 0)
        m_listView->setCurrentIndex(m_model->index(target));
}

void TypeSelectorPopup::acceptCurrent() {
    QModelIndex idx = m_listView->currentIndex();
    if (idx.isValid())
        acceptIndex(idx.row());
}

void TypeSelectorPopup::acceptIndex(int row) {
    // Ignore accepts while the skeleton placeholders are showing: the model
    // has dummy rows but m_filteredTypes still holds the PREVIOUS open's
    // entries, so accepting here would emit a stale, unrelated type.
    if (m_loading) return;
    if (row < 0 || row >= m_filteredTypes.size()) return;
    // Copy by value: the typeSelected slot can rebuild the document and
    // re-enter applyFilter(), which clears m_filteredTypes — a reference into
    // it would dangle mid-emit.
    const TypeEntry entry = m_filteredTypes[row];
    // Expand/collapse row: flip the simple/all view and re-render in place.
    // Never emits a selection and never hides the popup.
//TODO-DELETE(acceptIndex() isExpandToggle branch)     if (entry.isExpandToggle) {
//        m_showAllTypes = !m_showAllTypes;
//        updateModeChrome();
//        applyFilter(m_filterEdit ? m_filterEdit->text() : QString());
//        return;
//    }
    // B1/B5 — create-new action row: create + apply in one step, carrying the
    // typed name (or empty → controller auto-names), the keyword, and the
    // active modifier so the new class lands as value/pointer/array directly.
    if (entry.isCreateNew) {
        int modId = currentModId();
        int arrCount = 0;
        if (modId == 3) {
            QString c = m_arrayCountEdit ? m_arrayCountEdit->text().trimmed() : QString();
            bool ok = false;
            int n = c.toInt(&ok);
            arrCount = (ok && n > 0) ? n : 1;
        }
        m_accepted = true;
        hide();
        emit createNewTypeRequested(modId, arrCount, entry.createName, entry.createKeyword);
        return;
    }
    // Collapsible section header: toggle its expand state and re-render in
    // place (never emits a type, never hides the popup).
    if (entry.entryKind == TypeEntry::Section) {
        if (entry.sectionCollapsible) {
            const QString key = entry.sectionKey;
            m_sectionExpanded[key] = !entry.sectionExpanded;
            applyFilter(m_filterEdit ? m_filterEdit->text() : QString());
            // applyFilter's restore only tracks real entries, so the header
            // selection would otherwise jump to the top — keep focus on the
            // header the user just toggled.
            for (int i = 0; i < m_filteredTypes.size(); ++i)
                // Require sectionCollapsible: the non-collapsible "Create" and
                // "Recent" headers reuse the "Ctr"/"Common" keys, so a bare key
                // match would land on them instead of the toggled section.
                if (m_filteredTypes[i].entryKind == TypeEntry::Section
                    && m_filteredTypes[i].sectionCollapsible
                    && m_filteredTypes[i].sectionKey == key) {
                    m_listView->setCurrentIndex(m_model->index(i));
                    break;
                }
        }
        return;
    }
    if (!entry.enabled) return;

    // Build full text with modifier from toggle buttons
    int modId = currentModId();
    QString fullText = entry.displayName;
    if (modId == 1)
        fullText += QStringLiteral("*");
    else if (modId == 2)
        fullText += QStringLiteral("**");
    else if (modId == 3) {
        // Array toggle is on → always emit an array. An empty/blank count
        // defaults to 1 rather than silently dropping the "[]" and committing
        // a plain (non-array) type, which contradicted the visible preview.
        QString countText = m_arrayCountEdit ? m_arrayCountEdit->text().trimmed() : QString();
        bool ok = false;
        int count = countText.toInt(&ok);
        if (!ok || count < 1) count = 1;
        fullText += QStringLiteral("[%1]").arg(count);
    }

    // Hide before emitting so the popup is inert while the slot runs.
    m_accepted = true;
    hide();
    emit typeSelected(entry, fullText);
}

int TypeSelectorPopup::nextSelectableRow(int from, int direction) const {
    int i = from;
    while (i >= 0 && i < m_filteredTypes.size()) {
        const auto& e = m_filteredTypes[i];
        // Real, enabled entries are selectable; so are collapsible section
        // headers (so the keyboard can land on one and Enter toggles it).
        if ((e.entryKind != TypeEntry::Section && e.enabled)
            || (e.entryKind == TypeEntry::Section && e.sectionCollapsible))
            return i;
        i += direction;
    }
    return -1;
}

bool TypeSelectorPopup::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);

        if (ke->key() == Qt::Key_Escape) {
            hide();
            return true;
        }

        // Ctrl+F focuses the filter from anywhere. Exact-match the modifier so
        // Ctrl+Shift+F / Ctrl+Alt+F don't get hijacked.
        if (ke->key() == Qt::Key_F && ke->modifiers() == Qt::ControlModifier) {
            m_filterEdit->setFocus();
            m_filterEdit->selectAll();
            return true;
        }

        if (obj == m_filterEdit) {
            if (ke->key() == Qt::Key_Down) {
                m_listView->setFocus();
                QModelIndex cur = m_listView->currentIndex();
                int startRow = cur.isValid() ? cur.row() : 0;
                int next = nextSelectableRow(startRow, 1);
                if (next >= 0)
                    m_listView->setCurrentIndex(m_model->index(next));
                return true;
            }
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                acceptCurrent();
                return true;
            }
        }

        if (obj == m_listView) {
            if (ke->key() == Qt::Key_Up) {
                QModelIndex cur = m_listView->currentIndex();
                if (!cur.isValid() || cur.row() == 0) {
                    m_filterEdit->setFocus();
                    return true;
                }
                // Skip sections and disabled entries
                int prev = nextSelectableRow(cur.row() - 1, -1);
                if (prev < 0) {
                    m_filterEdit->setFocus();
                    return true;
                }
                m_listView->setCurrentIndex(m_model->index(prev));
                return true;
            }
            if (ke->key() == Qt::Key_Down) {
                QModelIndex cur = m_listView->currentIndex();
                int startRow = cur.isValid() ? cur.row() + 1 : 0;
                int next = nextSelectableRow(startRow, 1);
                if (next >= 0)
                    m_listView->setCurrentIndex(m_model->index(next));
                return true;
            }
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                acceptCurrent();
                return true;
            }
            // Backspace in list removes last filter char
            if (ke->key() == Qt::Key_Backspace) {
                QString txt = m_filterEdit->text();
                if (!txt.isEmpty()) {
                    m_filterEdit->setText(txt.left(txt.size() - 1));
                    m_filterEdit->setFocus();
                }
                return true;
            }
            // Forward printable keys to filter edit for type-to-filter
            if (!ke->text().isEmpty() && ke->text()[0].isPrint()) {
                m_filterEdit->setFocus();
                m_filterEdit->setText(m_filterEdit->text() + ke->text());
                return true;
            }
        }
    }

    return QFrame::eventFilter(obj, event);
}

void TypeSelectorPopup::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);
    // 1px border drawn manually (QFrame::Box draws 2px with Fusion)
    QPainter p(this);
    QColor bd = palette().color(QPalette::Dark);
    int w = width(), h = height();
    p.fillRect(0, 0, w, 1, bd);
    p.fillRect(0, h - 1, w, 1, bd);
    p.fillRect(0, 0, 1, h, bd);
    p.fillRect(w - 1, 0, 1, h, bd);
}

void TypeSelectorPopup::hideEvent(QHideEvent* event) {
    QFrame::hideEvent(event);
    // Dismiss the delegate's themed field-summary tooltip. It's a
    // top-level Qt::ToolTip window separate from the popup, so it
    // would otherwise stay on screen after the chooser is closed.
    if (m_listView) {
        // The delegate is the static_cast'able TypeSelectorDelegate
        // we install in the constructor — there is only ever one item
        // delegate set on m_listView. No Q_OBJECT on the delegate
        // class, so qobject_cast doesn't apply.
        if (auto* d = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate()))
            d->hideTip();
    }
    // Only emit dismissed() when the popup closed WITHOUT a pick — a
    // successful accept already emitted typeSelected/createNewTypeRequested.
    if (!m_accepted)
        emit dismissed();
}

} // namespace rcx
