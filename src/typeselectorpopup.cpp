#include "typeselectorpopup.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QToolButton>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QIcon>
#include <QApplication>
#include <QScreen>
#include "themes/thememanager.h"

namespace rcx {

// ── Custom delegate: gutter checkmark + icon + text ──

class TypeSelectorDelegate : public QStyledItemDelegate {
public:
    explicit TypeSelectorDelegate(TypeSelectorPopup* popup, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_popup(popup) {}

    void setFont(const QFont& f) { m_font = f; }
    void setCurrentTypes(const QVector<TypeEntry>* filtered, uint64_t currentId) {
        m_filtered = filtered;
        m_currentId = currentId;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();

        // Background: themed colors
        const auto& t = ThemeManager::instance().current();
        if (option.state & QStyle::State_Selected)
            painter->fillRect(option.rect, t.selected);
        else if (option.state & QStyle::State_MouseOver)
            painter->fillRect(option.rect, t.hover);

        int x = option.rect.x();
        int y = option.rect.y();
        int h = option.rect.height();

        // 18px gutter: side triangle if current
        int row = index.row();
        if (m_filtered && row >= 0 && row < m_filtered->size()
            && (*m_filtered)[row].id == m_currentId) {
            painter->setPen(t.syntaxType);
            QFont checkFont = m_font;
            painter->setFont(checkFont);
            painter->drawText(QRect(x, y, 18, h), Qt::AlignCenter,
                              QString(QChar(0x25B8)));
        }
        x += 18;

        // Icon 16x16
        static QIcon structIcon(QStringLiteral(":/vsicons/symbol-structure.svg"));
        structIcon.paint(painter, x, y + (h - 16) / 2, 16, 16);
        x += 20;

        // Text
        painter->setPen(option.state & QStyle::State_Selected
                        ? option.palette.color(QPalette::HighlightedText)
                        : option.palette.color(QPalette::Text));
        painter->setFont(m_font);
        painter->drawText(QRect(x, y, option.rect.right() - x, h),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          index.data().toString());

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& /*option*/,
                   const QModelIndex& /*index*/) const override {
        QFontMetrics fm(m_font);
        return QSize(200, fm.height() + 8);
    }

private:
    TypeSelectorPopup* m_popup = nullptr;
    QFont m_font;
    const QVector<TypeEntry>* m_filtered = nullptr;
    uint64_t m_currentId = 0;
};

// ── TypeSelectorPopup ──

TypeSelectorPopup::TypeSelectorPopup(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_DeleteOnClose, false);

    // Theme palette
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

    // Thin border
    setFrameShape(QFrame::Box);
    setLineWidth(1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    // Row 1: title + Esc hint
    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        m_titleLabel = new QLabel(QStringLiteral("View as type"));
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

    // Row 2: + Create new type button
    {
        m_createBtn = new QToolButton;
        m_createBtn->setText(QStringLiteral("+ Create new type\u2026"));
        m_createBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_createBtn->setAutoRaise(true);
        m_createBtn->setCursor(Qt::PointingHandCursor);
        m_createBtn->setPalette(pal);
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

    // Row 3: Filter
    {
        m_filterEdit = new QLineEdit;
        m_filterEdit->setPlaceholderText(QStringLiteral("Filter types\u2026"));
        m_filterEdit->setClearButtonEnabled(true);
        m_filterEdit->setPalette(pal);
        m_filterEdit->installEventFilter(this);
        connect(m_filterEdit, &QLineEdit::textChanged,
                this, &TypeSelectorPopup::applyFilter);
        layout->addWidget(m_filterEdit);
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

void TypeSelectorPopup::setFont(const QFont& font) {
    m_font = font;

    m_titleLabel->setFont([&]() {
        QFont f = font; f.setBold(true); return f;
    }());
    m_escLabel->setFont(font);
    m_createBtn->setFont(font);
    m_filterEdit->setFont(font);
    m_listView->setFont(font);

    auto* delegate = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
    if (delegate)
        delegate->setFont(font);
}

void TypeSelectorPopup::setTypes(const QVector<TypeEntry>& types, uint64_t currentId) {
    m_allTypes = types;
    m_currentId = currentId;
    m_filterEdit->clear();
    applyFilter(QString());
}

void TypeSelectorPopup::popup(const QPoint& globalPos) {
    // Size: width based on longest entry, height based on count
    QFontMetrics fm(m_font);
    int maxTextW = fm.horizontalAdvance(QStringLiteral("View as type      Esc"));
    for (const auto& t : m_allTypes) {
        QString text = t.classKeyword + QStringLiteral(" ") + t.displayName;
        int w = 18 + 20 + fm.horizontalAdvance(text) + 16;  // gutter + icon + text + pad
        if (w > maxTextW) maxTextW = w;
    }
    int popupW = qBound(250, maxTextW + 24, 500);  // +margins
    int rowH = fm.height() + 8;
    int headerH = rowH * 3 + 30;  // title + button + filter + separators/margins
    int listH = qBound(rowH * 3, rowH * (int)m_allTypes.size(), rowH * 12);
    int popupH = headerH + listH;

    // Clamp to screen
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
    for (int i = 0; i < m_filteredTypes.size(); i++) {
        if (m_filteredTypes[i].id == m_currentId) {
            m_listView->setCurrentIndex(m_model->index(i));
            break;
        }
    }
}

void TypeSelectorPopup::applyFilter(const QString& text) {
    m_filteredTypes.clear();
    QStringList displayStrings;

    for (const auto& t : m_allTypes) {
        if (text.isEmpty()
            || t.displayName.contains(text, Qt::CaseInsensitive)
            || t.classKeyword.contains(text, Qt::CaseInsensitive)) {
            m_filteredTypes.append(t);
            displayStrings << (t.classKeyword + QStringLiteral(" ") + t.displayName);
        }
    }

    m_model->setStringList(displayStrings);

    // Update delegate data
    auto* delegate = static_cast<TypeSelectorDelegate*>(m_listView->itemDelegate());
    if (delegate)
        delegate->setCurrentTypes(&m_filteredTypes, m_currentId);

    // Select first match
    if (!m_filteredTypes.isEmpty())
        m_listView->setCurrentIndex(m_model->index(0));
}

void TypeSelectorPopup::acceptCurrent() {
    QModelIndex idx = m_listView->currentIndex();
    if (idx.isValid())
        acceptIndex(idx.row());
}

void TypeSelectorPopup::acceptIndex(int row) {
    if (row < 0 || row >= m_filteredTypes.size()) return;
    emit typeSelected(m_filteredTypes[row].id);
    hide();
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
                if (!m_listView->currentIndex().isValid() && m_model->rowCount() > 0)
                    m_listView->setCurrentIndex(m_model->index(0));
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
            }
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                acceptCurrent();
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
