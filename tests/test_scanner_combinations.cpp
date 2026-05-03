// ─────────────────────────────────────────────────────────────────────
//  test_scanner_combinations
//
//  Pure-engine combinatorial sweep. Exercises every tuple of
//   (scan condition, value type, alignment, executable/writable region
//    type, address range)
//  that the ScannerPanel UI can produce, asserting the engine returns
//  the expected result count against a synthetic provider.
//
//  No QtWidgets, no QApplication, no popups — runs entirely on
//  QCoreApplication via QTEST_MAIN, so it doesn't flash a window.
//  Catches regressions when ScanRequest fields drift apart from the
//  engine's interpretation, and validates that every Fast Scan
//  alignment value (1, 4, 8, 16, 32, 64) actually finds the right
//  number of hits given a deterministic memory layout.
// ─────────────────────────────────────────────────────────────────────

#include <QTest>
#include <QEventLoop>
#include <QTimer>
#include <QByteArray>
#include <QCoreApplication>
#include <cstring>
#include "scanner.h"
#include "providers/provider.h"
#include "providers/buffer_provider.h"

using namespace rcx;

// ── Synthetic provider with one configurable region ────────────────────
class SyntheticProvider : public BufferProvider {
    QVector<MemoryRegion> m_regions;
public:
    SyntheticProvider(QByteArray data, QVector<MemoryRegion> regions)
        : BufferProvider(std::move(data), "synthetic")
        , m_regions(std::move(regions)) {}
    QVector<MemoryRegion> enumerateRegions() const override { return m_regions; }
};

class TestScannerCombinations : public QObject {
    Q_OBJECT

    // Synchronous wrapper around the async engine. Bounded so a hung
    // scan fails fast.
    QVector<ScanResult> sync(std::shared_ptr<Provider> prov,
                              const ScanRequest& req,
                              int timeoutMs = 5000) {
        ScanEngine eng;
        QVector<ScanResult> out;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&eng, &ScanEngine::finished, &loop,
            [&](QVector<ScanResult> r) { out = std::move(r); loop.quit(); });
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(timeoutMs);
        eng.start(prov, req);
        loop.exec();
        return out;
    }

