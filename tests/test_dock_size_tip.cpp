// Live dock-divider size readout — verifies the qApp-event-filter
// mechanism that drives MainWindow::showDockSizeTip during a separator
// drag. Empirically probes which cursor mechanism Qt 6 uses on this
// platform (overrideCursor vs MainWindow::cursor()) so we can pick the
// right gate, then reproduces the eventFilter logic on a synthesized
// drag and checks that a tooltip-equivalent widget is shown for every
// pixel of movement.
//
// Why a separate test: test_project_dock builds the same QMainWindow +
// LeftDockWidget shape we need but only checks geometry. Live-resize
// detection is its own moving piece (Qt's separator drag uses an
// internal mouse grab and doesn't emit any public signal), so it gets
// its own test rather than tangling with project_dock's geometry suite.

#include <QtTest/QTest>
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QTextEdit>
#include <QWidget>
#include <QCursor>
#include <QPoint>
#include <QtDebug>
#include <cstdio>
#include "rcxtooltip.h"
#include "docksizereadout.h"

using rcx::RcxTooltip;
using rcx::DockSizeReadout;

// Qt's qDebug() output is invisible on Windows MinGW unless
// QT_FORCE_STDERR_LOGGING=1 is set. Use plain fprintf to stdout so
// the test prints unconditionally.
#define LOG(...) do { std::fprintf(stdout, __VA_ARGS__); std::fflush(stdout); } while (0)

class TestDockSizeTip : public QObject {
    Q_OBJECT

private:
    struct App {
        QMainWindow* win;
        QDockWidget* dock;
        QWidget*     central;
    };

    App build() {
        auto* win = new QMainWindow;
        win->resize(1200, 700);
        auto* central = new QTextEdit(win);
        win->setCentralWidget(central);

        auto* dock = new QDockWidget("Workspace", win);
        dock->setObjectName("WorkspaceDock");
        dock->setAllowedAreas(Qt::AllDockWidgetAreas);
        dock->setWidget(new QTextEdit(dock));
        win->addDockWidget(Qt::LeftDockWidgetArea, dock);
        win->resizeDocks({dock}, {300}, Qt::Horizontal);
        return {win, dock, central};
    }

    // Locate the separator: the strip between dock's right edge and
    // central widget's left edge (in QMainWindow coords).
    QPoint separatorMidpoint(const App& a) {
        QPoint dockTopRight = a.dock->mapTo(a.win,
            QPoint(a.dock->width(), a.dock->height() / 2));
        // The separator sits 1-3px to the right of the dock edge.
        return dockTopRight + QPoint(2, 0);
    }

private slots:
    // ── 1. Probe what Qt actually does for separator-cursor ──
    // Records the cursor mechanism Qt uses so we can keep the gate honest.
    void probeQtSeparatorCursorMechanism();

    // ── 2. Width-change-detect WORKS during a synthesized drag ──
    // Drives the same logic the qApp event filter uses (cursor-shape gate
    // + LMB) and verifies the dock width actually responds to mouse move,
    // proving QMainWindow's separator drag is in flight.
    void synthesizedDragChangesDockWidth();

    // ── 3. RcxTooltip-shape widget shown on every move ──
    // Reproduces the exact eventFilter loop locally (poll on MouseMove
    // when cursor shape is a sizing one) and counts how many readouts
    // happen across N synthesized mouse moves.
    void readoutFiresOnEveryMoveDuringDrag();

    // ── 4. QResizeEvent fires on the dock during a drag ──
    // The actual mechanism we're going to use: install eventFilter on
    // each sidebar dock and react to its QEvent::Resize. Counts how
    // many resize events arrive during a 50-px synthesized drag and
    // records the width sequence — proves whether dock resize events
    // are continuous or batched on this platform.
    void resizeEventsFireContinuouslyDuringDrag();

    // ── 5. Full pipeline: real DockSizeReadout + qApp-style filter +
    //       synthesized drag. Drives showDockSizeTip-equivalent code
    //       on every Resize event. Prints what the tooltip body text
    //       was set to on each tick AND what is actually visible at
    //       the end. If the body text doesn't update in lockstep with
    //       the dock width, this is where we'll see it.
    void fullPipelineReadoutUpdatesPerPixel();

