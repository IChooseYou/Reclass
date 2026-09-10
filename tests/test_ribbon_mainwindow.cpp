// test_ribbon_mainwindow — the ribbon hosted the way MainWindow hosts it:
// a chrome-less QToolBar ("RibbonHost") in the top toolbar area of a
// frameless QMainWindow whose menu widget is the custom TitleBarWidget.
//
// Pins the three integration facts the app relies on:
//   1. the ribbon sits directly under the title bar (no Fusion margin rows)
//      and the dock area starts directly under the ribbon;
//   2. the ribbon fills the window width (QToolBar must stretch it, never
//      shove it into the » extension popup);
//   3. DockOverlay's drop area starts below the ribbon, follows minimise,
//      and reverts to the title bar when the host is hidden.
//
// Runs on the hidden desktop: no qWaitForWindowExposed (it never succeeds
// there) — show() + qWait + processEvents is enough for geometry.
#include <QtTest/QTest>
#include <QApplication>
#include <QDockWidget>
#include <QFile>
#include <QJsonDocument>
#include <QMainWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QToolBar>
#include <QToolButton>

#include "dockoverlay.h"
#include "ribbon.h"
#include "themes/theme.h"
#include "themes/thememanager.h"
#include "titlebar.h"

using namespace rcx;

static Theme loadTheme(const QString& baseName) {
    QFile f(QStringLiteral(RCX_SOURCE_DIR) + QStringLiteral("/src/themes/defaults/") + baseName
            + QStringLiteral(".json"));
    if (!f.open(QIODevice::ReadOnly)) return Theme();
    return Theme::fromJson(QJsonDocument::fromJson(f.readAll()).object());
}

class TestRibbonMainWindow : public QObject {
    Q_OBJECT

    struct Fixture {
        QMainWindow*    win = nullptr;
        TitleBarWidget* titleBar = nullptr;
        RibbonBar*      ribbon = nullptr;
        QToolBar*       host = nullptr;
        QDockWidget*    dock = nullptr;
    };

    // Mirrors MainWindow::createRibbon() + the applyTheme QSS rule.
    Fixture build(int width, const Theme& theme) {
        Fixture f;
        f.win = new QMainWindow;
        f.win->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);
        f.win->resize(width, 700);

        f.titleBar = new TitleBarWidget(f.win);
        f.titleBar->applyTheme(theme);
        f.win->setMenuWidget(f.titleBar);

        f.ribbon = new RibbonBar(f.win);
        f.ribbon->applyTheme(theme);
        f.host = new QToolBar(QStringLiteral("Ribbon"), f.win);
        f.host->setObjectName(QStringLiteral("RibbonHost"));
        f.host->setMovable(false);
        f.host->setFloatable(false);
        f.host->setAllowedAreas(Qt::TopToolBarArea);
        f.host->setContextMenuPolicy(Qt::PreventContextMenu);
        f.host->setFocusPolicy(Qt::NoFocus);
        f.host->addWidget(f.ribbon);
        f.win->addToolBar(Qt::TopToolBarArea, f.host);
        f.win->setStyleSheet(QStringLiteral(
            "QToolBar#RibbonHost { border: none; margin: 0px; padding: 0px; "
            "spacing: 0px; background: transparent; }"));

        f.dock = new QDockWidget(QStringLiteral("Doc"), f.win);
        auto* emptyTitle = new QWidget(f.dock);
        emptyTitle->setFixedHeight(0);
        f.dock->setTitleBarWidget(emptyTitle);
        auto* body = new QWidget(f.dock);
        body->setMinimumHeight(200);
        f.dock->setWidget(body);
        f.win->addDockWidget(Qt::TopDockWidgetArea, f.dock);

        f.win->show();
        QTest::qWait(150);
        QApplication::processEvents();
        return f;
    }

    static QRect inWindow(QWidget* w, QMainWindow* win) {
        return QRect(w->mapTo(win, QPoint(0, 0)), w->size());
    }

