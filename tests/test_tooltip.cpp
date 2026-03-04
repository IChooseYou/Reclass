#include <QtTest>
#include <QApplication>
#include <QPushButton>
#include <QScreen>
#include <QImage>
#include "rcxtooltip.h"
#include "themes/thememanager.h"

using namespace rcx;

// ─────────────────────────────────────────────────────────────────
// Test suite for the RcxTooltip callout widget
//
// These tests verify both geometry math AND real-world behavior:
// - Actual pixel rendering (catches WA_TranslucentBackground failures)
// - Leave-event resilience (catches spurious dismiss on tooltip popup)
// - Dismiss correctness (cursor truly leaves trigger zone)
// ─────────────────────────────────────────────────────────────────
class TestTooltip : public QObject {
    Q_OBJECT

private:
    QWidget*      m_window  = nullptr;
    QPushButton*  m_btnTop  = nullptr;
    QPushButton*  m_btnMid  = nullptr;
    QPushButton*  m_btnLeft = nullptr;
    QPushButton*  m_btnRight= nullptr;

    void showAndProcess(QWidget* trigger, const QString& text) {
        RcxTooltip::instance()->showFor(trigger, text);
        // Process events + allow paint to complete
        QCoreApplication::processEvents();
        QTest::qWait(20);
        QCoreApplication::processEvents();
    }

    // Count non-transparent pixels in a QImage region
    int countOpaquePixels(const QImage& img, const QRect& region) {
        int count = 0;
        QRect r = region.intersected(img.rect());
        for (int y = r.top(); y <= r.bottom(); ++y)
            for (int x = r.left(); x <= r.right(); ++x)
                if (qAlpha(img.pixel(x, y)) > 0)
                    ++count;
        return count;
    }

private slots:
    void initTestCase() {
        m_window = new QWidget;
        m_window->setFixedSize(800, 600);

        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        m_window->move(avail.center() - QPoint(400, 300));

        m_btnMid = new QPushButton("Middle", m_window);
        m_btnMid->setFixedSize(80, 24);
        m_btnMid->move(360, 288);

        m_btnTop = new QPushButton("Top", m_window);
        m_btnTop->setFixedSize(80, 24);
        m_btnTop->move(360, 0);

        m_btnLeft = new QPushButton("Left", m_window);
        m_btnLeft->setFixedSize(80, 24);
        m_btnLeft->move(0, 288);

        m_btnRight = new QPushButton("Right", m_window);
        m_btnRight->setFixedSize(80, 24);
        m_btnRight->move(720, 288);

        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanupTestCase() {
        RcxTooltip::instance()->dismiss();
        delete m_window;
        m_window = nullptr;
    }

    void cleanup() {
        RcxTooltip::instance()->dismiss();
        QCoreApplication::processEvents();
    }

    // ── Singleton ──
    void testSingleton() {
        QCOMPARE(RcxTooltip::instance(), RcxTooltip::instance());
    }

    // ── Basic show/dismiss ──
    void testShowAndDismiss() {
        auto* tip = RcxTooltip::instance();
        QVERIFY(!tip->isVisible());

        showAndProcess(m_btnMid, "Hello");
        QVERIFY(tip->isVisible());
        QCOMPARE(tip->currentText(), QString("Hello"));
        QCOMPARE(tip->currentTrigger(), m_btnMid);

        tip->dismiss();
        QVERIFY(!tip->isVisible());
        QVERIFY(tip->currentTrigger() == nullptr);
    }

    // ── Empty text / null trigger = dismiss ──
    void testEmptyTextDismisses() {
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "Test");
        QVERIFY(tip->isVisible());
        showAndProcess(m_btnMid, "");
        QVERIFY(!tip->isVisible());
    }

