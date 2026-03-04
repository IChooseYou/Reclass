// Tests the full tooltip flow including DarkApp-style ToolTip interception.
// Verifies that QEvent::ToolTip fires and our custom tooltip appears.

#include <QtTest>
#include <QApplication>
#include <QPushButton>
#include <QScreen>
#include <QHelpEvent>
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

// Simulates DarkApp::notify behavior — installed as a global event filter
class DarkAppSimulator : public QObject {
public:
    int tooltipEventCount = 0;
    int leaveEventCount = 0;
    int showForCallCount = 0;

    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (ev->type() == QEvent::ToolTip) {
            tooltipEventCount++;
            if (obj->isWidgetType()) {
                auto* w = static_cast<QWidget*>(obj);
                QString tip = w->toolTip();
                LOG("  [darkapp-sim] ToolTip #%d on '%s' tip='%s'\n",
                    tooltipEventCount, qPrintable(w->objectName()),
                    qPrintable(tip.left(60)));
                if (!tip.isEmpty()) {
                    showForCallCount++;
                    LOG("  [darkapp-sim] calling showFor #%d\n", showForCallCount);
                    RcxTooltip::instance()->showFor(w, tip);
                    LOG("  [darkapp-sim] after showFor: visible=%d pos=(%d,%d) size=%dx%d\n",
                        RcxTooltip::instance()->isVisible(),
                        RcxTooltip::instance()->x(), RcxTooltip::instance()->y(),
                        RcxTooltip::instance()->width(), RcxTooltip::instance()->height());
                    return true;  // consume — same as DarkApp
                }
            }
            return true;  // suppress default QToolTip
        }
        if (ev->type() == QEvent::Leave && obj->isWidgetType()) {
            auto* tip = RcxTooltip::instance();
            if (tip->isVisible() && tip->currentTrigger() == obj) {
                leaveEventCount++;
                LOG("  [darkapp-sim] Leave #%d on trigger\n", leaveEventCount);
                tip->scheduleDismiss();
            }
        }
        return false;
    }
};

class TestTooltipEvent : public QObject {
    Q_OBJECT

private:
    QWidget*          m_window = nullptr;
    QPushButton*      m_btn    = nullptr;
    QPushButton*      m_btn2   = nullptr;
    DarkAppSimulator* m_sim    = nullptr;

private slots:
    void initTestCase() {
        LOG("=== TestTooltipEvent starting ===\n");

        m_window = new QWidget;
        m_window->setFixedSize(400, 300);
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        m_window->move(avail.center() - QPoint(200, 150));

        m_btn = new QPushButton("Scan", m_window);
        m_btn->setToolTip("Start scanning memory");
        m_btn->setFixedSize(120, 40);
        m_btn->move(30, 130);
        m_btn->setObjectName("btnScan");

        m_btn2 = new QPushButton("Copy", m_window);
        m_btn2->setToolTip("Copy to clipboard");
        m_btn2->setFixedSize(120, 40);
        m_btn2->move(250, 130);
        m_btn2->setObjectName("btnCopy");

        // Install DarkApp simulator as global event filter
        m_sim = new DarkAppSimulator;
        qApp->installEventFilter(m_sim);

        m_window->show();
        m_window->activateWindow();
        m_window->raise();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        // Let window become active
        QTest::qWait(200);
        QCoreApplication::processEvents();

        LOG("  window at (%d,%d)\n", m_window->x(), m_window->y());
        LOG("  btn global: (%d,%d)\n",
            m_btn->mapToGlobal(QPoint(60, 20)).x(),
            m_btn->mapToGlobal(QPoint(60, 20)).y());
    }

    void cleanupTestCase() {
        qApp->removeEventFilter(m_sim);
        RcxTooltip::instance()->dismiss();
        delete m_sim;
        delete m_window;
        LOG("=== TestTooltipEvent finished ===\n");
    }

    void cleanup() {
        RcxTooltip::instance()->dismiss();
        QCoreApplication::processEvents();
        m_sim->tooltipEventCount = 0;
        m_sim->leaveEventCount = 0;
        m_sim->showForCallCount = 0;
    }

    // Test 1: Post QHelpEvent → DarkApp simulator intercepts → RcxTooltip shows
    void testManualEventShowsTooltip() {
        LOG("\n--- testManualEventShowsTooltip ---\n");
        auto* tip = RcxTooltip::instance();

        QPoint btnGlobal = m_btn->mapToGlobal(QPoint(60, 20));
        QCursor::setPos(btnGlobal);
        QCoreApplication::processEvents();

        LOG("  posting QHelpEvent\n");
        QHelpEvent helpEvent(QEvent::ToolTip, QPoint(60, 20), btnGlobal);
        QApplication::sendEvent(m_btn, &helpEvent);
        QCoreApplication::processEvents();
        QTest::qWait(100);
        QCoreApplication::processEvents();

        LOG("  sim: tooltipEvents=%d showForCalls=%d\n",
            m_sim->tooltipEventCount, m_sim->showForCallCount);
        LOG("  tip: visible=%d text='%s'\n",
            tip->isVisible(), qPrintable(tip->currentText()));

        QVERIFY2(m_sim->tooltipEventCount > 0, "Event filter didn't see ToolTip event");
        QVERIFY2(m_sim->showForCallCount > 0, "showFor was never called");
        QVERIFY2(tip->isVisible(), "RcxTooltip not visible after manual event");
        QCOMPARE(tip->currentText(), QString("Start scanning memory"));

        // Verify pixels
        tip->setWindowOpacity(1.0);
        QCoreApplication::processEvents();
        QImage img = tip->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        QRect body = tip->bodyRect().adjusted(2, 2, -2, -2);
        int opaque = 0;
        for (int y = body.top(); y <= body.bottom(); ++y)
            for (int x = body.left(); x <= body.right(); ++x)
                if (qAlpha(img.pixel(x, y)) > 0) opaque++;
        LOG("  pixels: %d/%d opaque\n", opaque, body.width() * body.height());
        QVERIFY2(opaque > body.width() * body.height() / 2, "Body not rendered");

        LOG("--- testManualEventShowsTooltip PASSED ---\n");
    }

