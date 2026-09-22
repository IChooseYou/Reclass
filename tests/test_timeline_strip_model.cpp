// The timeline strip's layout and time math, pinned without a widget.
//
// The strip's two promises are that nothing moves when capture state
// changes, and that the graph never shimmers while it follows "now". Both
// are properties of this pure model, so they are tested here, headless.

#include <QtTest/QTest>
#include <random>

#include "widgets/timeline_strip_model.h"

using namespace rcx::timeline;

class TestTimelineStripModel : public QObject {
    Q_OBJECT
private slots:

    void ladderPicksTheSmallestStepThatFits() {
        QCOMPARE(stepForSpan(1000, 100), int64_t(10));
        QCOMPARE(stepForSpan(1001, 100), int64_t(12));
        QCOMPARE(stepForSpan(60'000, 600), int64_t(100));
        QCOMPARE(stepForSpan(300'000, 700), int64_t(500));
        QCOMPARE(stepForSpan(int64_t(1e12), 10), stepLadder().last());
        for (int64_t s : stepLadder()) QVERIFY(s > 0);
        QVERIFY(std::is_sorted(stepLadder().cbegin(), stepLadder().cend()));
        // Dense enough that framing wastes little: no rung more than 2.5× the last
        // below a second.
        for (int i = 1; i < stepLadder().size() && stepLadder()[i] <= 1000; ++i)
            QVERIFY(stepLadder()[i] <= stepLadder()[i - 1] * 5 / 2);
    }

    // The whole recording, from the left edge: a short one fills the width
    // instead of a sliver at the right, and none of it is cut off.
    void aRecordingFillsTheWidthFromTheLeft() {
        const int columns = 1700;
        for (int64_t len : {7'000LL, 45'000LL, 3'600'000LL}) {
            const int64_t begin = 1'000'003, end = begin + len;
            const Window w = fitLeftWindow(begin, end, columns, 5, 12);
            QVERIFY(w.follow);
            QVERIFY(w.t0Ms <= begin);
            QVERIFY(w.t1Ms() >= end);
            QCOMPARE(w.t0Ms % w.stepMs, int64_t(0));
            const double used = double(end - begin) / double(w.spanMs());
            QVERIFY2(used >= 0.75, qPrintable(QStringLiteral("%1 ms fills %2 %").arg(len).arg(used * 100)));
            QVERIFY((begin - w.t0Ms) / w.stepMs <= 6);   // it starts at the left
        }
    }

    // Following "now" slides by whole columns: while now advances less than
    // one step, the window — and so every bin — is identical.
    void followingNowDoesNotShimmer() {
        const int columns = 640;
        Window prev = followWindow(300'000, 1'000'000, columns);
        QCOMPARE(prev.t0Ms % prev.stepMs, int64_t(0));
        QVERIFY(prev.t1Ms() >= 1'000'000);
        int changes = 0;
        for (int64_t now = 1'000'001; now < 1'000'000 + 5 * prev.stepMs; now += 37) {
            const Window w = followWindow(300'000, now, columns);
            QCOMPARE(w.stepMs, prev.stepMs);
            QVERIFY(w.t1Ms() >= now);
            QVERIFY(w.t1Ms() - now < w.stepMs);
            if (!(w == prev)) {
                QCOMPARE(w.t0Ms - prev.t0Ms, w.stepMs);   // exactly one column
                ++changes;
            }
            prev = w;
        }
        QVERIFY(changes >= 4 && changes <= 5);
    }

    void timeAndXRoundTrip() {
        const Window w = followWindow(120'000, 5'000'000, 800);
        const double left = 100, width = 800;
        for (int x = 100; x <= 900; x += 13) {
            const int64_t t = timeForX(w, left, width, x);
            QVERIFY(std::abs(xForTime(w, left, width, t) - x) <= 1.0);
        }
        QCOMPARE(timeForX(w, left, width, left), w.t0Ms);
        QCOMPARE(timeForX(w, left, width, left + width), w.t1Ms());
    }

    // Zooming keeps the moment under the pointer under the pointer.
    void zoomKeepsTheAnchorStill() {
        const int columns = 700;
        const double left = 0, width = 700;
        const int64_t lo = 0, hi = 3'600'000;
        Window w = followWindow(1'800'000, hi, columns);
        std::mt19937 rng(3);
        for (int i = 0; i < 60; ++i) {
            const double x = double(rng() % 700);
            const int64_t anchor = timeForX(w, left, width, x);
            const double factor = (i % 2) ? 0.8 : 1.25;
            const Window z = zoomAround(w, anchor, factor, lo, hi);
            QVERIFY(!z.follow);
            QVERIFY(z.spanMs() >= kMinSpanMs);
            QCOMPARE(z.t0Ms % z.stepMs, int64_t(0));
            // Grid-snapped, so the anchor may move by at most a column plus
            // any clamping at the ends of retained time.
            const double nx = xForTime(z, left, width, anchor);
            const bool clamped = z.t0Ms <= lo || z.t1Ms() >= hi;
            if (!clamped) QVERIFY2(std::abs(nx - x) <= 2.0, qPrintable(QStringLiteral("moved %1 px").arg(nx - x)));
            w = z;
        }
        // Zooming in cannot go below the minimum span.
        Window tiny = w;
        for (int i = 0; i < 40; ++i) tiny = zoomAround(tiny, 1'000'000, 0.5, lo, hi);
        QVERIFY(tiny.spanMs() >= kMinSpanMs);
        QVERIFY(tiny.spanMs() < 10 * kMinSpanMs);
    }

    // A frame that already shows the whole recording has nowhere to pan to —
    // and never jumps the wrong way trying.
    void panningAWholeFrameGoesNowhere() {
        const Window f = fitLeftWindow(0, 120'000, 1042, 4, 64);
        QVERIFY(panBy(f, f.spanMs() / 10, 0, 120'000) == f);
        QVERIFY(panBy(f, -f.spanMs() / 10, 0, 120'000) == f);
    }

    void panClampsAndRefollowsAtNow() {
        const int64_t lo = 100'000, hi = 900'000;
        Window w = followWindow(200'000, hi, 400);
        QVERIFY(w.follow);
        Window back = panBy(w, -150'000, lo, hi);
        QVERIFY(!back.follow);
        QVERIFY(back.t0Ms < w.t0Ms);
        Window far = panBy(back, -10'000'000, lo, hi);
        QVERIFY(far.t0Ms <= lo);
        QVERIFY(far.t0Ms > lo - far.stepMs);
        Window again = panBy(far, 10'000'000, lo, hi);
        QVERIFY(again.follow);
        QCOMPARE(again.t1Ms(), ceilTo(hi, again.stepMs));
    }

    void snapPrefersTheNearestTickWithinTolerance() {
        const QVector<int64_t> ticks = {100, 250, 400};
        QCOMPARE(snapToChange(240, ticks, 20), int64_t(250));
        QCOMPARE(snapToChange(330, ticks, 20), int64_t(330));   // nothing close
        QCOMPARE(snapToChange(160, ticks, 100), int64_t(100));  // nearer of two in range
        QCOMPARE(snapToChange(399, ticks, 1), int64_t(400));
        QCOMPARE(snapToChange(50, {}, 100), int64_t(50));
        QCOMPARE(snapToChange(250, ticks, 0), int64_t(250));
    }

    void domainGrowsAtOnceShrinksLazily() {
        QCOMPARE(domainFor(0, 0), uint32_t(4));
        QCOMPARE(domainFor(3, 0), uint32_t(4));
        QCOMPARE(domainFor(5, 4), uint32_t(8));
        QCOMPARE(domainFor(1000, 8), uint32_t(1024));
        QCOMPARE(domainFor(300, 1024), uint32_t(1024));   // still ≥ a quarter
        QCOMPARE(domainFor(255, 1024), uint32_t(256));    // below a quarter: shrink
    }

    void columnHeightsAreSqrtWithAOneRowFloor() {
        QCOMPARE(columnHeightDev(0, 64, 25), 0);
        QCOMPARE(columnHeightDev(1, 1 << 20, 25), 1);      // never invisible
        QCOMPARE(columnHeightDev(64, 64, 25), 25);
        QCOMPARE(columnHeightDev(1000, 64, 25), 25);        // clipped to the band
        QCOMPARE(columnHeightDev(16, 64, 20), 10);          // sqrt(1/4) of 20
        int prev = 0;
        for (uint32_t v = 0; v <= 256; ++v) {
            const int h = columnHeightDev(v, 256, 25);
            QVERIFY(h >= prev);
            prev = h;
        }
    }

    // Forgotten beats not-watched beats watched.
    void regionPrecedence() {
        const QVector<Span> gaps = { {500, 700}, {900, -1} };
        QCOMPARE(regionAt(100, 200, 2000, gaps), Region::NotRetained);
        QCOMPARE(regionAt(3000, 200, 2000, gaps), Region::NotRetained);
        QCOMPARE(regionAt(300, 200, 2000, gaps), Region::Captured);
        QCOMPARE(regionAt(600, 200, 2000, gaps), Region::Gap);
        QCOMPARE(regionAt(700, 200, 2000, gaps), Region::Captured);   // end exclusive
        QCOMPARE(regionAt(1500, 200, 2000, gaps), Region::Gap);       // open gap
        QCOMPARE(regionAt(600, 650, 2000, gaps), Region::NotRetained);
    }

    void durationsAndClockRead() {
        const QString m = QString(QChar(0x2212));
        QCOMPARE(formatAgo(400), QStringLiteral("now"));
        QCOMPARE(formatAgo(42'000), m + QStringLiteral("42s"));
        QCOMPARE(formatAgo(192'000), m + QStringLiteral("3m 12s"));
        QCOMPARE(formatAgo(3'840'000), m + QStringLiteral("1h 04m"));
        // 12:04:31.250 UTC on some day.
        const int64_t epoch = 1'700'000'000'000LL - (1'700'000'000'000LL % 86'400'000) + (12 * 3600 + 4 * 60 + 31) * 1000 + 250;
        QCOMPARE(formatClock(epoch, 0), QStringLiteral("12:04:31.2"));
        QCOMPARE(formatClock(epoch, -3600), QStringLiteral("11:04:31.2"));
    }

    // The strip is the graph and one square overflow button at the right
    // margin; the graph takes everything else, at every width.
    void layoutIsTheGraphAndOneButton() {
        for (int w : {120, 240, 300, 480, 760, 1080, 1920}) {
            const Layout L = layoutStrip(w);
            QCOMPARE(L.cells.size(), 1);
            const QRect more = L.rect(QStringLiteral("more"));
            QCOMPARE(more.width(), kIconCellW);
            QCOMPARE(more.height(), kCellH);
            QCOMPARE(more.width(), more.height());          // a square hit area
            QCOMPARE(more.top(), kCellTop);
            QCOMPARE(more.right() + 1 + kRightMargin, w);
            // The graph is the whole bar; the button floats over its right end.
            QCOMPARE(L.graph.left(), 0);
            QCOMPARE(L.graph.right() + 1, w);
            QVERIFY(more.left() > L.graph.left() && more.right() < L.graph.right());
            QCOMPARE(L.graph.top(), kGraphTop);
            QCOMPARE(L.graph.bottom() + 1, kGraphBottom);
        }
        QVERIFY(layoutStrip(1920).graph.width() > 1800);
    }

    void hitTesting() {
        const Layout L = layoutStrip(1080);
        QCOMPARE(cellIdAt(L, L.rect(QStringLiteral("more")).center()), QStringLiteral("more"));
        QCOMPARE(cellIdAt(L, QPoint(L.graph.center().x(), 1)), QStringLiteral("graph"));
        QCOMPARE(cellIdAt(L, QPoint(L.graph.center().x(), 24)), QStringLiteral("graph"));
        QCOMPARE(cellIdAt(L, QPoint(2, 13)), QStringLiteral("graph"));   // no dead gutter any more
    }

    // "Fit all" shows the whole retained span, on the grid.
    void fitCoversEverything() {
        const int64_t cases[][2] = { {0, 1000}, {123'456, 987'654}, {5'000, 5'100},
                                     {1'000'000, 1'000'000 + 8LL * 3600 * 1000} };
        for (const auto& c : cases) {
            for (int columns : {60, 333, 1920}) {
                const Window w = fitWindow(c[0], c[1], columns);
                QVERIFY2(w.t0Ms <= c[0], qPrintable(QStringLiteral("t0 %1 > begin %2").arg(w.t0Ms).arg(c[0])));
                QVERIFY(w.t1Ms() >= c[1] || w.stepMs == stepLadder().last());
                QCOMPARE(w.t0Ms % w.stepMs, int64_t(0));
                QCOMPARE(w.columns, columns);
                QVERIFY(w.spanMs() >= kMinSpanMs);
            }
        }
    }

    // ── The painted mapping ──

    // A View puts times exactly where xForTime always did, and inverts.
    void theViewIsTheSameMappingAndInverts() {
        for (int columns : {320, 1042}) {
            const double left = 8.0, width = double(columns);
            const Window w = fitLeftWindow(1'000'000, 1'300'000, columns, 4, 64);
            const View v = viewFor(w, left, width);
            for (int64_t t : {w.t0Ms, w.t0Ms + w.spanMs() / 3, w.t1Ms()}) {
                QVERIFY(std::abs(v.xOf(t) - xForTime(w, left, width, t)) < 1e-6);
                QVERIFY(std::abs(double(v.tOf(v.xOf(t)) - t)) <= double(w.stepMs));
            }
        }
    }

    // Following now, the picture hangs off the advancing edge: a push that
    // moves the bins forward changes nothing on screen, while time passing
    // moves everything — which is the motion the strip is after.
    void aPushMovesNothingButTimeDoes() {
        const int columns = 800;
        const double left = 8.0, width = double(columns);
        const int64_t now = 5'000'000, shown = now + 137;
        const Window bins = followWindow(120'000, now, columns);
        const Window later = followWindow(120'000, shown, columns);
        QCOMPARE(bins.stepMs, later.stepMs);

        const View before = followView(bins, shown, left, width);
        const View pushed = followView(later, shown, left, width);
        QVERIFY2(!viewsDiffer(before, pushed, left, width, 0.001), "a push moved the picture");

        const View onward = followView(bins, shown + 400, left, width);
        QVERIFY2(viewsDiffer(before, onward, left, width, 0.5), "time passing moved nothing");
    }

    // A rescale is blended: both ends are exact, it is monotone in between,
    // and it still inverts mid-blend.
    void aBlendedMappingPinsBothEnds() {
        const int columns = 640;
        const double left = 8.0, width = double(columns);
        const View a = viewFor(fitLeftWindow(0, 60'000, columns, 4, 64), left, width);
        const View b = viewFor(fitLeftWindow(0, 900'000, columns, 4, 64), left, width);
        const int64_t probe = 30'000;
        QVERIFY(std::abs(lerpView(a, b, 0.0).xOf(probe) - a.xOf(probe)) < 1e-9);
        QVERIFY(std::abs(lerpView(a, b, 1.0).xOf(probe) - b.xOf(probe)) < 1e-9);
        const bool rising = b.xOf(probe) > a.xOf(probe);
        double prev = a.xOf(probe);
        for (double u = 0.1; u <= 1.0001; u += 0.1) {
            const double x = lerpView(a, b, u).xOf(probe);
            QVERIFY2(rising ? x >= prev - 1e-9 : x <= prev + 1e-9, "the blend doubles back");
            prev = x;
        }
        const View mid = lerpView(a, b, 0.5);
        QVERIFY(std::abs(double(mid.tOf(mid.xOf(probe)) - probe)) <= 2.0 / mid.pxPerMs);
    }

    // The strip's own clock: still while nothing is written, forward with the
    // clock while recording, never backwards, never past its data.
    void nowAdvancesSmoothlyAndNeverRunsAway() {
        QCOMPARE(shownNow(1000, 500, false, 0, kMaxClockDriftMs), int64_t(1000));
        QCOMPARE(shownNow(1000, 250, true, 0, kMaxClockDriftMs), int64_t(1250));
        QCOMPARE(shownNow(1000, 0, true, 1200, kMaxClockDriftMs), int64_t(1200));
        QCOMPARE(shownNow(1000, 99'000, true, 0, kMaxClockDriftMs), int64_t(1000) + kMaxClockDriftMs);
    }

    void easingIsFlatAtBothEnds() {
        QVERIFY(qFuzzyIsNull(easeInOut(0.0)));
        QVERIFY(std::abs(easeInOut(1.0) - 1.0) < 1e-9);
        QVERIFY(easeInOut(0.05) < 0.05);     // starts gently
        QVERIFY(easeInOut(0.95) > 0.95);     // ends gently
        QVERIFY(std::abs(easeInOut(0.5) - 0.5) < 1e-9);
    }

    // The taller strip: lanes in order, inside it, and the data lane holds
    // most of the room.
    void theLanesFitTheTallerStrip() {
        QCOMPARE(kStripHeight, 43);
        QVERIFY(kGraphTop >= 0 && kGraphBottom <= kStripHeight);
        QVERIFY(kPinTop >= kGraphTop && kPinBottom <= kBandTop);
        QVERIFY(kBandTop < kBandBottom && kBandBottom <= kSelTop);
        QVERIFY(kSelTop < kSelBottom && kSelBottom <= kGraphBottom);
        QVERIFY2(kBandBottom - kBandTop > (kPinBottom - kPinTop) + (kSelBottom - kSelTop),
                 "the envelope is not the biggest lane");
        QVERIFY(kCellTop > 0 && kCellTop + kCellH <= kStripHeight);
        QVERIFY(std::abs((kStripHeight - kCellH) / 2 - kCellTop) <= 1);   // the button is centred
    }
};

QTEST_MAIN(TestTimelineStripModel)
#include "test_timeline_strip_model.moc"
