#pragma once

// ── The timeline strip's geometry and time math, without a widget ──
//
// Everything the strip decides that can be decided without painting lives
// here, so a QtCore-only test can pin it: where the graph, its lanes and the
// overflow button sit, how time maps to columns, how the window frames the
// whole recording and follows "now" without shimmering, how zoom keeps the
// moment under the pointer still, how tall a column of changes is, and how
// the hover readout's clock reads.
//
// The strip is the graph and one overflow button — nothing else. Record /
// Stop, and Back to live while looking back, sit beside the address, where
// the moment being shown is named; the strip only answers "when". The layout is a function of the
// width alone, so no state change can move anything.

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace rcx::timeline {

// ── Metrics (logical px) ──
// 43: four lanes need the room, and the envelope is the point of the strip —
// at 32 it was a sliver. Still only about two editor lines under the pane tabs.
inline constexpr int kStripHeight  = 43;
inline constexpr int kCellTop      = 10;   // the square button, centred in the strip
inline constexpr int kCellH        = 22;
inline constexpr int kGutter       = 8;    // first ink of the strip (paintutil.h kGutter)
inline constexpr int kIconCellW    = 22;   // square hit area, as the address bar's buttons
inline constexpr int kIconPx       = 14;
inline constexpr int kCellGap      = 2;
inline constexpr int kRightMargin  = 6;
inline constexpr int kGraphTop     = 2;
inline constexpr int kGraphBottom  = 41;   // exclusive; the strip's bottom seam is row 42
// The graph's lanes, top to bottom (strip rows, bottom exclusive). The band
// takes most of the extra height: it carries the data.
inline constexpr int kPinTop       = 2;    // beads: a field started changing
inline constexpr int kPinBottom    = 11;
inline constexpr int kBandTop      = 12;   // the envelope over the recorded band
inline constexpr int kBandBottom   = 36;   // the baseline is the last row of the band
inline constexpr int kSelTop       = 37;   // the selected fields
inline constexpr int kSelBottom    = 41;
inline constexpr int kSnapPx       = 4;
inline constexpr int64_t kMinSpanMs = 2000;
// The shortest span a FOLLOWING view will draw. kMinSpanMs is the floor for
// zooming and for framing a range; a recording two seconds old must already
// fill the bar, not sit in the last tenth of it.
inline constexpr int64_t kMinFollowSpanMs = 1000;
// Framing the whole recording: a little room before it, and room after it to
// grow into while it records, so the scale steps now and then, not every
// tick. The same room in every state: pressing Stop rescales nothing.
inline constexpr int kLeadPx       = 2;
inline constexpr int kTailPx       = 16;   // room for the writing edge, no more
// Two records join into one envelope when at most this far apart (a few
// capture ticks) — a longer quiet stretch drops to the baseline.
inline constexpr int64_t kJoinMs   = 1200;

enum class Capture { None, Static, Rolling, Recording, Paused };

// ── Time → columns ──

// Milliseconds per column. A fixed ladder, so the scale changes in steps and
// a window that follows "now" slides by whole columns: a bin's contents never
// change as time advances, so the graph never shimmers.
// Dense enough (about 25 % a rung) that framing a recording leaves little
// empty paper after the rounding.
inline const QVector<int64_t>& stepLadder() {
    static const QVector<int64_t> ladder = {
        1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50, 60, 80,
        100, 120, 150, 200, 250, 300, 400, 500, 600, 800,
        1000, 1200, 1500, 2000, 2500, 3000, 4000, 5000, 6000, 8000,
        10000, 15000, 20000, 30000, 60000, 120000, 300000, 600000, 1800000, 3600000,
    };
    return ladder;
}

inline int64_t stepForSpan(int64_t spanMs, int columns) {
    if (columns <= 0) return stepLadder().last();
    const int64_t need = (std::max<int64_t>(spanMs, 1) + columns - 1) / columns;
    for (int64_t s : stepLadder())
        if (s >= need) return s;
    return stepLadder().last();
}

inline int64_t floorTo(int64_t v, int64_t step) {
    int64_t q = v / step;
    if (v % step != 0 && v < 0) --q;
    return q * step;
}
inline int64_t ceilTo(int64_t v, int64_t step) {
    return -floorTo(-v, step);
}

struct Window {
    int64_t t0Ms = 0;         // left edge, always a multiple of stepMs
    int64_t stepMs = 1000;    // milliseconds per column
    int     columns = 0;
    bool    follow = true;    // the right edge tracks "now"
    int64_t spanMs() const { return stepMs * columns; }
    int64_t t1Ms() const { return t0Ms + spanMs(); }
    bool operator==(const Window& o) const {
        return t0Ms == o.t0Ms && stepMs == o.stepMs && columns == o.columns && follow == o.follow;
    }
};

// The window that ends at "now" and shows about `spanMs`.
inline Window followWindow(int64_t spanMs, int64_t nowMs, int columns) {
    Window w;
    w.columns = std::max(columns, 1);
    w.stepMs = stepForSpan(spanMs, w.columns);
    w.t0Ms = ceilTo(nowMs, w.stepMs) - w.stepMs * w.columns;
    w.follow = true;
    return w;
}

// Everything from `beginMs` to `endMs`, right edge at `endMs`. Rounding
// both edges onto the grid can need one more rung of the ladder than the raw
// span suggests; take it rather than cut off the beginning.
inline Window fitWindow(int64_t beginMs, int64_t endMs, int columns) {
    Window w;
    w.columns = std::max(columns, 1);
    w.follow = true;
    const int64_t span = std::max<int64_t>(endMs - beginMs, kMinSpanMs);
    const QVector<int64_t>& ladder = stepLadder();
    int rung = int(std::lower_bound(ladder.cbegin(), ladder.cend(), stepForSpan(span, w.columns)) - ladder.cbegin());
    for (;; ++rung) {
        w.stepMs = ladder[std::min(rung, int(ladder.size()) - 1)];
        w.t0Ms = ceilTo(endMs, w.stepMs) - w.stepMs * w.columns;
        if (w.t0Ms <= beginMs || rung >= int(ladder.size()) - 1) break;
    }
    return w;
}

// The whole of [beginMs, endMs] starting `leadCols` columns in from the left,
// with at least `tailCols` columns left over on the right: the smallest step
// that fits. Both edges are on the grid.
inline Window fitLeftWindow(int64_t beginMs, int64_t endMs, int columns, int leadCols, int tailCols) {
    Window w;
    w.columns = std::max(columns, 1);
    w.follow = true;
    const int lead = std::max(0, leadCols);
    const int usable = std::max(1, w.columns - lead - std::max(0, tailCols));
    const int64_t span = std::max<int64_t>(endMs - beginMs, kMinSpanMs);
    const QVector<int64_t>& ladder = stepLadder();
    int rung = int(std::lower_bound(ladder.cbegin(), ladder.cend(), stepForSpan(span, usable)) - ladder.cbegin());
    for (;; ++rung) {
        w.stepMs = ladder[std::min(rung, int(ladder.size()) - 1)];
        w.t0Ms = floorTo(beginMs, w.stepMs) - w.stepMs * lead;
        if (w.t1Ms() >= endMs || rung >= int(ladder.size()) - 1) break;
    }
    return w;
}

inline double xForTime(const Window& w, double graphLeft, double graphWidth, int64_t t) {
    if (w.spanMs() <= 0) return graphLeft;
    return graphLeft + (double(t - w.t0Ms) / double(w.spanMs())) * graphWidth;
}

inline int64_t timeForX(const Window& w, double graphLeft, double graphWidth, double x) {
    if (graphWidth <= 0) return w.t0Ms;
    const double frac = (x - graphLeft) / graphWidth;
    return w.t0Ms + int64_t(std::llround(frac * double(w.spanMs())));
}

// Zoom by `factor` (< 1 zooms in) keeping `anchorMs` under the same column.
// The result is clamped to [kMinSpanMs, everything retained] and never follows.
inline Window zoomAround(const Window& w, int64_t anchorMs, double factor,
                         int64_t loMs, int64_t hiMs) {
    Window out = w;
    out.follow = false;
    if (w.columns <= 0 || w.spanMs() <= 0) return out;
    const double frac = std::clamp(double(anchorMs - w.t0Ms) / double(w.spanMs()), 0.0, 1.0);
    const int64_t maxSpan = std::max<int64_t>(hiMs - loMs, kMinSpanMs);
    int64_t wanted = int64_t(double(w.spanMs()) * factor);
    wanted = std::clamp<int64_t>(wanted, kMinSpanMs, std::max<int64_t>(maxSpan, kMinSpanMs));
    out.stepMs = stepForSpan(wanted, w.columns);
    // The ladder has gaps (200 → 500 ms, 2 → 5 s …) wider than one wheel
    // notch, so a zoom can round straight back onto the rung it started from
    // and stick there. A zoom always moves at least one rung — then the
    // span limits have the last word.
    {
        const QVector<int64_t>& ladder = stepLadder();
        const int last = int(ladder.size()) - 1;
        const int cur = std::min(last, int(std::lower_bound(ladder.cbegin(), ladder.cend(), w.stepMs) - ladder.cbegin()));
        int rung = std::min(last, int(std::lower_bound(ladder.cbegin(), ladder.cend(), out.stepMs) - ladder.cbegin()));
        if (factor < 1.0 && rung >= cur && cur > 0) rung = cur - 1;
        if (factor > 1.0 && rung <= cur && cur < last) rung = cur + 1;
        while (rung < last && ladder[rung] * w.columns < kMinSpanMs) ++rung;
        while (rung > 0 && ladder[rung - 1] * w.columns >= maxSpan) --rung;
        out.stepMs = ladder[rung];
    }
    // Place t0 so the anchor keeps its fraction of the width, on the grid.
    const int64_t rawT0 = anchorMs - int64_t(frac * double(out.stepMs * w.columns));
    out.t0Ms = floorTo(rawT0, out.stepMs);
    // Keep the view on retained time where possible.
    const int64_t span = out.spanMs();
    if (out.t0Ms + span > ceilTo(hiMs, out.stepMs)) out.t0Ms = ceilTo(hiMs, out.stepMs) - span;
    if (out.t0Ms < floorTo(loMs, out.stepMs) && span <= (hiMs - loMs) + out.stepMs)
        out.t0Ms = floorTo(loMs, out.stepMs);
    return out;
}

// Pan by `dtMs` (positive: later), clamped to retained time. Reaching "now"
// re-enables following.
inline Window panBy(const Window& w, int64_t dtMs, int64_t loMs, int64_t hiMs) {
    Window out = w;
    const int64_t span = w.spanMs();
    // A window that already shows everything has nowhere to pan to.
    if (w.t0Ms <= floorTo(loMs, w.stepMs) && w.t1Ms() >= ceilTo(hiMs, w.stepMs)) return out;
    // A following window may end past now (the room kept on the right):
    // panning never pulls it back toward now.
    const int64_t right = w.follow ? std::max(ceilTo(hiMs, w.stepMs), w.t1Ms()) : ceilTo(hiMs, w.stepMs);
    int64_t t0 = floorTo(w.t0Ms + dtMs, w.stepMs);
    const int64_t minT0 = std::min(floorTo(loMs, w.stepMs), right - span);
    t0 = std::clamp(t0, minT0, right - span);
    out.t0Ms = t0;
    out.follow = (t0 + span >= right);
    return out;
}

// The change tick nearest `t` within `tolMs`, else `t` itself. `ticks` sorted.
inline int64_t snapToChange(int64_t t, const QVector<int64_t>& ticks, int64_t tolMs) {
    if (ticks.isEmpty() || tolMs <= 0) return t;
    auto it = std::lower_bound(ticks.cbegin(), ticks.cend(), t);
    int64_t best = t;
    int64_t bestD = tolMs + 1;
    if (it != ticks.cend() && *it - t < bestD) { best = *it; bestD = *it - t; }
    if (it != ticks.cbegin() && t - *(it - 1) < bestD) { best = *(it - 1); bestD = t - *(it - 1); }
    return bestD <= tolMs ? best : t;
}

// ── The painted mapping ──
//
// A Window says which time each COLUMN holds — that is binning, and it stays
// on the ladder so a bin's contents never change as time passes. A View says
// where a time is drawn, as an affine map, so the picture can keep moving
// between pushes (and a scale change can be animated) without re-binning
// anything: x = x0 + pxPerMs * (t - tRef).
struct View {
    double  pxPerMs = 0;
    double  x0 = 0;
    int64_t tRef = 0;

    double xOf(int64_t t) const { return x0 + pxPerMs * double(t - tRef); }
    int64_t tOf(double x) const {
        return pxPerMs > 0 ? tRef + int64_t(std::llround((x - x0) / pxPerMs)) : tRef;
    }
};

// Where `w` puts its times: the same mapping xForTime has always made.
inline View viewFor(const Window& w, double graphLeft, double graphWidth) {
    View v;
    v.pxPerMs = w.spanMs() > 0 ? graphWidth / double(w.spanMs()) : 0.0;
    v.x0 = graphLeft;
    v.tRef = w.t0Ms;
    return v;
}

// The same scale, pinned to a continuously advancing "now" at the right edge:
// what lets a following graph glide between pushes instead of stepping.
inline View followView(const Window& w, int64_t shownNowMs, double graphLeft, double graphWidth) {
    View v = viewFor(w, graphLeft, graphWidth);
    v.x0 = graphLeft + graphWidth;
    v.tRef = shownNowMs;
    return v;
}

// Smoothstep: flat at both ends, so an animation starts and stops without a
// visible kick.
inline double easeInOut(double u) {
    u = std::clamp(u, 0.0, 1.0);
    return u * u * (3.0 - 2.0 * u);
}

// Blend two mappings. Both are affine in t, so the blend is affine too — a
// rescale animates without touching the ladder or re-binning a single column.
inline View lerpView(const View& a, const View& b, double u) {
    u = std::clamp(u, 0.0, 1.0);
    View v;
    v.tRef = b.tRef;
    v.pxPerMs = a.pxPerMs + (b.pxPerMs - a.pxPerMs) * u;
    const double ax = a.xOf(b.tRef);
    v.x0 = ax + (b.x0 - ax) * u;
    return v;
}

// "Now", advanced by the strip's own clock between pushes: monotone, and
// never more than `maxDriftMs` past the last state it was told about — a
// stalled push freezes the graph rather than running it past its own data.
inline int64_t shownNow(int64_t stateNowMs, int64_t sinceMs, bool advancing,
                        int64_t lastShownMs, int64_t maxDriftMs) {
    if (!advancing) return stateNowMs;
    const int64_t t = stateNowMs + std::clamp<int64_t>(sinceMs, 0, std::max<int64_t>(maxDriftMs, 0));
    return std::max(t, lastShownMs);
}

// Do two mappings put the same moments in noticeably different places? A pure
// slide answers false — the follow view moves with now, so every datum keeps
// its x — while a rung change or a retention trim answers true. This is the
// line between "nothing to animate" and "animate this".
inline bool viewsDiffer(const View& a, const View& b, double graphLeft, double graphWidth,
                        double tolPx) {
    if (a.pxPerMs <= 0 || b.pxPerMs <= 0) return true;
    for (double x : {graphLeft, graphLeft + graphWidth}) {
        const int64_t t = a.tOf(x);
        if (std::abs(a.xOf(t) - b.xOf(t)) > tolPx) return true;
    }
    return false;
}

// Columns rendered past the right edge, so a following graph can glide for a
// while before the picture must be drawn again.
// How much history the graph keeps behind "now" while it follows: enough to
// read a run of changes, short enough that records stay far apart on screen.
inline constexpr int64_t kDefaultFollowMs = 60'000;
// More records than this in view and they are binned into columns instead of
// drawn one by one.
inline constexpr int     kMaxDirectPoints = 4000;
inline constexpr int     kImageMarginCols = 64;
inline constexpr int     kLeadFadePx      = 10;    // the writing edge fades in over this
inline constexpr int     kScaleAnimMs     = 180;
inline constexpr int64_t kMaxClockDriftMs = 1500;

// ── Column heights ──

// A power-of-two vertical scale with hysteresis: it grows at once, but only
// shrinks when the visible peak falls below a quarter of it — so a burst
// scrolling out of view does not rescale everything each tick.
inline uint32_t domainFor(uint32_t visibleMax, uint32_t prevDomain) {
    uint32_t need = 4;
    while (need < visibleMax && need < (1u << 31)) need <<= 1;
    if (prevDomain >= need && visibleMax >= prevDomain / 4) return prevDomain;
    return need;
}

// sqrt scale: a handful of changes stays visible next to a burst of
// thousands. Any change is at least one device row.
inline int columnHeightDev(uint32_t value, uint32_t domain, int bandDev) {
    if (value == 0 || bandDev <= 0) return 0;
    const double frac = std::sqrt(double(std::min(value, domain)) / double(std::max<uint32_t>(domain, 1)));
    return std::clamp(int(std::lround(frac * bandDev)), 1, bandDev);
}

// ── What a moment is ──

struct Span {
    int64_t startMs = 0;
    int64_t endMs = -1;   // < 0: still open
    bool contains(int64_t t) const { return t >= startMs && (endMs < 0 || t < endMs); }
};

enum class Region { NotRetained, Gap, Captured };

// Precedence: forgotten (or future) beats "not watched" beats "watched".
inline Region regionAt(int64_t t, int64_t retainedBeginMs, int64_t nowMs, const QVector<Span>& gaps) {
    if (t < retainedBeginMs || t > nowMs) return Region::NotRetained;
    for (const Span& g : gaps)
        if (g.contains(t)) return Region::Gap;
    return Region::Captured;
}

// ── Text ──

// "now", "−42s", "−3m 12s", "−1h 04m". The minus is U+2212, not a hyphen.
inline QString formatAgo(int64_t ms) {
    if (ms < 1000) return QStringLiteral("now");
    const int64_t s = ms / 1000;
    const QChar minus(0x2212);
    if (s < 60) return QStringLiteral("%1%2s").arg(minus).arg(s);
    if (s < 3600) return QStringLiteral("%1%2m %3s").arg(minus).arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1%2h %3m").arg(minus).arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0'));
}

// "12:04:31.2" in local time.
inline QString formatClock(int64_t epochMs, int utcOffsetSeconds) {
    int64_t local = epochMs + int64_t(utcOffsetSeconds) * 1000;
    int64_t dayMs = local % 86'400'000;
    if (dayMs < 0) dayMs += 86'400'000;
    const int h = int(dayMs / 3'600'000);
    const int m = int((dayMs / 60'000) % 60);
    const int s = int((dayMs / 1000) % 60);
    const int tenth = int((dayMs % 1000) / 100);
    return QStringLiteral("%1:%2:%3.%4").arg(h, 2, 10, QLatin1Char('0'))
                                        .arg(m, 2, 10, QLatin1Char('0'))
                                        .arg(s, 2, 10, QLatin1Char('0'))
                                        .arg(tenth);
}

// ── Layout ──

struct LaidCell {
    QString id;
    QRect   rect;
};

struct Layout {
    QVector<LaidCell> cells;
    QRect graph;

    bool has(const QString& id) const {
        for (const LaidCell& c : cells) if (c.id == id) return true;
        return false;
    }
    QRect rect(const QString& id) const {
        for (const LaidCell& c : cells) if (c.id == id) return c.rect;
        return {};
    }
};

// The graph from the gutter to the overflow button, which is a square cell
// at the right margin. A function of the width alone.
inline Layout layoutStrip(int width) {
    Layout L;
    // The graph takes the WHOLE bar. Reserving a lane of chrome for the
    // overflow button left the trace short of both edges, which is the first
    // thing the eye catches; the button now floats over the graph's right end
    // behind a scrim, and cellIdAt still tests cells before the graph, so
    // hit-testing is unchanged.
    const int moreLeft = std::max(0, width - kRightMargin - kIconCellW);
    L.cells.append({QStringLiteral("more"), QRect(moreLeft, kCellTop, kIconCellW, kCellH)});
    L.graph = QRect(0, kGraphTop, std::max(0, width), kGraphBottom - kGraphTop);
    return L;
}

inline QString cellIdAt(const Layout& L, const QPoint& p) {
    for (const LaidCell& c : L.cells)
        if (c.rect.contains(p)) return c.id;
    if (L.graph.contains(QPoint(p.x(), L.graph.center().y())) && p.y() >= 0 && p.y() < kStripHeight)
        return QStringLiteral("graph");
    return {};
}

} // namespace rcx::timeline
