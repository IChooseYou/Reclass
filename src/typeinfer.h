#pragma once
#include <QVector>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core.h"

namespace rcx {

// ── Hints from value history (optional, improves accuracy) ──

struct InferHints {
    const uint8_t* minObserved = nullptr; // raw bytes, same len as data
    const uint8_t* maxObserved = nullptr;
    bool monotonic    = false; // value only increases or only decreases
    bool neverChanged = false; // identical across all samples
    int  sampleCount  = 0;    // 0 = no history
    int  ptrSize      = 8;
};

// ── Suggestion result ──

struct TypeSuggestion {
    QVector<NodeKind> kinds; // size==1: convert, size>1: uniform split
    int score    = 0;        // 0-100 feature ratio (passed / checked × 100)
    int strength = 0;        // 0=hidden, 1=weak, 2=moderate, 3=strong
};

// ── Public API ──

QVector<TypeSuggestion> inferTypes(
    const uint8_t* data, int len,
    const InferHints& hints = {},
    int maxResults = 3);

// Format top suggestion as short display string (e.g. "Float×2", "Int32", "UTF8")
inline QString formatHint(const TypeSuggestion& s) {
    if (s.kinds.isEmpty()) return {};
    const char* name = kindMeta(s.kinds[0])->typeName;
    QString base = (s.kinds.size() == 1)
        ? QString::fromLatin1(name)
        : QStringLiteral("%1\u00D7%2").arg(QString::fromLatin1(name)).arg(s.kinds.size());
    if (s.strength <= 2) base += QLatin1Char('?');  // moderate gets ?
    return base;
}

// ── Implementation (header-only) ──

namespace detail {

inline uint32_t loadU32(const uint8_t* p) {
    uint32_t v; std::memcpy(&v, p, 4); return v;
}
inline uint64_t loadU64(const uint8_t* p) {
    uint64_t v; std::memcpy(&v, p, 8); return v;
}
inline uint16_t loadU16(const uint8_t* p) {
    uint16_t v; std::memcpy(&v, p, 2); return v;
}
inline float loadF32(const uint8_t* p) {
    float v; std::memcpy(&v, p, 4); return v;
}
inline double loadF64(const uint8_t* p) {
    double v; std::memcpy(&v, p, 8); return v;
}

inline bool allZero(const uint8_t* p, int n) {
    for (int i = 0; i < n; ++i) if (p[i]) return false;
    return true;
}

inline int popcount32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcount(v);
#else
    int c = 0; while (v) { v &= v - 1; ++c; } return c;
#endif
}

inline bool isPrintable(uint8_t c) {
    return c >= 0x20 && c <= 0x7E;
}

// ── Float feature checker ──
// Returns features passed out of features checked (as pair)

struct FeatureResult { int passed; int checked; };

inline bool isGoodFloat(uint32_t bits) {
    uint32_t exp = (bits >> 23) & 0xFF;
    if (exp == 0xFF) return false;  // inf/nan
    if (exp == 0 && (bits & 0x7FFFFF)) return false; // denormal
    float f; std::memcpy(&f, &bits, 4);
    double af = std::fabs((double)f);
    return f == 0.0f || (af >= 1e-6 && af <= 1e7);
}

inline FeatureResult countFloatFeatures(uint32_t cur,
                                        const uint8_t* minP, const uint8_t* maxP,
                                        const InferHints& h) {
    int passed = 0, checked = 4;
    float f; std::memcpy(&f, &cur, 4);

    // Feature 1: finite
    passed += std::isfinite((double)f) ? 1 : 0;
    // Feature 2: non-denormal (exponent > 0 or value is ±0)
    uint32_t exp = (cur >> 23) & 0xFF;
    passed += (exp > 0 || (cur & 0x7FFFFFFF) == 0) ? 1 : 0;
    // Feature 3: reasonable range
    double af = std::fabs((double)f);
    passed += (f == 0.0f || (af >= 1e-6 && af <= 1e7)) ? 1 : 0;
    // Feature 4: has fractional part (not just a reinterpreted integer)
    float ip; double frac = std::fabs((double)std::modf(f, &ip));
    passed += (frac > 0.0001) ? 1 : 0;

    if (h.sampleCount > 0 && minP && maxP) {
        checked += 4;
        uint32_t minBits = loadU32(minP), maxBits = loadU32(maxP);
        // Feature 5-6: min/max are also valid floats
        passed += isGoodFloat(minBits) ? 1 : 0;
        passed += isGoodFloat(maxBits) ? 1 : 0;
        // Feature 7: field changes
        passed += (minBits != maxBits) ? 1 : 0;
        // Feature 8: range is game-plausible
        float fmin, fmax;
        std::memcpy(&fmin, &minBits, 4);
        std::memcpy(&fmax, &maxBits, 4);
        double range = std::fabs((double)fmax - (double)fmin);
        passed += (range < 1e6) ? 1 : 0;
    }
    return {passed, checked};
}

// ── Integer feature checker ──

inline FeatureResult countIntFeatures(uint32_t val,
                                      const uint8_t* minP, const uint8_t* maxP,
                                      const InferHints& h) {
    int passed = 0, checked = 3;
    int32_t sv = (int32_t)val;

    // Feature 1: non-zero
    passed += (val != 0) ? 1 : 0;
    // Feature 2: small absolute value
    passed += (val <= 1000000u || (uint32_t)(sv + 1000000) <= 2000000u) ? 1 : 0;
    // Feature 3: fits int16 range
    passed += (sv >= -32768 && sv <= 32767) ? 1 : 0;

    if (h.sampleCount > 0 && minP && maxP) {
        checked += 3;
        uint32_t minV = loadU32(minP), maxV = loadU32(maxP);
        // Feature 4: min/max in reasonable range
        passed += (minV <= 1000000u && maxV <= 1000000u) ? 1 : 0;
        // Feature 5: monotonic (counter/timer)
        passed += h.monotonic ? 1 : 0;
        // Feature 6: field varies
        passed += (minV != maxV) ? 1 : 0;
    }
    return {passed, checked};
}

// ── Flags feature checker ──

inline FeatureResult countFlagFeatures(uint32_t val,
                                       const uint8_t* minP, const uint8_t* maxP,
                                       const InferHints& h) {
    int passed = 0, checked = 2;
    int pc = popcount32(val);

    // Feature 1: sparse bits (1-3 set)
    passed += (pc >= 1 && pc <= 3) ? 1 : 0;
    // Feature 2: not a small sequential integer (flags are usually not 1,2,3...)
    passed += (val > 256 || (val & (val - 1)) != 0) ? 1 : 0;

    if (h.sampleCount > 0 && minP && maxP) {
        checked += 3;
        uint32_t minV = loadU32(minP), maxV = loadU32(maxP);
        // Feature 3: XOR of min/max has low popcount (specific bits toggle)
        passed += (popcount32(minV ^ maxV) <= 4) ? 1 : 0;
        // Feature 4: field varies
        passed += (minV != maxV) ? 1 : 0;
        // Feature 5: max is superset of min bits
        passed += ((minV & maxV) == minV) ? 1 : 0;
    }
    return {passed, checked};
}

// ── Pointer feature checker ──

inline FeatureResult countPtrFeatures64(uint64_t val) {
    int passed = 0, checked = 5;
    // Feature 1: non-zero and not common sentinel values
    passed += (val != 0 && val != 0xFFFFFFFFFFFFFFFFULL
               && val != 0x00000000FFFFFFFFULL) ? 1 : 0;
    // Feature 2: canonical 48-bit address (sign-extended from bit 47)
    passed += (val <= 0x00007FFFFFFFFFFFULL
               || val >= 0xFFFF800000000000ULL) ? 1 : 0;
    // Feature 3: aligned to 8 (heap/vtable allocations)
    passed += ((val & 7) == 0) ? 1 : 0;
    // Feature 4: above null guard pages (real addresses >= 64KB)
    passed += (val >= 0x10000) ? 1 : 0;
    // Feature 5: has upper 32 bits (real 64-bit address, not a small constant)
    passed += ((val >> 32) != 0) ? 1 : 0;
    return {passed, checked};
}

inline FeatureResult countPtrFeatures32(uint32_t val) {
    int passed = 0, checked = 3;
    // Feature 1: non-zero and not sentinel
    passed += (val != 0 && val != 0xFFFFFFFF) ? 1 : 0;
    // Feature 2: aligned to 4
    passed += ((val & 3) == 0) ? 1 : 0;
    // Feature 3: above null guard pages (>= 64KB)
    passed += (val >= 0x10000) ? 1 : 0;
    return {passed, checked};
}

// ── String feature checker ──

inline FeatureResult countStringFeatures(const uint8_t* data, int len) {
    if (len < 2) return {0, 4};
    int printable = 0, letters = 0, consecutive = 0, maxConsec = 0;
    for (int i = 0; i < len; ++i) {
        if (isPrintable(data[i])) {
            printable++;
            consecutive++;
            maxConsec = std::max(maxConsec, consecutive);
            if ((data[i] >= 'A' && data[i] <= 'Z') || (data[i] >= 'a' && data[i] <= 'z'))
                letters++;
        } else {
            consecutive = 0;
        }
    }
    double ratio = (double)printable / len;
    int passed = 0, checked = 4;
    passed += (maxConsec >= 4) ? 1 : 0;
    passed += (ratio > 0.75) ? 1 : 0;
    passed += (letters >= 1) ? 1 : 0;
    passed += (ratio > 0.90) ? 1 : 0;
    return {passed, checked};
}

// ── Int16 feature checker ──

inline FeatureResult countInt16Features(uint16_t val,
                                        const uint8_t* minP, const uint8_t* maxP,
                                        const InferHints& h) {
    int passed = 0, checked = 2;
    int16_t sv = (int16_t)val;
    passed += (val != 0) ? 1 : 0;
    passed += (sv >= -4096 && sv <= 4096) ? 1 : 0;

    if (h.sampleCount > 0 && minP && maxP) {
        checked += 2;
        uint16_t minV = loadU16(minP), maxV = loadU16(maxP);
        passed += (minV <= 4096 && maxV <= 4096) ? 1 : 0;
        passed += (minV != maxV) ? 1 : 0;
    }
    return {passed, checked};
}

// ── Score from feature result ──

inline int featureScore(FeatureResult r) {
    if (r.checked == 0) return 0;
    return (r.passed * 100) / r.checked;
}

inline int strengthFromScore(int score) {
    if (score >= 75) return 3;
    if (score >= 50) return 2;
    if (score >= 25) return 1;
    return 0;
}

// ── Candidate accumulator ──

struct Candidate {
    QVector<NodeKind> kinds;
    int score;
};

inline void addCandidate(QVector<Candidate>& out, NodeKind k, int score) {
    if (score >= 25) out.append({{k}, score});
}

inline void addSplitCandidate(QVector<Candidate>& out, NodeKind k, int count, int score) {
    if (score >= 25) {
        QVector<NodeKind> kinds(count, k);
        out.append({std::move(kinds), score});
    }
}

// ── Try whole-width interpretations ──

inline void tryWhole8(const uint8_t* data, const InferHints& h, QVector<Candidate>& out) {
    uint64_t u64 = loadU64(data);

    // Pointer64
    if (h.ptrSize == 8)
        addCandidate(out, NodeKind::Pointer64, featureScore(countPtrFeatures64(u64)));

    // Double
    {
        double d; std::memcpy(&d, data, 8);
        uint64_t exp = (u64 >> 52) & 0x7FF;
        int passed = 0, checked = 3;
        passed += std::isfinite(d) ? 1 : 0;
        passed += (exp > 0 || (u64 & 0x7FFFFFFFFFFFFFFFull) == 0) ? 1 : 0;
        double ad = std::fabs(d);
        passed += (d == 0.0 || (ad >= 1e-6 && ad <= 1e12)) ? 1 : 0;
        addCandidate(out, NodeKind::Double, featureScore({passed, checked}));
    }

    // UTF8
    addCandidate(out, NodeKind::UTF8, featureScore(countStringFeatures(data, 8)));

    // UInt64 / Int64
    {
        int passed = 0, checked = 4;
        // Feature 1: fits in 32 bits (small constant, not an address)
        passed += (u64 <= 0xFFFFFFFFull) ? 1 : 0;
        // Feature 2: upper 32 bits are zero (confirms it's a small value, not a pointer)
        passed += ((u64 >> 32) == 0) ? 1 : 0;
        // Feature 3: non-zero
        passed += (u64 != 0) ? 1 : 0;
        // Feature 4: monotonic or very small (< 0x10000)
        passed += (h.monotonic || u64 < 0x10000) ? 1 : 0;
        addCandidate(out, NodeKind::UInt64, featureScore({passed, checked}));
    }
}

inline void tryWhole4(const uint8_t* data, const uint8_t* minP, const uint8_t* maxP,
                      const InferHints& h, QVector<Candidate>& out) {
    uint32_t u32 = loadU32(data);

    // Float
    addCandidate(out, NodeKind::Float, featureScore(countFloatFeatures(u32, minP, maxP, h)));

    // Int32
    addCandidate(out, NodeKind::Int32, featureScore(countIntFeatures(u32, minP, maxP, h)));

    // UInt32
    addCandidate(out, NodeKind::UInt32, featureScore(countIntFeatures(u32, minP, maxP, h)));

    // Flags (only if sparse bits)
    addCandidate(out, NodeKind::UInt32, featureScore(countFlagFeatures(u32, minP, maxP, h)));

    // Pointer32
    if (h.ptrSize == 4)
        addCandidate(out, NodeKind::Pointer32, featureScore(countPtrFeatures32(u32)));
}

inline void tryWhole2(const uint8_t* data, const uint8_t* minP, const uint8_t* maxP,
                      const InferHints& h, QVector<Candidate>& out) {
    uint16_t u16 = loadU16(data);
    int scoreI = featureScore(countInt16Features(u16, minP, maxP, h));
    addCandidate(out, NodeKind::Int16, scoreI);
    addCandidate(out, NodeKind::UInt16, scoreI);
}

inline void tryWhole1(const uint8_t* data, QVector<Candidate>& out) {
    uint8_t v = data[0];
    int score = (v == 0 || v == 1) ? 50 : 25;
    addCandidate(out, NodeKind::UInt8, score);
    if (v <= 1) addCandidate(out, NodeKind::Bool, 60);
}

// ── Try uniform splits ──

inline void trySplitUniform(const uint8_t* data, int len,
                            const InferHints& h,
                            QVector<Candidate>& out) {

    // 8 → 2×4
    if (len == 8) {
        const uint8_t* minA = h.minObserved;
        const uint8_t* minB = h.minObserved ? h.minObserved + 4 : nullptr;
        const uint8_t* maxA = h.maxObserved;
        const uint8_t* maxB = h.maxObserved ? h.maxObserved + 4 : nullptr;
        bool zA = allZero(data, 4), zB = allZero(data + 4, 4);

        // Float×2: both halves must be good floats and at least one non-zero
        if (!zA || !zB) {
            uint32_t bitsA = loadU32(data), bitsB = loadU32(data + 4);
            bool fA = zA || isGoodFloat(bitsA);
            bool fB = zB || isGoodFloat(bitsB);
            if (fA && fB) {
                auto rA = zA ? FeatureResult{2, 4} : countFloatFeatures(bitsA, minA, maxA, h);
                auto rB = zB ? FeatureResult{2, 4} : countFloatFeatures(bitsB, minB, maxB, h);
                int score = std::min(featureScore(rA), featureScore(rB));
                addSplitCandidate(out, NodeKind::Float, 2, score);
            }
        }

        // Int32×2: both halves, at least one non-zero
        if (!zA || !zB) {
            auto rA = zA ? FeatureResult{1, 3} : countIntFeatures(loadU32(data), minA, maxA, h);
            auto rB = zB ? FeatureResult{1, 3} : countIntFeatures(loadU32(data + 4), minB, maxB, h);
            int score = std::min(featureScore(rA), featureScore(rB));
            addSplitCandidate(out, NodeKind::Int32, 2, score);
        }

        // UInt32×2
        if (!zA || !zB) {
            auto rA = zA ? FeatureResult{1, 3} : countIntFeatures(loadU32(data), minA, maxA, h);
            auto rB = zB ? FeatureResult{1, 3} : countIntFeatures(loadU32(data + 4), minB, maxB, h);
            int score = std::min(featureScore(rA), featureScore(rB));
            addSplitCandidate(out, NodeKind::UInt32, 2, score);
        }
    }

    // 8 → 4×2 or 4 → 2×2
    int halfLen = len / 2;
    if (halfLen == 2) {
        int minScore = 100;
        int count = len / 2;
        bool anyNonZero = false;
        for (int i = 0; i < count; ++i) {
            const uint8_t* part = data + i * 2;
            if (!allZero(part, 2)) anyNonZero = true;
            const uint8_t* mp = h.minObserved ? h.minObserved + i * 2 : nullptr;
            const uint8_t* xp = h.maxObserved ? h.maxObserved + i * 2 : nullptr;
            int s = featureScore(countInt16Features(loadU16(part), mp, xp, h));
            minScore = std::min(minScore, s);
        }
        if (anyNonZero) {
            addSplitCandidate(out, NodeKind::Int16, count, minScore);
            addSplitCandidate(out, NodeKind::UInt16, count, minScore);
        }
    }
}

// ── Prune and rank ──

inline QVector<TypeSuggestion> pruneAndRank(QVector<Candidate>& cands, int maxResults) {
    // Sort descending by score
    std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        return a.score > b.score;
    });

    // Dedup: keep highest-scoring per unique kinds vector
    QVector<Candidate> deduped;
    for (const auto& c : cands) {
        bool dup = false;
        for (const auto& d : deduped) {
            if (d.kinds == c.kinds) { dup = true; break; }
        }
        if (!dup) deduped.append(c);
    }

    // Dominance: if top >= 1.5× second, keep only top
    if (deduped.size() >= 2 && deduped[0].score >= deduped[1].score * 3 / 2)
        deduped.resize(1);
    else if (deduped.size() > maxResults)
        deduped.resize(maxResults);

    QVector<TypeSuggestion> result;
    result.reserve(deduped.size());
    for (const auto& c : deduped) {
        int str = strengthFromScore(c.score);
        if (str > 0)
            result.append({c.kinds, c.score, str});
    }
    return result;
}

} // namespace detail

// ── Entry point ──

inline QVector<TypeSuggestion> inferTypes(
    const uint8_t* data, int len,
    const InferHints& hints,
    int maxResults)
{
    using namespace detail;

    if (!data || len <= 0) return {};
    if (allZero(data, len)) return {};  // NULL → skip entirely

    QVector<Candidate> cands;
    cands.reserve(12);

    // Whole-width candidates
    if (len >= 8) tryWhole8(data, hints, cands);
    if (len == 4) tryWhole4(data, hints.minObserved, hints.maxObserved, hints, cands);
    if (len == 2) tryWhole2(data, hints.minObserved, hints.maxObserved, hints, cands);
    if (len == 1) tryWhole1(data, cands);

    // Uniform splits (compete directly with whole-width candidates)
    if (len >= 4)
        trySplitUniform(data, len, hints, cands);

    return pruneAndRank(cands, maxResults);
}

} // namespace rcx