    // Test 2: Qt's native tooltip timer fires → our filter intercepts → tooltip shows
    void testNativeTimerShowsTooltip() {
        LOG("\n--- testNativeTimerShowsTooltip ---\n");
        auto* tip = RcxTooltip::instance();

        // Move cursor away first
        QPoint away = m_window->mapToGlobal(QPoint(380, 10));
        QCursor::setPos(away);
        QTest::qWait(200);
        QCoreApplication::processEvents();

        // Move to button
        QPoint btnCenter = m_btn->mapToGlobal(QPoint(60, 20));
        LOG("  moving cursor to (%d,%d)\n", btnCenter.x(), btnCenter.y());
        QCursor::setPos(btnCenter);

        // Send Enter + MouseMove to kick the tooltip timer
        QEvent enterEv(QEvent::Enter);
        QApplication::sendEvent(m_btn, &enterEv);
        QMouseEvent moveEv(QEvent::MouseMove, QPointF(60, 20),
                           m_btn->mapToGlobal(QPointF(60, 20)),
                           Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(m_btn, &moveEv);

        // Wait up to 2000ms for tooltip to appear
        LOG("  waiting for Qt tooltip timer...\n");
        bool appeared = false;
        for (int i = 0; i < 20; i++) {
            QTest::qWait(100);
            QCoreApplication::processEvents();
            if (m_sim->tooltipEventCount > 0) {
                LOG("  tooltip event at ~%dms! events=%d showFor=%d\n",
                    (i+1)*100, m_sim->tooltipEventCount, m_sim->showForCallCount);
                appeared = true;
                break;
            }
        }

        // Process remaining events
        QTest::qWait(100);
        QCoreApplication::processEvents();

        LOG("  final: events=%d showFor=%d visible=%d text='%s'\n",
            m_sim->tooltipEventCount, m_sim->showForCallCount,
            tip->isVisible(), qPrintable(tip->currentText()));

        QVERIFY2(appeared, "Qt tooltip timer never fired (no ToolTip event in 2 seconds)");
        QVERIFY2(tip->isVisible(), "Tooltip not visible after native timer fired");

        LOG("--- testNativeTimerShowsTooltip PASSED ---\n");
    }

    // Test 3: Leave after tooltip shown → tooltip survives (cursor still in zone)
    void testLeaveSurvival() {
        LOG("\n--- testLeaveSurvival ---\n");
        auto* tip = RcxTooltip::instance();

        QPoint btnCenter = m_btn->mapToGlobal(QPoint(60, 20));
        QCursor::setPos(btnCenter);
        QCoreApplication::processEvents();

        // Show via manual event
        QHelpEvent helpEvent(QEvent::ToolTip, QPoint(60, 20), btnCenter);
        QApplication::sendEvent(m_btn, &helpEvent);
        QCoreApplication::processEvents();
        QTest::qWait(100);
        QCoreApplication::processEvents();
        QVERIFY(tip->isVisible());

        // Send Leave (cursor still on button)
        LOG("  sending Leave while cursor on button\n");
        QEvent leaveEv(QEvent::Leave);
        QApplication::sendEvent(m_btn, &leaveEv);
        QTest::qWait(200);
        QCoreApplication::processEvents();

        LOG("  after Leave+200ms: visible=%d leaves=%d\n",
            tip->isVisible(), m_sim->leaveEventCount);
        QVERIFY2(tip->isVisible(), "Tooltip dismissed by spurious Leave");

        LOG("--- testLeaveSurvival PASSED ---\n");
    }

    // Test 4: Switch between widgets
    void testWidgetSwitch() {
        LOG("\n--- testWidgetSwitch ---\n");
        auto* tip = RcxTooltip::instance();

        // Show on btn1
        QPoint btn1Center = m_btn->mapToGlobal(QPoint(60, 20));
        QCursor::setPos(btn1Center);
        QCoreApplication::processEvents();
        QHelpEvent ev1(QEvent::ToolTip, QPoint(60, 20), btn1Center);
        QApplication::sendEvent(m_btn, &ev1);
        QCoreApplication::processEvents();
        QTest::qWait(100);
        QVERIFY(tip->isVisible());
        QCOMPARE(tip->currentText(), QString("Start scanning memory"));
        QPoint pos1 = tip->pos();

        // Switch to btn2
        QPoint btn2Center = m_btn2->mapToGlobal(QPoint(60, 20));
        QCursor::setPos(btn2Center);
        QCoreApplication::processEvents();
        QHelpEvent ev2(QEvent::ToolTip, QPoint(60, 20), btn2Center);
        QApplication::sendEvent(m_btn2, &ev2);
        QCoreApplication::processEvents();
        QTest::qWait(100);

        LOG("  after switch: visible=%d text='%s' pos=(%d,%d)\n",
            tip->isVisible(), qPrintable(tip->currentText()),
            tip->x(), tip->y());
        QVERIFY(tip->isVisible());
        QCOMPARE(tip->currentText(), QString("Copy to clipboard"));
        QVERIFY(tip->pos() != pos1);

        LOG("--- testWidgetSwitch PASSED ---\n");
    }
};

QTEST_MAIN(TestTooltipEvent)
#include "test_tooltip_event.moc"
