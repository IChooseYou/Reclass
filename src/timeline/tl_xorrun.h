#pragma once

// XOR-run deltas between two versions of one page.
//
// A change is stored as the XOR of old and new bytes over the runs that
// differ. XOR is its own inverse, so the SAME payload steps a page forward
// (old → new) and backward (new → old): scrubbing one change in either
// direction costs the bytes that changed, not a page reconstruction.
//
// Payload layout (XorRuns):
//   runCount varint
//   per run:  gap varint   (bytes from the end of the previous run, or from 0)
//             len varint   (1 .. 4096)
//             len XOR bytes
// Runs separated by ≤ kRunMergeGap equal bytes are encoded as one; the equal
// bytes inside carry XOR 0, which is exactly what applying them requires.
//
// XorDense is 4096 raw XOR bytes, chosen whenever runs would be no smaller.

#include "tl_types.h"
#include "tl_varint.h"

#include <QByteArray>
#include <QVector>
#include <cstring>
#include <functional>

namespace rcx::tl {

struct XorStats {
    uint32_t changedBytes = 0;   // bytes that actually differ (merge gaps excluded)
    uint32_t runCount = 0;       // encoded runs (after merging)
};

namespace detail {
struct RawRun { int start; int len; };

// Maximal differing runs of [0, len), 8-byte stride.
inline void collectRuns(const char* a, const char* b, int len,
                        QVector<RawRun>& runs, uint32_t& changedBytes) {
    int runStart = -1;
    auto close = [&](int end) {
        if (runStart < 0) return;
        runs.append({runStart, end - runStart});
        runStart = -1;
    };
    int i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t x, y;
        std::memcpy(&x, a + i, 8);
        std::memcpy(&y, b + i, 8);
        uint64_t d = x ^ y;
        if (!d) { close(i); continue; }
        for (int k = 0; k < 8; ++k) {
            if ((d >> (k * 8)) & 0xFFu) {
                ++changedBytes;
                if (runStart < 0) runStart = i + k;
            } else {
                close(i + k);
            }
        }
    }
    for (; i < len; ++i) {
        if (a[i] != b[i]) { ++changedBytes; if (runStart < 0) runStart = i; }
        else close(i);
    }
    close(len);
}
} // namespace detail

// Encode the delta old → new (both `len` bytes) and append it to `out`.
// Returns the op kind used, or XorRuns with an empty run list when the pages
// are identical (callers normally skip identical pages before encoding).
inline OpKind encodeXor(const char* oldP, const char* newP, int len,
                        QByteArray& out, XorStats* stats = nullptr) {
    QVector<detail::RawRun> raw;
    uint32_t changed = 0;
    detail::collectRuns(oldP, newP, len, raw, changed);

    // Merge runs across small equal gaps.
    QVector<detail::RawRun> runs;
    runs.reserve(raw.size());
    for (const auto& r : raw) {
        if (!runs.isEmpty()) {
            auto& last = runs.last();
            if (r.start - (last.start + last.len) <= kRunMergeGap) {
                last.len = r.start + r.len - last.start;
                continue;
            }
        }
        runs.append(r);
    }

    // Size the sparse form before writing it.
    int sparse = varU64Size(uint64_t(runs.size()));
    int prevEnd = 0;
    for (const auto& r : runs) {
        sparse += varU64Size(uint64_t(r.start - prevEnd)) + varU64Size(uint64_t(r.len)) + r.len;
        prevEnd = r.start + r.len;
    }

    if (stats) { stats->changedBytes = changed; stats->runCount = uint32_t(runs.size()); }

    if (sparse >= len && len == int(kPageSize)) {
        const int base = out.size();
        out.resize(base + len);
        char* w = out.data() + base;
        for (int i = 0; i < len; ++i) w[i] = char(oldP[i] ^ newP[i]);
        return OpKind::XorDense;
    }

    out.reserve(out.size() + sparse);
    putVarU32(out, uint32_t(runs.size()));
    prevEnd = 0;
    for (const auto& r : runs) {
        putVarU32(out, uint32_t(r.start - prevEnd));
        putVarU32(out, uint32_t(r.len));
        const int base = out.size();
        out.resize(base + r.len);
        char* w = out.data() + base;
        for (int i = 0; i < r.len; ++i) w[i] = char(oldP[r.start + i] ^ newP[r.start + i]);
        prevEnd = r.start + r.len;
    }
    return OpKind::XorRuns;
}

