#include "sourcechooserpopup.h"
#include "fontutil.h"
#include "paintutil.h"
#include "svgicon.h"
#include "widgets/popup_chrome.h"
#include "widgets/section_header.h"
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace rcx {
namespace {

constexpr int kMaxProviderButtons = 8;

QRect rowMenuRect(const QRect& row) {
    const int size = qMin(row.height(), 28);
    return QRect(row.right() - size - 5, row.center().y() - size / 2, size, size);
}

// Bounded linear matching, including paths longer than 256 characters. Every
// query token must match; exact substrings rank ahead of subsequences.
int matchScore(const QString& query, const QString& text, const QString& name) {
    int total = 0;
    for (const QString& token : query.split(' ', Qt::SkipEmptyParts)) {
        const int at = text.indexOf(token, 0, Qt::CaseInsensitive);
        if (at >= 0) {
            total += 100 + token.size() * 5 + (at == 0 ? 40 : 0);
            continue;
        }
        int pos = 0, score = 0, previous = -2;
        for (const QChar ch : token) {
            pos = name.indexOf(ch, pos, Qt::CaseInsensitive);
            if (pos < 0) return 0;
            score += pos == previous + 1 ? 5 : 1;
            previous = pos++;
        }
        total += score;
    }
    return total;
}

QString tooltip(const SourceEntry& entry) {
    return QStringLiteral("<qt>") + entry.details().toHtmlEscaped().replace('\n', QStringLiteral("<br>"))
        + QStringLiteral("</qt>");
}

class SourceListModel : public QStringListModel {
public:
    using QStringListModel::QStringListModel;
    const QVector<SourceEntry>* entries = nullptr;
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        if (!entries || !index.isValid() || index.row() >= entries->size()) return Qt::NoItemFlags;
        const auto& e = entries->at(index.row());
        if (!e.enabled || e.entryKind == SourceEntry::SectionHeader) return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (entries && index.isValid() && index.row() < entries->size()) {
            const auto& e = entries->at(index.row());
            if (role == Qt::ToolTipRole) return tooltip(e);
            if (role == Qt::AccessibleDescriptionRole) return e.details();
            if (role == Qt::UserRole) return e.identity();
        }
        return QStringListModel::data(index, role);
    }
};

class ProviderButton : public QToolButton {
public:
    using QToolButton::QToolButton;
    const Theme* theme = nullptr;
    QString iconPath;
protected:
    void paintEvent(QPaintEvent*) override {
        if (!theme) return;
        QPainter p(this);
        const Theme& t = *theme;
        p.fillRect(rect(), isDown() || hasFocus() ? t.selected : underMouse() ? t.hover : t.background);
        fillDeviceFrameOfRect(p, QRectF(rect()), hasFocus() ? t.borderFocused : containerBorderColor(t));
        if (!isEnabled()) p.setOpacity(0.4);
        p.setFont(font());
        const int iconSize = qMin(18, fontMetrics().height());
        const QPixmap pm = themedVsIcon(iconPath, t.textDim, iconSize, devicePixelRatioF())
            .pixmap(QSize(iconSize, iconSize), devicePixelRatioF());
        drawPixmapSnapped(p, QPointF(8, (height() - iconSize) / 2), pm);
        const QRect textRect(8 + iconSize + 8, 0, qMax(0, width() - iconSize - 32), height());
        p.setPen(t.text);
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                   fontMetrics().elidedText(text(), Qt::ElideRight, textRect.width()));
    }
};

class SourceChooserDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    const QVector<SourceEntry>* entries = nullptr;
    const Theme* theme = nullptr;
    QFont baseFont;
    int hoverMenuRow = -1;

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        if (entries && idx.row() >= 0 && idx.row() < entries->size()
            && entries->at(idx.row()).entryKind == SourceEntry::SectionHeader)
            return QSize(opt.rect.width(), sectionHeaderHeight(QFontMetrics(sectionHeaderFont(baseFont))));
        return QSize(opt.rect.width(), QFontMetrics(baseFont).height() + 12);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        if (!entries || !theme || idx.row() < 0 || idx.row() >= entries->size()) return;
        const auto& e = entries->at(idx.row());
        const Theme& t = *theme;
        const QRect r = opt.rect;
        p->save();
        p->setClipRect(r);
        p->setRenderHint(QPainter::Antialiasing, false);
        if (e.entryKind == SourceEntry::SectionHeader) {
            p->fillRect(r, t.background);
            p->setFont(sectionHeaderFont(baseFont));
            p->setPen(t.textDim);
            p->drawText(r.adjusted(kGutter, 0, -kGutter, 0), Qt::AlignVCenter | Qt::AlignLeft, e.displayName);
            fillBottomDeviceRowOfRect(*p, QRectF(r), containerBorderColor(t));
            p->restore();
            return;
        }
        const bool selected = e.enabled && (opt.state & QStyle::State_Selected);
        const bool hovered = e.enabled && (opt.state & QStyle::State_MouseOver);
        p->fillRect(r, selected ? t.selected : hovered ? t.hover : t.background);
        if (!e.enabled) p->setOpacity(0.4);
        p->setFont(baseFont);
        const QFontMetrics fm(baseFont);
        const int iconSize = qMin(18, fm.height());
        const int iconX = r.left() + kGutter;
        if (e.isActive) {
            p->setPen(t.indHoverSpan);
            p->drawText(QRect(iconX, r.y(), iconSize, r.height()), Qt::AlignCenter, QStringLiteral("\u2713"));
        } else {
            const QPixmap pm = themedVsIcon(e.iconPath, t.textDim, iconSize, p->device()->devicePixelRatioF())
                .pixmap(QSize(iconSize, iconSize), p->device()->devicePixelRatioF());
            drawPixmapSnapped(*p, QPointF(iconX, r.y() + (r.height() - iconSize) / 2), pm);
        }
        const int x = iconX + iconSize + kGutter;
        const int right = rowMenuRect(r).left() - 8;
        const int available = qMax(0, right - x);
        if (e.entryKind == SourceEntry::ProviderAction) {
            p->setPen(t.text);
            p->drawText(QRect(x, r.y(), r.right() - x - kGutter, r.height()),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        fm.elidedText(e.displayName + QStringLiteral("\u2026"),
                                      Qt::ElideRight, r.right() - x - kGutter));
            p->restore();
            return;
        }
        QFont small = baseFont;
        small.setPointSize(qMax(8, resolvedPointSize(baseFont) - 1));
        const QFontMetrics sfm(small);
        const int kindWidth = qMin(sfm.horizontalAdvance(QStringLiteral("Process via IPC")), available / 3);
        const int detailWidth = available / 4;
        const int nameWidth = qMax(0, available - kindWidth - detailWidth - 16);
        QFont nameFont = baseFont;
        nameFont.setBold(e.isActive);
        p->setFont(nameFont);
        p->setPen(e.isStale ? t.textMuted : t.text);
        p->drawText(QRect(x, r.y(), nameWidth, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(nameFont).elidedText(e.displayName, Qt::ElideRight, nameWidth));
        QString detail = e.statusText();
        if (detail.isEmpty()) {
            if (!e.pid.isEmpty()) detail = QStringLiteral("PID ") + e.pid;
            else if (!e.filePath.isEmpty()) detail = QFileInfo(e.filePath).absolutePath();
        }
        p->setFont(small);
        p->setPen(e.isStale ? t.markerPtr : t.textMuted);
        p->drawText(QRect(x + nameWidth + 8, r.y(), detailWidth, r.height()),
                    Qt::AlignLeft | Qt::AlignVCenter, sfm.elidedText(detail, Qt::ElideMiddle, detailWidth));
        p->setPen(t.textDim);
        p->drawText(QRect(right - kindWidth, r.y(), kindWidth, r.height()),
                    Qt::AlignRight | Qt::AlignVCenter, sfm.elidedText(e.kindLabel, Qt::ElideRight, kindWidth));
        if (e.entryKind == SourceEntry::SavedSource) {
            p->setFont(baseFont);
            p->setPen(idx.row() == hoverMenuRow ? t.text : t.textMuted);
            p->drawText(rowMenuRect(r), Qt::AlignCenter, QStringLiteral("\u22ef"));
        }
        p->restore();
    }
};

} // namespace

