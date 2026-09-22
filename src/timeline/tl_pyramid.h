#pragma once

// A max/sum pyramid over a growing series — one value per record.
//
// The timeline graph asks one question per pixel column: "over these
// records, what was the peak and the total?". A fan-out-16 pyramid answers
// any index range in O(log n), so a 1920-column graph over millions of
// records costs a few thousand lookups, not a scan.

#include <QVector>
#include <algorithm>
#include <cstdint>

namespace rcx::tl {

class MaxSumPyramid {
public:
    struct Agg {
        uint32_t max = 0;
        uint64_t sum = 0;
        int      count = 0;
    };

    int size() const { return m_max.isEmpty() ? 0 : m_max[0].size(); }
    bool isEmpty() const { return size() == 0; }

    void clear() { m_max.clear(); m_sum.clear(); }

    void append(uint32_t v) {
        if (m_max.isEmpty()) { m_max.resize(1); m_sum.resize(1); }
        m_max[0].append(v);
        m_sum[0].append(v);
        int idx = m_max[0].size() - 1;
        // A level exists only while the one below has more than one node.
        for (int level = 1; m_max[level - 1].size() > 1; ++level) {
            const int parent = idx / kFan;
            if (level >= m_max.size()) {
                // A brand-new top level must summarise EVERYTHING below it,
                // not just the value that made it necessary — the level
                // below already includes v, so aggregate it wholesale.
                m_max.resize(level + 1);
                m_sum.resize(level + 1);
                const QVector<uint32_t>& bm = m_max[level - 1];
                const QVector<uint64_t>& bs = m_sum[level - 1];
                QVector<uint32_t>& mx = m_max[level];
                QVector<uint64_t>& sm = m_sum[level];
                for (int i = 0; i < bm.size(); ++i) {
                    const int p = i / kFan;
                    if (p >= mx.size()) { mx.append(0); sm.append(0); }
                    mx[p] = std::max(mx[p], bm[i]);
                    sm[p] += bs[i];
                }
            } else {
                QVector<uint32_t>& mx = m_max[level];
                QVector<uint64_t>& sm = m_sum[level];
                if (parent >= mx.size()) { mx.append(0); sm.append(0); }
                mx[parent] = std::max(mx[parent], v);
                sm[parent] += v;
            }
            idx = parent;
        }
    }

    // Drop the first n values (retention trims whole chunks, rarely).
    void dropFront(int n) {
        if (n <= 0) return;
        if (n >= size()) { clear(); return; }
        QVector<uint32_t> keep = m_max[0].mid(n);
        clear();
        for (uint32_t v : keep) append(v);
    }

    uint32_t valueAt(int i) const { return m_max[0][i]; }

    // Aggregate over values [lo, hi).
    Agg query(int lo, int hi) const {
        Agg a;
        lo = std::max(lo, 0);
        hi = std::min(hi, size());
        if (lo >= hi) return a;
        a.count = hi - lo;
        int level = 0;
        while (lo < hi) {
            const bool top = (level + 1 >= m_max.size());
            if (top) {
                for (int i = lo; i < hi; ++i) take(a, level, i);
                break;
            }
            while (lo < hi && (lo % kFan) != 0) take(a, level, lo++);
            while (lo < hi && (hi % kFan) != 0) take(a, level, --hi);
            if (lo >= hi) break;
            lo /= kFan;
            hi /= kFan;
            ++level;
        }
        return a;
    }

private:
    static constexpr int kFan = 16;
    void take(Agg& a, int level, int i) const {
        a.max = std::max(a.max, m_max[level][i]);
        a.sum += m_sum[level][i];
    }
    QVector<QVector<uint32_t>> m_max;   // level 0 = the values themselves
    QVector<QVector<uint64_t>> m_sum;
};

} // namespace rcx::tl
