// The timeline strip as a widget: its controls, its scrubbing, and the
// pixels that carry meaning (the checked underline, the amber playhead, no
// destructive red at rest). The geometry and time math behind it are pinned
// headless in test_timeline_strip_model.

#include <QtTest/QTest>
#include <QApplication>
#include <QImage>
#include <QMouseEvent>

#include "themes/theme.h"
#include "widgets/chrome_fallback_theme.h"
#include "widgets/timeline_strip.h"

using namespace rcx;

namespace {

TimelineStripState rollingState(int64_t now = 600'000) {
    TimelineStripState s;
    s.capture = timeline::Capture::Rolling;
    s.hasData = true;
    s.nowMs = now;
    s.retainedBeginMs = now - 300'000;
    s.rollingWindowMs = 1'800'000;
    s.dataGeneration = 1;
    return s;
}

// Any pixel within `tol` (sum of channel differences) of `c` — for text,
// whose antialiased edges never hit the exact colour.
bool imageHasNear(const QImage& img, const QRect& devRect, const QColor& c, int tol) {
    const QRect r = devRect.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QColor p = img.pixelColor(x, y);
            if (std::abs(p.red() - c.red()) + std::abs(p.green() - c.green()) + std::abs(p.blue() - c.blue()) <= tol)
                return true;
        }
    return false;
}

bool imageHasColour(const QImage& img, const QRect& devRect, const QColor& c) {
    const QRect r = devRect.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (img.pixelColor(x, y).rgb() == c.rgb()) return true;
    return false;
}

QRect toDevice(const QRect& logical, qreal dpr) {
    return QRect(int(std::floor(logical.left() * dpr)), int(std::floor(logical.top() * dpr)),
                 int(std::ceil(logical.width() * dpr)), int(std::ceil(logical.height() * dpr)));
}

} // namespace

class TestTimelineStrip : public QObject {
    Q_OBJECT
private:
    QWidget* m_host = nullptr;
    TimelineStrip* m_strip = nullptr;
    Theme m_theme;
    int m_clear = 0, m_live = 0, m_hide = 0;
    QVector<QPair<int64_t, bool>> m_scrubs;
    // The strip runs on its own clock between pushes; drive it by hand so no
    // frame depends on wall time.
    int64_t m_clockMs = 1'000'000;
    int64_t m_tickMs = INT64_MIN;   // the one change a snap could pull onto
    bool m_burst = false;           // the scale test's tall record