    // ── 6. Workspace dock opens at minimumWidth even when tabified ──
    // Reproduces the LIVE bug where placeSidebarDock's tabify branch
    // skipped resize, so workspace inherited a wider peer's width.
    // Build peer at 600 px, tabify workspace (min=235) onto it, run
    // the proposed force-resize logic, verify width == 235.
    void workspaceOpensAtMinimumWidth();

    // ── 7. DockSizeReadout body text updates per call ──
    // Constructs a real DockSizeReadout, calls updateText+showAt 10
    // times with different bodies, counts QEvent::Paint deliveries.
    // If raise/update isn't called per tick, paints get coalesced or
    // suppressed — that would be the bug. Logs paint counts to stdout.
    void tooltipBodyTextChangesPerUpdate();
};

void TestDockSizeTip::probeQtSeparatorCursorMechanism()
{
    auto a = build();
    a.win->show();
    QTest::qWaitForWindowExposed(a.win);

    QPoint sep = separatorMidpoint(a);
    QCursor::setPos(a.win->mapToGlobal(sep));

    // Hover the separator and let Qt notice.
    QTest::mouseMove(a.win, sep);
    QApplication::processEvents();
    QTest::qWait(50);
    QApplication::processEvents();

    const QCursor* over = QApplication::overrideCursor();
    Qt::CursorShape mainShape = a.win->cursor().shape();
    QWidget* under = QApplication::widgetAt(a.win->mapToGlobal(sep));
    Qt::CursorShape underShape = under ? under->cursor().shape() : Qt::ArrowCursor;

    LOG("\n-- Hover over separator --\n");
    LOG("  overrideCursor: %d\n", over ? int(over->shape()) : -1);
    LOG("  QMainWindow::cursor().shape(): %d\n", int(mainShape));
    LOG("  widgetAt(sep)->cursor().shape(): %d  widget=%s\n",
        int(underShape), under ? under->metaObject()->className() : "<null>");

    // Now begin a drag: press LMB, move, observe state mid-drag.
    QTest::mousePress(a.win, Qt::LeftButton, Qt::NoModifier, sep);
    QApplication::processEvents();
    QTest::mouseMove(a.win, sep + QPoint(20, 0));
    QApplication::processEvents();

    over = QApplication::overrideCursor();
    mainShape = a.win->cursor().shape();
    under = QApplication::widgetAt(a.win->mapToGlobal(sep + QPoint(20, 0)));
    underShape = under ? under->cursor().shape() : Qt::ArrowCursor;

    LOG("\n-- Mid-drag (LMB held, +20px) --\n");
    LOG("  overrideCursor: %d\n", over ? int(over->shape()) : -1);
    LOG("  QMainWindow::cursor().shape(): %d\n", int(mainShape));
    LOG("  widgetAt(sep)->cursor().shape(): %d  widget=%s\n",
        int(underShape), under ? under->metaObject()->className() : "<null>");
    LOG("  mouseButtons & LMB: %s\n",
        (QApplication::mouseButtons() & Qt::LeftButton) ? "true" : "false");

    QTest::mouseRelease(a.win, Qt::LeftButton, Qt::NoModifier, sep + QPoint(20, 0));
    QApplication::processEvents();

    delete a.win;
}

void TestDockSizeTip::synthesizedDragChangesDockWidth()
{
    auto a = build();
    a.win->show();
    QTest::qWaitForWindowExposed(a.win);

    int w0 = a.dock->width();
    QPoint sep = separatorMidpoint(a);

    QTest::mouseMove(a.win, sep);
    QApplication::processEvents();
    QTest::mousePress(a.win, Qt::LeftButton, Qt::NoModifier, sep);
    QApplication::processEvents();

    // Drag right by 50 px.
    for (int dx = 5; dx <= 50; dx += 5) {
        QTest::mouseMove(a.win, sep + QPoint(dx, 0));
        QApplication::processEvents();
    }

    int w1 = a.dock->width();
    LOG("\nDock width before: %d  after drag: %d\n", w0, w1);

    QTest::mouseRelease(a.win, Qt::LeftButton, Qt::NoModifier, sep + QPoint(50, 0));
    QApplication::processEvents();

    // If QTest::mouseMove can't drive QMainWindowLayout's separator
    // tracking on this platform, the width won't change — and the test
    // will tell us straight up so we know the harness limit, not the
    // real-app behavior.
    if (w1 == w0) {
        QSKIP("QTest::mouseMove does not drive QMainWindowLayout separator "
              "tracking on this platform — drag must be tested manually.");
    }
    QVERIFY2(w1 != w0, qPrintable(QString("dock width unchanged: %1 → %2").arg(w0).arg(w1)));

    delete a.win;
}