    void testNullTriggerDismisses() {
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "Test");
        QVERIFY(tip->isVisible());
        showAndProcess(nullptr, "Test");
        QVERIFY(!tip->isVisible());
    }

    // ── Arrow direction ──
    void testArrowDownByDefault() {
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "Default placement");
        QVERIFY(tip->isVisible());
        QVERIFY(tip->arrowPointsDown());

        QRect trigGlobal(m_btnMid->mapToGlobal(QPoint(0,0)), m_btnMid->size());
        int tipBottom = tip->y() + tip->height();
        QVERIFY2(tipBottom <= trigGlobal.top() + RcxTooltip::kGap + 2,
                 qPrintable(QStringLiteral("tipBottom=%1 trigTop=%2")
                     .arg(tipBottom).arg(trigGlobal.top())));
    }

    void testArrowFlipsAtScreenTop() {
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        QPoint oldPos = m_window->pos();
        m_window->move(avail.center().x() - 400, avail.top());
        QCoreApplication::processEvents();

        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnTop, "Flipped");
        QVERIFY(tip->isVisible());
        QVERIFY2(!tip->arrowPointsDown(),
                 "Expected arrow to flip upward when trigger is near screen top");

        QRect trigGlobal(m_btnTop->mapToGlobal(QPoint(0,0)), m_btnTop->size());
        QVERIFY2(tip->y() >= trigGlobal.bottom(),
                 qPrintable(QStringLiteral("tipY=%1 trigBottom=%2")
                     .arg(tip->y()).arg(trigGlobal.bottom())));

        m_window->move(oldPos);
        QCoreApplication::processEvents();
    }

    // ── Arrow centering ──
    void testArrowCenteredOnTrigger() {
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "Center");
        QVERIFY(tip->isVisible());

        QRect trigGlobal(m_btnMid->mapToGlobal(QPoint(0,0)), m_btnMid->size());
        int trigCenterX = trigGlobal.center().x();
        int arrowGlobalX = tip->x() + tip->arrowLocalX();
        int delta = qAbs(arrowGlobalX - trigCenterX);
        QVERIFY2(delta <= 2,
                 qPrintable(QStringLiteral("arrowGlobalX=%1 trigCenterX=%2 delta=%3")
                     .arg(arrowGlobalX).arg(trigCenterX).arg(delta)));
    }

    // ── Anti-teleport ──
    void testNoTeleportSameWidget() {
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "Stable");
        QPoint pos1 = tip->pos();
        showAndProcess(m_btnMid, "Stable");
        QCOMPARE(tip->pos(), pos1);
    }

    // ── Repositions for different widget ──
    void testRepositionsForDifferentWidget() {
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnLeft, "Left");
        QPoint pos1 = tip->pos();
        showAndProcess(m_btnRight, "Right");
        QVERIFY2(tip->pos() != pos1, "Tooltip should move when trigger widget changes");
    }

    // ── Horizontal clamping ──
    void testHorizontalClampLeft() {
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        QPoint oldPos = m_window->pos();
        m_window->move(avail.left(), avail.center().y() - 300);
        QCoreApplication::processEvents();

        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnLeft, "Clamped left");
        QVERIFY(tip->isVisible());
        QVERIFY2(tip->x() >= avail.left(),
                 qPrintable(QStringLiteral("tipX=%1 screenLeft=%2")
                     .arg(tip->x()).arg(avail.left())));

        m_window->move(oldPos);
        QCoreApplication::processEvents();
    }

    void testHorizontalClampRight() {
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        QPoint oldPos = m_window->pos();
        m_window->move(avail.right() - m_window->width(), avail.center().y() - 300);
        QCoreApplication::processEvents();

        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnRight, "Clamped right");
        QVERIFY(tip->isVisible());
        QVERIFY2(tip->x() + tip->width() <= avail.right() + 2,
                 qPrintable(QStringLiteral("tipRight=%1 screenRight=%2")
                     .arg(tip->x() + tip->width()).arg(avail.right())));

        m_window->move(oldPos);
        QCoreApplication::processEvents();
    }

    // ── Body rect dimensions ──
    void testBodyRectSanity() {
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "Body");
        QVERIFY(tip->isVisible());

        QRect body = tip->bodyRect();
        QVERIFY(body.width() > 0);
        QVERIFY(body.height() > 0);
        QCOMPARE(tip->height(), body.height() + RcxTooltip::kArrowH);
    }

    // ── Constants ──
    void testConstants() {
        QCOMPARE(RcxTooltip::kArrowH, 6);
        QCOMPARE(RcxTooltip::kArrowHalfW, 6);
        QCOMPARE(RcxTooltip::kGap, 2);
    }

    // ──────────────────────────────────────────────────────────────
    // RENDERING VERIFICATION — catches invisible tooltip bugs
    // ──────────────────────────────────────────────────────────────

    void testShowForRendersBodyPixels() {
        // Show tooltip and grab its rendered pixels.
        // Verify that the body area has non-transparent content.
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "Render test");
        QVERIFY(tip->isVisible());

        // Force full opacity so grab gets real pixels
        tip->setWindowOpacity(1.0);
        QCoreApplication::processEvents();

        QImage img = tip->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY2(!img.isNull(), "grab() returned null image");
        QVERIFY2(img.width() > 0 && img.height() > 0, "grab() returned empty image");

        // Check body rect area for opaque pixels
        QRect body = tip->bodyRect();
        // Inset by 2px to avoid anti-aliased border edges
        QRect checkRect = body.adjusted(2, 2, -2, -2);
        int opaquePixels = countOpaquePixels(img, checkRect);
        int totalPixels = checkRect.width() * checkRect.height();

        QVERIFY2(opaquePixels > totalPixels / 2,
                 qPrintable(QStringLiteral(
                     "Body area has too few opaque pixels: %1 / %2 (< 50%%). "
                     "The tooltip is not rendering its background.")
                     .arg(opaquePixels).arg(totalPixels)));
    }

    void testArrowRendersPixels() {
        // Verify the triangle arrow region has some opaque pixels.
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "Arrow test");
        QVERIFY(tip->isVisible());
        QVERIFY(tip->arrowPointsDown());

        tip->setWindowOpacity(1.0);
        QCoreApplication::processEvents();

        QImage img = tip->grab().toImage().convertToFormat(QImage::Format_ARGB32);

        // Arrow region: below the body rect, centered on arrowLocalX
        QRect body = tip->bodyRect();
        int arrowTop = body.bottom();
        int arrowLeft = tip->arrowLocalX() - RcxTooltip::kArrowHalfW;
        int arrowRight = tip->arrowLocalX() + RcxTooltip::kArrowHalfW;
        QRect arrowRect(arrowLeft, arrowTop, arrowRight - arrowLeft, RcxTooltip::kArrowH);

        int opaquePixels = countOpaquePixels(img, arrowRect);
        QVERIFY2(opaquePixels > 0,
                 qPrintable(QStringLiteral(
                     "Arrow region has 0 opaque pixels — triangle not painted. "
                     "arrowRect=(%1,%2 %3x%4) imgSize=(%5x%6)")
                     .arg(arrowRect.x()).arg(arrowRect.y())
                     .arg(arrowRect.width()).arg(arrowRect.height())
                     .arg(img.width()).arg(img.height())));
    }

    // ──────────────────────────────────────────────────────────────
    // LEAVE EVENT RESILIENCE — catches spurious dismiss bugs
    // ──────────────────────────────────────────────────────────────

    void testSurvivesLeaveEvent() {
        // The tooltip should NOT be dismissed when a Leave event fires
        // on the trigger widget while the cursor is still in the
        // trigger+tooltip zone (simulates the synthetic Leave that Qt
        // sends when a tooltip window pops up above the trigger).
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "Survive Leave");
        QVERIFY(tip->isVisible());

        tip->setWindowOpacity(1.0);

        // Move real cursor to center of trigger (so geometry check passes)
        QPoint trigCenter = m_btnMid->mapToGlobal(
            QPoint(m_btnMid->width() / 2, m_btnMid->height() / 2));
        QCursor::setPos(trigCenter);
        QCoreApplication::processEvents();

        // Send a Leave event to the trigger (like DarkApp::notify would)
        QEvent leaveEvent(QEvent::Leave);
        QApplication::sendEvent(m_btnMid, &leaveEvent);

        // Now call scheduleDismiss as DarkApp would
        tip->scheduleDismiss();
        QCoreApplication::processEvents();

        // Tooltip should STILL be visible — cursor is inside trigger zone
        QVERIFY2(tip->isVisible(),
                 "Tooltip was dismissed by spurious Leave event while cursor "
                 "was still over the trigger widget");

        // Wait beyond the dismiss timer to be sure
        QTest::qWait(200);
        QCoreApplication::processEvents();
        QVERIFY2(tip->isVisible(),
                 "Tooltip was dismissed after 200ms despite cursor being over trigger");
    }

    void testDismissesOnRealLeave() {
        // When the cursor truly leaves the trigger+tooltip zone,
        // scheduleDismiss() should queue dismissal and it should fire.
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "Real leave");
        QVERIFY(tip->isVisible());

        tip->setWindowOpacity(1.0);

        // Move cursor far away from both trigger and tooltip
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        QCursor::setPos(avail.bottomRight() - QPoint(10, 10));
        QCoreApplication::processEvents();

        // scheduleDismiss should detect cursor is outside zone
        tip->scheduleDismiss();
        QCoreApplication::processEvents();

        // Wait for the 100ms dismiss timer
        QTest::qWait(200);
        QCoreApplication::processEvents();

        QVERIFY2(!tip->isVisible(),
                 "Tooltip should have been dismissed when cursor left the zone");
    }

    void testLeaveAndReshow() {
        // Dismiss via real leave, then re-show on a different widget.
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "First");
        QVERIFY(tip->isVisible());

        // Force dismiss
        tip->dismiss();
        QCoreApplication::processEvents();
        QVERIFY(!tip->isVisible());

        // Re-show on different widget
        showAndProcess(m_btnLeft, "Second");
        QVERIFY2(tip->isVisible(), "Tooltip failed to re-appear after dismiss");
        QCOMPARE(tip->currentText(), QString("Second"));
        QCOMPARE(tip->currentTrigger(), m_btnLeft);
    }

    // ── Scheduled dismiss cancelled by new showFor ──
    void testScheduledDismissCancelledByShow() {
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "First");

        // Move cursor far away and schedule dismiss
        QScreen* scr = QApplication::primaryScreen();
        QCursor::setPos(scr->availableGeometry().bottomRight() - QPoint(10, 10));
        QCoreApplication::processEvents();
        tip->scheduleDismiss();

        // Before timer fires, show on a different widget
        showAndProcess(m_btnLeft, "Second");
        QTest::qWait(200);
        QCoreApplication::processEvents();

        // Should still be visible — new showFor cancelled the timer
        QVERIFY(tip->isVisible());
        QCOMPARE(tip->currentText(), QString("Second"));
    }

    // ── Text change on same widget ──
    void testTextChangeOnSameWidget() {
        auto* tip = RcxTooltip::instance();
        showAndProcess(m_btnMid, "Text A");
        QCOMPARE(tip->currentText(), QString("Text A"));

        tip->dismiss();
        showAndProcess(m_btnMid, "Text B");
        QCOMPARE(tip->currentText(), QString("Text B"));
    }
};

QTEST_MAIN(TestTooltip)
#include "test_tooltip.moc"
