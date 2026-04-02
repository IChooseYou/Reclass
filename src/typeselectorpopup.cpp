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

    void setCount(int n) { m_count = n; update(); }

    QSize sizeHint() const override {
        QFontMetrics fm(font());
        QString text = m_count >= 0
            ? QStringLiteral("%1 (%2)").arg(m_label).arg(m_count)
            : m_label;
        int checkW = fm.height();  // space for checkmark
        return QSize(checkW + 4 + fm.horizontalAdvance(text) + 12, fm.height() + 6);
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const auto& t = ThemeManager::instance().current();
        QFontMetrics fm(font());
        bool hov = underMouse();
        bool chk = isChecked();

        // Background
        QColor bg;
        if (isDown())
            bg = t.surface;
        else if (chk)
            bg = t.selected;
        else if (hov)
            bg = t.hover;
        else
            bg = t.background;
        p.fillRect(rect(), bg);

        int x = 4;
        int cy = height() / 2;
        int boxSz = fm.height() - 2;
        int boxY = cy - boxSz / 2;

        // Checkbox square
        p.setPen(chk ? t.indHoverSpan : t.textDim);
        p.drawRect(x, boxY, boxSz - 1, boxSz - 1);

        // Checkmark when checked
        if (chk) {
            p.setPen(QPen(t.indHoverSpan, 1.5));
            p.setRenderHint(QPainter::Antialiasing, true);
            int cx = x + boxSz / 2;
            int my = boxY + boxSz / 2;
            // Simple checkmark: down-right then up-right
            p.drawLine(x + 2, my, cx - 1, boxY + boxSz - 3);
            p.drawLine(cx - 1, boxY + boxSz - 3, x + boxSz - 3, boxY + 2);
            p.setRenderHint(QPainter::Antialiasing, false);
        }

        x += boxSz + 4;

        // Text
        QString text = m_count >= 0
            ? QStringLiteral("%1 (%2)").arg(m_label).arg(m_count)
            : m_label;
        p.setPen(chk ? t.text : t.textDim);
        p.setFont(font());
        p.drawText(x, 0, width() - x - 4, height(), Qt::AlignVCenter | Qt::AlignLeft, text);
    }

    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    QString m_label;
    int m_count = -1;
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

// ── Custom delegate: gutter checkmark + icon + text + sections ──