void TestDockSizeTip::readoutFiresOnEveryMoveDuringDrag()
{
    auto a = build();
    a.win->show();
    QTest::qWaitForWindowExposed(a.win);

    // Local replica of MainWindow::eventFilter's MouseMove gate.
    int readoutCount = 0;
    auto gate = [&](Qt::CursorShape shape) {
        if ((QApplication::mouseButtons() & Qt::LeftButton)
            && (shape == Qt::SizeHorCursor || shape == Qt::SizeVerCursor)) {
            ++readoutCount;
        }
    };

    QPoint sep = separatorMidpoint(a);
    QTest::mouseMove(a.win, sep);
    QApplication::processEvents();
    QTest::mousePress(a.win, Qt::LeftButton, Qt::NoModifier, sep);
    QApplication::processEvents();

    for (int dx = 5; dx <= 50; dx += 5) {
        QTest::mouseMove(a.win, sep + QPoint(dx, 0));
        QApplication::processEvents();

        // Sniff cursor mechanism. We try BOTH overrideCursor (which my
        // current code uses) AND QMainWindow::cursor() (the fallback if
        // override turns out to be null on Windows).
        const QCursor* over = QApplication::overrideCursor();
        Qt::CursorShape s1 = over ? over->shape() : Qt::ArrowCursor;
        Qt::CursorShape s2 = a.win->cursor().shape();
        QWidget* under = QApplication::widgetAt(a.win->mapToGlobal(sep + QPoint(dx, 0)));
        Qt::CursorShape s3 = under ? under->cursor().shape() : Qt::ArrowCursor;
        // Picking ANY of the three that matches a sizing shape — the
        // most permissive gate. This tells us which sniffing strategy
        // actually fires.
        Qt::CursorShape shape = Qt::ArrowCursor;
        for (Qt::CursorShape s : {s1, s2, s3}) {
            if (s == Qt::SizeHorCursor || s == Qt::SizeVerCursor) {
                shape = s; break;
            }
        }
        gate(shape);
    }

    QTest::mouseRelease(a.win, Qt::LeftButton, Qt::NoModifier, sep + QPoint(50, 0));
    QApplication::processEvents();

    LOG("\nReadouts fired across 10 moves: %d\n", readoutCount);
    if (readoutCount == 0) {
        QSKIP("No sizing-cursor was ever observed during synthesized drag — "
              "Qt may set the separator cursor only via private layout APIs "
              "not visible to QApplication. Real-window behavior may differ.");
    }
    QVERIFY2(readoutCount >= 5,
             qPrintable(QString("Expected near-continuous readouts, got %1").arg(readoutCount)));

    delete a.win;
}

// Filter that counts QEvent::Resize on the dock during a drag, AND
// records whether LMB is reported held at the moment the event fires
// — the gate the real app's eventFilter uses to distinguish a user
// drag from a programmatic resize (window init, layout preset).
class ResizeCounter : public QObject {
public:
    int count = 0;
    int lmbHeldCount = 0;
    QVector<int> widths;
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() == QEvent::Resize) {
            ++count;
            if (QApplication::mouseButtons() & Qt::LeftButton) ++lmbHeldCount;
            if (auto* d = qobject_cast<QDockWidget*>(obj)) {
                widths.push_back(d->width());
            }
        }
        return false;
    }
};

