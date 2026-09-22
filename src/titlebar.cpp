#include "titlebar.h"
#include "paintutil.h"
#include "svgicon.h"
#include "themes/thememanager.h"
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QTimer>
#include <QWindow>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QVariantAnimation>
#include <QSignalBlocker>

namespace rcx {

static const QString kAppWordmark = QStringLiteral("RC");

TitleBarWidget::TitleBarWidget(QWidget* parent)
    : QWidget(parent)
    , m_theme(ThemeManager::instance().current())
{
    setFixedHeight(32);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // App name
    m_appLabel = new QLabel(kAppWordmark, this);
    // The stylesheet owns the left gutter (a QSS-styled QLabel ignores
    // contentsMargins) — two competing margins put the brand 14 px in.
    m_appLabel->setContentsMargins(0, 0, 0, 0);
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

    // Quick-access slot (filled by setQuickActions once RibbonActions exists).
    m_quickLayout = new QHBoxLayout;
    m_quickLayout->setContentsMargins(0, 0, 0, 0);
    m_quickLayout->setSpacing(0);
    layout->addLayout(m_quickLayout);

    layout->addStretch();
    m_themeLayout = new QHBoxLayout;
    m_themeLayout->setContentsMargins(0, 0, 0, 0);
    m_themeLayout->setSpacing(0);
    layout->addLayout(m_themeLayout);

    // Chrome buttons. (The workspace show/hide toggle pair that used to sit
    // left of these is gone — the Project dock's close button + the
    // collapsed-rail handle replaced it.)
    m_btnMin   = makeChromeButton(":/vsicons/chrome-minimize.svg");
    m_btnMax   = makeChromeButton(":/vsicons/chrome-maximize.svg");
    m_btnClose = makeChromeButton(":/vsicons/chrome-close.svg");

    layout->addWidget(m_btnMin);
    layout->addWidget(m_btnMax);
    layout->addWidget(m_btnClose);

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

namespace {

// The brand is chrome, so it is the chrome type size (10 pt DemiBold), not a
// hand-set 12 PIXEL bold that ignored the app font and grew/shrank with the
// DPI independently of the menus beside it. `kGutter` is the same left edge
// the ribbon, address bar and status bar start their ink on.
QString appLabelSheet(const QColor& text) {
    return QStringLiteral("QLabel { color: %1; font-family: 'JetBrains Mono'; "
                          "font-size: 10pt; font-weight: 600; padding-left: %2px; }")
        .arg(text.name()).arg(kGutter);
}

// The 16-px hairline between the menus and the quick-access pair. Its own
// widget so it can paint a DEVICE-exact column (a 1-logical-px frame is two
// device rows at 125 %), and transparent to the mouse so the title strip's
// window-drag still works across it.
class QuickAccessRule : public QWidget {
public:
    explicit QuickAccessRule(QWidget* parent) : QWidget(parent) {
        setFixedWidth(1);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }
    void setColour(const QColor& c) { m_colour = c; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!m_colour.isValid()) return;
        QPainter p(this);
        const int h = 16, y = (height() - h) / 2;
        fillLeftDeviceColOfRect(p, QRectF(0, y, 1, h), m_colour);
    }

private:
    QColor m_colour;
};

class ThemeSwitch : public QAbstractButton {
public:
    explicit ThemeSwitch(QWidget* parent) : QAbstractButton(parent) {
        setObjectName(QStringLiteral("themeSwitch"));
        setCheckable(true);
        setFixedSize(31, 32);
        setFocusPolicy(Qt::TabFocus);
        setMouseTracking(true);
        setAccessibleName(tr("Dark theme"));
        updateDescription();
        m_animation.setDuration(100);
        m_animation.setEasingCurve(QEasingCurve::OutCubic);
        connect(&m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            m_darkMix = v.toReal();
            update();
        });
        connect(this, &QAbstractButton::toggled, this, [this](bool dark) {
            updateDescription();
            m_animation.stop();
            if (isVisible()) {
                m_animation.setStartValue(m_darkMix);
                m_animation.setEndValue(dark ? 1.0 : 0.0);
                m_animation.start();
            } else {
                m_darkMix = dark ? 1.0 : 0.0;
                update();
            }
        });
    }

    void applyTheme(const Theme& theme) {
        m_theme = theme;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        if (!isEnabled()) p.setOpacity(0.45);
        if (isEnabled() && (underMouse() || isDown()))
            p.fillRect(rect(), isDown() ? m_theme.selected : m_theme.hover);
        if (hasFocus() && m_keyboardFocus)
            fillDeviceFrameOfRect(p, QRectF((width() - 20) / 2, 6, 20, 20), m_theme.textDim);

        // The icon's 14-unit circle occupies 12 px inside this 14-px cell.
        const QRect iconRect((width() - 14) / 2, (height() - 14) / 2, 14, 14);
        const qreal opacity = p.opacity();
        for (int i = 0; i < 2; ++i) {
            p.setOpacity(opacity * (i == 0 ? 1.0 - m_darkMix : m_darkMix));
            themedVsIcon(QStringLiteral(":/vsicons/color-mode.svg"), m_theme.text,
                14, devicePixelRatioF(), i != 0).paint(&p, iconRect);
        }
    }

    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) return true;
        if (e->type() == QEvent::Enter || e->type() == QEvent::Leave) update();
        return QAbstractButton::event(e);
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override {
        mousePressEvent(e);
        e->accept();
    }
    void mousePressEvent(QMouseEvent* e) override {
        m_keyboardFocus = false;
        update();
        QAbstractButton::mousePressEvent(e);
    }
    void focusInEvent(QFocusEvent* e) override {
        m_keyboardFocus = e->reason() == Qt::TabFocusReason
            || e->reason() == Qt::BacktabFocusReason || e->reason() == Qt::ShortcutFocusReason;
        QAbstractButton::focusInEvent(e);
        update();
    }
    void focusOutEvent(QFocusEvent* e) override {
        m_keyboardFocus = false;
        QAbstractButton::focusOutEvent(e);
        update();
    }
    void keyPressEvent(QKeyEvent* e) override {
        m_keyboardFocus = true;
        update();
        if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) {
            setChecked(e->key() == Qt::Key_Right);
            e->accept();
            return;
        }
        QAbstractButton::keyPressEvent(e);
    }

