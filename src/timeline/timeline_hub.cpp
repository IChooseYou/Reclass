#include "timeline_hub.h"

#include <algorithm>

namespace rcx::tl {

namespace {
const ClassTrack& defaultTrack() {
    static const ClassTrack t;
    return t;
}
} // namespace

TimelineHub::~TimelineHub() {
    if (m_ctx) m_ctx->clearRetentionFor(this);
}

void TimelineHub::bindSource(std::shared_ptr<CaptureContext> ctx, const QString& label, int64_t nowMs) {
    if (ctx == m_ctx) return;
    if (m_ctx) m_ctx->clearRetentionFor(this);
    m_ctx = std::move(ctx);
    // A pin is a record id of the context it was taken in; in another one it
    // would protect arbitrary records. A recording being kept is kept from
    // the start of this context's history instead.
    for (ClassTrack& t : m_tracks)
        if (t.pin != kNoRecord) t.pin = RecordId(0);
    if (m_ctx && !m_segments.contains(m_ctx)) m_segments.append(m_ctx);
    if (m_ctx) noteEvent(TimelineEventKind::SourceAttached, label, nowMs);
    applyRetention();
    changed();
}

void TimelineHub::unbindSource(const QString& label, int64_t nowMs) {
    if (!m_ctx) return;
    noteEvent(TimelineEventKind::SourceDetached, label, nowMs);
    m_ctx->clearRetentionFor(this);
    m_ctx.reset();
    changed();
}

void TimelineHub::clearHistory() {
    if (m_ctx) m_ctx->clearRetentionFor(this);
    m_ctx.reset();
    m_segments.clear();
    m_tracks.clear();
    m_events.clear();
    m_bases.clear();
    changed();
}

const ClassTrack& TimelineHub::track(uint64_t classId) const {
    auto it = m_tracks.constFind(classId);
    return it == m_tracks.constEnd() ? defaultTrack() : it.value();
}

ClassTrack& TimelineHub::mutableTrack(uint64_t classId) {
    return m_tracks[classId];
}

RecordId TimelineHub::oldestRetained() const {
    return m_ctx ? m_ctx->model().firstRecord() : kNoRecord;
}

void TimelineHub::startRecording(uint64_t classId, int64_t nowMs) {
    ClassTrack& t = mutableTrack(classId);
    if (t.mode == CaptureMode::Recording) return;
    if (t.recordedFromMs == INT64_MAX) t.recordedFromMs = nowMs;
    if (t.mode == CaptureMode::Paused) t.pausedSinceMs = -1;
    // The stretch since Stop (or a pause) was not recorded: that gap closes.
    if (!t.pauses.isEmpty() && t.pauses.last().isOpen()) t.pauses.last().endMs = nowMs;
    // A recording is kept until Clear, so a second Record goes on keeping
    // what the first one kept. The first keeps whatever is already retained;
    // with nothing captured yet, from the first record that arrives (ids only
    // grow), or a recording started right after attach keeps nothing.
    if (t.pin == kNoRecord) t.pin = (m_ctx && !m_ctx->model().isEmpty()) ? oldestRetained() : RecordId(0);
    t.mode = CaptureMode::Recording;
    t.resumeTo = CaptureMode::Recording;
    t.recordingSinceMs = nowMs;
    noteEvent(TimelineEventKind::RecordStart, QString(), nowMs, classId);
    applyRetention();
    changed();
}

void TimelineHub::stopRecording(uint64_t classId, int64_t nowMs) {
    ClassTrack& t = mutableTrack(classId);
    if (t.mode == CaptureMode::Paused && t.resumeTo == CaptureMode::Recording) {
        t.resumeTo = CaptureMode::Rolling;
    } else if (t.mode == CaptureMode::Recording) {
        t.mode = CaptureMode::Rolling;
        t.resumeTo = CaptureMode::Rolling;
        // What was recorded stays (the pin with it) until Clear. From here
        // nothing is captured for this class: "not recorded", never "nothing
        // changed", until the next Record closes the gap.
        Gap g;
        g.startMs = nowMs;
        g.reason = GapReason::Paused;
        t.pauses.append(g);
    } else {
        return;
    }
    t.recordingSinceMs = -1;
    noteEvent(TimelineEventKind::RecordStop, QString(), nowMs, classId);
    applyRetention();
    changed();
}

void TimelineHub::toggleRecording(uint64_t classId, int64_t nowMs) {
    const ClassTrack& t = track(classId);
    const bool recording = t.mode == CaptureMode::Recording
        || (t.mode == CaptureMode::Paused && t.resumeTo == CaptureMode::Recording);
    if (recording) stopRecording(classId, nowMs);
    else startRecording(classId, nowMs);
}

void TimelineHub::pause(uint64_t classId, int64_t nowMs) {
    ClassTrack& t = mutableTrack(classId);
    if (t.mode == CaptureMode::Paused) return;
    t.resumeTo = t.mode;
    t.mode = CaptureMode::Paused;
    // Pausing freezes the timeline: what is retained stops ageing out.
    if (t.pin == kNoRecord) t.pin = (m_ctx && !m_ctx->model().isEmpty()) ? oldestRetained() : RecordId(0);
    t.pausedSinceMs = nowMs;
    Gap g;
    g.startMs = nowMs;
    g.reason = GapReason::Paused;
    t.pauses.append(g);
    applyRetention();
    changed();
}

void TimelineHub::resume(uint64_t classId, int64_t nowMs) {
    ClassTrack& t = mutableTrack(classId);
    if (t.mode != CaptureMode::Paused) return;
    t.mode = t.resumeTo;
    if (!t.pauses.isEmpty() && t.pauses.last().isOpen()) t.pauses.last().endMs = nowMs;
    t.pausedSinceMs = -1;
    if (t.mode == CaptureMode::Rolling) t.pin = kNoRecord;
    applyRetention();
    changed();
}

void TimelineHub::togglePause(uint64_t classId, int64_t nowMs) {
    if (track(classId).mode == CaptureMode::Paused) resume(classId, nowMs);
    else pause(classId, nowMs);
}

void TimelineHub::reset(uint64_t classId, int64_t nowMs) {
    ClassTrack& t = mutableTrack(classId);
    const bool paused = t.mode == CaptureMode::Paused;
    // Clearing a recording discards what it kept, not the recording: it goes
    // on from now. Record (F9) is what stops it.
    const bool recording = t.mode == CaptureMode::Recording
        || (paused && t.resumeTo == CaptureMode::Recording);
    const RecordId next = (m_ctx && !m_ctx->model().isEmpty()) ? m_ctx->model().lastRecord() + 1 : RecordId(0);
    t.floorMs = nowMs;
    t.pauses.clear();
    t.recordingSinceMs = recording ? nowMs : -1;
    t.resumeTo = recording ? CaptureMode::Recording : CaptureMode::Rolling;
    if (paused) {
        Gap g;
        g.startMs = nowMs;
        g.reason = GapReason::Paused;
        t.pauses.append(g);
        t.pausedSinceMs = nowMs;
        t.pin = next;
    } else {
        t.mode = recording ? CaptureMode::Recording : CaptureMode::Rolling;
        t.pin = recording ? next : kNoRecord;
        t.pausedSinceMs = -1;
        // Not recording: nothing after the clear is captured either.
        if (!recording) {
            Gap g;
            g.startMs = nowMs;
            g.reason = GapReason::Paused;
            t.pauses.append(g);
        }
    }
    // Events for this class before the reset are gone with its history.
    m_events.erase(std::remove_if(m_events.begin(), m_events.end(),
        [&](const TimelineEvent& e) { return e.classId == classId && e.timeMs < nowMs; }),
        m_events.end());
    noteEvent(TimelineEventKind::Reset, QString(), nowMs, classId);
    if (recording) noteEvent(TimelineEventKind::RecordStart, QString(), nowMs, classId);
    applyRetention();
    changed();
}

bool TimelineHub::isHiddenFor(uint64_t classId, int64_t timeMs) const {
    const ClassTrack& t = track(classId);
    // Before its first Record — never recorded: always — a class has no
    // history, whatever the source's history holds for other classes.
    if (timeMs < t.recordedFromMs) return true;
    if (timeMs < t.floorMs) return true;
    for (const Gap& g : t.pauses)
        if (g.contains(timeMs)) return true;
    return false;
}

RecordId TimelineHub::firstVisibleRecord(uint64_t classId) const {
    if (!m_ctx || m_ctx->model().isEmpty()) return kNoRecord;
    const TimelineModel& m = m_ctx->model();
    const ClassTrack& t = track(classId);
    if (t.recordedFromMs == INT64_MAX) return kNoRecord;
    // The first record past the floor and the first Record that is not
    // inside a not-recorded stretch (after Stop, or a Clear while stopped).
    int64_t from = std::max(t.floorMs, t.recordedFromMs);
    for (int guard = 0; guard <= t.pauses.size(); ++guard) {
        const RecordId r = m.recordAtOrAfter(from);
        if (r == kNoRecord) return kNoRecord;
        const int64_t at = m.timeOf(r);
        const Gap* inside = nullptr;
        for (const Gap& g : t.pauses)
            if (g.contains(at)) { inside = &g; break; }
        if (!inside) return r;
        if (inside->isOpen()) return kNoRecord;
        from = inside->endMs;
    }
    return kNoRecord;
}

void TimelineHub::noteBase(uint64_t base, int64_t nowMs) {
    if (!m_bases.isEmpty() && m_bases.last().base == base) return;
    if (!m_bases.isEmpty() && nowMs < m_bases.last().timeMs) nowMs = m_bases.last().timeMs;
    m_bases.append({nowMs, base});
}

uint64_t TimelineHub::baseAt(int64_t timeMs, uint64_t fallback) const {
    auto it = std::upper_bound(m_bases.cbegin(), m_bases.cend(), timeMs,
        [](int64_t t, const BaseEpoch& e) { return t < e.timeMs; });
    if (it == m_bases.cbegin()) return m_bases.isEmpty() ? fallback : m_bases.first().base;
    return (it - 1)->base;
}

void TimelineHub::noteEvent(TimelineEventKind kind, const QString& label, int64_t nowMs, uint64_t classId) {
    TimelineEvent e;
    e.timeMs = nowMs;
    e.kind = kind;
    e.classId = classId;
    e.label = label;
    m_events.append(e);
    changed();
}

RetentionPolicy TimelineHub::retention() const {
    RetentionPolicy p;
    bool recording = false;
    RecordId protect = kNoRecord;
    for (auto it = m_tracks.constBegin(); it != m_tracks.constEnd(); ++it) {
        const ClassTrack& t = it.value();
        // A pin is a recording being kept — running, paused or stopped — and
        // a recording keeps everything, whatever the class is doing now.
        if (t.pin == kNoRecord) continue;
        recording = true;
        protect = std::min(protect, t.pin);
    }
    p.budgetBytes = recording ? m_budgets.recordingBytes : m_budgets.rollingBytes;
    p.windowMs = m_budgets.rollingWindowMs;
    p.protectFrom = protect;
    // A recording keeps everything, so it spills to disk; rolling never does.
    p.spill = recording;
    p.diskBudgetBytes = recording ? m_budgets.recordingDiskBytes : 0;
    p.blobRamBytes = std::min<qint64>(64LL << 20, m_budgets.recordingBytes / 4);
    return p;
}

void TimelineHub::applyRetention() {
    // Other hubs may share the context: it combines what each one needs.
    if (m_ctx) m_ctx->setRetentionFor(this, retention());
}

void TimelineHub::changed() {
    if (onChanged) onChanged();
}

} // namespace rcx::tl
