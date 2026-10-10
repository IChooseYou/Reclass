#include "type_resolution.h"
#include "typeinfer.h"
#include "matrixscan.h"
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>

namespace rcx::resolution {

QJsonObject Candidate::toJson() const {
    return {{"candidateId", id}, {"offset", offset}, {"size", size},
            {"kind", kindToString(kind)}, {"rankScore", score},
            {"verified", verified}, {"autoApplicable", autoApplicable},
            {"evidenceLevel", evidenceLevel}, {"declaredName", declaredName},
            {"validObservations", observations}, {"distinctValues", distinctValues},
            {"evidence", QJsonArray::fromStringList(evidence)},
            {"caveats", QJsonArray::fromStringList(caveats)}, {"pointerTarget", target},
            {"recentValues", values}};
}

static bool replaceable(const Node& n) {
    // A name describes the byte range; it does not make its raw representation
    // known. prepare() carries the name to the replacement at the same offset.
    return isHexNode(n.kind) && !n.bigEndian && n.comment.isEmpty()
        && n.enumMembers.isEmpty() && n.bitfieldMembers.isEmpty()
        && n.refId == 0 && n.structTypeName.isEmpty()
        && n.offset >= 0 && int64_t(n.offset) + n.byteSize() <= INT_MAX;
}

QByteArray eligibleBytes(const NodeTree& tree, uint64_t parentId, int offset, int length) {
    QByteArray mask(length, '\0'), blocked(length, '\0');
    for (int idx : tree.childrenOf(parentId)) {
        const Node& n = tree.nodes[idx];
        const int64_t end = int64_t(n.offset) + n.totalByteSize(tree);
        const int lo = int(qMax<int64_t>(offset, n.offset));
        const int hi = int(qMin<int64_t>(int64_t(offset) + length, end));
        for (int p = lo; p < hi; ++p) {
            if (replaceable(n) && tree.childrenOf(n.id).isEmpty()) {
                // Overlapping hex leaves are ambiguous too.
                if (mask[p - offset]) blocked[p - offset] = 1;
                mask[p - offset] = 1;
            } else blocked[p - offset] = 1;
        }
    }
    for (int i = 0; i < length; ++i) if (blocked[i]) mask[i] = 0;
    return mask;
}

static bool covered(const QByteArray& mask, int off, int size) {
    if (off < 0 || size <= 0 || int64_t(off) + size > mask.size()) return false;
    for (int i = 0; i < size; ++i) if (!mask[off + i]) return false;
    return true;
}

QByteArray knownFloatBytes(const NodeTree& tree, uint64_t parentId, int offset, int length) {
    QByteArray mask(length, '\0'), occupied(length, '\0');
    for (int idx : tree.childrenOf(parentId)) {
        const Node& n = tree.nodes[idx];
        const bool floating = !n.bigEndian && n.offset % 4 == 0 && tree.childrenOf(n.id).isEmpty()
            && (n.kind == NodeKind::Float || isVectorKind(n.kind) || isMatrixKind(n.kind));
        const int lo = int(qMax<int64_t>(offset, n.offset));
        const int hi = int(qMin<int64_t>(int64_t(offset) + length,
                                        int64_t(n.offset) + n.totalByteSize(tree)));
        for (int p = lo; p < hi; ++p) {
            mask[p - offset] = !occupied[p - offset] && floating ? 1 : 0;
            occupied[p - offset] = 1;
        }
    }
    return mask;
}

static void promote(Candidate& c, int score, const QString& reason) {
    c.autoApplicable = true;
    c.evidenceLevel = QStringLiteral("high");
    c.score = qMax(c.score, score);
    c.evidence << reason;
}

// A homogeneous signature alone also matches mostly-zero buffers. Require a
// non-degenerate, approximately orthogonal basis, allowing non-uniform scale.
static bool affineMatrix(const uint8_t* bytes) {
    MatrixScanParams params;
    params.affineEps = 1e-6f;
    if (!scoreMatrixWindow(bytes, params).affine) return false;
    double basis[3][3], lengths[3] = {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            basis[i][j] = detail::loadF32(bytes + (i * 4 + j) * 4);
            lengths[i] += basis[i][j] * basis[i][j];
        }
        if (lengths[i] < 1e-8) return false;
    }
    for (int i = 0; i < 3; ++i) for (int j = i + 1; j < 3; ++j) {
        double dot = 0;
        for (int k = 0; k < 3; ++k) dot += basis[i][k] * basis[j][k];
        if (std::fabs(dot) > .05 * std::sqrt(lengths[i] * lengths[j])) return false;
    }
    return true;
}

