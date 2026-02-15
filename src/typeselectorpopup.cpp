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
#include "themes/thememanager.h"

namespace rcx {

// ── parseTypeSpec ──

TypeSpec parseTypeSpec(const QString& text) {
    TypeSpec spec;
    QString s = text.trimmed();
    if (s.isEmpty()) return spec;

    // Check for pointer suffix: "Ball*" or "Ball**"
    if (s.endsWith('*')) {
        spec.isPointer = true;
        s.chop(1);
        if (s.endsWith('*')) s.chop(1);  // double pointer
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

// ── Custom delegate: gutter checkmark + icon + text + sections ──

class TypeSelectorDelegate : public QStyledItemDelegate {
public:
    explicit TypeSelectorDelegate(TypeSelectorPopup* popup, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_popup(popup) {}

    void setFont(const QFont& f) { m_font = f; }
    void setFilteredTypes(const QVector<TypeEntry>* filtered, const TypeEntry* current, bool hasCurrent) {
        m_filtered = filtered;
        m_current = current;
        m_hasCurrent = hasCurrent;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();

        const auto& t = ThemeManager::instance().current();
        int row = index.row();
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

        int x = option.rect.x();
        int y = option.rect.y();
        int h = option.rect.height();
        int w = option.rect.width();

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

        // Gutter: side triangle if current
        if (m_hasCurrent && m_filtered && row >= 0 && row < m_filtered->size()) {
            const TypeEntry& entry = (*m_filtered)[row];
            bool isCurrent = false;
            if (m_current->entryKind == TypeEntry::Primitive && entry.entryKind == TypeEntry::Primitive)
                isCurrent = (entry.primitiveKind == m_current->primitiveKind);
            else if (m_current->entryKind == TypeEntry::Composite && entry.entryKind == TypeEntry::Composite)
                isCurrent = (entry.structId == m_current->structId);
            if (isCurrent) {
                painter->setPen(t.text);
                painter->setFont(m_font);
                painter->drawText(QRect(x, y, 10, h), Qt::AlignCenter,
                                  QString(QChar(0x25B8)));
            }
        }
        x += 10;

        // Icon 16x16 — only for composite entries
        bool hasIcon = (m_filtered && row >= 0 && row < m_filtered->size()
                        && (*m_filtered)[row].entryKind == TypeEntry::Composite);
        if (hasIcon) {
            static QIcon structIcon(QStringLiteral(":/vsicons/symbol-structure.svg"));
            QPixmap pm = structIcon.pixmap(16, 16);
            if (isDisabled) {
                // Paint dimmed
                QPixmap dimmed(pm.size());
                dimmed.fill(Qt::transparent);
                QPainter p(&dimmed);
                p.setOpacity(0.35);
                p.drawPixmap(0, 0, pm);
                p.end();
                painter->drawPixmap(x, y + (h - 16) / 2, dimmed);
            } else {
                structIcon.paint(painter, x, y + (h - 16) / 2, 16, 16);
            }
        }
        x += 20;

        // Text
        QColor textColor;
        if (isDisabled)
            textColor = t.textDim;
        else if (option.state & QStyle::State_Selected)
            textColor = option.palette.color(QPalette::HighlightedText);
        else
            textColor = option.palette.color(QPalette::Text);

        painter->setPen(textColor);
        painter->setFont(m_font);
        painter->drawText(QRect(x, y, option.rect.right() - x, h),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          index.data().toString());

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& /*option*/,
                   const QModelIndex& index) const override {
        QFontMetrics fm(m_font);
        int row = index.row();
        bool isSection = (m_filtered && row >= 0 && row < m_filtered->size()
                          && (*m_filtered)[row].entryKind == TypeEntry::Section);
        int h = isSection ? fm.height() + 2 : fm.height() + 8;
        return QSize(200, h);
    }

private:
    TypeSelectorPopup* m_popup = nullptr;
    QFont m_font;
    const QVector<TypeEntry>* m_filtered = nullptr;
    const TypeEntry* m_current = nullptr;
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
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    // Row 1: title + Esc hint
    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        m_titleLabel = new QLabel(QStringLiteral("Change type"));
        m_titleLabel->setPalette(pal);
        QFont bold = m_titleLabel->font();
        bold.setBold(true);
        m_titleLabel->setFont(bold);
        row->addWidget(m_titleLabel);

        row->addStretch();

        m_escLabel = new QToolButton;
        m_escLabel->setText(QStringLiteral("\u2715 Esc"));
        m_escLabel->setAutoRaise(true);
        m_escLabel->setCursor(Qt::PointingHandCursor);
        m_escLabel->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: none; padding: 2px 6px; }"
            "QToolButton:hover { color: %2; }")
            .arg(theme.textDim.name(), theme.indHoverSpan.name()));
        connect(m_escLabel, &QToolButton::clicked, this, [this]() {
            hide();
        });
        row->addWidget(m_escLabel);

        layout->addLayout(row);
    }

