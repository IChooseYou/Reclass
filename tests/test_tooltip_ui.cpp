// Integration test: simulates the full tooltip flow as DarkApp would see it.
// Posts QHelpEvent (ToolTip), sends Leave events, verifies RcxTooltip behavior
// with fprintf at every stage so we can see exactly what happens.

#include <QtTest>
#include <QApplication>
#include <QPushButton>
#include <QHelpEvent>
#include <QScreen>
#include <QImage>
#include "rcxtooltip.h"
#include "themes/thememanager.h"
#include <cstdio>

using namespace rcx;

static void LOG(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

// Simulates what DarkApp::notify does when a ToolTip event arrives
static bool simulateDarkAppToolTip(QWidget* w) {
    QString tip = w->toolTip();
    LOG("  [darkapp] widget='%s' class=%s tip='%s'\n",
        qPrintable(w->objectName()), w->metaObject()->className(),
        qPrintable(tip));
    if (!tip.isEmpty()) {
        LOG("  [darkapp] calling RcxTooltip::showFor\n");
        RcxTooltip::instance()->showFor(w, tip);
        LOG("  [darkapp] showFor returned, visible=%d opacity=%.2f pos=(%d,%d) size=%dx%d\n",
            RcxTooltip::instance()->isVisible(),
            RcxTooltip::instance()->windowOpacity(),
            RcxTooltip::instance()->x(), RcxTooltip::instance()->y(),
            RcxTooltip::instance()->width(), RcxTooltip::instance()->height());
        return true;
    }
    return false;
}

// Simulates what DarkApp::notify does when a Leave event arrives
static void simulateDarkAppLeave(QWidget* w) {
    auto* tip = RcxTooltip::instance();
    if (tip->isVisible() && tip->currentTrigger() == w) {
        LOG("  [darkapp] Leave on trigger — calling scheduleDismiss\n");
        tip->scheduleDismiss();
        LOG("  [darkapp] after scheduleDismiss: visible=%d\n", tip->isVisible());
    } else {
        LOG("  [darkapp] Leave ignored (visible=%d trigger_match=%d)\n",
            tip->isVisible(), tip->currentTrigger() == w);
    }
}

class TestTooltipUI : public QObject {
    Q_OBJECT

private:
    QWidget*     m_window = nullptr;
    QPushButton* m_btn    = nullptr;
    QPushButton* m_btn2   = nullptr;

private slots:
    void initTestCase() {
        LOG("=== TestTooltipUI starting ===\n");

        m_window = new QWidget;
        m_window->setFixedSize(400, 300);
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        m_window->move(avail.center() - QPoint(200, 150));

        m_btn = new QPushButton("Scan", m_window);
        m_btn->setToolTip("Start scanning memory");
        m_btn->setFixedSize(80, 28);
        m_btn->move(160, 140);
        m_btn->setObjectName("btnScan");

        m_btn2 = new QPushButton("Copy", m_window);
        m_btn2->setToolTip("Copy address to clipboard");
        m_btn2->setFixedSize(80, 28);
        m_btn2->move(260, 140);
        m_btn2->setObjectName("btnCopy");

        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        LOG("  window shown at (%d,%d)\n", m_window->x(), m_window->y());
    }

    void cleanupTestCase() {
        RcxTooltip::instance()->dismiss();
        delete m_window;
        LOG("=== TestTooltipUI finished ===\n");
    }

    void cleanup() {
        RcxTooltip::instance()->dismiss();
        QCoreApplication::processEvents();
    }

    // ─── Test 1: Full tooltip lifecycle with event simulation ───
    void testFullLifecycle() {
        LOG("\n--- testFullLifecycle ---\n");
        auto* tip = RcxTooltip::instance();

        // Step 1: Post a ToolTip event (what Qt does after hover delay)
        LOG("Step 1: Posting ToolTip event to btn\n");
        QPoint btnCenter = m_btn->mapToGlobal(QPoint(40, 14));
        LOG("  btn global center: (%d,%d)\n", btnCenter.x(), btnCenter.y());

        // Move real cursor to button center
        QCursor::setPos(btnCenter);
        QCoreApplication::processEvents();
        LOG("  cursor moved to button\n");

        // Simulate what DarkApp does on ToolTip event
        bool handled = simulateDarkAppToolTip(m_btn);
        QVERIFY2(handled, "DarkApp should have handled the tooltip");

        // Process events (paint, animation start)
        QCoreApplication::processEvents();
        QTest::qWait(100);  // let fade-in animation run
        QCoreApplication::processEvents();

        LOG("Step 2: Check tooltip state after 100ms\n");
        LOG("  visible=%d opacity=%.2f text='%s'\n",
            tip->isVisible(), tip->windowOpacity(),
            qPrintable(tip->currentText()));
        LOG("  pos=(%d,%d) size=%dx%d\n",
            tip->x(), tip->y(), tip->width(), tip->height());
        LOG("  arrowDown=%d arrowX=%d bodyRect=(%d,%d %dx%d)\n",
            tip->arrowPointsDown(), tip->arrowLocalX(),
            tip->bodyRect().x(), tip->bodyRect().y(),
            tip->bodyRect().width(), tip->bodyRect().height());

        QVERIFY2(tip->isVisible(), "Tooltip should be visible after showFor + 100ms");
        QCOMPARE(tip->currentText(), QString("Start scanning memory"));

        // Step 3: Grab pixels and verify rendering
        LOG("Step 3: Verify rendering\n");
        tip->setWindowOpacity(1.0);
        QCoreApplication::processEvents();

        QImage img = tip->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        LOG("  grabbed image: %dx%d format=%d\n", img.width(), img.height(), img.format());

        int opaquePixels = 0;
        QRect body = tip->bodyRect().adjusted(2, 2, -2, -2);
        for (int y = body.top(); y <= body.bottom(); ++y)
            for (int x = body.left(); x <= body.right(); ++x)
                if (qAlpha(img.pixel(x, y)) > 0)
                    ++opaquePixels;
        int totalPixels = body.width() * body.height();
        LOG("  body opaque pixels: %d / %d (%.1f%%)\n",
            opaquePixels, totalPixels,
            totalPixels > 0 ? 100.0 * opaquePixels / totalPixels : 0.0);

        QVERIFY2(opaquePixels > totalPixels / 2,
                 qPrintable(QStringLiteral("Only %1/%2 opaque pixels in body — tooltip not rendering")
                     .arg(opaquePixels).arg(totalPixels)));

        // Step 4: Simulate Leave event (spurious — cursor still on button)
        LOG("Step 4: Simulate spurious Leave (cursor still on button)\n");
        simulateDarkAppLeave(m_btn);
        QTest::qWait(200);
        QCoreApplication::processEvents();
        LOG("  after 200ms: visible=%d\n", tip->isVisible());

        QVERIFY2(tip->isVisible(),
                 "Tooltip dismissed by spurious Leave — geometry check failed");

        // Step 5: Move cursor away and simulate real Leave
        LOG("Step 5: Move cursor away, simulate real Leave\n");
        QScreen* scr = QApplication::primaryScreen();
        QPoint farAway = scr->availableGeometry().bottomRight() - QPoint(50, 50);
        QCursor::setPos(farAway);
        QCoreApplication::processEvents();
        LOG("  cursor at (%d,%d)\n", farAway.x(), farAway.y());

        simulateDarkAppLeave(m_btn);
        QTest::qWait(200);
        QCoreApplication::processEvents();
        LOG("  after 200ms: visible=%d\n", tip->isVisible());

        QVERIFY2(!tip->isVisible(),
                 "Tooltip should be dismissed when cursor truly left the zone");

        // Step 6: Re-show on different widget
        LOG("Step 6: Re-show on different widget\n");
        QPoint btn2Center = m_btn2->mapToGlobal(QPoint(40, 14));
        QCursor::setPos(btn2Center);
        QCoreApplication::processEvents();

        handled = simulateDarkAppToolTip(m_btn2);
        QVERIFY(handled);
        QCoreApplication::processEvents();
        QTest::qWait(100);
        QCoreApplication::processEvents();

        LOG("  visible=%d text='%s'\n", tip->isVisible(), qPrintable(tip->currentText()));
        QVERIFY(tip->isVisible());
        QCOMPARE(tip->currentText(), QString("Copy address to clipboard"));

        LOG("--- testFullLifecycle PASSED ---\n");
    }

    // ─── Test 2: Rapid widget switching (no dismiss between) ───
    void testRapidSwitch() {
        LOG("\n--- testRapidSwitch ---\n");
        auto* tip = RcxTooltip::instance();

        QCursor::setPos(m_btn->mapToGlobal(QPoint(40, 14)));
        QCoreApplication::processEvents();
        simulateDarkAppToolTip(m_btn);
        QCoreApplication::processEvents();
        QTest::qWait(50);

        LOG("  switch to btn2 immediately\n");
        QCursor::setPos(m_btn2->mapToGlobal(QPoint(40, 14)));
        QCoreApplication::processEvents();
        simulateDarkAppToolTip(m_btn2);
        QCoreApplication::processEvents();
        QTest::qWait(100);
        QCoreApplication::processEvents();

        LOG("  visible=%d text='%s'\n", tip->isVisible(), qPrintable(tip->currentText()));
        QVERIFY(tip->isVisible());
        QCOMPARE(tip->currentText(), QString("Copy address to clipboard"));
        LOG("--- testRapidSwitch PASSED ---\n");
    }

    // ─── Test 3: Widget with no tooltip ───
    void testNoTooltipWidget() {
        LOG("\n--- testNoTooltipWidget ---\n");
        QPushButton noTip("NoTip", m_window);
        noTip.setFixedSize(80, 28);
        noTip.move(50, 50);
        noTip.show();
        // No setToolTip called

        auto* tip = RcxTooltip::instance();
        bool handled = simulateDarkAppToolTip(&noTip);
        LOG("  handled=%d visible=%d\n", handled, tip->isVisible());
        QVERIFY(!handled);
        QVERIFY(!tip->isVisible());
        LOG("--- testNoTooltipWidget PASSED ---\n");
    }
};

QTEST_MAIN(TestTooltipUI)
#include "test_tooltip_ui.moc"