SourceChooserPopup::SourceChooserPopup(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFocusPolicy(Qt::StrongFocus);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(1, 1, 1, 1);
    outer->setSpacing(0);
    // The one-device-pixel frame and seams retain the shared popup grammar.
    auto* title = new QHBoxLayout;
    title->setContentsMargins(kGutter, 6, 6, 2);
    m_titleLabel = new QLabel(QStringLiteral("Data source"));
    title->addWidget(m_titleLabel);
    title->addStretch();
    m_menuButton = new QToolButton;
    m_menuButton->setObjectName(QStringLiteral("sourceManagement"));
    m_menuButton->setText(QStringLiteral("\u22ef"));
    m_menuButton->setToolTip(QStringLiteral("Manage sources and provider actions"));
    m_menuButton->setAccessibleName(QStringLiteral("Manage sources"));
    m_menuButton->setFocusPolicy(Qt::StrongFocus);
    title->addWidget(m_menuButton);
    connect(m_menuButton, &QToolButton::clicked, this, &SourceChooserPopup::showManagementMenu);
    m_escBtn = new QToolButton;
    m_escBtn->setText(QStringLiteral("Esc"));
    m_escBtn->setFocusPolicy(Qt::NoFocus);
    title->addWidget(m_escBtn);
    connect(m_escBtn, &QToolButton::clicked, this, &QFrame::hide);
    outer->addLayout(title);

    auto* filter = new PopupFilterField;
    m_filterEdit = filter;
    filter->setObjectName(QStringLiteral("sourceSearch"));
    filter->setPlaceholderText(QStringLiteral("Search names, PID, paths, source types..."));
    filter->setAccessibleName(QStringLiteral("Search sources"));
    filter->setFrame(false);
    m_clearAction = filter->addAction(QIcon(), QLineEdit::TrailingPosition);
    m_clearAction->setToolTip(QStringLiteral("Clear search"));
    m_clearAction->setVisible(false);
    connect(m_clearAction, &QAction::triggered, filter, &QLineEdit::clear);
    outer->addWidget(filter);

    m_providerArea = new QWidget;
    m_providerArea->setObjectName(QStringLiteral("providerActions"));
    auto* providers = new QVBoxLayout(m_providerArea);
    providers->setContentsMargins(kGutter, 4, kGutter, 8);
    providers->setSpacing(4);
    auto* providerHeading = new QHBoxLayout;
    m_providerTitle = new QLabel(QStringLiteral("Add source"));
    providerHeading->addWidget(m_providerTitle);
    providerHeading->addStretch();
    m_allButton = new QToolButton;
    m_allButton->setObjectName(QStringLiteral("allSources"));
    m_allButton->setText(QStringLiteral("All sources..."));
    m_allButton->setFocusPolicy(Qt::StrongFocus);
    providerHeading->addWidget(m_allButton);
    providers->addLayout(providerHeading);
    m_providerGrid = new QGridLayout;
    m_providerGrid->setSpacing(4);
    m_providerGrid->setColumnStretch(0, 1);
    m_providerGrid->setColumnStretch(1, 1);
    providers->addLayout(m_providerGrid);
    outer->addWidget(m_providerArea);
    connect(m_allButton, &QToolButton::clicked, this, [this] {
        m_showAllProviders = true;
        applyFilter(m_filterEdit->text());
        focusList();
    });

    m_listHeading = new QWidget;
    m_listHeading->setObjectName(QStringLiteral("sourceListHeading"));
    auto* heading = new QHBoxLayout(m_listHeading);
    heading->setContentsMargins(kGutter, 4, kGutter, 4);
    m_listTitle = new QLabel;
    heading->addWidget(m_listTitle);
    heading->addStretch();
    m_backButton = new QToolButton;
    m_backButton->setObjectName(QStringLiteral("backToSources"));
    m_backButton->setText(QStringLiteral("\u2190 Back"));
    m_backButton->setFocusPolicy(Qt::StrongFocus);
    heading->addWidget(m_backButton);
    outer->addWidget(m_listHeading);
    connect(m_backButton, &QToolButton::clicked, this, [this] {
        m_showAllProviders = false;
        m_filterEdit->clear();
        applyFilter(QString());
        m_filterEdit->setFocus();
    });

    m_listView = new QListView;
    m_listView->setObjectName(QStringLiteral("savedSources"));
    m_listView->setAccessibleName(QStringLiteral("Sources"));
    auto* model = new SourceListModel(this);
    model->entries = &m_filteredEntries;
    m_model = model;
    m_listView->setModel(m_model);
    m_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_listView->setMouseTracking(true);
    m_listView->setFrameShape(QFrame::NoFrame);
    m_listView->setMinimumHeight(0);
    auto* delegate = new SourceChooserDelegate(this);
    delegate->entries = &m_filteredEntries;
    delegate->theme = &m_theme;
    m_listView->setItemDelegate(delegate);
    auto* seamBar = new SeamScrollBar(Qt::Vertical);
    seamBar->seams = [this] {
        QVector<SeamScrollBar::Seam> seams;
        for (int i = 0; i < m_filteredEntries.size(); ++i) {
            if (m_filteredEntries[i].entryKind != SourceEntry::SectionHeader) continue;
            const QRect r = m_listView->visualRect(m_model->index(i));
            if (r.intersects(m_listView->viewport()->rect()))
                seams.append({QRectF(r), true, containerBorderColor(m_theme)});
        }
        return seams;
    };
    m_listView->setVerticalScrollBar(seamBar);
    connect(seamBar, &QScrollBar::valueChanged, this, [this] { update(); });
    outer->addWidget(m_listView, 1);
    m_emptyLabel = new QLabel(m_listView->viewport());
    m_emptyLabel->setObjectName(QStringLiteral("emptySources"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_footerLabel = new QLabel(QStringLiteral("Tab actions   \u2191\u2193 sources   Enter select   Esc close"));
    m_footerLabel->setObjectName(QStringLiteral("sourceFooter"));
    m_footerLabel->setContentsMargins(kGutter, 4, kGutter, 4);
    m_footerLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_footerLabel->installEventFilter(this);
    outer->addWidget(m_footerLabel);

    connect(filter, &QLineEdit::textChanged, this, &SourceChooserPopup::applyFilter);
    connect(m_listView, &QListView::clicked, this, [this](const QModelIndex& i) { acceptIndex(i.row()); });
    for (QObject* object : {static_cast<QObject*>(filter), static_cast<QObject*>(m_listView),
                           static_cast<QObject*>(m_listView->viewport()), static_cast<QObject*>(m_menuButton),
                           static_cast<QObject*>(m_allButton), static_cast<QObject*>(m_backButton)})
        object->installEventFilter(this);
    setFont(QApplication::font());
}

void SourceChooserPopup::setFont(const QFont& font) {
    QFrame::setFont(font);
    m_font = font;
    m_filterEdit->setFont(font);
    m_filterEdit->setFixedHeight(qMax(PopupFilterField::kMinHeight, QFontMetrics(font).height() + 10));
    m_listView->setFont(font);
    QFont bold = font;
    bold.setBold(true);
    m_titleLabel->setFont(bold);
    QFont small = font;
    small.setPointSize(qMax(8, resolvedPointSize(font) - 1));
    m_emptyLabel->setFont(small);
    for (QWidget* w : {static_cast<QWidget*>(m_footerLabel), static_cast<QWidget*>(m_escBtn),
                       static_cast<QWidget*>(m_allButton), static_cast<QWidget*>(m_backButton)})
        w->setFont(small);
    m_providerTitle->setFont(sectionHeaderFont(font));
    m_listTitle->setFont(sectionHeaderFont(font));
    for (auto* button : m_providerButtons) {
        button->setFont(font);
        button->setFixedHeight(QFontMetrics(font).height() + 14);
    }
    static_cast<SourceChooserDelegate*>(m_listView->itemDelegate())->baseFont = font;
}

void SourceChooserPopup::applyTheme(const Theme& theme) {
    m_theme = theme;
    QPalette pal = palette();
    pal.setColor(QPalette::Window, theme.background);
    pal.setColor(QPalette::WindowText, theme.text);
    pal.setColor(QPalette::Base, theme.background);
    pal.setColor(QPalette::Text, theme.text);
    pal.setColor(QPalette::Highlight, theme.selected);
    pal.setColor(QPalette::HighlightedText, theme.text);
    setPalette(pal);
    auto* field = static_cast<PopupFilterField*>(m_filterEdit);
    field->applyTheme(theme, theme.background, kGutter - 2);
    m_clearAction->setIcon(themedVsIcon(QStringLiteral(":/vsicons/close.svg"),
                                      theme.textDim, 12, devicePixelRatioF()));
    m_listView->setStyleSheet(QStringLiteral(
        "QListView { background: %1; border: none; }"
        "QListView::item { border: none; background: transparent; }").arg(theme.background.name()));
    m_listView->verticalScrollBar()->setStyleSheet(popupScrollBarQss(theme, theme.background));
    for (QLabel* label : {m_providerTitle, m_listTitle, m_footerLabel, m_emptyLabel})
        label->setStyleSheet(QStringLiteral("color: %1;").arg(theme.textDim.name()));
    m_titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme.text.name()));
    for (QToolButton* button : {m_escBtn, m_menuButton, m_allButton, m_backButton}) {
        button->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; background: transparent; border: none; padding: 2px 4px; }"
            "QToolButton:hover { background: %2; color: %3; }"
            "QToolButton:focus { background: %4; color: %3; }")
            .arg(theme.textDim.name(), theme.hover.name(), theme.text.name(), theme.selected.name()));
    }
    for (auto* button : m_providerButtons) button->update();
    m_listView->viewport()->update();
    update();
}