    // Row 2: + Create new type button (flat, no gradient)
    {
        m_createBtn = new QToolButton;
        m_createBtn->setText(QStringLiteral("+ Create new type\u2026"));
        m_createBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_createBtn->setAutoRaise(true);
        m_createBtn->setCursor(Qt::PointingHandCursor);
        m_createBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: none; padding: 3px 6px; }"
            "QToolButton:hover { color: %2; background: %3; }")
            .arg(theme.textMuted.name(), theme.text.name(), theme.hover.name()));
        connect(m_createBtn, &QToolButton::clicked, this, [this]() {
            emit createNewTypeRequested();
            hide();
        });
        layout->addWidget(m_createBtn);
    }

    // Separator
    {
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Plain);
        QPalette sepPal = pal;
        sepPal.setColor(QPalette::WindowText, theme.border);
        sep->setPalette(sepPal);
        sep->setFixedHeight(1);
        layout->addWidget(sep);
    }

    // Row 3: Modifier toggles [ plain ] [ * ] [ ** ] [ [n] ]
    {
        m_modRow = new QWidget;
        auto* modLayout = new QHBoxLayout(m_modRow);
        modLayout->setContentsMargins(0, 0, 0, 0);
        modLayout->setSpacing(3);

        m_modGroup = new QButtonGroup(this);
        m_modGroup->setExclusive(true);

        QString btnStyle = QStringLiteral(
            "QToolButton { color: %1; background: %2; border: 1px solid %3;"
            "  padding: 2px 8px; border-radius: 3px; }"
            "QToolButton:checked { color: %4; background: %5; border-color: %5; }"
            "QToolButton:hover:!checked { background: %6; }")
            .arg(theme.textDim.name(), theme.background.name(), theme.border.name(),
                 theme.text.name(), theme.selected.name(), theme.hover.name());

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

        m_btnPlain  = makeToggle(QStringLiteral("plain"), 0);
        m_btnPtr    = makeToggle(QStringLiteral("*"),     1);
        m_btnDblPtr = makeToggle(QStringLiteral("**"),    2);
        m_btnArray  = makeToggle(QStringLiteral("[n]"),   3);
        m_btnPlain->setChecked(true);

        // Array count input (shown only when [n] is active)
        m_arrayCountEdit = new QLineEdit;
        m_arrayCountEdit->setPlaceholderText(QStringLiteral("n"));
        m_arrayCountEdit->setValidator(new QIntValidator(1, 99999, m_arrayCountEdit));
        m_arrayCountEdit->setFixedWidth(50);
        m_arrayCountEdit->setPalette(pal);
        m_arrayCountEdit->hide();
        modLayout->addWidget(m_arrayCountEdit);

        modLayout->addStretch();
        layout->addWidget(m_modRow);

        connect(m_modGroup, &QButtonGroup::idToggled,
                this, [this](int id, bool checked) {
            if (!checked) return;
            m_arrayCountEdit->setVisible(id == 3);
            if (id == 3) m_arrayCountEdit->setFocus();
            updateModifierPreview();
        });
        connect(m_arrayCountEdit, &QLineEdit::textChanged,
                this, [this]() { updateModifierPreview(); });
    }

    // Row 4: Filter + preview
    {
        m_filterEdit = new QLineEdit;
        m_filterEdit->setPlaceholderText(QStringLiteral("Filter types\u2026"));
        m_filterEdit->setClearButtonEnabled(true);
        m_filterEdit->setPalette(pal);
        m_filterEdit->installEventFilter(this);
        connect(m_filterEdit, &QLineEdit::textChanged,
                this, &TypeSelectorPopup::applyFilter);
        layout->addWidget(m_filterEdit);

        m_previewLabel = new QLabel;
        m_previewLabel->setPalette(pal);
        m_previewLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; padding: 1px 6px; }").arg(theme.syntaxType.name()));
        m_previewLabel->hide();
        layout->addWidget(m_previewLabel);
    }

    // Row 4: List
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
        m_listView->installEventFilter(this);

        auto* delegate = new TypeSelectorDelegate(this, m_listView);
        m_listView->setItemDelegate(delegate);

        layout->addWidget(m_listView, 1);

        connect(m_listView, &QListView::clicked,
                this, [this](const QModelIndex& index) {
            acceptIndex(index.row());
        });
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

