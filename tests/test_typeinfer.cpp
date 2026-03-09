#include <QtTest/QTest>
#include <cstring>
#include "typeinfer.h"

using namespace rcx;

class TestTypeInfer : public QObject {
    Q_OBJECT
private slots:

    // ── NULL / zero → empty ──

    void nullPtr() {
        QVERIFY(inferTypes(nullptr, 8).isEmpty());
    }
    void zeroLen() {
        uint8_t d[4] = {};
        QVERIFY(inferTypes(d, 0).isEmpty());
    }
    void allZeros8() {
        uint8_t d[8] = {};
        QVERIFY(inferTypes(d, 8).isEmpty());
    }
    void allZeros4() {
        uint8_t d[4] = {};
        QVERIFY(inferTypes(d, 4).isEmpty());
    }
    void allZeros2() {
        uint8_t d[2] = {};
        QVERIFY(inferTypes(d, 2).isEmpty());
    }

    // ── Hex64: float pair ──
    // {21.0488f, 547.3f} — two clear floats with fractional parts;
    // whole-width Double/Ptr64 score poorly → Float×2 dominates
    void hex64_floatPair() {
        float a = 21.0488f, b = 547.3f;
        uint8_t d[8];
        std::memcpy(d, &a, 4);
        std::memcpy(d + 4, &b, 4);
        auto r = inferTypes(d, 8);
        QVERIFY(!r.isEmpty());
        auto& top = r[0];
        QCOMPARE(top.kinds.size(), 2);
        QCOMPARE(top.kinds[0], NodeKind::Float);
        QVERIFY(top.strength >= 3); // strong
    }

    // ── Hex64: int32 pair ──
    // {42, 99} — two small integers
    void hex64_intPair() {
        int32_t a = 42, b = 99;
        uint8_t d[8];
        std::memcpy(d, &a, 4);
        std::memcpy(d + 4, &b, 4);
        auto r = inferTypes(d, 8);
        QVERIFY(!r.isEmpty());
        auto& top = r[0];
        QVERIFY(top.kinds.size() == 2);
        QVERIFY(top.kinds[0] == NodeKind::Int32 || top.kinds[0] == NodeKind::UInt32);
    }

    // ── Hex64: UTF-8 string ──
    void hex64_utf8() {
        uint8_t d[8] = {'I', 'C', 'h', 'o', 'o', 's', 'e', 'Y'};
        auto r = inferTypes(d, 8);
        QVERIFY(!r.isEmpty());
        // Top should be UTF8 (strong)
        bool foundUtf8 = false;
        for (const auto& s : r) {
            if (s.kinds.size() == 1 && s.kinds[0] == NodeKind::UTF8) {
                foundUtf8 = true;
                QVERIFY(s.strength >= 3); // strong
            }
        }
        QVERIFY(foundUtf8);
    }

    // ── Hex64: pointer-like value ──
    void hex64_pointer() {
        // 0x00007FF6A0B01000 — typical Windows user-mode address
        uint8_t d[8] = {0x00, 0x10, 0xB0, 0xA0, 0xF6, 0x7F, 0x00, 0x00};
        auto r = inferTypes(d, 8);
        QVERIFY(!r.isEmpty());
        bool foundPtr = false;
        for (const auto& s : r)
            if (s.kinds.size() == 1 && s.kinds[0] == NodeKind::Pointer64)
                foundPtr = true;
        QVERIFY(foundPtr);
    }

    // ── Hex32: clear float ──
    void hex32_float() {
        // 21.0488f = 0x41A86600
        uint8_t d[4] = {0x00, 0x66, 0xA8, 0x41};
        auto r = inferTypes(d, 4);
        QVERIFY(!r.isEmpty());
        QCOMPARE(r[0].kinds.size(), 1);
        QCOMPARE(r[0].kinds[0], NodeKind::Float);
        QVERIFY(r[0].strength >= 2);
    }

    // ── Hex32: small integer with monotonic history ──
    void hex32_int_monotonic() {
        // Value: 0x0000BFFC = 49148 (signed: 49148)
        uint8_t d[4] = {0xFC, 0xBF, 0x00, 0x00};
        InferHints h;
        h.monotonic = true;
        h.sampleCount = 10;
        uint8_t minB[4] = {0x10, 0x00, 0x00, 0x00}; // 16
        uint8_t maxB[4] = {0xFC, 0xBF, 0x00, 0x00}; // 49148
        h.minObserved = minB;
        h.maxObserved = maxB;
        auto r = inferTypes(d, 4, h);
        QVERIFY(!r.isEmpty());
        QVERIFY(r[0].kinds[0] == NodeKind::Int32 || r[0].kinds[0] == NodeKind::UInt32);
        QVERIFY(r[0].strength >= 2);
    }

    // ── Hex16: small unsigned ──
    void hex16_uint() {
        uint8_t d[2] = {0x5F, 0x00}; // 95
        auto r = inferTypes(d, 2);
        QVERIFY(!r.isEmpty());
        QVERIFY(r[0].kinds[0] == NodeKind::Int16 || r[0].kinds[0] == NodeKind::UInt16);
    }

    // ── Hex8: uint8 ──
    void hex8_uint() {
        uint8_t d[1] = {1};
        auto r = inferTypes(d, 1);
        QVERIFY(!r.isEmpty());
        QCOMPARE(r[0].kinds[0], NodeKind::UInt8);
    }

    // ── formatHint ──
    void formatHint_single() {
        TypeSuggestion s;
        s.kinds = {NodeKind::Float};
        s.strength = 3;
        QCOMPARE(formatHint(s), QStringLiteral("float"));
    }
    void formatHint_split() {
        TypeSuggestion s;
        s.kinds = {NodeKind::Float, NodeKind::Float};
        s.strength = 3;
        QString h = formatHint(s);
        QCOMPARE(h, QStringLiteral("float\u00D72"));
    }

    // ── Denormal rejection ──
    void denormalRejected() {
        // Denormal float: exp=0, mantissa non-zero → 0x00000001
        uint8_t d[4] = {0x01, 0x00, 0x00, 0x00};
        auto r = inferTypes(d, 4);
        // Should NOT suggest Float as top pick
        if (!r.isEmpty() && r[0].kinds.size() == 1)
            QVERIFY(r[0].kinds[0] != NodeKind::Float);
    }

    // ── Benchmark: single call ──
    void bench_singleCall() {
        uint8_t d[8] = {0x00, 0x00, 0x80, 0x3F, 0xCD, 0xCC, 0x4C, 0x3E};
        QBENCHMARK {
            inferTypes(d, 8);
        }
    }

    // ── Benchmark: 200-node batch (simulates one refresh) ──
    void bench_batchRefresh() {
        // Prepare 200 varied byte patterns
        QVector<std::array<uint8_t, 8>> data(200);
        for (int i = 0; i < 200; ++i) {
            uint32_t seed = (uint32_t)(i * 7919 + 1);
            for (int j = 0; j < 8; ++j)
                data[i][j] = (uint8_t)((seed >> (j * 3)) ^ (i + j));
        }
        QBENCHMARK {
            for (int i = 0; i < 200; ++i)
                inferTypes(data[i].data(), 8);
        }
    }
};

QTEST_MAIN(TestTypeInfer)
#include "test_typeinfer.moc"
