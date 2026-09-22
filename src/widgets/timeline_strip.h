#pragma once

// ── TimelineStrip: look back through a class's recording ──
//
// One strip per document tab, at the bottom of the tab: capture state is
// per tab, so a per-pane strip would silently rewind the split sibling, and
// at the bottom, showing it only trims the editor from below — the first
// visible line never moves.
//
//    ●       ●   ●●          ●       ●   ●●                  what started changing
//   ▁▅▇▇▇▇▇▆▅▄▄▅▇▇▇▇▇▇▆▅▄▄▅▇▇▇▇▇▇▇▆▅▄▄▅▇▇▇▇▇▆▅▄▄        ⋯   how much of the class changed
//   ━━━━━━━━━━━━━━━━━━━━━━━━━━┄┄┄┄┄┄━━━━━━━━━━━━━━━━          recorded · not recorded
//    ▬       ▬   ▬▬                                          the selected fields
//
// The strip is the graph and one overflow button (⋯): Record / Stop, and
// Back to live while looking back, sit beside the address. The graph shows the WHOLE recording,
// starting at the left edge (it grows into room kept on the right while it
// records) until the user zooms in. No time is written on it; hovering says
// when.
//
// Four lanes, top to bottom:
//   · beads — records where a field STARTED changing (one the record before
//     did not change): a bounce stands out from steady motion;
//   · the envelope — how many fields of the class one record changed, as a
//     filled shape joining record to record (records are sparser than device
//     columns, so drawing columns alone left a comb of 1-px ticks), over a
//     faint band that marks what was recorded;
//   · the baseline — solid where recorded, dotted where not;
//   · the selection lane — when the selected fields changed.
//
// Colour budget: the envelope is data, in the document's number ink
// (syntaxNumber — the colour the values themselves are drawn in), a crisp
// edge over a fill that fades toward the baseline; structure is neutral
// (textDim mixes); indHoverSpan only for the selected fields; focusGlow — the
// app's "not live" amber — only for the playhead and its handle while looking
// back, when what was recorded after the moment shown is faded.
//
// Header-only, no Q_OBJECT, a Callbacks struct and string-id cells: the
// AddressBar template. All geometry and time math is in timeline_strip_model.h
// and tested without a widget.

#include "paintutil.h"
#include "rcxtooltip.h"
#include "svgicon.h"
#include "themes/theme.h"
#include "themes/thememanager.h"
#include "widgets/chrome_fallback_theme.h"
#include "widgets/dock_header.h"
#include "widgets/timeline_strip_model.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPair>
#include <QPointer>
#include <QTimer>
#include <QTimeZone>
#include <QWheelEvent>
#include <QWidget>
#include <functional>

namespace rcx {

struct TimelineColumn {
    uint32_t maxChanged = 0;   // most fields (or bytes) changed by one record in this column
    uint64_t sumChanged = 0;
    int      records = 0;
};

// One record, drawn where it happened. Records are sparser than pixels in
// every normal view, so the graph is drawn from these directly: a record's x
// is a real number of pixels from "now", which is what lets the picture flow
// instead of stepping one column at a time.
struct TimelinePoint {
    int64_t  tMs = 0;
    uint32_t changed = 0;    // fields (or bytes) it changed
    uint32_t onset = 0;      // fields it started changing
    bool     selected = false;
};

// Kinds of markers on the graph. Mirrors tl::TimelineEventKind so the
// controller can pass them straight through.
enum class TimelineMarkerKind : int { Rebase = 0, SourceAttached, SourceDetached,
                                      RecordStart, RecordStop, Reset };

struct TimelineStripState {
    timeline::Capture capture = timeline::Capture::None;
    bool    past = false;
    bool    readsFailing = false;
    bool    hasData = false;
    int64_t nowMs = 0;              // capture clock
    int64_t epochAtZeroMs = 0;      // capture clock → wall clock
    int     utcOffsetSeconds = 0;
    int64_t retainedBeginMs = 0;    // oldest moment this class can still show
    int64_t recordBeginMs = -1;     // Recording: when it started
    int64_t viewedMs = 0;           // Past: the moment on screen
    int64_t rollingWindowMs = 30LL * 60 * 1000;
    qint64  bytesUsed = 0;
    qint64  byteBudget = 0;
    quint64 dataGeneration = 0;     // bumps with every commit: re-pull columns
    quint64 selectionKey = 0;       // the selected fields (0: none) — re-pull their ticks
    QVector<timeline::Span> gaps;   // store gaps and this class's pauses
    struct Marker {
        int64_t tMs = 0;
        TimelineMarkerKind kind = TimelineMarkerKind::Rebase;
        QString label;
        bool operator==(const Marker& o) const { return tMs == o.tMs && kind == o.kind && label == o.label; }
    };
    QVector<Marker> markers;

    bool operator==(const TimelineStripState& o) const {
        if (gaps.size() != o.gaps.size()) return false;
        for (int i = 0; i < gaps.size(); ++i)
            if (gaps[i].startMs != o.gaps[i].startMs || gaps[i].endMs != o.gaps[i].endMs) return false;
        return capture == o.capture && past == o.past && readsFailing == o.readsFailing
            && hasData == o.hasData && nowMs == o.nowMs && epochAtZeroMs == o.epochAtZeroMs
            && utcOffsetSeconds == o.utcOffsetSeconds && retainedBeginMs == o.retainedBeginMs
            && recordBeginMs == o.recordBeginMs && viewedMs == o.viewedMs
            && rollingWindowMs == o.rollingWindowMs && bytesUsed == o.bytesUsed
            && byteBudget == o.byteBudget && dataGeneration == o.dataGeneration
            && selectionKey == o.selectionKey && markers == o.markers;
    }
    bool operator!=(const TimelineStripState& o) const { return !(*this == o); }

    bool canCapture() const {
        return capture == timeline::Capture::Rolling || capture == timeline::Capture::Recording
            || capture == timeline::Capture::Paused;
    }
};

class TimelineStrip : public QWidget {
public:
    static constexpr int kHeight = timeline::kStripHeight;
    static constexpr double kDisabledOpacity = 0.40;
    static constexpr int kSelectionTickPx = timeline::kSelBottom - timeline::kSelTop;

    struct Callbacks {
        std::function<void()>               onClear;         // "Clear Recording…"
        std::function<void()>               onReturnToLive;  // a scrub released at now, or cancelled from live
        std::function<void()>               onScrubBegin;    // a drag on the graph starts
        std::function<void()>               onScrubCancel;   // Esc / right-press: put back what was shown
        std::function<void()>               onHide;
        std::function<void(int64_t, bool)>  onScrub;         // (capture ms, final) — latest wins
        std::function<void(int)>            onStep;          // -1 previous change, +1 next
        // Pulled on paint (cached by data generation and window), never per tick.
        std::function<void(int64_t t0, int64_t stepMs, int n, QVector<TimelineColumn>&)> columns;
        std::function<QVector<int64_t>(int64_t t0, int64_t t1)> changeTicks;   // snap candidates
        std::function<QString(int64_t t)>   describe;        // hover readout body
        // Per graph column (the same t0 / step / n as `columns`): did the
        // SELECTED fields change in it? The selection lane.
        std::function<void(int64_t t0, int64_t stepMs, int n, QVector<char>&)> selection;
        // Per graph column: the most fields one record in it STARTED changing
        // (0: none). The beads.
        std::function<void(int64_t t0, int64_t stepMs, int n, QVector<uint32_t>&)> events;
        // Every record in [t0, t1). Returns whether the answer is COMPLETE —
        // false only when there are more than `cap` of them, the one signal to
        // fall back to the binned columns. An empty window is complete. The
        // graph prefers these: they carry their own time, so the picture moves
        // with the clock instead of with the bin grid.
        std::function<bool(int64_t t0, int64_t t1, int cap, QVector<TimelinePoint>&)> points;
    };

