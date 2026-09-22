// Drawn tree lines on a real RcxEditor, read back from the window's backing
// store — the exact pixels Qt composed, including the rounding of partial
// repaints' clip rects at fractional scale — at five display scales.
//
// Run without arguments it re-runs itself once per scale (the scale must be in
// the environment before QApplication exists) and prints one combined Totals
// line; `--scale 1.25 [QTest args]` runs a single scale.
//
// Every view is compared device pixel by device pixel against the model in
// treeguides.h, recomputed from live Scintilla state; then the model itself is
// held to Scintilla's own row fills (the selection band), Scintilla's own
// column positions, whole-row scrolling, partial-vs-full repaints, the patch
// path, and turning the option off.

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QBackingStore>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QMouseEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QWidget>
#include <qpa/qplatformbackingstore.h>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include "core.h"
#include "editor.h"
#include "treeguides.h"
#include "providers/buffer_provider.h"
#include "tree_fixture.h"

#include <algorithm>
#include <cstdio>
#include <tuple>
#include <vector>

using namespace rcx;

namespace {

qreal g_scale = 1.0;
const QRgb kGuide = qRgb(255, 0, 255);
const QRgb kBand  = qRgb(0, 255, 0);
bool isGuide(QRgb p) { return (p & 0xFFFFFF) == (kGuide & 0xFFFFFF); }
bool isBand(QRgb p)  { return (p & 0xFFFFFF) == (kBand & 0xFFFFFF); }
const QRgb kColumn = qRgb(0, 255, 255);
bool isColumn(QRgb p) { return (p & 0xFFFFFF) == (kColumn & 0xFFFFFF); }

using Sci = QsciScintillaBase;

long sci(QsciScintilla* s, unsigned int msg, unsigned long w = 0, long l = 0) {
    return s->SendScintilla(msg, w, l);
}

using RectKey = std::tuple<int, int, int, int>;
QVector<RectKey> keys(const QVector<QRect>& rects) {
    QVector<RectKey> k;
    for (const QRect& r : rects) k.append({r.x(), r.y(), r.width(), r.height()});
    std::sort(k.begin(), k.end());
    return k;
}

// Device pixels where the picture and the rects disagree (and the first one).
int maskMismatches(const QImage& img, const QVector<QRect>& rects, QPoint* firstBad,
                   bool (*is)(QRgb) = isGuide) {
    QImage want(img.size(), QImage::Format_Grayscale8);
    want.fill(0);
    for (const QRect& r : rects) {
        const QRect c = r.intersected(want.rect());
        for (int y = c.top(); y <= c.bottom(); ++y) {
            uchar* line = want.scanLine(y);
            for (int x = c.left(); x <= c.right(); ++x) line[x] = 1;
        }
    }
    int bad = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* px = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        const uchar* w = want.constScanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            if (is(px[x]) != (w[x] != 0)) {
                if (!bad && firstBad) *firstBad = QPoint(x, y);
                ++bad;
            }
        }
    }
    return bad;
}

} // namespace

class TestTreeGuidesRender : public QObject {
    Q_OBJECT

    QWidget* m_host = nullptr;
    RcxEditor* m_editor = nullptr;
    treefix::Rich m_fx;
    QVector<LineMeta> m_meta;

    QsciScintilla* s() const { return m_editor->scintilla(); }

    void apply(bool brace, bool treeLines = true) {
        BufferProvider prov(m_fx.data);
        const ComposeResult cr = compose(m_fx.tree, prov, m_fx.rootId, false, treeLines, brace);
        m_meta = cr.meta;
        m_editor->applyDocument(cr);
        QApplication::processEvents();
    }

    void setView(int zoom, int first, int xOffset) {
        {
            QSignalBlocker block(s());   // the zoom slider's path: no SCN_ZOOM
            s()->zoomTo(zoom);
        }
        sci(s(), Sci::SCI_SETFIRSTVISIBLELINE, (unsigned long)first);
        sci(s(), Sci::SCI_SETXOFFSET, (unsigned long)xOffset);
        QApplication::processEvents();
    }

    QImage readBack() const {
        QImage img;
        if (QBackingStore* bs = m_host->backingStore())
            if (bs->handle()) img = bs->handle()->toImage();
        if (img.isNull()) {
            qWarning("backing store image unavailable; grabbing the screen instead");
            img = m_host->screen()->grabWindow(m_host->winId()).toImage();
        }
        return img.convertToFormat(QImage::Format_RGB32);
    }

    // Everything repainted at once, the way every scroll repaints.
    QImage frame() {
        QApplication::processEvents();
        s()->viewport()->repaint();
        return readBack();
    }

    struct Live { TreeGuideGeom g; int lastLine = 0; QRect viewportDev; };

    // The model's inputs, read from Scintilla and the widget tree here — not
    // from the editor's own cached frame.
    Live liveGeometry() const {
        Live live;
        TreeGuideGeom& g = live.g;
        const qreal dpr = m_host->devicePixelRatioF();
        const QPoint o = s()->viewport()->mapTo(m_host, QPoint(0, 0));
        g.sx = g.sy = dpr;
        g.ox = o.x() * dpr;
        g.oy = o.y() * dpr;
        g.first = (int)sci(s(), Sci::SCI_GETFIRSTVISIBLELINE);
        g.lh = (int)sci(s(), Sci::SCI_TEXTHEIGHT, 0);
        g.ea = (int)sci(s(), Sci::SCI_GETEXTRAASCENT);
        g.ed = (int)sci(s(), Sci::SCI_GETEXTRADESCENT);
        const long pos = sci(s(), Sci::SCI_POSITIONFROMLINE, (unsigned long)g.first);
        g.xOrigin = (int)sci(s(), Sci::SCI_POINTXFROMPOSITION, 0, pos);
        const int textStart = g.xOrigin + (int)sci(s(), Sci::SCI_GETXOFFSET);
        const unsigned long style = 0;
        const QByteArray spaces(1024, ' ');
        g.adv = qreal(s()->SendScintilla(Sci::SCI_TEXTWIDTH, style, spaces.constData())) / 1024;
        g.t = treeStrokeDevPx(dpr, g.adv);
        const QSize vp = s()->viewport()->size();
        const int left = treegeom::snapEdge(g.ox + g.sx * textStart);
        const int top = treegeom::snapEdge(g.oy);
        const int right = treegeom::snapEdge(g.ox + g.sx * vp.width());
        const int bottom = treegeom::snapEdge(g.oy + g.sy * vp.height());
        g.clipDev = QRect(left, top, right - left, bottom - top);
        live.viewportDev = QRect(treegeom::snapEdge(g.ox), top,
                                 right - treegeom::snapEdge(g.ox), bottom - top);
        live.lastLine = g.first + (vp.height() + g.lh - 1) / g.lh;
        return live;
    }