private slots:

    // ═══════════════════════════════════════════════════════════════════
    // Fast Scan alignment dropdown — every value the UI exposes
    // ═══════════════════════════════════════════════════════════════════

    void fastScan_alignmentValues_data() {
        QTest::addColumn<int>("alignment");
        QTest::addColumn<int>("expectedHits");
        // Buffer: 256 bytes, value 0xCAFEBABE planted at offsets 0, 4, 8,
        // 12, ... 252 (every 4 bytes). With a 4-byte aligned scan, every
        // hit lands. With 8-byte align, every other hit. Etc.
        // Total slots:
        //   align 1 → all 64 (every 4-byte slot is also 1-byte aligned)
        //   align 4 → 64
        //   align 8 → 32
        //   align 16 → 16
        //   align 32 → 8
        //   align 64 → 4
        QTest::newRow("1")  << 1  << 64;
        QTest::newRow("4")  << 4  << 64;
        QTest::newRow("8")  << 8  << 32;
        QTest::newRow("16") << 16 << 16;
        QTest::newRow("32") << 32 << 8;
        QTest::newRow("64") << 64 << 4;
    }
    void fastScan_alignmentValues() {
        QFETCH(int, alignment);
        QFETCH(int, expectedHits);

        QByteArray data(256, 0);
        const uint32_t needle = 0xCAFEBABE;
        for (int off = 0; off + 4 <= 256; off += 4)
            memcpy(data.data() + off, &needle, 4);
        QVector<MemoryRegion> regs;
        regs.push_back(MemoryRegion{0, 256, true, true, false, "", RegionType::Private});
        auto prov = std::make_shared<SyntheticProvider>(data, regs);

        ScanRequest req;
        QString err;
        QVERIFY(serializeValue(ValueType::UInt32, "0xCAFEBABE",
                                req.pattern, req.mask, &err));
        req.alignment = alignment;
        req.condition = ScanCondition::ExactValue;
        req.valueType = ValueType::UInt32;
        req.valueSize = 4;

        auto results = sync(prov, req);
        QCOMPARE(results.size(), expectedHits);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Scan-condition × value-type matrix — every CE-style condition
    // against every supported numeric value type, against a buffer
    // pre-populated with deterministic values.
    // ═══════════════════════════════════════════════════════════════════

    void condition_valueType_matrix_data() {
        QTest::addColumn<int>("vt");
        QTest::addColumn<int>("cond");
        QTest::addColumn<QString>("value");
        QTest::addColumn<int>("expected");

        // For each value type, plant one slot of value 100, two of 50.
        // ExactValue 100 → 1, ExactValue 50 → 2,
        // BiggerThan 25 → 3, SmallerThan 25 → 0,
        // Between 40..60 → 2 (50,50).
        struct Vt { ValueType t; int sz; const char* name; };
        const Vt vts[] = {
            {ValueType::Int8,   1, "i8"},
            {ValueType::Int16,  2, "i16"},
            {ValueType::Int32,  4, "i32"},
            {ValueType::Int64,  8, "i64"},
            {ValueType::UInt8,  1, "u8"},
            {ValueType::UInt16, 2, "u16"},
            {ValueType::UInt32, 4, "u32"},
            {ValueType::UInt64, 8, "u64"},
        };
        struct Case { ScanCondition c; const char* v; int exp; const char* tag; };
        const Case cases[] = {
            {ScanCondition::ExactValue,  "100", 1, "exact100"},
            {ScanCondition::ExactValue,  "50",  2, "exact50"},
            {ScanCondition::BiggerThan,  "25",  3, "bigger25"},
            {ScanCondition::SmallerThan, "25",  0, "smaller25"},
            {ScanCondition::SmallerThan, "75",  2, "smaller75"},
        };
        for (const auto& vt : vts) {
            for (const auto& c : cases) {
                QTest::newRow(qPrintable(QString("%1_%2").arg(vt.name, c.tag)))
                    << (int)vt.t << (int)c.c << QString::fromLatin1(c.v) << c.exp;
            }
        }
    }
    void condition_valueType_matrix() {
        QFETCH(int, vt);
        QFETCH(int, cond);
        QFETCH(QString, value);
        QFETCH(int, expected);

        ValueType t = (ValueType)vt;
        int sz = valueSizeForType(t);
        // Buffer: 3 slots of the type, each at slot-aligned offsets.
        // Layout: [100][50][50] then 16 zero bytes of pad to absorb stride.
        QByteArray data(qMax(3 * sz + 16, 32), 0);
        auto write = [&](int off, int64_t v) {
            switch (t) {
            case ValueType::Int8:   { int8_t   x = (int8_t)v;   memcpy(data.data() + off, &x, 1); break; }
            case ValueType::UInt8:  { uint8_t  x = (uint8_t)v;  memcpy(data.data() + off, &x, 1); break; }
            case ValueType::Int16:  { int16_t  x = (int16_t)v;  memcpy(data.data() + off, &x, 2); break; }
            case ValueType::UInt16: { uint16_t x = (uint16_t)v; memcpy(data.data() + off, &x, 2); break; }
            case ValueType::Int32:  { int32_t  x = (int32_t)v;  memcpy(data.data() + off, &x, 4); break; }
            case ValueType::UInt32: { uint32_t x = (uint32_t)v; memcpy(data.data() + off, &x, 4); break; }
            case ValueType::Int64:  { int64_t  x = (int64_t)v;  memcpy(data.data() + off, &x, 8); break; }
            case ValueType::UInt64: { uint64_t x = (uint64_t)v; memcpy(data.data() + off, &x, 8); break; }
            default: break;
            }
        };
        write(0,        100);
        write(sz,       50);
        write(2 * sz,   50);

        QVector<MemoryRegion> regs;
        regs.push_back(MemoryRegion{0, (uint64_t)data.size(), true, true, false,
                                     "", RegionType::Private});
        auto prov = std::make_shared<SyntheticProvider>(data, regs);

        ScanRequest req;
        QString err;
        QVERIFY(serializeValue(t, value, req.pattern, req.mask, &err));
        req.alignment = sz;
        req.condition = (ScanCondition)cond;
        req.valueType = t;
        req.valueSize = sz;
        // Restrict to the populated region so trailing zero pad doesn't
        // pollute the count for "smaller than" comparisons.
        req.endAddress = 3 * sz;

        auto results = sync(prov, req);
        QCOMPARE(results.size(), expected);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Between condition matrix
    // ═══════════════════════════════════════════════════════════════════

    void condition_between_data() {
        QTest::addColumn<QString>("lo");
        QTest::addColumn<QString>("hi");
        QTest::addColumn<int>("expected");
        // Buffer holds values 10, 50, 100, 200, 300 at int32 offsets.
        QTest::newRow("0..1000")    << "0"   << "1000" << 5;
        QTest::newRow("50..200")    << "50"  << "200"  << 3;  // 50, 100, 200
        QTest::newRow("11..99")     << "11"  << "99"   << 1;  // 50
        QTest::newRow("301..999")   << "301" << "999"  << 0;
        QTest::newRow("100..100")   << "100" << "100"  << 1;  // exact-Between
    }
    void condition_between() {
        QFETCH(QString, lo);
        QFETCH(QString, hi);
        QFETCH(int, expected);

        QByteArray data(20, 0);
        int32_t vs[5] = {10, 50, 100, 200, 300};
        for (int i = 0; i < 5; i++) memcpy(data.data() + i * 4, &vs[i], 4);
        QVector<MemoryRegion> regs;
        regs.push_back(MemoryRegion{0, 20, true, true, false, "", RegionType::Private});
        auto prov = std::make_shared<SyntheticProvider>(data, regs);

        ScanRequest req;
        QString err, err2;
        QByteArray dummy;
        QVERIFY(serializeValue(ValueType::Int32, lo, req.pattern,  dummy, &err));
        QVERIFY(serializeValue(ValueType::Int32, hi, req.pattern2, dummy, &err2));
        req.mask.fill('\xFF', req.pattern.size());
        req.alignment = 4;
        req.condition = ScanCondition::Between;
        req.valueType = ValueType::Int32;
        req.valueSize = 4;

        auto results = sync(prov, req);
        QCOMPARE(results.size(), expected);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Region-filter combinations (exec / write / both / neither)
    // ═══════════════════════════════════════════════════════════════════

    void regionFilter_combos_data() {
        QTest::addColumn<bool>("filterExec");
        QTest::addColumn<bool>("filterWrite");
        QTest::addColumn<int>("expected");
        // Three regions: rwx (R+W+X), rw- (R+W), r-x (R+X). Plant the
        // value 0xC0DE in each (4-byte int32, 4-byte aligned). All three
        // are MEM_PRIVATE so the type filter doesn't matter.
        QTest::newRow("none-none") << false << false << 3;  // all 3
        QTest::newRow("exec-only") << true  << false << 2;  // rwx + r-x
        QTest::newRow("write-only")<< false << true  << 2;  // rwx + rw-
        QTest::newRow("both")      << true  << true  << 1;  // only rwx
    }
    void regionFilter_combos() {
        QFETCH(bool, filterExec);
        QFETCH(bool, filterWrite);
        QFETCH(int, expected);

        QByteArray data(48, 0);
        int32_t needle = 0xC0DE;
        memcpy(data.data() + 0,  &needle, 4);    // rwx
        memcpy(data.data() + 16, &needle, 4);    // rw-
        memcpy(data.data() + 32, &needle, 4);    // r-x
        QVector<MemoryRegion> regs;
        regs.push_back(MemoryRegion{0,  16, true, true,  true,  "", RegionType::Private});
        regs.push_back(MemoryRegion{16, 16, true, true,  false, "", RegionType::Private});
        regs.push_back(MemoryRegion{32, 16, true, false, true,  "", RegionType::Private});
        auto prov = std::make_shared<SyntheticProvider>(data, regs);

        ScanRequest req;
        QString err;
        QVERIFY(serializeValue(ValueType::Int32, "49374", req.pattern, req.mask, &err));
        req.alignment = 4;
        req.filterExecutable = filterExec;
        req.filterWritable   = filterWrite;
        req.condition = ScanCondition::ExactValue;
        req.valueType = ValueType::Int32;
        req.valueSize = 4;

        auto results = sync(prov, req);
        QCOMPARE(results.size(), expected);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Address-range cap × Fast Scan alignment — verify endAddress
    // intersects correctly across every alignment value.
    // ═══════════════════════════════════════════════════════════════════

    void addressRange_alignment_data() {
        QTest::addColumn<int>("alignment");
        QTest::addColumn<uint64_t>("endAddr");
        QTest::addColumn<int>("expected");
        // 1 KB buffer with int32 0xDEADBEEF planted every 32 bytes
        // → 32 hits in the full range. endAddr clips proportionally.
        QTest::newRow("4_full")    << 4  << uint64_t(1024) << 32;
        QTest::newRow("4_half")    << 4  << uint64_t(512)  << 16;
        QTest::newRow("8_full")    << 8  << uint64_t(1024) << 32;
        QTest::newRow("16_full")   << 16 << uint64_t(1024) << 32;
        QTest::newRow("32_full")   << 32 << uint64_t(1024) << 32;
        QTest::newRow("64_full")   << 64 << uint64_t(1024) << 16;  // every-other
        QTest::newRow("4_quarter") << 4  << uint64_t(256)  << 8;
    }
    void addressRange_alignment() {
        QFETCH(int, alignment);
        QFETCH(uint64_t, endAddr);
        QFETCH(int, expected);

        QByteArray data(1024, 0);
        uint32_t needle = 0xDEADBEEF;
        for (int off = 0; off + 4 <= 1024; off += 32)
            memcpy(data.data() + off, &needle, 4);
        QVector<MemoryRegion> regs;
        regs.push_back(MemoryRegion{0, 1024, true, true, false, "", RegionType::Private});
        auto prov = std::make_shared<SyntheticProvider>(data, regs);

        ScanRequest req;
        QString err;
        QVERIFY(serializeValue(ValueType::UInt32, "0xDEADBEEF",
                                req.pattern, req.mask, &err));
        req.alignment = alignment;
        req.condition = ScanCondition::ExactValue;
        req.valueType = ValueType::UInt32;
        req.valueSize = 4;
        req.endAddress = endAddr;

        auto results = sync(prov, req);
        QCOMPARE(results.size(), expected);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Signature-mode wildcards × pattern alignment
    // ═══════════════════════════════════════════════════════════════════

    void signature_wildcards_data() {
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<int>("expected");
        // Buffer: "AB CC DD EE   AB AA DD EE   AB CC DD AA"
        QTest::newRow("exact")      << "AB CC DD EE"        << 1;
        QTest::newRow("any-second") << "AB ?? DD EE"        << 2;
        QTest::newRow("any-tail")   << "AB ?? DD ??"        << 3;
        QTest::newRow("noMatch")    << "FF FF FF FF"        << 0;
    }
    void signature_wildcards() {
        QFETCH(QString, pattern);
        QFETCH(int, expected);

        QByteArray data(48, 0);
        const uint8_t set1[4] = {0xAB, 0xCC, 0xDD, 0xEE};
        const uint8_t set2[4] = {0xAB, 0xAA, 0xDD, 0xEE};
        const uint8_t set3[4] = {0xAB, 0xCC, 0xDD, 0xAA};
        memcpy(data.data() + 0,  set1, 4);
        memcpy(data.data() + 16, set2, 4);
        memcpy(data.data() + 32, set3, 4);
        QVector<MemoryRegion> regs;
        regs.push_back(MemoryRegion{0, 48, true, false, true, "", RegionType::Private});
        auto prov = std::make_shared<SyntheticProvider>(data, regs);

        ScanRequest req;
        QString err;
        QVERIFY(parseSignature(pattern, req.pattern, req.mask, &err));
        req.alignment = 1;  // signature scans are byte-aligned
        req.condition = ScanCondition::ExactValue;
        auto results = sync(prov, req);
        QCOMPARE(results.size(), expected);
    }
};

QTEST_MAIN(TestScannerCombinations)
#include "test_scanner_combinations.moc"
