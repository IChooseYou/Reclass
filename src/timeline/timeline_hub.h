#pragma once

// ── TimelineHub: capture state for one "address owner" ──
//
// A document's tabs share one base address, so they share one hub; an
// instance tab (its own address) gets its own. The hub binds to the data
// source's CaptureContext (one byte history per process, shared further by
// every hub on that process) and owns everything that is about CLASSES rather
// than bytes:
//
//   · per-class capture mode — Rolling (not recording: the controller feeds
//     nothing), Recording, Paused
//   · pins: a recording keeps what it captured from ageing out until Clear —
//     Stop does not drop it, and a second Record goes on keeping it
//   · per-class floors: Reset hides everything before that moment
//   · per-class pause intervals — pauses, and the stretches between Stop and
//     the next Record: "not recorded", never "nothing changed"
//   · an event log (rebase, source attached/detached, record start/stop,
//     reset) and base-address epochs, so a past frame is shown at the
//     address the class had THEN
//
// UI-thread only. QtCore only, so it is tested headless.

#include "capture_context.h"
#include "tl_clock.h"

#include <QHash>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

namespace rcx::tl {

enum class TimelineEventKind : uint8_t {
    Rebase,
    SourceAttached,
    SourceDetached,
    RecordStart,
    RecordStop,
    Reset,
};

struct TimelineEvent {
    int64_t           timeMs = 0;
    TimelineEventKind kind = TimelineEventKind::Rebase;
    uint64_t          classId = 0;   // 0: applies to every class
    QString           label;
};

struct ClassTrack {
    CaptureMode mode = CaptureMode::Rolling;
    CaptureMode resumeTo = CaptureMode::Rolling;   // what Resume returns to
    RecordId    pin = kNoRecord;                   // kept from here while Recording/Paused
    int64_t     floorMs = INT64_MIN;               // Reset: nothing before this moment
    int64_t     recordingSinceMs = -1;
    int64_t     pausedSinceMs = -1;
    // The class's first Record. Nothing before it is this class's history —
    // not what another class or tab on the same source recorded earlier —
    // and a class never recorded has none at all.
    int64_t     recordedFromMs = INT64_MAX;
    QVector<Gap> pauses;                           // closed and open pause intervals
};

struct BaseEpoch {
    int64_t  timeMs = 0;
    uint64_t base = 0;
};

class TimelineHub {
public:
    TimelineHub() = default;
    ~TimelineHub();   // takes its retention policy back from the context
    TimelineHub(const TimelineHub&) = delete;
    TimelineHub& operator=(const TimelineHub&) = delete;

    // ── Source ──
    // Bind to a live source's history. A different source is noted as an
    // event; the previous history stays reachable through segments().
    void bindSource(std::shared_ptr<CaptureContext> ctx, const QString& label, int64_t nowMs);
    void unbindSource(const QString& label, int64_t nowMs);
    // Forget everything: the timeline was switched off. Drops every segment
    // (so the byte histories are freed), every class's mode, pins, pauses,
    // events and base epochs.
    void clearHistory();
    const std::shared_ptr<CaptureContext>& context() const { return m_ctx; }
    const QVector<std::shared_ptr<CaptureContext>>& segments() const { return m_segments; }

    // ── Classes ──
    const ClassTrack& track(uint64_t classId) const;
    CaptureMode mode(uint64_t classId) const { return track(classId).mode; }

    void startRecording(uint64_t classId, int64_t nowMs);
    void stopRecording(uint64_t classId, int64_t nowMs);
    void toggleRecording(uint64_t classId, int64_t nowMs);
    void pause(uint64_t classId, int64_t nowMs);
    void resume(uint64_t classId, int64_t nowMs);
    void togglePause(uint64_t classId, int64_t nowMs);
    void reset(uint64_t classId, int64_t nowMs);

    // Is `timeMs` inside a pause of this class, or before its floor?
    bool isHiddenFor(uint64_t classId, int64_t timeMs) const;
    // The first record this class may show (floor applied).
    RecordId firstVisibleRecord(uint64_t classId) const;

    // ── Base epochs ──
    void noteBase(uint64_t base, int64_t nowMs);
    uint64_t baseAt(int64_t timeMs, uint64_t fallback) const;

    // ── Events ──
    void noteEvent(TimelineEventKind kind, const QString& label, int64_t nowMs, uint64_t classId = 0);
    const QVector<TimelineEvent>& events() const { return m_events; }

    // ── Retention ──
    struct Budgets {
        qint64  rollingBytes = 64LL << 20;
        int64_t rollingWindowMs = 30LL * 60 * 1000;
        qint64  recordingBytes = 256LL << 20;       // RAM
        qint64  recordingDiskBytes = 16LL << 30;    // what a recording may spill
    };
    void setBudgets(const Budgets& b) { m_budgets = b; applyRetention(); }
    RetentionPolicy retention() const;
    void applyRetention();

    // Something observable about capture state changed (mode, pins, events).
    std::function<void()> onChanged;

private:
    ClassTrack& mutableTrack(uint64_t classId);
    RecordId oldestRetained() const;
    void changed();

    std::shared_ptr<CaptureContext> m_ctx;
    QVector<std::shared_ptr<CaptureContext>> m_segments;
    QHash<uint64_t, ClassTrack> m_tracks;
    QVector<TimelineEvent> m_events;
    QVector<BaseEpoch> m_bases;
    Budgets m_budgets;
};

} // namespace rcx::tl
