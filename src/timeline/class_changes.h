#pragma once

// ── What changed in a class, counted in fields ──
//
// The store counts bytes; a person thinks in fields. "health went down" is
// one change whether health is a byte or a double, and a vector moving is
// three. So each class view keeps its own series: for every record that
// touched it, how many of its fields changed and which ones. That series is
// the graph's height, the hover readout ("3 fields changed: health, ammo,
// pos.x") and the scope of previous / next change — including "of the field
// I have selected".
//
// Both halves live on the UI thread, where the layout is: a FieldIndex is
// the composed view's fields as absolute byte spans, and a record's changed
// spans map onto it in O(spans · log fields). Only records that changed a
// field are kept.

#include "tl_pyramid.h"
#include "tl_store.h"

#include <QPair>
#include <QVector>
#include <algorithm>
#include <cstdint>

namespace rcx::tl {

struct FieldSpan {
    uint64_t addr = 0;
    uint32_t len = 0;
    uint64_t id = 0;       // the row's selection id: selecting it scopes stepping to it
};

class FieldIndex {
public:
    void clear() { m_fields.clear(); m_maxEnd.clear(); }

    void build(QVector<FieldSpan> fields) {
        fields.erase(std::remove_if(fields.begin(), fields.end(),
                                    [](const FieldSpan& f) { return f.len == 0; }),
                     fields.end());
        std::sort(fields.begin(), fields.end(), [](const FieldSpan& a, const FieldSpan& b) {
            return a.addr != b.addr ? a.addr < b.addr : a.len > b.len;
        });
        m_fields = std::move(fields);
        // Unions overlap, so ends are not sorted; a running max makes "the
        // first field that could reach this address" a binary search.
        m_maxEnd.resize(m_fields.size());
        uint64_t running = 0;
        for (int i = 0; i < m_fields.size(); ++i) {
            running = std::max(running, endOf(m_fields[i].addr, m_fields[i].len));
            m_maxEnd[i] = running;
        }
    }

    bool isEmpty() const { return m_fields.isEmpty(); }
    int  size() const { return m_fields.size(); }
    const FieldSpan& at(int i) const { return m_fields[i]; }
    const QVector<FieldSpan>& fields() const { return m_fields; }