class TypeSelectorDelegate : public QStyledItemDelegate {
public:
    explicit TypeSelectorDelegate(TypeSelectorPopup* popup, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_popup(popup) {}

    void setFont(const QFont& f) { m_font = f; updateCachedSizeHint(); }
    void setLoading(bool v) { m_isLoading = v; }
    void setFilteredTypes(const QVector<TypeEntry>* filtered) {
        m_filtered = filtered;
    }
    void setMatchPositions(const QVector<QVector<int>>* mp) {
        m_matchPositions = mp;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();

        const auto& t = ThemeManager::instance().current();
        int row = index.row();

        // Skeleton placeholder bars while loading
        if (m_isLoading) {
            QFontMetrics fm(m_font);
            int barH = fm.height() - 2;
            int x0 = option.rect.x() + fm.height() + 8;
            int y0 = option.rect.y() + (option.rect.height() - barH) / 2;
            int barW = 50 + ((row * 73 + 29) % 110);
            painter->setPen(Qt::NoPen);
            painter->setBrush(t.surface);
            painter->drawRoundedRect(x0, y0, barW, barH, 3, 3);
            // Size column placeholder
            painter->drawRoundedRect(option.rect.right() - 46, y0, 30, barH, 3, 3);
            painter->restore();
            return;
        }

        bool isSection = (m_filtered && row >= 0 && row < m_filtered->size()
                          && (*m_filtered)[row].entryKind == TypeEntry::Section);
        bool isDisabled = (m_filtered && row >= 0 && row < m_filtered->size()
                           && !(*m_filtered)[row].enabled);

        // Background
        if (isSection) {
            // No background highlight for sections
        } else if (isDisabled) {
            // Subtle background on hover only
            if (option.state & QStyle::State_MouseOver)
                painter->fillRect(option.rect, t.surface);
        } else {
            if (option.state & QStyle::State_Selected)
                painter->fillRect(option.rect, t.selected);
            else if (option.state & QStyle::State_MouseOver)
                painter->fillRect(option.rect, t.hover);
        }

        const int leftPad = 6;
        int x = option.rect.x() + leftPad;
        int y = option.rect.y();
        int h = option.rect.height();
        int w = option.rect.width() - leftPad;

        // Scale metrics from font height
        QFontMetrics fmMain(m_font);
        int iconSz = fmMain.height();            // icon matches text height
        int iconColW = iconSz + 4;

        // Section: centered dim text with horizontal rules
        if (isSection) {
            painter->setPen(t.textDim);
            QFont dimFont = m_font;
            dimFont.setPointSize(qMax(7, m_font.pointSize() - 1));
            painter->setFont(dimFont);
            QFontMetrics fm(dimFont);
            QString text = index.data().toString();
            int textW = fm.horizontalAdvance(text);
            int textX = x + (w - textW) / 2;
            int lineY = y + h / 2;

            // Left rule
            if (textX > x + 8)
                painter->drawLine(x + 8, lineY, textX - 6, lineY);
            // Text
            painter->drawText(QRect(textX, y, textW, h), Qt::AlignVCenter, text);
            // Right rule
            if (textX + textW + 6 < x + w - 8)
                painter->drawLine(textX + textW + 6, lineY, x + w - 8, lineY);

            painter->restore();
            return;
        }

        // Icon (scaled to font height)
        bool hasIcon = (m_filtered && row >= 0 && row < m_filtered->size()
                        && (*m_filtered)[row].entryKind != TypeEntry::Section);
        if (hasIcon) {
            static QIcon structIcon(QStringLiteral(":/vsicons/symbol-class.svg"));
            static QIcon enumIcon(QStringLiteral(":/vsicons/symbol-enum.svg"));
            static QIcon primIcon(QStringLiteral(":/vsicons/symbol-variable.svg"));
            const auto& entry = (*m_filtered)[row];
            QIcon& icon = (entry.entryKind == TypeEntry::Composite)
                ? (entry.category == TypeEntry::CatEnum ? enumIcon : structIcon)
                : primIcon;
            QPixmap pm = icon.pixmap(iconSz, iconSz);
            if (isDisabled) {
                QPixmap dimmed(pm.size());
                dimmed.fill(Qt::transparent);
                QPainter p(&dimmed);
                p.setOpacity(0.35);
                p.drawPixmap(0, 0, pm);
                p.end();
                painter->drawPixmap(x, y + (h - iconSz) / 2, dimmed);
            } else {
                icon.paint(painter, x, y + (h - iconSz) / 2, iconSz, iconSz);
            }
        }
        x += iconColW;

        // Text: type name in normal color, size suffix dimmed
        QColor textColor;
        if (isDisabled)
            textColor = t.textDim;
        else if (option.state & QStyle::State_Selected)
            textColor = option.palette.color(QPalette::HighlightedText);
        else
            textColor = option.palette.color(QPalette::Text);

        painter->setFont(m_font);
        QString fullText = index.data().toString();
        int dashIdx = fullText.lastIndexOf(QStringLiteral(" - "));
        int rightPad = 6;

        // Fuzzy-match highlight flags for the name portion
        QVector<bool> hlFlags;
        if (m_matchPositions && row >= 0 && row < m_matchPositions->size()
            && !(*m_matchPositions)[row].isEmpty()) {
            int nameLen = dashIdx > 0 ? dashIdx : fullText.size();
            hlFlags.resize(nameLen, false);
            for (int p : (*m_matchPositions)[row])
                if (p < nameLen) hlFlags[p] = true;
        }

        // Lambda: draw text with optional highlight runs
        int sizeColW = 55;
        auto drawHL = [&](const QString& text, int x0, int maxW) {
            if (hlFlags.isEmpty()) {
                painter->setPen(textColor);
                painter->drawText(QRect(x0, y, maxW, h),
                                  Qt::AlignVCenter | Qt::AlignLeft, text);
                return;
            }
            QFontMetrics fm(m_font);
            int xp = x0;
            int i = 0;
            while (i < text.size()) {
                bool hl = i < hlFlags.size() && hlFlags[i];
                int s = i;
                while (i < text.size() && (i < hlFlags.size() && hlFlags[i]) == hl) i++;
                QString seg = text.mid(s, i - s);
                int segW = fm.horizontalAdvance(seg);
                painter->setPen(hl ? t.indHoverSpan : textColor);
                painter->drawText(QRect(xp, y, segW, h), Qt::AlignVCenter, seg);
                xp += segW;
            }
        };

        if (dashIdx > 0) {
            int nameW = option.rect.right() - x - sizeColW - rightPad;
            drawHL(fullText.left(dashIdx), x, nameW);
            painter->setPen(t.textDim);
            painter->drawText(QRect(option.rect.right() - sizeColW - rightPad, y, sizeColW, h),
                              Qt::AlignVCenter | Qt::AlignRight, fullText.mid(dashIdx + 3));
        } else {
            drawHL(fullText, x, option.rect.right() - x - rightPad);
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& /*option*/,
                   const QModelIndex& /*index*/) const override {
        return m_cachedSizeHint;
    }

    void updateCachedSizeHint() {
        QFontMetrics fm(m_font);
        m_cachedSizeHint = QSize(200, fm.height() + 8);
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
    QSize m_cachedSizeHint{200, 20};
    bool m_isLoading = false;
    const QVector<TypeEntry>* m_filtered = nullptr;
    const QVector<QVector<int>>* m_matchPositions = nullptr;
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
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);

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

    // ── Category chips ──
    {
        m_chipRow = new QWidget;
        auto* chipLayout = new QHBoxLayout(m_chipRow);
        chipLayout->setContentsMargins(0, 0, 0, 2);
        chipLayout->setSpacing(6);

        auto makeChip = [&](const QString& label) -> CategoryChip* {
            auto* btn = new CategoryChip(label);
            chipLayout->addWidget(btn);
            return btn;
        };

        m_chipPrim  = makeChip(QStringLiteral("Built-in"));
        m_chipTypes = makeChip(QStringLiteral("Types"));
        m_chipEnums = makeChip(QStringLiteral("Enum"));
        m_chipPrim->setAccessibleName(QStringLiteral("Show primitives"));
        m_chipTypes->setAccessibleName(QStringLiteral("Show composites"));
        m_chipEnums->setAccessibleName(QStringLiteral("Show enums"));
        chipLayout->addStretch();

        m_statusLabel = new QLabel;
        m_statusLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; padding: 1px 4px; }").arg(theme.textDim.name()));
        chipLayout->addWidget(m_statusLabel);

        layout->addWidget(m_chipRow);

        auto refilter = [this]() { applyFilter(m_filterEdit->text()); };
        connect(m_chipPrim,  &QToolButton::toggled, this, refilter);
        connect(m_chipTypes, &QToolButton::toggled, this, refilter);
        connect(m_chipEnums, &QToolButton::toggled, this, refilter);
    }

    // ── List view (main content) ──
    {
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
        m_listView->setUniformItemSizes(true);
        m_listView->setLayoutMode(QListView::Batched);
        m_listView->setBatchSize(50);
        m_listView->installEventFilter(this);

        auto* delegate = new TypeSelectorDelegate(this, m_listView);
        m_listView->setItemDelegate(delegate);

        layout->addWidget(m_listView, 1);

        connect(m_listView, &QListView::doubleClicked,
                this, [this](const QModelIndex& index) {
            acceptIndex(index.row());
        });
        connect(m_listView->selectionModel(), &QItemSelectionModel::currentChanged,
                this, [this]() { updateModifierPreview(); });
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
        QFont bold = m_titleLabel->font();
        bold.setBold(true);
        m_titleLabel->setFont(bold);
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
        m_saveBtn->setText(QStringLiteral("Save"));
        m_saveBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_saveBtn->setAutoRaise(true);
        m_saveBtn->setCursor(Qt::PointingHandCursor);
        m_saveBtn->setAccessibleName(QStringLiteral("Save"));
        m_saveBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; background: %2; border: 1px solid %3;"
            "  padding: 2px 10px; border-radius: 3px; }"
            "QToolButton:hover { color: %4; background: %5; border-color: %5; }"
            "QToolButton:pressed { background: %6; }")
            .arg(theme.text.name(), theme.background.name(), theme.border.name(),
                 theme.text.name(), theme.selected.name(), theme.surface.name()));
        connect(m_saveBtn, &QToolButton::clicked, this, [this]() {
            acceptCurrent();
        });
        row->addWidget(m_saveBtn);

