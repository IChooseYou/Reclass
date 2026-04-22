#include "typeselectorpopup.h"

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

namespace rcx {

// ── CategoryChip — flat custom-painted toggle button ──
// No CSS, no Fusion — pure QPainter with direct theme colors.

class CategoryChip : public QAbstractButton {
public:
    explicit CategoryChip(const QString& label, QWidget* parent = nullptr)
        : QAbstractButton(parent), m_label(label)
    {
        setCheckable(true);
        setChecked(true);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setMouseTracking(true);
    }

    void setCount(int n) { m_count = n; m_totalCount = -1; update(); }
    void setCount(int visible, int total) { m_count = visible; m_totalCount = total; update(); }
    void setGroupColor(const QColor& c) { m_groupColor = c; update(); }

    QSize sizeHint() const override {
        QFontMetrics fm(font());
        QString text = chipText();
        // Natural width: pip(5) + gap(4) + text + 16px total horizontal pad
        // (split as 8 on each side at paint time, since the block is centred).
        // Chips are further equalized to the row's max width by
        // TypeSelectorPopup so the 4-chip row reads as a uniform strip even
        // when digit counts differ between labels.
        return QSize(5 + 4 + fm.horizontalAdvance(text) + 16, fm.height() + 4);
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const auto& t = ThemeManager::instance().current();
        bool hov = underMouse();
        bool chk = isChecked();
        QColor gc = m_groupColor.isValid() ? m_groupColor : t.textMuted;

        if (hov) p.fillRect(rect(), t.hover);

        // Centre the pip + text block horizontally. Combined with chips being
        // equalized to a uniform width, this yields consistent visual spacing
        // on both sides of every chip regardless of how many digits its count
        // has. (Previously content hugged the left edge and each chip had
        // different right-over-run, making the row look ragged.)
        const int pipSz = 5;
        const int gap   = 4;
        QFontMetrics fm(font());
        int textW  = fm.horizontalAdvance(chipText());
        int blockW = pipSz + gap + textW;
        int x      = (width() - blockW) / 2;
        int baseline = (height() + fm.ascent() - fm.descent()) / 2;

        p.fillRect(x, (height() - pipSz) / 2, pipSz, pipSz, chk ? gc : t.textFaint);
        x += pipSz + gap;

        p.setPen(chk ? gc : t.textMuted);
        p.setFont(font());
        p.drawText(x, baseline, chipText());
    }

    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    QString chipText() const {
        if (m_count < 0) return m_label;
        // Space between label and count so "Int(11)" reads as "Int (11)" —
        // less cramped and avoids visual collision with neighbouring chips.
        if (m_totalCount >= 0 && m_totalCount != m_count)
            return QStringLiteral("%1 (%2/%3)").arg(m_label).arg(m_count).arg(m_totalCount);
        return QStringLiteral("%1 (%2)").arg(m_label).arg(m_count);
    }

    QString m_label;
    int m_count = -1;
    int m_totalCount = -1;
    QColor m_groupColor;
};

// ── parseTypeSpec ──

TypeSpec parseTypeSpec(const QString& text) {
    TypeSpec spec;
    QString s = text.trimmed();
    if (s.isEmpty()) return spec;

    // Check for pointer suffix: "Ball*" or "Ball**"
    if (s.endsWith('*')) {
        spec.isPointer = true;
        s.chop(1);
        spec.ptrDepth = 1;
        if (s.endsWith('*')) { s.chop(1); spec.ptrDepth = 2; }
        spec.baseName = s.trimmed();
        return spec;
    }

    // Check for array suffix: "int32_t[10]"
    int bracket = s.indexOf('[');
    if (bracket > 0 && s.endsWith(']')) {
        spec.baseName = s.left(bracket).trimmed();
        QString countStr = s.mid(bracket + 1, s.size() - bracket - 2);
        bool ok;
        int count = countStr.toInt(&ok);
        if (ok && count > 0)
            spec.arrayCount = count;
        return spec;
    }

    spec.baseName = s;
    return spec;
}

// ── Fuzzy scorer: subsequence match with word-boundary bonuses ──
// Hot path — uses stack arrays and pre-lowered QChars to avoid heap allocs.

static constexpr int kMaxFuzzyLen = 64;

static int fuzzyScore(const QString& pattern, const QString& text,
                      QVector<int>* outPositions = nullptr) {
    int pLen = pattern.size(), tLen = text.size();
    if (pLen == 0) return 1;
    if (pLen > tLen) return 0;
    if (pLen > kMaxFuzzyLen || tLen > 256) {
        // Fallback: prefix match only for very long names
        if (text.startsWith(pattern, Qt::CaseInsensitive)) return 1;
        return 0;
    }

    // Pre-compute lowercase chars on the stack
    QChar pLow[kMaxFuzzyLen];
    for (int i = 0; i < pLen; i++) pLow[i] = pattern[i].toLower();
    QChar tLow[256];
    for (int i = 0; i < tLen; i++) tLow[i] = text[i].toLower();

    // Quick subsequence reject using pre-lowered arrays
    { int pi = 0;
      for (int ti = 0; ti < tLen && pi < pLen; ti++)
          if (pLow[pi] == tLow[ti]) pi++;
      if (pi < pLen) return 0;
    }

    // Recursive best-match (bounded: max 4 branches per pattern char)
    // Stack arrays instead of QVector to avoid heap allocation
    int bestPos[kMaxFuzzyLen];
    int curPos[kMaxFuzzyLen];
    int best = 0;
    int bestLen = 0;

    auto solve = [&](auto& self, int pi, int ti, int curLen, int score) -> void {
        if (pi == pLen) {
            if (score > best) {
                best = score;
                bestLen = curLen;
                memcpy(bestPos, curPos, curLen * sizeof(int));
            }
            return;
        }
        int maxTi = tLen - (pLen - pi);
        int branches = 0;
        for (int i = ti; i <= maxTi && branches < 4; i++) {
            if (pLow[pi] != tLow[i]) continue;
            int bonus = 1;
            if (i == 0)                                          bonus = 10;
            else if (text[i - 1] == '_' || text[i - 1] == ' ') bonus = 8;
            else if (text[i].isUpper() && text[i - 1].isLower()) bonus = 8;
            if (curLen > 0 && i == curPos[curLen - 1] + 1)      bonus += 5;
            curPos[curLen] = i;
            self(self, pi + 1, i + 1, curLen + 1, score + bonus);
            branches++;
        }
    };

    solve(solve, 0, 0, 0, 0);
    if (best > 0) {
        best += qMax(0, 20 - (tLen - pLen));  // tightness bonus
        if (pLen == tLen) best += 20;          // exact match bonus
        if (outPositions) {
            outPositions->resize(bestLen);
            memcpy(outPositions->data(), bestPos, bestLen * sizeof(int));
        }
    }
    return best;
}

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
    // Blend 18% accent + 82% dark background
    return QColor(
        (c.red()   * 18 + 0x1e * 82) / 100,
        (c.green() * 18 + 0x1e * 82) / 100,
        (c.blue()  * 18 + 0x1e * 82) / 100);
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

