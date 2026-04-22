#include "titlebar.h"
#include "themes/thememanager.h"
#include <QButtonGroup>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QTimer>
#include <QWindow>

namespace rcx {

TitleBarWidget::TitleBarWidget(QWidget* parent)
    : QWidget(parent)
    , m_theme(ThemeManager::instance().current())
{
    setFixedHeight(32);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // App name
    m_appLabel = new QLabel(QStringLiteral("Reclass"), this);
    m_appLabel->setContentsMargins(10, 0, 4, 0);
    m_appLabel->setAlignment(Qt::AlignVCenter);
    m_appLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(m_appLabel);

    // Menu bar — hidden on Linux; visible on Windows.
    // On Linux, QMenuBar inside a custom widget collapses all items into an
    // extension popup.  We keep it hidden and mirror its menus as QToolButtons
    // via finalizeMenuBar() after createMenus() populates it.
    m_menuBar = new QMenuBar(this);
    m_menuBar->setNativeMenuBar(false);
#ifdef __linux__
    m_useToolButtons = true;
    m_menuBar->hide();
    m_menuBtnLayout = new QHBoxLayout;
    m_menuBtnLayout->setContentsMargins(0, 0, 0, 0);
    m_menuBtnLayout->setSpacing(0);
    layout->addLayout(m_menuBtnLayout);
#else
    m_menuBar->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
    layout->addWidget(m_menuBar);
#endif

    layout->addStretch();

    // Chrome buttons
    // Workspace mode toggles — two side-by-side checkable buttons, VS Code
    // codicons. "Off" shows the editor-only layout (sidebar hidden),
    // "On" shows the editor-with-sidebar layout (sidebar visible). Mutually
    // exclusive via QButtonGroup so exactly one is always checked.
    m_btnLayoutOff = makeChromeButton(":/vsicons/layout-sidebar-left-off.svg");
    m_btnLayoutOn  = makeChromeButton(":/vsicons/layout-sidebar-left.svg");
    m_btnLayoutOff->setCheckable(true);
    m_btnLayoutOn->setCheckable(true);
    m_btnLayoutOff->setToolTip(QStringLiteral("Editor only"));
    m_btnLayoutOn->setToolTip(QStringLiteral("Editor + workspace"));
    // Tighter than chrome buttons — they come in pairs, share the slot.
    m_btnLayoutOff->setFixedSize(34, 32);
    m_btnLayoutOn->setFixedSize(34, 32);

    auto* layoutGroup = new QButtonGroup(this);
    layoutGroup->setExclusive(true);
    layoutGroup->addButton(m_btnLayoutOff, Layout_NoWorkspace);
    layoutGroup->addButton(m_btnLayoutOn,  Layout_Workspace);
    // Default: off (matches the initial workspace-hidden state at startup).
    m_btnLayoutOff->setChecked(true);

    m_btnMin   = makeChromeButton(":/vsicons/chrome-minimize.svg");
    m_btnMax   = makeChromeButton(":/vsicons/chrome-maximize.svg");
    m_btnClose = makeChromeButton(":/vsicons/chrome-close.svg");

    layout->addWidget(m_btnLayoutOff);
    layout->addWidget(m_btnLayoutOn);
    layout->addWidget(m_btnMin);
    layout->addWidget(m_btnMax);
    layout->addWidget(m_btnClose);

    connect(layoutGroup, &QButtonGroup::idClicked,
            this, [this](int id) { emit layoutPresetSelected(id); });

    connect(m_btnMin, &QToolButton::clicked, this, [this]() {
        window()->showMinimized();
    });
    connect(m_btnMax, &QToolButton::clicked, this, [this]() {
        toggleMaximize();
    });
    connect(m_btnClose, &QToolButton::clicked, this, [this]() {
        window()->close();
    });
}

QToolButton* TitleBarWidget::makeChromeButton(const QString& iconPath) {
    auto* btn = new QToolButton(this);
    btn->setIcon(QIcon(iconPath));
    btn->setIconSize(QSize(16, 16));
    btn->setFixedSize(46, 32);
    btn->setAutoRaise(true);
    btn->setFocusPolicy(Qt::NoFocus);
    return btn;
}

void TitleBarWidget::setWorkspaceChecked(bool on) {
    if (!m_btnLayoutOff || !m_btnLayoutOn) return;
    QToolButton* target = on ? m_btnLayoutOn : m_btnLayoutOff;
    if (target->isChecked()) return;
    // Block signals so we don't re-emit layoutPresetSelected and loop
    // when MainWindow is syncing us from an external visibility change.
    QSignalBlocker bOff(m_btnLayoutOff);
    QSignalBlocker bOn(m_btnLayoutOn);
    target->setChecked(true);
}

void TitleBarWidget::applyTheme(const Theme& theme) {
    m_theme = theme;

    // Title bar background
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, theme.background);
    setPalette(pal);