private:
    Theme m_theme;
    QVariantAnimation m_animation;
    qreal m_darkMix = 0;
    bool m_keyboardFocus = false;
    void updateDescription() {
        setAccessibleDescription(isChecked() ? tr("Dark theme") : tr("Light theme"));
    }
};

}  // namespace

QToolButton* TitleBarWidget::makeChromeButton(const QString& iconPath) {
    auto* btn = new QToolButton(this);
    btn->setIcon(QIcon(iconPath));
    btn->setIconSize(QSize(16, 16));
    btn->setFixedSize(46, 32);
    btn->setAutoRaise(true);
    btn->setFocusPolicy(Qt::NoFocus);
    return btn;
}

// Undo / Redo used to be a two-button ribbon panel with an empty third row —
// a whole panel, a caption and a divider spent on two arrows. They belong in
// the title strip, next to the menus that also carry them: always reachable,
// never taking a panel's worth of the working strip.
void TitleBarWidget::setQuickActions(QAction* undo, QAction* redo) {
    if (!m_quickLayout || m_btnUndo) return;   // built once
    m_quickLayout->addSpacing(10);
    m_quickRule = new QuickAccessRule(this);
    m_quickLayout->addWidget(m_quickRule);
    m_quickLayout->addSpacing(6);

    auto makeQuick = [this](QAction* a) -> QToolButton* {
        auto* btn = new QToolButton(this);
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setFixedSize(28, 32);
        btn->setIconSize(QSize(16, 16));
        // Everything visible (icon, tooltip, enabled) comes from the action:
        // the strip can never disagree with Edit ▸ Undo. The button consumes
        // its own press, so the title bar's startSystemMove never fires here.
        if (a) btn->setDefaultAction(a);
        m_quickLayout->addWidget(btn);
        return btn;
    };
    m_btnUndo = makeQuick(undo);
    m_btnRedo = makeQuick(redo);
    m_themeSwitch = new ThemeSwitch(this);
    m_themeLayout->addWidget(m_themeSwitch, 0, Qt::AlignVCenter);
    m_themeLayout->addSpacing(8);
    connect(m_themeSwitch, &QAbstractButton::toggled,
            this, &TitleBarWidget::darkThemeRequested);
    applyTheme(m_theme);   // pick up the chrome button sheet
}

void TitleBarWidget::setDarkTheme(bool enabled) {
    const QSignalBlocker blocker(this);
    if (m_themeSwitch) m_themeSwitch->setChecked(enabled);
}

QToolButton* TitleBarWidget::quickButton(int index) const {
    return index == 0 ? m_btnUndo : index == 1 ? m_btnRedo : nullptr;
}