    void setFont(const QFont& f) {
        m_font = f;
        m_fm = QFontMetrics(f);
        m_smallFont = f;
        m_smallFont.setPointSize(qMax(7, f.pointSize() - 2));
        m_sfm = QFontMetrics(m_smallFont);
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

    // Column layout: [2px accent | 4px pad | icon | 4px | name ... | 6 | bar 32px | 6 | size 38px | 6]
    static constexpr int kBarW  = 32;
    static constexpr int kMaxSz = 64;
    static constexpr int kSzCol = 38;
    static constexpr int kAccent = 2;
    static constexpr int kPadL  = 4;

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
            painter->fillRect(r.x(), y, kAccent, h, groupCol);
        } else if (isHov && !isSection && !isDisabled) {
            painter->fillRect(r, t.hover);
        }

        // ── Section header: pip + left-aligned text ──
        if (isSection) {
            const int px = r.x() + kAccent + kPadL + 1;
            const int pipSz = 4;
            painter->fillRect(px, y + (h - pipSz) / 2, pipSz, pipSz, groupCol);
            painter->setFont(m_smallFont);
            painter->setPen(t.textFaint);
            const int tx = px + pipSz + 4;
            painter->drawText(tx, y + (h + m_sfm.ascent() - m_sfm.descent()) / 2,
                              index.data().toString());
            painter->restore();
            return;
        }

        // ── Columns ──
        int x = r.x() + kAccent + kPadL;
        const int rightW = 6 + kBarW + 6 + kSzCol + 6;  // gap + bar + gap + size + pad
        const int nameEnd = r.right() - rightW;

        // ── Icon ──
        const int iconSz = m_fm.height();
        if (entry) {
            static QIcon sI(QStringLiteral(":/vsicons/symbol-class.svg"));
            static QIcon eI(QStringLiteral(":/vsicons/symbol-enum.svg"));
            static QIcon pI(QStringLiteral(":/vsicons/symbol-variable.svg"));
            const QIcon& ico = (entry->entryKind == TypeEntry::Composite)
                ? (entry->category == TypeEntry::CatEnum ? eI : sI) : pI;
            const int iy = y + (h - iconSz) / 2;
            if (isDisabled) {
                painter->setOpacity(0.25);
                ico.paint(painter, x, iy, iconSz, iconSz);
                painter->setOpacity(1.0);
            } else {
                ico.paint(painter, x, iy, iconSz, iconSz);
            }
        }
        x += iconSz + 4;

        // ── Name: baseline-aligned for pixel-perfect text ──
        const QColor nameColor = isDisabled ? t.textMuted
                               : isSel      ? t.text
                                            : t.text;
        painter->setFont(m_font);
        const QString fullText = index.data().toString();
        const int dashIdx = fullText.lastIndexOf(QStringLiteral(" - "));
        const QString namePart = dashIdx > 0 ? fullText.left(dashIdx) : fullText;
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
            const int szTextX = r.right() - 6 - szTextW;  // right-aligned in right pad

