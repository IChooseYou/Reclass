#include "profiler.h"
#include <QMutexLocker>
#include <QVector>
#include <QPair>
#include <algorithm>
#include <cstdio>

namespace rcx {

Profiler& Profiler::instance() {
    static Profiler s_inst;
    return s_inst;
}

void Profiler::record(const char* name, qint64 nanos) {
    if (!isEnabled()) return;  // dropped if disabled between scope start and end
    QMutexLocker lock(&m_mutex);
    auto& s = m_stats[QString::fromLatin1(name)];
    s.totalNs += nanos;
    s.lastNs   = nanos;
    s.count   += 1;
    if (nanos < s.minNs) s.minNs = nanos;
    if (nanos > s.maxNs) s.maxNs = nanos;
}

QHash<QString, ProfileStats> Profiler::snapshot() const {
    QMutexLocker lock(&m_mutex);
    return m_stats;
}

void Profiler::reset() {
    QMutexLocker lock(&m_mutex);
    m_stats.clear();
}

void Profiler::dumpToStderr() const {
    QHash<QString, ProfileStats> snap = snapshot();
    QVector<QPair<QString, ProfileStats>> rows;
    rows.reserve(snap.size());
    for (auto it = snap.constBegin(); it != snap.constEnd(); ++it)
        rows.append({it.key(), it.value()});
    std::sort(rows.begin(), rows.end(),
              [](const QPair<QString, ProfileStats>& a,
                 const QPair<QString, ProfileStats>& b) {
                  return a.second.totalNs > b.second.totalNs;
              });

    fprintf(stderr, "\n=== PROFILE (by total time, %d scopes) ===\n", int(rows.size()));
    fprintf(stderr, "%-46s %10s %7s %10s %10s\n",
            "scope", "total_ms", "count", "avg_us", "max_us");
    for (const auto& r : rows) {
        const ProfileStats& s = r.second;
        const double totalMs = s.totalNs / 1e6;
        const double avgUs   = s.count ? (double(s.totalNs) / double(s.count)) / 1e3 : 0.0;
        const double maxUs   = s.maxNs / 1e3;
        fprintf(stderr, "%-46s %10.2f %7lld %10.1f %10.1f\n",
                r.first.toUtf8().constData(), totalMs,
                static_cast<long long>(s.count), avgUs, maxUs);
    }
    fprintf(stderr, "=== END PROFILE ===\n");
    fflush(stderr);
}

} // namespace rcx
