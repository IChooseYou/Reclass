// The frameless main window's resize zones: every edge and corner — the top
// ones included — inside the window, never overlapping, clear of the title
// bar's Close button, and none at all while maximized or full screen.

#include <QtTest/QTest>

#include "widgets/resize_edges.h"

using namespace rcx;

namespace {
const Qt::Edges kZones[] = {
    Qt::TopEdge, Qt::BottomEdge, Qt::LeftEdge, Qt::RightEdge,
    Qt::TopEdge | Qt::LeftEdge, Qt::TopEdge | Qt::RightEdge,
    Qt::BottomEdge | Qt::LeftEdge, Qt::BottomEdge | Qt::RightEdge,
};
}

class TestResizeEdges : public QObject {
    Q_OBJECT

private slots:
    void everyEdgeAndCornerHasAZone() {
        const int w = 1080, h = 720, caption = 32;
        const QRect window(0, 0, w, h);
        QVector<QRect> rects;
        for (Qt::Edges e : kZones) {
            const QRect r = resizeEdgeRect(e, w, h, caption);
            QVERIFY2(!r.isEmpty() && window.contains(r), qPrintable(QStringLiteral("zone %1").arg(int(e))));
            rects.append(r);
        }
        for (int i = 0; i < rects.size(); ++i)
            for (int j = i + 1; j < rects.size(); ++j)
                QVERIFY2(!rects[i].intersects(rects[j]), qPrintable(QStringLiteral("zones %1 and %2 overlap").arg(i).arg(j)));

        const int E = kResizeEdgeThickness, C = kResizeCornerSize;
        // Along the very top: the strip and its two corners, one band E tall.
        QCOMPARE(resizeEdgeRect(Qt::TopEdge, w, h, caption), QRect(C, 0, w - 2 * C, E));
        QCOMPARE(resizeEdgeRect(Qt::TopEdge | Qt::LeftEdge, w, h, caption), QRect(0, 0, C, E));
        QCOMPARE(resizeEdgeRect(Qt::TopEdge | Qt::RightEdge, w, h, caption), QRect(w - C, 0, C, E));
        // The sides start below the title bar.
        QCOMPARE(resizeEdgeRect(Qt::LeftEdge, w, h, caption), QRect(0, caption, E, h - caption - C));
        QCOMPARE(resizeEdgeRect(Qt::RightEdge, w, h, caption), QRect(w - E, caption, E, h - caption - C));
        QCOMPARE(resizeEdgeRect(Qt::BottomEdge, w, h, caption), QRect(C, h - E, w - 2 * C, E));
        QVERIFY(resizeEdgeRect(Qt::Edges(), w, h, caption).isEmpty());
        // Without a title bar the sides run between their corners.
        QCOMPARE(resizeEdgeRect(Qt::LeftEdge, w, h), QRect(0, C, E, h - 2 * C));
    }

    // Close is 46×32 in the top-right corner: only its top band resizes, the
    // way a native caption button does.
    void closeKeepsItsCornerAndEdge() {
        const int w = 1080, h = 720, caption = 32, E = kResizeEdgeThickness;
        const QRect closeBelowTheBand(w - 46, E, 46, caption - E);
        for (Qt::Edges e : kZones)
            QVERIFY2(!resizeEdgeRect(e, w, h, caption).intersects(closeBelowTheBand),
                     qPrintable(QStringLiteral("zone %1 covers Close").arg(int(e))));
    }

    void aMaximizedWindowHasNoZones() {
        QVERIFY(resizeZonesEnabled(Qt::WindowNoState));
        QVERIFY(resizeZonesEnabled(Qt::WindowActive));
        QVERIFY(!resizeZonesEnabled(Qt::WindowMaximized));
        QVERIFY(!resizeZonesEnabled(Qt::WindowFullScreen));
        QVERIFY(!resizeZonesEnabled(Qt::WindowMaximized | Qt::WindowActive));
    }
};

QTEST_GUILESS_MAIN(TestResizeEdges)
#include "test_resize_edges.moc"