            // Size bar: positioned to the left of size text
            const int barX = szTextX - 6 - kBarW;  // gap between bar and text
            const int barH = 6;
            const int barY = y + (h - barH) / 2;

            painter->fillRect(barX, barY, kBarW, barH, t.surface);
            if (entry->sizeBytes > 0) {
                const int fillW = qBound(1, entry->sizeBytes * kBarW / kMaxSz, kBarW);
                QColor fc = groupCol;
                fc.setAlpha(isSel ? 200 : 140);
                painter->fillRect(barX, barY, fillW, barH, fc);
            } else if (isDyn) {
                for (int dx = 0; dx < kBarW; dx += 4)
                    painter->fillRect(barX + dx, barY, 2, barH, t.border);
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
                   const QModelIndex& /*index*/) const override {
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
                    QString tip = QStringLiteral("%1 (0x%2 bytes, %3 fields)\n")
                        .arg(e.displayName, QString::number(e.sizeBytes, 16).toUpper())
                        .arg(e.fieldCount);
                    tip += e.fieldSummary.join(QChar('\n'));
                    if (e.fieldCount > e.fieldSummary.size())
                        tip += QStringLiteral("\n...");
                    QToolTip::showText(event->globalPos(), tip, view);
                    return true;
                }
            }
            QToolTip::hideText();
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
        m_filterEdit->setPlaceholderText(QStringLiteral("Filter types..  (Ctrl+F)"));
        m_filterEdit->setClearButtonEnabled(true);
        m_filterEdit->setPalette(pal);
        m_filterEdit->setStyleSheet(QStringLiteral(
            "QLineEdit { border: 1px solid %1; padding: 2px 4px; border-radius: 3px; }")
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
            for (auto* c : m_groupChips) c->setChecked(true);
        });
        connect(noneBtn, &QToolButton::clicked, this, [this, refilter]() {
            // Keep at least one group on
            bool first = true;
            for (auto it = m_groupChips.begin(); it != m_groupChips.end(); ++it) {
                it.value()->setChecked(first);
                first = false;
            }
        });
        chipLayout->addWidget(allBtn);
        chipLayout->addWidget(noneBtn);

        m_statusLabel = new QLabel;
        m_statusLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; padding: 1px 4px; }").arg(theme.textDim.name()));
        chipLayout->addWidget(m_statusLabel);

        layout->addWidget(m_chipRow);
    }

    // ── Sort toolbar ──
    {
        auto* sortRow = new QWidget;
        sortRow->setFixedHeight(22);
        auto* slay = new QHBoxLayout(sortRow);
        slay->setContentsMargins(0, 0, 0, 0);
        slay->setSpacing(0);

        struct SortDef { const char* label; SortMode mode; };
        static const SortDef defs[] = {
            {"group", SortGroup}, {"name", SortName},
            {"size", SortSize}
        };
        for (const auto& d : defs) {
            auto* btn = new QToolButton;
            btn->setText(QString::fromLatin1(d.label));
            btn->setCheckable(true);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setFixedHeight(22);
            btn->setStyleSheet(QStringLiteral(
                "QToolButton { color: %1; background: transparent; border: none;"
                " border-right: 1px solid %2; padding: 0 8px; }"
                "QToolButton:checked { color: %3; background: %4; }"
                "QToolButton:hover:!checked { color: %5; background: %6; }")
                .arg(theme.textMuted.name(), theme.border.name(),
                     theme.syntaxKeyword.name(), theme.selected.name(),
                     theme.text.name(), theme.hover.name()));
            if (d.mode == SortGroup) btn->setChecked(true);
            SortMode sm = d.mode;
            connect(btn, &QToolButton::clicked, this, [this, btn, sm]() {
                if (m_sortMode == sm) {
                    m_sortDir *= -1;
                } else {
                    m_sortMode = sm;
                    m_sortDir = 1;
                }
                // Update button labels
                for (auto* b : m_sortBtns) b->setChecked(false);
                btn->setChecked(true);
                // Show direction arrow on active sort
                static const char* labels[] = {"group", "name", "size"};
                for (int i = 0; i < m_sortBtns.size(); i++) {
                    if (m_sortBtns[i] == btn)
                        m_sortBtns[i]->setText(
                            QString::fromLatin1(labels[i])
                            + (m_sortDir == 1 ? QStringLiteral(" \u2191")
                                              : QStringLiteral(" \u2193")));
                    else
                        m_sortBtns[i]->setText(QString::fromLatin1(labels[i]));
                }
                applyFilter(m_filterEdit->text());
            });
            slay->addWidget(btn);
            m_sortBtns.append(btn);
        }
        slay->addStretch();

        // Density toggle: normal / compact
        QString vbtnStyle = QStringLiteral(
            "QToolButton { color: %1; border: none; border-left: 1px solid %2;"
            " padding: 2px; width: 22px; height: 22px; }"
            "QToolButton:checked { color: %3; }"
            "QToolButton:hover { color: %4; background: %5; }")
            .arg(theme.textMuted.name(), theme.border.name(),
                 theme.syntaxKeyword.name(), theme.text.name(), theme.hover.name());
        auto* normBtn = new QToolButton;
        normBtn->setCheckable(true);
        normBtn->setChecked(true);
        normBtn->setText(QStringLiteral("\u2261")); // ≡
        normBtn->setToolTip(QStringLiteral("Normal density"));
        normBtn->setStyleSheet(vbtnStyle);
        auto* compBtn = new QToolButton;
        compBtn->setCheckable(true);
        compBtn->setText(QStringLiteral("\u2630")); // ☰
        compBtn->setToolTip(QStringLiteral("Compact density"));
        compBtn->setStyleSheet(vbtnStyle);
        slay->addWidget(normBtn);
        slay->addWidget(compBtn);
        connect(normBtn, &QToolButton::clicked, this, [this, normBtn, compBtn]() {
            m_compact = false;
            normBtn->setChecked(true); compBtn->setChecked(false);
            auto* d = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
            if (d) { d->setCompact(false); }
            m_listView->doItemsLayout();
        });
        connect(compBtn, &QToolButton::clicked, this, [this, normBtn, compBtn]() {
            m_compact = true;
            compBtn->setChecked(true); normBtn->setChecked(false);
            auto* d = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
            if (d) { d->setCompact(true); }
            m_listView->doItemsLayout();
        });

        // Detail pane toggle
        m_detailBtn = new QToolButton;
        m_detailBtn->setCheckable(true);
        m_detailBtn->setChecked(false);
        m_detailBtn->setText(QStringLiteral("\u25E8")); // ◨
        m_detailBtn->setToolTip(QStringLiteral("Toggle detail pane"));
        m_detailBtn->setStyleSheet(vbtnStyle);
        slay->addWidget(m_detailBtn);
        connect(m_detailBtn, &QToolButton::clicked, this, [this]() {
            m_showDetail = m_detailBtn->isChecked();
            if (m_detailPane) m_detailPane->setVisible(m_showDetail);
            if (m_showDetail) updateDetailPane();
        });

        layout->addWidget(sortRow);
    }

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
                // Clear modifier and accept
                for (auto* b : m_modGroup->buttons()) b->setChecked(false);
                m_arrayCountEdit->hide();
                acceptCurrent();
            } else if (link == QStringLiteral("action:ptr")) {
                // Set pointer modifier and accept
                for (auto* b : m_modGroup->buttons()) b->setChecked(false);
                m_btnPtr->setChecked(true);
                m_arrayCountEdit->hide();
                acceptCurrent();
            } else if (link == QStringLiteral("action:arr")) {
                // Set array modifier and accept
                for (auto* b : m_modGroup->buttons()) b->setChecked(false);
                m_btnArray->setChecked(true);
                if (m_arrayCountEdit->text().trimmed().isEmpty())
                    m_arrayCountEdit->setText(QStringLiteral("1"));
                m_arrayCountEdit->show();
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
        connect(m_listView->selectionModel(), &QItemSelectionModel::currentChanged,
                this, [this]() { updateModifierPreview(); updateDetailPane(); });
    }

    // ── Action row: "Apply as: ..." + modifiers + "+ New" ──
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

        // Modifier toggles: [*] [**] [[] count]
        {
            m_modRow = new QWidget;
            auto* modLayout = new QHBoxLayout(m_modRow);
            modLayout->setContentsMargins(0, 0, 0, 0);
            modLayout->setSpacing(3);

            m_modGroup = new QButtonGroup(this);
            m_modGroup->setExclusive(false);

            QString btnStyle = QStringLiteral(
                "QToolButton { color: %1; background: %2; border: 1px solid %3;"
                "  padding: 2px 8px; border-radius: 3px; }"
                "QToolButton:checked { color: %4; background: %5; border-color: %5; }"
                "QToolButton:hover:!checked { background: %6; }"
                "QToolButton:pressed { background: %7; }")
                .arg(theme.textDim.name(), theme.background.name(), theme.border.name(),
                     theme.text.name(), theme.selected.name(), theme.hover.name(),
                     theme.surface.name());

            auto makeToggle = [&](const QString& label, int id) -> QToolButton* {
                auto* btn = new QToolButton;
                btn->setText(label);
                btn->setCheckable(true);
                btn->setCursor(Qt::PointingHandCursor);
                btn->setStyleSheet(btnStyle);
                m_modGroup->addButton(btn, id);
                modLayout->addWidget(btn);
                return btn;
            };

            m_btnPtr    = makeToggle(QStringLiteral("*"),  1);
            m_btnDblPtr = makeToggle(QStringLiteral("**"), 2);
            m_btnArray  = makeToggle(QStringLiteral("[]"), 3);

            m_arrayCountEdit = new QLineEdit;
            m_arrayCountEdit->setPlaceholderText(QStringLiteral("n"));
            m_arrayCountEdit->setValidator(new QIntValidator(1, 99999, m_arrayCountEdit));
            m_arrayCountEdit->setFixedWidth(50);
            m_arrayCountEdit->setPalette(pal);
            m_arrayCountEdit->hide();
            modLayout->addWidget(m_arrayCountEdit);

            row->addWidget(m_modRow);

            connect(m_modGroup, &QButtonGroup::idClicked,
                    this, [this](int id) {
                QAbstractButton* btn = m_modGroup->button(id);
                if (btn->isChecked()) {
                    for (auto* b : m_modGroup->buttons())
                        if (b != btn) b->setChecked(false);
                }
                // If unchecked (toggled off), all buttons stay unchecked = plain
                m_arrayCountEdit->setVisible(m_btnArray->isChecked());
                if (m_btnArray->isChecked()) {
                    if (m_arrayCountEdit->text().trimmed().isEmpty())
                        m_arrayCountEdit->setText(QStringLiteral("1"));
                    m_arrayCountEdit->setFocus();
                    m_arrayCountEdit->selectAll();
                }
                updateModifierPreview();
            });
            connect(m_arrayCountEdit, &QLineEdit::textChanged,
                    this, [this]() { updateModifierPreview(); });
        }

        m_createBtn = new QToolButton;
        m_createBtn->setText(QStringLiteral("+ New"));
        m_createBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_createBtn->setAutoRaise(true);
        m_createBtn->setCursor(Qt::PointingHandCursor);
        m_createBtn->setAccessibleName(QStringLiteral("Create new type"));
        m_createBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; background: %2; border: 1px solid %3;"
            "  padding: 2px 10px; border-radius: 3px; }"
            "QToolButton:hover { color: %4; background: %5; border-color: %5; }"
            "QToolButton:pressed { background: %6; }")
            .arg(theme.text.name(), theme.background.name(), theme.border.name(),
                 theme.text.name(), theme.selected.name(), theme.surface.name()));
        connect(m_createBtn, &QToolButton::clicked, this, [this]() {
            int modId = m_modGroup ? m_modGroup->checkedId() : -1;
            if (modId < 0) modId = 0;  // -1 (no button checked) → 0 (plain)
            int arrCount = 0;
            if (modId == 3 && m_arrayCountEdit)
                arrCount = m_arrayCountEdit->text().trimmed().toInt();
            emit createNewTypeRequested(modId, arrCount);
            hide();
        });
        row->addWidget(m_createBtn);

        m_saveBtn = new QToolButton;
        m_saveBtn->setText(QStringLiteral("OK"));
        m_saveBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_saveBtn->setAutoRaise(true);
        m_saveBtn->setCursor(Qt::PointingHandCursor);
        m_saveBtn->setAccessibleName(QStringLiteral("OK"));
        m_saveBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; background: %2; border: 1px solid %3;"
            "  padding: 2px 14px; border-radius: 3px; }"
            "QToolButton:hover { background: %4; }"
            "QToolButton:pressed { background: %5; }")
            .arg(theme.text.name(), theme.selection.name(), theme.syntaxKeyword.name(),
                 theme.selection.lighter(120).name(), theme.selection.darker(110).name()));
        connect(m_saveBtn, &QToolButton::clicked, this, [this]() {
            acceptCurrent();
        });
        row->addWidget(m_saveBtn);

        layout->addLayout(row);
    }

    // ── Footer crumb: shows selected type info ──
    {
        m_footerLabel = new QLabel;
        m_footerLabel->setTextFormat(Qt::RichText);
        m_footerLabel->setFixedHeight(20);
        QFont footerFont = m_footerLabel->font();
        footerFont.setPointSize(qMax(7, footerFont.pointSize() - 2));
        m_footerLabel->setFont(footerFont);
        m_footerLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; padding: 0 3px; border-top: 1px solid %2; }")
            .arg(theme.textFaint.name(), theme.border.name()));
        layout->addWidget(m_footerLabel);
    }

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