        layout->addLayout(row);
    }

}

void TypeSelectorPopup::warmUp() {
    // One-time per-process cost (~170ms): Qt lazily initializes the style/font/DLL
    // subsystem the first time a popup with complex children is shown. Pre-pay it
    // by briefly showing a throwaway dummy popup with a QListView, then show+hide
    // ourselves.
    {
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

    // Default popup size (compact — 66% of old width)
    QFontMetrics fm(m_font);
    int rowH = fm.height() + 8;
    int popupW = 560;
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

    m_titleLabel->setFont([&]() {
        QFont f = font;
        f.setPointSize(qMax(7, font.pointSize() * 3 / 4));
        f.setBold(true);
        return f;
    }());
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
    m_chipPrim->setFont(chipFont);
    m_chipTypes->setFont(chipFont);
    m_chipEnums->setFont(chipFont);
    if (m_statusLabel) m_statusLabel->setFont(chipFont);

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

    // Save button
    m_saveBtn->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; background: %2; border: 1px solid %3;"
        "  padding: 2px 10px; border-radius: 3px; }"
        "QToolButton:hover { color: %4; background: %5; border-color: %5; }"
        "QToolButton:pressed { background: %6; }")
        .arg(theme.text.name(), theme.background.name(), theme.border.name(),
             theme.text.name(), theme.selected.name(), theme.surface.name()));

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

    // Category chips — custom painted, theme read at paint time, no stylesheet needed
    if (m_chipPrim) m_chipPrim->update();
    if (m_chipTypes) m_chipTypes->update();
    if (m_chipEnums) m_chipEnums->update();

    // Status label
    if (m_statusLabel) {
        m_statusLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; padding: 1px 2px; }").arg(theme.textDim.name()));
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
    // Don't reset modifier buttons here — setMode() already resets to plain,
    // and setModifier() may have preselected a button between setMode/setTypes.
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
    constexpr int kMaxPopupW = 560;
    // Estimate max width from cached max name length (avoids iterating all types)
    int iconColW = fm.height() + 4;
    int estMaxW = iconColW + fm.horizontalAdvance(QChar('W')) * m_cachedMaxNameLen + 16;
    int maxTextW = qMax(fm.horizontalAdvance(QStringLiteral("Choose element type        ")), estMaxW);
    int popupW = qBound(480, maxTextW + 24, kMaxPopupW);
    int rowH = fm.height() + 8;
    int headerH = rowH * 2 + 10;   // filter + chips + separator
    int footerH = rowH + 6;        // separator + action row
    int listH = qBound(rowH * 3, rowH * (int)m_filteredTypes.size(), rowH * 14);
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
        return;
    }

    const TypeEntry& entry = m_filteredTypes[row];

    // Disabled entry
    if (!entry.enabled) {
        m_titleLabel->setText(QStringLiteral("<span style='color:%1'>Not selectable</span>")
            .arg(t.textDim.name()));
        return;
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
        label += QStringLiteral("<span style='color:%1'> \u2192 %2</span>")
            .arg(t.textDim.name()).arg(newSize);

        if (m_currentNodeSize > 0 && newSize != m_currentNodeSize) {
            int diff = newSize - m_currentNodeSize;
            label += QStringLiteral("<span style='color:%1'> (%2%3)</span>")
                .arg(t.textDim.name(),
                     diff > 0 ? QStringLiteral("+") : QString(),
                     QString::number(diff));
        }
    }

    m_titleLabel->setText(label);
}