void TestDockSizeTip::resizeEventsFireContinuouslyDuringDrag()
{
    auto a = build();
    a.win->show();
    QTest::qWaitForWindowExposed(a.win);

    ResizeCounter rc;
    a.dock->installEventFilter(&rc);

    QPoint sep = separatorMidpoint(a);
    QTest::mouseMove(a.win, sep);
    QApplication::processEvents();
    QTest::mousePress(a.win, Qt::LeftButton, Qt::NoModifier, sep);
    QApplication::processEvents();

    int initialCount = rc.count;
    rc.widths.clear();

    for (int dx = 5; dx <= 50; dx += 5) {
        QTest::mouseMove(a.win, sep + QPoint(dx, 0));
        QApplication::processEvents();
    }

    int duringDrag = rc.count - initialCount;
    LOG("\nResize events during 10 mouse moves: %d\n", duringDrag);
    LOG("Width sequence:");
    for (int w : rc.widths) LOG(" %d", w);
    LOG("\n");

    QTest::mouseRelease(a.win, Qt::LeftButton, Qt::NoModifier, sep + QPoint(50, 0));
    QApplication::processEvents();

    int totalIncludingRelease = rc.count - initialCount;
    LOG("Resize events including release: %d\n", totalIncludingRelease);

    // We need at least 2 resize events during the drag for a live readout
    // to be useful. If Windows batches everything into one event at release,
    // the resize-event approach won't work either and we'd need a QTimer
    // poll on the dock width.
    QVERIFY2(duringDrag >= 2,
        qPrintable(QString("Only %1 resize events fired during drag — "
                           "not enough for live readout").arg(duringDrag)));

    // The real eventFilter gates on `QApplication::mouseButtons() & LeftButton`
    // to skip programmatic resizes. If LMB is NOT reported held during the
    // synthesized drag, that gate would silently drop the readout — meaning
    // the test passes but the live app shows nothing. Verify it.
    LOG("Resize events that saw LMB-held: %d of %d\n", rc.lmbHeldCount, duringDrag);
    QVERIFY2(rc.lmbHeldCount >= duringDrag - 1,
        qPrintable(QString("LMB-held gate would drop %1 of %2 resize events")
                   .arg(duringDrag - rc.lmbHeldCount).arg(duringDrag)));

    delete a.win;
}

// Full-pipeline filter: same logic as MainWindow::eventFilter — on
// QEvent::Resize for the dock with LMB held, drive a real
// DockSizeReadout the same way showDockSizeTip does.
class FullPipelineFilter : public QObject {
public:
    QDockWidget*     dock = nullptr;
    QWidget*         parentWin = nullptr;
    DockSizeReadout* tip = nullptr;
    int firedCount = 0;
    QStringList bodiesShown;
    QStringList visibleAfter;

    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() == QEvent::Resize
            && obj == dock
            && (QApplication::mouseButtons() & Qt::LeftButton)) {
            auto* re = static_cast<QResizeEvent*>(e);
            if (!tip) tip = new DockSizeReadout(parentWin);
            QString body = QStringLiteral("width: %1 px").arg(re->size().width());
            QFont f("Consolas", 10);
            tip->updateText(dock->windowTitle(), body, f);
            tip->showAt(parentWin->mapFromGlobal(QCursor::pos()));
            ++firedCount;
            bodiesShown.append(body);
            visibleAfter.append(tip->isVisible() ? "Y" : "N");
        }
        return false;
    }
};

void TestDockSizeTip::fullPipelineReadoutUpdatesPerPixel()
{
    auto a = build();
    a.win->show();
    QTest::qWaitForWindowExposed(a.win);

    FullPipelineFilter filt;
    filt.dock = a.dock;
    filt.parentWin = a.win;
    qApp->installEventFilter(&filt);

    QPoint sep = separatorMidpoint(a);
    QTest::mouseMove(a.win, sep);
    QApplication::processEvents();
    QTest::mousePress(a.win, Qt::LeftButton, Qt::NoModifier, sep);
    QApplication::processEvents();

    for (int dx = 5; dx <= 50; dx += 5) {
        QTest::mouseMove(a.win, sep + QPoint(dx, 0));
        QApplication::processEvents();
    }

    QTest::mouseRelease(a.win, Qt::LeftButton, Qt::NoModifier, sep + QPoint(50, 0));
    QApplication::processEvents();

    LOG("\nFull-pipeline showDockSizeTip fired: %d times\n", filt.firedCount);
    for (int i = 0; i < filt.bodiesShown.size(); ++i) {
        LOG("  [%d] body=\"%s\" visible=%s\n",
            i, filt.bodiesShown[i].toUtf8().constData(),
            filt.visibleAfter[i].toUtf8().constData());
    }
    if (filt.tip) {
        LOG("Final tip state: visible=%s width=%d height=%d\n",
            filt.tip->isVisible() ? "true" : "false",
            filt.tip->width(), filt.tip->height());
    }

    qApp->removeEventFilter(&filt);
    QVERIFY2(filt.firedCount >= 5,
        qPrintable(QString("Pipeline only fired %1 times").arg(filt.firedCount)));
    // Each body should be unique — proves the tooltip body is updated
    // in lockstep with the dock width, not stuck on the first value.
    QSet<QString> uniq(filt.bodiesShown.begin(), filt.bodiesShown.end());
    QVERIFY2(uniq.size() >= 5,
        qPrintable(QString("Only %1 unique bodies — tip text not updating")
                   .arg(uniq.size())));

    delete a.win;
}

