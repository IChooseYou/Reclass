#include <QtTest/QTest>
#include <QJsonDocument>
#include <cstring>
#include "type_resolution.h"
#include "demo_simulation.h"

using namespace rcx;
using namespace rcx::resolution;

class TestTypeResolution : public QObject {
    Q_OBJECT
    static NodeTree rawTree(int length = 16) {
        NodeTree tree;
        Node root; root.id = tree.reserveId(); root.kind = NodeKind::Struct;
        tree.nodes.append(root);
        for (int offset = 0; offset < length; offset += 8) {
            Node n; n.id = tree.reserveId(); n.parentId = root.id;
            n.kind = NodeKind::Hex64; n.offset = offset;
            n.name = QStringLiteral("field_%1").arg(offset, 4, 16, QChar('0'));
            tree.nodes.append(n);
        }
        tree.touch();
        return tree;
    }
    static Candidate candidate(int offset, int size, NodeKind kind) {
        Candidate c; c.offset = offset; c.size = size; c.kind = kind; c.score = 70;
        c.id = QStringLiteral("%1:%2:%3").arg(offset).arg(size).arg(kindToString(kind));
        return c;
    }
    static QJsonObject leaf(const Candidate& c) { return {{"candidateId", c.id}}; }
    static Sample sample(float a, float b, float c, qint64 timestamp) {
        Sample s; s.bytes.resize(16); s.bytes.fill(0); s.valid = QByteArray(16, char(1));
        const float values[] = {a, b, c}; std::memcpy(s.bytes.data(), values, sizeof(values));
        s.timestampMs = timestamp; return s;
    }
    static bool automatic(const QVector<Candidate>& candidates, int offset, NodeKind kind) {
        for (const auto& c : candidates)
            if (c.offset == offset && c.kind == kind && c.autoApplicable) return true;
        return false;
    }
    static QVector<Sample> repeated(const QByteArray& bytes) {
        QVector<Sample> samples;
        for (int i = 0; i < 12; ++i) samples.append({bytes, QByteArray(bytes.size(), char(1)), i, {}});
        return samples;
    }
    static QByteArray matrix() {
        const float values[] = {.35f, 0, 0, 0, 0, .30f, 0, 0, 0, 0, .35f, 0, 2.2f, .5f, 1.1f, 1};
        return QByteArray(reinterpret_cast<const char*>(values), sizeof(values));
    }
private slots:
    void constantsNeedContextAndAllReads() {
        const auto bytes = sample(.35f, .72f, 1.0f, 0).bytes;
        auto samples = repeated(bytes);
        const QByteArray eligible(16, char(1));
        auto candidates = infer(samples, eligible, 0, 8);
        for (int off : {0, 4, 8}) QVERIFY(automatic(candidates, off, NodeKind::Float));
        QVERIFY(!automatic(candidates, 12, NodeKind::Float)); // zero remains unknown
        QVERIFY(!automatic(candidates, 0, NodeKind::Double));
        QVERIFY(automatic(infer({samples.first()}, eligible, 0, 8), 0, NodeKind::Float));
        QVERIFY(!automatic(infer(repeated(bytes.left(4)), QByteArray(4, char(1)), 0, 8), 0, NodeKind::Float));
        samples[3].valid.fill(0);
        candidates = infer(samples, eligible, 0, 8);
        for (const auto& c : candidates) QVERIFY(!c.autoApplicable);

        // Repeated IEEE-shaped flag words are not enough without an anchor.
        candidates = infer(repeated(sample(1, 1, 1, 0).bytes), eligible, 0, 8);
        for (const auto& c : candidates) QVERIFY(!c.autoApplicable);
        candidates = infer(repeated(bytes), eligible, 1, 8);
        QVERIFY(!automatic(candidates, 1, NodeKind::Float));
    }
    void occasionalChangesKeepContextButRawCountersDoNot() {
        auto samples = repeated(sample(.35f, .72f, 1, 0).bytes);
        Sample fresh = samples.last(); fresh.timestampMs++;
        const float changed = .375f;
        std::memcpy(fresh.bytes.data(), &changed, 4);
        samples.append(fresh);
        auto candidates = infer(samples, QByteArray(16, char(1)), 0, 8);
        QVERIFY(automatic(candidates, 0, NodeKind::Float));
        const float zero = 0;
        std::memcpy(samples.last().bytes.data(), &zero, 4);
        candidates = infer(samples, QByteArray(16, char(1)), 0, 8);
        QVERIFY(automatic(candidates, 0, NodeKind::Float));
        for (int i = 0; i < samples.size(); ++i) {
            const uint32_t bits = 0x3f800000u + i;
            std::memcpy(samples[i].bytes.data(), &bits, 4);
        }
        candidates = infer(samples, QByteArray(16, char(1)), 0, 8);
        QVERIFY(!automatic(candidates, 0, NodeKind::Float));
        // A fresh unreadable or non-IEEE value must still invalidate the candidate.
        const uint32_t nan = 0x7fc00000;
        std::memcpy(samples.last().bytes.data() + 4, &nan, 4);
        candidates = infer(samples, QByteArray(16, char(1)), 0, 8);
        QVERIFY(!automatic(candidates, 4, NodeKind::Float));
    }
    void existingFloatContextHelpsRemainingRawBytes() {
        auto tree = rawTree(); const auto root = tree.nodes.first().id;
        tree.nodes[1].kind = NodeKind::Float;
        tree.nodes[2].offset = 4; tree.nodes[2].kind = NodeKind::Hex32;
        tree.touch();
        auto eligible = eligibleBytes(tree, root, 0, 8);
        auto context = knownFloatBytes(tree, root, 0, 8);
        QCOMPARE(context, QByteArray(4, char(1)) + QByteArray(4, '\0'));
        const auto samples = repeated(sample(1.234567f, 1.0f, 0, 0).bytes.left(8));
        QVERIFY(!automatic(infer(samples, eligible, 0, 8), 4, NodeKind::Float));
        auto candidates = infer(samples, eligible, 0, 8, context);
        QVERIFY(automatic(candidates, 4, NodeKind::Float));
        QVERIFY(!automatic(candidates, 0, NodeKind::Float));
        // Context must not cross an overlapping or endian-incompatible field.
        tree.nodes[1].bigEndian = true;
        QCOMPARE(knownFloatBytes(tree, root, 0, 8), QByteArray(8, '\0'));
        tree.nodes[1].bigEndian = false;
        Node overlap = tree.nodes[1]; overlap.id = tree.reserveId(); overlap.kind = NodeKind::Hex32;
        tree.nodes.append(overlap); tree.invalidateIdCache(); tree.touch();
        QCOMPARE(knownFloatBytes(tree, root, 0, 8), QByteArray(8, '\0'));
    }
    void constantPromotionDoesNotCascadeOrSplitRealDoubles() {
        auto samples = repeated(sample(.1234567f, .7654321f, 1.234567f, 0).bytes);
        for (int i = 0; i < samples.size(); ++i) {
            const float changing = .25f + i * .125f;
            std::memcpy(samples[i].bytes.data(), &changing, 4);
        }
        auto candidates = infer(samples, QByteArray(16, char(1)), 0, 8);
        QVERIFY(automatic(candidates, 0, NodeKind::Float));
        QVERIFY(automatic(candidates, 4, NodeKind::Float));
        QVERIFY(!automatic(candidates, 8, NodeKind::Float));
        for (int i = 0; i < samples.size(); ++i) {
            const double value = 4.123456789 + i * .1379;
            std::memcpy(samples[i].bytes.data(), &value, 8);
        }
        candidates = infer(samples, QByteArray(16, char(1)), 0, 8);
        QVERIFY(automatic(candidates, 0, NodeKind::Double));
        QVERIFY(!automatic(candidates, 4, NodeKind::Float));
    }
    void affineStructureExplainsStaticZeroAndOneLanes() {
        const QByteArray eligible(64, char(1));
        const auto samples = repeated(matrix());
        QVERIFY(automatic(infer({samples.first()}, eligible, 0, 8), 0, NodeKind::Mat4x4));
        Analysis analysis; analysis.candidates = infer(samples, eligible, 0, 8);
        QVERIFY(automatic(analysis.candidates, 0, NodeKind::Mat4x4));
        selectLayout(analysis, eligible, 0, true);
        QCOMPARE(analysis.suggestedFields.size(), 1);
        QCOMPARE(analysis.suggestedFields.first().toObject()["candidateId"].toString(), QString("0:64:Mat4x4"));
        QVERIFY(analysis.unresolved.isEmpty());
        auto tree = rawTree(64);
        NodeTree result; QJsonArray applied; QString error;
        QVERIFY2(prepare(tree, tree.nodes.first().id, 0, 64, analysis.candidates, analysis.suggestedFields,
                         result, applied, error), qPrintable(error));
        QCOMPARE(result.structSpan(tree.nodes.first().id), 64);
        QCOMPARE(result.childrenOf(tree.nodes.first().id).size(), 1);

        auto partial = eligible;
        partial[0] = partial[1] = partial[2] = partial[3] = 0;
        QByteArray context(64, '\0'); context[0] = context[1] = context[2] = context[3] = 1;
        const auto remaining = infer(samples, partial, 0, 8, context);
        QVERIFY(!automatic(remaining, 0, NodeKind::Mat4x4));
        for (int off = 4; off < 64; off += 4) QVERIFY(automatic(remaining, off, NodeKind::Float));
        // Without typed context the protected gap cannot establish a matrix.
        QVERIFY(!automatic(infer(samples, partial, 0, 8), 4, NodeKind::Float));
    }
    void matrixRequiresBasisAndFreshConsistentReadableBytes() {
        auto bytes = matrix();
        std::memset(bytes.data(), 0, 48); // affine signature with a degenerate basis
        QVERIFY(!automatic(infer(repeated(bytes), QByteArray(64, char(1)), 0, 8), 0, NodeKind::Mat4x4));
        auto samples = repeated(matrix());
        samples[3].valid[20] = 0;
        QVERIFY(!automatic(infer(samples, QByteArray(64, char(1)), 0, 8), 0, NodeKind::Mat4x4));
        samples = repeated(matrix());
        Sample fresh = samples.last(); fresh.timestampMs++;
        const float bad = .4f;
        std::memcpy(fresh.bytes.data() + 60, &bad, 4);
        samples.append(fresh);
        QVERIFY(!automatic(infer(samples, QByteArray(64, char(1)), 0, 8), 0, NodeKind::Mat4x4));

        // Two overlapping identity-shaped windows cannot establish a boundary.
        QByteArray overlapping(84, '\0');
        const float one = 1;
        for (int off : {0, 20, 40, 60, 80}) std::memcpy(overlapping.data() + off, &one, 4);
        const auto ambiguous = infer(repeated(overlapping), QByteArray(84, char(1)), 0, 8);
        QVERIFY(!automatic(ambiguous, 0, NodeKind::Mat4x4));
        QVERIFY(!automatic(ambiguous, 20, NodeKind::Mat4x4));
        QVERIFY(!automatic(ambiguous, 4, NodeKind::Float));
    }
    void bouncingBallRecoversConstantsAndMatrixWithoutNames() {
        QByteArray buffer(demo::kBufferMin, '\0');
        demo::BouncingBallSim simulation(reinterpret_cast<uint8_t*>(buffer.data()), buffer.size());
        simulation.reset();
        QVector<Sample> samples;
        for (int i = 0; i < 16; ++i) {
            for (int step = 0; step < 3; ++step) simulation.step();
            samples.append({buffer.left(demo::ball::size), QByteArray(demo::ball::size, char(1)), i, {}});
        }
        const QByteArray eligible(demo::ball::size, char(1));
        Analysis analysis; analysis.candidates = infer(samples, eligible, 0, 8);
        QVERIFY(automatic(analysis.candidates, demo::ball::radius, NodeKind::Float));
        QVERIFY(automatic(analysis.candidates, demo::ball::restitution, NodeKind::Float));
        QVERIFY(automatic(analysis.candidates, demo::ball::transform, NodeKind::Mat4x4));
        QVERIFY(automatic(analysis.candidates, demo::ball::time, NodeKind::Double));
        selectLayout(analysis, eligible, 0, true);
        auto tree = rawTree(demo::ball::size);
        NodeTree result; QJsonArray applied; QString error;
        QVERIFY2(prepare(tree, tree.nodes.first().id, 0, eligible.size(), analysis.candidates,
                         analysis.suggestedFields, result, applied, error), qPrintable(error));
        QByteArray occupancy(eligible.size(), '\0');
        for (int idx : result.childrenOf(tree.nodes.first().id)) {
            const auto& n = result.nodes[idx];
            for (int i = n.offset; i < n.offset + n.byteSize(); ++i) ++occupancy[i];
        }
        QCOMPARE(occupancy, eligible); // no overlapping or missing byte ranges
        const auto paused = infer(repeated(samples.last().bytes), eligible, 0, 8);
        QVERIFY(automatic(paused, demo::ball::radius, NodeKind::Float));
        QVERIFY(automatic(paused, demo::ball::restitution, NodeKind::Float));
        QVERIFY(automatic(paused, demo::ball::transform, NodeKind::Mat4x4));
    }
    void crossRowVectorAndUnchangedExtent() {
        auto tree = rawTree();
        const auto c = candidate(0, 12, NodeKind::Vec3);
        NodeTree result; QJsonArray applied; QString error;
        QVERIFY2(prepare(tree, tree.nodes[0].id, 0, 16, {c}, {leaf(c)}, result, applied, error), qPrintable(error));
        QCOMPARE(result.structSpan(tree.nodes[0].id), 16);
        QCOMPARE(tree.nodes.size(), 3); // Input is unchanged.
        int covered = 0;
        for (int idx : result.childrenOf(tree.nodes[0].id)) covered += result.nodes[idx].byteSize();
        QCOMPARE(covered, 16);
        QVERIFY(result.validate(false).clean());
    }
    void partialRowKeepsBothUnknownEdges() {
        auto tree = rawTree();
        const auto c = candidate(2, 4, NodeKind::Int32);
        NodeTree result; QJsonArray applied; QString error;
        QVERIFY2(prepare(tree, tree.nodes[0].id, 0, 16, {c}, {leaf(c)}, result, applied, error), qPrintable(error));
        QByteArray occupancy(16, '\0');
        for (int idx : result.childrenOf(tree.nodes[0].id)) {
            const auto& n = result.nodes[idx];
            for (int i = n.offset; i < n.offset + n.byteSize(); ++i) ++occupancy[i];
        }
        QCOMPARE(occupancy, QByteArray(16, char(1)));
        QCOMPARE(result.nodes[result.indexOfId(tree.nodes[2].id)].name, tree.nodes[2].name);
    }
    void preserveNamesButProtectAnnotationsAndOverlaps() {
        auto tree = rawTree();
        const auto root = tree.nodes[0].id;
        tree.nodes[1].name = "health";
        QCOMPARE(eligibleBytes(tree, root, 0, 16), QByteArray(16, char(1)));
        tree.nodes[1].name = "field_0000"; tree.nodes[1].comment = "manual";
        QCOMPARE(eligibleBytes(tree, root, 0, 8), QByteArray(8, '\0'));
        tree.nodes[1].comment.clear(); tree.nodes[1].kind = NodeKind::Double;
        QCOMPARE(eligibleBytes(tree, root, 0, 8), QByteArray(8, '\0'));
        tree.nodes[1].kind = NodeKind::Hex64;
        Node overlap = tree.nodes[1]; overlap.id = tree.reserveId();
        tree.nodes.append(overlap); tree.invalidateIdCache(); tree.touch();
        QCOMPARE(eligibleBytes(tree, root, 0, 8), QByteArray(8, '\0'));
    }
    void rejectOverlappingProposalWithoutMutatingInput() {
        auto tree = rawTree(); const auto before = QJsonDocument(tree.toJson()).toJson();
        const auto a = candidate(0, 8, NodeKind::Double), b = candidate(4, 4, NodeKind::Float);
        NodeTree result; QJsonArray applied; QString error;
        QVERIFY(!prepare(tree, tree.nodes[0].id, 0, 16, {a, b}, {leaf(a), leaf(b)}, result, applied, error));
        QVERIFY(error.contains("overlap"));
        QCOMPARE(QJsonDocument(tree.toJson()).toJson(), before);
    }
    void nestedStructAndArrayUseOriginalParentOffsets() {
        auto tree = rawTree(); const auto root = tree.nodes[0].id;
        const auto a = candidate(8, 4, NodeKind::Float), b = candidate(12, 4, NodeKind::Float);
        QJsonObject array{{"kind", "Array"}, {"offset", 8}, {"size", 8}, {"elements", QJsonArray{a.id, b.id}}};
        QJsonObject group{{"kind", "Struct"}, {"offset", 8}, {"size", 8}, {"fields", QJsonArray{array}}};
        QJsonObject outer{{"kind", "Struct"}, {"offset", 8}, {"size", 8}, {"fields", QJsonArray{group}}};
        NodeTree result; QJsonArray applied; QString error;
        QVERIFY2(prepare(tree, root, 0, 16, {a, b}, {outer}, result, applied, error), qPrintable(error));
        QCOMPARE(result.structSpan(root), 16);
        for (const auto& n : result.nodes) if (n.kind == NodeKind::Array) {
            QCOMPARE(n.offset, 0); QCOMPARE(n.arrayLen, 2); QCOMPARE(n.elementKind, NodeKind::Float);
            QCOMPARE(n.byteSize(), 8);
        }
        QVERIFY(result.validate(false).clean());
    }
    void temporalCandidatesSpanRowsButFailedLatestReadIsUnknown() {
        QVector<Sample> samples{sample(1.25f, 2.5f, 3.75f, 1), sample(1.5f, 2.75f, 4.25f, 2),
                                sample(1.75f, 3.25f, 4.5f, 3)};
        const QByteArray eligible(16, char(1));
        const auto candidates = infer(samples, eligible, 0, 8);
        bool found = false;
        for (const auto& c : candidates) if (c.kind == NodeKind::Vec3 && c.offset == 0) {
            found = true; QCOMPARE(c.size, 12); QCOMPARE(c.distinctValues, 3);
        }
        QVERIFY(found);
        samples.last().valid.fill(0);
        QVERIFY(infer(samples, eligible, 0, 8).isEmpty());
        Sample zero; zero.bytes = QByteArray(16, '\0'); zero.valid = eligible;
        QVERIFY(infer({zero}, eligible, 0, 8).isEmpty());
    }
    void automaticTierNeedsMeaningfulTemporalEvidenceAndRejectsLaneAmbiguity() {
        QVector<Sample> samples;
        for (int i = 0; i < 12; ++i)
            samples.append(sample(1.25f + i * .25f, 2.5f + i * .125f, 0.0f, i + 1));
        const auto candidates = infer(samples, QByteArray(16, char(1)), 0, 8);
        bool firstFloat = false, secondFloat = false, automaticDouble = false;
        for (const auto& c : candidates) {
            if (c.kind == NodeKind::Float && c.offset == 0) firstFloat = c.autoApplicable;
            if (c.kind == NodeKind::Float && c.offset == 4) secondFloat = c.autoApplicable;
            if (c.kind == NodeKind::Double && c.offset == 0) automaticDouble |= c.autoApplicable;
        }
        QVERIFY(firstFloat); QVERIFY(secondFloat); QVERIFY(!automaticDouble);

        QVector<Sample> counterSamples;
        for (uint32_t bits = 0x3f800000; bits < 0x3f80000c; ++bits) {
            Sample s; s.bytes = QByteArray(4, '\0'); s.valid = QByteArray(4, char(1));
            std::memcpy(s.bytes.data(), &bits, 4); s.timestampMs = bits;
            counterSamples.append(s);
        }
        bool promotedCounter = false;
        for (const auto& c : infer(counterSamples, QByteArray(4, char(1)), 0, 8))
            if (c.kind == NodeKind::Float) promotedCounter |= c.autoApplicable;
        QVERIFY(!promotedCounter);

        QVector<Sample> counterPlusFloat;
        for (int i = 0; i < 12; ++i) {
            Sample s; s.bytes = QByteArray(8, '\0'); s.valid = QByteArray(8, char(1));
            const uint32_t counter = 0;
            const float changing = .5f + i * .1f;
            std::memcpy(s.bytes.data(), &counter, 4);
            std::memcpy(s.bytes.data() + 4, &changing, 4);
            s.timestampMs = i; counterPlusFloat.append(s);
        }
        bool combinedDouble = false, highFloat = false;
        for (const auto& c : infer(counterPlusFloat, QByteArray(8, char(1)), 0, 8)) {
            if (c.kind == NodeKind::Double && c.offset == 0) combinedDouble |= c.autoApplicable;
            if (c.kind == NodeKind::Float && c.offset == 4) highFloat |= c.autoApplicable;
        }
        QVERIFY(!combinedDouble); QVERIFY(highFloat);
    }
    void suggestionsRespectBatchLimitAndCanBeRecomputed() {
        Analysis analysis;
        for (int i = 0; i < 600; ++i) analysis.candidates.append(candidate(i, 1, NodeKind::Bool));
        selectLayout(analysis, QByteArray(600, char(1)), 0);
        QCOMPARE(analysis.suggestedFields.size(), 512); QVERIFY(analysis.truncated);
        QCOMPARE(analysis.unresolved.size(), 1);
        QCOMPARE(analysis.unresolved[0].toObject()["size"].toInt(), 88);
        selectLayout(analysis, QByteArray(600, char(1)), 0);
        QCOMPARE(analysis.suggestedFields.size(), 512);
    }
    void rejectCandidateSizeOverrideAndUnverifiedPointer() {
        auto tree = rawTree(); auto c = candidate(0, 8, NodeKind::Pointer64); c.score = 45;
        NodeTree result; QJsonArray applied; QString error;
        QVERIFY(!prepare(tree, tree.nodes[0].id, 0, 16, {c}, {leaf(c)}, result, applied, error));
        c.score = 65;
        auto proposal = leaf(c); proposal["size"] = 4;
        QVERIFY(!prepare(tree, tree.nodes[0].id, 0, 16, {c}, {proposal}, result, applied, error));
    }
};

QTEST_GUILESS_MAIN(TestTypeResolution)
#include "test_type_resolution.moc"