void TypeSelectorPopup::applyFilter(const QString& text) {
    m_filteredTypes.clear();
    m_matchPositions.clear();
    QStringList displayStrings;

    QString filterBase = text.trimmed();

    bool showPrim  = m_chipPrim  && m_chipPrim->isChecked();
    bool showTypes = m_chipTypes && m_chipTypes->isChecked();
    bool showEnums = m_chipEnums && m_chipEnums->isChecked();

    auto catAllowed = [&](const TypeEntry& t) {
        if (t.entryKind == TypeEntry::Primitive) return showPrim;
        return (t.category == TypeEntry::CatEnum) ? showEnums : showTypes;
    };

    auto makeLabel = [](const TypeEntry& e) {
        QString label = e.displayName;
        if (e.sizeBytes > 0) label += QStringLiteral(" - %1B").arg(e.sizeBytes);
        return label;
    };

    int primCount = 0, typeCount = 0, enumCount = 0;
    const int totalTypes = m_allTypes.size();

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
            const auto& t = m_allTypes[i];
            if (t.entryKind == TypeEntry::Section) continue;
            QVector<int> pos;
            int sc = fuzzyScore(filterBase, t.displayName, &pos);
            if (sc <= 0) continue;
            if (t.entryKind == TypeEntry::Primitive) primCount++;
            else if (t.category == TypeEntry::CatEnum) enumCount++;
            else typeCount++;
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
        // ── No filter: grouped sections, alphabetical ──
        QVector<TypeEntry> primitives, types, enums;
        for (const auto& t : m_allTypes) {
            if (t.entryKind == TypeEntry::Section) continue;
            if (t.entryKind == TypeEntry::Primitive) {
                primCount++;
                if (showPrim) primitives.append(t);
            } else if (t.category == TypeEntry::CatEnum) {
                enumCount++;
                if (showEnums) enums.append(t);
            } else {
                typeCount++;
                if (showTypes) types.append(t);
            }
        }

        auto alphabetical = [](const TypeEntry& a, const TypeEntry& b) {
            return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
        };
        if (m_mode != TypePopupMode::Root && m_currentNodeSize > 0 && !primitives.isEmpty()) {
            QVector<TypeEntry> sameSize, other;
            for (const auto& p : primitives) {
                if (sizeForKind(p.primitiveKind) == m_currentNodeSize) sameSize.append(p);
                else other.append(p);
            }
            std::sort(sameSize.begin(), sameSize.end(), alphabetical);
            std::sort(other.begin(), other.end(), alphabetical);
            primitives = sameSize + other;
        } else {
            std::sort(primitives.begin(), primitives.end(), alphabetical);
        }
        std::sort(types.begin(), types.end(), alphabetical);
        std::sort(enums.begin(), enums.end(), alphabetical);

        auto appendSection = [&](const QString& title, const QVector<TypeEntry>& items) {
            if (items.isEmpty()) return;
            TypeEntry sec;
            sec.entryKind = TypeEntry::Section;
            sec.displayName = title;
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

        if (m_mode == TypePopupMode::Root) {
            appendSection(QStringLiteral("types"), types);
            appendSection(QStringLiteral("enums"), enums);
            appendSection(QStringLiteral("primitives"), primitives);
        } else {
            appendSection(QStringLiteral("primitives"), primitives);
            appendSection(QStringLiteral("types"), types);
            appendSection(QStringLiteral("enums"), enums);
        }
    }

    // Empty state
    int resultCount = 0;
    for (const auto& f : m_filteredTypes)
        if (f.entryKind != TypeEntry::Section) resultCount++;

    if (resultCount == 0) {
        TypeEntry empty;
        empty.entryKind = TypeEntry::Section;
        empty.displayName = QStringLiteral("No matching types");
        empty.enabled = false;
        m_filteredTypes.append(empty);
        m_matchPositions.append(QVector<int>());
        displayStrings << empty.displayName;
    }

    m_model->setStringList(displayStrings);

    if (m_chipPrim)  static_cast<CategoryChip*>(m_chipPrim)->setCount(primCount);
    if (m_chipTypes) static_cast<CategoryChip*>(m_chipTypes)->setCount(typeCount);
    if (m_chipEnums) static_cast<CategoryChip*>(m_chipEnums)->setCount(enumCount);

    if (m_statusLabel)
        m_statusLabel->setText(QStringLiteral("%1 results").arg(resultCount));

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

void TypeSelectorPopup::hideEvent(QHideEvent* event) {
    QFrame::hideEvent(event);
    emit dismissed();
}

} // namespace rcx