void SourceChooserPopup::rebuildProviders() {
    QVector<SourceEntry> providers;
    QStringList keys;
    for (const auto& e : m_allEntries) {
        if (e.entryKind != SourceEntry::ProviderAction) continue;
        providers.append(e);
        keys << e.identity() + e.displayName + e.details() + QString::number(e.enabled);
    }
    if (keys == m_providerKeys) return;
    m_providerKeys = keys;
    while (auto* item = m_providerGrid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_providerButtons.clear();
    for (int i = 0; i < qMin(kMaxProviderButtons, int(providers.size())); ++i) {
        const auto& e = providers[i];
        auto* button = new ProviderButton;
        button->setObjectName(QStringLiteral("provider_") + e.providerIdentifier);
        button->setProperty("providerId", e.providerIdentifier);
        button->setText(e.displayName + QStringLiteral("\u2026"));
        button->setAccessibleName(e.displayName);
        button->setAccessibleDescription(e.description);
        button->setToolTip(tooltip(e));
        button->setEnabled(e.enabled);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        button->setMinimumWidth(0);
        button->theme = &m_theme;
        button->iconPath = e.iconPath;
        button->installEventFilter(this);
        connect(button, &QToolButton::clicked, this, [this, id = e.providerIdentifier] { activateProvider(id); });
        m_providerGrid->addWidget(button, i / 2, i % 2);
        m_providerButtons.append(button);
    }
    m_allButton->setVisible(providers.size() > kMaxProviderButtons);
    // Search -> provider buttons -> all providers -> source list -> management.
    QWidget* previous = m_filterEdit;
    for (auto* button : m_providerButtons) {
        QWidget::setTabOrder(previous, button);
        previous = button;
    }
    QWidget::setTabOrder(previous, m_allButton);
    QWidget::setTabOrder(m_allButton, m_backButton);
    QWidget::setTabOrder(m_backButton, m_listView);
    QWidget::setTabOrder(m_listView, m_menuButton);
    QWidget::setTabOrder(m_menuButton, m_filterEdit);
    setFont(m_font);
}

void SourceChooserPopup::setSources(const QVector<SourceEntry>& entries) {
    m_allEntries.clear();
    for (auto e : entries) {
        if (e.entryKind != SourceEntry::SavedSource && e.entryKind != SourceEntry::ProviderAction) continue;
        if (e.entryKind == SourceEntry::ProviderAction) {
            e.searchAliases += QChar(' ') + e.displayName;
            e.displayName = providerLabel(e.providerIdentifier, e.displayName);
            e.description = providerDescription(e.providerIdentifier, e.description);
        }
        m_allEntries.append(e);
    }
    std::stable_sort(m_allEntries.begin(), m_allEntries.end(), [](const SourceEntry& a, const SourceEntry& b) {
        if (a.entryKind != b.entryKind) return a.entryKind == SourceEntry::SavedSource;
        if (a.entryKind == SourceEntry::SavedSource) return a.isActive && !b.isActive;
        const int ao = providerOrder(a.providerIdentifier), bo = providerOrder(b.providerIdentifier);
        if (ao != bo) return ao < bo;
        return QString::compare(a.displayName, b.displayName, Qt::CaseInsensitive) < 0;
    });
    if (!isVisible()) {
        QSignalBlocker blocker(m_filterEdit);
        m_filterEdit->clear();
        m_lastQuery.clear();
        m_showAllProviders = false;
        m_filteredEntries.clear();
    }
    rebuildProviders();
    applyFilter(m_filterEdit->text());
}

void SourceChooserPopup::setLivenessResults(const QVector<bool>& alive) {
    auto updateEntries = [&alive](QVector<SourceEntry>& entries) {
        for (auto& e : entries) {
            if (e.entryKind != SourceEntry::SavedSource || e.savedIndex < 0 || e.savedIndex >= alive.size()) continue;
            e.isStale = !alive[e.savedIndex] || !e.unavailableReason.isEmpty();
        }
    };
    updateEntries(m_allEntries);
    updateEntries(m_filteredEntries);
    // A status update never resets the model, selection, or scroll offset.
    if (m_model->rowCount())
        emit m_model->dataChanged(m_model->index(0), m_model->index(m_model->rowCount() - 1));
    m_listView->viewport()->update();
}

void SourceChooserPopup::applyFilter(const QString& text) {
    const QString query = text.trimmed();
    const bool queryChanged = query != m_lastQuery;
    const int oldRow = m_listView->currentIndex().row();
    const QString selectedId = oldRow >= 0 && oldRow < m_filteredEntries.size()
        ? m_filteredEntries[oldRow].identity() : QString();
    const int scroll = m_listView->verticalScrollBar()->value();
    m_lastQuery = query;
    m_clearAction->setVisible(!text.isEmpty());
    m_filteredEntries.clear();
    QVector<SourceEntry> saved, providers;
    struct Scored { SourceEntry entry; int score; };
    QVector<Scored> matches;
    for (const auto& e : m_allEntries) {
        if (query.isEmpty()) {
            if (e.entryKind == SourceEntry::SavedSource) saved.append(e);
            else providers.append(e);
            continue;
        }
        const QString searchable = e.displayName + ' ' + e.kindLabel + ' ' + e.pid + ' '
            + e.filePath + ' ' + e.providerTarget + ' ' + e.providerIdentifier + ' '
            + e.searchAliases + ' ' + e.description + ' ' + e.dllFileName;
        const int score = matchScore(query, searchable, e.displayName);
        if (score > 0) matches.append({e, score});
    }
    if (!query.isEmpty()) {
        std::stable_sort(matches.begin(), matches.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
        for (const auto& match : matches) {
            if (match.entry.entryKind == SourceEntry::SavedSource) saved.append(match.entry);
            else providers.append(match.entry);
        }
        auto appendGroup = [this](const QString& title, const QVector<SourceEntry>& group) {
            if (group.isEmpty()) return;
            SourceEntry header;
            header.entryKind = SourceEntry::SectionHeader;
            header.displayName = title;
            header.enabled = false;
            m_filteredEntries.append(header);
            m_filteredEntries += group;
        };
        appendGroup(QStringLiteral("Saved sources"), saved);
        appendGroup(QStringLiteral("Add source"), providers);
        m_listTitle->setText(QStringLiteral("Results (%1)").arg(saved.size() + providers.size()));
    } else if (m_showAllProviders) {
        m_filteredEntries = providers;
        m_listTitle->setText(QStringLiteral("All sources (%1)").arg(providers.size()));
    } else {
        m_filteredEntries = saved;
        m_listTitle->setText(QStringLiteral("Saved sources (%1)").arg(saved.size()));
    }
    m_providerArea->setVisible(query.isEmpty() && !m_showAllProviders && !m_providerButtons.isEmpty());
    m_backButton->setVisible(m_showAllProviders);
    QStringList names;
    for (const auto& e : m_filteredEntries) names.append(e.displayName);
    m_model->setStringList(names);
    int current = -1;
    if (!queryChanged && !selectedId.isEmpty()) {
        for (int i = 0; i < m_filteredEntries.size(); ++i)
            if (m_filteredEntries[i].identity() == selectedId) { current = i; break; }
        if (current < 0) {
            current = nextSelectableRow(qMin(oldRow, int(m_filteredEntries.size()) - 1), 1);
            if (current < 0) current = nextSelectableRow(int(m_filteredEntries.size()) - 1, -1);
        }
    } else if (!query.isEmpty()) {
        current = nextSelectableRow(0, 1);
    }
    m_listView->setCurrentIndex(current >= 0 ? m_model->index(current) : QModelIndex());
    if (!queryChanged) m_listView->verticalScrollBar()->setValue(scroll);
    m_emptyLabel->setText(query.isEmpty() ? QStringLiteral("No saved sources\nChoose a source above to get started.")
                                        : QStringLiteral("No matching sources"));
    m_emptyLabel->setVisible(m_filteredEntries.isEmpty());
    m_emptyLabel->setGeometry(m_listView->viewport()->rect().adjusted(16, 8, -16, -8));
    m_listView->verticalScrollBar()->update();
    update();
}

void SourceChooserPopup::popup(const QPoint& globalPos, int anchorTop) {
    m_anchor = globalPos;
    m_anchorTop = anchorTop;
    fitToScreen();
    show();
    raise();
    activateWindow();
    m_filterEdit->setFocus();
    m_filterEdit->selectAll();
}

void SourceChooserPopup::fitToScreen() {
    const QScreen* display = QApplication::screenAt(m_anchor);
    const QRect screen = (display ? display : QApplication::primaryScreen())->availableGeometry().adjusted(8, 8, -8, -8);
    const int width = qMin(screen.width(), qMax(480, QFontMetrics(m_font).horizontalAdvance('M') * 48));
    setFixedWidth(width);
    // Warm-up leaves cached size hints for the empty, styled containers.
    // Settle their layouts before measuring the popup; otherwise the grid's
    // new buttons only enter the outer size hint after its height is locked.
    for (int i = 0; i < layout()->count(); ++i) {
        auto* child = layout()->itemAt(i)->widget();
        if (child && child->layout()) {
            child->layout()->activate();
            child->updateGeometry();
        }
    }
    layout()->invalidate();
    layout()->activate();
    const int rowHeight = QFontMetrics(m_font).height() + 12;
    // Size once on open: small histories stay compact, long histories get
    // six rows. Adding/removing/filtering after opening never resizes it.
    const int chrome = layout()->sizeHint().height() - m_listView->sizeHint().height();
    const int savedCount = int(std::count_if(m_allEntries.cbegin(), m_allEntries.cend(),
        [](const SourceEntry& e) { return e.entryKind == SourceEntry::SavedSource; }));
    const int wanted = chrome + rowHeight * qBound(2, savedCount, 6);
    const int barTop = m_anchorTop >= 0 ? m_anchorTop : m_anchor.y();
    const int below = screen.bottom() + 1 - m_anchor.y();
    const int above = barTop - screen.top();
    const bool flip = below < wanted && above > below;
    const int space = qMax(0, flip ? above : below);
    const int height = qMin(wanted, qMax(space, qMin(screen.height(), chrome + rowHeight)));
    setFixedHeight(height);
    const int x = qBound(screen.left(), m_anchor.x(), screen.right() - width + 1);
    const int y = qBound(screen.top(), flip ? barTop - height : m_anchor.y(), screen.bottom() - height + 1);
    move(x, y);
}

void SourceChooserPopup::warmUp() { show(); hide(); }

void SourceChooserPopup::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), m_theme.background);
    const QColor seam = containerBorderColor(m_theme);
    const QRect field = m_filterEdit->geometry();
    fillBottomDeviceRowOfRect(p, QRectF(0, field.top(), width(), field.height()),
        static_cast<PopupFilterField*>(m_filterEdit)->seamColor());
    const QRect heading = m_listHeading->geometry();
    fillBottomDeviceRowOfRect(p, QRectF(0, heading.top(), width(), heading.height()), seam);
    const QRect footer = m_footerLabel->geometry();
    fillTopDeviceRowOfRect(p, QRectF(0, footer.top(), width(), footer.height()), seam);
    auto* bar = static_cast<SeamScrollBar*>(m_listView->verticalScrollBar());
    if (bar->seams) {
        const int y = m_listView->viewport()->mapTo(this, QPoint()).y();
        for (const auto& s : bar->seams())
            fillBottomDeviceRowOfRect(p, QRectF(0, s.rect.top() + y, width(), s.rect.height()), s.color);
    }
    fillDeviceFrameOfRect(p, QRectF(rect()), m_theme.border);
}

