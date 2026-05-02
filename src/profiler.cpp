#include "profiler.h"
#include <QMutexLocker>

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

} // namespace rcx