    explicit TimelineStrip(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("rcxTimelineStrip"));
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(kHeight);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setFont(chromeFont());
        m_theme = ThemeManager::instance().current();
        if (!m_theme.background.isValid() || !m_theme.text.isValid())
            m_theme = chromeFallbackTheme();
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
                [this](const Theme& t) { applyTheme(t); });
        m_scrubTimer.setSingleShot(true);
        m_scrubTimer.setInterval(0);
        connect(&m_scrubTimer, &QTimer::timeout, this, [this] { flushScrub(false); });
        m_dwell.setSingleShot(true);
        m_dwell.setInterval(300);
        connect(&m_dwell, &QTimer::timeout, this, [this] { showReadout(); });
        // The strip's own clock. Records land every capture tick, but "now"
        // moves continuously, and the writing edge should move with it rather
        // than jump when the next state arrives.
        m_clock = [] {
            static QElapsedTimer t = [] { QElapsedTimer e; e.start(); return e; }();
            return int64_t(t.elapsed());
        };
        m_stateClockMs = m_clock();
        // Windows' coarse timers quantise to ~15 ms, which is the judder this
        // is here to remove.
        m_frame.setTimerType(Qt::PreciseTimer);
        m_frame.setInterval(16);
        connect(&m_frame, &QTimer::timeout, this, [this] { onFrame(); });
    }

    ~TimelineStrip() override {
        if (m_filterInstalled && qApp) qApp->removeEventFilter(this);
        delete m_tip;
    }

    void setCallbacks(Callbacks cb) {
        m_cb = std::move(cb);
        m_graphKey = {};
        m_pointsOk = false;
        m_points1 = -1;     // nothing pulled yet
        update();
    }

    void setState(const TimelineStripState& s) {
        if (s == m_state) return;
        if (s.nowMs != m_state.nowMs) m_stateClockMs = m_clock();
        // Record pressed: hang the graph off now so what is being written
        // flows in at the right edge. Stop changes nothing — the window stays
        // where it is and simply stops moving, because now stops moving.
        const bool started = s.capture == timeline::Capture::Recording
                          && m_state.capture != timeline::Capture::Recording;
        if (started && m_spanMs <= 0 && !m_userFitted) {
            m_spanMs = timeline::kDefaultFollowMs;
            m_follow = true;
            m_frozen = false;
            m_graphKey = {};
        }
        // Back to live: the graph follows again, at whatever zoom it had —
        // there is no separate "follow" switch to remember to turn back on.
        const bool returnedLive = m_state.past && !s.past && !m_dragging;
        m_state = s;
        if (returnedLive) {
            m_follow = true;
            m_frozen = false;
            m_graphKey = {};
        }
        update();
        refreshCellToolTip();
        updateFrameTimer();
    }
    const TimelineStripState& state() const { return m_state; }

    void applyTheme(const Theme& t) {
        m_theme = t.background.isValid() ? t : chromeFallbackTheme();
        setFont(chromeFont());
        m_layoutWidth = -1;
        m_graphKey = {};
        update();
    }

    // ── Test / harness hooks ──
    QRect itemRect(const QString& id) {
        ensureLayout();
        return id == QStringLiteral("graph") ? m_layout.graph : m_layout.rect(id);
    }
    QRect graphRect() { ensureLayout(); return m_layout.graph; }
    const timeline::Layout& layout() { ensureLayout(); return m_layout; }
    timeline::Window window() { ensureLayout(); return currentWindow(); }
    QString hoverId() const { return m_hoverId; }
    bool isDragging() const { return m_dragging; }
    // The logical x of the playhead, or -1 when live.
    double playheadX() {
        ensureLayout();
        if (!m_state.past) return -1;
        // The mapping the paint uses, animation included: the handle must sit
        // where the moment is drawn, not where the bin window would put it.
        beginFrame();
        return paintView().xOf(m_dragging ? m_dragMs : m_state.viewedMs);
    }
    int menuOpenCount() const { return m_menuOpens; }
    // The overflow menu's rows as they would read now (separators skipped,
    // a submenu by its title).
    QStringList menuTextsForTest() {
        QMenu menu;
        buildMenu(menu);
        QStringList out;
        for (QAction* a : menu.actions())
            if (!a->isSeparator()) out << a->text();
        return out;
    }
    // The words the graph shows when there is nothing to draw ("" otherwise).
    QString emptyMessage() const {
        if (m_state.hasData) return QString();
        if (!m_state.canCapture())
            return QStringLiteral("Static source — nothing changes to record. "
                                  "Attach a process to record live data.");
        if (m_state.capture == timeline::Capture::Recording)
            return QStringLiteral("Recording — changes show up here as they happen");
        return QStringLiteral("Press Record to capture changes over time");
    }
    void setFollowForTest(bool follow) { m_follow = follow; m_frozen = false; update(); }
    // The strip's clock, so a test can drive frames instead of waiting on one.
    void setClockForTest(std::function<int64_t()> fn) {
        m_clock = std::move(fn);
        m_stateClockMs = m_clock();
        m_shownNowMs = 0;
        update();
    }
    // How many times the graph's picture has been drawn again: motion between
    // pushes must not cost a redraw.
    int renderCountForTest() const { return m_renderCount; }
    // How many times the widget has painted: a trace that flows must repaint
    // every frame, not once per push.
    int paintCountForTest() const { return m_paintCount; }
    // The mapping the last paint used (animation included).
    timeline::View viewForTest() { ensureLayout(); beginFrame(); return paintView(); }
    bool isAnimatingForTest() const { return m_animStartMs >= 0; }
    bool frameTimerActiveForTest() const { return m_frame.isActive(); }
    int64_t shownNowForTest() { beginFrame(); return m_shownNowMs; }
    // Envelope ink, for pixel tests.
    QColor envelopeLineColour() const { return inks().line; }
    // Where the envelope's top edge is in a graph device column (-1: none),
    // as of the last paint — the hover dot sits there.
    int envelopeTopForTest(int deviceColumn) const {
        return (deviceColumn >= 0 && deviceColumn < m_envTopDev.size()) ? m_envTopDev[deviceColumn] : -1;
    }

    QSize sizeHint() const override { return QSize(800, kHeight); }
    QSize minimumSizeHint() const override { return QSize(120, kHeight); }

