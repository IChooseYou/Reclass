#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>

// Structure-aware predicate for locating a 4x4 affine view/camera matrix in raw
// memory. Scoring a 64-byte window (16 little-endian floats) collapses a
// multi-GB space to a handful of candidates: random byte windows almost never
// hold 16 consecutive plausible floats, and a true view/world matrix has a
// recognizable homogeneous signature.
//
// Storage conventions: for BOTH DirectX row-major and OpenGL column-major
// affine matrices, the homogeneous (0,0,0,1) vector lands at float indices
// {3,7,11,15} and the translation at {12,13,14}. We test that primary
// signature, and also the {12,13,14,15}=(0,0,0,1) variant (transposed/
// pre-multiply layouts), taking whichever fits.
//
// The float-validity gate mirrors typeinfer.h::isGoodFloat (reject NaN/inf/
// denormal) but allows a caller-tunable magnitude ceiling so large world-space
// translations aren't rejected.

namespace rcx {

struct MatrixScanParams {
    bool  requireAffine      = true;   // demand the (0,0,0,1) signature
    bool  requireOrthonormal = false;  // demand a unit, mutually-orthogonal 3x3
    float affineEps = 1e-3f;           // tolerance for the 0 / 1 signature tests
    float orthoEps  = 1e-2f;           // tolerance for unit-length / orthogonality
    float magMax    = 1e7f;            // reject any |component| above this (NaN/inf always rejected)
    int   minScore  = 60;              // drop candidates below this (0..100)
};

struct MatrixScanResult {
    int  score        = 0;       // 0..100
    bool lastRowIsHomog = false; // (0,0,0,1) at floats {12,13,14,15} vs {3,7,11,15}
    bool affine       = false;   // full (0,0,0,1) signature present
    bool orthonormal  = false;   // 3x3 rotation block is orthonormal
};

namespace detail {

// finite, non-denormal, and |value| <= magMax.
inline bool matFloatOK(uint32_t bits, float magMax) {
    uint32_t exp = (bits >> 23) & 0xFFu;
    if (exp == 0xFFu) return false;                 // inf / nan
    if (exp == 0 && (bits & 0x7FFFFFu)) return false; // denormal
    float f; std::memcpy(&f, &bits, 4);
    return std::fabs(f) <= magMax;
}

inline float dot3(const float* a, const float* b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

} // namespace detail

// Score a 64-byte little-endian window as a 4x4 affine view/camera matrix.
// On the float-validity gate failure (any NaN/inf/denormal/over-magnitude
// component) returns {score:0}.
inline MatrixScanResult scoreMatrixWindow(const uint8_t* win, const MatrixScanParams& p) {
    MatrixScanResult out;
    float m[16];
    for (int i = 0; i < 16; ++i) {
        uint32_t bits;
        std::memcpy(&bits, win + i * 4, 4);
        if (!detail::matFloatOK(bits, p.magMax)) return out;  // hard gate
        std::memcpy(&m[i], &bits, 4);
    }

    auto near0 = [&](float x) { return std::fabs(x)        <= p.affineEps; };
    auto near1 = [&](float x) { return std::fabs(x - 1.0f) <= p.affineEps; };

    // Homogeneous signature, two layouts.
    int colHits = (near0(m[3]) ? 1 : 0) + (near0(m[7]) ? 1 : 0)
                + (near0(m[11]) ? 1 : 0) + (near1(m[15]) ? 1 : 0);   // {3,7,11,15} — the common case
    int rowHits = (near0(m[12]) ? 1 : 0) + (near0(m[13]) ? 1 : 0)
                + (near0(m[14]) ? 1 : 0) + (near1(m[15]) ? 1 : 0);   // {12,13,14,15} — transposed

    bool lastRowHomog = rowHits > colHits;
    int  affineHits   = lastRowHomog ? rowHits : colHits;
    out.lastRowIsHomog = lastRowHomog;
    out.affine = (affineHits == 4);

    // 3x3 rotation block. When (0,0,0,1) sits at {3,7,11,15} the basis vectors
    // are the storage columns (0,4,8)/(1,5,9)/(2,6,10); when it sits at the last
    // contiguous row they're the storage rows (0,1,2)/(4,5,6)/(8,9,10).
    float r0[3], r1[3], r2[3];
    if (lastRowHomog) {
        r0[0]=m[0]; r0[1]=m[1]; r0[2]=m[2];
        r1[0]=m[4]; r1[1]=m[5]; r1[2]=m[6];
        r2[0]=m[8]; r2[1]=m[9]; r2[2]=m[10];
    } else {
        r0[0]=m[0]; r0[1]=m[4]; r0[2]=m[8];
        r1[0]=m[1]; r1[1]=m[5]; r1[2]=m[9];
        r2[0]=m[2]; r2[1]=m[6]; r2[2]=m[10];
    }
    auto unit = [&](const float* v) {
        return std::fabs(std::sqrt(detail::dot3(v, v)) - 1.0f) <= p.orthoEps;
    };
    int orthoHits = 0;
    if (unit(r0)) ++orthoHits;
    if (unit(r1)) ++orthoHits;
    if (unit(r2)) ++orthoHits;
    if (std::fabs(detail::dot3(r0, r1)) <= p.orthoEps) ++orthoHits;
    if (std::fabs(detail::dot3(r0, r2)) <= p.orthoEps) ++orthoHits;
    if (std::fabs(detail::dot3(r1, r2)) <= p.orthoEps) ++orthoHits;
    out.orthonormal = (orthoHits == 6);

    // Affine signature is the discriminator (4 * 15 = 60), orthonormality is the
    // tiebreaker (6 checks -> up to 40). A perfect view matrix scores 100.
    int score = affineHits * 15 + (orthoHits * 40) / 6;
    if (score > 100) score = 100;
    if (p.requireAffine && !out.affine)            score = (score < 30) ? score : 30;
    if (p.requireOrthonormal && !out.orthonormal)  score = (score < 40) ? score : 40;
    out.score = score;
    return out;
}

} // namespace rcx
