#pragma once

// Word-strided page diff for the live-refresh change highlighter.
//
// onReadComplete() compares each freshly-read memory page against the prior
// snapshot to find which byte offsets changed (for the heatmap / changed-
// byte highlight). The naive version was a per-byte compare; on big pages
// that are mostly unchanged it's pure overhead. diffPageInto() compares 8
// bytes at a time and only descends to per-byte work inside words that
// actually differ. The output (a membership set of absolute offsets) is
// byte-identical to the naive loop — see test_refresh_speedups fuzz test.

#include <QSet>
#include <QVector>
#include <algorithm>
#include <cstdint>
#include <cstring>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace rcx {

// Index of the lowest set bit (count-trailing-zeros). x must be non-zero.
inline int detail_ctz64(uint64_t x) {
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward64(&idx, x);
    return static_cast<int>(idx);
#else
    return __builtin_ctzll(x);
#endif
}

// Insert pageAddr + i into `out` for every i in [0, len) where
// oldP[i] != newP[i]. Returns true if any byte differed.
//
// Fast path: 8-byte stride with alignment-safe memcpy loads (the page
// buffers aren't guaranteed 8-aligned), XOR, and ctz to walk only the
// differing bytes of a non-equal word. The byte mapping (byte k of the
// loaded word lives at bits [k*8, k*8+8)) is little-endian — correct on
// x86_64, the only target here. The 0–7 byte tail is a portable per-byte
// compare. Output is identical to a naive per-byte loop because `out` is a
// set keyed on the absolute offset; word order within a page is irrelevant.
inline bool diffPageInto(QSet<int64_t>& out, uint64_t pageAddr,
                         const char* oldP, const char* newP, int len) {
    bool changed = false;
    int i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t a, b;
        std::memcpy(&a, oldP + i, 8);
        std::memcpy(&b, newP + i, 8);
        uint64_t x = a ^ b;
        if (!x) continue;
        changed = true;
        while (x) {
            int byte = detail_ctz64(x) >> 3;   // lowest differing byte
            out.insert(static_cast<int64_t>(pageAddr) + i + byte);
            x &= ~(0xFFULL << (byte * 8));       // clear that whole byte
        }
    }
    for (; i < len; ++i) {
        if (oldP[i] != newP[i]) {
            out.insert(static_cast<int64_t>(pageAddr) + i);
            changed = true;
        }
    }
    return changed;
}

// ── Changed-byte RUNS ──
//
// The refresh loop used to record every changed byte as its own hash-set
// entry, keyed by ABSOLUTE address, and then looked lines up by
// base-RELATIVE offset — so for any class not sitting at address 0 the
// changed-byte highlight silently never matched. A sorted vector of
// [addr, addr+len) runs fixes both halves: one entry per contiguous change
// instead of one per byte, and lookups by the absolute address compose
// already puts on every line (LineMeta::offsetAddr).
struct ChangedRun {
    uint64_t addr = 0;   // absolute address of the first changed byte
    uint32_t len  = 0;   // number of consecutive changed bytes (>= 1)
    bool operator==(const ChangedRun& o) const { return addr == o.addr && len == o.len; }
};

// Append the maximal runs of differing bytes in [0, len) to `out`, as
// absolute addresses. Returns true if any byte differed. Same 8-byte stride
// as diffPageInto: an unchanged page costs one XOR per word.
inline bool diffPageRuns(QVector<ChangedRun>& out, uint64_t pageAddr,
                         const char* oldP, const char* newP, int len) {
    bool changed = false;
    int runStart = -1;
    auto closeRun = [&](int end) {
        if (runStart < 0) return;
        out.append({pageAddr + static_cast<uint64_t>(runStart),
                    static_cast<uint32_t>(end - runStart)});
        runStart = -1;
    };
    int i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t a, b;
        std::memcpy(&a, oldP + i, 8);
        std::memcpy(&b, newP + i, 8);
        uint64_t x = a ^ b;
        if (!x) { closeRun(i); continue; }
        changed = true;
        for (int k = 0; k < 8; ++k) {
            if ((x >> (k * 8)) & 0xFFu) {
                if (runStart < 0) runStart = i + k;
            } else {
                closeRun(i + k);
            }
        }
    }
    for (; i < len; ++i) {
        if (oldP[i] != newP[i]) {
            changed = true;
            if (runStart < 0) runStart = i;
        } else {
            closeRun(i);
        }
    }
    closeRun(len);
    return changed;
}

// Sort by address and merge runs that touch or overlap. Pages are diffed in
// hash order, so a change straddling a page boundary arrives as two runs.
inline void normalizeRuns(QVector<ChangedRun>& runs) {
    if (runs.size() < 2) return;
    std::sort(runs.begin(), runs.end(),
              [](const ChangedRun& a, const ChangedRun& b) { return a.addr < b.addr; });
    int w = 0;
    for (int r = 1; r < runs.size(); ++r) {
        ChangedRun& cur = runs[w];
        const ChangedRun& nxt = runs[r];
        // Compare last bytes, not one-past-the-end: a run ending on the top
        // page of the address space would wrap addr + len to 0.
        const uint64_t curLast = cur.addr + (cur.len - 1);
        if (nxt.addr <= curLast || nxt.addr - curLast == 1) {
            const uint64_t nxtLast = nxt.addr + (nxt.len - 1);
            if (nxtLast > curLast)
                cur.len = static_cast<uint32_t>(nxtLast - cur.addr + 1);
        } else {
            runs[++w] = nxt;
        }
    }
    runs.resize(w + 1);
}

// True if any run in `sorted` (normalized) overlaps [addr, addr + len).
inline bool runsOverlap(const QVector<ChangedRun>& sorted, uint64_t addr, uint64_t len) {
    if (len == 0 || sorted.isEmpty()) return false;
    const uint64_t last = (addr > UINT64_MAX - (len - 1)) ? UINT64_MAX : addr + (len - 1);
    // First run whose last byte is at or after `addr`.
    auto it = std::partition_point(sorted.cbegin(), sorted.cend(),
        [addr](const ChangedRun& r) { return r.addr + (r.len - 1) < addr; });
    return it != sorted.cend() && it->addr <= last;
}

} // namespace rcx
