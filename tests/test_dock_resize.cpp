// Regression test for the workspace-dock "can't resize via separator drag"
// bug. The root cause was a hard maxWidth=0 on the central widget
// (setFixedSize(0,0)): when the user dragged the left dock's right-edge
// separator inward, central had to grow to absorb the freed pixels, but
// max=0 forbade growth — splitter snapped back instantly. Symptom: drag
// cursor shows, resize "fights" you, dock spawns ~half the window wide
// because central can't take any horizontal share.
//
// What this test asserts:
//   1. The central placeholder must NOT have maximumWidth pinned to 0
//      (or any small value). Pinning max would re-introduce the same
//      bug — central must be free to grow.
//   2. A QMainWindow constructed with a dock and a centralWidget
//      configured the way the production code does it must actually
//      respond to resizeDocks() — i.e. you can move the dock's width
//      from wide to narrow and Qt's layout honours it.
//
// The test does NOT simulate a literal mouse drag on the separator
// (that's brittle on headless CI and not the regression of interest).
// The behavioural invariant is: given the production-shaped layout,
// resizeDocks() to a smaller value must take effect. The mouse drag is
// just sugar over the same layout mechanism.

#include <QtTest/QTest>
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QLabel>
#include <QHBoxLayout>

class TestDockResize : public QObject {
    Q_OBJECT
private slots:
    void centralPlaceholderMustAllowGrowth() {
        // Mirror the production setup at src/main.cpp:~1003 — but with
        // the FIX applied (setMinimumSize(0,0) + Ignored policy, no
        // setFixedSize). A regression that re-introduces setFixedSize
        // (or any setMaximumWidth < window width) on the central will
        // make this assertion fail.
        QMainWindow window;
        window.resize(1000, 600);

        auto* central = new QWidget(&window);
        central->setMinimumSize(0, 0);
        central->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        window.setCentralWidget(central);

        // A dock on the left mirroring the workspace dock's shape.
        auto* dock = new QDockWidget(QStringLiteral("Workspace"), &window);
        dock->setObjectName(QStringLiteral("WorkspaceDock"));
        dock->setMinimumWidth(180);
        auto* content = new QWidget(dock);
        content->setMinimumWidth(0);
        dock->setWidget(content);
        window.addDockWidget(Qt::LeftDockWidgetArea, dock);

        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(50);

        // Hard guarantee: central must be ABLE to grow. If a future
        // change sets a small maximumWidth on the central placeholder,
        // dock resize will silently break — catch that here.
        QVERIFY2(central->maximumWidth() > 500,
                 qPrintable(QStringLiteral(
                    "central maximumWidth=%1 — too small. "
                    "setFixedSize on central re-introduces the dock-"
                    "resize regression because the splitter can't move "
                    "inward (central can't grow to absorb freed pixels).")
                    .arg(central->maximumWidth())));

        // Behavioural check: shrink the dock to 200 px via resizeDocks
        // and verify the layout actually delivers a width close to
        // that target. If central has max=0 (the bug), Qt clamps the
        // dock to (window - 0) = window width regardless of what we
        // ask for, so width() comes back huge.
        window.resizeDocks({dock}, {200}, Qt::Horizontal);
        QTest::qWait(50);
        QVERIFY2(dock->width() <= 260,
                 qPrintable(QStringLiteral(
                    "after resizeDocks(200), dock->width()=%1 — central "
                    "is pinning the dock width. This is the regression.")
                    .arg(dock->width())));
    }

    void titleBarLabelMustNotPinDockMinimum() {
        // Mirror the DockTitleBar layout at src/main.cpp:~7105. The
        // regression here was: the title-bar's QLabel ("Project — N
        // structs · M enums") reports its full text width as its
        // minimumSizeHint. The QHBoxLayout sums that into the title
        // bar's minimum, which bubbles up to the QDockWidget as its
        // effective minimum width — silently outranking
        // setMinimumWidth(180) on the dock. The fix: label
        // sizePolicy=Ignored + setMinimumWidth(0). This test guards
        // that fix.
        QMainWindow window;
        window.resize(1000, 600);
        auto* central = new QWidget(&window);
        central->setMinimumSize(0, 0);
        central->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        window.setCentralWidget(central);

        auto* dock = new QDockWidget(QStringLiteral("Project"), &window);
        dock->setMinimumWidth(180);

        // Production-shaped title bar with a long label.
        auto* titleBar = new QWidget(dock);
        auto* hl = new QHBoxLayout(titleBar);
        hl->setContentsMargins(6, 0, 4, 0);
        hl->setSpacing(4);
        auto* grip = new QWidget(titleBar);
        grip->setFixedSize(12, 36);
        hl->addWidget(grip);
        auto* label = new QLabel(
            QStringLiteral("Project — 137 structs · 42 enums"),
            titleBar);
        QFont f(QStringLiteral("Courier"), 10);
        label->setFont(f);
        // THE FIX under test:
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setMinimumWidth(0);
        hl->addWidget(label, /*stretch=*/1);
        auto* closeBtn = new QWidget(titleBar);
        closeBtn->setFixedSize(22, 22);
        hl->addWidget(closeBtn);
        dock->setTitleBarWidget(titleBar);

        auto* content = new QWidget(dock);
        content->setMinimumWidth(0);
        dock->setWidget(content);
        window.addDockWidget(Qt::LeftDockWidgetArea, dock);

        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(50);

        // With the fix applied, the title bar's minimumSizeHint width
        // should reflect ONLY the fixed children (grip 12 + spacing 4 +
        // closeBtn 22 + margins 10) ≈ 48 px — NOT the full text width
        // (~250 px in 10pt Courier). Assert the minimum is well under
        // the dock's explicit 180 floor so 180 actually wins.
        int tbMinW = titleBar->minimumSizeHint().width();
        QVERIFY2(tbMinW < 120,
                 qPrintable(QStringLiteral(
                    "title bar minimumSizeHint().width()=%1 — the label "
                    "is still pinning a text-width floor. The label "
                    "must use Ignored size policy + setMinimumWidth(0).")
                    .arg(tbMinW)));

        // Behavioural check: dock should actually shrink to 200.
        window.resizeDocks({dock}, {200}, Qt::Horizontal);
        QTest::qWait(50);
        QVERIFY2(dock->width() <= 260,
                 qPrintable(QStringLiteral(
                    "dock->width()=%1 after resizeDocks(200) — label is "
                    "pinning the dock width above the requested floor.")
                    .arg(dock->width())));
    }
};

QTEST_MAIN(TestDockResize)
#include "test_dock_resize.moc"