void SourceChooserPopup::hideEvent(QHideEvent* event) {
    QFrame::hideEvent(event);
    emit dismissed();
}

void SourceChooserPopup::styleMenu(QMenu* menu) {
    menu->setAttribute(Qt::WA_DeleteOnClose);
    // QMenu normally hides after a pick; that does not deliver a close event.
    // Dispose transient root menus then, while submenus live with their root.
    if (menu->parent() == this)
        connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->setFont(m_font);
    menu->setPalette(palette());
    menu->setStyleSheet(QStringLiteral(
        "QMenu { background: %1; color: %2; border: 1px solid %3; padding: 4px; }"
        "QMenu::item { padding: 5px 18px 5px 8px; }"
        "QMenu::item:selected { background: %4; }"
        "QMenu::item:disabled { color: %5; }")
        .arg(m_theme.background.name(), m_theme.text.name(), m_theme.border.name(),
             m_theme.selected.name(), m_theme.textMuted.name()));
}

void SourceChooserPopup::showEntryMenu(int row, const QPoint& at) {
    if (row < 0 || row >= m_filteredEntries.size()) return;
    const SourceEntry e = m_filteredEntries[row];
    if (e.entryKind == SourceEntry::ProviderAction) {
        showProviderMenu(e.providerIdentifier, at);
        return;
    }
    if (e.entryKind != SourceEntry::SavedSource) return;
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("sourceRowMenu"));
    styleMenu(menu);
    auto* remove = menu->addAction(e.isActive ? QStringLiteral("Disconnect and remove")
                                            : QStringLiteral("Remove from list"));
    connect(remove, &QAction::triggered, this, [this, id = e.identity()] {
        for (const auto& entry : m_allEntries) {
            if (entry.entryKind == SourceEntry::SavedSource && entry.identity() == id) {
                const int index = entry.savedIndex;
                emit removeRequested(index);
                return;
            }
        }
    });
    menu->popup(at);
}