    // `withPoints`: install a record feed, so the test drives the renderer
    // that actually ships (paintPointGraph). Without it ensurePoints finds no
    // callback and the strip falls back to the binned picture — which is what
    // every older pixel test here exercises. Do not add points to the default.
    void make(int width = 1080, bool withPoints = false, int64_t tickMs = 200) {
        m_clear = m_live = m_hide = 0;
        m_scrubs.clear();
        m_host = new QWidget;
        m_host->resize(width, TimelineStrip::kHeight);
        m_strip = new TimelineStrip(m_host);
        m_strip->setGeometry(0, 0, width, TimelineStrip::kHeight);
        m_theme = chromeFallbackTheme();
        m_strip->applyTheme(m_theme);
        m_clockMs = 1'000'000;
        m_tickMs = INT64_MIN;
        m_strip->setClockForTest([this] { return m_clockMs; });
        TimelineStrip::Callbacks cb;
        cb.onClear = [this] { ++m_clear; };
        cb.onReturnToLive = [this] { ++m_live; };
        cb.onHide = [this] { ++m_hide; };
        cb.onScrub = [this](int64_t t, bool final) { m_scrubs.append({t, final}); };
        cb.columns = [](int64_t t0, int64_t step, int n, QVector<TimelineColumn>& out) {
            out.resize(n);
            for (int i = 0; i < n; ++i) {
                const int64_t t = t0 + step * i;
                out[i].maxChanged = (t / 1000) % 17 == 0 ? 40 : ((t / 1000) % 5 == 0 ? 3 : 0);
                out[i].records = out[i].maxChanged ? 1 : 0;
            }
        };
        cb.changeTicks = [this](int64_t t0, int64_t t1) {
            QVector<int64_t> out;
            if (m_tickMs >= t0 && m_tickMs < t1) out.append(m_tickMs);
            return out;
        };
        if (withPoints) {
            cb.points = [tickMs](int64_t t0, int64_t t1, int cap, QVector<TimelinePoint>& out) {
                out.clear();
                for (int64_t t = ((t0 + tickMs - 1) / tickMs) * tickMs; t < t1; t += tickMs) {
                    if (out.size() >= cap) return false;
                    out.append(TimelinePoint{t, uint32_t(4 + (t / tickMs) % 3),
                                             (t / tickMs) % 5 == 0 ? 2u : 0u, false});
                }
                return true;
            };
        }
        m_strip->setCallbacks(cb);
        m_host->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_host));
    }

    void send(QEvent::Type type, const QPoint& pos, Qt::MouseButton button, Qt::MouseButtons buttons,
              Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QMouseEvent e(type, QPointF(pos), QPointF(m_strip->mapToGlobal(pos)), button, buttons, mods);
        QApplication::sendEvent(m_strip, &e);
    }
    void click(const QPoint& pos) {
        send(QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
    }

private slots:
    void cleanup() {
        delete m_host;
        m_host = nullptr;
        m_strip = nullptr;
    }

    // Nothing moves when state changes: every cell sits exactly where it
    // did, whatever capture is doing and whether the view is live or past.
    void nothingMovesBetweenStates() {
        make(1080);
        QVector<TimelineStripState> states;
        states << rollingState();
        TimelineStripState rec = rollingState(); rec.capture = timeline::Capture::Recording; rec.recordBeginMs = 400'000; states << rec;
        TimelineStripState paused = rollingState(); paused.capture = timeline::Capture::Paused; states << paused;
        TimelineStripState past = rollingState(); past.past = true; past.viewedMs = 500'000; states << past;
        TimelineStripState stat; stat.capture = timeline::Capture::Static; states << stat;
        const QStringList ids = {QStringLiteral("graph"), QStringLiteral("more")};
        QHash<QString, QRect> first;
        for (int i = 0; i < states.size(); ++i) {
            m_strip->setState(states[i]);
            QApplication::processEvents();
            for (const QString& id : ids) {
                const QRect r = m_strip->itemRect(id);
                if (i == 0) first.insert(id, r);
                else QVERIFY2(r == first.value(id), qPrintable(QStringLiteral("%1 moved in state %2").arg(id).arg(i)));
            }
        }
    }

    // The strip is the graph and one square overflow button. Record, Pause
    // and the rest live beside the address now.
    void theStripIsTheGraphAndOneButton() {
        make();
        m_strip->setState(rollingState());
        QCOMPARE(m_strip->layout().cells.size(), 1);
        const QRect more = m_strip->itemRect(QStringLiteral("more"));
        QCOMPARE(more.width(), more.height());
        QCOMPARE(more.right() + 1 + timeline::kRightMargin, m_strip->width());
        for (const char* gone : {"rec", "pause", "reset", "state", "range.lo", "range.hi", "live"})
            QVERIFY2(m_strip->itemRect(QString::fromLatin1(gone)).isNull(), gone);
        QCOMPARE(m_strip->graphRect().left(), 0);
        QCOMPARE(m_strip->graphRect().right() + 1, m_strip->width());
        QCOMPARE(timeline::cellIdAt(m_strip->layout(), QPoint(timeline::kGutter + 11, 13)), QStringLiteral("graph"));
    }

    void aStaticSourceCannotBeScrubbed() {
        make();
        TimelineStripState s;
        s.capture = timeline::Capture::Static;
        m_strip->setState(s);
        click(m_strip->itemRect(QStringLiteral("graph")).center());
        QVERIFY(m_scrubs.isEmpty());
        QCOMPARE(m_live, 0);
    }

    // Back to live, the graph follows now again however it was framed —
    // there is no Follow switch to forget.
    void returningLiveFollowsNowAgain() {
        make();
        const TimelineStripState live = rollingState();
        m_strip->setState(live);
        m_strip->showRange(live.nowMs - 200'000, live.nowMs - 150'000);
        QVERIFY(!m_strip->window().follow);
        TimelineStripState past = live;
        past.past = true;
        past.viewedMs = live.nowMs - 170'000;
        m_strip->setState(past);
        QVERIFY(!m_strip->window().follow);             // parked while viewing history
        m_strip->setState(live);
        QVERIFY(m_strip->window().follow);
        QVERIFY(m_strip->window().t1Ms() >= live.nowMs);
    }

    // A wheel zoom while live keeps following now; parked in the past it
    // zooms about the pointer and stays put.
    void zoomingWhileLiveKeepsFollowing() {
        make();
        TimelineStripState s = rollingState();
        m_strip->setState(s);
        const QPoint at = m_strip->graphRect().center();
        auto wheel = [&](int dy) {
            QWheelEvent e(QPointF(at), QPointF(m_strip->mapToGlobal(at)), QPoint(), QPoint(0, dy),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(m_strip, &e);
        };
        const int64_t spanBefore = m_strip->window().spanMs();
        wheel(120);
        QVERIFY(m_strip->window().follow);
        QVERIFY(m_strip->window().spanMs() < spanBefore);
        s.nowMs += 60'000;
        m_strip->setState(s);
        QVERIFY(m_strip->window().t1Ms() >= s.nowMs);

        TimelineStripState past = s;
        past.past = true;
        past.viewedMs = s.nowMs - 120'000;
        m_strip->setState(past);
        wheel(120);
        QVERIFY(!m_strip->window().follow);
    }

    // The overflow menu holds exactly the reduced set, in plain words.
    void theMenuIsReduced() {
        make();
        m_strip->setState(rollingState());
        const QStringList expected = {
            QStringLiteral("Previous Change\tCtrl+,"), QStringLiteral("Next Change\tCtrl+."),
            QStringLiteral("Show Whole Recording"),
            QStringLiteral("Clear Recording…"), QStringLiteral("Hide Timeline")};
        QCOMPARE(m_strip->menuTextsForTest(), expected);
    }

    // Nothing recorded yet: the graph says how to start, in words, and no
    // time is written on it anywhere. Recording with nothing yet says so.
    void anEmptyTimelineSaysPressRecord() {
        make();
        m_strip->setCallbacks(TimelineStrip::Callbacks{});   // no columns: only words ink the graph
        TimelineStripState s = rollingState();
        s.hasData = false;
        m_strip->setState(s);
        QCOMPARE(m_strip->emptyMessage(), QStringLiteral("Press Record to capture changes over time"));
        QImage img = m_strip->grab().toImage();
        const qreal dpr = img.devicePixelRatio();
        QRect gd = toDevice(m_strip->graphRect(), dpr);
        // The graph runs the whole bar now, with the ⋯ button floating over
        // its right end: stop before the button, or its glyph reads as words
        // written on the graph.
        gd.setRight(int((m_strip->itemRect(QStringLiteral("more")).left() - 10) * dpr));
        // The middle of the graph: clear of the markers' top ticks and the baseline.
        const QRect g(gd.left(), gd.top() + int(5 * dpr), gd.width(), gd.height() - int(10 * dpr));
        QVERIFY2(imageHasNear(img, g, m_theme.textDim, 60), "the empty timeline says nothing");
        s.capture = timeline::Capture::Recording;
        m_strip->setState(s);
        QVERIFY(m_strip->emptyMessage().startsWith(QStringLiteral("Recording")));
        // With data there are no words at all: no time marks, no labels.
        s = rollingState();
        m_strip->setState(s);
        QVERIFY(m_strip->emptyMessage().isEmpty());
        img = m_strip->grab().toImage();
        QVERIFY2(!imageHasNear(img, g, m_theme.textDim, 60), "something is written on the graph");
    }

    // A drag fires far more moves than can be served: the strip sends the
    // latest, once per event-loop turn, then a final value on release.
    void scrubIsLatestWinsWithAFinalOnRelease() {
        make();
        m_strip->setState(rollingState());
        const QRect g = m_strip->graphRect();
        const int y = g.center().y();
        send(QEvent::MouseButtonPress, QPoint(g.left() + 100, y), Qt::LeftButton, Qt::LeftButton);
        QVERIFY(m_strip->isDragging());
        for (int i = 1; i <= 10; ++i)
            send(QEvent::MouseMove, QPoint(g.left() + 100 + i * 20, y), Qt::NoButton, Qt::LeftButton);
        QApplication::processEvents();
        const QPoint end(g.left() + 300, y);
        send(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        QVERIFY(!m_strip->isDragging());
        QVERIFY(!m_scrubs.isEmpty());
        int nonFinal = 0;
        for (const auto& s : m_scrubs) if (!s.second) ++nonFinal;
        QVERIFY2(nonFinal <= 2, qPrintable(QStringLiteral("%1 intermediate scrubs for one turn").arg(nonFinal)));
        QVERIFY(m_scrubs.last().second);
        const timeline::Window w = m_strip->window();
        const TimelineStripState st = m_strip->state();
        // A moment before what is retained is clamped to the oldest one kept.
        const int64_t expected = std::clamp(timeline::timeForX(w, g.left(), g.width(), end.x()),
                                            st.retainedBeginMs, st.nowMs);
        QVERIFY(std::abs(m_scrubs.last().first - expected) <= w.stepMs);
        QCOMPARE(m_live, 0);
    }

    void releasingAtTheLiveEdgeReturnsToLive() {
        make();
        m_strip->setState(rollingState());
        const QRect g = m_strip->graphRect();
        send(QEvent::MouseButtonPress, QPoint(g.center().x(), g.center().y()), Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, QPoint(g.right(), g.center().y()), Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, QPoint(g.right(), g.center().y()), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(m_live, 1);
        for (const auto& s : m_scrubs) QVERIFY(!s.second);
    }

    void rightPressCancelsAScrub() {
        make();
        m_strip->setState(rollingState());
        const QRect g = m_strip->graphRect();
        send(QEvent::MouseButtonPress, QPoint(g.left() + 50, g.center().y()), Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, QPoint(g.left() + 150, g.center().y()), Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonPress, QPoint(g.left() + 150, g.center().y()), Qt::RightButton,
             Qt::LeftButton | Qt::RightButton);
        QVERIFY(!m_strip->isDragging());
        QCOMPARE(m_live, 1);   // it was live before the press: back to live
    }

    // A short recording fills the graph from the left — not a sliver jammed
    // against the right edge with paper everywhere else.
    void aShortRecordingFillsTheGraph() {
        make();
        TimelineStripState s = rollingState();
        s.retainedBeginMs = s.nowMs - 7'000;
        m_strip->setState(s);
        const timeline::Window w = m_strip->window();
        const QRect g = m_strip->graphRect();
        const double left = timeline::xForTime(w, g.left(), g.width(), s.retainedBeginMs);
        const double right = timeline::xForTime(w, g.left(), g.width(), s.nowMs);
        QVERIFY2(left < g.left() + 12, qPrintable(QString::number(left)));
        QVERIFY2(right - left >= 0.75 * g.width(), qPrintable(QString::number(right - left)));
    }

    // Records far sparser than pixels still draw one continuous shape: every
    // column between them carries the envelope's edge — no comb of 1-px ticks
    // with paper in between.
    void sparseRecordsJoinIntoOneShape() {
        make();
        TimelineStripState s = rollingState();
        s.retainedBeginMs = s.nowMs - 10'000;
        TimelineStrip::Callbacks cb;
        cb.columns = [](int64_t t0, int64_t step, int n, QVector<TimelineColumn>& out) {
            out.resize(n);
            for (int i = 0; i < n; ++i) {             // a record every 200 ms, 12 fields each
                const int64_t a = t0 + step * i;
                const int64_t first = ((a + 199) / 200) * 200;
                if (first < a + step) { out[i].maxChanged = 12; out[i].records = 1; }
            }
        };
        m_strip->setCallbacks(cb);
        m_strip->setState(s);
        const QImage img = m_strip->grab().toImage();
        const qreal dpr = img.devicePixelRatio();
        const QRect g = m_strip->graphRect();
        const timeline::Window w = m_strip->window();
        const QColor line = m_strip->envelopeLineColour();
        const int x0 = int(timeline::xForTime(w, g.left(), g.width(), s.nowMs - 7'000) * dpr);
        const int x1 = int(timeline::xForTime(w, g.left(), g.width(), s.nowMs - 3'000) * dpr);
        const int top = int(timeline::kBandTop * dpr), bottom = int(timeline::kBandBottom * dpr);
        int bare = 0;
        for (int x = x0; x <= x1; ++x)
            if (!imageHasNear(img, QRect(x, top, 1, bottom - top), line, 120)) ++bare;
        QVERIFY2(bare == 0, qPrintable(QStringLiteral("%1 of %2 columns without the envelope").arg(bare).arg(x1 - x0 + 1)));
    }

    // The strip spends no accent colour, recording or not: red, green and
    // amber belong to the buttons beside the address (amber also marks the
    // past's playhead, which these states do not have). The envelope is data,
    // in the number ink.
    void theStripSpendsNoAccents() {
        make();
        for (timeline::Capture capture : {timeline::Capture::Rolling, timeline::Capture::Recording}) {
            TimelineStripState s = rollingState();
            s.capture = capture;
            if (capture == timeline::Capture::Recording) s.recordBeginMs = 450'000;
            m_strip->setState(s);
            const QImage img = m_strip->grab().toImage();
            for (const QColor& c : {m_theme.markerPtr, m_theme.indHintGreen, m_theme.indHoverSpan, m_theme.focusGlow})
                QVERIFY2(!imageHasColour(img, img.rect(), c), qPrintable(c.name()));
        }
    }

    void thePastHasAnAmberPlayhead() {
        make();
        TimelineStripState past = rollingState();
        past.past = true;
        past.viewedMs = past.nowMs - 60'000;
        m_strip->setState(past);
        const double x = m_strip->playheadX();
        QVERIFY(x > m_strip->graphRect().left() && x < m_strip->graphRect().right());
        const QImage img = m_strip->grab().toImage();
        const qreal dpr = img.devicePixelRatio();
        const int dx = int(std::floor(x * dpr + 0.5));
        QVERIFY2(imageHasColour(img, QRect(dx - 1, int(8 * dpr), 3, int(8 * dpr)), m_theme.focusGlow),
                 "no focusGlow playhead column");

        m_strip->setState(rollingState());
        QCOMPARE(m_strip->playheadX(), -1.0);
    }

    // Destructive red belongs to Reset, and only when you are about to press it.
    void noDestructiveRedAtRest() {
        make();
        m_strip->setState(rollingState());
        QImage img = m_strip->grab().toImage();
        QVERIFY(!imageHasColour(img, img.rect(), m_theme.markerPtr));
    }

    // The widget's tooltip names the hovered cell; the graph has none (its
    // readout is its own tip).
    void tooltipFollowsTheHoveredCell() {
        make();
        m_strip->setState(rollingState());
        QMouseEvent overMore(QEvent::MouseMove, QPointF(m_strip->itemRect(QStringLiteral("more")).center()),
                             QPointF(m_strip->mapToGlobal(m_strip->itemRect(QStringLiteral("more")).center())),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        qApp->notify(m_strip, &overMore);
        QCOMPARE(m_strip->hoverId(), QStringLiteral("more"));
        QCOMPARE(m_strip->toolTip(), QStringLiteral("Timeline options"));
        const QPoint gp = m_strip->graphRect().center();
        QMouseEvent overGraph(QEvent::MouseMove, QPointF(gp), QPointF(m_strip->mapToGlobal(gp)),
                              Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        qApp->notify(m_strip, &overGraph);
        QCOMPARE(m_strip->hoverId(), QStringLiteral("graph"));
        QVERIFY(m_strip->toolTip().isEmpty());
    }

    // The selected fields' changes are ticks in the selection colour, rising
    // from the baseline; with nothing selected there are none.
    void selectedFieldChangesAreTicks() {
        make();
        TimelineStripState s = rollingState();
        const int64_t at = s.nowMs - 120'000;
        TimelineStrip::Callbacks cb;
        cb.selection = [at](int64_t t0, int64_t step, int n, QVector<char>& out) {
            out.fill(0, n);
            const int64_t col = (at - t0) / step;
            if (at >= t0 && col < n) out[int(col)] = 1;
        };
        m_strip->setCallbacks(cb);
        m_strip->setState(s);
        QImage img = m_strip->grab().toImage();
        QVERIFY2(!imageHasColour(img, img.rect(), m_theme.indHoverSpan), "ticks with nothing selected");

        s.selectionKey = 42;
        m_strip->setState(s);
        img = m_strip->grab().toImage();
        const qreal dpr = img.devicePixelRatio();
        const QRect g = m_strip->graphRect();
        const double x = timeline::xForTime(m_strip->window(), g.left(), g.width(), at);
        const int dx = int(std::floor(x * dpr + 0.5));
        const int laneTop = int(std::floor((g.top() + g.height() - TimelineStrip::kSelectionTickPx) * dpr));
        QVERIFY2(imageHasColour(img, QRect(dx - 1, laneTop, 3, int(TimelineStrip::kSelectionTickPx * dpr)),
                                m_theme.indHoverSpan),
                 "no selection tick where the field changed");
    }

    // Selection bars only where something was recorded: not after now, not
    // in a not-recorded stretch.
    void selectionBarsOnlyWhereRecorded() {
        make();
        TimelineStripState s = rollingState();
        s.selectionKey = 9;
        TimelineStrip::Callbacks cb;
        cb.selection = [](int64_t, int64_t, int n, QVector<char>& out) { out.fill(1, n); };
        m_strip->setCallbacks(cb);
        m_strip->setState(s);
        const QImage img = m_strip->grab().toImage();
        const qreal dpr = img.devicePixelRatio();
        const QRect g = m_strip->graphRect();
        const timeline::Window w = m_strip->window();
        const int nowX = int(std::ceil(timeline::xForTime(w, g.left(), g.width(), s.nowMs) * dpr)) + 3;
        QVERIFY(imageHasColour(img, QRect(int(g.left() * dpr) + 20, 0, 40, img.height()), m_theme.indHoverSpan));
        QVERIFY2(!imageHasColour(img, QRect(nowX, 0, int(g.right() * dpr) - nowX, img.height()), m_theme.indHoverSpan),
                 "selection bars after now");
    }

    // The hover dot sits on the record under the pointer — not on the next
    // record's height — and nowhere past the envelope's end.
    void theHoverDotSitsOnTheRecordUnderIt() {
        make();
        TimelineStripState s = rollingState();
        s.retainedBeginMs = s.nowMs - 10'000;
        TimelineStrip::Callbacks cb;
        cb.columns = [](int64_t t0, int64_t step, int n, QVector<TimelineColumn>& out) {
            out.resize(n);
            for (int i = 0; i < n; ++i) {             // a record every 200 ms: 12 fields, then 3
                const int64_t a = t0 + step * i;
                const int64_t first = ((a + 199) / 200) * 200;
                if (first < a + step) { out[i].maxChanged = (first / 200) % 2 == 0 ? 12 : 3; out[i].records = 1; }
            }
        };
        m_strip->setCallbacks(cb);
        m_strip->setState(s);
        (void)m_strip->grab();
        const timeline::Window w = m_strip->window();
        auto colOf = [&](int64_t t) { return int((t - w.t0Ms) / w.stepMs); };
        const int high = colOf(s.nowMs - 4'800);          // 595 200 ms: an even tick, 12 fields
        const int low = colOf(s.nowMs - 4'600);           // 595 400 ms: the next tick, 3 fields
        QVERIFY(m_strip->envelopeTopForTest(high) >= 0);
        QVERIFY2(m_strip->envelopeTopForTest(high) < m_strip->envelopeTopForTest(low),
                 "the peak's own column reads the next record's height");
        QCOMPARE(m_strip->envelopeTopForTest(colOf(s.nowMs) + 3), -1);
    }

    // Recording, the graph hangs off now: the end of the recording IS the
    // right edge, and releasing there is back to live.
    void releasingAtTheEndOfTheRecordingReturnsToLive() {
        make();
        TimelineStripState s = rollingState();
        s.capture = timeline::Capture::Recording;
        s.retainedBeginMs = s.nowMs - 120'000;
        m_strip->setState(s);
        const QRect g = m_strip->graphRect();
        const int y = g.center().y();
        const int endX = g.right();
        QVERIFY2(std::abs(m_strip->viewForTest().xOf(s.nowMs) - double(g.right() + 1)) <= 2.0,
                 "now is not at the right edge while recording");
        send(QEvent::MouseButtonPress, QPoint(g.left() + 100, y), Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, QPoint(endX, y), Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, QPoint(endX, y), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(m_live, 1);
        for (const auto& sc : m_scrubs) QVERIFY(!sc.second);

        // Released well before the end: parked on that moment.
        m_live = 0;
        m_scrubs.clear();
        send(QEvent::MouseButtonPress, QPoint(g.left() + 100, y), Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, QPoint(g.left() + 200, y), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(m_live, 0);
        QVERIFY(!m_scrubs.isEmpty() && m_scrubs.last().second);
    }

    // Shift+wheel on the whole recording goes nowhere — it used to jump the
    // graph the wrong way and drop the whole-recording frame.
    void panningTheWholeRecordingDoesNothing() {
        make();
        m_strip->setState(rollingState());
        const timeline::Window before = m_strip->window();
        const QPoint at = m_strip->graphRect().center();
        QWheelEvent e(QPointF(at), QPointF(m_strip->mapToGlobal(at)), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::ShiftModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(m_strip, &e);
        QVERIFY(m_strip->window() == before);
    }

    // Pressing Stop changes nothing on the graph: the frame keeps the same
    // room after the recording in every state.
    void stoppingDoesNotRescaleTheGraph() {
        make();
        TimelineStripState s = rollingState();
        s.retainedBeginMs = s.nowMs - 120'000;
        s.capture = timeline::Capture::Recording;
        m_strip->setState(s);
        const timeline::Window recording = m_strip->window();
        s.capture = timeline::Capture::Rolling;
        m_strip->setState(s);
        QVERIFY(m_strip->window() == recording);
        s.capture = timeline::Capture::Paused;
        m_strip->setState(s);
        QVERIFY(m_strip->window() == recording);
    }

    // The wheel does nothing mid-scrub: the window stays frozen under the
    // pointer until the release.
    void aWheelMidScrubIsIgnored() {
        make();
        m_strip->setState(rollingState());
        const QRect g = m_strip->graphRect();
        const QPoint at(g.left() + 200, g.center().y());
        send(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        const timeline::Window atPress = m_strip->window();
        QWheelEvent e(QPointF(at), QPointF(m_strip->mapToGlobal(at)), QPoint(), QPoint(0, 120),
                      Qt::LeftButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(m_strip, &e);
        QVERIFY(m_strip->window() == atPress);
        send(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    }

    // "Show in Timeline" frames the range and stops following now.
    void showRangeFramesItAndStopsFollowing() {
        make();
        const TimelineStripState s = rollingState();
        m_strip->setState(s);
        const int64_t a = s.nowMs - 200'000, b = s.nowMs - 150'000;
        m_strip->showRange(a, b);
        const timeline::Window w = m_strip->window();
        QVERIFY(!w.follow);
        QVERIFY(w.t0Ms <= a);
        QVERIFY(w.t1Ms() >= b);
        QVERIFY(w.spanMs() < 300'000);
        m_strip->showRange(s.nowMs - 10'000'000, s.nowMs + 10'000'000);
        QVERIFY(m_strip->window().t1Ms() >= s.nowMs);
        QVERIFY(m_strip->window().t0Ms <= s.retainedBeginMs);
    }

    // Time passes between pushes and the writing edge moves with it, on the
    // strip's own clock — and that motion costs no redraw of the picture.
    void theEdgeMovesBetweenPushesWithoutRedrawing() {
        make();
        TimelineStripState s = rollingState();
        s.capture = timeline::Capture::Recording;
        s.recordBeginMs = s.nowMs - 120'000;
        s.retainedBeginMs = s.nowMs - 120'000;
        m_strip->setState(s);
        (void)m_strip->grab();
        const int renders = m_strip->renderCountForTest();
        const int64_t shown = m_strip->shownNowForTest();
        QCOMPARE(shown, s.nowMs);
        QVERIFY(m_strip->frameTimerActiveForTest());

        m_clockMs += 400;
        (void)m_strip->grab();
        QCOMPARE(m_strip->shownNowForTest(), shown + 400);            // time moved
        QCOMPARE(m_strip->renderCountForTest(), renders);             // the picture did not

        // It never runs away from its own data when a push stalls.
        m_clockMs += 10 * timeline::kMaxClockDriftMs;
        QCOMPARE(m_strip->shownNowForTest(), s.nowMs + timeline::kMaxClockDriftMs);
    }

    // The recorded band is rendered to the picture's edge and CLIPPED at the
    // moment now has reached — that is what lets the writing edge advance
    // without drawing the picture again. So nothing may show right of now.
    void nothingIsDrawnPastNow() {
        make();
        TimelineStripState s = rollingState();
        s.retainedBeginMs = s.nowMs - 120'000;
        m_strip->setState(s);
        // The shipping frame: the whole recording from the left, with room
        // kept after it to grow into — that room is what must stay empty.
        const QImage img = m_strip->grab().toImage();
        const qreal dpr = img.devicePixelRatio();
        const QRect g = m_strip->graphRect();
        const double nowX = timeline::xForTime(m_strip->window(), g.left(), g.width(), s.nowMs);
        QVERIFY2(nowX > g.left() && nowX < g.right() - 10,
                 qPrintable(QStringLiteral("now at %1 leaves no room before %2").arg(nowX).arg(g.right())));
        const QRgb paper = editorPaperColor(m_theme).rgb();
        const int x0 = int(std::ceil((nowX + 2) * dpr));
        // Stop before the ⋯ button's scrim: it blends paper over the graph, so
        // its pixels are a channel off paper by design.
        const int x1 = std::min({int(g.right() * dpr), img.width() - 1,
                                 int((m_strip->itemRect(QStringLiteral("more")).left() - 10) * dpr)});
        for (int x = x0; x <= x1; ++x)
            for (int y = int(timeline::kPinTop * dpr); y < int(timeline::kSelBottom * dpr); ++y)
                QVERIFY2(img.pixelColor(x, y).rgb() == paper,
                         qPrintable(QStringLiteral("scale %1: ink at (%2,%3), right of now")
                                        .arg(dpr).arg(x).arg(y)));
    }

    // Recording: the graph hangs off now at the right edge and history flows
    // left out of it. A record's x moves with the clock by FRACTIONS of a
    // pixel — that is the difference between flowing and stepping — and the
    // picture is not drawn again to do it.
    void theGraphFlowsOffNow() {
        make();
        TimelineStripState s = rollingState();
        s.capture = timeline::Capture::Recording;
        // Older than the window, so the span is locked and the graph SCROLLS:
        // a record's x moves by exactly the time that passed. (A younger
        // recording stretches instead — aYoungRecordingFillsTheStripThenScrolls.)
        s.retainedBeginMs = s.nowMs - 600'000;
        const int64_t now = s.nowMs;
        TimelineStrip::Callbacks cb;
        cb.points = [now](int64_t t0, int64_t t1, int cap, QVector<TimelinePoint>& out) {
            out.clear();
            for (int64_t t = ((t0 + 199) / 200) * 200; t < t1 && out.size() < cap; t += 200)
                if (t <= now) out.append(TimelinePoint{t, 6, (t / 200) % 5 == 0 ? 2u : 0u, false});
            return true;
        };
        m_strip->setCallbacks(cb);
        m_strip->setState(s);
        (void)m_strip->grab();

        const QRect g = m_strip->graphRect();
        const timeline::View v0 = m_strip->viewForTest();
        QVERIFY2(std::abs(v0.xOf(s.nowMs) - double(g.right() + 1)) <= 2.0,
                 qPrintable(QStringLiteral("now at %1, right edge %2").arg(v0.xOf(s.nowMs)).arg(g.right() + 1)));
        QVERIFY(v0.xOf(s.retainedBeginMs) < v0.xOf(s.nowMs));

        const int renders = m_strip->renderCountForTest();
        const double before = v0.xOf(s.nowMs - 10'000);
        m_clockMs += 33;                       // a fraction of one capture tick
        (void)m_strip->grab();
        const double after = m_strip->viewForTest().xOf(s.nowMs - 10'000);
        const double moved = before - after;
        const double expect = 33.0 * v0.pxPerMs;
        QVERIFY2(moved > 0.0 && std::abs(moved - expect) < 0.25,
                 qPrintable(QStringLiteral("moved %1 px, expected %2").arg(moved).arg(expect)));
        QCOMPARE(m_strip->renderCountForTest(), renders);
    }

    // A young recording FILLS the strip — it does not huddle at the right
    // edge — and once it outgrows the window the span holds and the history
    // scrolls out of the left instead.
    void aYoungRecordingFillsTheStripThenScrolls() {
        make();
        TimelineStripState s = rollingState();
        s.capture = timeline::Capture::Recording;
        s.retainedBeginMs = s.nowMs - 8'000;         // eight seconds in
        m_strip->setState(s);
        (void)m_strip->grab();
        const QRect g = m_strip->graphRect();
        {
            const timeline::View v = m_strip->viewForTest();
            const double begin = v.xOf(s.retainedBeginMs), end = v.xOf(s.nowMs);
            QVERIFY2(end - begin > 0.80 * g.width(),
                     qPrintable(QStringLiteral("8s of recording covers only %1 of %2 px")
                                    .arg(end - begin).arg(g.width())));
            QVERIFY(std::abs(end - double(g.right() + 1)) <= 2.0);   // now at the right edge
        }
        // Long past the window: the span holds, so the oldest part is off screen.
        s.retainedBeginMs = s.nowMs - 600'000;
        m_strip->setState(s);
        (void)m_strip->grab();
        {
            const timeline::View v = m_strip->viewForTest();
            QVERIFY2(v.xOf(s.retainedBeginMs) < g.left(), "the whole recording still fits: it never scrolls");
            QVERIFY(std::abs(v.xOf(s.nowMs) - double(g.right() + 1)) <= 2.0);
            // The window it settles at is the default, not something larger.
            const double heldMs = double(g.width()) / v.pxPerMs;
            QVERIFY2(std::abs(heldMs - double(timeline::kDefaultFollowMs)) < 1000.0,
                     qPrintable(QStringLiteral("holds %1 ms").arg(heldMs)));
        }
    }

    // The trace reaches both ends of the bar: a young recording fills it,
    // edge to edge, with no paper margin left or right.
    void theTraceSpansTheWholeBar() {
        make(1080, /*withPoints=*/true);
        TimelineStripState s = rollingState();
        s.capture = timeline::Capture::Recording;
        s.retainedBeginMs = s.nowMs - 8'000;      // eight seconds in
        m_strip->setState(s);
        const QImage img = m_strip->grab().toImage();
        const qreal dpr = img.devicePixelRatio();
        const QRect g = m_strip->graphRect();
        QCOMPARE(g.left(), 0);
        const QRgb paper = editorPaperColor(m_theme).rgb() & 0xFFFFFF;
        auto inked = [&](int devX) {
            for (int y = int(timeline::kBandTop * dpr); y < int(timeline::kBandBottom * dpr); ++y)
                if ((img.pixelColor(devX, y).rgb() & 0xFFFFFF) != paper) return true;
            return false;
        };
        QVERIFY2(inked(0), "the bar is empty at its left edge");
        const int rightMost = int((m_strip->itemRect(QStringLiteral("more")).left() - 10) * dpr);
        QVERIFY2(inked(rightMost), "the bar is empty before the overflow button");
        // And the mapping says the same thing.
        const timeline::View v = m_strip->viewForTest();
        QVERIFY(std::abs(v.xOf(s.retainedBeginMs) - double(g.left())) <= 2.0);
        QVERIFY(std::abs(v.xOf(s.nowMs) - double(g.right() + 1)) <= 2.0);
    }

    // A trace that flows repaints every frame, not once per push. At the
    // settled span one 16 ms tick moves the picture ~0.27 px, so a
    // whole-pixel repaint rule left it advancing ~8 times a second in 2 px
    // jumps — the chunkiness that survived the first pass.
    void theTraceRepaintsEveryFrameWhileFollowing() {
        make(1080, /*withPoints=*/true);
        TimelineStripState s = rollingState();
        s.capture = timeline::Capture::Recording;
        s.retainedBeginMs = s.nowMs - 600'000;   // long enough that the span is locked
        m_strip->setState(s);
        (void)m_strip->grab();
        const int paints = m_strip->paintCountForTest();
        const int renders = m_strip->renderCountForTest();
        for (int i = 0; i < 12; ++i) {
            m_clockMs += 16;
            (void)m_strip->grab();
        }
        QVERIFY2(m_strip->paintCountForTest() - paints >= 12,
                 qPrintable(QStringLiteral("only %1 paints for 12 frames")
                                .arg(m_strip->paintCountForTest() - paints)));
        QCOMPARE(m_strip->renderCountForTest(), renders);   // and none of them redrew the picture
    }

    // The vertical scale is blended, not snapped: domainFor doubles in one
    // step, which would drop the whole trace ~30 % in a single frame.
    void theVerticalScaleIsBlendedNotSnapped() {
        make(1080);
        TimelineStripState s = rollingState();
        s.capture = timeline::Capture::Recording;
        s.retainedBeginMs = s.nowMs - 600'000;
        const int64_t now = s.nowMs;
        // ONE feed for the whole test: every record changes 5 fields, except
        // that flipping m_burst adds a single tall one. Only the peak moves,
        // so any change at a steady column is the vertical scale itself.
        TimelineStrip::Callbacks cb;
        cb.points = [this, now](int64_t t0, int64_t t1, int cap, QVector<TimelinePoint>& out) {
            out.clear();
            for (int64_t t = ((t0 + 199) / 200) * 200; t < t1 && t <= now; t += 200) {
                if (out.size() >= cap) return false;
                const bool tall = m_burst && t == now - 1'000;
                out.append(TimelinePoint{t, tall ? 60u : 5u, 0u, false});
            }
            return true;
        };
        m_strip->setCallbacks(cb);
        m_burst = false;
        m_strip->setState(s);
        (void)m_strip->grab();
        const timeline::View v = m_strip->viewForTest();
        const QRect g = m_strip->graphRect();
        const qreal dpr = m_strip->devicePixelRatioF();
        const int probe = int((v.xOf(now - 60'000) - g.left()) * dpr);
        const int before = m_strip->envelopeTopForTest(probe);
        QVERIFY(before >= 0);

        m_burst = true;                       // same feed, one tall record
        s.dataGeneration++;
        m_strip->setState(s);
        (void)m_strip->grab();
        const int atStart = m_strip->envelopeTopForTest(probe);
        m_clockMs += timeline::kScaleAnimMs / 2;
        (void)m_strip->grab();
        const int mid = m_strip->envelopeTopForTest(probe);
        m_clockMs += timeline::kScaleAnimMs;
        (void)m_strip->grab();
        const int after = m_strip->envelopeTopForTest(probe);

        const QString trail = QStringLiteral("%1 -> %2 -> %3 -> %4")
                                  .arg(before).arg(atStart).arg(mid).arg(after);
        // A bigger peak means the steady trace sits lower (a larger top y).
        QVERIFY2(after > before, qPrintable(QStringLiteral("the trace never rescaled: ") + trail));
        QVERIFY2(atStart == before, qPrintable(QStringLiteral("the scale jumped on arrival: ") + trail));
        QVERIFY2(mid > before && mid < after,
                 qPrintable(QStringLiteral("the scale snapped instead of blending: ") + trail));
    }

    // A quiet window is a complete answer, not "too dense": the strip must
    // not flip to the binned picture, whose stroke and beads differ.
    void anEmptyWindowKeepsThePointRenderer() {
        make(1080, /*withPoints=*/true);
        TimelineStrip::Callbacks cb;
        cb.points = [](int64_t, int64_t, int, QVector<TimelinePoint>& out) {
            out.clear();
            return true;                      // nothing here, completely
        };
        m_strip->setCallbacks(cb);
        TimelineStripState s = rollingState();
        s.capture = timeline::Capture::Recording;
        m_strip->setState(s);
        (void)m_strip->grab();
        QCOMPARE(m_strip->renderCountForTest(), 0);
    }

    // Nothing moving, nothing ticking.
    void theFrameTimerSleepsWhenNothingMoves() {
        make();
        TimelineStripState rec = rollingState();
        rec.capture = timeline::Capture::Recording;
        m_strip->setState(rec);
        QVERIFY(m_strip->frameTimerActiveForTest());
        m_strip->setState(rollingState());                 // stopped: nothing is being written
        QVERIFY(!m_strip->frameTimerActiveForTest());
        m_strip->setState(rec);
        QVERIFY(m_strip->frameTimerActiveForTest());
        m_strip->hide();
        QVERIFY(!m_strip->frameTimerActiveForTest());
        m_strip->show();
        QVERIFY(m_strip->frameTimerActiveForTest());
    }

    // A drag stops exactly where it is released — standing between two
    // changes is a place to stand — and Ctrl is what pulls it onto one.
    void aScrubStopsWhereItIsReleasedAndCtrlSnaps() {
        make();
        m_strip->setState(rollingState());
        const QRect g = m_strip->graphRect();
        const QPoint at(g.left() + 137, g.center().y());
        const timeline::Window w = m_strip->window();
        const int64_t want = timeline::timeForX(w, g.left(), g.width(), at.x());
        m_tickMs = want + 2 * w.stepMs;      // a change two columns away

        send(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
        QVERIFY(!m_scrubs.isEmpty() && m_scrubs.last().second);
        QCOMPARE(m_scrubs.last().first, want);

        m_scrubs.clear();
        send(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
        send(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
        QVERIFY(!m_scrubs.isEmpty());
        QCOMPARE(m_scrubs.last().first, m_tickMs);
    }

    // A rescale the user did not ask for is animated, not snapped. (While
    // recording the graph flows off now at a fixed span, so nothing rescales;
    // this is the resting view, which frames the whole recording — where the
    // scale used to jump a rung as the recording grew.)
    void aRescaleAnimatesInsteadOfSnapping() {
        make();
        TimelineStripState s = rollingState();
        s.retainedBeginMs = s.nowMs - 30'000;
        m_strip->setState(s);
        (void)m_strip->grab();
        const QRect g = m_strip->graphRect();
        const timeline::View before = m_strip->viewForTest();

        s.retainedBeginMs = s.nowMs - 900'000;   // far more to frame: the scale steps
        m_strip->setState(s);
        (void)m_strip->grab();
        QVERIFY2(m_strip->isAnimatingForTest(), "a rescale snapped");
        const timeline::View target = timeline::viewFor(m_strip->window(), g.left(), g.width());

        m_clockMs += timeline::kScaleAnimMs / 2;
        (void)m_strip->grab();
        const timeline::View mid = m_strip->viewForTest();
        QVERIFY2(timeline::viewsDiffer(before, mid, g.left(), g.width(), 0.5), "it never left");
        QVERIFY2(timeline::viewsDiffer(mid, target, g.left(), g.width(), 0.5), "it arrived at once");

        m_clockMs += timeline::kScaleAnimMs;
        (void)m_strip->grab();
        QVERIFY(!m_strip->isAnimatingForTest());
        QVERIFY(!timeline::viewsDiffer(m_strip->viewForTest(), target, g.left(), g.width(), 0.5));
    }

    // Equal states do not repaint or re-lay out.
    void anEqualStateIsIgnored() {
        make();
        const TimelineStripState s = rollingState();
        m_strip->setState(s);
        const QRect before = m_strip->graphRect();
        m_strip->setState(s);
        QCOMPARE(m_strip->graphRect(), before);
        QVERIFY(m_strip->state() == s);
    }
};

QTEST_MAIN(TestTimelineStrip)
#include "test_timeline_strip.moc"
