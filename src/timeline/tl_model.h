#pragma once

// The UI thread's mirror of a store's index.
//
// The store lives on its strand; the graph, the scrubber's snapping and
// "previous / next change" must answer instantly on the UI thread. So every
// commit carries a small summary back — record ids, times, changed-byte
// counts, flags, gaps, what is still retained — and this model keeps it.
// ~13 bytes per record plus the pyramid; no page data ever crosses.

#include "tl_store.h"

#include <QPair>
#include <QVector>
#include <algorithm>
#include <optional>

namespace rcx::tl {

struct RecordSummary {
    RecordId rec = kNoRecord;
    int64_t  timeMs = 0;
    uint32_t changedBytes = 0;
    uint8_t  flags = 0;
};

struct CommitBatch {
    QVector<RecordSummary> appended;
    // Changed spans of the appended records — only when a view asked for
    // them (per-class "fields changed" is computed on the UI thread, where
    // the layout lives).
    QVector<QPair<RecordId, QVector<ChangedSpan>>> spans;
    RecordId     firstRetained = kNoRecord;
    QVector<Gap> gaps;
    StoreStats   stats;
};

class TimelineModel {
public:
    void apply(const CommitBatch& b) {
        // Retention first: drop what the store no longer has.
        if (!m_times.isEmpty() && b.firstRetained != kNoRecord && b.firstRetained > m_base) {
            const int drop = int(std::min<qint64>(qint64(b.firstRetained) - m_base, m_times.size()));
            m_times.remove(0, drop);
            m_changed.remove(0, drop);
            m_flags.remove(0, drop);
            m_pyramid.dropFront(drop);
            m_base += RecordId(drop);
        }
        for (const RecordSummary& s : b.appended) {
            if (m_times.isEmpty()) {
                m_base = s.rec;
            } else if (s.rec != m_base + RecordId(m_times.size())) {
                // Out of step (should not happen): resynchronise from here.
                m_times.clear(); m_changed.clear(); m_flags.clear(); m_pyramid.clear();
                m_base = s.rec;
            }
            m_times.append(s.timeMs);
            m_changed.append(s.changedBytes);
            m_flags.append(s.flags);
            m_pyramid.append(s.changedBytes);
        }
        m_gaps = b.gaps;
        m_stats = b.stats;
        ++m_generation;
    }

    bool     isEmpty() const { return m_times.isEmpty(); }
    int      size() const { return m_times.size(); }
    RecordId firstRecord() const { return isEmpty() ? kNoRecord : m_base; }
    RecordId lastRecord() const { return isEmpty() ? kNoRecord : m_base + RecordId(m_times.size()) - 1; }
    bool     contains(RecordId r) const { return !isEmpty() && r >= m_base && r <= lastRecord(); }
    quint64  generation() const { return m_generation; }

    int64_t  timeOf(RecordId r) const { return contains(r) ? m_times[int(r - m_base)] : 0; }
    uint32_t changedBytesOf(RecordId r) const { return contains(r) ? m_changed[int(r - m_base)] : 0; }
    uint8_t  flagsOf(RecordId r) const { return contains(r) ? m_flags[int(r - m_base)] : 0; }

    RecordId recordAtOrBefore(int64_t ms) const {
        auto it = std::upper_bound(m_times.cbegin(), m_times.cend(), ms);
        if (it == m_times.cbegin()) return kNoRecord;
        return m_base + RecordId(it - m_times.cbegin()) - 1;
    }
    RecordId recordAtOrAfter(int64_t ms) const {
        auto it = std::lower_bound(m_times.cbegin(), m_times.cend(), ms);
        if (it == m_times.cend()) return kNoRecord;
        return m_base + RecordId(it - m_times.cbegin());
    }
    // Index range [lo, hi) of records with times in [t0, t1).
    QPair<int, int> indexRange(int64_t t0, int64_t t1) const {
        const int lo = int(std::lower_bound(m_times.cbegin(), m_times.cend(), t0) - m_times.cbegin());
        const int hi = int(std::lower_bound(m_times.cbegin(), m_times.cend(), t1) - m_times.cbegin());
        return {lo, std::max(lo, hi)};
    }
    MaxSumPyramid::Agg aggregate(int64_t t0, int64_t t1) const {
        const auto r = indexRange(t0, t1);
        return m_pyramid.query(r.first, r.second);
    }
    RecordId recordAtIndex(int i) const { return m_base + RecordId(i); }

    const QVector<Gap>& gaps() const { return m_gaps; }
    std::optional<Gap> gapAt(int64_t ms) const {
        for (auto it = m_gaps.crbegin(); it != m_gaps.crend(); ++it)
            if (it->contains(ms)) return *it;
        return std::nullopt;
    }
    const StoreStats& stats() const { return m_stats; }

private:
    RecordId          m_base = 0;
    QVector<int64_t>  m_times;
    QVector<uint32_t> m_changed;
    QVector<uint8_t>  m_flags;
    MaxSumPyramid     m_pyramid;
    QVector<Gap>      m_gaps;
    StoreStats        m_stats;
    quint64           m_generation = 0;
};

} // namespace rcx::tl
