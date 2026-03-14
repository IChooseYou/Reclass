#include <QtTest>
#include <QApplication>
#include <QPushButton>
#include <QScreen>
#include <QImage>
#include "rcxtooltip.h"
#include "themes/thememanager.h"

using namespace rcx;

// ─────────────────────────────────────────────────────────────────
// Test suite for the RcxTooltip arrow callout widget
//
// Validates:
// - Arrow direction auto-detection (above/below based on screen space)
// - Arrow X clamped to stay within rounded corners
// - WA_TranslucentBackground rendering (arrow + body have opaque pixels,
//   corners are transparent)
// - Content sizing (title + separator + body)
// ─────────────────────────────────────────────────────────────────
class TestTooltip : public QObject {
    Q_OBJECT

private:
    QWidget*     m_window = nullptr;
    RcxTooltip*  m_tip    = nullptr;

    QFont testFont() {
        QFont f("JetBrains Mono", 12);
        f.setFixedPitch(true);
        return f;
    }

    void showAndProcess(const QPoint& anchor) {
        m_tip->showAt(anchor);
        QCoreApplication::processEvents();
        QTest::qWait(20);
        QCoreApplication::processEvents();
    }

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
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));

        m_tip = new RcxTooltip(m_window);
        const auto& t = ThemeManager::instance().current();
        m_tip->setTheme(t.backgroundAlt, t.border, t.text, t.syntaxNumber, t.border);
    }

    void cleanupTestCase() {
        m_tip->dismiss();
        delete m_tip;
        delete m_window;
    }

    void cleanup() {
        m_tip->dismiss();
        QCoreApplication::processEvents();
    }

    // ── Basic show/dismiss ──
    void testShowAndDismiss() {
        QVERIFY(!m_tip->isVisible());
        m_tip->populate("Title", "Body text", testFont());
        showAndProcess(m_window->mapToGlobal(QPoint(400, 300)));
        QVERIFY(m_tip->isVisible());
        m_tip->dismiss();
        QVERIFY(!m_tip->isVisible());
    }

    // ── Duplicate populate is no-op ──
    void testDuplicatePopulateSkipped() {
        m_tip->populate("Title", "Body", testFont());
        showAndProcess(m_window->mapToGlobal(QPoint(400, 300)));
        QPoint pos1 = m_tip->pos();
        // Same content — populate returns early, position unchanged
        m_tip->populate("Title", "Body", testFont());
        QCOMPARE(m_tip->pos(), pos1);
    }

    // ── Arrow direction: below when room exists ──
    void testArrowUpWhenBelow() {
        m_tip->populate("Test", "Below", testFont());
        // Anchor in middle of screen — plenty of room below
        QPoint anchor = m_window->mapToGlobal(QPoint(400, 300));
        showAndProcess(anchor);
        QVERIFY(m_tip->isVisible());
        // Arrow up (tooltip below anchor): widget top == anchor.y
        QCOMPARE(m_tip->y(), anchor.y());
    }

    // ── Arrow direction: above when no room below ──
    void testArrowDownWhenAbove() {
        m_tip->populate("Test", "Above", testFont());
        // Anchor near bottom of screen
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        QPoint anchor(avail.center().x(), avail.bottom() - 5);
        showAndProcess(anchor);
        QVERIFY(m_tip->isVisible());
        // Arrow down (tooltip above anchor): widget bottom == anchor.y
        int tipBottom = m_tip->y() + m_tip->height();
        QCOMPARE(tipBottom, anchor.y());
    }

    // ── Horizontal clamping ──
    void testHorizontalClampLeft() {
        m_tip->populate("Test", "Wide body text for clamping", testFont());
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        QPoint anchor(avail.left() + 5, avail.center().y());
        showAndProcess(anchor);
        QVERIFY(m_tip->x() >= avail.left());
    }

    void testHorizontalClampRight() {
        m_tip->populate("Test", "Wide body text for clamping", testFont());
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        QPoint anchor(avail.right() - 5, avail.center().y());
        showAndProcess(anchor);
        QVERIFY(m_tip->x() + m_tip->width() <= avail.right() + 2);
    }

    // ── Constants ──
    void testConstants() {
        QCOMPARE(RcxTooltip::kArrowH, 8);
        QCOMPARE(RcxTooltip::kArrowW, 14);
        QCOMPARE(RcxTooltip::kRadius, 6);
    }

    // ── Title-only vs title+body sizing ──
    void testTitleOnlySizing() {
        m_tip->dismiss();
        m_tip->populate("", "Just body", testFont());
        showAndProcess(m_window->mapToGlobal(QPoint(400, 300)));
        int hNoTitle = m_tip->height();

        m_tip->dismiss();
        m_tip->populate("Title", "Just body", testFont());
        showAndProcess(m_window->mapToGlobal(QPoint(400, 300)));
        int hWithTitle = m_tip->height();

        QVERIFY2(hWithTitle > hNoTitle,
                 "Tooltip with title should be taller than body-only");
    }

    // ── Multi-line body ──
    void testMultilineBody() {
        m_tip->dismiss();
        m_tip->populate("Title", "Line 1", testFont());
        showAndProcess(m_window->mapToGlobal(QPoint(400, 300)));
        int h1 = m_tip->height();

        m_tip->dismiss();
        m_tip->populate("Title", "Line 1\nLine 2\nLine 3", testFont());
        showAndProcess(m_window->mapToGlobal(QPoint(400, 300)));
        int h3 = m_tip->height();

        QVERIFY2(h3 > h1, "3-line tooltip should be taller than 1-line");
    }

    // ──────────────────────────────────────────────────────────────
    // RENDERING VERIFICATION — WA_TranslucentBackground works
    // ──────────────────────────────────────────────────────────────

    void testBodyRendersOpaquePixels() {
        m_tip->populate("Render", "Test body", testFont());
        showAndProcess(m_window->mapToGlobal(QPoint(400, 300)));
        QVERIFY(m_tip->isVisible());

        QImage img = m_tip->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(!img.isNull());

        // Check center of body for opaque pixels (avoid edges/corners)
        QRect center(img.width() / 4, img.height() / 4,
                     img.width() / 2, img.height() / 2);
        int opaque = countOpaquePixels(img, center);
        int total = center.width() * center.height();
        QVERIFY2(opaque > total / 2,
                 qPrintable(QStringLiteral("Body center has %1/%2 opaque pixels (<50%%)")
                     .arg(opaque).arg(total)));
    }

    void testCornersAreTransparent() {
        m_tip->populate("Corner", "Test", testFont());
        showAndProcess(m_window->mapToGlobal(QPoint(400, 300)));
        QVERIFY(m_tip->isVisible());

        QImage img = m_tip->grab().toImage().convertToFormat(QImage::Format_ARGB32);

        // Top-left 2x2 corner should be fully transparent (rounded corner)
        QRect corner(0, 0, 2, 2);
        int opaque = countOpaquePixels(img, corner);
        QCOMPARE(opaque, 0);
    }

    void testArrowRendersPixels() {
        m_tip->populate("Arrow", "Test", testFont());
        // Show below (arrow up) — arrow is in the top strip
        showAndProcess(m_window->mapToGlobal(QPoint(400, 300)));
        QVERIFY(m_tip->isVisible());

        QImage img = m_tip->grab().toImage().convertToFormat(QImage::Format_ARGB32);

        // Arrow region: top kArrowH pixels, centered horizontally
        int centerX = img.width() / 2;
        QRect arrowRect(centerX - RcxTooltip::kArrowW / 2, 0,
                        RcxTooltip::kArrowW, RcxTooltip::kArrowH);
        int opaque = countOpaquePixels(img, arrowRect);
        QVERIFY2(opaque > 0,
                 qPrintable(QStringLiteral("Arrow region has 0 opaque pixels")));
    }
};

QTEST_MAIN(TestTooltip)
#include "test_tooltip.moc"