void SourceChooserPopup::showProviderMenu(const QString& id, const QPoint& at) {
    auto* menu = new QMenu(this);
    styleMenu(menu);
    emit populateProviderMenu(id, menu);
    if (menu->isEmpty()) { delete menu; return; }
    menu->popup(at);
}

void SourceChooserPopup::showManagementMenu() {
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("sourceManagementMenu"));
    styleMenu(menu);
    bool hasSaved = false, active = false;
    for (const auto& e : m_allEntries) {
        hasSaved |= e.entryKind == SourceEntry::SavedSource;
        active |= e.isActive;
    }
    auto* clear = menu->addAction(active ? QStringLiteral("Disconnect and clear sources")
                                        : QStringLiteral("Clear saved sources"));
    clear->setEnabled(hasSaved);
    connect(clear, &QAction::triggered, this, [this] { hide(); emit clearRequested(); });
    bool separated = false;
    for (const auto& e : m_allEntries) {
        if (e.entryKind != SourceEntry::ProviderAction) continue;
        auto* sub = new QMenu(e.displayName, menu);
        styleMenu(sub);
        // Submenus are owned by the root menu, not independently on close.
        sub->setAttribute(Qt::WA_DeleteOnClose, false);
        emit populateProviderMenu(e.providerIdentifier, sub);
        if (sub->isEmpty()) { delete sub; continue; }
        if (!separated) { menu->addSeparator(); separated = true; }
        menu->addMenu(sub);
    }
    menu->popup(m_menuButton->mapToGlobal(QPoint(0, m_menuButton->height())));
}