protected:
    // ── Paint ──
    void paintEvent(QPaintEvent*) override {
        ++m_paintCount;
        ensureLayout();
        const Theme& t = m_theme;
        const QColor paper = editorPaperColor(t);
        const QColor seam = containerBorderColor(t);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(rect(), paper);

        paintGraph(p);
        for (const timeline::LaidCell& c : m_layout.cells) paintCell(p, c);

        // PaneBox paints the line above; the strip closes its own box.
        const QRectF r(rect());
        fillLeftDeviceColOfRect(p, r, seam);
        fillRightDeviceColOfRect(p, r, seam);
        fillBottomDeviceRowOfRect(p, r, seam);
    }

    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        m_layoutWidth = -1;
        m_graphKey = {};
    }

    void showEvent(QShowEvent* e) override { QWidget::showEvent(e); updateFrameTimer(); }
    void hideEvent(QHideEvent* e) override { QWidget::hideEvent(e); updateFrameTimer(); }

    // ── Pointer ──
    void mouseMoveEvent(QMouseEvent* e) override {
        ensureLayout();
        const QPoint pos = e->position().toPoint();
        if (m_dragging) {
            updateDrag(pos, e->modifiers());
            return;
        }
        const QString id = timeline::cellIdAt(m_layout, pos);
        if (id != m_hoverId) {
            m_hoverId = id;
            refreshCellToolTip();
            if (id != QStringLiteral("graph")) hideReadout();
            update();
        }
        if (id == QStringLiteral("graph") && m_state.hasData) {
            m_hoverX = pos.x();
            setCursor(Qt::PointingHandCursor);
            if (m_tip && m_tip->isVisible()) showReadout(); else m_dwell.start();
            update(QRect(m_layout.graph.left() - 4, 0, m_layout.graph.width() + 8, kHeight));
        } else {
            m_hoverX = -1;
            setCursor(isClickable(id) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        }
    }

    void leaveEvent(QEvent* e) override {
        QWidget::leaveEvent(e);
        if (m_dragging) return;
        m_hoverId.clear();
        m_hoverX = -1;
        m_pressId.clear();
        hideReadout();
        unsetCursor();
        update();
    }

    void mousePressEvent(QMouseEvent* e) override {
        ensureLayout();
        const QPoint pos = e->position().toPoint();
        if (m_dragging && e->button() == Qt::RightButton) {
            cancelDrag();
            return;
        }
        if (e->button() == Qt::MiddleButton && m_layout.graph.contains(QPoint(pos.x(), m_layout.graph.center().y()))) {
            m_panning = true;
            m_panAnchorX = pos.x();
            m_panWindow = currentWindow();
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        const QString id = timeline::cellIdAt(m_layout, pos);
        if (id == QStringLiteral("graph")) {
            if (!m_state.hasData) return;
            beginDrag(pos, e->modifiers());
            return;
        }
        m_pressId = isEnabledCell(id) ? id : QString();
        update();
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        const QPoint pos = e->position().toPoint();
        if (m_panning && e->button() == Qt::MiddleButton) { m_panning = false; return; }
        if (m_dragging && e->button() == Qt::LeftButton) {
            endDrag(pos);
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        const QString id = timeline::cellIdAt(m_layout, pos);
        const QString pressed = m_pressId;
        m_pressId.clear();
        update();
        if (!pressed.isEmpty() && pressed == id) activate(id, pos);
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override {
        ensureLayout();
        if (e->button() == Qt::LeftButton
            && timeline::cellIdAt(m_layout, e->position().toPoint()) == QStringLiteral("graph")) {
            fitAll();
            return;
        }
        QWidget::mouseDoubleClickEvent(e);
    }

    void wheelEvent(QWheelEvent* e) override {
        ensureLayout();
        // Mid-scrub the window stays frozen under the pointer.
        if (m_dragging) { e->accept(); return; }
        const QPoint pos = e->position().toPoint();
        if (!m_state.hasData || !m_layout.graph.contains(QPoint(pos.x(), m_layout.graph.center().y()))) {
            e->ignore();
            return;
        }
        hideReadout();
        const timeline::Window w = currentWindow();
        const int dy = e->angleDelta().y();
        const int dx = e->angleDelta().x();
        const bool pan = (e->modifiers() & Qt::ShiftModifier) || (dx != 0 && dy == 0);
        if (pan) {
            // The whole recording is already on screen: nothing to pan to.
            if (m_follow && m_spanMs <= 0) { m_wheelAccum = 0; e->accept(); return; }
            m_wheelAccum += (dx != 0 ? dx : dy);
            const int notches = m_wheelAccum / 120;
            if (notches == 0) { e->accept(); return; }
            m_wheelAccum -= notches * 120;
            const int64_t dt = -int64_t(notches) * w.spanMs() / 10;
            setWindow(timeline::panBy(w, dt, m_state.retainedBeginMs, m_state.nowMs));
        } else {
            m_wheelAccum += dy;
            const int notches = m_wheelAccum / 120;
            if (notches == 0) { e->accept(); return; }
            m_wheelAccum -= notches * 120;
            if (!m_state.past && m_follow) {
                // Live and following: a couple of ladder rungs per notch and
                // keep following — there is no Follow switch to find afterwards.
                // Zooming out past the whole recording shows the whole
                // recording again.
                const QVector<int64_t>& ladder = timeline::stepLadder();
                const int last = int(ladder.size()) - 1;
                int rung = std::min(last, int(std::lower_bound(ladder.cbegin(), ladder.cend(), w.stepMs)
                                              - ladder.cbegin()));
                rung = std::clamp(rung - 2 * notches, 0, last);
                const int64_t whole = std::max<int64_t>(m_state.nowMs - m_state.retainedBeginMs, timeline::kMinSpanMs);
                const int64_t span = ladder[rung] * w.columns;
                if (notches < 0 && span >= whole) fitAll();
                else followWithSpan(std::clamp<int64_t>(span, timeline::kMinSpanMs, whole));
            } else {
                // Parked: zoom about the pointer and stay put.
                const int64_t anchor = timeline::timeForX(w, m_layout.graph.left(), m_layout.graph.width(), pos.x());
                setWindow(timeline::zoomAround(w, anchor, std::pow(0.8, notches),
                                               m_state.retainedBeginMs, m_state.nowMs));
            }
        }
        e->accept();
    }

    void contextMenuEvent(QContextMenuEvent* e) override {
        if (m_dragging) return;
        showMenu(e->globalPos());
    }

    // Esc while scrubbing restores what was on screen before the press. The
    // strip never takes focus, so it listens application-wide for the drag.
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (m_dragging && e->type() == QEvent::KeyPress
            && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
            cancelDrag();
            return true;
        }
        return QWidget::eventFilter(obj, e);
    }

private:
    // ── Motion ──
    // Does "now" move by itself right now? Only a running recording writes
    // new data; every other mode pins nowMs to the end of what was recorded
    // (main.cpp timelineStripStateFor), and extrapolating there would drag
    // the graph away from its own data. A drag freezes it too.
    bool nowAdvances() const {
        return m_state.hasData && m_state.capture == timeline::Capture::Recording
            && !m_dragging && !m_frozen;
    }

    // Once per frame: where "now" is, by the strip's own clock.
    void beginFrame() {
        m_shownNowMs = timeline::shownNow(m_state.nowMs, m_clock() - m_stateClockMs,
                                          nowAdvances(), m_shownNowMs, timeline::kMaxClockDriftMs);
    }

    // The mapping with the slide taken out, for deciding what to animate. A
    // following view is pinned to now, so time passing moves it every frame —
    // comparing painted views would read that as a rescale and the graph
    // would swim. Referred to a fixed moment, only a real change of scale (a
    // rung, a width) or of framing (a trim) shows up.
    timeline::View cmpView() const {
        const timeline::Window w = currentWindow();
        const QRect g = m_layout.graph;
        if (!(w.follow && m_spanMs > 0)) return timeline::viewFor(w, g.left(), g.width());
        // Referred to a fixed moment, and with the growing span left out: a
        // following view is meant to move and to stretch as the recording
        // fills it — neither is a rescale to animate.
        timeline::View v = timeline::followView(w, 0, g.left(), g.width());
        v.pxPerMs = 0.0;
        return v;
    }

    // Where the picture is drawn: the bin window's mapping, pinned to the
    // advancing now while a zoomed view follows, blended toward the target
    // while a rescale animates.
    timeline::View paintView() const {
        const timeline::Window w = currentWindow();
        const QRect g = m_layout.graph;
        // Following: hang off now, over a span that GROWS with the recording
        // until it reaches the window — so a young recording fills the strip
        // instead of huddling at the right edge, and it grows continuously
        // (a real span, not a ladder rung, which is what made it step).
        timeline::View target;
        if (w.follow && m_spanMs > 0) {
            // Exactly what was recorded, edge to edge: the left edge sits on
            // the oldest moment kept, the right on now. No padding — the
            // 2 s floor used here put a young recording in the last tenth of
            // the bar with paper either side.
            const int64_t held = std::max<int64_t>(m_shownNowMs - m_state.retainedBeginMs, 1);
            const int64_t span = std::clamp<int64_t>(held, timeline::kMinFollowSpanMs, m_spanMs);
            target.pxPerMs = double(g.width()) / double(span);
            target.x0 = g.left() + g.width();
            target.tRef = m_shownNowMs;
        } else {
            target = timeline::viewFor(w, g.left(), g.width());
        }
        if (m_animStartMs < 0) return target;
        const double u = double(m_clock() - m_animStartMs) / double(timeline::kScaleAnimMs);
        if (u >= 1.0) return target;
        return timeline::lerpView(m_animFrom, target, timeline::easeInOut(u));
    }

    // The frame timer runs only while something actually moves.
    // The vertical scale doubles in one step (domainFor is power-of-two), so
    // snapping it drops the whole trace by ~30 % in a single frame. Blend it
    // in LOG space — halving and doubling then look the same size of move.
    double blendedDomain(uint32_t target) {
        const double want = double(std::max<uint32_t>(target, 1));
        if (m_domainShown <= 0) { m_domain = target; m_domainShown = want; return want; }
        if (target != m_domain) {
            m_domainFrom = m_domainShown;
            m_domain = target;
            m_domainAnimMs = m_clock();
        }
        if (m_domainAnimMs < 0) { m_domainShown = want; return want; }
        const double u = double(m_clock() - m_domainAnimMs) / double(timeline::kScaleAnimMs);
        if (u >= 1.0) { m_domainAnimMs = -1; m_domainShown = want; return want; }
        const double e = timeline::easeInOut(u);
        m_domainShown = std::exp(std::log(std::max(m_domainFrom, 1.0)) * (1.0 - e)
                                 + std::log(want) * e);
        return m_domainShown;
    }

    void updateFrameTimer() {
        const bool want = isVisible()
                       && (nowAdvances() || m_animStartMs >= 0 || m_domainAnimMs >= 0);
        if (want && !m_frame.isActive()) m_frame.start();
        else if (!want && m_frame.isActive()) m_frame.stop();
    }

    // A frame of motion: repaint only when the picture would actually differ,
    // so a slow scale costs a repaint a second and a fast one sixty.
    void onFrame() {
        ensureLayout();
        const bool wasAnimating = m_animStartMs >= 0;
        beginFrame();
        const timeline::View now = paintView();
        const qreal dpr = devicePixelRatioF();
        // Against what was last PAINTED, at a quarter pixel. Comparing this
        // frame with itself one tick later, at a whole pixel, meant the timer
        // never repainted at the settled span (one tick moves ~0.27 px), so
        // the trace only advanced on the 125 ms push — in 2 px jumps.
        if (wasAnimating || m_view.pxPerMs <= 0
            || timeline::viewsDiffer(m_view, now, m_layout.graph.left(), m_layout.graph.width(),
                                     0.25 / dpr))
            update();
    }

    // ── Layout and window ──
    void ensureLayout() {
        if (m_layoutWidth == width()) return;
        m_layout = timeline::layoutStrip(width());
        m_layoutWidth = width();
    }

    int graphDeviceColumns() const {
        return qMax(1, int(std::lround(m_layout.graph.width() * devicePixelRatioF())));
    }

    timeline::Window currentWindow() const {
        const int columns = graphDeviceColumns();
        if (m_frozen && m_frozenWindow.columns == columns) return m_frozenWindow;
        if (m_follow) {
            if (m_state.hasData && m_spanMs <= 0) {
                // The whole recording, from the left edge — not a sliver
                // jammed against the right. It grows into room kept on the
                // right, so the scale steps now and then instead of changing
                // every tick; the same room after Stop, so Stop moves nothing.
                const qreal dpr = devicePixelRatioF();
                return timeline::fitLeftWindow(m_state.retainedBeginMs, m_state.nowMs, columns,
                                               int(std::lround(timeline::kLeadPx * dpr)),
                                               int(std::lround(timeline::kTailPx * dpr)));
            }
            return timeline::followWindow(std::max<int64_t>(m_spanMs, timeline::kMinSpanMs),
                                          m_state.nowMs, columns);
        }
        timeline::Window w = m_manualWindow;
        if (w.columns != columns && w.columns > 0) {
            // Width changed: keep the right edge and the scale.
            const int64_t right = w.t1Ms();
            w.columns = columns;
            w.t0Ms = timeline::floorTo(right - w.stepMs * columns, w.stepMs);
        }
        return w;
    }

    void setWindow(const timeline::Window& w) {
        m_manualWindow = w;
        m_follow = w.follow;
        m_frozen = false;
        m_spanMs = w.spanMs();
        m_graphKey = {};
        m_userWindowChange = true;   // asked for: it happens at once
        update();
    }

    // The whole recording again ("Show Whole Recording", double-click).
    void fitAll() {
        m_spanMs = 0;
        m_follow = true;
        m_frozen = false;
        m_graphKey = {};
        m_userWindowChange = true;
        m_userFitted = true;   // asked for: a later Record leaves it alone
        update();
    }

public:
    // Frame [beginMs, endMs] and stop following now — "Show in Timeline".
    void showRange(int64_t beginMs, int64_t endMs) {
        ensureLayout();
        int64_t lo = std::max(beginMs, m_state.retainedBeginMs);
        int64_t hi = std::min(endMs, m_state.nowMs);
        if (hi - lo < timeline::kMinSpanMs) {
            const int64_t mid = lo + (hi - lo) / 2;
            lo = mid - timeline::kMinSpanMs / 2;
            hi = lo + timeline::kMinSpanMs;
        }
        timeline::Window w = timeline::fitWindow(lo, hi, graphDeviceColumns());
        w.follow = false;
        setWindow(w);
    }

private:
    // Follow now showing about `spanMs` (a zoom while live).
    void followWithSpan(int64_t spanMs) {
        m_spanMs = qMax<int64_t>(spanMs, timeline::kMinSpanMs);
        m_follow = true;
        m_frozen = false;
        m_graphKey = {};
        m_userWindowChange = true;
        m_userFitted = false;
        update();
    }

    // ── The overflow button ──
    bool isEnabledCell(const QString& id) const { return id == QStringLiteral("more"); }
    bool isClickable(const QString& id) const { return isEnabledCell(id); }

    void activate(const QString& id, const QPoint& pos) {
        Q_UNUSED(pos);
        if (id != QStringLiteral("more")) return;
        const QRect r = m_layout.rect(id);
        showMenu(mapToGlobal(QPoint(r.left(), 0)));
    }

    void paintIcon(QPainter& p, const QRect& cell, const QString& file, const QColor& tint) {
        const qreal dpr = devicePixelRatioF();
        const QIcon icon = themedVsIcon(QStringLiteral(":/vsicons/") + file, tint, timeline::kIconPx, dpr);
        const QPixmap pm = icon.pixmap(QSize(timeline::kIconPx, timeline::kIconPx), dpr);
        const QPointF at(cell.left() + (cell.width() - timeline::kIconPx) / 2.0,
                         cell.top() + (cell.height() - timeline::kIconPx) / 2.0);
        drawPixmapSnapped(p, at, pm);
    }

    // The overflow button: a neutral glyph, the straight hover fill of every
    // other chrome button, held while its menu is up.
    void paintCell(QPainter& p, const timeline::LaidCell& c) {
        const Theme& t = m_theme;
        if (c.id != QStringLiteral("more")) return;
        const bool hovered = m_hoverId == c.id;
        // The graph runs under the button now, so the glyph needs its own
        // ground: paper fading in from the left, opaque under the icon.
        {
            const QRectF scrim(c.rect.left() - 8, 0, c.rect.width() + 8 + timeline::kRightMargin, kHeight);
            QLinearGradient fade(scrim.left(), 0, c.rect.left(), 0);
            QColor clear = editorPaperColor(t), solid = clear;
            clear.setAlphaF(0.0);
            solid.setAlphaF(0.92);
            fade.setColorAt(0.0, clear);
            fade.setColorAt(1.0, solid);
            p.fillRect(scrim, fade);
            p.fillRect(QRectF(c.rect.left(), 0, scrim.right() - c.rect.left(), kHeight), solid);
        }
        if (m_pressId == c.id)       p.fillRect(c.rect, pressedFill(t));
        else if (hovered || m_menuUp) p.fillRect(c.rect, t.hover);
        paintIcon(p, c.rect, QStringLiteral("ellipsis.svg"), (hovered || m_menuUp) ? t.text : t.textDim);
    }

    // ── Inks ──
    struct Inks {
        QColor paper;
        QColor data;     // the number ink the envelope is drawn from
        QColor band;     // the recorded span, behind the envelope
        QColor base;     // the baseline under what was recorded
        QColor gap;      // the dotted baseline where nothing was recorded
        QColor fill;     // the envelope's body
        QColor line;     // the envelope's top edge
        QColor bead;     // "a field started changing"
        QColor marker;   // rebase / attach / detach / clear
        QColor hover;    // the pointer's hairline
    };
    Inks inks() const {
        const Theme& t = m_theme;
        Inks k;
        k.paper = editorPaperColor(t);
        // Data in the document's number ink — the colour the values
        // themselves are drawn in — so the graph reads as the values moving,
        // not as more grey chrome.
        const QColor data = t.syntaxNumber.isValid() ? t.syntaxNumber : t.text;
        k.data   = data;
        k.band   = mixColor(k.paper, t.textDim, 0.07);
        k.base   = mixColor(k.paper, t.textDim, 0.45);
        k.gap    = mixColor(k.paper, t.textDim, 0.45);
        k.fill   = mixColor(k.paper, data, 0.30);
        k.line   = mixColor(k.paper, data, 0.95);
        k.bead   = mixColor(k.paper, t.text, 0.85);
        k.marker = mixColor(k.paper, t.textDim, 0.55);
        k.hover  = mixColor(k.paper, t.text, 0.45);
        return k;
    }

    // ── The graph ──
    struct GraphKey {
        int64_t t0 = -1, step = 0; int columns = 0; quint64 gen = 0; qreal dpr = 0;
        int height = 0; QString theme; int64_t retained = 0, recBegin = -2, now = 0;
        int gapCount = -1; int64_t gapHash = 0; bool canCapture = false; quint64 selection = 0;
        // `now` is NOT part of the key: the recorded band runs to the image's
        // end and the paint clips it at the moment now has reached, so time
        // passing moves the writing edge without drawing the picture again.
        bool operator==(const GraphKey& o) const {
            return t0 == o.t0 && step == o.step && columns == o.columns && gen == o.gen
                && selection == o.selection
                && dpr == o.dpr && height == o.height && theme == o.theme && retained == o.retained
                && recBegin == o.recBegin && gapCount == o.gapCount && gapHash == o.gapHash
                && canCapture == o.canCapture;
        }
    };

    void paintGraph(QPainter& p) {
        const Theme& t = m_theme;
        const QRect g = m_layout.graph;
        if (g.width() <= 0) return;

        // Nothing recorded yet: say how to start, instead of an empty axis.
        const QString msg = emptyMessage();
        if (!msg.isEmpty()) {
            p.setPen(t.textDim);
            const int inset = timeline::kGutter;   // the graph is edge to edge; words are not
            p.drawText(QRect(g.left() + inset, 0, g.width() - 2 * inset, kHeight - 1),
                       Qt::AlignVCenter | Qt::AlignLeft,
                       QFontMetrics(font()).elidedText(msg, Qt::ElideRight, g.width() - 2 * inset));
            return;
        }

        beginFrame();
        const timeline::Window w = currentWindow();
        const qreal dpr = devicePixelRatioF();
        // An elapsed animation ends here as well as on a frame tick: a paint
        // can be driven by anything (a grab, an expose), and the state must
        // not depend on who asked for it.
        if (m_animStartMs >= 0 && m_clock() - m_animStartMs >= timeline::kScaleAnimMs)
            m_animStartMs = -1;
        // A scale or framing that moved without the user asking — a rung
        // change, a retention trim, a re-frame — is animated from whatever is
        // on screen. A pure slide never gets here: cmpView drops it.
        const timeline::View cmp = cmpView();
        if (m_cmpView.pxPerMs > 0 && !m_userWindowChange && m_animStartMs < 0
            && timeline::viewsDiffer(m_cmpView, cmp, g.left(), g.width(), 1.0)) {
            m_animFrom = m_view;
            m_animStartMs = m_clock();
        }
        m_cmpView = cmp;
        m_userWindowChange = false;
        const timeline::View V = paintView();
        m_view = V;
        updateFrameTimer();
        GraphKey key;
        key.t0 = w.t0Ms; key.step = w.stepMs; key.columns = w.columns;
        key.gen = m_state.dataGeneration; key.dpr = dpr; key.height = g.height();
        key.theme = m_theme.name; key.retained = m_state.retainedBeginMs;
        key.recBegin = m_state.recordBeginMs; key.now = m_state.nowMs;
        key.gapCount = m_state.gaps.size(); key.canCapture = m_state.canCapture();
        key.selection = m_state.selectionKey;
        for (const auto& s : m_state.gaps) key.gapHash = key.gapHash * 31 + s.startMs * 7 + s.endMs;
        auto xOf = [&](int64_t tMs) { return V.xOf(tMs); };
        const Inks ink = inks();
        const double nowX = std::clamp(xOf(m_shownNowMs), double(g.left()), double(g.right() + 1));

        // Records where they happened, whenever there are few enough to draw
        // one by one — the picture then follows the clock, not the bin grid.
        // Denser than that (zoomed far out), fall back to binned columns in a
        // cached picture, placed by the same view.
        ensurePoints(w);
        if (m_pointsOk) {
            p.save();
            p.setClipRect(QRectF(g.left(), 0, g.width(), kHeight), Qt::IntersectClip);
            paintPointGraph(p, V);
            p.restore();
        } else {
            if (!(key == m_graphKey) || m_graphImage.isNull()) {
                renderGraphImage(w, dpr);
                ++m_renderCount;
                m_graphKey = key;
            }
            // The picture was drawn for the bin window; the view may have
            // moved on (a slide) or be mid-rescale, so place it accordingly.
            // Everything past the moment now has reached is clipped away.
            const timeline::View base = timeline::viewFor(w, g.left(), g.width());
            const double dx = V.xOf(w.t0Ms) - base.xOf(w.t0Ms);
            const double k = base.pxPerMs > 0 ? V.pxPerMs / base.pxPerMs : 1.0;
            p.save();
            p.setClipRect(QRectF(g.left(), 0, nowX - g.left(), kHeight), Qt::IntersectClip);
            if (std::abs(k - 1.0) < 1e-6) {
                drawPixmapSnapped(p, QPointF(g.left() + dx, g.top()), QPixmap::fromImage(m_graphImage));
            } else {
                p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                p.translate(g.left() + dx, g.top());
                p.scale(k, 1.0);
                p.drawImage(QPointF(0, 0), m_graphImage);
                p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            }
            p.restore();
        }

        // The writing edge: while a recording runs, the newest ink fades in
        // over the last few px instead of appearing whole. Paper, not an
        // accent — the strip spends none.
        if (m_state.capture == timeline::Capture::Recording && !m_state.past && !m_dragging
            && nowX > g.left()) {
            const double from = std::max(double(g.left()), nowX - timeline::kLeadFadePx);
            QLinearGradient fade(from, 0, nowX, 0);
            QColor clear = ink.paper, solid = ink.paper;
            clear.setAlphaF(0.0);
            solid.setAlphaF(0.75);
            fade.setColorAt(0.0, clear);
            fade.setColorAt(1.0, solid);
            p.fillRect(QRectF(from, timeline::kPinTop, nowX - from,
                              timeline::kBandBottom - timeline::kPinTop), fade);
        }

        // No time is written on the graph: hovering it says when.

        // Rebase, attach, detach, clear: a dashed hairline through the band.
        // Record start / stop need none — the band's ends are where they are.
        for (const auto& m : m_state.markers) {
            if (m.kind == TimelineMarkerKind::RecordStart || m.kind == TimelineMarkerKind::RecordStop) continue;
            const double x = xOf(m.tMs);
            if (x < g.left() || x >= g.right()) continue;
            for (int y = timeline::kBandTop; y < timeline::kBandBottom; y += 3)
                fillLeftDeviceColOfRect(p, QRectF(x, y, 1, 2), ink.marker);
        }

        // (The selection lane is part of the cached image: hover and clock
        // repaints never re-query it.)

        // Looking back: the moment shown, an amber playhead with a handle to
        // grab, and what was recorded after it faded back — still there to
        // scrub forward to, but clearly not what is on screen.
        const bool showPast = m_state.past || m_dragging;
        if (showPast) {
            const int64_t viewed = m_dragging ? m_dragMs : m_state.viewedMs;
            const double playX = std::clamp(xOf(viewed), double(g.left()), double(g.right()));
            const double fadeEnd = std::clamp(xOf(m_shownNowMs), double(g.left()), double(g.right() + 1));
            if (fadeEnd > playX + 1) {
                QColor veil = ink.paper;
                veil.setAlphaF(0.58);
                p.fillRect(QRectF(playX + 1, timeline::kPinTop, fadeEnd - playX - 1,
                                  timeline::kBandBottom - timeline::kPinTop), veil);
            }
            fillLeftDeviceColOfRect(p, QRectF(playX, 0, 1, kHeight - 1), t.focusGlow);
            const double cx = (std::floor(playX * dpr) + 0.5) / dpr;
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(t.focusGlow);
            p.drawPolygon(QPolygonF({QPointF(cx - 4.5, 0), QPointF(cx + 4.5, 0), QPointF(cx, 5.5)}));
            p.restore();
        } else if (m_hoverX >= g.left() && m_hoverX < g.right()) {
            fillLeftDeviceColOfRect(p, QRectF(m_hoverX, timeline::kPinTop, 1,
                                              timeline::kSelBottom - timeline::kPinTop), ink.hover);
            // A dot where the envelope is under the pointer.
            const int col = int(std::floor((m_hoverX - g.left()) * dpr));
            if (col >= 0 && col < m_envTopDev.size() && m_envTopDev[col] >= 0) {
                const QPointF at((std::floor(m_hoverX * dpr) + 0.5) / dpr, g.top() + m_envTopDev[col] / dpr);
                p.save();
                p.setRenderHint(QPainter::Antialiasing, true);
                p.setPen(QPen(ink.paper, 1.2));
                p.setBrush(ink.line);
                p.drawEllipse(at, 2.6, 2.6);
                p.restore();
            }
        }
    }

    // ── The graph, drawn from the records themselves ──
    //
    // Records carry their own moment, so their x is a real number of pixels
    // from "now" — the picture moves with the clock rather than with the bin
    // grid, which is what makes it flow. Pulled once per data generation for
    // a padded span, then redrawn every frame from the same points.
    void ensurePoints(const timeline::Window& w) {
        if (!m_cb.points || !m_state.hasData) { m_pointsOk = false; return; }
        const int64_t pad = w.spanMs() / 4 + w.stepMs;
        const int64_t want0 = w.t0Ms - pad, want1 = w.t1Ms() + pad;
        if (m_pointsOk && m_pointsGen == m_state.dataGeneration
            && m_pointsSel == m_state.selectionKey
            && m_points0 <= w.t0Ms && m_points1 >= w.t1Ms())
            return;
        // Complete, not non-empty: a quiet window has no records and is still
        // the right answer, and flipping renderer for it changed the trace's
        // stroke and beads mid-recording.
        m_pointsOk = m_cb.points(want0, want1, timeline::kMaxDirectPoints, m_points);
        if (!m_pointsOk) m_points.clear();
        m_pointsGen = m_state.dataGeneration;
        m_pointsSel = m_state.selectionKey;
        m_points0 = want0;
        m_points1 = want1;
    }

    void paintPointGraph(QPainter& p, const timeline::View& V) {
        const QRect g = m_layout.graph;
        const Inks ink = inks();
        const double baseline = timeline::kBandBottom - 1;   // the baseline's row
        const double maxRise = baseline - timeline::kBandTop - 1;
        const double left = g.left(), right = g.right() + 1;
        auto clampX = [&](double x) { return std::clamp(x, left, right); };

        // 1. What was recorded: a faint band with a solid baseline, dotted
        //    where nothing was. It ends where now has reached.
        const double endX = clampX(V.xOf(std::min(m_shownNowMs, m_state.nowMs + timeline::kMaxClockDriftMs)));
        const double beginX = clampX(V.xOf(m_state.retainedBeginMs));
        // Antialiased: the writing edge moves a fraction of a pixel per frame,
        // and an aliased fill would round that back into a stair. The baseline
        // still takes exactly one device row (1 logical px is 1.25 rows at
        // 125 %, the project's hairline trap).
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        if (endX > beginX) {
            p.fillRect(QRectF(beginX, timeline::kBandTop, endX - beginX, baseline - timeline::kBandTop), ink.band);
            fillBottomDeviceRowOfRect(p, QRectF(beginX, baseline, endX - beginX, 1), ink.base);
        }
        for (const timeline::Span& gap : m_state.gaps) {
            const double a = clampX(V.xOf(gap.startMs));
            const double b = clampX(V.xOf(gap.endMs < 0 ? m_shownNowMs : gap.endMs));
            if (b <= a) continue;
            p.fillRect(QRectF(a, timeline::kBandTop, b - a, baseline + 1 - timeline::kBandTop), ink.paper);
            for (double x = a; x < b; x += 4)                       // a dotted baseline
                fillBottomDeviceRowOfRect(p, QRectF(x, baseline, 2, 1), ink.gap);
        }
        p.restore();

        // 2. The envelope: every record where it happened, joined into one
        //    shape while they are close, dropping to the baseline over a
        //    quiet stretch.
        // Only what is ON SCREEN sets the scale: the pull carries a quarter of
        // a window of padding either side, and letting that decide the height
        // made the trace rescale for data nobody can see.
        uint32_t visibleMax = 0;
        for (const TimelinePoint& pt : m_points) {
            if (pt.tMs < m_state.retainedBeginMs || pt.tMs > m_shownNowMs) continue;
            const double px = V.xOf(pt.tMs);
            if (px < left || px > right) continue;
            visibleMax = qMax(visibleMax, pt.changed);
        }
        const double domain = blendedDomain(timeline::domainFor(visibleMax, m_domain));

        struct Pt { double x, y; };
        QVector<Pt> pts;
        pts.reserve(m_points.size());
        for (const TimelinePoint& pt : m_points) {
            if (pt.tMs < m_state.retainedBeginMs || pt.tMs > m_shownNowMs) continue;
            const double x = V.xOf(pt.tMs);
            if (x < left - 8 || x > right + 8) continue;
            double rise = 0;
            if (pt.changed > 0) {
                const double frac = std::sqrt(std::min(double(pt.changed), domain)
                                              / std::max(domain, 1.0));
                rise = std::max(1.0, frac * maxRise);
            }
            pts.append(Pt{x, baseline - rise});
        }

        // ── The pen ──
        // The line has to reach both ends of what was recorded and keep up
        // with "now" between samples. Without that it starts in mid-air and
        // grows a whole vertex at a time — a record every ~200 ms is a
        // multi-pixel jump, which is what reads as chunky.
        if (!pts.isEmpty()) {
            const double joinPx = std::max(2.0, V.pxPerMs * double(timeline::kJoinMs));
            const int64_t firstT = V.tOf(pts.first().x);
            bool quietWasRecorded = true;
            for (const timeline::Span& gap : m_state.gaps)
                if (gap.startMs < firstT && (gap.endMs < 0 || gap.endMs > m_state.retainedBeginMs))
                    quietWasRecorded = false;
            if (quietWasRecorded && pts.first().x - beginX > 0.5) {
                if (pts.first().x - beginX > joinPx) {
                    // Nothing changed for a while before the first sample:
                    // lie on the baseline until it, then rise.
                    pts.prepend(Pt{std::max(beginX, pts.first().x - 2.0), baseline});
                    pts.prepend(Pt{beginX, baseline});
                } else {
                    pts.prepend(Pt{beginX, pts.first().y});
                }
            }
            // Hold the last value out to the writing edge so the pen advances
            // every frame — but only while a capture interval is a couple of
            // pixels. In a young, wide-open view each record is a deliberate
            // step and holding would smear it into a plateau.
            if (m_state.capture == timeline::Capture::Recording && !m_state.past && !m_dragging
                && m_points.size() >= 2) {
                double dt = 0;
                int n = 0;
                for (int i = m_points.size() - 1; i > 0 && n < 8; --i, ++n)
                    dt += double(m_points[i].tMs - m_points[i - 1].tMs);
                dt = n > 0 ? dt / n : 0.0;
                if (dt > 0 && V.pxPerMs * dt <= 8.0 && endX > pts.last().x + 0.25)
                    pts.append(Pt{endX, pts.last().y});
            }
        }
        const int devW = qMax(1, int(std::lround(g.width() * devicePixelRatioF())));
        m_envTopDev.fill(-1, devW);
        if (!pts.isEmpty()) {
            QLinearGradient body(0, timeline::kBandTop, 0, baseline + 1);
            QColor top = ink.data, bottom = ink.data;
            top.setAlphaF(0.46);
            bottom.setAlphaF(0.08);
            body.setColorAt(0.0, top);
            body.setColorAt(1.0, bottom);
            const QPen edge(ink.line, 1.15, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin);
            const double joinPx = std::max(2.0, V.pxPerMs * double(timeline::kJoinMs));
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            int a = 0;
            for (int i = 1; i <= pts.size(); ++i) {
                if (i < pts.size() && pts[i].x - pts[i - 1].x <= joinPx) continue;
                const int b = i - 1;
                const double half = (a == b) ? 1.0 : 0.5;
                QPainterPath shape, line;
                shape.moveTo(pts[a].x - half, baseline + 0.5);
                shape.lineTo(pts[a].x - half, pts[a].y);
                line.moveTo(pts[a].x - half, pts[a].y);
                for (int k = a; k <= b; ++k) { shape.lineTo(pts[k].x, pts[k].y); line.lineTo(pts[k].x, pts[k].y); }
                shape.lineTo(pts[b].x + half, pts[b].y);
                line.lineTo(pts[b].x + half, pts[b].y);
                shape.lineTo(pts[b].x + half, baseline + 0.5);
                shape.closeSubpath();
                p.fillPath(shape, QBrush(body));
                p.strokePath(line, edge);
                a = i;
            }
            p.restore();
            // Where the edge is per device column: the hover dot rides on it.
            const qreal dpr = devicePixelRatioF();
            for (int i = 0; i < pts.size(); ++i) {
                const int c0 = int(std::floor((pts[i == 0 ? 0 : i - 1].x - g.left()) * dpr));
                const int c1 = int(std::ceil((pts[i].x - g.left()) * dpr));
                for (int c = std::max(0, c0); c <= c1 && c < devW; ++c) {
                    double y = pts[i].y;
                    if (i > 0 && pts[i].x > pts[i - 1].x) {
                        const double f = std::clamp((g.left() + (c + 0.5) / dpr - pts[i - 1].x)
                                                        / (pts[i].x - pts[i - 1].x), 0.0, 1.0);
                        y = pts[i - 1].y + (pts[i].y - pts[i - 1].y) * f;
                    }
                    m_envTopDev[c] = int(std::lround((y - g.top()) * dpr));
                }
            }
        }

        // 3. Beads: what started changing, at most one per few pixels.
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(ink.bead);
        const double beadY = (timeline::kPinTop + timeline::kPinBottom) / 2.0;
        double lastBead = -1e9;
        for (const TimelinePoint& pt : m_points) {
            if (pt.onset == 0 || pt.tMs > m_shownNowMs) continue;
            const double x = V.xOf(pt.tMs);
            if (x < left || x > right || x - lastBead < 5.0) continue;
            lastBead = x;
            const double r = pt.onset >= 3 ? 2.3 : 1.7;
            p.drawEllipse(QPointF(x, beadY), r, r);
        }
        p.restore();

        // 4. The selected fields, in their own lane under the baseline.
        if (m_state.selectionKey != 0) {
            for (const TimelinePoint& pt : m_points) {
                if (!pt.selected || pt.tMs > m_shownNowMs) continue;
                const double x = V.xOf(pt.tMs);
                if (x < left || x > right) continue;
                p.fillRect(QRectF(x - 0.75, timeline::kSelTop, 1.5,
                                  timeline::kSelBottom - timeline::kSelTop), m_theme.indHoverSpan);
            }
        }
    }

    void renderGraphImage(const timeline::Window& w, qreal dpr) {
        const QRect g = m_layout.graph;
        const int devW = qMax(1, int(std::lround(g.width() * dpr)));
        const int devH = qMax(1, int(std::lround(g.height() * dpr)));
        m_graphImage = QImage(devW, devH, QImage::Format_ARGB32_Premultiplied);
        m_graphImage.fill(Qt::transparent);
        const Inks ink = inks();

        // Lane rows in image device rows.
        auto row = [&](int y) { return int(std::lround((y - g.top()) * dpr)); };
        const int bandTop = row(timeline::kBandTop);
        const int baseline = row(timeline::kBandBottom) - 1;           // the baseline's device row
        const int beadY = (row(timeline::kPinTop) + row(timeline::kPinBottom)) / 2;
        const int maxRows = qMax(2, baseline - bandTop - 1);            // a row of air above the peak

        const int n = qMin(w.columns, devW);
        QVector<TimelineColumn> cols;
        if (m_cb.columns && m_state.hasData) m_cb.columns(w.t0Ms, w.stepMs, w.columns, cols);
        cols.resize(w.columns);
        QVector<timeline::Region> regions(n);
        uint32_t visibleMax = 0;
        // Two region readings. The DATA lanes stop at the state's own now —
        // nothing was recorded past it. The band and baseline run to the
        // image's end and the paint clips them at the moment now has reached,
        // so the writing edge advances without drawing the picture again.
        QVector<timeline::Region> bandRegions(n);
        for (int x = 0; x < n; ++x) {
            const int64_t mid = w.t0Ms + w.stepMs * x + w.stepMs / 2;
            regions[x] = timeline::regionAt(mid, m_state.retainedBeginMs, m_state.nowMs, m_state.gaps);
            bandRegions[x] = timeline::regionAt(mid, m_state.retainedBeginMs, INT64_MAX, m_state.gaps);
            if (regions[x] == timeline::Region::Captured) visibleMax = qMax(visibleMax, cols[x].maxChanged);
        }
        m_domain = timeline::domainFor(visibleMax, m_domain);
        m_envTopDev.fill(-1, n);

        QPainter ip(&m_graphImage);
        ip.setRenderHint(QPainter::Antialiasing, false);

        // 1. What was recorded: a faint band with a solid baseline; a dotted
        //    baseline where nothing was recorded; nothing at all outside.
        for (int x = 0; x < n; ++x) {
            if (bandRegions[x] == timeline::Region::Captured) {
                int run = x;
                while (run + 1 < n && bandRegions[run + 1] == timeline::Region::Captured) ++run;
                ip.fillRect(x, bandTop, run - x + 1, baseline - bandTop, ink.band);
                ip.fillRect(x, baseline, run - x + 1, 1, ink.base);
                x = run;
            } else if (bandRegions[x] == timeline::Region::Gap) {
                if ((x / qMax(2, int(std::lround(2 * dpr)))) % 2 == 0) ip.fillRect(x, baseline, 1, 1, ink.gap);
            }
        }

        // 2. The envelope. Records are much sparser than device columns, so
        //    the points are joined record to record into one filled shape —
        //    columns alone made a comb of isolated 1-px ticks. Two records join
        //    when they are neighbours on screen or at most kJoinMs apart — a
        //    few capture ticks — so a quiet stretch between bursts drops to the
        //    baseline instead of drawing a mountain; never across a
        //    not-recorded stretch.
        struct Pt { double x; double y; };
        QVector<QVector<Pt>> segments(1);
        for (int x = 0; x < n; ++x) {
            if (regions[x] != timeline::Region::Captured) {
                if (!segments.last().isEmpty()) segments.append(QVector<Pt>());
                continue;
            }
            if (cols[x].records <= 0) continue;
            const uint32_t v = cols[x].maxChanged;
            double h = 0;
            if (v > 0) {
                const double frac = std::sqrt(double(std::min(v, m_domain)) / double(std::max<uint32_t>(m_domain, 1)));
                h = std::max(1.5, frac * maxRows);
            }
            segments.last().append(Pt{x + 0.5, baseline + 0.5 - h});
        }
        auto joined = [&](const Pt& a, const Pt& b) {
            const double cols = b.x - a.x;
            return cols <= 2.0 || cols * double(w.stepMs) <= double(timeline::kJoinMs);
        };
        ip.setRenderHint(QPainter::Antialiasing, true);
        const double floorY = baseline + 0.5;
        // The body fades toward the baseline under a crisp edge: steady churn
        // reads as a level, not as a solid block.
        QLinearGradient body(0, bandTop, 0, baseline + 1);
        {
            QColor top = ink.data, bottom = ink.data;
            top.setAlphaF(0.46);
            bottom.setAlphaF(0.08);
            body.setColorAt(0.0, top);
            body.setColorAt(1.0, bottom);
        }
        const QBrush bodyBrush(body);
        const QPen edge(ink.line, std::max(1.0, 1.15 * dpr), Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin);
        auto drawRun = [&](const QVector<Pt>& pts, int a, int b) {   // points [a, b]
            const double half = (a == b) ? 1.5 * dpr : 0.5;
            QPainterPath shape, top;
            shape.moveTo(pts[a].x - half, floorY);
            shape.lineTo(pts[a].x - half, pts[a].y);
            top.moveTo(pts[a].x - half, pts[a].y);
            for (int i = a; i <= b; ++i) { shape.lineTo(pts[i].x, pts[i].y); top.lineTo(pts[i].x, pts[i].y); }
            shape.lineTo(pts[b].x + half, pts[b].y);
            top.lineTo(pts[b].x + half, pts[b].y);
            shape.lineTo(pts[b].x + half, floorY);
            shape.closeSubpath();
            ip.fillPath(shape, bodyBrush);
            ip.strokePath(top, edge);
            // Where the edge is, per column — the hover dot sits on it.
            // Point i owns the columns after point i-1's, up to its own; the
            // first also its left half-width, the last its right one — and no
            // column past where the shape ends.
            for (int i = a; i <= b; ++i) {
                const int c0 = (i == a) ? int(std::floor(pts[i].x - half)) : int(std::floor(pts[i - 1].x)) + 1;
                const int c1 = (i == b) ? int(std::ceil(pts[i].x + half)) - 1 : int(std::floor(pts[i].x));
                for (int c = std::max(0, c0); c <= c1 && c < n; ++c) {
                    double y = pts[i].y;
                    if (i > a && pts[i].x > pts[i - 1].x) {
                        const double f = std::clamp((c + 0.5 - pts[i - 1].x) / (pts[i].x - pts[i - 1].x), 0.0, 1.0);
                        y = pts[i - 1].y + (pts[i].y - pts[i - 1].y) * f;
                    }
                    m_envTopDev[c] = int(std::lround(y));
                }
            }
        };
        for (const auto& seg : segments) {
            int start = 0;
            for (int i = 1; i <= seg.size(); ++i) {
                if (i == seg.size() || !joined(seg[i - 1], seg[i])) {
                    if (start < i) drawRun(seg, start, i - 1);
                    start = i;
                }
            }
        }

        // 3. What started changing: a bead above the band — a bounce stands
        //    out from the steady churn of a moving object. One bead per few
        //    pixels, the busiest record's; a bigger bead for several fields.
        if (m_cb.events && m_state.hasData) {
            QVector<uint32_t> started;
            m_cb.events(w.t0Ms, w.stepMs, w.columns, started);
            started.resize(w.columns);
            const int bucket = std::max(1, int(std::lround(5 * dpr)));
            ip.setPen(Qt::NoPen);
            ip.setBrush(ink.bead);
            for (int b0 = 0; b0 < n; b0 += bucket) {
                int best = -1;
                for (int c = b0; c < std::min(n, b0 + bucket); ++c)
                    if (regions[c] == timeline::Region::Captured && started[c] > 0
                        && (best < 0 || started[c] > started[best]))
                        best = c;
                if (best < 0) continue;
                const double r = (started[best] >= 3 ? 2.3 : 1.7) * dpr;
                ip.drawEllipse(QPointF(best + 0.5, beadY + 0.5), r, r);
            }
        }

        // 4. When the selected fields changed: short bars in the selection
        //    colour, in their own lane under the baseline — the envelope is
        //    the class, these are the rows you picked. Only where recorded.
        if (m_state.selectionKey != 0 && m_state.hasData && m_cb.selection) {
            QVector<char> hit;
            m_cb.selection(w.t0Ms, w.stepMs, w.columns, hit);
            hit.resize(w.columns);
            ip.setRenderHint(QPainter::Antialiasing, false);
            const int selTop = row(timeline::kSelTop);
            const int selRows = std::max(1, row(timeline::kSelBottom) - selTop);
            const int barW = std::max(2, int(std::lround(1.5 * dpr)));
            for (int c = 0; c < n; ++c) {
                if (!hit[c] || regions[c] != timeline::Region::Captured) continue;
                ip.fillRect(c, selTop, std::min(barW, n - c), selRows, m_theme.indHoverSpan);
                c += barW - 1;
            }
        }
        ip.end();
        m_graphImage.setDevicePixelRatio(dpr);
    }

    // ── Scrubbing ──
    int64_t timeAtX(int x, Qt::KeyboardModifiers mods) const {
        // Through the painted mapping: the pointer always addresses what is
        // on screen, mid-animation included.
        const timeline::View V = paintView();
        int64_t tMs = V.tOf(x);
        tMs = std::clamp(tMs, m_state.retainedBeginMs, m_state.nowMs);
        // A drag stops exactly where it is released: the moment shown is
        // whatever was captured at or before it, and standing between two
        // records is a legitimate place to stand. Ctrl snaps to a change for
        // when that IS what you want (stepping does it too: Ctrl+, / Ctrl+.).
        if ((mods & Qt::ControlModifier) && m_cb.changeTicks) {
            const int64_t tol = int64_t(timeline::kSnapPx / std::max(V.pxPerMs, 1e-9));
            const QVector<int64_t> ticks = m_cb.changeTicks(tMs - tol, tMs + tol + 1);
            tMs = timeline::snapToChange(tMs, ticks, tol);
        }
        return tMs;
    }

    void beginDrag(const QPoint& pos, Qt::KeyboardModifiers mods) {
        hideReadout();
        if (m_cb.onScrubBegin) m_cb.onScrubBegin();
        m_frozenWindow = currentWindow();
        m_frozen = true;                  // the window must not slide under the pointer
        m_priorPast = m_state.past;
        m_priorMs = m_state.viewedMs;
        m_dragging = true;
        m_dragMs = timeAtX(pos.x(), mods);
        m_lastSentMs = INT64_MIN;
        if (!m_filterInstalled && qApp) { qApp->installEventFilter(this); m_filterInstalled = true; }
        setCursor(Qt::SplitHCursor);
        m_scrubTimer.start();
        update();
    }

    void updateDrag(const QPoint& pos, Qt::KeyboardModifiers mods) {
        const int64_t tMs = timeAtX(pos.x(), mods);
        if (tMs == m_dragMs) return;
        m_dragMs = tMs;
        if (!m_scrubTimer.isActive()) m_scrubTimer.start();
        update();
    }

    void flushScrub(bool final) {
        if (!m_cb.onScrub) return;
        if (!final && m_dragMs == m_lastSentMs) return;
        m_lastSentMs = m_dragMs;
        m_cb.onScrub(m_dragMs, final);
    }

    void endDrag(const QPoint& pos) {
        stopDragTracking();
        const timeline::Window w = m_frozenWindow;
        // The live edge by time: released at or past where the recording
        // ends now — the frame keeps room on its right, so the graph's edge
        // is not "now" — or at the graph's right edge.
        const int gw = qMax(1, m_layout.graph.width());
        const int64_t tol = w.spanMs() * timeline::kSnapPx / gw;
        const int64_t released = timeline::timeForX(w, m_layout.graph.left(), gw, pos.x());
        const bool atLiveEdge = (w.follow || w.t1Ms() >= m_state.nowMs)
                             && (released + tol >= m_state.nowMs || pos.x() >= m_layout.graph.right() - 3);
        m_frozen = false;
        if (!m_follow) m_manualWindow = w;
        if (atLiveEdge) {
            m_follow = true;               // live again, so following — even if the
                                           // view went live during the drag
            if (m_cb.onReturnToLive) m_cb.onReturnToLive();
        } else {
            flushScrub(true);
            // Parking in the past stops following so the moment stays put.
            if (m_follow) { m_manualWindow = w; m_manualWindow.follow = false; m_follow = false; }
        }
        update();
    }

    void cancelDrag() {
        stopDragTracking();
        m_frozen = false;
        if (m_cb.onScrubCancel) {
            m_cb.onScrubCancel();
        } else if (m_priorPast) {
            if (m_cb.onScrub) m_cb.onScrub(m_priorMs, true);
        } else if (m_cb.onReturnToLive) {
            m_cb.onReturnToLive();
        }
        update();
    }

    void stopDragTracking() {
        // A move's pending scrub must not land after the release: it would put
        // the view back in the past right after a return to live.
        m_scrubTimer.stop();
        m_dragging = false;
        if (m_filterInstalled && qApp) { qApp->removeEventFilter(this); m_filterInstalled = false; }
        unsetCursor();
    }

    // ── Readout ──
    void showReadout() {
        if (m_hoverX < 0 || !m_state.hasData || m_dragging) return;
        const int64_t tMs = timeAtX(m_hoverX, Qt::NoModifier);
        // The one place a time is shown: the clock time of the moment under
        // the pointer, asked for by hovering.
        const QString title = timeline::formatClock(m_state.epochAtZeroMs + tMs, m_state.utcOffsetSeconds);
        QString body;
        const timeline::Region region = timeline::regionAt(tMs, m_state.retainedBeginMs, m_state.nowMs, m_state.gaps);
        if (region == timeline::Region::Gap) body = QStringLiteral("Not recorded");
        else if (m_cb.describe) body = m_cb.describe(tMs);
        if (!m_tip) m_tip = new RcxTooltip(nullptr);
        const Theme& t = m_theme;
        m_tip->setTheme(t.backgroundAlt, t.border, t.text, t.text, t.border);
        m_tip->populate(title, body, font());
        m_tip->showAt(mapToGlobal(QPoint(m_hoverX, 0)), /*preferAbove=*/true);
    }

    void hideReadout() {
        m_dwell.stop();
        if (m_tip) m_tip->dismiss();
    }

    // The widget's own tooltip names the hovered CELL; the graph's readout is
    // the private tip above. Republished only when the hovered cell changes.
    void refreshCellToolTip() {
        setToolTip(m_hoverId == QStringLiteral("more") ? QStringLiteral("Timeline options") : QString());
    }

    // ── Menu ──
    // Everything the strip does not show is either beside the address
    // (Record / Stop, and Back to live while looking back) or here. Following now is not an
    // option: the graph follows whenever the view is live.
    void buildMenu(QMenu& menu) {
        menu.setObjectName(QStringLiteral("rcxTimelineMenu"));
        QAction* prev = menu.addAction(QStringLiteral("Previous Change\tCtrl+,"));
        prev->setEnabled(m_state.hasData);
        connect(prev, &QAction::triggered, this, [this] { if (m_cb.onStep) m_cb.onStep(-1); });
        QAction* next = menu.addAction(QStringLiteral("Next Change\tCtrl+."));
        next->setEnabled(m_state.past);
        connect(next, &QAction::triggered, this, [this] { if (m_cb.onStep) m_cb.onStep(+1); });
        menu.addSeparator();
        QAction* fit = menu.addAction(QStringLiteral("Show Whole Recording"));
        fit->setEnabled(m_state.hasData);
        connect(fit, &QAction::triggered, this, [this] { fitAll(); });
        menu.addSeparator();
        QAction* clear = menu.addAction(QStringLiteral("Clear Recording…"));
        clear->setEnabled(m_state.hasData);
        connect(clear, &QAction::triggered, this, [this] { if (m_cb.onClear) m_cb.onClear(); });
        menu.addSeparator();
        QAction* hide = menu.addAction(QStringLiteral("Hide Timeline"));
        connect(hide, &QAction::triggered, this, [this] { if (m_cb.onHide) m_cb.onHide(); });
    }

    void showMenu(const QPoint& globalPos) {
        hideReadout();
        ++m_menuOpens;
        QMenu menu(this);
        buildMenu(menu);
        m_menuUp = true;
        update();
        menu.exec(globalPos);
        m_menuUp = false;
        update();
    }

    Theme m_theme;
    TimelineStripState m_state;
    Callbacks m_cb;
    timeline::Layout m_layout;
    int m_layoutWidth = -1;

    // Following: 0 = frame the whole recording (the resting view, and what
    // "Show Whole Recording" asks for), else the span to keep behind "now" —
    // the graph then hangs off now and history flows left out of it. Pressing
    // Record switches to that; Stop leaves the window alone, so stopping
    // rescales nothing.
    int64_t m_spanMs = 0;
    bool    m_userFitted = false;   // an explicit fit is not overridden
    bool m_follow = true;
    timeline::Window m_manualWindow;
    bool m_frozen = false;
    timeline::Window m_frozenWindow;

    QString m_hoverId, m_pressId;
    int m_hoverX = -1;
    bool m_dragging = false;
    int64_t m_dragMs = 0;
    int64_t m_lastSentMs = INT64_MIN;
    bool m_priorPast = false;
    int64_t m_priorMs = 0;
    bool m_panning = false;
    int m_panAnchorX = 0;
    timeline::Window m_panWindow;
    int m_wheelAccum = 0;
    bool m_filterInstalled = false;
    QTimer m_scrubTimer;
    QTimer m_dwell;
    RcxTooltip* m_tip = nullptr;
    int m_menuOpens = 0;
    bool m_menuUp = false;

    QImage m_graphImage;
    GraphKey m_graphKey;
    // The records in view, pulled once per data generation and redrawn every
    // frame. Empty / !ok: more records than pixels, so the binned picture
    // above is used instead.
    QVector<TimelinePoint> m_points;
    bool     m_pointsOk = false;
    quint64  m_pointsGen = 0;
    quint64  m_pointsSel = 0;
    int64_t  m_points0 = 0, m_points1 = -1;
    uint32_t m_domain = 4;
    double   m_domainShown = 0;      // 0: not established yet — the first paint never animates
    double   m_domainFrom = 0;
    int64_t  m_domainAnimMs = -1;    // -1: the scale is not moving
    QVector<int> m_envTopDev;        // the envelope's top edge per device column (-1: none)

    // ── Motion ──
    // The bins come from the state's own now (so `window()` means what it
    // always did); the PICTURE is placed by a View that keeps moving between
    // pushes, and a rescale is animated rather than snapped.
    std::function<int64_t()> m_clock;
    int64_t m_stateClockMs = 0;      // the clock when nowMs last changed
    int64_t m_shownNowMs = 0;        // now, advanced by the clock — set once per frame
    QTimer  m_frame;
    timeline::View m_view;           // what the last paint used
    timeline::View m_cmpView;        // the same, with the slide taken out
    timeline::View m_animFrom;       // where an animation started
    int64_t m_animStartMs = -1;      // -1: not animating
    bool    m_userWindowChange = false;   // a zoom / fit / reveal: never animated
    int     m_renderCount = 0;
    int     m_paintCount = 0;
};

} // namespace rcx