void TypeSelectorPopup::setFont(const QFont& font) {
    m_font = font;

    m_titleLabel->setFont([&]() {
        QFont f = font; f.setBold(true); return f;
    }());
    m_escLabel->setFont(font);
    m_createBtn->setFont(font);
    m_filterEdit->setFont(font);
    m_listView->setFont(font);
    m_previewLabel->setFont(font);

    QFont smallFont = font;
    smallFont.setPointSize(qMax(7, font.pointSize() - 1));
    m_btnPlain->setFont(smallFont);
    m_btnPtr->setFont(smallFont);
    m_btnDblPtr->setFont(smallFont);
    m_btnArray->setFont(smallFont);
    m_arrayCountEdit->setFont(smallFont);

    auto* delegate = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
    if (delegate)
        delegate->setFont(font);
}

void TypeSelectorPopup::setTitle(const QString& title) {
    m_titleLabel->setText(title);
}

void TypeSelectorPopup::setMode(TypePopupMode mode) {
    m_mode = mode;
    // Show modifier toggles for modes where type modifiers make sense
    bool showMods = (mode == TypePopupMode::FieldType
                     || mode == TypePopupMode::ArrayElement);
    m_modRow->setVisible(showMods);
    // Reset to plain when showing
    if (showMods) {
        m_btnPlain->setChecked(true);
        m_arrayCountEdit->clear();
        m_arrayCountEdit->hide();
    }
}

void TypeSelectorPopup::setCurrentNodeSize(int bytes) {
    m_currentNodeSize = bytes;
}

void TypeSelectorPopup::setTypes(const QVector<TypeEntry>& types, const TypeEntry* current) {
    m_allTypes = types;
    if (current) {
        m_currentEntry = *current;
        m_hasCurrent = true;
    } else {
        m_currentEntry = TypeEntry{};
        m_hasCurrent = false;
    }
    // Reset modifier toggles
    m_btnPlain->setChecked(true);
    m_arrayCountEdit->clear();
    m_arrayCountEdit->hide();
    m_previewLabel->hide();

    m_filterEdit->clear();
    applyFilter(QString());
}

