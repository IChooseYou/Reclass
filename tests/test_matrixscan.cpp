#include <QTest>
#include <QSignalSpy>
#include <QByteArray>
#include <cstring>
#include "scanner.h"
#include "matrixscan.h"
#include "providers/buffer_provider.h"

using namespace rcx;

// A canonical row-major DirectX-style affine matrix: identity rotation +
// translation. Homogeneous (0,0,0,1) lands at float indices {3,7,11,15}.
static void identityWithTranslation(float m[16], float tx, float ty, float tz) {
    float src[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        tx,ty,tz,1
    };
    std::memcpy(m, src, sizeof(src));
}

// 90-degree rotation about Z (orthonormal, non-identity), row-major.
static void rotZ90(float m[16], float tx, float ty, float tz) {
    float src[16] = {
        0,-1,0,0,
        1, 0,0,0,
        0, 0,1,0,
        tx,ty,tz,1
    };
    std::memcpy(m, src, sizeof(src));
}

static QByteArray matBytes(const float m[16]) {
    QByteArray b(64, Qt::Uninitialized);
    std::memcpy(b.data(), m, 64);
    return b;
}

static void putMatrix(QByteArray& buf, int off, const float m[16]) {
    std::memcpy(buf.data() + off, m, 64);
}

static void putFloat(QByteArray& buf, int off, float v) {
    std::memcpy(buf.data() + off, &v, 4);
}

class TestMatrixScan : public QObject {
    Q_OBJECT

private slots:
    // ── Predicate unit tests (pure, no engine) ──

    void predicate_identityTranslation_scores100() {
        float m[16]; identityWithTranslation(m, 100, 200, 300);
        MatrixScanParams p;
        auto r = scoreMatrixWindow(reinterpret_cast<const uint8_t*>(m), p);
        QCOMPARE(r.score, 100);
        QVERIFY(r.affine);
        QVERIFY(r.orthonormal);
    }

    void predicate_rotationMatrix_scores100() {
        float m[16]; rotZ90(m, 5, 6, 7);
        MatrixScanParams p;
        auto r = scoreMatrixWindow(reinterpret_cast<const uint8_t*>(m), p);
        QCOMPARE(r.score, 100);
        QVERIFY(r.affine);
        QVERIFY(r.orthonormal);
    }

    void predicate_transposedLayout_homogLastRow() {
        // (0,0,0,1) at the last contiguous row {12,13,14,15}; translation at {3,7,11}.
        float m[16] = {
            1,0,0,10,
            0,1,0,20,
            0,0,1,30,
            0,0,0,1
        };
        MatrixScanParams p;
        auto r = scoreMatrixWindow(reinterpret_cast<const uint8_t*>(m), p);
        QCOMPARE(r.score, 100);
        QVERIFY(r.affine);
        QVERIFY(r.lastRowIsHomog);
    }

    void predicate_nanWindow_scores0() {
        uint32_t nan = 0x7FC00000;
        float m[16];
        for (int i = 0; i < 16; ++i) std::memcpy(&m[i], &nan, 4);
        MatrixScanParams p;
        auto r = scoreMatrixWindow(reinterpret_cast<const uint8_t*>(m), p);
        QCOMPARE(r.score, 0);
        QVERIFY(!r.affine);
    }

    void predicate_hugeMagnitude_rejectedByGate() {
        float m[16]; identityWithTranslation(m, 1e9f, 2e9f, 3e9f);  // translation > default magMax 1e7
        MatrixScanParams p;  // magMax = 1e7
        auto r = scoreMatrixWindow(reinterpret_cast<const uint8_t*>(m), p);
        QCOMPARE(r.score, 0);  // gate rejects the over-magnitude translation
        // Raising magMax recovers it.
        p.magMax = 1e10f;
        auto r2 = scoreMatrixWindow(reinterpret_cast<const uint8_t*>(m), p);
        QCOMPARE(r2.score, 100);
    }

    void predicate_scaleMatrix_affineButNotOrthonormal() {
        // 2x scale on the diagonal: affine signature present, rotation block not unit-length.
        float m[16] = {
            2,0,0,0,
            0,2,0,0,
            0,0,2,0,
            1,2,3,1
        };
        MatrixScanParams p;
        auto r = scoreMatrixWindow(reinterpret_cast<const uint8_t*>(m), p);
        QVERIFY(r.affine);
        QVERIFY(!r.orthonormal);
        QVERIFY(r.score >= 60);          // passes default minScore (affine)
        // With requireOrthonormal it's capped below minScore.
        p.requireOrthonormal = true;
        auto r2 = scoreMatrixWindow(reinterpret_cast<const uint8_t*>(m), p);
        QVERIFY(r2.score < p.minScore);
    }