void SourceChooserPopup::focusList() {
    m_listView->setFocus();
    if (!m_listView->currentIndex().isValid()) {
        const int row = nextSelectableRow(0, 1);
        if (row >= 0) m_listView->setCurrentIndex(m_model->index(row));
    }
}

bool SourceChooserPopup::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_footerLabel && event->type() == QEvent::Resize) {
        const QFontMetrics fm(m_footerLabel->font());
        const int available = m_footerLabel->contentsRect().width();
        const QString full = QStringLiteral("Tab actions   \u2191\u2193 sources   Enter select   Esc close");
        const QString compact = QStringLiteral("Tab actions  \u2191\u2193 move  Enter pick  Esc close");
        m_footerLabel->setText(fm.horizontalAdvance(full) <= available ? full
            : fm.elidedText(compact, Qt::ElideRight, available));
    }
    if (obj == m_listView->viewport()) {
        if (event->type() == QEvent::Resize) {
            m_emptyLabel->setGeometry(m_listView->viewport()->rect().adjusted(16, 8, -16, -8));
        } else if (event->type() == QEvent::MouseMove || event->type() == QEvent::Leave) {
            auto* delegate = static_cast<SourceChooserDelegate*>(m_listView->itemDelegate());
            int hover = -1;
            bool clickable = false;
            if (event->type() == QEvent::MouseMove) {
                const QPoint pos = static_cast<QMouseEvent*>(event)->pos();
                const auto index = m_listView->indexAt(pos);
                if (index.isValid()) {
                    const auto& e = m_filteredEntries[index.row()];
                    clickable = e.enabled && e.entryKind != SourceEntry::SectionHeader;
                    if (e.entryKind == SourceEntry::SavedSource && rowMenuRect(m_listView->visualRect(index)).contains(pos))
                        hover = index.row();
                }
            }
            m_listView->viewport()->setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
            if (delegate->hoverMenuRow != hover) {
                delegate->hoverMenuRow = hover;
                m_listView->viewport()->update();
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const auto index = m_listView->indexAt(mouse->pos());
            if (mouse->button() == Qt::LeftButton && index.isValid()
                && m_filteredEntries[index.row()].entryKind == SourceEntry::SavedSource
                && rowMenuRect(m_listView->visualRect(index)).contains(mouse->pos())) {
                showEntryMenu(index.row(), m_listView->viewport()->mapToGlobal(mouse->pos()));
                return true;
            }
        } else if (event->type() == QEvent::ContextMenu) {
            auto* context = static_cast<QContextMenuEvent*>(event);
            const auto index = m_listView->indexAt(context->pos());
            if (index.isValid()) showEntryMenu(index.row(), context->globalPos());
            return true;
        }
    }
    const QString providerId = obj->property("providerId").toString();
    if (!providerId.isEmpty() && event->type() == QEvent::ContextMenu) {
        showProviderMenu(providerId, static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
    }
    if (event->type() != QEvent::KeyPress) return QFrame::eventFilter(obj, event);
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() == Qt::Key_Escape) { hide(); return true; }
    const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
    if (obj == m_filterEdit) {
        if (key->key() == Qt::Key_Down) { focusList(); return true; }
        if (enter) { acceptCurrent(); return true; }
    } else if (!providerId.isEmpty()) {
        if (enter) { activateProvider(providerId); return true; }
        const int index = m_providerButtons.indexOf(qobject_cast<QToolButton*>(obj));
        int next = index;
        if (key->key() == Qt::Key_Right) ++next;
        else if (key->key() == Qt::Key_Left) --next;
        else if (key->key() == Qt::Key_Down) next += 2;
        else if (key->key() == Qt::Key_Up) next -= 2;
        if (next != index) {
            if (next < 0) m_filterEdit->setFocus();
            else if (next >= m_providerButtons.size()) focusList();
            else m_providerButtons[next]->setFocus();
            return true;
        }
    } else if (obj == m_listView) {
        const int row = m_listView->currentIndex().row();
        if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
            const int direction = key->key() == Qt::Key_Up ? -1 : 1;
            const int next = nextSelectableRow(row + direction, direction);
            if (next >= 0) m_listView->setCurrentIndex(m_model->index(next));
            else if (direction < 0) m_filterEdit->setFocus();
            return true;
        }
        if (enter) { acceptCurrent(); return true; }
        if (key->key() == Qt::Key_Menu || (key->key() == Qt::Key_F10 && key->modifiers() == Qt::ShiftModifier)) {
            showEntryMenu(row, m_listView->viewport()->mapToGlobal(m_listView->visualRect(m_model->index(row)).center()));
            return true;
        }
        if (key->key() == Qt::Key_Delete) {
            if (row >= 0 && row < m_filteredEntries.size()) {
                const auto e = m_filteredEntries[row];
                if (e.entryKind == SourceEntry::SavedSource && e.savedIndex >= 0) emit removeRequested(e.savedIndex);
            }
            return true;
        }
    } else if (enter) {
        if (auto* button = qobject_cast<QToolButton*>(obj)) { button->click(); return true; }
    }
    // Typing from either the actions or the list returns to the search field.
    // Ctrl shortcuts and Tab retain their normal focus/clipboard behavior.
    if (obj != m_filterEdit && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        if (key->key() == Qt::Key_Backspace) {
            m_filterEdit->setFocus();
            m_filterEdit->backspace();
            return true;
        }
        if (!key->text().isEmpty() && key->text()[0].isPrint() && key->key() != Qt::Key_Space) {
            m_filterEdit->setFocus();
            m_filterEdit->deselect();
            m_filterEdit->insert(key->text());
            return true;
        }
    }
    return QFrame::eventFilter(obj, event);
}