void TypeSelectorPopup::popupLoading(const QPoint& globalPos) {
    m_loading = true;
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

    // Default popup size
    QFontMetrics fm(m_font);
    int rowH = fm.height() + 6;
    int popupW = 540;
    int popupH = qMax(400, rowH * 14 + rowH * 2 + 20);

    QScreen* screen = QApplication::screenAt(globalPos);
    if (screen) {
        QRect avail = screen->availableGeometry();
        if (globalPos.y() + popupH > avail.bottom())
            popupH = avail.bottom() - globalPos.y();
        if (globalPos.x() + popupW > avail.right())
            popupW = avail.right() - globalPos.x();
    }

    setFixedSize(popupW, popupH);
    move(globalPos);
    show();
    raise();
    activateWindow();
    m_filterEdit->setFocus();
}

void TypeSelectorPopup::setFont(const QFont& font) {
    m_font = font;

    {
        QFont tf = font;
        tf.setPointSize(qMax(7, font.pointSize() - 1));
        m_titleLabel->setFont(tf);
    }
    m_escLabel->setFont(font);
    m_filterEdit->setFont(font);
    m_listView->setFont(font);

    QFont smallFont = font;
    smallFont.setPointSize(qMax(7, font.pointSize() - 1));
    m_btnPtr->setFont(smallFont);
    m_btnDblPtr->setFont(smallFont);
    m_btnArray->setFont(smallFont);
    m_arrayCountEdit->setFont(smallFont);

    m_createBtn->setFont(smallFont);
    m_saveBtn->setFont(smallFont);

    QFont chipFont = font;
    chipFont.setPointSize(qMax(7, (int)(font.pointSize() * 0.75)));
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

    // Create button
    m_createBtn->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; background: %2; border: 1px solid %3;"
        "  padding: 2px 10px; border-radius: 3px; }"
        "QToolButton:hover { color: %4; background: %5; border-color: %5; }"
        "QToolButton:pressed { background: %6; }")
        .arg(theme.text.name(), theme.background.name(), theme.border.name(),
             theme.text.name(), theme.selected.name(), theme.surface.name()));

    // OK button (primary accent)
    m_saveBtn->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; background: %2; border: 1px solid %3;"
        "  padding: 2px 14px; border-radius: 3px; }"
        "QToolButton:hover { background: %4; }"
        "QToolButton:pressed { background: %5; }")
        .arg(theme.text.name(), theme.selection.name(), theme.syntaxKeyword.name(),
             theme.selection.lighter(120).name(), theme.selection.darker(110).name()));

    // Filter (no focus accent)
    m_filterEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { border: 1px solid %1; padding: 2px 4px; border-radius: 3px; }")
        .arg(theme.border.name()));

    // Modifier toggle buttons
    QString btnStyle = QStringLiteral(
        "QToolButton { color: %1; background: %2; border: 1px solid %3;"
        "  padding: 2px 8px; border-radius: 3px; }"
        "QToolButton:checked { color: %4; background: %5; border-color: %5; }"
        "QToolButton:hover:!checked { background: %6; }"
        "QToolButton:pressed { background: %7; }")
        .arg(theme.textDim.name(), theme.background.name(), theme.border.name(),
             theme.text.name(), theme.selected.name(), theme.hover.name(),
             theme.surface.name());
    m_btnPtr->setStyleSheet(btnStyle);
    m_btnDblPtr->setStyleSheet(btnStyle);
    m_btnArray->setStyleSheet(btnStyle);

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