    // App label
    m_appLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 12px; font-weight: bold; }")
            .arg(theme.text.name()));

    // Menu bar palette — all roles used by MenuBarStyle, so live theme
    // switches don't rely on app-palette inheritance (which can stall
    // once setPalette has been called on a widget).
    {
        QPalette mbPal = m_menuBar->palette();
        mbPal.setColor(QPalette::Window, theme.background);
        mbPal.setColor(QPalette::Button, theme.background);
        mbPal.setColor(QPalette::ButtonText, theme.text);
        mbPal.setColor(QPalette::Text, theme.text);
        mbPal.setColor(QPalette::Highlight, theme.selected);
        mbPal.setColor(QPalette::Link, theme.indHoverSpan);
        mbPal.setColor(QPalette::AlternateBase, theme.surface);
        mbPal.setColor(QPalette::Dark, theme.border);
        mbPal.setColor(QPalette::Mid, theme.hover);
        m_menuBar->setPalette(mbPal);
        m_menuBar->setAutoFillBackground(false);

        // Propagate to existing QMenu children so dropdown popups update too
        for (auto* menu : m_menuBar->findChildren<QMenu*>()) {
            QPalette mp = menu->palette();
            mp.setColor(QPalette::Window, theme.background);
            mp.setColor(QPalette::WindowText, theme.text);
            mp.setColor(QPalette::Text, theme.text);
            mp.setColor(QPalette::Highlight, theme.selected);
            mp.setColor(QPalette::Link, theme.indHoverSpan);
            mp.setColor(QPalette::AlternateBase, theme.surface);
            mp.setColor(QPalette::Dark, theme.border);
            menu->setPalette(mp);
        }
    }

    // Chrome buttons
    QString btnStyle = QStringLiteral(
        "QToolButton { background: transparent; border: none; }"
        "QToolButton:hover { background: %1; }")
        .arg(theme.hover.name());
    m_btnMin->setStyleSheet(btnStyle);
    m_btnMax->setStyleSheet(btnStyle);
    // Workspace toggle pair: checked one gets accent underline + subtle fill
    // so the active mode is obvious at a glance.
    QString layoutBtnStyle = QStringLiteral(
        "QToolButton { background: transparent; border: none;"
        "              border-bottom: 2px solid transparent; }"
        "QToolButton:hover { background: %1; }"
        "QToolButton:checked { background: %2;"
        "                      border-bottom: 2px solid %3; }")
        .arg(theme.hover.name(),
             theme.backgroundAlt.name(),
             theme.indHoverSpan.name());
    m_btnLayoutOff->setStyleSheet(layoutBtnStyle);
    m_btnLayoutOn->setStyleSheet(layoutBtnStyle);

    // Linux menu tool buttons
    if (m_useToolButtons) {
        QString menuBtnStyle = QStringLiteral(
            "QToolButton { background: transparent; border: none; padding: 0 8px; color: %1; }"
            "QToolButton:hover { background: %2; }"
            "QToolButton::menu-indicator { image: none; }")
            .arg(theme.text.name(), theme.hover.name());
        for (auto* btn : m_menuButtons)
            btn->setStyleSheet(menuBtnStyle);
    }

    // Close button: themed red hover
    m_btnClose->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; }"
        "QToolButton:hover { background: %1; }").arg(theme.indHeatHot.name()));

    update();
}