void TitleBarWidget::applyTheme(const Theme& theme) {
    m_theme = theme;

    // Title bar background — menuBarColor (slightly darker than the editor
    // paper) so the menu strip reads as chrome, not part of the document.
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, menuBarColor(theme));
    setPalette(pal);

    // App label. padding-left in the stylesheet (not setContentsMargins, which
    // a QSS-styled QLabel ignores) so "RC" isn't jammed into the corner.
    m_appLabel->setStyleSheet(appLabelSheet(theme.text));

    // Menu bar palette — all roles used by MenuBarStyle, so live theme
    // switches don't rely on app-palette inheritance (which can stall
    // once setPalette has been called on a widget).
    {
        QPalette mbPal = m_menuBar->palette();
        mbPal.setColor(QPalette::Window, menuBarColor(theme));
        mbPal.setColor(QPalette::Button, menuBarColor(theme));
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

    // Undo and Redo share a horizontal return arrow, mirrored for Redo.
    if (m_btnUndo || m_btnRedo) {
        const qreal qdpr = devicePixelRatioF();
        const QIcon undoIcon(rcx::themedVsIcon(QStringLiteral(":/vsicons/reply.svg"),
                                                theme.text, 16, qdpr));
        const QIcon redoIcon(rcx::themedVsIcon(QStringLiteral(":/vsicons/reply.svg"),
                                                theme.text, 16, qdpr, true));
        if (m_btnUndo) {
            m_btnUndo->setStyleSheet(btnStyle);
            if (QAction* a = m_btnUndo->defaultAction()) a->setIcon(undoIcon);
        }
        if (m_btnRedo) {
            m_btnRedo->setStyleSheet(btnStyle);
            if (QAction* a = m_btnRedo->defaultAction()) a->setIcon(redoIcon);
        }
    }
    // static_cast: m_quickRule is only ever set to a QuickAccessRule (this
    // file is the only writer), and a Q_OBJECT in a .cpp would need its own
    // moc include for qobject_cast.
    if (m_quickRule) static_cast<QuickAccessRule*>(m_quickRule)->setColour(theme.border);
    if (m_themeSwitch) {
        static_cast<ThemeSwitch*>(m_themeSwitch)->applyTheme(theme);
        setDarkTheme(theme.background.lightnessF() < 0.5);
    }

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

    // Close button: themed red hover. Uses markerPtr (the conventional
    // warning red, same hue every desktop OS paints on the X). Earlier
    // rev used indHeatHot which is the AMBER heatmap token meant for
    // "this value changes frequently" — wrong semantic for a destructive
    // close affordance and visibly orange against the dark chrome.
    QColor closeWarn = theme.markerPtr.isValid() ? theme.markerPtr : theme.indHeatHot;
    m_btnClose->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; }"
        "QToolButton:hover { background: %1; }").arg(closeWarn.name()));

    // Re-tint all chrome SVG icons to theme.text so light-theme chrome
    // (gray) still shows the controls. Source SVGs hard-fill #C5C5C5
    // which was invisible on the new XP Luna gray. Pass our dpr so the
    // glyphs render crisp on HiDPI rather than upscaled from 32x32 logical.
    const qreal dpr = devicePixelRatioF();
    if (m_btnMin)
        m_btnMin->setIcon(themedVsIcon(":/vsicons/chrome-minimize.svg", theme.text, 32, dpr));
    if (m_btnMax)
        m_btnMax->setIcon(themedVsIcon(":/vsicons/chrome-maximize.svg", theme.text, 32, dpr));
    if (m_btnClose)
        m_btnClose->setIcon(themedVsIcon(":/vsicons/chrome-close.svg", theme.text, 32, dpr));

    update();
}

void TitleBarWidget::setShowIcon(bool show) {
    if (show) {
        m_appLabel->setText(QString());
        m_appLabel->setPixmap(QIcon(":/icons/class.png").pixmap(24, 24));
        setFixedHeight(34);
    } else {
        m_appLabel->setPixmap(QPixmap());
        m_appLabel->setText(kAppWordmark);
        m_appLabel->setStyleSheet(appLabelSheet(m_theme.text));
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
    // Theme-tint like the other chrome icons (applyTheme re-tints max/restore
    // too, but maximize<->restore toggles here independently of theme changes);
    // a plain QIcon keeps the baked #C5C5C5 ink, invisible on the light theme.
    const qreal dpr = devicePixelRatioF();
    const char* path = window()->isMaximized()
        ? ":/vsicons/chrome-restore.svg" : ":/vsicons/chrome-maximize.svg";
    m_btnMax->setIcon(themedVsIcon(QString::fromLatin1(path), m_theme.text, 32, dpr));
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

    // Bottom hairline: exactly ONE device row. drawLine on a 1-logical-px
    // pen covers 1.25 device px at 125 % and snaps to two rows — a double
    // line next to the ribbon's device-exact one.
    QPainter p(this);
    fillBottomDeviceRowOfRect(p, QRectF(0, 0, width(), height()), m_theme.border);
}

} // namespace rcx