void TypeSelectorPopup::setTitle(const QString& /*title*/) {
    // Title is now dynamic — set by updateModifierPreview()
}

void TypeSelectorPopup::setMode(TypePopupMode mode) {
    m_mode = mode;
    bool showMods = (mode == TypePopupMode::FieldType
                     || mode == TypePopupMode::ArrayElement);
    m_modRow->setVisible(showMods);
    // Reset all modifiers — no modifier = plain
    for (auto* b : m_modGroup->buttons())
        b->setChecked(false);
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
    for (auto* b : m_modGroup->buttons())
        b->setChecked(false);
    if (modId == 1)      m_btnPtr->setChecked(true);
    else if (modId == 2) m_btnDblPtr->setChecked(true);
    else if (modId == 3) {
        m_btnArray->setChecked(true);
        m_arrayCountEdit->setText(QString::number(arrayCount));
        m_arrayCountEdit->show();
    }
    // else: all unchecked = plain (no modifier)
}

void TypeSelectorPopup::setTypes(const QVector<TypeEntry>& types, const TypeEntry* current) {
    m_loading = false;
    auto* delegate = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
    if (delegate) delegate->setLoading(false);

    m_allTypes = types;
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
    default:                            placeholder = QStringLiteral("Filter %1 types..."); break;
    }
    m_filterEdit->setPlaceholderText(placeholder.arg(typeCount));

    m_filterEdit->clear();
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