void TitleBarWidget::setShowIcon(bool show) {
    if (show) {
        m_appLabel->setText(QString());
        m_appLabel->setPixmap(QIcon(":/icons/class.png").pixmap(24, 24));
        setFixedHeight(34);
    } else {
        m_appLabel->setPixmap(QPixmap());
        m_appLabel->setText(QStringLiteral("Reclass"));
        m_appLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 12px; font-weight: bold; }")
                .arg(m_theme.text.name()));
        setFixedHeight(32);
    }
}

void TitleBarWidget::setMenuBarTitleCase(bool titleCase) {
    m_titleCase = titleCase;
    for (QAction* action : m_menuBar->actions()) {
        QString text = action->text();
        QString clean = text;
        clean.remove('&');

        if (titleCase) {
            action->setText("&" + clean.toUpper());
        } else {
            QString result;
            bool capitalizeNext = true;
            for (int i = 0; i < clean.length(); ++i) {
                QChar ch = clean[i];
                if (ch.isLetter()) {
                    result += capitalizeNext ? ch.toUpper() : ch.toLower();
                    capitalizeNext = false;
                } else {
                    result += ch;
                    if (ch.isSpace()) capitalizeNext = true;
                }
            }
            action->setText("&" + result);
        }
    }
    // Sync tool button labels on Linux
    if (m_useToolButtons) {
        auto actions = m_menuBar->actions();
        for (int i = 0; i < m_menuButtons.size() && i < actions.size(); ++i)
            m_menuButtons[i]->setText(actions[i]->text());
    }
}

void TitleBarWidget::finalizeMenuBar() {
    if (!m_useToolButtons) return;
    // Create a QToolButton for each top-level menu in the hidden QMenuBar
    for (auto* action : m_menuBar->actions()) {
        if (!action->menu()) continue;
        auto* btn = new QToolButton(this);
        btn->setText(action->text());
        btn->setMenu(action->menu());
        btn->setPopupMode(QToolButton::InstantPopup);
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { background: transparent; border: none; padding: 0 8px; }"
            "QToolButton:hover { background: %1; }"
            "QToolButton::menu-indicator { image: none; }")
            .arg(m_theme.hover.name()));
        btn->installEventFilter(this);
        btn->menu()->installEventFilter(this);
        m_menuBtnLayout->addWidget(btn);
        m_menuButtons.append(btn);
    }
}

bool TitleBarWidget::eventFilter(QObject* obj, QEvent* event) {
    if (!m_useToolButtons) return QWidget::eventFilter(obj, event);

    // Watch for mouse movement inside an open QMenu — if the cursor moves
    // over a sibling menu button, close this menu and open the other.
    if (event->type() == QEvent::MouseMove) {
        auto* menu = qobject_cast<QMenu*>(obj);
        if (!menu || !menu->isVisible()) return false;
        QPoint globalPos = QCursor::pos();
        for (auto* btn : m_menuButtons) {
            if (btn->menu() == menu) continue;
            QRect btnRect(btn->mapToGlobal(QPoint(0, 0)), btn->size());
            if (btnRect.contains(globalPos)) {
                menu->close();
                QTimer::singleShot(0, btn, [btn]() { btn->showMenu(); });
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void TitleBarWidget::updateMaximizeIcon() {
    if (window()->isMaximized())
        m_btnMax->setIcon(QIcon(":/vsicons/chrome-restore.svg"));
    else
        m_btnMax->setIcon(QIcon(":/vsicons/chrome-maximize.svg"));
}

void TitleBarWidget::toggleMaximize() {
    if (window()->isMaximized())
        window()->showNormal();
    else
        window()->showMaximized();
    updateMaximizeIcon();
}

void TitleBarWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        window()->windowHandle()->startSystemMove();
        event->accept();
    } else {
        QWidget::mousePressEvent(event);
    }
}

void TitleBarWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        toggleMaximize();
        event->accept();
    } else {
        QWidget::mouseDoubleClickEvent(event);
    }
}

void TitleBarWidget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);

    // 1px bottom border
    QPainter p(this);
    p.setPen(m_theme.border);
    p.drawLine(0, height() - 1, width() - 1, height() - 1);
}

} // namespace rcx