private slots:
    void testRibbonSitsBetweenTitleBarAndDocks() {
        const Theme theme = loadTheme(QStringLiteral("vs"));
        QVERIFY2(theme.background.isValid(), "vs.json not found under RCX_SOURCE_DIR");
        Fixture f = build(1350, theme);

        const QRect tb = inWindow(f.titleBar, f.win);
        const QRect hr = inWindow(f.host, f.win);
        const QRect rr = inWindow(f.ribbon, f.win);
        const QRect dr = inWindow(f.dock, f.win);

        QVERIFY2(hr.top() == tb.bottom() + 1,
                 qPrintable(QString("host top %1 != title bar bottom+1 %2 (Fusion margin rows?)")
                            .arg(hr.top()).arg(tb.bottom() + 1)));
        QVERIFY2(rr.top() == hr.top() && rr.left() == hr.left(),
                 qPrintable(QString("ribbon %1,%2 not flush with host %3,%4 (toolbar padding)")
                            .arg(rr.left()).arg(rr.top()).arg(hr.left()).arg(hr.top())));
        QCOMPARE(f.ribbon->height(), f.ribbon->preferredHeight());
        QCOMPARE(f.host->height(), f.ribbon->height());
        QVERIFY2(dr.top() == hr.bottom() + 1,
                 qPrintable(QString("dock top %1 != host bottom+1 %2")
                            .arg(dr.top()).arg(hr.bottom() + 1)));
        QCOMPARE(f.ribbon->focusPolicy(), Qt::NoFocus);
        QCOMPARE(f.host->focusPolicy(), Qt::NoFocus);
        delete f.win;
    }

    // QToolBar must stretch the ribbon to the toolbar width — otherwise it
    // sizes it to sizeHint(), compacts/overflows panels with free space to
    // the right, and shows its own » extension button.
    void testRibbonFillsWindowWidth() {
        const Theme theme = loadTheme(QStringLiteral("vs"));
        Fixture f = build(1350, theme);
        const int hostW = f.host->width();
        QVERIFY2(f.ribbon->width() >= hostW - 2,
                 qPrintable(QString("ribbon width %1 < host width %2 - QToolBar did not stretch it")
                            .arg(f.ribbon->width()).arg(hostW)));
        QVERIFY2(f.host->width() >= f.win->width() - 2,
                 qPrintable(QString("host width %1 < window width %2")
                            .arg(f.host->width()).arg(f.win->width())));
        // The extension button only appears when items don't fit.
        for (auto* child : f.host->findChildren<QWidget*>()) {
            if (child->objectName() == QLatin1String("qt_toolbar_ext_button"))
                QVERIFY2(!child->isVisible(), "QToolBar extension button visible - ribbon overflowed the toolbar");
        }
        // Home at 1350 fits without hiding panels once the ribbon has the full width.
        f.ribbon->setCurrentTab(QStringLiteral("home"));
        QApplication::processEvents();
        QVERIFY2(f.ribbon->overflowedPanelIds().isEmpty(),
                 qPrintable(QString("Home overflowed at width %1: %2")
                            .arg(f.ribbon->width()).arg(f.ribbon->overflowedPanelIds().join(','))));
        // Growing the window re-lays the ribbon out at the new width.
        f.win->resize(1800, 700);
        QTest::qWait(50);
        QApplication::processEvents();
        QVERIFY2(f.ribbon->width() >= f.win->width() - 2,
                 qPrintable(QString("ribbon width %1 after resize to %2")
                            .arg(f.ribbon->width()).arg(f.win->width())));
        delete f.win;
    }

    void testDockOverlayContentRectSkipsRibbon() {
        const Theme theme = loadTheme(QStringLiteral("vs"));
        Fixture f = build(1350, theme);
        auto* overlay = new DockOverlay(f.win);   // owned by the window (deleted with it)
        overlay->setGeometry(f.win->rect());

        const QRect hr = inWindow(f.host, f.win);
        QCOMPARE(overlay->dropContentRect().top(), hr.bottom() + 1);

        // Minimising the ribbon (tab row only) moves the boundary up.
        const int fullH = f.host->height();
        f.ribbon->setMinimized(true);
        QTest::qWait(50);
        QApplication::processEvents();
        QVERIFY2(f.host->height() < fullH,
                 qPrintable(QString("host height %1 not below %2 after minimise")
                            .arg(f.host->height()).arg(fullH)));
        QCOMPARE(overlay->dropContentRect().top(), inWindow(f.host, f.win).bottom() + 1);

        // Hiding the host (View > Ribbon = Hidden) hands the row back to the docks.
        f.host->hide();
        QTest::qWait(50);
        QApplication::processEvents();
        QCOMPARE(overlay->dropContentRect().top(), inWindow(f.titleBar, f.win).bottom() + 1);
        delete f.win;
    }

    // P0 #9: ONE persisted key. The old pair (`showRibbon` from the View menu,
    // `ribbonMinimized` from the tab double-click) could disagree — hidden but
    // "expanded", collapsed but with no menu entry saying so. The migration
    // folds both into `ribbonState` once and deletes them.
    void testRibbonStateMigratesTheTwoLegacyKeys() {
        struct Case { bool shown; bool mini; int want; const char* what; };
        const Case cases[] = {
            {true,  false, rcx::RibbonFull,      "shown + expanded -> Full"},
            {true,  true,  rcx::RibbonCollapsed, "shown + minimized -> Collapsed"},
            {false, false, rcx::RibbonHidden,    "hidden -> Hidden"},
            {false, true,  rcx::RibbonHidden,    "hidden wins over minimized"},
        };
        for (const Case& c : cases) {
            QSettings s(QStringLiteral("REECLASS-test"), QStringLiteral("ribbon-migration"));
            s.clear();
            s.setValue(QStringLiteral("showRibbon"), c.shown);
            s.setValue(QStringLiteral("ribbonMinimized"), c.mini);
            QCOMPARE(rcx::ribbonStateFromSettings(s), c.want);
            QVERIFY2(!s.contains(QStringLiteral("showRibbon")), c.what);
            QVERIFY2(!s.contains(QStringLiteral("ribbonMinimized")), c.what);
            QCOMPARE(s.value(QStringLiteral("ribbonState")).toInt(), c.want);
            // Second read is a plain read — it must not re-migrate.
            s.setValue(QStringLiteral("ribbonState"), int(rcx::RibbonCollapsed));
            QCOMPARE(rcx::ribbonStateFromSettings(s), int(rcx::RibbonCollapsed));
            s.clear();
        }
        // Nothing stored at all = Full; a junk value falls back to Full.
        QSettings s(QStringLiteral("REECLASS-test"), QStringLiteral("ribbon-migration"));
        s.clear();
        QCOMPARE(rcx::ribbonStateFromSettings(s), int(rcx::RibbonFull));
        s.setValue(QStringLiteral("ribbonState"), 99);
        QCOMPARE(rcx::ribbonStateFromSettings(s), int(rcx::RibbonFull));
        s.clear();
    }

    // P1 #16: Undo / Redo are title-strip quick access. The buttons are views
    // of the RibbonActions QActions, take no focus, and — the reason they can
    // live on the drag strip at all — consume their own press so the title
    // bar never starts a system move under them.
    void testTitleStripQuickAccess() {
        const Theme theme = loadTheme(QStringLiteral("vs"));
        TitleBarWidget bar;
        bar.applyTheme(theme);
        QCOMPARE(bar.quickButton(0), nullptr);

        QAction undo(QStringLiteral("Undo")), redo(QStringLiteral("Redo"));
        undo.setToolTip(QStringLiteral("Undo (Ctrl+Z)"));
        redo.setToolTip(QStringLiteral("Redo (Ctrl+Y)"));
        undo.setEnabled(false);
        bar.setQuickActions(&undo, &redo);
        QToolButton* bu = bar.quickButton(0);
        QToolButton* br = bar.quickButton(1);
        QVERIFY(bu && br);
        QCOMPARE(bu->defaultAction(), &undo);
        QCOMPARE(br->defaultAction(), &redo);
        QCOMPARE(bu->focusPolicy(), Qt::NoFocus);
        QCOMPARE(br->focusPolicy(), Qt::NoFocus);
        QCOMPARE(bu->size(), QSize(28, 32));
        QCOMPARE(bu->toolTip(), QStringLiteral("Undo (Ctrl+Z)"));
        QCOMPARE(br->toolTip(), QStringLiteral("Redo (Ctrl+Y)"));
        // Enabled state follows the action, not the button.
        QVERIFY(!bu->isEnabled());
        undo.setEnabled(true);
        QVERIFY(bu->isEnabled());
        // Both carry an icon, and Redo's is the mirror of Undo's, not a
        // different arrow (they used to be arrow-left / arrow-right).
        QVERIFY(!undo.icon().isNull());
        QVERIFY(!redo.icon().isNull());
        const QImage a = undo.icon().pixmap(16, 16).toImage()
                             .convertToFormat(QImage::Format_ARGB32);
        const QImage b = redo.icon().pixmap(16, 16).toImage()
                             .convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(a.mirrored(true, false), b);
        // The press lands on the button, not on the title bar's drag handler.
        QSignalSpy spy(&redo, &QAction::triggered);
        QTest::mouseClick(br, Qt::LeftButton, Qt::NoModifier, br->rect().center());
        QCOMPARE(spy.count(), 1);
        // Building twice is a no-op (applyTheme re-runs on every theme switch).
        bar.setQuickActions(&undo, &redo);
        QCOMPARE(bar.quickButton(0), bu);
    }

    void testThemeSwitchMouseKeyboardAndAnimation() {
        TitleBarWidget bar;
        QAction undo(QStringLiteral("Undo")), redo(QStringLiteral("Redo"));
        bar.setQuickActions(&undo, &redo);
        const Theme light = loadTheme(QStringLiteral("tw"));
        bar.applyTheme(light);
        bar.resize(800, bar.height());
        bar.show();
        QTest::qWait(30);
        auto* toggle = bar.themeSwitch();
        QVERIFY(toggle);
        QVERIFY(toggle->isCheckable());
        QVERIFY(!toggle->isChecked());
        QCOMPARE(toggle->focusPolicy(), Qt::TabFocus);
        QVERIFY(toggle->x() >= bar.quickButton(1)->geometry().right() + 8);
        auto* separator = bar.findChild<QWidget*>(QStringLiteral("themeSwitchSeparator"));
        QVERIFY(!separator);
        QCOMPARE(toggle->geometry().right(), bar.width() - 3 * 46 - 8 - 1);
        QCOMPARE(toggle->size(), QSize(31, 32));
        const QRect bounds = toggle->geometry();
        const int centerX = toggle->width() / 2;
        const int focusX = (toggle->width() - 20) / 2;
        const QImage before = toggle->grab().toImage();
        const qreal dpr = before.devicePixelRatio();
        auto at = [dpr](const QImage& img, int x, int y) {
            return img.pixelColor(qRound(x * dpr), qRound(y * dpr));
        };
        // Initial window activation must not create an accent box.
        QCOMPARE(at(before, 4, 6).rgba(), menuBarColor(light).rgba());
        QCOMPARE(at(before, 1, 1).rgba(), menuBarColor(light).rgba());
        QVERIFY(at(before, centerX - 4, 16) != at(before, centerX + 4, 16));
        QSignalSpy changes(&bar, &TitleBarWidget::darkThemeRequested);
        const QPoint right(toggle->width() - 8, toggle->height() / 2);
        QTest::mouseClick(toggle, Qt::LeftButton, Qt::NoModifier, right);
        QVERIFY(toggle->isChecked());
        QCOMPARE(changes.count(), 1);
        QTest::qWait(40);
        const QImage moving = toggle->grab().toImage();
        QTest::qWait(110);
        const QImage after = toggle->grab().toImage();
        QVERIFY(before != after);
        QVERIFY(moving != after);
        QVERIFY(at(before, centerX - 4, 16) != at(after, centerX - 4, 16));
        QVERIFY(at(before, centerX + 4, 16) != at(after, centerX + 4, 16));
        QCOMPARE(toggle->geometry(), bounds);
        QTest::mouseClick(toggle, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
        QCOMPARE(changes.count(), 2);
        QVERIFY(!toggle->isChecked());
        QTest::keyClick(toggle, Qt::Key_Left);
        QVERIFY(!toggle->isChecked());
        toggle->setFocus(Qt::TabFocusReason);
        QVERIFY(toggle->hasFocus());
        QCOMPARE(at(toggle->grab().toImage(), focusX, 6), light.textDim);
        QTest::mouseClick(toggle, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
        QVERIFY(at(toggle->grab().toImage(), focusX, 6) != light.textDim);
        bar.setDarkTheme(false);
        QTest::keyClick(toggle, Qt::Key_Space);
        QVERIFY(toggle->isChecked());
        const int requested = changes.count();
        bar.setDarkTheme(false);  // reverse the transition while it is running
        QTest::qWait(150);
        QVERIFY(!toggle->isChecked());
        QCOMPARE(toggle->geometry(), bounds);
        QVERIFY(!toggle->accessibleName().isEmpty());
        QCOMPARE(toggle->accessibleDescription(), QStringLiteral("Light theme"));
        QCOMPARE(changes.count(), requested);  // theme feedback must not request another change
        bar.applyTheme(loadTheme(QStringLiteral("vs")));
        QVERIFY(toggle->isChecked());
        QCOMPARE(changes.count(), requested);
        QVERIFY(toggle->toolTip().isEmpty());
    }

    void testThemeButtonHoverFillsItsCell() {
        TitleBarWidget bar;
        QAction undo(QStringLiteral("Undo")), redo(QStringLiteral("Redo"));
        bar.setQuickActions(&undo, &redo);
        const Theme theme = loadTheme(QStringLiteral("vs"));
        bar.applyTheme(theme);
        bar.resize(800, bar.height());
        bar.show();
        QTest::qWait(30);
        auto* toggle = bar.themeSwitch();
        QCOMPARE(toggle->size(), QSize(31, 32));
        toggle->setAttribute(Qt::WA_UnderMouse, true);
        const QImage hover = toggle->grab().toImage();
        for (const QPoint p : {QPoint(0, 0), QPoint(hover.width() - 1, 0),
                QPoint(0, hover.height() - 1), QPoint(hover.width() - 1, hover.height() - 1)})
            QCOMPARE(hover.pixelColor(p).rgba(), theme.hover.rgba());
        QVERIFY(toggle->toolTip().isEmpty());
        QEvent tooltip(QEvent::ToolTip);
        QVERIFY(QApplication::sendEvent(toggle, &tooltip));
        hover.save(QStringLiteral("theme_button_hover.png"));
    }

    void testThemeSwitchChangesThemeAndRemembersLastDarkTheme() {
        auto& tm = ThemeManager::instance();
        const int original = tm.currentIndex();
        QSettings settings(QStringLiteral("REECLASS"), QStringLiteral("REECLASS"));
        QMap<QString, QVariant> saved;
        for (const auto* key : {"theme", "darkTheme", "lightTheme"})
            saved.insert(QString::fromLatin1(key), settings.value(QString::fromLatin1(key)));
        const auto restore = qScopeGuard([&]() {
            tm.setCurrent(original);
            for (auto it = saved.cbegin(); it != saved.cend(); ++it) {
                if (it.value().isValid()) settings.setValue(it.key(), it.value());
                else settings.remove(it.key());
            }
        });
        int dark = -1, alternateDark = -1, light = -1;
        const auto all = tm.themes();
        for (int i = 0; i < all.size(); ++i) {
            if (all[i].isDark()) {
                if (dark < 0) dark = i;
                else alternateDark = i;
            } else light = i;
        }
        QVERIFY(dark >= 0 && alternateDark >= 0 && light >= 0);
        tm.setCurrent(light);
        tm.setCurrent(dark);
        TitleBarWidget bar;
        QAction undo(QStringLiteral("Undo")), redo(QStringLiteral("Redo"));
        bar.setQuickActions(&undo, &redo);
        bar.applyTheme(tm.current());
        connect(&bar, &TitleBarWidget::darkThemeRequested, &tm, &ThemeManager::setDarkMode);
        connect(&tm, &ThemeManager::themeChanged, &bar, &TitleBarWidget::applyTheme);
        auto click = [&]() {
            QTest::mouseClick(bar.themeSwitch(), Qt::LeftButton, Qt::NoModifier,
                bar.themeSwitch()->rect().center());
        };
        click();
        QCOMPARE(tm.currentIndex(), light);
        QVERIFY(!bar.themeSwitch()->isChecked());
        click();
        QCOMPARE(tm.currentIndex(), dark);
        tm.setCurrent(alternateDark);  // selecting another theme in View updates the pair
        QVERIFY(bar.themeSwitch()->isChecked());
        click();
        QCOMPARE(tm.currentIndex(), light);
        click();
        QCOMPARE(tm.currentIndex(), alternateDark);
        QSignalSpy changes(&tm, &ThemeManager::themeChanged);
        tm.setDarkMode(true);
        QCOMPARE(changes.count(), 0);
        settings.setValue(QStringLiteral("lightTheme"), QStringLiteral("Removed theme"));
        click();
        QVERIFY(!tm.current().isDark());
        QCOMPARE(settings.value(QStringLiteral("theme")).toString(), tm.current().name);
    }
};

QTEST_MAIN(TestRibbonMainWindow)
#include "test_ribbon_mainwindow.moc"