void TypeSelectorPopup::popup(const QPoint& globalPos) {
    QFontMetrics fm(m_font);
    constexpr int kMaxPopupW = 540;
    int iconColW = fm.height() + 6;
    int estMaxW = iconColW + fm.horizontalAdvance(QChar('W')) * m_cachedMaxNameLen + 16;
    int maxTextW = qMax(fm.horizontalAdvance(QStringLiteral("Choose element type        ")), estMaxW);
    int popupW = qBound(460, maxTextW + 24, kMaxPopupW);
    int rowH = fm.height() + 6;
    int headerH = rowH * 2 + 10;
    int footerH = rowH + 6;
    int listH = qBound(rowH * 3, rowH * (int)m_filteredTypes.size(), rowH * 16);
    int popupH = qMax(400, headerH + listH + footerH);

    QScreen* screen = QApplication::screenAt(globalPos);
    if (screen) {
        QRect avail = screen->availableGeometry();
        if (globalPos.y() + popupH > avail.bottom())
            popupH = avail.bottom() - globalPos.y();
        if (globalPos.x() + popupW > avail.right())
            popupW = avail.right() - globalPos.x();
    }

    setFixedSize(popupW, popupH);
    move(globalPos);
    show();
    raise();
    activateWindow();
    m_filterEdit->setFocus();
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
            .arg(t.textFaint.name(), entry.displayName, szText, entry.kindGroup));
    }

    int modId = m_modGroup->checkedId();

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
        .arg(t.text.name(), entry.displayName, suffix);

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
        || m_filteredTypes[row].entryKind == TypeEntry::Section) {
        m_detailContent->setText(QStringLiteral(
            "<div style='color:%1;padding:6px'>No selection</div>").arg(t.textFaint.name()));
        return;
    }

    const TypeEntry& entry = m_filteredTypes[row];
    const QColor gc = kindGroupColor(entry.kindGroup);
    const int modId = m_modGroup ? m_modGroup->checkedId() : -1;

    // Use the actual font point size — all HTML sizes are relative to this
    const int pt = m_font.pointSize();
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
        .arg(pt).arg(t.text.name(), entry.displayName)
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

        QString tn = entry.displayName;
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
            .arg(ptS).arg(t.text.name(), entry.displayName);

        bool canPtr = entry.entryKind == TypeEntry::Composite
            || isValidPrimitivePtrTarget(entry.primitiveKind);
        if (canPtr)
            html += QStringLiteral(
                "<div style='padding:3px 6px;margin-bottom:2px;background:%1;"
                "border:1px solid %2;font-size:%3pt'>"
                "<a href='action:ptr' style='color:%4;text-decoration:none'>"
                "* Pointer \u2192 %5</a></div>")
                .arg(t.surface.name(), t.border.name())
                .arg(ptS).arg(t.textDim.name(), entry.displayName);

        html += QStringLiteral(
            "<div style='padding:3px 6px;background:%1;"
            "border:1px solid %2;font-size:%3pt'>"
            "<a href='action:arr' style='color:%4;text-decoration:none'>"
            "[ ] Array of %5</a></div>")
            .arg(t.surface.name(), t.border.name())
            .arg(ptS).arg(t.textDim.name(), entry.displayName);
        html += QStringLiteral("</div>");
    }

    m_detailContent->setText(html);
}