    // Ordinals of the fields any span overlaps: ascending, unique.
    void hits(const QVector<ChangedSpan>& spans, QVector<int>& out) const {
        out.clear();
        if (m_fields.isEmpty()) return;
        for (const ChangedSpan& s : spans) {
            if (s.len == 0) continue;
            const uint64_t sEnd = endOf(s.addr, s.len);
            int i = int(std::partition_point(m_maxEnd.cbegin(), m_maxEnd.cend(),
                                             [&](uint64_t e) { return e <= s.addr; })
                        - m_maxEnd.cbegin());
            for (; i < m_fields.size() && m_fields[i].addr < sEnd; ++i)
                if (endOf(m_fields[i].addr, m_fields[i].len) > s.addr) out.append(i);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    // The fields whose id is in `ids` — a selection, as spans.
    template <typename IdSet>
    QVector<FieldSpan> spansOf(const IdSet& ids) const {
        QVector<FieldSpan> out;
        for (const FieldSpan& f : m_fields)
            if (ids.contains(f.id)) out.append(f);
        return out;
    }

    static uint64_t endOf(uint64_t addr, uint64_t len) {
        return addr > UINT64_MAX - len ? UINT64_MAX : addr + len;
    }

private:
    QVector<FieldSpan> m_fields;
    QVector<uint64_t>  m_maxEnd;
};

class ClassChangeSeries {
public:
    // Ids kept per record, for the readout and field-scoped stepping. A
    // record that changed more keeps its count exactly and the byte hull of
    // what it changed, so a scoped search can still find it.
    static constexpr int kMaxIdsPerRecord = 32;
    // A record more than this after the one before is not a continuation of
    // it: everything it changed counts as having started (a flag flipping
    // every few seconds is an event each time; so is the first record after a
    // pause or a quiet spell).
    static constexpr int64_t kContinuousMs = 1200;

    void clear() {
        m_recs.clear(); m_times.clear(); m_counts.clear(); m_idBegin.clear();
        m_lo.clear(); m_hi.clear(); m_ids.clear(); m_pyramid.clear();
        m_onsets.clear(); m_onsetPyramid.clear();
    }
    bool isEmpty() const { return m_recs.isEmpty(); }
    int  size() const { return m_recs.size(); }
    RecordId lastRecord() const { return m_recs.isEmpty() ? kNoRecord : m_recs.last(); }

    // `hull` = [lo, hi) of every changed byte that hit a field.
    void append(RecordId r, int64_t tMs, int count, const QVector<uint64_t>& ids,
                uint64_t lo, uint64_t hi) {
        if (count <= 0) return;
        if (!m_recs.isEmpty() && r <= m_recs.last()) return;   // replayed or out of order
        // How many of its fields STARTED changing — counted once, here, while
        // the record before is still the last one: fields that record did
        // not change, or all of them after a gap. The very first record starts
        // nothing, and a record whose ids were cut short is never guessed at.
        uint32_t started = 0;
        const int kept = std::min(int(ids.size()), kMaxIdsPerRecord);
        if (!m_recs.isEmpty() && kept >= count) {
            const int p = m_recs.size() - 1;
            if (tMs - m_times[p] > kContinuousMs) {
                started = uint32_t(count);
            } else if (idEnd(p) - m_idBegin[p] >= int(m_counts[p])) {
                for (int k = 0; k < kept; ++k) {
                    bool before = false;
                    for (int j = m_idBegin[p]; j < idEnd(p) && !before; ++j)
                        before = m_ids[j] == ids[k];
                    if (!before) ++started;
                }
            }
        }
        m_recs.append(r);
        m_times.append(tMs);
        m_counts.append(uint32_t(count));
        m_idBegin.append(m_ids.size());
        for (int i = 0; i < ids.size() && i < kMaxIdsPerRecord; ++i) m_ids.append(ids[i]);
        m_lo.append(lo);
        m_hi.append(hi);
        m_pyramid.append(uint32_t(count));
        m_onsets.append(started);
        m_onsetPyramid.append(started);
    }

    // Append `newer`'s records after this series' last — a recount finished
    // while live commits kept being counted.
    void appendNewer(const ClassChangeSeries& newer) {
        int i = 0;
        if (!isEmpty())
            i = int(std::upper_bound(newer.m_recs.cbegin(), newer.m_recs.cend(), lastRecord())
                    - newer.m_recs.cbegin());
        for (; i < newer.size(); ++i)
            append(newer.m_recs[i], newer.m_times[i], int(newer.m_counts[i]),
                   newer.m_ids.mid(newer.m_idBegin[i], newer.idEnd(i) - newer.m_idBegin[i]),
                   newer.m_lo[i], newer.m_hi[i]);
    }

    // Retention moved: forget records the store no longer has.
    void dropBefore(RecordId first) {
        const int n = int(std::lower_bound(m_recs.cbegin(), m_recs.cend(), first) - m_recs.cbegin());
        if (n <= 0) return;
        if (n >= m_recs.size()) { clear(); return; }
        const int cut = m_idBegin[n];
        m_recs.remove(0, n); m_times.remove(0, n); m_counts.remove(0, n);
        m_idBegin.remove(0, n); m_lo.remove(0, n); m_hi.remove(0, n);
        m_ids.remove(0, cut);
        for (int& b : m_idBegin) b -= cut;
        m_pyramid.dropFront(n);
        m_onsets.remove(0, n);
        m_onsetPyramid.dropFront(n);
    }

    int indexOf(RecordId r) const {
        auto it = std::lower_bound(m_recs.cbegin(), m_recs.cend(), r);
        return (it != m_recs.cend() && *it == r) ? int(it - m_recs.cbegin()) : -1;
    }
    int countOf(RecordId r) const {
        const int i = indexOf(r);
        return i < 0 ? 0 : int(m_counts[i]);
    }
    QVector<uint64_t> idsOf(RecordId r) const {
        const int i = indexOf(r);
        if (i < 0) return {};
        return m_ids.mid(m_idBegin[i], idEnd(i) - m_idBegin[i]);
    }
    RecordId recordAt(int i) const { return m_recs[i]; }
    int64_t  timeAt(int i) const { return m_times[i]; }

    // Peak and total fields changed over records in [t0, t1).
    MaxSumPyramid::Agg aggregate(int64_t t0, int64_t t1) const {
        const auto r = indexRange(t0, t1);
        return m_pyramid.query(r.first, r.second);
    }

    // Does record i touch the scope? Null scope = the whole class.
    bool matches(int i, const QVector<FieldSpan>* scope) const {
        if (!scope) return true;
        for (int k = m_idBegin[i]; k < idEnd(i); ++k)
            for (const FieldSpan& f : *scope)
                if (f.id == m_ids[k]) return true;
        if (idEnd(i) - m_idBegin[i] < int(m_counts[i])) {
            // Too many fields to list: fall back to the byte hull.
            for (const FieldSpan& f : *scope)
                if (f.addr < m_hi[i] && FieldIndex::endOf(f.addr, f.len) > m_lo[i]) return true;
        }
        return false;
    }

    // How many fields record r started changing (see append) — a bounce
    // among steady motion, a restart, a flag flipping: the strip's beads.
    int onsetsOf(RecordId r) const {
        const int i = indexOf(r);
        return i < 0 ? 0 : int(m_onsets[i]);
    }
    // The most fields one record in [t0, t1) started changing (0: none), in
    // O(log n) — one query per graph column, however long the recording.
    uint32_t onsetMax(int64_t t0, int64_t t1) const {
        const auto r = indexRange(t0, t1);
        return m_onsetPyramid.query(r.first, r.second).max;
    }
    // Per column of `step` from t0: did a record touching `scope` land in it?
    // Skips to the next column after a hit, so a field changing every tick
    // costs about one match per column — and there is no cap: the lane
    // covers the whole window.
    void matchColumns(int64_t t0, int64_t step, int n, const QVector<FieldSpan>* scope,
                      QVector<char>& out) const {
        out.fill(0, std::max(n, 0));
        if (n <= 0 || step <= 0) return;
        const auto r = indexRange(t0, t0 + step * n);
        int i = r.first;
        while (i < r.second) {
            if (!matches(i, scope)) { ++i; continue; }
            const int64_t col = (m_times[i] - t0) / step;
            if (col >= 0 && col < n) out[int(col)] = 1;
            const int64_t nextColumn = t0 + (col + 1) * step;
            i = int(std::lower_bound(m_times.cbegin() + i + 1, m_times.cbegin() + r.second, nextColumn)
                    - m_times.cbegin());
        }
    }

    // One entry per record, for a graph that draws records as themselves
    // rather than binning them into columns — what lets the strip flow
    // instead of stepping a column at a time.
    struct Point {
        int64_t  tMs = 0;
        uint32_t count = 0;     // fields this record changed
        uint32_t onset = 0;     // fields it STARTED changing (the beads)
        bool     matched = false;   // it touched the scope
    };

    // Every record in [t0, t1), oldest first. Returns whether the answer is
    // COMPLETE: false only when there are more than `cap` of them (0: no cap),
    // which is the caller's signal to bin instead. An empty window is a
    // complete answer — a quiet stretch must not look like "too dense", or the
    // graph flips renderer mid-recording and the trace visibly changes shape.
    bool points(int64_t t0, int64_t t1, const QVector<FieldSpan>* scope, int cap,
                QVector<Point>& out) const {
        out.clear();
        const auto r = indexRange(t0, t1);
        const int n = r.second - r.first;
        if (n <= 0) return true;
        if (cap > 0 && n > cap) return false;
        out.reserve(n);
        for (int i = r.first; i < r.second; ++i)
            out.append(Point{m_times[i], m_counts[i], m_onsets[i], matches(i, scope)});
        return true;
    }

    // Newest matching record strictly before `before` (kNoRecord: none).
    RecordId previous(RecordId before, const QVector<FieldSpan>* scope = nullptr,
                      RecordId floor = 0) const {
        int i = int(std::lower_bound(m_recs.cbegin(), m_recs.cend(), before) - m_recs.cbegin());
        while (--i >= 0) {
            if (m_recs[i] < floor) break;
            if (matches(i, scope)) return m_recs[i];
        }
        return kNoRecord;
    }
    // Oldest matching record strictly after `after`.
    RecordId next(RecordId after, const QVector<FieldSpan>* scope = nullptr) const {
        int i = int(std::upper_bound(m_recs.cbegin(), m_recs.cend(), after) - m_recs.cbegin());
        for (; i < m_recs.size(); ++i)
            if (matches(i, scope)) return m_recs[i];
        return kNoRecord;
    }

    // Times of matching records in [t0, t1), at most `cap`.
    QVector<int64_t> times(int64_t t0, int64_t t1, const QVector<FieldSpan>* scope = nullptr,
                           int cap = 256) const {
        QVector<int64_t> out;
        const auto r = indexRange(t0, t1);
        for (int i = r.first; i < r.second && out.size() < cap; ++i)
            if (matches(i, scope)) out.append(m_times[i]);
        return out;
    }

private:
    int idEnd(int i) const { return i + 1 < m_idBegin.size() ? m_idBegin[i + 1] : m_ids.size(); }
    QPair<int, int> indexRange(int64_t t0, int64_t t1) const {
        const int lo = int(std::lower_bound(m_times.cbegin(), m_times.cend(), t0) - m_times.cbegin());
        const int hi = int(std::lower_bound(m_times.cbegin(), m_times.cend(), t1) - m_times.cbegin());
        return {lo, std::max(lo, hi)};
    }

    QVector<RecordId> m_recs;
    QVector<int64_t>  m_times;
    QVector<uint32_t> m_counts;
    QVector<int>      m_idBegin;
    QVector<uint64_t> m_lo, m_hi;
    QVector<uint64_t> m_ids;
    MaxSumPyramid     m_pyramid;
    QVector<uint32_t> m_onsets;          // fields each record started changing
    MaxSumPyramid     m_onsetPyramid;
};

} // namespace rcx::tl