    void predicate_inRangeFloatsNoSignature_rejected() {
        // 16 small plausible floats but no (0,0,0,1) anywhere — like a vertex/normal block.
        float m[16];
        for (int i = 0; i < 16; ++i) m[i] = 0.3f + 0.01f * i;
        MatrixScanParams p;
        auto r = scoreMatrixWindow(reinterpret_cast<const uint8_t*>(m), p);
        QVERIFY(!r.affine);
        QVERIFY(r.score < p.minScore);   // capped at 30 by requireAffine
    }

    // ── Engine end-to-end matrix scan ──

    void engine_findsPlantedMatrix_topRanked() {
        // 1 MB of 0xCD: every 4 bytes is -4.3e8 (out of magMax range) -> all windows
        // rejected by the float gate, so the only survivor is the planted matrix.
        QByteArray data(1 * 1024 * 1024, (char)0xCD);
        float m[16]; rotZ90(m, 11, 22, 33);
        const int off = 0x4000;  // 16 KB, 4-aligned
        putMatrix(data, off, m);
        auto prov = std::make_shared<BufferProvider>(data, "target");

        ScanEngine engine;
        QSignalSpy fin(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.matrixScan = true;
        req.alignment = 4;
        engine.start(prov, req);
        QVERIFY(fin.wait(15000));

        auto results = fin.first().first().value<QVector<ScanResult>>();
        QVERIFY(!results.isEmpty());
        QCOMPARE(results[0].address, (uint64_t)off);
        QCOMPARE(results[0].matchScore, 100);
    }

    void engine_ranksMultipleMatricesByScore() {
        QByteArray data(512 * 1024, (char)0xCD);
        float perfect[16];  rotZ90(perfect, 1, 2, 3);                 // score 100
        float scaled[16] = { 2,0,0,0, 0,2,0,0, 0,0,2,0, 4,5,6,1 };    // affine, not orthonormal (~80)
        putMatrix(data, 0x1000, scaled);
        putMatrix(data, 0x8000, perfect);
        auto prov = std::make_shared<BufferProvider>(data, "target");

        ScanEngine engine;
        QSignalSpy fin(&engine, &ScanEngine::finished);
        ScanRequest req; req.matrixScan = true; req.alignment = 4;
        engine.start(prov, req);
        QVERIFY(fin.wait(15000));

        auto results = fin.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
        // Best-first: the orthonormal one outranks the scale matrix.
        QCOMPARE(results[0].address, (uint64_t)0x8000);
        QCOMPARE(results[0].matchScore, 100);
        QVERIFY(results[1].matchScore < 100);
        QVERIFY(results[1].matchScore >= 60);
    }

    void engine_matrixStraddlingChunkBoundary() {
        // 2 MB chunk boundary lives at 0x200000. Place the 64-byte matrix so it
        // spans it; the 63-byte chunk overlap must still catch it.
        QByteArray data(3 * 1024 * 1024, (char)0xCD);
        float m[16]; identityWithTranslation(m, 7, 8, 9);
        const int off = 0x200000 - 32;  // straddles the boundary, 4-aligned
        putMatrix(data, off, m);
        auto prov = std::make_shared<BufferProvider>(data, "target");

        ScanEngine engine;
        QSignalSpy fin(&engine, &ScanEngine::finished);
        ScanRequest req; req.matrixScan = true; req.alignment = 4;
        engine.start(prov, req);
        QVERIFY(fin.wait(20000));

        auto results = fin.first().first().value<QVector<ScanResult>>();
        bool found = false;
        for (const auto& r : results) if (r.address == (uint64_t)off) found = true;
        QVERIFY2(found, "matrix straddling the 2MB chunk boundary was not found");
    }

    // ── The MCP feature core: capture floats -> mutate (camera move) -> rescan Changed ──

    void narrowing_rescanChangedIsolatesMovedValue() {
        // 4 KB buffer of distinct floats at every 4-byte slot.
        QByteArray data(4096, Qt::Uninitialized);
        for (int i = 0; i < data.size(); i += 4) putFloat(data, i, 1.0f + (float)i);
        auto prov = std::make_shared<BufferProvider>(data, "target");

        ScanEngine engine;

        // First scan: UnknownValue float capture (what scanner.scan condition=unknown does).
        QSignalSpy fin(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.condition = ScanCondition::UnknownValue;
        req.valueType = ValueType::Float;
        req.valueSize = 4;
        req.alignment = 4;
        req.maxResults = 10000000;
        engine.start(prov, req);
        QVERIFY(fin.wait(10000));
        auto captured = fin.first().first().value<QVector<ScanResult>>();
        QCOMPARE(captured.size(), 1024);  // 4096 / 4

        // "Move the camera": change exactly one float in target memory.
        const uint64_t movedAddr = 0x800;
        putFloat(data, (int)movedAddr, 999.5f);
        prov->write(movedAddr, data.constData() + movedAddr, 4);  // ensure provider sees it

        // Rescan Changed (what scanner.rescan condition=changed does).
        QSignalSpy rfin(&engine, &ScanEngine::rescanFinished);
        engine.startRescan(prov, captured, 4, ScanCondition::Changed, ValueType::Float);
        QVERIFY(rfin.wait(10000));
        auto changed = rfin.first().first().value<QVector<ScanResult>>();

        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed[0].address, movedAddr);
    }

    void narrowing_matrixRescanDetectsTranslationChange() {
        // find_matrix -> rescan-changed must catch a translation-only camera
        // move. The matrix candidate carries a 64-byte window, so the rescan
        // re-reads all 64 bytes (readSize=64); a change at byte 48 (translation)
        // is detected even though the rotation block (bytes 0-47) is unchanged.
        QByteArray data(4096, (char)0xCD);
        float m[16]; rotZ90(m, 10, 20, 30);
        const int off = 0x400;
        putMatrix(data, off, m);
        auto prov = std::make_shared<BufferProvider>(data, "t");

        ScanEngine engine;
        QSignalSpy fin(&engine, &ScanEngine::finished);
        ScanRequest req; req.matrixScan = true; req.alignment = 4;
        engine.start(prov, req);
        QVERIFY(fin.wait(10000));
        auto found = fin.first().first().value<QVector<ScanResult>>();
        QVERIFY(!found.isEmpty());
        QCOMPARE(found[0].scanValue.size(), 64);

        // "Move the camera": change ONLY the translation (m[12] at byte off+48).
        putFloat(data, off + 48, 999.0f);
        prov->write(off + 48, data.constData() + off + 48, 4);

        // Rescan Changed re-reading all 64 bytes with a byte-wise compare
        // (HexBytes -> memcmp) detects it. This mirrors what runRescanAndWait
        // does for matrix candidates. (A typed Float compare would only look at
        // the first 4 bytes and miss the byte-48 translation change.)
        QSignalSpy rfin(&engine, &ScanEngine::rescanFinished);
        engine.startRescan(prov, found, 64, ScanCondition::Changed, ValueType::HexBytes);
        QVERIFY(rfin.wait(10000));
        auto changed = rfin.first().first().value<QVector<ScanResult>>();
        bool detected = false;
        for (const auto& r : changed) if (r.address == (uint64_t)off) detected = true;
        QVERIFY2(detected, "64-byte rescan missed a translation-only matrix change");
    }

    void narrowing_rescanUnchangedComplement() {
        QByteArray data(256, Qt::Uninitialized);
        for (int i = 0; i < data.size(); i += 4) putFloat(data, i, 2.0f + (float)i);
        auto prov = std::make_shared<BufferProvider>(data, "target");

        ScanEngine engine;
        QSignalSpy fin(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.condition = ScanCondition::UnknownValue;
        req.valueType = ValueType::Float; req.valueSize = 4; req.alignment = 4;
        req.maxResults = 10000000;
        engine.start(prov, req);
        QVERIFY(fin.wait(10000));
        auto captured = fin.first().first().value<QVector<ScanResult>>();
        const int total = captured.size();
        QCOMPARE(total, 64);

        // Change two values, then Unchanged should keep total-2.
        putFloat(data, 0x10, -1.0f); prov->write(0x10, data.constData() + 0x10, 4);
        putFloat(data, 0x20, -2.0f); prov->write(0x20, data.constData() + 0x20, 4);

        QSignalSpy rfin(&engine, &ScanEngine::rescanFinished);
        engine.startRescan(prov, captured, 4, ScanCondition::Unchanged, ValueType::Float);
        QVERIFY(rfin.wait(10000));
        auto unchanged = rfin.first().first().value<QVector<ScanResult>>();
        QCOMPARE(unchanged.size(), total - 2);
        for (const auto& r : unchanged) {
            QVERIFY(r.address != (uint64_t)0x10);
            QVERIFY(r.address != (uint64_t)0x20);
        }
    }
};

QTEST_MAIN(TestMatrixScan)
#include "test_matrixscan.moc"