// Walk an XorRuns payload, calling fn(offset, xorBytes, len) per run.
// Returns false (having called fn for any well-formed prefix) if the payload
// is malformed or a run would leave the page.
inline bool forEachXorRun(ByteReader& in, int pageLen,
                          const std::function<void(int, const char*, int)>& fn) {
    uint32_t count;
    if (!in.getVarU32(count)) return false;
    int pos = 0;
    for (uint32_t r = 0; r < count; ++r) {
        uint32_t gap, len;
        if (!in.getVarU32(gap) || !in.getVarU32(len)) return false;
        if (len == 0 || gap > uint32_t(pageLen) || len > uint32_t(pageLen)) return false;
        const int64_t start = int64_t(pos) + gap;
        if (start + len > pageLen) return false;
        const char* bytes;
        if (!in.getBytes(int(len), bytes)) return false;
        fn(int(start), bytes, int(len));
        pos = int(start + len);
    }
    return true;
}

// XOR a payload into `page` (in place). Applying the same payload twice
// restores the original. Returns false on a malformed payload; the page may
// then be partially modified and must be discarded by the caller.
inline bool applyXor(OpKind kind, ByteReader& in, char* page, int pageLen) {
    if (kind == OpKind::XorDense) {
        const char* bytes;
        if (!in.getBytes(pageLen, bytes)) return false;
        for (int i = 0; i < pageLen; ++i) page[i] ^= bytes[i];
        return true;
    }
    if (kind != OpKind::XorRuns) return false;
    return forEachXorRun(in, pageLen, [page](int off, const char* x, int n) {
        for (int i = 0; i < n; ++i) page[off + i] ^= x[i];
    });
}

// Skip over a payload without applying it.
inline bool skipXor(OpKind kind, ByteReader& in, int pageLen) {
    if (kind == OpKind::XorDense) return in.skip(pageLen);
    if (kind != OpKind::XorRuns) return false;
    return forEachXorRun(in, pageLen, [](int, const char*, int) {});
}

// The byte ranges a payload touches, relative to the page. XorRuns reports
// its (merged) runs; XorDense reports the bytes that are actually non-zero,
// so a dense op on a mostly-quiet page still reports precise changes.
inline bool xorTouchedRanges(OpKind kind, ByteReader& in, int pageLen,
                             QVector<QPair<int, int>>& ranges) {
    if (kind == OpKind::XorDense) {
        const char* bytes;
        if (!in.getBytes(pageLen, bytes)) return false;
        int start = -1;
        for (int i = 0; i <= pageLen; ++i) {
            const bool nz = (i < pageLen) && bytes[i];
            if (nz && start < 0) start = i;
            if (!nz && start >= 0) { ranges.append({start, i - start}); start = -1; }
        }
        return true;
    }
    return forEachXorRun(in, pageLen, [&ranges](int off, const char* x, int n) {
        // Trim the merge-gap zeros at the ends; interior zeros stay (they
        // are equal bytes between two changes — callers care about spans).
        int b = 0, e = n;
        while (b < e && !x[b]) ++b;
        while (e > b && !x[e - 1]) --e;
        if (e > b) ranges.append({off + b, e - b});
    });
}

// 64 buckets of 64 bytes: which parts of a page an op touches. Lets a range
// query skip ops (and whole chunks) that cannot overlap the bytes it wants.
inline uint64_t bucketMaskForRange(int off, int len) {
    if (len <= 0) return 0;
    const int first = off / 64;
    const int last = qMin(63, (off + len - 1) / 64);
    uint64_t m = 0;
    for (int b = first; b <= last; ++b) m |= (uint64_t(1) << b);
    return m;
}

} // namespace rcx::tl
