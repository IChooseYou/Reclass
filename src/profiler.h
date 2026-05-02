#pragma once
#include <QString>
#include <QHash>
#include <QMutex>
#include <QElapsedTimer>
#include <QtGlobal>
#include <limits>

namespace rcx {

// Per-bucket aggregated timing stats.
struct ProfileStats {
    qint64 totalNs = 0;                                    // sum of all sample durations
    qint64 minNs   = std::numeric_limits<qint64>::max();
    qint64 maxNs   = 0;
    qint64 lastNs  = 0;                                    // most recent sample (useful for live overlay)
    qint64 count   = 0;                                    // number of samples (qint64: a per-paint scope can rack up >2B over a long session)
};

// Lightweight aggregating profiler. Records (name, duration) samples from
// any thread; aggregates per-name min/max/total/count. Read via snapshot()
// for display. Reset clears everything to zero. Two operating modes:
//
//  * Disabled (default): record() is an early-return no-op so wrapping a
//    hot path with PROFILE_SCOPE costs ~2 ns of branch + return when
//    profiling is off in production builds.
//  * Enabled: record() takes the mutex and updates the bucket. Cost is
//    ~20–40 ns per sample, dwarfed by anything worth profiling.
//
// Enable with `Profiler::instance().setEnabled(true)` from the dialog.
class Profiler {
public:
    static Profiler& instance();

    void setEnabled(bool on) { m_enabled.storeRelaxed(on ? 1 : 0); }
    bool isEnabled() const   { return m_enabled.loadRelaxed() != 0; }

    void record(const char* name, qint64 nanos);
    QHash<QString, ProfileStats> snapshot() const;
    void reset();

private:
    Profiler() = default;
    QAtomicInt m_enabled { 0 };
    mutable QMutex m_mutex;
    QHash<QString, ProfileStats> m_stats;
};

// RAII scoped timer. Constructs a QElapsedTimer, starts it, records
// `nsecsElapsed()` against `name` on destruction. The `name` MUST be a
// string literal (we capture by pointer; no copy). One sample per scope.
//
// Enabled state is captured at construction so a scope is "all or nothing":
// flipping the global flag mid-scope does not retroactively start a sample
// (we'd have no start timestamp) and does not abort one in flight. The
// destructor's record() is a cheap no-op when m_active is false.
class ProfileScope {
public:
    explicit ProfileScope(const char* name)
        : m_name(name), m_active(Profiler::instance().isEnabled()) {
        if (m_active) m_timer.start();
    }
    ~ProfileScope() {
        if (m_active) Profiler::instance().record(m_name, m_timer.nsecsElapsed());
    }
    ProfileScope(const ProfileScope&)            = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
private:
    const char*    m_name;
    bool           m_active;
    QElapsedTimer  m_timer;
};

} // namespace rcx

// Convenience macro. The token-paste makes the local variable unique per
// expansion line so multiple PROFILE_SCOPE in one function don't collide.
#define RCX_PROFILE_PASTE_INNER(a, b) a##b
#define RCX_PROFILE_PASTE(a, b)       RCX_PROFILE_PASTE_INNER(a, b)
#define PROFILE_SCOPE(literalName) \
    ::rcx::ProfileScope RCX_PROFILE_PASTE(_rcxProf_, __LINE__){literalName}