static QVector<Candidate> matrixCandidates(const QVector<Sample>& samples, const QByteArray& eligible,
                                           const QByteArray& knownFloats, int regionOffset,
                                           QByteArray& matrixFloats) {
    QVector<Candidate> result;
    if (samples.isEmpty()) return result;
    QByteArray context = eligible;
    for (int i = 0; i < context.size(); ++i)
        if (i < knownFloats.size() && knownFloats[i]) context[i] = 1;
    for (int off = 0; off + 64 <= context.size(); ++off) {
        if ((regionOffset + off) % 4 || !covered(context, off, 64)) continue;
        Candidate c;
        c.offset = regionOffset + off; c.size = 64; c.kind = NodeKind::Mat4x4;
        c.id = QStringLiteral("%1:64:Mat4x4").arg(c.offset);
        QSet<QByteArray> distinct;
        bool consistent = true;
        for (const auto& sample : samples) {
            if (!covered(sample.valid, off, 64) || off + 64 > sample.bytes.size()
                || !affineMatrix(reinterpret_cast<const uint8_t*>(sample.bytes.constData() + off))) {
                consistent = false; break;
            }
            const auto bytes = sample.bytes.mid(off, 64);
            distinct.insert(bytes); ++c.observations;
            if (c.values.size() == 3) c.values.removeFirst();
            c.values.append(QJsonObject{{"timestampMs", double(sample.timestampMs)},
                {"hex", QString::fromLatin1(bytes.toHex(' '))}});
        }
        if (!consistent) continue;
        c.distinctValues = distinct.size();
        promote(c, 94, "All observations form an affine 4x4 float matrix with a non-degenerate orthogonal basis");
        c.caveats << "Rank is heuristic, not a correctness probability"
                  << "Matrix storage does not establish its semantic role or source-level type";
        result.append(c);
    }
    // Do not choose a matrix boundary when overlapping windows fit equally well.
    QVector<Candidate> unambiguous;
    for (const auto& c : result) {
        bool overlaps = false;
        for (const auto& other : result)
            if (other.offset != c.offset && other.offset < c.offset + c.size
                && c.offset < other.offset + other.size) { overlaps = true; break; }
        if (overlaps) continue;
        const int local = c.offset - regionOffset;
        for (int i = local; i < local + c.size; ++i) matrixFloats[i] = 1;
        if (covered(eligible, local, c.size)) unambiguous.append(c);
    }
    return unambiguous;
}

static bool simpleFraction(float value) {
    if (value == std::trunc(value) || std::fabs(value) > 10000) return false;
    // Exact round-trip through a short decimal is useful supporting evidence;
    // it is never enough to promote an isolated constant.
    for (double scale : {10.0, 100.0, 1000.0})
        if (float(std::round(double(value) * scale) / scale) == value) return true;
    return false;
}

static bool contextualFloat(const Candidate& c, const QVector<Sample>& samples,
                            int regionOffset, bool& simple) {
    float minimum = std::numeric_limits<float>::infinity(), maximum = -minimum;
    uint32_t previous = 0;
    bool first = true, changed = false, meaningfulBits = false, fractional = false;
    simple = true;
    for (const auto& sample : samples) {
        const auto* p = reinterpret_cast<const uint8_t*>(sample.bytes.constData() + c.offset - regionOffset);
        const float value = detail::loadF32(p);
        const uint32_t bits = detail::loadU32(p);
        // All-zero histories need structural evidence. A previously nonzero
        // float reaching zero during verification is still compatible.
        if (value == 0 && c.distinctValues == 1) return false;
        minimum = qMin(minimum, value); maximum = qMax(maximum, value);
        fractional |= value != std::trunc(value);
        simple &= value == std::trunc(value) || simpleFraction(value);
        if (!first && bits != previous) {
            changed = true;
            meaningfulBits |= (bits > previous ? bits - previous : previous - bits) > 4;
        }
        first = false; previous = bits;
    }
    simple &= fractional;
    if (!changed) return true;
    const double magnitude = qMax(std::fabs(double(minimum)), std::fabs(double(maximum)));
    return meaningfulBits && double(maximum) - minimum >= qMax(1e-4, magnitude * 1e-5);
}

