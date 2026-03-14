// Tests RcxTooltip positioning and arrow direction across screen edges.
// Validates that the arrow tip touches the anchor point and the tooltip
// body stays within screen bounds.

#include <QtTest>
#include <QApplication>
#include <QScreen>
#include <QImage>
#include "rcxtooltip.h"
#include "themes/thememanager.h"

using namespace rcx;

class TestTooltipEvent : public QObject {
    Q_OBJECT

private:
    RcxTooltip* m_tip = nullptr;

    QFont testFont() {
        QFont f("JetBrains Mono", 12);
        f.setFixedPitch(true);
        return f;
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

    // Arrow tip Y matches anchor Y when showing below
    void testArrowTipMatchesAnchorBelow() {
        m_tip->populate("Test", "Body", testFont());
        QScreen* scr = QApplication::primaryScreen();
        QPoint anchor = scr->availableGeometry().center();
        m_tip->showAt(anchor);
        QCoreApplication::processEvents();
        QVERIFY(m_tip->isVisible());
        // Arrow up (tooltip below): widget top == anchor.y
        QCOMPARE(m_tip->y(), anchor.y());
    }

    // Arrow tip Y matches anchor Y when showing above
    void testArrowTipMatchesAnchorAbove() {
        m_tip->populate("Test", "Body", testFont());
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        QPoint anchor(avail.center().x(), avail.bottom() - 2);
        m_tip->showAt(anchor);
        QCoreApplication::processEvents();
        QVERIFY(m_tip->isVisible());
        // Arrow down (tooltip above): widget bottom == anchor.y
        QCOMPARE(m_tip->y() + m_tip->height(), anchor.y());
    }

    // Tooltip stays within screen bounds at left edge
    void testScreenLeftEdge() {
        m_tip->populate("Test", "Wide body content for edge test", testFont());
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        QPoint anchor(avail.left() + 2, avail.center().y());
        m_tip->showAt(anchor);
        QCoreApplication::processEvents();
        QVERIFY(m_tip->x() >= avail.left());
    }

    // Tooltip stays within screen bounds at right edge
    void testScreenRightEdge() {
        m_tip->populate("Test", "Wide body content for edge test", testFont());
        QScreen* scr = QApplication::primaryScreen();
        QRect avail = scr->availableGeometry();
        QPoint anchor(avail.right() - 2, avail.center().y());
        m_tip->showAt(anchor);
        QCoreApplication::processEvents();
        QVERIFY(m_tip->x() + m_tip->width() <= avail.right() + 2);
    }

    // Content change triggers resize
    void testContentResize() {
        m_tip->populate("Short", "A", testFont());
        m_tip->showAt(QPoint(500, 500));
        QCoreApplication::processEvents();
        int w1 = m_tip->width();

        m_tip->dismiss();
        m_tip->populate("Much Longer Title", "A much wider body line that should be larger", testFont());
        m_tip->showAt(QPoint(500, 500));
        QCoreApplication::processEvents();
        int w2 = m_tip->width();

        QVERIFY2(w2 > w1, "Wider content should produce a wider tooltip");
    }
};

QTEST_MAIN(TestTooltipEvent)
#include "test_tooltip_event.moc"
