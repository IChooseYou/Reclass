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

} // namespace rcx
