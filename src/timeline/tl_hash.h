#pragma once

// Page hashing for the timeline's deduplicated image store, plus the block
// checksum.
//
// Two independent 64-bit lanes give a 128-bit key. The engine hashes a page
// only when it stores an image (first sight, becoming readable, or after its
// delta cap) — never per tick — so this is nowhere near a hot path. Matches
// are still VERIFIED byte-for-byte before an image is shared: a hash is an
// index, not a proof, and a false share would silently corrupt history.

#include <QtGlobal>
#include <QHashFunctions>
#include <cstdint>
#include <cstring>

namespace rcx::tl {

struct Hash128 {
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool operator==(const Hash128& o) const { return lo == o.lo && hi == o.hi; }
    bool operator!=(const Hash128& o) const { return !(*this == o); }
};

inline size_t qHash(const Hash128& h, size_t seed = 0) noexcept {
    return ::qHash(h.lo ^ (h.hi * 0x9E3779B97F4A7C15ull), seed);
}

namespace detail {

inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

// MurmurHash3's finaliser: full avalanche of a 64-bit state.
inline uint64_t fmix64(uint64_t k) {
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull;
    k ^= k >> 33;
    return k;
}

inline uint64_t loadU64(const char* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

} // namespace detail

// MurmurHash3_x64_128's block structure: two lanes with cross-mixing, 8-byte
// words. Deterministic across runs (fixed seed) so tests can pin collisions
// through the injectable hash function in the blob table instead.
inline Hash128 hashBytes(const char* data, size_t len, uint64_t seed = 0x5243585F544C31ull) {
    using namespace detail;
    const uint64_t c1 = 0x87C37B91114253D5ull;
    const uint64_t c2 = 0x4CF5AD432745937Full;
    uint64_t h1 = seed, h2 = ~seed;
    const size_t blocks = len / 16;
    for (size_t i = 0; i < blocks; ++i) {
        uint64_t k1 = loadU64(data + i * 16);
        uint64_t k2 = loadU64(data + i * 16 + 8);
        k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
        h1 = rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52DCE729;
        k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
        h2 = rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495AB5;
    }
    uint64_t k1 = 0, k2 = 0;
    const char* tail = data + blocks * 16;
    const size_t rem = len & 15;
    for (size_t i = rem; i > 8; --i) k2 ^= uint64_t(uint8_t(tail[i - 1])) << ((i - 9) * 8);
    if (rem > 8) { k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2; }
    for (size_t i = qMin<size_t>(rem, 8); i > 0; --i) k1 ^= uint64_t(uint8_t(tail[i - 1])) << ((i - 1) * 8);
    if (rem > 0) { k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1; }
    h1 ^= uint64_t(len); h2 ^= uint64_t(len);
    h1 += h2; h2 += h1;
    h1 = fmix64(h1); h2 = fmix64(h2);
    h1 += h2; h2 += h1;
    return {h1, h2};
}

// Block checksum: verified BEFORE a block is decompressed, so a flipped byte
// marks its data Lost instead of feeding garbage to the decoder.
inline uint32_t checksum32(const char* data, size_t len) {
    const Hash128 h = hashBytes(data, len, 0x434B53554D5F7631ull);
    return uint32_t(h.lo ^ (h.lo >> 32));
}

inline bool isZeroBytes(const char* data, size_t len) {
    size_t i = 0;
    for (; i + 8 <= len; i += 8)
        if (detail::loadU64(data + i)) return false;
    for (; i < len; ++i)
        if (data[i]) return false;
    return true;
}

} // namespace rcx::tl