void SourceChooserPopup::activateProvider(const QString& id) {
    for (const auto& e : m_allEntries) {
        if (e.entryKind == SourceEntry::ProviderAction && e.providerIdentifier == id && e.enabled) {
            const QString selectedId = id;
            hide();
            emit providerSelected(selectedId);
            return;
        }
    }
}

void SourceChooserPopup::acceptCurrent() {
    if (m_listView->currentIndex().isValid()) acceptIndex(m_listView->currentIndex().row());
}

void SourceChooserPopup::acceptIndex(int row) {
    if (row < 0 || row >= m_filteredEntries.size()) return;
    const SourceEntry entry = m_filteredEntries[row];
    if (!entry.enabled || entry.entryKind == SourceEntry::SectionHeader) return;
    if (entry.entryKind == SourceEntry::ProviderAction) {
        activateProvider(entry.providerIdentifier);
        return;
    }
    hide();
    if (!entry.isActive && entry.savedIndex >= 0) emit sourceSelected(entry.savedIndex);
}

int SourceChooserPopup::nextSelectableRow(int from, int direction) const {
    for (int i = from; i >= 0 && i < m_filteredEntries.size(); i += direction)
        if (m_filteredEntries[i].entryKind != SourceEntry::SectionHeader && m_filteredEntries[i].enabled) return i;
    return -1;
}

} // namespace rcx