void TypeSelectorPopup::popup(const QPoint& globalPos) {
    QFontMetrics fm(m_font);
    int maxTextW = fm.horizontalAdvance(QStringLiteral("Choose element type      Esc"));
    for (const auto& t : m_allTypes) {
        QString text = t.classKeyword.isEmpty()
            ? t.displayName
            : (t.classKeyword + QStringLiteral(" ") + t.displayName);
        int w = 10 + 20 + fm.horizontalAdvance(text) + 16;
        if (w > maxTextW) maxTextW = w;
    }
    int popupW = qBound(280, maxTextW + 24, 500);
    int rowH = fm.height() + 8;
    int headerH = rowH * 3 + 30;
    if (m_modRow->isVisible())
        headerH += rowH + 4;  // extra row for modifier toggles
    int listH = qBound(rowH * 3, rowH * (int)m_filteredTypes.size(), rowH * 14);
    int popupH = headerH + listH;

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

void TypeSelectorPopup::updateModifierPreview() {
    int modId = m_modGroup->checkedId();
    if (modId <= 0) {
        m_previewLabel->hide();
        return;
    }
    QString suffix;
    if (modId == 1) suffix = QStringLiteral("*");
    else if (modId == 2) suffix = QStringLiteral("**");
    else if (modId == 3) {
        QString countText = m_arrayCountEdit->text().trimmed();
        suffix = countText.isEmpty()
            ? QStringLiteral("[n]")
            : QStringLiteral("[%1]").arg(countText);
    }
    m_previewLabel->setText(QStringLiteral("\u2192 <type>%1").arg(suffix));
    m_previewLabel->show();
}

void TypeSelectorPopup::applyFilter(const QString& text) {
    m_filteredTypes.clear();
    QStringList displayStrings;

    QString filterBase = text.trimmed();

    // Separate primitives and composites
    QVector<TypeEntry> primitives, composites;
    for (const auto& t : m_allTypes) {
        if (t.entryKind == TypeEntry::Section) continue;  // skip stale sections
        bool matchesFilter = filterBase.isEmpty()
            || t.displayName.contains(filterBase, Qt::CaseInsensitive)
            || t.classKeyword.contains(filterBase, Qt::CaseInsensitive);
        if (!matchesFilter) continue;

        if (t.entryKind == TypeEntry::Primitive)
            primitives.append(t);
        else if (t.entryKind == TypeEntry::Composite)
            composites.append(t);
    }

    // For non-Root modes, sort primitives: same-size first, then rest
    if (m_mode != TypePopupMode::Root && m_currentNodeSize > 0 && !primitives.isEmpty()) {
        QVector<TypeEntry> sameSize, other;
        for (const auto& p : primitives) {
            if (sizeForKind(p.primitiveKind) == m_currentNodeSize)
                sameSize.append(p);
            else
                other.append(p);
        }
        primitives = sameSize + other;
    }

    // Helper lambdas for appending sections
    auto appendPrimitives = [&]() {
        if (primitives.isEmpty()) return;
        TypeEntry sec;
        sec.entryKind = TypeEntry::Section;
        sec.displayName = QStringLiteral("primitives");
        sec.enabled = false;
        m_filteredTypes.append(sec);
        displayStrings << sec.displayName;
        for (const auto& p : primitives) {
            m_filteredTypes.append(p);
            displayStrings << p.displayName;
        }
    };
    auto appendComposites = [&]() {
        if (composites.isEmpty()) return;
        TypeEntry sec;
        sec.entryKind = TypeEntry::Section;
        sec.displayName = QStringLiteral("project types");
        sec.enabled = false;
        m_filteredTypes.append(sec);
        displayStrings << sec.displayName;
        for (const auto& c : composites) {
            m_filteredTypes.append(c);
            QString label = c.classKeyword.isEmpty()
                ? c.displayName
                : (c.classKeyword + QStringLiteral(" ") + c.displayName);
            displayStrings << label;
        }
    };

    // Root mode: project types first (composites are the primary selection)
    if (m_mode == TypePopupMode::Root) {
        appendComposites();
        appendPrimitives();
    } else {
        appendPrimitives();
        appendComposites();
    }

    m_model->setStringList(displayStrings);

    auto* delegate = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
    if (delegate)
        delegate->setFilteredTypes(&m_filteredTypes, &m_currentEntry, m_hasCurrent);

    // Select first selectable item
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
