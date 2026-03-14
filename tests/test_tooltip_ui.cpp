// Rendering verification for RcxTooltip.
// Grabs widget pixels to confirm WA_TranslucentBackground works correctly
// and the arrow/body are painted with the expected alpha.

#include <QtTest>
#include <QApplication>
#include <QScreen>
#include <QImage>
#include "rcxtooltip.h"
#include "themes/thememanager.h"

using namespace rcx;

class TestTooltipUI : public QObject {
    Q_OBJECT

private:
    RcxTooltip* m_tip = nullptr;

    QFont testFont() {
        QFont f("JetBrains Mono", 12);
        f.setFixedPitch(true);
        return f;
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
        m_tip = new RcxTooltip;
        const auto& t = ThemeManager::instance().current();
        m_tip->setTheme(t.backgroundAlt, t.border, t.text, t.syntaxNumber, t.border);
    }

    void cleanupTestCase() {
        m_tip->dismiss();
        delete m_tip;
    }

    void cleanup() {
        m_tip->dismiss();
        QCoreApplication::processEvents();
    }

    // Body center should be opaque (background painted)
    void testBodyIsOpaque() {
        m_tip->populate("Render Test", "Body content here", testFont());
        QScreen* scr = QApplication::primaryScreen();
        m_tip->showAt(scr->availableGeometry().center());
        QCoreApplication::processEvents();
        QTest::qWait(50);

        QImage img = m_tip->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(!img.isNull());

        // Center 50% of widget should be mostly opaque
        QRect center(img.width() / 4, img.height() / 4,
                     img.width() / 2, img.height() / 2);
        int opaque = countOpaquePixels(img, center);
        int total = center.width() * center.height();
        QVERIFY2(opaque > total * 0.8,
                 qPrintable(QStringLiteral("Body has %1/%2 opaque pixels — expected >80%%")
                     .arg(opaque).arg(total)));
    }

    // Top-left corner should be transparent (rounded corner + WA_TranslucentBackground)
    void testCornerTransparency() {
        m_tip->populate("Corner", "Test", testFont());
        QScreen* scr = QApplication::primaryScreen();
        m_tip->showAt(scr->availableGeometry().center());
        QCoreApplication::processEvents();
        QTest::qWait(50);

        QImage img = m_tip->grab().toImage().convertToFormat(QImage::Format_ARGB32);

        // When arrow is up, body starts at kArrowH. The corner at (0, kArrowH)
        // should be transparent due to rounding.
        QRect corner(0, 0, 2, 2);
        int opaque = countOpaquePixels(img, corner);
        QCOMPARE(opaque, 0);
    }

    // Arrow region should have some opaque pixels
    void testArrowHasPixels() {
        m_tip->populate("Arrow", "Test", testFont());
        QScreen* scr = QApplication::primaryScreen();
        m_tip->showAt(scr->availableGeometry().center());
        QCoreApplication::processEvents();
        QTest::qWait(50);

        QImage img = m_tip->grab().toImage().convertToFormat(QImage::Format_ARGB32);

        // Arrow is at top (m_up = true): check top kArrowH pixels around center
        int cx = img.width() / 2;
        QRect arrowRect(cx - RcxTooltip::kArrowW / 2, 0,
                        RcxTooltip::kArrowW, RcxTooltip::kArrowH);
        int opaque = countOpaquePixels(img, arrowRect);
        QVERIFY2(opaque > 0, "Arrow region has no opaque pixels");
    }

    // Grabbing after dismiss should not crash
    void testDismissAndReshow() {
        m_tip->populate("First", "Body", testFont());
        QScreen* scr = QApplication::primaryScreen();
        m_tip->showAt(scr->availableGeometry().center());
        QCoreApplication::processEvents();
        QVERIFY(m_tip->isVisible());

        m_tip->dismiss();
        QVERIFY(!m_tip->isVisible());

        m_tip->populate("Second", "Different", testFont());
        m_tip->showAt(scr->availableGeometry().center());
        QCoreApplication::processEvents();
        QVERIFY(m_tip->isVisible());
    }
};

QTEST_MAIN(TestTooltipUI)
#include "test_tooltip_ui.moc"