void TypeSelectorPopup::applyFilter(const QString& text) {
    m_filteredTypes.clear();
    m_matchPositions.clear();
    QStringList displayStrings;

    QString filterBase = text.trimmed();

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

    QHash<QString, int> groupCounts;       // counts of matched/visible types per group
    QHash<QString, int> totalGroupCounts;  // counts of ALL types per group (for chip totals)
    const int totalTypes = m_allTypes.size();
    int totalNonSection = 0;

    // Pre-compute total counts per group (independent of filter)
    for (const auto& t : m_allTypes) {
        if (t.entryKind == TypeEntry::Section) continue;
        totalNonSection++;
        QString g = t.kindGroup;
        if (g.isEmpty()) {
            g = (t.entryKind == TypeEntry::Primitive)
                ? kindGroupFor(t.primitiveKind) : QStringLiteral("Ctr");
        }
        totalGroupCounts[g]++;
    }

    // Pre-reserve to avoid realloc churn
    m_filteredTypes.reserve(totalTypes);
    m_matchPositions.reserve(totalTypes);
    displayStrings.reserve(totalTypes);

    if (!filterBase.isEmpty()) {
        // ── Fuzzy search: flat ranked list, no section headers ──
        // Use index + score to avoid deep-copying TypeEntry structs
        struct Scored { int idx; int score; QVector<int> pos; };
        QVector<Scored> scored;
        scored.reserve(totalTypes);

        for (int i = 0; i < totalTypes; i++) {
            auto& t = m_allTypes[i];
            if (t.entryKind == TypeEntry::Section) continue;
            // Auto-assign kindGroup if missing
            if (t.kindGroup.isEmpty()) {
                if (t.entryKind == TypeEntry::Primitive)
                    t.kindGroup = kindGroupFor(t.primitiveKind);
                else
                    t.kindGroup = QStringLiteral("Ctr");
            }
            QVector<int> pos;
            int sc = fuzzyScore(filterBase, t.displayName, &pos);
            if (sc <= 0) continue;
            groupCounts[t.kindGroup]++;
            if (catAllowed(t))
                scored.push_back(Scored{i, sc, std::move(pos)});
        }
        std::sort(scored.begin(), scored.end(),
                  [](const Scored& a, const Scored& b) { return a.score > b.score; });

        for (const auto& s : scored) {
            m_filteredTypes.append(m_allTypes[s.idx]);
            m_matchPositions.append(s.pos);
            displayStrings << makeLabel(m_allTypes[s.idx]);
        }
    } else {
        // ── No filter: build list with current sort mode ──
        // Bucket by kindGroup, count all, filter by enabled groups
        static const char* kGroupOrder[] = {"Hex","Int","Float","Ptr","Vec","Str","Ctr","Common"};
        static const char* kGroupLabels[] = {"Hex","Int / Bool","Float","Pointer / FuncPtr","Vec / Mat","String","Type","Common Types"};
        QHash<QString, QVector<TypeEntry>> buckets;
        for (auto& t : m_allTypes) {
            if (t.entryKind == TypeEntry::Section) continue;
            // Auto-assign kindGroup if missing (tests / external callers)
            if (t.kindGroup.isEmpty()) {
                if (t.entryKind == TypeEntry::Primitive)
                    t.kindGroup = kindGroupFor(t.primitiveKind);
                else
                    t.kindGroup = QStringLiteral("Ctr");
            }
            groupCounts[t.kindGroup]++;
            if (catAllowed(t))
                buckets[t.kindGroup].append(t);
        }

        auto alphabetical = [](const TypeEntry& a, const TypeEntry& b) {
            return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
        };

        auto appendSection = [&](const QString& title, const QVector<TypeEntry>& items) {
            if (items.isEmpty()) return;
            TypeEntry sec;
            sec.entryKind = TypeEntry::Section;
            sec.displayName = title;
            sec.kindGroup = title.left(title.indexOf(' ')); // for section pip color
            sec.enabled = false;
            m_filteredTypes.append(sec);
            m_matchPositions.append(QVector<int>());
            displayStrings << sec.displayName;
            for (const auto& c : items) {
                m_filteredTypes.append(c);
                m_matchPositions.append(QVector<int>());
                displayStrings << makeLabel(c);
            }
        };

        if (m_sortMode == SortGroup) {
            // Group view: per-kindGroup sections
            for (int gi = 0; gi < 8; gi++) {
                QString g = QString::fromLatin1(kGroupOrder[gi]);
                auto& items = buckets[g];
                if (items.isEmpty()) continue;
                // Same-size-first sorting for the matching group
                if (m_mode != TypePopupMode::Root && m_currentNodeSize > 0) {
                    QVector<TypeEntry> sameSize, other;
                    for (const auto& p : items) {
                        if (p.sizeBytes == m_currentNodeSize) sameSize.append(p);
                        else other.append(p);
                    }
                    std::sort(sameSize.begin(), sameSize.end(), alphabetical);
                    std::sort(other.begin(), other.end(), alphabetical);
                    items = sameSize + other;
                } else {
                    std::sort(items.begin(), items.end(), alphabetical);
                }
                appendSection(QString::fromLatin1(kGroupLabels[gi]), items);
            }
        } else {
            // Flat sorted list (no sections) — name/size/align
            QVector<TypeEntry> all;
            for (auto& items : buckets) all += items;
            int dir = m_sortDir;
            switch (m_sortMode) {
            case SortName:
                std::sort(all.begin(), all.end(), [dir](const TypeEntry& a, const TypeEntry& b) {
                    return dir * a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
                });
                break;
            case SortSize:
                std::sort(all.begin(), all.end(), [dir](const TypeEntry& a, const TypeEntry& b) {
                    if (a.sizeBytes != b.sizeBytes) return dir * (a.sizeBytes - b.sizeBytes) < 0;
                    return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
                });
                break;
            case SortAlign:
                std::sort(all.begin(), all.end(), [dir](const TypeEntry& a, const TypeEntry& b) {
                    if (a.alignment != b.alignment) return dir * (a.alignment - b.alignment) < 0;
                    return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
                });
                break;
            default: break;
            }
            for (const auto& c : all) {
                m_filteredTypes.append(c);
                m_matchPositions.append(QVector<int>());
                displayStrings << makeLabel(c);
            }
        }
    }

    // Empty state
    int resultCount = 0;
    for (const auto& f : m_filteredTypes)
        if (f.entryKind != TypeEntry::Section) resultCount++;

    if (resultCount == 0) {
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

    auto* delegate = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
    if (delegate) {
        delegate->setFilteredTypes(&m_filteredTypes);
        delegate->setMatchPositions(&m_matchPositions);
    }

    int first = nextSelectableRow(0, 1);
    if (first >= 0)
        m_listView->setCurrentIndex(m_model->index(first));
}

void TypeSelectorPopup::acceptCurrent() {
    QModelIndex idx = m_listView->currentIndex();
    if (idx.isValid())
        acceptIndex(idx.row());
}

void TypeSelectorPopup::acceptIndex(int row) {
    if (row < 0 || row >= m_filteredTypes.size()) return;
    const TypeEntry& entry = m_filteredTypes[row];
    if (entry.entryKind == TypeEntry::Section) return;
    if (!entry.enabled) return;

    // Build full text with modifier from toggle buttons
    int modId = m_modGroup->checkedId();
    QString fullText = entry.displayName;
    if (modId == 1)
        fullText += QStringLiteral("*");
    else if (modId == 2)
        fullText += QStringLiteral("**");
    else if (modId == 3) {
        QString countText = m_arrayCountEdit->text().trimmed();
        if (!countText.isEmpty())
            fullText += QStringLiteral("[%1]").arg(countText);
    }

    emit typeSelected(entry, fullText);
    hide();
}

int TypeSelectorPopup::nextSelectableRow(int from, int direction) const {
    int i = from;
    while (i >= 0 && i < m_filteredTypes.size()) {
        const auto& e = m_filteredTypes[i];
        if (e.entryKind != TypeEntry::Section && e.enabled)
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

        // Ctrl+F focuses the filter from anywhere
        if (ke->key() == Qt::Key_F && (ke->modifiers() & Qt::ControlModifier)) {
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
    emit dismissed();
}

} // namespace rcx
