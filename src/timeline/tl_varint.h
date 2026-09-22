#pragma once

// LEB128 varints and zigzag for the timeline record format.
//
// Records are dominated by small numbers — millisecond deltas, run gaps,
// run lengths, page-id deltas — so a 4-byte field change encodes in about
// ten bytes. Every reader is bounds-checked: a truncated or corrupt block
// must fail the read, never walk off the end of a buffer.

#include <QByteArray>
#include <cstdint>

namespace rcx::tl {

inline void putVarU64(QByteArray& out, uint64_t v) {
    char buf[10];
    int n = 0;
    do {
        uint8_t b = uint8_t(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        buf[n++] = char(b);
    } while (v);
    out.append(buf, n);
}

inline void putVarU32(QByteArray& out, uint32_t v) { putVarU64(out, v); }

inline uint64_t zigzagEncode(int64_t v) {
    return (uint64_t(v) << 1) ^ uint64_t(v >> 63);
}
inline int64_t zigzagDecode(uint64_t v) {
    return int64_t(v >> 1) ^ -int64_t(v & 1);
}

inline void putVarS64(QByteArray& out, int64_t v) { putVarU64(out, zigzagEncode(v)); }

// Encoded length in bytes, without encoding.
inline int varU64Size(uint64_t v) {
    int n = 1;
    while (v >= 0x80) { v >>= 7; ++n; }
    return n;
}

// A cursor over a const byte range. Every get* returns false — and leaves
// the cursor unusable (`ok()` false) — on truncation or overflow.
class ByteReader {
public:
    ByteReader() = default;
    ByteReader(const char* data, int size)
        : m_p(reinterpret_cast<const uint8_t*>(data))
        , m_end(reinterpret_cast<const uint8_t*>(data) + (size > 0 ? size : 0)) {}

    bool ok() const        { return m_ok; }
    bool atEnd() const     { return m_p >= m_end; }
    int  remaining() const { return m_ok ? int(m_end - m_p) : 0; }
    const char* pos() const { return reinterpret_cast<const char*>(m_p); }

    bool getVarU64(uint64_t& v) {
        v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (m_p >= m_end) return fail();
            const uint8_t b = *m_p++;
            // The 10th byte may only carry the top bit of a 64-bit value.
            if (shift == 63 && (b & 0x7E)) return fail();
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80)) return m_ok;
        }
        return fail();
    }
    bool getVarU32(uint32_t& v) {
        uint64_t w;
        if (!getVarU64(w) || w > 0xFFFFFFFFull) return fail();
        v = uint32_t(w);
        return true;
    }
    bool getVarS64(int64_t& v) {
        uint64_t w;
        if (!getVarU64(w)) return false;
        v = zigzagDecode(w);
        return true;
    }
    bool getU8(uint8_t& v) {
        if (m_p >= m_end) return fail();
        v = *m_p++;
        return m_ok;
    }
    // Hands back a pointer into the buffer rather than copying.
    bool getBytes(int n, const char*& out) {
        if (n < 0 || m_end - m_p < n) return fail();
        out = reinterpret_cast<const char*>(m_p);
        m_p += n;
        return m_ok;
    }
    bool skip(int n) {
        const char* dummy;
        return getBytes(n, dummy);
    }

private:
    bool fail() { m_ok = false; m_p = m_end; return false; }
    const uint8_t* m_p = nullptr;
    const uint8_t* m_end = nullptr;
    bool m_ok = true;
};

} // namespace rcx::tl