    // Everything painted in the line colour: the lines and the (unlit) fold boxes.
    QVector<QRect> linesAndBoxes() const {
        return m_editor->lastTreeGuideDeviceRects() + m_editor->lastFoldBoxDeviceRects();
    }

    QImage viewportPixels(const QImage& img) const {
        return img.copy(liveGeometry().viewportDev.intersected(img.rect()));
    }

    int lineOf(const QString& name, LineKind kind = LineKind::Field) const {
        for (int i = 0; i < m_meta.size(); ++i) {
            const LineMeta& lm = m_meta[i];
            if (lm.nodeIdx >= 0 && lm.lineKind == kind && !lm.isContinuation
                && m_fx.tree.nodes[lm.nodeIdx].name == name)
                return i;
        }
        return -1;
    }

private slots:
    void initTestCase() {
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/JetBrainsMono.ttf"));
        m_host = new QWidget;
        m_host->resize(1000, 640);
        m_editor = new RcxEditor(m_host);
        m_editor->setGeometry(0, 0, 960, 600);
        m_host->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_host));
        QCOMPARE(m_host->devicePixelRatioF(), g_scale);

        m_fx = treefix::richTree();
        apply(false);
        m_editor->setTreeGuideColorOverrideForTest(QColor::fromRgb(kGuide));
        m_editor->setTreeColumnColorOverrideForTest(QColor::fromRgb(kColumn));
        m_editor->setTreeColumns(true);
        s()->SendScintilla(Sci::SCI_MARKERSETBACK, (unsigned long)M_SELECTED, QColor::fromRgb(kBand));
        sci(s(), Sci::SCI_MARKERSETALPHA, (unsigned long)M_SELECTED, 256 /*SC_ALPHA_NOALPHA*/);
    }

    void cleanupTestCase() { delete m_host; }

    void everyViewPaintsExactlyTheModel() {
        const int zooms[] = {-8, -4, 0, 3, 8, 14, 20};
        int frames = 0;
        for (bool brace : {false, true}) {
            apply(brace);
            const TreeGuides& tg = m_editor->treeGuides();
            QVERIFY(!tg.isEmpty());
            for (int zoom : zooms) {
                setView(zoom, 0, 0);
                const Live at0 = liveGeometry();
                const int rows = qMax(1, s()->viewport()->height() / at0.g.lh);
                int ownerOff = 2;   // two rows into a long nested group: its parent is scrolled away
                for (const TreeSpan& sp : tg.byLevel.value(1))
                    if (sp.endLine - sp.topLine >= 4) { ownerOff = sp.topLine + 2; break; }
                const int lastPage = qMax(0, tg.lineCount - rows);
                const int cross = qCeil(at0.g.adv * (kFoldCol + 0.5)) + 1;   // level 0 under the margin
                for (int first : {0, 1, 2, 3, ownerOff, lastPage}) {
                    for (int xOffset : {0, 1, 2, 3, cross}) {
                        setView(zoom, first, xOffset);
                        const QImage img = frame();
                        const Live live = liveGeometry();
                        const QString ctx = QStringLiteral("scale %1 brace %2 zoom %3 first %4 xOffset %5 (t %6 lh %7 adv %8 origin %9)")
                            .arg(g_scale).arg(brace).arg(zoom).arg(live.g.first).arg(xOffset)
                            .arg(live.g.t).arg(live.g.lh).arg(live.g.adv)
                            .arg(QStringLiteral("%1,%2").arg(live.g.ox).arg(live.g.oy));

                        const FoldBoxes& fb = m_editor->foldBoxes();
                        QVERIFY2(fb == buildFoldBoxes(m_meta),
                                 qPrintable(ctx + QStringLiteral(": the editor's fold boxes differ from the document")));
                        QVector<QRect> want;
                        treeGuideDeviceRects(live.g, tg, live.g.first, live.lastLine, want, &fb);
                        const QVector<QRect>& painted = m_editor->lastTreeGuideDeviceRects();
                        QVERIFY2(keys(painted) == keys(want),
                                 qPrintable(ctx + QStringLiteral(": painted %1 rects, live state gives %2")
                                                      .arg(painted.size()).arg(want.size())));
                        if (xOffset < cross)
                            QVERIFY2(!painted.isEmpty(), qPrintable(ctx + QStringLiteral(": nothing painted")));
                        QVector<QRect> wantBoxes;
                        foldBoxDeviceRects(live.g, fb, live.g.first, live.lastLine, wantBoxes);
                        QVERIFY2(keys(m_editor->lastFoldBoxDeviceRects()) == keys(wantBoxes),
                                 qPrintable(ctx + QStringLiteral(": painted %1 box strokes, live state gives %2")
                                                      .arg(m_editor->lastFoldBoxDeviceRects().size()).arg(wantBoxes.size())));
                        QVERIFY(m_editor->lastFoldBoxHoverDeviceRects().isEmpty());

                        QPoint bad;
                        const int n = maskMismatches(img, linesAndBoxes(), &bad);
                        QVERIFY2(n == 0, qPrintable(ctx + QStringLiteral(": %1 device px disagree, first at (%2,%3)")
                                                              .arg(n).arg(bad.x()).arg(bad.y())));

                        // Expected from the document itself, not the editor's cache.
                        const TreeColumns modelCols = buildTreeColumns(m_meta, s()->text());
                        QVERIFY2(m_editor->treeColumnGuides() == modelCols,
                                 qPrintable(ctx + QStringLiteral(": the editor's columns differ from the document")));
                        QVector<QRect> wantCols;
                        treeColumnDeviceRects(live.g, modelCols, live.g.first, live.lastLine, wantCols);
                        const QVector<QRect>& dashes = m_editor->lastTreeColumnDeviceRects();
                        QVERIFY2(keys(dashes) == keys(wantCols),
                                 qPrintable(ctx + QStringLiteral(": painted %1 column dashes, live state gives %2")
                                                      .arg(dashes.size()).arg(wantCols.size())));
                        if (xOffset < cross)
                            QVERIFY2(!dashes.isEmpty(), qPrintable(ctx + QStringLiteral(": no column dashes painted")));
                        const int nc = maskMismatches(img, dashes, &bad, isColumn);
                        QVERIFY2(nc == 0, qPrintable(ctx + QStringLiteral(": %1 column px disagree, first at (%2,%3)")
                                                               .arg(nc).arg(bad.x()).arg(bad.y())));

                        // Nothing ever crosses a depth-0 row.
                        for (int L = live.g.first; L <= qMin(live.lastLine, int(m_meta.size()) - 1); ++L) {
                            if (m_meta[L].depth > 0 && m_meta[L].lineKind != LineKind::CommandRow) continue;
                            const int y0 = qMax(0, treegeom::rowTop(live.g, L));
                            const int y1 = qMin(img.height(), treegeom::rowTop(live.g, L + 1));
                            for (int y = y0; y < y1; ++y) {
                                const QRgb* px = reinterpret_cast<const QRgb*>(img.constScanLine(y));
                                for (int x = 0; x < img.width(); ++x)
                                    QVERIFY2(!isGuide(px[x]), qPrintable(ctx + QStringLiteral(": a line crosses depth-0 row %1").arg(L)));
                            }
                        }
                        ++frames;
                    }
                }
            }
        }
        QVERIFY(frames >= 400);
    }

    void theColumnGridIsScintillasOwn() {
        apply(false);
        for (int zoom : {-8, 0, 8, 20}) {
            setView(zoom, 0, 0);
            frame();
            const Live live = liveGeometry();
            for (int L = live.g.first; L <= qMin(live.lastLine, int(m_meta.size()) - 1); ++L) {
                const int cols = qMin(160, int(s()->text(L).remove(QLatin1Char('\n')).size()));
                for (int c = 0; c <= cols; ++c) {
                    const long pos = sci(s(), Sci::SCI_FINDCOLUMN, (unsigned long)L, c);
                    if ((int)sci(s(), Sci::SCI_GETCOLUMN, (unsigned long)pos) != c) break;
                    const int x = (int)sci(s(), Sci::SCI_POINTXFROMPOSITION, 0, pos);
                    const qreal model = live.g.xOrigin + c * live.g.adv;
                    QVERIFY2(qAbs(x - model) < 1.0,
                             qPrintable(QStringLiteral("scale %1 zoom %2 line %3 col %4: Scintilla x %5, model %6")
                                            .arg(g_scale).arg(zoom).arg(L).arg(c).arg(x).arg(model)));
                }
            }
        }
    }

    void verticalsStartAndEndOnScintillasRowBands() {
        apply(false);
        const TreeGuides& tg = m_editor->treeGuides();
        const QPoint offsets[] = {QPoint(0, 0), QPoint(1, 1), QPoint(2, 3), QPoint(3, 2)};
        int checked = 0;
        for (const QPoint& at : offsets) {
            m_editor->move(at);   // sweeps the viewport's device-origin phase
            for (int zoom : {-8, -4, 0, 3, 8, 14, 20}) {
                for (int above : {1, 3}) {
                    // The deepest group with two or more children (its rows carry
                    // the most lines), scrolled so its parent sits `above` rows down.
                    const TreeSpan* pick = nullptr;
                    int level = -1;
                    for (int l = tg.byLevel.size() - 1; l >= 0 && !pick; --l)
                        for (const TreeSpan& sp : tg.byLevel[l])
                            if (sp.endLine > sp.topLine) { pick = &sp; level = l; break; }
                    QVERIFY(pick);
                    setView(zoom, qMax(0, pick->topLine - above), 0);
                    const Live live = liveGeometry();
                    QVERIFY2(pick->topLine >= live.g.first && pick->endLine < live.lastLine - 1,
                             qPrintable(QStringLiteral("scale %1 zoom %2: group %3..%4 not inside rows %5..%6 (lh %7)")
                                            .arg(g_scale).arg(zoom).arg(pick->topLine).arg(pick->endLine)
                                            .arg(live.g.first).arg(live.lastLine).arg(live.g.lh)));
                    const int first = live.g.first;
                    const int t = live.g.t;
                    const int armOff = treegeom::armOffset(live.g);
                    const int probeX = treegeom::armEnd(live.g, level) + 1;   // indent space right of the arm
                    const QString ctx = QStringLiteral("scale %1 at %2,%3 zoom %4 first %5 level %6 span %7..%8")
                        .arg(g_scale).arg(at.x()).arg(at.y()).arg(zoom).arg(live.g.first).arg(level)
                        .arg(pick->topLine).arg(pick->endLine);

                    auto band = [&](int line, int* y0, int* y1) {
                        // Anchored on this row: the node may be shown higher up too.
                        const uint64_t sel = selIdForLine(m_meta[line]);
                        m_editor->applySelectionOverlay({sel}, {{sel, anchorForLine(m_meta, line)}});
                        const QImage img = frame();
                        *y0 = *y1 = -1;
                        const int lo = qMax(0, treegeom::rowTop(live.g, line) - 4);
                        const int hi = qMin(img.height(), treegeom::rowTop(live.g, line + 1) + 4);
                        for (int y = lo; y < hi; ++y)
                            if (isBand(img.pixel(probeX, y))) { if (*y0 < 0) *y0 = y; *y1 = y; }
                    };

                    int y0 = -1, y1 = -1;
                    band(pick->topLine, &y0, &y1);
                    // Scintilla's band is exactly the model's row: the vertical
                    // starts on its first device row, the arm lies inside it.
                    QVERIFY2(y0 == treegeom::rowTop(live.g, pick->topLine),
                             qPrintable(ctx + QStringLiteral(": band starts at %1, vertical at %2").arg(y0).arg(treegeom::rowTop(live.g, pick->topLine))));
                    QVERIFY2(y1 + 1 == treegeom::rowTop(live.g, pick->topLine + 1),
                             qPrintable(ctx + QStringLiteral(": band ends at %1, next row at %2").arg(y1).arg(treegeom::rowTop(live.g, pick->topLine + 1))));
                    const int armY = y0 + armOff;
                    QVERIFY2(armY >= y0 && armY + t - 1 <= y1, qPrintable(ctx + QStringLiteral(": arm leaves the band")));

                    band(pick->endLine, &y0, &y1);
                    const int bottom = treegeom::rowTop(live.g, pick->endLine) + armOff + t;   // exclusive
                    QVERIFY2(y0 == treegeom::rowTop(live.g, pick->endLine) && bottom - 1 >= y0 && bottom - 1 <= y1,
                             qPrintable(ctx + QStringLiteral(": last child band %1..%2, vertical ends %3").arg(y0).arg(y1).arg(bottom - 1)));
                    m_editor->applySelectionOverlay({});
                    ++checked;
                }
            }
        }
        m_editor->move(0, 0);
        QCOMPARE(checked, 56);
    }

    // The zoom slider zooms under a signal blocker, so Scintilla's styles are
    // still stale when the next paint starts; refreshing them there pulls a view
    // scrolled to the bottom back up. The lines must follow the rows Scintilla
    // actually drew in that first frame.
    void theFirstFrameAfterASliderZoomAtTheBottomIsRight() {
        apply(false);
        setView(0, 0, 0);
        const TreeGuides& tg = m_editor->treeGuides();
        sci(s(), Sci::SCI_SETFIRSTVISIBLELINE, (unsigned long)tg.lineCount);   // clamps to the last page
        QApplication::processEvents();
        frame();
        const int before = (int)sci(s(), Sci::SCI_GETFIRSTVISIBLELINE);
        {
            QSignalBlocker block(s());
            s()->zoomTo(-6);
        }
        s()->viewport()->repaint();   // the first frame after the zoom, nothing read before it
        const QImage img = readBack();
        const Live live = liveGeometry();
        QVERIFY2(live.g.first < before,
                 qPrintable(QStringLiteral("zooming out at the bottom kept first line %1").arg(before)));
        QVector<QRect> want;
        treeGuideDeviceRects(live.g, tg, live.g.first, live.lastLine, want, &m_editor->foldBoxes());
        QVERIFY2(keys(m_editor->lastTreeGuideDeviceRects()) == keys(want),
                 qPrintable(QStringLiteral("scale %1: lines painted for the old scroll position (first %2 → %3)")
                                .arg(g_scale).arg(before).arg(live.g.first)));
        QVector<QRect> wantBoxes;
        foldBoxDeviceRects(live.g, m_editor->foldBoxes(), live.g.first, live.lastLine, wantBoxes);
        QVERIFY2(keys(m_editor->lastFoldBoxDeviceRects()) == keys(wantBoxes),
                 qPrintable(QStringLiteral("scale %1: fold boxes painted for the old scroll position").arg(g_scale)));
        QPoint bad;
        QVector<QRect> wantCols;
        treeColumnDeviceRects(live.g, buildTreeColumns(m_meta, s()->text()), live.g.first, live.lastLine, wantCols);
        QVERIFY2(keys(m_editor->lastTreeColumnDeviceRects()) == keys(wantCols),
                 qPrintable(QStringLiteral("scale %1: column dashes painted for the old scroll position").arg(g_scale)));
        QCOMPARE(maskMismatches(img, m_editor->lastTreeColumnDeviceRects(), &bad, isColumn), 0);
        const int n = maskMismatches(img, linesAndBoxes(), &bad);
        QVERIFY2(n == 0, qPrintable(QStringLiteral("%1 device px disagree, first at (%2,%3)").arg(n).arg(bad.x()).arg(bad.y())));
        setView(0, 0, 0);
    }

    void scrollingByARowMovesEveryLineWhole() {
        apply(false);
        for (int zoom : {0, 8}) {
            for (int f = 2; f <= 4; ++f) {
                setView(zoom, f, 0);
                const QImage a = frame();
                const Live la = liveGeometry();
                setView(zoom, f + 1, 0);
                const QImage b = frame();
                const Live lb = liveGeometry();
                QCOMPARE(lb.g.first, la.g.first + 1);
                const int rowsDev = qFloor(la.g.sy * la.g.lh);
                for (int L = lb.g.first; L < la.lastLine - 1; ++L) {
                    const int ya = treegeom::rowTop(la.g, L), yb = treegeom::rowTop(lb.g, L);
                    for (int k = 0; k < rowsDev; ++k) {
                        const QRgb* pa = reinterpret_cast<const QRgb*>(a.constScanLine(ya + k));
                        const QRgb* pb = reinterpret_cast<const QRgb*>(b.constScanLine(yb + k));
                        for (int x = la.g.clipDev.left(); x <= la.g.clipDev.right(); ++x)
                            QVERIFY2(isGuide(pa[x]) == isGuide(pb[x]) && isColumn(pa[x]) == isColumn(pb[x]),
                                     qPrintable(QStringLiteral("scale %1 zoom %2: line %3 row +%4 x %5 changed when scrolled %6→%7")
                                                    .arg(g_scale).arg(zoom).arg(L).arg(k).arg(x).arg(f).arg(f + 1)));
                    }
                }
            }
        }
    }

    void aPartialRepaintPaintsTheSamePixelsAsAFullOne() {
        apply(false);
        setView(0, 3, 0);
        frame();
        const Live live = liveGeometry();

        // Hovering repaints only the rows whose hover changed.
        const int line = live.g.first + 4;
        const QPoint p(qRound(live.g.xOrigin + (kFoldCol + 8) * live.g.adv),
                       (line - live.g.first) * live.g.lh + live.g.lh / 2);
        QMouseEvent move(QEvent::MouseMove, QPointF(p), QPointF(p), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(s()->viewport(), &move);
        QApplication::processEvents();
        const QImage hovered = viewportPixels(readBack());
        s()->viewport()->repaint();
        QVERIFY2(hovered == viewportPixels(readBack()), "hover repaint left different pixels than a full repaint");
        QPoint bad;
        QCOMPARE(maskMismatches(readBack(), linesAndBoxes(), &bad), 0);

        // Selecting repaints the old and new rows.
        m_editor->applySelectionOverlay({selIdForLine(m_meta[line + 2])});
        QApplication::processEvents();
        const QImage selected = viewportPixels(readBack());
        s()->viewport()->repaint();
        QVERIFY2(selected == viewportPixels(readBack()), "selection repaint left different pixels than a full repaint");

        m_editor->applySelectionOverlay({});
        QApplication::processEvents();
        frame();

        // An odd sliver whose edges cut a vertical mid-row and an arm mid-cell:
        // its clip rounds at fractional scale, and the lines inside must still
        // come out exactly as a full repaint draws them.
        const Live lv = liveGeometry();
        const TreeGuides& tg = m_editor->treeGuides();
        int elbow = -1;   // a visible level-0 arm row that is not its group's last
        for (int L = lv.g.first + 1; L < lv.lastLine - 3 && elbow < 0; ++L) {
            if (tg.elbowLevel.value(L, -1) != 0) continue;
            const int si = treeSpanAt(tg, 0, L);
            if (si >= 0 && L < tg.byLevel[0][si].endLine) elbow = L;
        }
        QVERIFY(elbow > 0);
        const qreal left = lv.g.xOrigin + (treegeom::levelCol(0) + 0.3) * lv.g.adv;   // inside the rail's cell, left of it
        const qreal top = (elbow - lv.g.first) * lv.g.lh + lv.g.lh / 3.0;             // below the row top
        const QRect sliverRect(qFloor(left), qFloor(top), qCeil(1.1 * lv.g.adv), qCeil(lv.g.lh * 1.5));
        const QRect sliverDev(QPoint(treegeom::snapEdge(lv.g.ox + lv.g.sx * sliverRect.left()),
                                     treegeom::snapEdge(lv.g.oy + lv.g.sy * sliverRect.top())),
                              QPoint(treegeom::snapEdge(lv.g.ox + lv.g.sx * (sliverRect.left() + sliverRect.width())) - 1,
                                     treegeom::snapEdge(lv.g.oy + lv.g.sy * (sliverRect.top() + sliverRect.height())) - 1));
        bool cuts = false, reaches = false;
        for (const QRect& r : m_editor->lastTreeGuideDeviceRects()) {
            if (!r.intersects(sliverDev)) continue;
            reaches = true;
            if (!sliverDev.contains(r)) cuts = true;
        }
        QVERIFY2(reaches && cuts, "the sliver misses the lines it is meant to cut");
        s()->viewport()->update(sliverRect);
        QApplication::processEvents();
        const QImage sliver = viewportPixels(readBack());
        s()->viewport()->repaint();
        QVERIFY2(sliver == viewportPixels(readBack()), "a sliver repaint left different pixels than a full repaint");

        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(s()->viewport(), &leave);
        QApplication::processEvents();
    }

    void appendingASiblingRedrawsTheArmAbove() {
        m_fx = treefix::richTree(/*lastChildIsMatrix=*/false);   // the last child is `tail`
        apply(false);
        const int tail = lineOf(QStringLiteral("tail"));
        QVERIFY(tail > 0);
        QCOMPARE(m_editor->treePrefixForLine(tail).right(2), QStringLiteral("└ "));
        setView(0, qMax(0, tail - 5), 0);
        frame();

        treefix::add(m_fx.tree, NodeKind::Int32, QStringLiteral("extra"), m_fx.rootId, 0x300);
        apply(false);
        QVERIFY2(m_editor->lastApplyWasPatch(), "appending a field took the full-replace path");
        const Live live = liveGeometry();
        QVERIFY(tail >= live.g.first && tail <= live.lastLine);
        QCOMPARE(m_editor->treePrefixForLine(tail).right(2), QStringLiteral("├ "));
        const QImage patched = viewportPixels(readBack());
        s()->viewport()->repaint();
        QVERIFY2(patched == viewportPixels(readBack()),
                 "after the patch the └ above stayed: the rows didn't repaint");
        QPoint bad;
        QCOMPARE(maskMismatches(readBack(), linesAndBoxes(), &bad), 0);

        m_fx = treefix::richTree();
        apply(false);
    }

    void turningTreeLinesOffKeepsTheTextAndTheFoldBoxes() {
        apply(false, true);
        setView(0, 0, 0);
        const QString on = s()->text();
        frame();
        const QVector<QRect> boxesWithLines = m_editor->lastFoldBoxDeviceRects();
        apply(false, false);
        QCOMPARE(s()->text(), on);
        QImage img = frame();
        QVERIFY(m_editor->lastTreeGuideDeviceRects().isEmpty());
        // The boxes are how a row folds, not part of the lines: still there, unmoved.
        QVERIFY(!m_editor->lastFoldBoxDeviceRects().isEmpty());
        QVERIFY(keys(m_editor->lastFoldBoxDeviceRects()) == keys(boxesWithLines));
        QPoint bad;
        QCOMPARE(maskMismatches(img, m_editor->lastFoldBoxDeviceRects(), &bad), 0);
        // The columns are their own option: still there, still exact.
        QVERIFY(!m_editor->lastTreeColumnDeviceRects().isEmpty());
        QCOMPARE(maskMismatches(img, m_editor->lastTreeColumnDeviceRects(), &bad, isColumn), 0);

        apply(false, true);
        img = frame();
        QVERIFY(!m_editor->lastTreeGuideDeviceRects().isEmpty());
        QCOMPARE(maskMismatches(img, linesAndBoxes(), &bad), 0);
    }

    // A fold box: the pointer over its slot lights it in the hover colour (its
    // row repaints, nothing else changes), a press there asks for the fold —
    // what the controller toggles on — and a press on the type text doesn't.
    void aFoldBoxLightsUnderThePointerAndFoldsOnPress() {
        apply(false, true);
        setView(0, 0, 0);
        frame();
        const int line = lineOf(QStringLiteral("inner"), LineKind::Header);
        QVERIFY(line > 0);
        const Live live = liveGeometry();
        const LineMeta& lm = m_meta[line];
        const ColumnSpan slot = foldSlotFor(lm);
        QVERIFY(slot.valid);
        auto at = [&](qreal col) {
            return QPoint(qRound(live.g.xOrigin + col * live.g.adv),
                          (line - live.g.first) * live.g.lh + live.g.lh / 2);
        };
        auto moveTo = [&](const QPoint& p) {
            QMouseEvent move(QEvent::MouseMove, QPointF(p), QPointF(p), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(s()->viewport(), &move);
            QApplication::processEvents();
        };

        moveTo(at(foldBoxCol(lm.depth) + 0.5));
        QCOMPARE(m_editor->hoveredFoldBoxLine(), line);
        const QImage lit = readBack();
        const QVector<QRect> hot = m_editor->lastFoldBoxHoverDeviceRects();
        QVERIFY(!hot.isEmpty());
        const QRgb hotColor = m_editor->foldBoxHoverColor().rgb() & 0xFFFFFF;
        for (const QRect& r : hot)
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x)
                    QVERIFY2((lit.pixel(x, y) & 0xFFFFFF) == hotColor,
                             qPrintable(QStringLiteral("scale %1: hovered box pixel (%2,%3) not in the hover colour").arg(g_scale).arg(x).arg(y)));
        const QImage partial = viewportPixels(lit);
        s()->viewport()->repaint();
        QVERIFY2(partial == viewportPixels(readBack()), "lighting the box left different pixels than a full repaint");
        QPoint bad;
        QCOMPARE(maskMismatches(readBack(), linesAndBoxes(), &bad), 0);

        QSignalSpy folds(m_editor, &RcxEditor::marginClicked);
        auto press = [&](const QPoint& p) {
            QMouseEvent down(QEvent::MouseButtonPress, QPointF(p), QPointF(p), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(s()->viewport(), &down);
            QMouseEvent up(QEvent::MouseButtonRelease, QPointF(p), QPointF(p), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(s()->viewport(), &up);
            QApplication::processEvents();
        };
        // The right edge of the arm's cell, still the box's slot (a caret-rounded
        // hit test would put it in the type text).
        const QPoint armCell = at(slot.end - 0.15);
        press(armCell);
        const long armPos = sci(s(), Sci::SCI_POSITIONFROMPOINTCLOSE, (unsigned long)armCell.x(), armCell.y());
        QVERIFY2(folds.count() == 1,
                 qPrintable(QStringLiteral("scale %1: no fold request; press at (%2,%3) is line %4 col %5, slot [%6,%7)")
                                .arg(g_scale).arg(armCell.x()).arg(armCell.y())
                                .arg(armPos < 0 ? -1 : (int)sci(s(), Sci::SCI_LINEFROMPOSITION, (unsigned long)armPos))
                                .arg(armPos < 0 ? -1 : (int)sci(s(), Sci::SCI_GETCOLUMN, (unsigned long)armPos))
                                .arg(slot.start).arg(slot.end)));
        QCOMPARE(folds.at(0).at(1).toInt(), line);
        press(at(slot.end + 0.15));   // the left edge of the type text
        QCOMPARE(folds.count(), 1);
        press(at(slot.start + 0.15)); // the left edge of the box's cell
        QCOMPARE(folds.count(), 2);

        // Off the box, it goes back to the line colour.
        moveTo(at(slot.end + 4.5));
        QCOMPARE(m_editor->hoveredFoldBoxLine(), -1);
        frame();
        QVERIFY(m_editor->lastFoldBoxHoverDeviceRects().isEmpty());
        QCOMPARE(maskMismatches(readBack(), linesAndBoxes(), &bad), 0);

        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(s()->viewport(), &leave);
        QApplication::processEvents();
    }

    void treeColumnsAreTheirOwnOption() {
        apply(false, true);
        setView(0, 0, 0);
        const QString text = s()->text();
        QPoint bad;

        // Columns off, lines on.
        m_editor->setTreeColumns(false);
        QImage img = frame();
        QVERIFY(m_editor->lastTreeColumnDeviceRects().isEmpty());
        QCOMPARE(maskMismatches(img, {}, &bad, isColumn), 0);
        QVERIFY(!m_editor->lastTreeGuideDeviceRects().isEmpty());
        QCOMPARE(maskMismatches(img, linesAndBoxes(), &bad), 0);

        // Columns on, lines off — the menu path: a document applied while the
        // columns were off, then the option turned on, nothing applied since.
        apply(false, false);
        QVERIFY(m_editor->treeColumnGuides().isEmpty());
        m_editor->setTreeColumns(true);
        img = frame();
        {
            const Live lv = liveGeometry();
            const TreeColumns model = buildTreeColumns(m_meta, s()->text());
            QVERIFY(m_editor->treeColumnGuides() == model);
            QVector<QRect> want;
            treeColumnDeviceRects(lv.g, model, lv.g.first, lv.lastLine, want);
            QVERIFY(!want.isEmpty());
            QVERIFY(keys(m_editor->lastTreeColumnDeviceRects()) == keys(want));
        }
        QVERIFY(m_editor->lastTreeGuideDeviceRects().isEmpty());
        QCOMPARE(maskMismatches(img, m_editor->lastFoldBoxDeviceRects(), &bad), 0);
        QVERIFY(!m_editor->lastTreeColumnDeviceRects().isEmpty());
        QCOMPARE(maskMismatches(img, m_editor->lastTreeColumnDeviceRects(), &bad, isColumn), 0);
        QCOMPARE(s()->text(), text);

        // Both on: each dash sits on blank paper between two columns' text —
        // the pixels around it in its row band are all paper.
        apply(false, true);
        img = frame();
        QCOMPARE(maskMismatches(img, m_editor->lastTreeColumnDeviceRects(), &bad, isColumn), 0);
        QCOMPARE(maskMismatches(img, linesAndBoxes(), &bad), 0);
        const Live live = liveGeometry();
        const TreeColumns& tc = m_editor->treeColumnGuides();
        int checked = 0;
        for (int L = live.g.first; L <= qMin(live.lastLine - 1, tc.lineCount - 1); ++L) {
            const int y0 = treegeom::rowTop(live.g, L), y1 = treegeom::rowTop(live.g, L + 1);
            for (int k = tc.firstCol(L); k < tc.endCol(L); ++k) {
                const int x = treegeom::cellLineX(live.g, tc.cols[k]);
                const QRgb paper = img.pixel(x - 2, y0);   // the band's top row, beside the line
                for (int y = y0; y < y1; ++y)
                    for (int dx = x - 1; dx <= x + live.g.t; ++dx) {
                        const QRgb p = img.pixel(dx, y);
                        QVERIFY2(isColumn(p) || (p & 0xFFFFFF) == (paper & 0xFFFFFF),
                                 qPrintable(QStringLiteral("scale %1: row %2 column %3 has ink at (%4,%5)")
                                                .arg(g_scale).arg(L).arg(tc.cols[k]).arg(dx).arg(y)));
                    }
                ++checked;
            }
        }
        QVERIFY(checked > 20);
    }

    // Renaming inserts or removes characters, so the rest of the row moves
    // while the edit is open: no dash may stay in the cells from the edit on.
    void aRenameInProgressDrawsNoStaleColumns() {
        apply(false, true);
        setView(0, 0, 0);
        frame();
        int line = -1;
        for (int i = 0; i < m_meta.size(); ++i)
            if (m_meta[i].nodeIdx >= 0 && m_meta[i].lineKind == LineKind::Field
                && m_fx.tree.nodes[m_meta[i].nodeIdx].name == QStringLiteral("health")) { line = i; break; }
        QVERIFY(line > 0);
        const TreeColumns& tc = m_editor->treeColumnGuides();
        QCOMPARE(tc.endCol(line) - tc.firstCol(line), 2);

        QVERIFY(m_editor->beginInlineEdit(EditTarget::Name, line));
        const int spanStart = m_editor->editSpanStart();
        const unsigned long zero = 0;
        s()->SendScintilla(Sci::SCI_REPLACESEL, zero, "healthWithAMuchLongerName");
        QImage img = frame();
        const Live live = liveGeometry();
        const int y0 = treegeom::rowTop(live.g, line), y1 = treegeom::rowTop(live.g, line + 1);
        const int editX = treegeom::cellLineX(live.g, spanStart);
        for (int y = y0; y < y1; ++y)
            for (int x = editX; x < img.width(); ++x)
                QVERIFY2(!isColumn(img.pixel(x, y)),
                         qPrintable(QStringLiteral("scale %1: column pixels at (%2,%3), right of the edit").arg(g_scale).arg(x).arg(y)));
        // The type|name line sits before the edit and still runs through the
        // row; the name|value line resumes on the next row.
        const int typeNameX = treegeom::cellLineX(live.g, tc.cols[tc.firstCol(line)]);
        const int nameValueX = treegeom::cellLineX(live.g, tc.cols[tc.firstCol(line) + 1]);
        for (int y = y0; y < y1; ++y)
            QVERIFY2(isColumn(img.pixel(typeNameX, y)),
                     qPrintable(QStringLiteral("scale %1: the type|name line breaks at y %2").arg(g_scale).arg(y)));
        QVERIFY2(isColumn(img.pixel(nameValueX, y1)), "the name|value line doesn't resume below the edit");

        m_editor->cancelInlineEdit();
        apply(false, true);   // what the controller's refresh does after an edit
        img = frame();
        QPoint bad;
        QCOMPARE(maskMismatches(img, m_editor->lastTreeColumnDeviceRects(), &bad, isColumn), 0);
        QVERIFY(m_editor->treeColumnGuides() == buildTreeColumns(m_meta, s()->text()));
    }

    // A node shown in several places (Rich again under its `self` pointer):
    // the hover band marks only the row under the pointer and its matrix rows,
    // never the other copies or an enum field's members; the selection band
    // marks the anchored copy, or the first one without an anchor.
    void aRowBandMarksOnlyThePlacePointedAt() {
        apply(false, true);
        QHash<uint64_t, QVector<int>> idx;
        for (int i = 0; i < m_meta.size(); ++i)
            if (m_meta[i].nodeId) idx[m_meta[i].nodeId].append(i);
        auto idOf = [&](const QString& name) {
            for (const Node& n : m_fx.tree.nodes) if (n.name == name) return n.id;
            return uint64_t(0);
        };
        auto marked = [&](int marker) {
            QVector<int> out;
            for (int i = 0; i < m_meta.size(); ++i)
                if (sci(s(), Sci::SCI_MARKERGET, (unsigned long)i) & (1L << marker)) out.append(i);
            return out;
        };
        auto hover = [&](int line) {
            setView(0, qMax(0, line - 2), 0);
            frame();
            const Live live = liveGeometry();
            const QPoint p(qRound(live.g.xOrigin + (kFoldCol + 12) * live.g.adv),
                           (line - live.g.first) * live.g.lh + live.g.lh / 2);
            QMouseEvent move(QEvent::MouseMove, QPointF(p), QPointF(p), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(s()->viewport(), &move);
            QApplication::processEvents();
        };

        const uint64_t view = idOf(QStringLiteral("view"));
        const QVector<int> starts = instanceStarts(m_meta, idx.value(view), view);
        QVERIFY2(starts.size() >= 2, "the fixture no longer shows the matrix twice");
        const QVector<int> second = instanceBlock(m_meta, starts[1]);
        QCOMPARE(second.size(), 4);

        hover(starts[1] + 2);   // the second copy's third matrix row
        QCOMPARE(marked(M_HOVER), second);

        const uint64_t mode = idOf(QStringLiteral("mode"));
        const QVector<int> modes = instanceStarts(m_meta, idx.value(mode), mode);
        QVERIFY(!modes.isEmpty());
        hover(modes[0]);
        QCOMPARE(marked(M_HOVER), QVector<int>{modes[0]});   // not its member rows, not the other copy

        m_editor->applySelectionOverlay({view}, {{view, anchorForLine(m_meta, starts[1] + 1)}});
        QCOMPARE(marked(M_SELECTED), second);
        m_editor->applySelectionOverlay({view});
        QCOMPARE(marked(M_SELECTED), instanceBlock(m_meta, starts[0]));
        m_editor->applySelectionOverlay({});
        QVERIFY(marked(M_SELECTED).isEmpty());

        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(s()->viewport(), &leave);
        QApplication::processEvents();
        QVERIFY(marked(M_HOVER).isEmpty());
    }

    void copyingARowKeepsItsTreeCharacters() {
        apply(false);
        int row = -1;
        for (int i = 1; i < m_meta.size(); ++i)
            if (m_meta[i].depth == 2 && isTreeElbowRow(m_meta[i])) { row = i; break; }
        QVERIFY(row > 0);
        const QString raw = s()->text(row);
        for (QChar ch : {QChar(0x2502), QChar(0x251C), QChar(0x2514)})
            QVERIFY(!raw.contains(ch));
        const QString copied = m_editor->lineTextForCopy(row);
        QCOMPARE(copied.mid(kFoldCol, 2 * kTreeIndent), m_editor->treePrefixForLine(row));
        QVERIFY(copied.contains(QChar(0x251C)) || copied.contains(QChar(0x2514)));
        QVERIFY(m_editor->textWithMargins().contains(copied));
    }
};

int main(int argc, char** argv) {
    int scaleAt = -1;
    for (int i = 1; i + 1 < argc; ++i)
        if (qstrcmp(argv[i], "--scale") == 0) { scaleAt = i; break; }

    if (scaleAt < 0) {
        // One child per display scale; their Totals are renamed so the runner
        // reads the combined line printed last.
        QCoreApplication app(argc, argv);
        // `-o file[,format]` is the parent's to honour: five children writing
        // one file would leave only the last scale in it. Children print to
        // stdout; the combined text goes to every requested file at the end.
        // Other formats go to one file per scale ("<file>.scale<S>"), with a
        // text copy on stdout for the totals; "-o -" is stdout, not a file.
        QStringList passthrough, outFiles;
        QVector<QPair<QString, QString>> perScale;   // file, format
        const QStringList args = app.arguments().mid(1);
        static const QStringList kLegacyFormats = {
            QStringLiteral("-txt"), QStringLiteral("-csv"), QStringLiteral("-xml"), QStringLiteral("-lightxml"),
            QStringLiteral("-junitxml"), QStringLiteral("-teamcity"), QStringLiteral("-tap")};
        for (int i = 0; i < args.size(); ++i) {
            if (kLegacyFormats.contains(args[i])) {
                std::fprintf(stderr, "%s: use -o <file>,<format> (it runs once per display scale)\n",
                             qPrintable(args[i]));
                return 1;
            }
            if (args[i] == QStringLiteral("-o") && i + 1 < args.size()) {
                const QString spec = args[++i];
                const QString file = spec.section(QLatin1Char(','), 0, 0);
                const QString format = spec.section(QLatin1Char(','), 1, 1);
                if (format.isEmpty() || format == QStringLiteral("txt")) {
                    if (file != QStringLiteral("-")) outFiles << file;
                } else {
                    perScale.append({file, format});
                }
                continue;
            }
            passthrough << args[i];
        }
        const QRegularExpression totals(QStringLiteral("Totals: (\\d+) passed, (\\d+) failed, (\\d+) skipped"));
        int passed = 0, failed = 0, skipped = 0;
        QString combined;
        for (const char* scale : {"1", "1.25", "1.5", "1.75", "2"}) {
            QStringList childArgs{QStringLiteral("--scale"), QString::fromLatin1(scale)};
            if (!perScale.isEmpty()) {
                for (const auto& o : perScale)
                    childArgs << QStringLiteral("-o")
                              << QStringLiteral("%1.scale%2,%3").arg(o.first, QLatin1String(scale), o.second);
                childArgs << QStringLiteral("-o") << QStringLiteral("-,txt");
            }
            QProcess child;
            child.setProcessChannelMode(QProcess::MergedChannels);
            child.start(QCoreApplication::applicationFilePath(), childArgs + passthrough);
            const bool finished = child.waitForFinished(-1);
            QString out = QString::fromLocal8Bit(child.readAll());
            const QRegularExpressionMatch m = totals.match(out);
            if (m.hasMatch()) {
                passed += m.captured(1).toInt();
                failed += m.captured(2).toInt();
                skipped += m.captured(3).toInt();
            }
            if (!finished || child.exitStatus() != QProcess::NormalExit || !m.hasMatch()) {
                ++failed;
                out += QStringLiteral("FAIL!  : scale %1 did not finish cleanly\n").arg(QLatin1String(scale));
            }
            out.replace(QStringLiteral("Totals: "), QStringLiteral("Totals at scale %1: ").arg(QLatin1String(scale)));
            std::fputs(out.toUtf8().constData(), stdout);
            std::fflush(stdout);
            combined += out;
        }
        const QString summary = QStringLiteral("Totals: %1 passed, %2 failed, %3 skipped\n")
                                    .arg(passed).arg(failed).arg(skipped);
        std::fputs(summary.toUtf8().constData(), stdout);
        combined += summary;
        for (const QString& path : outFiles) {
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(combined.toUtf8());
        }
        return failed;
    }

    const QByteArray scale(argv[scaleAt + 1]);
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    qputenv("QT_SCALE_FACTOR", scale);
    g_scale = scale.toDouble();

    std::vector<char*> rest;
    for (int i = 0; i < argc; ++i)
        if (i != scaleAt && i != scaleAt + 1) rest.push_back(argv[i]);
    int restCount = int(rest.size());
    rest.push_back(nullptr);

    QApplication app(restCount, rest.data());
    TestTreeGuidesRender tc;
    return QTest::qExec(&tc, restCount, rest.data());
}

#include "test_tree_guides_render.moc"