static void promoteContextualFloats(QVector<Candidate>& candidates, const QVector<Sample>& samples,
                                  const QByteArray& knownFloats, const QByteArray& matrixFloats,
                                  int regionOffset, int length) {
    if (samples.isEmpty()) return;
    QHash<int, int> floats;
    QSet<int> anchors, simple, corroboratable, blocked;
    for (const auto& c : candidates)
        if (c.kind == NodeKind::Double && c.autoApplicable)
            for (int p = c.offset; p < c.offset + c.size; p += 4) blocked.insert(p);
    for (int i = 0; i < candidates.size(); ++i) {
        auto& c = candidates[i];
        if (c.kind != NodeKind::Float || c.offset % 4 || c.observations != samples.size()) continue;
        if (blocked.contains(c.offset)) {
            c.autoApplicable = false; c.evidenceLevel = QStringLiteral("hypothesis");
            c.score = qMin(c.score, 70);
            c.caveats << "This lane is part of a stronger double interpretation";
            continue;
        }
        floats.insert(c.offset, i);
        if (c.autoApplicable) anchors.insert(c.offset);
        bool shortDecimal = false;
        if (contextualFloat(c, samples, regionOffset, shortDecimal)) {
            corroboratable.insert(c.offset);
            if (shortDecimal) simple.insert(c.offset);
        }
    }
    for (int off = 0; off + 4 <= length; ++off) {
        if ((regionOffset + off) % 4 || !covered(knownFloats, off, 4)) continue;
        bool good = true;
        for (const auto& sample : samples) {
            if (!covered(sample.valid, off, 4) || off + 4 > sample.bytes.size()
                || !detail::isGoodFloat(detail::loadU32(
                    reinterpret_cast<const uint8_t*>(sample.bytes.constData() + off)))) { good = false; break; }
        }
        if (good) anchors.insert(regionOffset + off);
    }
    // Use only the original anchors: newly promoted constants cannot propagate
    // confidence through an arbitrary run of unrelated bytes.
    for (auto& c : candidates) {
        if (c.kind != NodeKind::Float || c.autoApplicable || c.offset % 4
            || c.observations != samples.size() || blocked.contains(c.offset)) continue;
        if (covered(matrixFloats, c.offset - regionOffset, 4)) {
            promote(c, 90, "Float lane belongs to a consistently validated affine matrix");
            continue;
        }
        if (!corroboratable.contains(c.offset)) continue;
        for (int neighbor : {c.offset - 4, c.offset + 4}) {
            if (blocked.contains(neighbor)) continue;
            const int other = floats.value(neighbor, -1);
            if (anchors.contains(neighbor)
                || (other >= 0 && corroboratable.contains(neighbor)
                    && (simple.contains(c.offset) || simple.contains(neighbor)))) {
                promote(c, 84, "Finite float corroborated by an adjacent established float or short-decimal float values");
                c.caveats << "Float representation inferred from context; adjacency does not establish vector semantics";
                break;
            }
        }
    }
}