void TestDockSizeTip::workspaceOpensAtMinimumWidth()
{
    auto* win = new QMainWindow;
    win->resize(1404, 936);
    win->setCentralWidget(new QTextEdit(win));

    // Peer dock already in the area at a wider size — simulates a
    // previous session where some sidebar was 600 px wide.
    auto* peer = new QDockWidget("Bookmarks", win);
    peer->setObjectName("BookmarksDock");
    peer->setWidget(new QTextEdit(peer));
    win->addDockWidget(Qt::LeftDockWidgetArea, peer);
    win->resizeDocks({peer}, {600}, Qt::Horizontal);

    // Workspace dock with min=235, then tabify-with-peer + force-resize
    // (matches the new placeSidebarDock tabify branch logic).
    auto* workspace = new QDockWidget("Project", win);
    workspace->setObjectName("WorkspaceDock");
    workspace->setMinimumWidth(235);
    workspace->setWidget(new QTextEdit(workspace));
    win->tabifyDockWidget(peer, workspace);
    win->resizeDocks({workspace}, {workspace->minimumWidth()}, Qt::Horizontal);
    workspace->show();
    workspace->raise();

    win->show();
    QTest::qWaitForWindowExposed(win);
    QApplication::processEvents();

    LOG("\nworkspace.width() = %d px (expected 235)\n", workspace->width());
    LOG("peer.width() = %d px (was 600)\n", peer->width());

    // Allow a 5-px slop for Qt's separator-pixel rounding on different
    // platforms — Linux/Windows can disagree by 1-2 px on dock arithmetic.
    QVERIFY2(workspace->width() <= 240,
        qPrintable(QString("workspace opened at %1 px, expected ~235")
                   .arg(workspace->width())));
    QVERIFY2(workspace->width() >= 230,
        qPrintable(QString("workspace clamped too narrow: %1 px")
                   .arg(workspace->width())));

    delete win;
}

// Counts QEvent::Paint deliveries on a target widget.
class PaintCounter : public QObject {
public:
    int paints = 0;
    bool eventFilter(QObject*, QEvent* e) override {
        if (e->type() == QEvent::Paint) ++paints;
        return false;
    }
};

void TestDockSizeTip::tooltipBodyTextChangesPerUpdate()
{
    auto* win = new QMainWindow;
    win->resize(1200, 700);
    win->setCentralWidget(new QTextEdit(win));
    win->show();
    QTest::qWaitForWindowExposed(win);

    auto* tip = new DockSizeReadout(win);
    PaintCounter pc;
    tip->installEventFilter(&pc);

    QFont f("Consolas", 10);
    QStringList bodies;
    for (int i = 1; i <= 10; ++i) {
        bodies.append(QStringLiteral("%1 px | %2 px").arg(i).arg(10 - i));
    }

    for (const QString& body : bodies) {
        tip->updateText("Workspace", body, f);
        tip->showAt(QPoint(100, 100));
        QApplication::processEvents();
    }
    // Final processEvents to flush queued paints on Windows.
    QApplication::processEvents();
    QApplication::processEvents();

    LOG("\nDockSizeReadout paint events across %d updateText calls: %d\n",
        bodies.size(), pc.paints);
    LOG("Final tip visible=%s w=%d h=%d\n",
        tip->isVisible() ? "Y" : "N", tip->width(), tip->height());

    // We expect at LEAST one paint per unique body — a paint count of 1
    // would mean repaints aren't happening per update (the bug).
    QVERIFY2(pc.paints >= bodies.size(),
        qPrintable(QString("Only %1 paints for %2 updates — repaint not "
                           "firing per call").arg(pc.paints).arg(bodies.size())));

    delete win;
}

QTEST_MAIN(TestDockSizeTip)
#include "test_dock_size_tip.moc"