QVector<Candidate> infer(const QVector<Sample>& samples, const QByteArray& eligible,
                         int regionOffset, int pointerSize, const QByteArray& knownFloats) {
    QVector<Candidate> result;
    if (samples.isEmpty()) return result;
    QByteArray matrixFloats(eligible.size(), '\0');
    result = matrixCandidates(samples, eligible, knownFloats, regionOffset, matrixFloats);
    const auto& last = samples.last();
    for (int off = 0; off < eligible.size(); ++off) {
        for (int size : {1, 2, 4, 8, 12, 16}) {
            if (!covered(eligible, off, size) || !covered(last.valid, off, size)
                || int64_t(off) + size > last.bytes.size()) continue;
            const QByteArray current = last.bytes.mid(off, size);
            const auto* raw = reinterpret_cast<const uint8_t*>(current.constData());
            if (detail::allZero(raw, size)) {
                bool previouslyNonzero = false;
                for (const auto& sample : samples)
                    if (covered(sample.valid, off, size) && off + size <= sample.bytes.size())
                        for (int i = 0; i < size; ++i) previouslyNonzero |= sample.bytes[off + i] != 0;
                if (!previouslyNonzero && !(size == 4 && covered(matrixFloats, off, 4)))
                    continue; // absence of evidence, not padding
            }
            QVector<NodeKind> kinds;
            auto addKind = [&](NodeKind kind) { if (!kinds.contains(kind)) kinds.append(kind); };
            if (size == 1) { addKind(NodeKind::Int8); addKind(NodeKind::UInt8); addKind(NodeKind::Bool); }
            if (size == 2) { addKind(NodeKind::Int16); addKind(NodeKind::UInt16); }
            if (size == 4) { addKind(NodeKind::Int32); addKind(NodeKind::UInt32); addKind(NodeKind::Float); }
            if (size == 8) { addKind(NodeKind::Int64); addKind(NodeKind::UInt64); addKind(NodeKind::Double); }
            if (size == pointerSize) addKind(size == 8 ? NodeKind::Pointer64 : NodeKind::Pointer32);
            // Vector candidates may span several original hex nodes.
            if (size == 8) kinds.append(NodeKind::Vec2);
            if (size == 12) kinds.append(NodeKind::Vec3);
            if (size == 16) kinds.append(NodeKind::Vec4);
            for (NodeKind kind : kinds) {
                Candidate c;
                c.offset = regionOffset + off; c.size = size; c.kind = kind;
                c.id = QStringLiteral("%1:%2:%3").arg(c.offset).arg(size).arg(kindToString(kind));
                QSet<QByteArray> distinct;
                int plausible = 0, transitions = 0, monotonicSteps = 0, tinyRawSteps = 0;
                double previous = 0, minimum = std::numeric_limits<double>::infinity();
                double maximum = -std::numeric_limits<double>::infinity(), maxAbs = 0;
                uint64_t previousBits = 0;
                bool havePrevious = false, havePreviousBits = false, fractional = false;
                const bool vector = isVectorKind(kind);
                const bool floating = kind == NodeKind::Float || kind == NodeKind::Double || vector;
                const bool pointer = kind == NodeKind::Pointer32 || kind == NodeKind::Pointer64;
                for (const auto& sample : samples) {
                    if (!covered(sample.valid, off, size) || off + size > sample.bytes.size()) continue;
                    const QByteArray bytes = sample.bytes.mid(off, size);
                    distinct.insert(bytes); ++c.observations;
                    const auto* p = reinterpret_cast<const uint8_t*>(bytes.constData());
                    bool good = false;
                    double value = 0;
                    if (floating) {
                        good = true;
                        const int count = vector ? size / 4 : 1;
                        for (int k = 0; k < count; ++k) {
                            const double v = kind == NodeKind::Double ? detail::loadF64(p) : detail::loadF32(p + k * 4);
                            const double a = std::fabs(v);
                            good &= std::isfinite(v) && (a == 0 || (a >= 1e-6 && a <= 1e7));
                            if (std::isfinite(v) && std::fabs(v - std::trunc(v)) > 0.0001) fractional = true;
                            value = v;
                        }
                    } else if (pointer) {
                        const uint64_t v = size == 8 ? detail::loadU64(p) : detail::loadU32(p);
                        const auto features = size == 8 ? detail::countPtrFeatures64(v)
                                                       : detail::countPtrFeatures32(uint32_t(v));
                        good = v == 0 || features.passed == features.checked;
                        if (sample.timestampMs == last.timestampMs && v == 0) good = false;
                    } else if (kind == NodeKind::Bool) {
                        good = p[0] <= 1; value = p[0];
                    } else {
                        // Evaluate each scalar interpretation, not bytewise min/max.
                        uint64_t bits = 0; std::memcpy(&bits, p, size);
                        const bool sign = kind == NodeKind::Int8 || kind == NodeKind::Int16
                            || kind == NodeKind::Int32 || kind == NodeKind::Int64;
                        int64_t signedValue = int64_t(bits);
                        if (sign && size < 8 && (bits & (uint64_t(1) << (size * 8 - 1))))
                            signedValue = int64_t(bits | (~uint64_t(0) << (size * 8)));
                        value = sign ? double(signedValue) : double(bits);
                        good = std::fabs(value) <= 1000000;
                    }
                    plausible += good;
                    if (good && floating && !vector) {
                        minimum = qMin(minimum, value); maximum = qMax(maximum, value);
                        maxAbs = qMax(maxAbs, std::fabs(value));
                        uint64_t bits = 0;
                        std::memcpy(&bits, p, size);
                        if (havePreviousBits && bits != previousBits) {
                            const uint64_t delta = bits > previousBits ? bits - previousBits : previousBits - bits;
                            if (delta <= 4) ++tinyRawSteps;
                        }
                        previousBits = bits; havePreviousBits = true;
                    }
                    if (havePrevious && value != previous) {
                        ++transitions;
                        if (value > previous) ++monotonicSteps;
                    }
                    previous = value; havePrevious = true;
                    if (c.values.size() == 3) c.values.removeFirst();
                    c.values.append(QJsonObject{{"timestampMs", double(sample.timestampMs)},
                        {"hex", QString::fromLatin1(bytes.toHex(' '))},
                        {"interpreted", floating || !pointer ? QString::number(value, 'g', 17) : QString()},
                        {"note", vector ? "Interpreted value is the last vector component; hex contains all components" : ""}});
                }
                if (!c.observations || plausible != c.observations) continue;
                c.distinctValues = distinct.size();
                c.evidence << QStringLiteral("Interpretation consistent with %1 successful reads").arg(c.observations);
                c.caveats << "Rank is heuristic, not a correctness probability"
                          << "Polling may miss changes; source-level type remains a hypothesis";
                c.score = 35;
                if (floating) {
                    if (!fractional && kind != NodeKind::Float) continue;
                    c.score = 55 + (c.distinctValues >= 3 ? 15 : 0);
                    if (c.distinctValues >= 3) c.evidence << "Multiple distinct finite IEEE values";
                    if (vector) { c.score -= 8; c.caveats << "Adjacent floats do not prove vector semantics"; }
                    if (!vector && c.observations == samples.size() && samples.size() >= 8
                        && c.distinctValues >= 4 && c.offset % size == 0) {
                        const double requiredRange = kind == NodeKind::Double
                            ? qMax(1e-9, maxAbs * 1e-7) : qMax(1e-4, maxAbs * 1e-5);
                        const bool meaningfulChange = std::isfinite(minimum) && std::isfinite(maximum)
                            && maximum - minimum >= requiredRange;
                        // A raw integer counter passing through an IEEE-looking
                        // bit range often moves by one ULP every sample. Do not
                        // promote that accidental interpretation automatically.
                        const bool rawCounterPattern = transitions >= 3 && tinyRawSteps == transitions;
                        if (meaningfulChange && !rawCounterPattern) {
                            c.score = 88;
                            c.autoApplicable = true;
                            c.evidenceLevel = QStringLiteral("high");
                            c.evidence << "All observations are readable and show meaningful IEEE value changes"
                                       << "Integer-bit counter pattern rejected";
                            c.caveats << "This establishes a primitive IEEE representation, not a semantic name or grouping";
                        }
                    }
                } else if (pointer) {
                    c.score = 45;
                    const uint64_t target = size == 8 ? detail::loadU64(raw) : detail::loadU32(raw);
                    c.target = QStringLiteral("0x%1").arg(target, 0, 16);
                    c.caveats << "Address shape alone does not establish a pointer";
                } else {
                    c.caveats << "Signedness and semantic name are unresolved";
                    if (transitions >= 2 && (monotonicSteps == transitions || monotonicSteps == 0)) {
                        c.score += 15; c.evidence << "Observed monotonic changes";
                    }
                    if (kind == NodeKind::Bool && c.distinctValues == 2) {
                        c.score = 60; c.evidence << "Both zero and one observed";
                    }
                }
                if (c.offset % qMin(size, 4) != 0) { c.score -= 10; c.caveats << "Unaligned candidate"; }
                result.append(c);
            }
        }
        // Recover a terminated printable string prefix; never infer allocation capacity.
        for (int width : {1, 2}) {
            if (width == 2 && (regionOffset + off) % 2) continue;
            Candidate c;
            c.offset = regionOffset + off;
            c.kind = width == 1 ? NodeKind::UTF8 : NodeKind::UTF16;
            bool consistent = true;
            QSet<QByteArray> distinct;
            for (const auto& sample : samples) {
                int chars = 0;
                bool terminated = false, letter = false;
                while (chars < 64 && covered(sample.valid, off + chars * width, width)
                       && covered(eligible, off + chars * width, width)
                       && off + (chars + 1) * width <= sample.bytes.size()) {
                    const auto* p = reinterpret_cast<const uint8_t*>(sample.bytes.constData() + off + chars * width);
                    const uint16_t ch = width == 1 ? p[0] : detail::loadU16(p);
                    if (ch == 0) { terminated = true; break; }
                    if (ch < 32 || ch > 126) break;
                    letter |= (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
                    ++chars;
                }
                if (!terminated || chars < 4 || !letter) { consistent = false; break; }
                const int extent = (chars + 1) * width;
                c.size = qMax(c.size, extent);
                ++c.observations;
                distinct.insert(sample.bytes.mid(off, chars * width));
                if (c.values.size() == 3) c.values.removeFirst();
                c.values.append(QJsonObject{{"timestampMs", double(sample.timestampMs)},
                    {"hex", QString::fromLatin1(sample.bytes.mid(off, chars * width).toHex(' '))}});
            }
            if (!consistent || !c.observations || !covered(eligible, off, c.size)) continue;
            c.distinctValues = distinct.size(); c.score = 60;
            c.id = QStringLiteral("%1:%2:%3").arg(c.offset).arg(c.size).arg(kindToString(c.kind));
            c.evidence << "Terminated printable text in all observations";
            c.caveats << "Only the observed prefix is known; buffer capacity is unresolved"
                      << "Printable bytes can occur in non-text data; score is not a probability";
            result.append(c);
        }
    }
    // Two adjacent 32-bit lanes can form a perfectly plausible double when
    // their bytes are concatenated (float+float and counter+float are common).
    // Keep the double as a hypothesis whenever both halves independently have
    // consistent scalar interpretations.
    for (auto& c : result) {
        if (!c.autoApplicable || c.kind != NodeKind::Double) continue;
        bool lowLane = false, highLane = false;
        for (const auto& other : result) {
            if (other.size != 4 || other.observations != samples.size() || other.score < 35) continue;
            lowLane |= other.offset == c.offset;
            highLane |= other.offset == c.offset + 4;
        }
        auto plausibleRawLane = [&](int absolute) {
            const int local = absolute - regionOffset;
            bool smallInteger = true, ieeeFloat = true;
            int seen = 0;
            for (const auto& sample : samples) {
                if (!covered(sample.valid, local, 4) || local + 4 > sample.bytes.size()) return false;
                const auto* bytes = reinterpret_cast<const uint8_t*>(sample.bytes.constData() + local);
                const uint32_t integer = detail::loadU32(bytes);
                const double value = detail::loadF32(bytes), magnitude = std::fabs(value);
                smallInteger &= integer <= 1000000;
                ieeeFloat &= std::isfinite(value) && (magnitude == 0 || (magnitude >= 1e-6 && magnitude <= 1e7));
                ++seen;
            }
            return seen == samples.size() && (smallInteger || ieeeFloat);
        };
        lowLane |= plausibleRawLane(c.offset);
        highLane |= plausibleRawLane(c.offset + 4);
        if (lowLane && highLane) {
            c.autoApplicable = false;
            c.evidenceLevel = QStringLiteral("hypothesis");
            c.score = qMin(c.score, 70);
            c.caveats << "The same bytes are also two plausible 32-bit lanes; double is ambiguous";
        }
    }
    promoteContextualFloats(result, samples, knownFloats, matrixFloats, regionOffset, eligible.size());
    return result;
}

void selectLayout(Analysis& analysis, const QByteArray& eligible, int regionOffset, bool automaticOnly) {
    // Weighted interval selection. Unknown bytes carry zero cost and remain hex.
    // AUTO uses only candidates that pass the native automatic gate.
    const int length = eligible.size();
    analysis.suggestedFields = {};
    analysis.unresolved = {};
    QVector<QVector<int>> ends(length + 1);
    for (int i = 0; i < analysis.candidates.size(); ++i) {
        const Candidate& c = analysis.candidates[i];
        const int end = c.offset - regionOffset + c.size;
        if ((!automaticOnly || c.autoApplicable) && c.score >= 60
            && covered(eligible, c.offset - regionOffset, c.size)
            && end > 0 && end <= length) ends[end].append(i);
    }
    QVector<double> score(length + 1, 0);
    QVector<int> choice(length + 1, -1);
    for (int end = 1; end <= length; ++end) {
        score[end] = score[end - 1];
        for (int i : ends[end]) {
            const Candidate& c = analysis.candidates[i];
            const double value = score[end - c.size] + (c.score - 50) * c.size;
            if (value > score[end]) { score[end] = value; choice[end] = i; }
        }
    }
    QVector<int> selected;
    for (int end = length; end > 0;) {
        if (choice[end] < 0) { --end; continue; }
        selected.prepend(choice[end]); end -= analysis.candidates[choice[end]].size;
    }
    QByteArray resolved(length, '\0');
    for (int i : selected) {
        if (analysis.suggestedFields.size() >= 512) { analysis.truncated = true; break; }
        const Candidate& c = analysis.candidates[i];
        analysis.suggestedFields.append(QJsonObject{{"candidateId", c.id}});
        for (int p = c.offset - regionOffset; p < c.offset - regionOffset + c.size; ++p) resolved[p] = 1;
    }
    for (int i = 0; i < length;) {
        if (!eligible[i] || resolved[i]) { ++i; continue; }
        const int start = i++;
        while (i < length && eligible[i] && !resolved[i]) ++i;
        analysis.unresolved.append(QJsonObject{{"offset", regionOffset + start}, {"size", i - start},
            {"reason", "Insufficient or ambiguous evidence; retain raw bytes"}});
    }
}

static bool jsonInt(const QJsonValue& value, int& out) {
    if (!value.isDouble() || !std::isfinite(value.toDouble())
        || value.toDouble() < 0 || value.toDouble() > INT_MAX
        || value.toDouble() != std::floor(value.toDouble())) return false;
    out = value.toInt(); return true;
}

bool prepare(const NodeTree& tree, uint64_t parentId, int regionOffset, int length,
             const QVector<Candidate>& candidates, const QJsonArray& fields,
             NodeTree& result, QJsonArray& appliedFields, QString& error) {
    appliedFields = {}; error.clear();
    if (regionOffset < 0 || length <= 0 || int64_t(regionOffset) + length > INT_MAX) {
        error = "Invalid resolution range"; return false;
    }
    if (fields.isEmpty() || fields.size() > 512) { error = "Provide 1..512 fields"; return false; }
    const QByteArray eligible = eligibleBytes(tree, parentId, regionOffset, length);
    QHash<QString, const Candidate*> byId;
    for (const auto& c : candidates) byId.insert(c.id, &c);
    QVector<Node> additions;
    QVector<QPair<int, int>> ranges;
    result = tree;
    int count = 0;
    std::function<bool(const QJsonObject&, uint64_t, int, int, int)> build;
    build = [&](const QJsonObject& field, uint64_t parent, int origin, int bound, int depth) {
        if (++count > 512 || depth > 4) { error = "Layout exceeds node/depth limit"; return false; }
        Node n;
        n.id = result.reserveId(); n.parentId = parent;
        int absolute = 0, size = 0;
        const QString candidateId = field.value("candidateId").toString();
        const QString group = field.value("kind").toString();
        const QSet<QString> allowed = !candidateId.isEmpty()
            ? QSet<QString>{"candidateId", "kind", "offset", "size", "name", "refId"}
            : group == "Struct"
                ? QSet<QString>{"kind", "offset", "size", "name", "typeName", "fields"}
                : QSet<QString>{"kind", "offset", "size", "name", "elements"};
        for (auto it = field.begin(); it != field.end(); ++it) {
            if (!allowed.contains(it.key())) { error = "Unsupported field property: " + it.key(); return false; }
        }
        if (field.contains("kind") && !field.value("kind").isString()) {
            error = "kind must be a string"; return false;
        }
        if (!candidateId.isEmpty()) {
            const Candidate* c = byId.value(candidateId, nullptr);
            if (!c) { error = "Unknown candidateId: " + candidateId; return false; }
            absolute = c->offset; size = c->size; n.kind = c->kind;
            if (n.kind == NodeKind::UTF8 || n.kind == NodeKind::UTF16)
                n.strLen = size / (n.kind == NodeKind::UTF16 ? 2 : 1);
            if ((!group.isEmpty() && group != kindToString(n.kind))
                || field.contains("fields") || field.contains("elements")) {
                error = "A candidate's kind and layout cannot be overridden"; return false;
            }
            if (field.contains("offset") && field.value("offset").toDouble(-1) != absolute) {
                error = "Offset does not match candidate"; return false;
            }
            if (field.contains("size") && field.value("size").toDouble(-1) != size) {
                error = "Size does not match candidate"; return false;
            }
            if (n.kind == NodeKind::Pointer32 || n.kind == NodeKind::Pointer64) {
                if (c->score < 60) { error = "Pointer target has not been validated"; return false; }
                if (field.contains("refId")) {
                    bool ok = false;
                    n.refId = field.value("refId").toString().toULongLong(&ok);
                    const int idx = result.indexOfId(n.refId);
                    if (!ok || idx < 0 || result.nodes[idx].kind != NodeKind::Struct) {
                        error = "refId must identify an existing struct"; return false;
                    }
                }
            } else if (field.contains("refId")) { error = "refId is only valid for pointers"; return false; }
        } else if (group == "Struct" || group == "Array") {
            if (field.contains("refId")) { error = "Group refId overrides are unsupported"; return false; }
            if (!jsonInt(field.value("offset"), absolute) || !jsonInt(field.value("size"), size) || size <= 0) {
                error = "Groups require absolute parent-relative offset and positive size"; return false;
            }
            n.kind = group == "Struct" ? NodeKind::Struct : NodeKind::Array;
        } else { error = "Leaves require a candidateId; groups require kind Struct or Array"; return false; }
        if (absolute < origin || size <= 0 || int64_t(absolute) + size > bound
            || !covered(eligible, absolute - regionOffset, size)) {
            error = "Field overlaps protected bytes or leaves the observed region"; return false;
        }
        n.offset = absolute - origin;
        if (field.contains("name") && !field.value("name").isString()) { error = "name must be a string"; return false; }
        n.name = field.value("name").toString();
        if (n.name.size() > 128 || n.name.contains('\n') || n.name.contains('\r')) {
            error = "Invalid field name"; return false;
        }
        if (n.name.isEmpty() && parent == parentId) {
            for (int index : tree.childrenOf(parentId)) {
                const Node& existing = tree.nodes[index];
                if (existing.offset == absolute && !existing.name.isEmpty()) { n.name = existing.name; break; }
            }
        }
        if (n.name.isEmpty()) n.name = QStringLiteral("field_%1").arg(absolute, 0, 16);
        n.comment = QStringLiteral("Inferred by AUTO; evidence %1").arg(candidateId.isEmpty() ? group : candidateId);
        if (n.kind == NodeKind::Struct) {
            if (field.contains("typeName") && !field.value("typeName").isString()) {
                error = "typeName must be a string"; return false;
            }
            n.structTypeName = field.value("typeName").toString(n.name);
            n.collapsed = false;
            if (n.structTypeName.size() > 128 || n.structTypeName.contains('\n') || n.structTypeName.contains('\r')) {
                error = "Invalid struct type name"; return false;
            }
            const QJsonArray children = field.value("fields").toArray();
            if (children.isEmpty()) { error = "Struct requires fields"; return false; }
            // Groups must be fully covered, without implied/guessed padding.
            int cursor = absolute;
            for (const auto& child : children) {
                const int before = additions.size();
                if (!child.isObject() || !build(child.toObject(), n.id, absolute, absolute + size, depth + 1)) return false;
                const Node& root = additions[before];
                if (root.offset + absolute != cursor) { error = "Struct fields must cover its range in order"; return false; }
                const auto* c = byId.value(child.toObject().value("candidateId").toString(), nullptr);
                cursor += c ? c->size : child.toObject().value("size").toInt();
            }
            if (cursor != absolute + size) { error = "Struct extent differs from its fields"; return false; }
        } else if (n.kind == NodeKind::Array) {
            const QJsonArray elements = field.value("elements").toArray();
            if (elements.isEmpty() || elements.size() > 512) { error = "Array requires candidateId elements"; return false; }
            int cursor = absolute, elementSize = 0;
            for (const auto& id : elements) {
                const Candidate* c = byId.value(id.toString(), nullptr);
                if (!c || sizeForKind(c->kind) != c->size || c->offset != cursor
                    || (elementSize && (c->size != elementSize || c->kind != n.elementKind))
                    || ((c->kind == NodeKind::Pointer32 || c->kind == NodeKind::Pointer64) && c->score < 60)) {
                    error = "Array elements must be contiguous, uniform validated candidates"; return false;
                }
                n.elementKind = c->kind; elementSize = c->size; cursor += c->size;
            }
            if (cursor != absolute + size) { error = "Array extent differs from its elements"; return false; }
            n.arrayLen = elements.size();
        }
        // Root precedes its children; the validator uses this order for grouping.
        if (n.kind == NodeKind::Struct) {
            // Insert before this group's descendants (their offsets are local).
            int at = 0;
            while (at < additions.size() && additions[at].parentId != n.id) ++at;
            additions.insert(at, n);
        } else additions.append(n);
        return true;
    };
    for (const auto& value : fields) {
        if (!value.isObject()) { error = "Each field must be an object"; return false; }
        const int first = additions.size();
        if (!build(value.toObject(), parentId, 0, regionOffset + length, 0)) return false;
        const Node& n = additions[first];
        const auto* c = byId.value(value.toObject().value("candidateId").toString(), nullptr);
        const int size = c ? c->size : value.toObject().value("size").toInt();
        ranges.append({n.offset, n.offset + size});
        appliedFields.append(QJsonObject{{"nodeId", QString::number(n.id)}, {"offset", n.offset},
                                        {"size", size}, {"kind", kindToString(n.kind)}, {"name", n.name}});
    }
    std::sort(ranges.begin(), ranges.end());
    for (int i = 1; i < ranges.size(); ++i) if (ranges[i].first < ranges[i - 1].second) {
        error = "Proposed fields overlap"; return false;
    }
    QVector<Node> kept;
    auto pad = [&](const Node& original, int start, int end, bool preserveId) {
        while (start < end) {
            Node n = original;
            int size = 1;
            for (int s : {16, 8, 4, 2, 1}) if (s <= end - start) { size = s; break; }
            n.id = preserveId ? original.id : result.reserveId(); preserveId = false;
            n.kind = size == 16 ? NodeKind::Hex128 : size == 8 ? NodeKind::Hex64
                   : size == 4 ? NodeKind::Hex32 : size == 2 ? NodeKind::Hex16 : NodeKind::Hex8;
            n.offset = start;
            if (start != original.offset) n.name = QStringLiteral("field_%1").arg(start, 0, 16);
            kept.append(n); start += size;
        }
    };
    for (const Node& n : tree.nodes) {
        if (n.parentId != parentId || !replaceable(n)) { kept.append(n); continue; }
        int cursor = n.offset, end = n.offset + n.byteSize();
        bool touched = false;
        for (const auto& range : ranges) if (range.first < end && range.second > n.offset) { touched = true; break; }
        if (!touched) { kept.append(n); continue; }
        bool preserve = true;
        for (const auto& range : ranges) {
            if (range.second <= cursor || range.first >= end) continue;
            if (range.first > cursor) { pad(n, cursor, qMin(end, range.first), preserve); preserve = false; }
            cursor = qMax(cursor, range.second);
        }
        if (cursor < end) pad(n, cursor, end, preserve);
    }
    kept += additions;
    result.nodes = kept; result.invalidateIdCache(); result.touch();
    if (!result.validate(false).clean() || result.structSpan(parentId) != tree.structSpan(parentId)) {
        error = "Replacement would invalidate the tree or change the structure extent"; return false;
    }
    return true;
}

} // namespace rcx::resolution
