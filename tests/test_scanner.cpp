#include <QTest>
#include <QSignalSpy>
#include <QByteArray>
#include <cstring>
#include <cmath>
#include "scanner.h"
#include "providers/provider.h"
#include "providers/buffer_provider.h"
#include "providers/null_provider.h"

using namespace rcx;

// ── Test provider that exposes custom memory regions ──
class RegionProvider : public BufferProvider {
    QVector<MemoryRegion> m_regions;
public:
    RegionProvider(QByteArray data, QVector<MemoryRegion> regions)
        : BufferProvider(std::move(data), "test")
        , m_regions(std::move(regions)) {}

    QVector<MemoryRegion> enumerateRegions() const override { return m_regions; }
};

class TestScanner : public QObject {
    Q_OBJECT

private slots:

    // ═══════════════════════════════════════════════════════════════════
    // Pattern Parsing — Signature mode
    // ═══════════════════════════════════════════════════════════════════

    void parse_emptyPattern() {
        QByteArray pat, mask;
        QString err;
        QVERIFY(!parseSignature("", pat, mask, &err));
        QVERIFY(err.contains("Empty"));
    }

    void parse_spacesOnly() {
        QByteArray pat, mask;
        QString err;
        QVERIFY(!parseSignature("   ", pat, mask, &err));
        QVERIFY(err.contains("Empty"));
    }

    void parse_singleByte() {
        QByteArray pat, mask;
        QVERIFY(parseSignature("AB", pat, mask));
        QCOMPARE(pat.size(), 1);
        QCOMPARE((uint8_t)pat[0], (uint8_t)0xAB);
        QCOMPARE((uint8_t)mask[0], (uint8_t)0xFF);
    }

    void parse_spaceSeparated() {
        QByteArray pat, mask;
        QVERIFY(parseSignature("48 8B 05", pat, mask));
        QCOMPARE(pat.size(), 3);
        QCOMPARE((uint8_t)pat[0], (uint8_t)0x48);
        QCOMPARE((uint8_t)pat[1], (uint8_t)0x8B);
        QCOMPARE((uint8_t)pat[2], (uint8_t)0x05);
        QCOMPARE((uint8_t)mask[0], (uint8_t)0xFF);
        QCOMPARE((uint8_t)mask[1], (uint8_t)0xFF);
        QCOMPARE((uint8_t)mask[2], (uint8_t)0xFF);
    }

    void parse_withWildcards() {
        QByteArray pat, mask;
        QVERIFY(parseSignature("48 ?? 05 ?? ??", pat, mask));
        QCOMPARE(pat.size(), 5);
        QCOMPARE((uint8_t)pat[0], (uint8_t)0x48);
        QCOMPARE((uint8_t)pat[2], (uint8_t)0x05);
        QCOMPARE((uint8_t)mask[0], (uint8_t)0xFF);
        QCOMPARE((uint8_t)mask[1], (uint8_t)0x00); // wildcard
        QCOMPARE((uint8_t)mask[2], (uint8_t)0xFF);
        QCOMPARE((uint8_t)mask[3], (uint8_t)0x00);
        QCOMPARE((uint8_t)mask[4], (uint8_t)0x00);
    }

    void parse_singleQuestionMark() {
        QByteArray pat, mask;
        QVERIFY(parseSignature("48 ? 05", pat, mask));
        QCOMPARE((uint8_t)mask[1], (uint8_t)0x00);
    }

    void parse_packedNoSpaces() {
        QByteArray pat, mask;
        QVERIFY(parseSignature("488B??05CC", pat, mask));
        QCOMPARE(pat.size(), 5);
        QCOMPARE((uint8_t)pat[0], (uint8_t)0x48);
        QCOMPARE((uint8_t)pat[1], (uint8_t)0x8B);
        QCOMPARE((uint8_t)mask[2], (uint8_t)0x00);
        QCOMPARE((uint8_t)pat[3], (uint8_t)0x05);
        QCOMPARE((uint8_t)pat[4], (uint8_t)0xCC);
    }

    void parse_cStyle() {
        QByteArray pat, mask;
        QVERIFY(parseSignature("\\x48\\x8B\\x05", pat, mask));
        QCOMPARE(pat.size(), 3);
        QCOMPARE((uint8_t)pat[0], (uint8_t)0x48);
        QCOMPARE((uint8_t)pat[1], (uint8_t)0x8B);
        QCOMPARE((uint8_t)pat[2], (uint8_t)0x05);
    }

    void parse_lowercaseHex() {
        QByteArray pat, mask;
        QVERIFY(parseSignature("ab cd ef", pat, mask));
        QCOMPARE((uint8_t)pat[0], (uint8_t)0xAB);
        QCOMPARE((uint8_t)pat[1], (uint8_t)0xCD);
        QCOMPARE((uint8_t)pat[2], (uint8_t)0xEF);
    }

    void parse_mixedCase() {
        QByteArray pat, mask;
        QVERIFY(parseSignature("aB Cd eF", pat, mask));
        QCOMPARE((uint8_t)pat[0], (uint8_t)0xAB);
        QCOMPARE((uint8_t)pat[1], (uint8_t)0xCD);
        QCOMPARE((uint8_t)pat[2], (uint8_t)0xEF);
    }

    void parse_invalidHex() {
        QByteArray pat, mask;
        QString err;
        QVERIFY(!parseSignature("GG", pat, mask, &err));
        QVERIFY(!err.isEmpty());
    }

    void parse_oddCharsNoSpaces() {
        QByteArray pat, mask;
        QString err;
        QVERIFY(!parseSignature("ABC", pat, mask, &err));
        QVERIFY(err.contains("Odd"));
    }

    void parse_invalidTokenWidth() {
        QByteArray pat, mask;
        QString err;
        QVERIFY(!parseSignature("48 ABC 05", pat, mask, &err));
        QVERIFY(!err.isEmpty());
    }

    void parse_leadingTrailingSpaces() {
        QByteArray pat, mask;
        QVERIFY(parseSignature("  48 8B  ", pat, mask));
        QCOMPARE(pat.size(), 2);
    }

    void parse_allWildcards() {
        QByteArray pat, mask;
        QVERIFY(parseSignature("?? ?? ??", pat, mask));
        QCOMPARE(pat.size(), 3);
        for (int i = 0; i < 3; i++)
            QCOMPARE((uint8_t)mask[i], (uint8_t)0x00);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Value Serialization
    // ═══════════════════════════════════════════════════════════════════

    void serialize_int8() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Int8, "-42", pat, mask));
        QCOMPARE(pat.size(), 1);
        QCOMPARE((int8_t)pat[0], (int8_t)-42);
        QCOMPARE((uint8_t)mask[0], (uint8_t)0xFF);
    }

    void serialize_int8_overflow() {
        QByteArray pat, mask;
        QString err;
        QVERIFY(!serializeValue(ValueType::Int8, "200", pat, mask, &err));
    }

    void serialize_int16() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Int16, "-1000", pat, mask));
        QCOMPARE(pat.size(), 2);
        int16_t v;
        std::memcpy(&v, pat.constData(), 2);
        QCOMPARE(v, (int16_t)-1000);
    }

    void serialize_int32() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Int32, "12345", pat, mask));
        QCOMPARE(pat.size(), 4);
        int32_t v;
        std::memcpy(&v, pat.constData(), 4);
        QCOMPARE(v, 12345);
    }

    void serialize_int32_negative() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Int32, "-1", pat, mask));
        int32_t v;
        std::memcpy(&v, pat.constData(), 4);
        QCOMPARE(v, -1);
    }

    void serialize_int64() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Int64, "9999999999", pat, mask));
        QCOMPARE(pat.size(), 8);
        int64_t v;
        std::memcpy(&v, pat.constData(), 8);
        QCOMPARE(v, (int64_t)9999999999LL);
    }

    void serialize_uint8() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UInt8, "255", pat, mask));
        QCOMPARE(pat.size(), 1);
        QCOMPARE((uint8_t)pat[0], (uint8_t)255);
    }

    void serialize_uint8_hex() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UInt8, "0xFF", pat, mask));
        QCOMPARE((uint8_t)pat[0], (uint8_t)0xFF);
    }

    void serialize_uint16() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UInt16, "1234", pat, mask));
        QCOMPARE(pat.size(), 2);
        uint16_t v;
        std::memcpy(&v, pat.constData(), 2);
        QCOMPARE(v, (uint16_t)1234);
    }

    void serialize_uint32() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UInt32, "0xDEADBEEF", pat, mask));
        QCOMPARE(pat.size(), 4);
        uint32_t v;
        std::memcpy(&v, pat.constData(), 4);
        QCOMPARE(v, (uint32_t)0xDEADBEEF);
    }

    void serialize_uint64() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UInt64, "0xCAFEBABEDEADBEEF", pat, mask));
        QCOMPARE(pat.size(), 8);
        uint64_t v;
        std::memcpy(&v, pat.constData(), 8);
        QCOMPARE(v, (uint64_t)0xCAFEBABEDEADBEEFULL);
    }

    void serialize_float() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Float, "3.14", pat, mask));
        QCOMPARE(pat.size(), 4);
        float v;
        std::memcpy(&v, pat.constData(), 4);
        QCOMPARE(v, 3.14f);
    }

    void serialize_double() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Double, "2.71828", pat, mask));
        QCOMPARE(pat.size(), 8);
        double v;
        std::memcpy(&v, pat.constData(), 8);
        QCOMPARE(v, 2.71828);
    }

    void serialize_vec2() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Vec2, "1.0 2.0", pat, mask));
        QCOMPARE(pat.size(), 8);
        float v[2];
        std::memcpy(v, pat.constData(), 8);
        QCOMPARE(v[0], 1.0f);
        QCOMPARE(v[1], 2.0f);
    }

    void serialize_vec3() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Vec3, "1.0 0.0 0.0", pat, mask));
        QCOMPARE(pat.size(), 12);
        float v[3];
        std::memcpy(v, pat.constData(), 12);
        QCOMPARE(v[0], 1.0f);
        QCOMPARE(v[1], 0.0f);
        QCOMPARE(v[2], 0.0f);
    }

    void serialize_vec3_wrongCount() {
        QByteArray pat, mask;
        QString err;
        QVERIFY(!serializeValue(ValueType::Vec3, "1.0 2.0", pat, mask, &err));
        QVERIFY(err.contains("3"));
    }

    void serialize_vec4() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Vec4, "1.0 2.0 3.0 4.0", pat, mask));
        QCOMPARE(pat.size(), 16);
        float v[4];
        std::memcpy(v, pat.constData(), 16);
        QCOMPARE(v[0], 1.0f);
        QCOMPARE(v[1], 2.0f);
        QCOMPARE(v[2], 3.0f);
        QCOMPARE(v[3], 4.0f);
    }

    void serialize_utf8() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UTF8, "Hello", pat, mask));
        QCOMPARE(pat, QByteArray("Hello"));
    }

    void serialize_utf16() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UTF16, "Hi", pat, mask));
        QCOMPARE(pat.size(), 4); // 2 chars * 2 bytes
        uint16_t v[2];
        std::memcpy(v, pat.constData(), 4);
        QCOMPARE(v[0], (uint16_t)'H');
        QCOMPARE(v[1], (uint16_t)'i');
    }

    void serialize_hexBytes() {
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::HexBytes, "DE AD BE EF", pat, mask));
        QCOMPARE(pat.size(), 4);
        QCOMPARE((uint8_t)pat[0], (uint8_t)0xDE);
        QCOMPARE((uint8_t)pat[1], (uint8_t)0xAD);
        QCOMPARE((uint8_t)pat[2], (uint8_t)0xBE);
        QCOMPARE((uint8_t)pat[3], (uint8_t)0xEF);
    }

    void serialize_emptyValue() {
        QByteArray pat, mask;
        QString err;
        QVERIFY(!serializeValue(ValueType::Int32, "", pat, mask, &err));
        QVERIFY(err.contains("Empty"));
    }

    void serialize_invalidInt() {
        QByteArray pat, mask;
        QString err;
        QVERIFY(!serializeValue(ValueType::Int32, "notanumber", pat, mask, &err));
    }

    void serialize_invalidFloat() {
        QByteArray pat, mask;
        QString err;
        QVERIFY(!serializeValue(ValueType::Float, "abc", pat, mask, &err));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Natural Alignment
    // ═══════════════════════════════════════════════════════════════════

    void alignment_int8()  { QCOMPARE(naturalAlignment(ValueType::Int8),  1); }
    void alignment_int16() { QCOMPARE(naturalAlignment(ValueType::Int16), 2); }
    void alignment_int32() { QCOMPARE(naturalAlignment(ValueType::Int32), 4); }
    void alignment_int64() { QCOMPARE(naturalAlignment(ValueType::Int64), 8); }
    void alignment_float() { QCOMPARE(naturalAlignment(ValueType::Float), 4); }
    void alignment_double(){ QCOMPARE(naturalAlignment(ValueType::Double),8); }
    void alignment_vec3()  { QCOMPARE(naturalAlignment(ValueType::Vec3),  4); }
    void alignment_utf8()  { QCOMPARE(naturalAlignment(ValueType::UTF8),  1); }
    void alignment_utf16() { QCOMPARE(naturalAlignment(ValueType::UTF16), 2); }

    // ═══════════════════════════════════════════════════════════════════
    // Scan Engine — Basic functionality
    // ═══════════════════════════════════════════════════════════════════

    void scan_exactMatch() {
        // Buffer: 00 11 22 33 44 55 66 77
        QByteArray data(8, '\0');
        data[2] = 0x22; data[3] = 0x33;
        auto prov = std::make_shared<BufferProvider>(data);

        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\x22\x33", 2);
        req.mask = QByteArray("\xFF\xFF", 2);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)2);
    }

    void scan_wildcardMatch() {
        // Buffer with known pattern
        QByteArray data(16, '\0');
        data[0] = 0x48; data[1] = 0x8B; data[2] = 0xAA; data[3] = 0x05;
        data[8] = 0x48; data[9] = 0x8B; data[10] = 0xBB; data[11] = 0x05;

        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        // Pattern: 48 8B ?? 05
        req.pattern = QByteArray("\x48\x8B\x00\x05", 4);
        req.mask    = QByteArray("\xFF\xFF\x00\xFF", 4);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].address, (uint64_t)0);
        QCOMPARE(results[1].address, (uint64_t)8);
    }

    void scan_noMatch() {
        QByteArray data(32, '\0');
        auto prov = std::make_shared<BufferProvider>(data);

        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xFF\xFF", 2);
        req.mask    = QByteArray("\xFF\xFF", 2);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 0);
    }

    void scan_alignment4() {
        // Put pattern at offset 2 (not 4-aligned) and offset 4 (4-aligned)
        QByteArray data(16, '\0');
        data[2] = 0xAA; data[3] = 0xBB;
        data[4] = 0xAA; data[5] = 0xBB;

        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xAA\xBB", 2);
        req.mask    = QByteArray("\xFF\xFF", 2);
        req.alignment = 4;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)4); // only 4-aligned match
    }

    void scan_maxResults() {
        // Fill buffer with pattern every byte
        QByteArray data(1000, '\xAA');
        auto prov = std::make_shared<BufferProvider>(data);

        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xAA", 1);
        req.mask    = QByteArray("\xFF", 1);
        req.maxResults = 10;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 10); // capped at maxResults
    }

    void scan_emptyProvider() {
        auto prov = std::make_shared<NullProvider>();
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xAA", 1);
        req.mask    = QByteArray("\xFF", 1);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 0);
    }

    void scan_emptyPattern() {
        auto prov = std::make_shared<BufferProvider>(QByteArray(16, '\0'));
        ScanEngine engine;
        QSignalSpy errSpy(&engine, &ScanEngine::error);

        ScanRequest req;
        // Empty pattern — error emitted synchronously
        engine.start(prov, req);
        QCOMPARE(errSpy.size(), 1);
    }

    void scan_chunkBoundaryOverlap() {
        // Create a buffer where the pattern straddles a chunk boundary
        // Use a small-ish buffer and simulate by creating a pattern
        // that sits at a position that would be at the overlap zone
        const int kChunkSize = 256 * 1024;
        QByteArray data(kChunkSize + 16, '\0');

        // Place pattern right at chunk boundary
        int pos = kChunkSize - 2; // pattern starts 2 bytes before boundary
        data[pos]     = 0xDE;
        data[pos + 1] = 0xAD;
        data[pos + 2] = 0xBE;
        data[pos + 3] = 0xEF;

        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xDE\xAD\xBE\xEF", 4);
        req.mask    = QByteArray("\xFF\xFF\xFF\xFF", 4);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(10000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)pos);
    }

    void scan_multipleMatches() {
        QByteArray data(64, '\0');
        // Place pattern at offsets 0, 16, 32, 48
        for (int i = 0; i < 4; i++) {
            data[i * 16]     = 0xCA;
            data[i * 16 + 1] = 0xFE;
        }

        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xCA\xFE", 2);
        req.mask    = QByteArray("\xFF\xFF", 2);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 4);
        for (int i = 0; i < 4; i++)
            QCOMPARE(results[i].address, (uint64_t)(i * 16));
    }

    void scan_singleBytePattern() {
        QByteArray data(8, '\0');
        data[5] = 0x42;

        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\x42", 1);
        req.mask    = QByteArray("\xFF", 1);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)5);
    }

    void scan_patternLargerThanData() {
        QByteArray data(4, '\xAA');
        auto prov = std::make_shared<BufferProvider>(data);

        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray(8, '\xAA');
        req.mask    = QByteArray(8, '\xFF');

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 0);
    }

    void scan_patternExactSize() {
        QByteArray data("\xDE\xAD\xBE\xEF", 4);
        auto prov = std::make_shared<BufferProvider>(data);

        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xDE\xAD\xBE\xEF", 4);
        req.mask    = QByteArray("\xFF\xFF\xFF\xFF", 4);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)0);
    }

    void scan_atEndOfBuffer() {
        QByteArray data(32, '\0');
        data[30] = 0xAB;
        data[31] = 0xCD;

        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xAB\xCD", 2);
        req.mask    = QByteArray("\xFF\xFF", 2);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)30);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Scan Engine — Region filtering
    // ═══════════════════════════════════════════════════════════════════

    void scan_filterExecutable() {
        QByteArray data(32, '\0');
        data[0]  = 0xAA; // in region 0 (not executable)
        data[16] = 0xAA; // in region 1 (executable)

        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0,  16, true, true, false, "heap"});
        regions.push_back(MemoryRegion{16, 16, true, false, true, "code"});

        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xAA", 1);
        req.mask    = QByteArray("\xFF", 1);
        req.filterExecutable = true;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)16);
        QCOMPARE(results[0].regionModule, QStringLiteral("code"));
    }

    void scan_filterWritable() {
        QByteArray data(32, '\0');
        data[0]  = 0xBB; // region 0 (writable)
        data[16] = 0xBB; // region 1 (not writable)

        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0,  16, true, true, false, "data"});
        regions.push_back(MemoryRegion{16, 16, true, false, true, "code"});

        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xBB", 1);
        req.mask    = QByteArray("\xFF", 1);
        req.filterWritable = true;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)0);
    }

    void scan_bothFilters() {
        QByteArray data(48, '\0');
        data[0]  = 0xCC; // region 0: +w -x
        data[16] = 0xCC; // region 1: -w +x
        data[32] = 0xCC; // region 2: +w +x

        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0,  16, true, true,  false, "data"});
        regions.push_back(MemoryRegion{16, 16, true, false, true,  "code"});
        regions.push_back(MemoryRegion{32, 16, true, true,  true,  "rwx"});

        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xCC", 1);
        req.mask    = QByteArray("\xFF", 1);
        req.filterExecutable = true;
        req.filterWritable = true;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        // Both filters: region must be BOTH executable AND writable
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)32);
    }

    void scan_regionModuleName() {
        QByteArray data(16, '\0');
        data[0] = 0xDD;

        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 16, true, true, true, "Game.exe"});

        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xDD", 1);
        req.mask    = QByteArray("\xFF", 1);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].regionModule, QStringLiteral("Game.exe"));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Scan Engine — Abort
    // ═══════════════════════════════════════════════════════════════════

    void scan_abort() {
        // Large buffer to ensure scan takes measurable time
        QByteArray data(1024 * 1024, '\0'); // 1MB
        auto prov = std::make_shared<BufferProvider>(data);

        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xFF", 1);
        req.mask    = QByteArray("\xFF", 1);

        engine.start(prov, req);
        QVERIFY(engine.isRunning());

        engine.abort();
        QVERIFY(finSpy.wait(5000));

        // Should complete (possibly with 0 results since buffer is all zeros anyway)
        QCOMPARE(finSpy.size(), 1);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Scan Engine — Progress signal
    // ═══════════════════════════════════════════════════════════════════

    void scan_progressEmitted() {
        QByteArray data(512 * 1024, '\0'); // 512KB
        auto prov = std::make_shared<BufferProvider>(data);

        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        QSignalSpy progSpy(&engine, &ScanEngine::progress);

        ScanRequest req;
        req.pattern = QByteArray("\xFF", 1);
        req.mask    = QByteArray("\xFF", 1);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        // Should have at least one progress signal
        QVERIFY(progSpy.size() > 0);

        // Last progress should be near 100
        int lastPct = progSpy.last().first().toInt();
        QVERIFY(lastPct >= 50); // at least past halfway
    }

    // ═══════════════════════════════════════════════════════════════════
    // Scan Engine — isRunning
    // ═══════════════════════════════════════════════════════════════════

    void scan_isRunning() {
        ScanEngine engine;
        QVERIFY(!engine.isRunning());

        QByteArray data(256 * 1024, '\0');
        auto prov = std::make_shared<BufferProvider>(data);
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xFF", 1);
        req.mask    = QByteArray("\xFF", 1);

        engine.start(prov, req);
        // May or may not be running depending on thread scheduling
        // Just verify it completes
        QVERIFY(finSpy.wait(5000));
        QVERIFY(!engine.isRunning());
    }

    // ═══════════════════════════════════════════════════════════════════
    // Scan Engine — Value scan integration
    // ═══════════════════════════════════════════════════════════════════

    void scan_findInt32Value() {
        QByteArray data(64, '\0');
        int32_t target = 12345;
        std::memcpy(data.data() + 20, &target, 4);

        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Int32, "12345", pat, mask));

        ScanRequest req;
        req.pattern = pat;
        req.mask = mask;
        req.alignment = 4;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)20);
    }

    void scan_findFloatValue() {
        QByteArray data(64, '\0');
        float target = 3.14f;
        std::memcpy(data.data() + 8, &target, 4);

        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Float, "3.14", pat, mask));

        ScanRequest req;
        req.pattern = pat;
        req.mask = mask;
        req.alignment = 4;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)8);
    }

    void scan_findUtf16String() {
        QByteArray data(128, '\0');
        // Write "Hi" in UTF-16LE at offset 32
        uint16_t chars[] = { 'H', 'i' };
        std::memcpy(data.data() + 32, chars, 4);

        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UTF16, "Hi", pat, mask));

        ScanRequest req;
        req.pattern = pat;
        req.mask = mask;
        req.alignment = 2;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)32);
    }

    void scan_findVec3() {
        QByteArray data(64, '\0');
        float v[] = { 1.0f, 0.0f, 0.0f };
        std::memcpy(data.data() + 12, v, 12);

        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Vec3, "1.0 0.0 0.0", pat, mask));

        ScanRequest req;
        req.pattern = pat;
        req.mask = mask;
        req.alignment = 4;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)12);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Provider — enumerateRegions default
    // ═══════════════════════════════════════════════════════════════════

    void provider_defaultRegionsEmpty() {
        BufferProvider p(QByteArray(16, '\0'));
        auto regions = p.enumerateRegions();
        QVERIFY(regions.isEmpty());
    }

    void provider_nullProviderRegionsEmpty() {
        NullProvider p;
        auto regions = p.enumerateRegions();
        QVERIFY(regions.isEmpty());
    }

    void provider_customRegions() {
        QVector<MemoryRegion> regs;
        regs.push_back(MemoryRegion{0x1000, 0x2000, true, true, false, "heap"});
        regs.push_back(MemoryRegion{0x3000, 0x1000, true, false, true, "code"});

        RegionProvider p(QByteArray(0x4000, '\0'), regs);
        auto result = p.enumerateRegions();
        QCOMPARE(result.size(), 2);
        QCOMPARE(result[0].base, (uint64_t)0x1000);
        QCOMPARE(result[0].moduleName, QStringLiteral("heap"));
        QCOMPARE(result[1].executable, true);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Scan Engine — Mask/pattern size mismatch
    // ═══════════════════════════════════════════════════════════════════

    void scan_maskSizeMismatch() {
        auto prov = std::make_shared<BufferProvider>(QByteArray(16, '\0'));
        ScanEngine engine;
        QSignalSpy errSpy(&engine, &ScanEngine::error);

        ScanRequest req;
        req.pattern = QByteArray(4, '\x00');
        req.mask = QByteArray(2, '\xFF'); // mismatch!

        engine.start(prov, req);
        QCOMPARE(errSpy.size(), 1);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Scan Engine — Multiple regions, pattern found in each
    // ═══════════════════════════════════════════════════════════════════

    void scan_multipleRegions() {
        QByteArray data(48, '\0');
        data[4]  = 0xEE; // region 0
        data[20] = 0xEE; // region 1
        data[36] = 0xEE; // region 2

        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0,  16, true, true, false, "region0"});
        regions.push_back(MemoryRegion{16, 16, true, true, false, "region1"});
        regions.push_back(MemoryRegion{32, 16, true, true, false, "region2"});

        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xEE", 1);
        req.mask    = QByteArray("\xFF", 1);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 3);
        QCOMPARE(results[0].regionModule, QStringLiteral("region0"));
        QCOMPARE(results[1].regionModule, QStringLiteral("region1"));
        QCOMPARE(results[2].regionModule, QStringLiteral("region2"));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Scan Engine — Overlapping matches
    // ═══════════════════════════════════════════════════════════════════

    void scan_overlappingMatches() {
        // Pattern AA AA — in buffer AA AA AA should find matches at 0 and 1
        QByteArray data(4, '\0');
        data[0] = 0xAA; data[1] = 0xAA; data[2] = 0xAA;

        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xAA\xAA", 2);
        req.mask    = QByteArray("\xFF\xFF", 2);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].address, (uint64_t)0);
        QCOMPARE(results[1].address, (uint64_t)1);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Edge case: scan 1-byte buffer
    // ═══════════════════════════════════════════════════════════════════

    void scan_oneByteBuffer() {
        QByteArray data(1, '\xAB');
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xAB", 1);
        req.mask    = QByteArray("\xFF", 1);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)0);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Edge case: all-wildcard pattern matches everywhere
    // ═══════════════════════════════════════════════════════════════════

    void scan_allWildcardPattern() {
        QByteArray data(8, '\x42');
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray(2, '\x00');
        req.mask    = QByteArray(2, '\x00'); // all wildcards

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 7); // 8 - 2 + 1 = 7 positions
    }

    // ═══════════════════════════════════════════════════════════════════
    // Address range filtering — "Current Struct" support
    // ═══════════════════════════════════════════════════════════════════

    void scan_addressRangeNoLimit() {
        // startAddress=0, endAddress=0 → scan all (default behavior unchanged)
        QByteArray data(32, '\x00');
        data[8] = '\xAA'; data[16] = '\xAA'; data[24] = '\xAA';
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xAA", 1);
        req.mask    = QByteArray("\xFF", 1);

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 3); // all 3 found
    }

    void scan_addressRangeClipsResults() {
        // Only scan addresses [8, 20) — should find match at offset 8 and 16 but not 24
        QByteArray data(32, '\x00');
        data[8] = '\xAA'; data[16] = '\xAA'; data[24] = '\xAA';
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xAA", 1);
        req.mask    = QByteArray("\xFF", 1);
        req.startAddress = 8;
        req.endAddress   = 20;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].address, (uint64_t)8);
        QCOMPARE(results[1].address, (uint64_t)16);
    }

    void scan_addressRangeOutsideData() {
        // Range entirely outside data → no results
        QByteArray data(16, '\xAA');
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xAA", 1);
        req.mask    = QByteArray("\xFF", 1);
        req.startAddress = 100;
        req.endAddress   = 200;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 0);
    }

    void scan_addressRangeWithRegions() {
        // Two regions: [1000, 1016) and [2000, 2016). Range [1000, 1020) clips to first region only.
        QByteArray data(4096, '\x00');
        // Place \xBB at offset 1000 and 2000
        data[1000] = '\xBB';
        data[2000] = '\xBB';

        QVector<MemoryRegion> regions;
        { MemoryRegion r; r.base = 1000; r.size = 16; r.readable = true; r.writable = true; regions.append(r); }
        { MemoryRegion r; r.base = 2000; r.size = 16; r.readable = true; r.writable = true; regions.append(r); }

        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.pattern = QByteArray("\xBB", 1);
        req.mask    = QByteArray("\xFF", 1);
        req.startAddress = 1000;
        req.endAddress   = 1020;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)1000);
    }

    void scan_unknownWithAddressRange() {
        // Unknown scan with address range should only capture within range
        QByteArray data(32, '\x42');
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);

        ScanRequest req;
        req.condition = ScanCondition::UnknownValue;
        req.valueSize = 4;
        req.alignment = 4;
        req.startAddress = 8;
        req.endAddress   = 24;

        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        // Range [8, 24) = 16 bytes, alignment 4, valueSize 4 → offsets 8, 12, 16, 20 = 4 results
        QCOMPARE(results.size(), 4);
        QCOMPARE(results[0].address, (uint64_t)8);
        QCOMPARE(results[3].address, (uint64_t)20);
    }

    // -- constrainRegions (multi-range intersection) --

    void scan_constrainRegions_multipleRanges() {
        QByteArray data(32, 0);
        data[4]  = char(0xBB);
        data[12] = char(0xBB);
        data[20] = char(0xBB);
        data[28] = char(0xBB);
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray("\xBB", 1);
        req.mask    = QByteArray("\xFF", 1);
        req.constrainRegions = {{0, 8}, {16, 24}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].address, (uint64_t)4);
        QCOMPARE(results[1].address, (uint64_t)20);
    }

    void scan_constrainRegions_intersectsProviderRegions() {
        QByteArray data(256, 0);
        data[160] = char(0xCC);
        data[210] = char(0xCC);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{100, 100, true, false, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray("\xCC", 1);
        req.mask    = QByteArray("\xFF", 1);
        req.constrainRegions = {{150, 250}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)160);
    }

    void scan_constrainRegions_noOverlap() {
        QByteArray data(32, char(0xEE));
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 16, true, false, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray("\xEE", 1);
        req.mask    = QByteArray("\xFF", 1);
        req.constrainRegions = {{100, 200}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 0);
    }

    // -- constrainRegions edge cases --

    void scan_constrainRegions_gapBetweenRegions() {
        // Provider has two regions with a gap: [0,16) and [32,48).
        // Constraint spans the gap: [8, 40). Should find matches in both.
        QByteArray data(64, 0);
        data[10] = char(0xDD);
        data[35] = char(0xDD);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 16, true, true, false, {}});
        regions.push_back(MemoryRegion{32, 16, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xDD));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{8, 40}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].address, (uint64_t)10);
        QCOMPARE(results[1].address, (uint64_t)35);
    }

    void scan_constrainRegions_partialRegionOverlap() {
        // Provider region [100, 200). Constraint [150, 250) clips to [150, 200).
        QByteArray data(256, 0);
        data[120] = char(0xAB);
        data[160] = char(0xAB);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{100, 100, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xAB));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{150, 250}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)160);
    }

    void scan_constrainRegions_mixedModuleAndAnonymous() {
        // Module region + anonymous heap region. Constraint covers both.
        QByteArray data(0x10000, 0);
        data[0x1500] = char(0xCC);
        data[0x5500] = char(0xCC);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0x1000, 0x1000, true, false, true, QString("game.exe")});
        regions.push_back(MemoryRegion{0x5000, 0x1000, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xCC));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{0x0, 0x10000}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].address, (uint64_t)0x1500);
        QCOMPARE(results[1].address, (uint64_t)0x5500);
    }

    void scan_constrainRegions_fallbackProvider() {
        // BufferProvider returns no regions -> fallback [0, size).
        // constrainRegions should still work against the fallback.
        QByteArray data(64, 0);
        data[10] = char(0xAA);
        data[30] = char(0xAA);
        data[50] = char(0xAA);
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xAA));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{5, 35}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].address, (uint64_t)10);
        QCOMPARE(results[1].address, (uint64_t)30);
    }

    void scan_constrainRegions_adjacentRegions() {
        // Two adjacent regions [0,16) and [16,32). Constraint [8,24) spans both.
        QByteArray data(32, 0);
        data[12] = char(0xEF);
        data[20] = char(0xEF);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 16, true, true, false, {}});
        regions.push_back(MemoryRegion{16, 16, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xEF));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{8, 24}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].address, (uint64_t)12);
        QCOMPARE(results[1].address, (uint64_t)20);
    }

    void scan_constrainRegions_writableFilterPreserved() {
        // filterWritable=true should still exclude non-writable clipped regions.
        QByteArray data(0x4000, 0);
        data[0x1100] = char(0xBB);
        data[0x2100] = char(0xBB);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0x1000, 0x1000, true, false, true, {}});
        regions.push_back(MemoryRegion{0x2000, 0x1000, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xBB));
        req.mask    = QByteArray(1, char(0xFF));
        req.filterWritable = true;
        req.constrainRegions = {{0x1000, 0x3000}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)0x2100);
    }


    void scan_constrainRegions_constraintExtendsBeforeAndAfter() {
        // Region [10, 20). Constraint [0, 30) extends before and after.
        // Should only scan [10, 20) — the intersection.
        QByteArray data(32, 0);
        data[5]  = char(0xAA);  // outside region, should NOT be found
        data[15] = char(0xAA);  // inside region, should be found
        data[25] = char(0xAA);  // outside region, should NOT be found
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{10, 10, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xAA));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{0, 30}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)15);
    }

    void scan_constrainRegions_emptyConstraintScansAll() {
        // Empty constrainRegions should scan everything (no restriction).
        QByteArray data(32, 0);
        data[5]  = char(0xBB);
        data[15] = char(0xBB);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 32, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xBB));
        req.mask    = QByteArray(1, char(0xFF));
        // constrainRegions left empty
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
    }

    void scan_constrainRegions_singleAddressRange() {
        // Equivalent to startAddress/endAddress: single constraint range.
        QByteArray data(32, 0);
        data[8]  = char(0xAA);
        data[16] = char(0xAA);
        data[24] = char(0xAA);
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xAA));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{8, 20}};  // same as startAddress=8, endAddress=20
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].address, (uint64_t)8);
        QCOMPARE(results[1].address, (uint64_t)16);
    }


    void scan_constrainRegions_withStartEndAddress() {
        // Both constrainRegions and startAddress/endAddress set.
        // constrainRegions: [0, 16) and [24, 32). startAddress/endAddress: [8, 28).
        // Effective scan should be intersection of both: [8, 16) and [24, 28).
        // Match at 4 (outside both), 12 (in both), 20 (in startEnd but not constrain),
        // 26 (in both), 30 (in constrain but not startEnd).
        QByteArray data(32, 0);
        data[4]  = char(0xDD);
        data[12] = char(0xDD);
        data[20] = char(0xDD);
        data[26] = char(0xDD);
        data[30] = char(0xDD);
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xDD));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{0, 16}, {24, 32}};
        req.startAddress = 8;
        req.endAddress   = 28;
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);  // only 12 and 26
        QCOMPARE(results[0].address, (uint64_t)12);
        QCOMPARE(results[1].address, (uint64_t)26);
    }

    void scan_constrainRegions_unknownValueScan() {
        // Unknown value scan with constrainRegions should only capture within ranges.
        QByteArray data(32, char(0x42));
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.condition = ScanCondition::UnknownValue;
        req.valueSize = 4;
        req.alignment = 4;
        req.constrainRegions = {{8, 24}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        // Range [8, 24) = 16 bytes, alignment 4, valueSize 4 -> offsets 8, 12, 16, 20 = 4 results
        QCOMPARE(results.size(), 4);
        QCOMPARE(results[0].address, (uint64_t)8);
        QCOMPARE(results[3].address, (uint64_t)20);
    }


    void scan_constrainRegions_nonZeroBase() {
        // Region with non-zero base; constraint matches exactly.
        QByteArray data(0x10000, 0);
        data[0x8100] = char(0xFF);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0x8000, 0x1000, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xFF));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{0x8000, 0x9000}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)0x8100);
    }

    void scan_constrainRegions_zeroSizeConstraint() {
        // Degenerate: constraint with start == end (zero size). Should scan nothing.
        QByteArray data(32, char(0xAA));
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xAA));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{10, 10}};  // zero-size
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 0);
    }

    void scan_constrainRegions_invertedRange() {
        // Degenerate: constraint with start > end. Should be treated as empty/invalid.
        QByteArray data(32, char(0xAA));
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xAA));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{20, 10}};  // inverted
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 0);
    }

    void scan_constrainRegions_overlappingConstraints() {
        // Two overlapping constraints: [4, 20) and [12, 28).
        // Should NOT double-count matches in the overlap [12, 20).
        QByteArray data(32, 0);
        data[8]  = char(0xCC);
        data[16] = char(0xCC);
        data[24] = char(0xCC);
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xCC));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{4, 20}, {12, 28}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        // After merge: [4, 28). All three matches are in range, no duplicates.
        QCOMPARE(results.size(), 3);
    }


    void scan_constrainRegions_patternAtFirstByte() {
        // Pattern at the very first byte of a clipped sub-region.
        // Region [0, 64). Constraint [20, 40). Match at offset 20.
        QByteArray data(64, 0);
        data[20] = char(0xFE);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 64, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0xFE));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{20, 40}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)20);
    }

    void scan_constrainRegions_patternAtLastByte() {
        // Pattern at the very last valid position of a clipped sub-region.
        // Region [0, 64). Constraint [20, 40). 4-byte pattern at offset 36 (last valid: 40-4=36).
        QByteArray data(64, 0);
        data[36] = char(0xDE); data[37] = char(0xAD); data[38] = char(0xBE); data[39] = char(0xEF);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 64, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray("\xDE\xAD\xBE\xEF", 4);
        req.mask    = QByteArray("\xFF\xFF\xFF\xFF", 4);
        req.constrainRegions = {{20, 40}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)36);
    }

    void scan_constrainRegions_patternOneByteAfterEnd() {
        // Pattern starts 1 byte before constraint end — only 3 of 4 bytes are in range.
        // Should NOT match because the full pattern doesn't fit.
        // Region [0, 64). Constraint [20, 39). 4-byte pattern at offset 36 (needs 36..39, but 39 is excluded).
        QByteArray data(64, 0);
        data[36] = char(0xDE); data[37] = char(0xAD); data[38] = char(0xBE); data[39] = char(0xEF);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 64, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray("\xDE\xAD\xBE\xEF", 4);
        req.mask    = QByteArray("\xFF\xFF\xFF\xFF", 4);
        req.constrainRegions = {{20, 39}};  // ends at 39, pattern needs 36..39 inclusive
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 0);  // pattern doesn't fit
    }

    void scan_constrainRegions_regionSmallerThanPattern() {
        // Clipped sub-region is smaller than the pattern. Should scan nothing, not crash.
        // Region [0, 64). Constraint [30, 32). 4-byte pattern can't fit in 2 bytes.
        QByteArray data(64, char(0xAA));
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 64, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray("\xAA\xAA\xAA\xAA", 4);
        req.mask    = QByteArray("\xFF\xFF\xFF\xFF", 4);
        req.constrainRegions = {{30, 32}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 0);
    }

    void scan_constrainRegions_patternExactlyFitsRegion() {
        // Clipped sub-region is exactly pattern size. Should find match if bytes match.
        // Region [0, 64). Constraint [30, 34). 4-byte pattern, 4-byte region.
        QByteArray data(64, 0);
        data[30] = char(0x11); data[31] = char(0x22); data[32] = char(0x33); data[33] = char(0x44);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 64, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray("\x11\x22\x33\x44", 4);
        req.mask    = QByteArray("\xFF\xFF\xFF\xFF", 4);
        req.constrainRegions = {{30, 34}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].address, (uint64_t)30);
    }

    void scan_constrainRegions_matchAtRegionBoundaries() {
        // Two adjacent clipped sub-regions. Matches at the last byte of the first
        // and first byte of the second. Both should be found.
        // Regions: [0, 16) and [16, 32). Constraint [0, 32) (full coverage).
        QByteArray data(32, 0);
        data[15] = char(0x77);  // last byte of first region
        data[16] = char(0x77);  // first byte of second region
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 16, true, true, false, {}});
        regions.push_back(MemoryRegion{16, 16, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray(1, char(0x77));
        req.mask    = QByteArray(1, char(0xFF));
        req.constrainRegions = {{0, 32}};
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].address, (uint64_t)15);
        QCOMPARE(results[1].address, (uint64_t)16);
    }

    void scan_constrainRegions_multibyteAtClipBoundary() {
        // 4-byte pattern that straddles the constraint boundary — should NOT be found
        // because the clipped region doesn't contain the full pattern.
        // Region [0, 64). Constraint [10, 13). Pattern at offset 10 is 4 bytes (10..13),
        // but constraint end is 13 (exclusive), so only 3 bytes [10,13) are in range.
        QByteArray data(64, 0);
        data[10] = char(0xAA); data[11] = char(0xBB); data[12] = char(0xCC); data[13] = char(0xDD);
        QVector<MemoryRegion> regions;
        regions.push_back(MemoryRegion{0, 64, true, true, false, {}});
        auto prov = std::make_shared<RegionProvider>(data, regions);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = QByteArray("\xAA\xBB\xCC\xDD", 4);
        req.mask    = QByteArray("\xFF\xFF\xFF\xFF", 4);
        req.constrainRegions = {{10, 13}};  // only 3 bytes, pattern needs 4
        engine.start(prov, req);
        QVERIFY(finSpy.wait(5000));
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 0);
    }


    // ── Value type + pattern scans at every position in a constrained region ──

    // Helper: run a scan with the given pattern/mask/alignment in a constrained region,
    // return the result addresses.
    QVector<uint64_t> scanConstrained(const QByteArray& data,
                                      const QByteArray& pat, const QByteArray& mask,
                                      int alignment, uint64_t cStart, uint64_t cEnd) {
        auto prov = std::make_shared<BufferProvider>(data);
        ScanEngine engine;
        QSignalSpy finSpy(&engine, &ScanEngine::finished);
        ScanRequest req;
        req.pattern = pat;
        req.mask = mask;
        req.alignment = alignment;
        req.constrainRegions = {{cStart, cEnd}};
        engine.start(prov, req);
        if (!finSpy.wait(5000)) return {};
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QVector<uint64_t> addrs;
        for (const auto& r : results) addrs.append(r.address);
        return addrs;
    }

    void scan_int32_atRegionStart() {
        QByteArray data(128, 0);
        int32_t v = 0x12345678;
        std::memcpy(data.data() + 32, &v, 4);
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Int32, "305419896", pat, mask));  // 0x12345678
        auto addrs = scanConstrained(data, pat, mask, 4, 32, 96);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)32);
    }

    void scan_int32_atRegionEnd() {
        QByteArray data(128, 0);
        int32_t v = 0x12345678;
        // Last aligned 4-byte position in [32, 96) is 92
        std::memcpy(data.data() + 92, &v, 4);
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Int32, "305419896", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 4, 32, 96);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)92);
    }

    void scan_float_atRegionStart() {
        QByteArray data(128, 0);
        float v = 3.14f;
        std::memcpy(data.data() + 16, &v, 4);
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Float, "3.14", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 4, 16, 80);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)16);
    }

    void scan_float_atRegionEnd() {
        QByteArray data(128, 0);
        float v = 3.14f;
        // Last aligned 4-byte position in [16, 80) is 76
        std::memcpy(data.data() + 76, &v, 4);
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Float, "3.14", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 4, 16, 80);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)76);
    }

    void scan_double_atRegionEnd() {
        QByteArray data(128, 0);
        double v = 2.71828;
        // Last aligned 8-byte position in [0, 128) is 120
        std::memcpy(data.data() + 120, &v, 8);
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Double, "2.71828", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 8, 0, 128);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)120);
    }

    void scan_int64_atRegionEnd() {
        QByteArray data(128, 0);
        int64_t v = 0x0BADC0DEDEADBEEFLL;
        // Last aligned 8-byte position in [8, 72) is 64
        std::memcpy(data.data() + 64, &v, 8);
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Int64, "841540768839352047", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 8, 8, 72);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)64);
    }

    void scan_utf16_atRegionEnd() {
        QByteArray data(128, 0);
        // "AB" in UTF-16LE = 4 bytes
        uint16_t chars[] = { 'A', 'B' };
        // Last aligned 2-byte position where 4 bytes fit in [0, 128) is 124
        std::memcpy(data.data() + 124, chars, 4);
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UTF16, "AB", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 2, 0, 128);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)124);
    }

    void scan_vec3_atRegionEnd() {
        QByteArray data(128, 0);
        float v[] = { 1.0f, 2.0f, 3.0f };  // 12 bytes
        // Last aligned 4-byte position where 12 bytes fit in [0, 128) is 116
        std::memcpy(data.data() + 116, v, 12);
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Vec3, "1.0 2.0 3.0", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 4, 0, 128);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)116);
    }

    void scan_pattern_atRegionStart() {
        QByteArray data(128, 0);
        data[20] = char(0x48); data[21] = char(0x8B); data[22] = char(0x05);
        QByteArray pat, mask;
        QVERIFY(parseSignature("48 8B 05", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 1, 20, 100);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)20);
    }

    void scan_pattern_atRegionEnd() {
        QByteArray data(128, 0);
        // 3-byte pattern, last position in [20, 100) is 97
        data[97] = char(0x48); data[98] = char(0x8B); data[99] = char(0x05);
        QByteArray pat, mask;
        QVERIFY(parseSignature("48 8B 05", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 1, 20, 100);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)97);
    }

    void scan_pattern_withWildcard_atRegionEnd() {
        QByteArray data(128, 0);
        // "48 ?? 05" at last position 97 in [20, 100)
        data[97] = char(0x48); data[98] = char(0xFF); data[99] = char(0x05);
        QByteArray pat, mask;
        QVERIFY(parseSignature("48 ?? 05", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 1, 20, 100);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)97);
    }

    void scan_int32_multiplePositions_inConstrainedRegion() {
        // Place int32 at first, middle, and last aligned positions in [32, 96).
        // Aligned positions: 32, 36, 40, ..., 88, 92. First=32, last=92, mid=60.
        QByteArray data(128, 0);
        int32_t v = 0xCAFEBABE;
        std::memcpy(data.data() + 32, &v, 4);
        std::memcpy(data.data() + 60, &v, 4);
        std::memcpy(data.data() + 92, &v, 4);
        // Also place one outside the constraint to verify it's excluded
        std::memcpy(data.data() + 8, &v, 4);
        std::memcpy(data.data() + 100, &v, 4);
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UInt32, "0xCAFEBABE", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 4, 32, 96);
        QCOMPARE(addrs.size(), 3);
        QCOMPARE(addrs[0], (uint64_t)32);
        QCOMPARE(addrs[1], (uint64_t)60);
        QCOMPARE(addrs[2], (uint64_t)92);
    }

    void scan_pattern_multiplePositions_inConstrainedRegion() {
        // IDA-style pattern at first, last, and middle of [16, 80).
        // Pattern "AA BB" (2 bytes), alignment 1. First=16, last=78, mid=50.
        QByteArray data(128, 0);
        data[16] = char(0xAA); data[17] = char(0xBB);
        data[50] = char(0xAA); data[51] = char(0xBB);
        data[78] = char(0xAA); data[79] = char(0xBB);
        // Outside constraint
        data[10] = char(0xAA); data[11] = char(0xBB);
        data[90] = char(0xAA); data[91] = char(0xBB);
        QByteArray pat, mask;
        QVERIFY(parseSignature("AA BB", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 1, 16, 80);
        QCOMPARE(addrs.size(), 3);
        QCOMPARE(addrs[0], (uint64_t)16);
        QCOMPARE(addrs[1], (uint64_t)50);
        QCOMPARE(addrs[2], (uint64_t)78);
    }


    void scan_int8_alignment1_atRegionEnd() {
        // 1-byte value at last byte of constrained region [10, 50).
        QByteArray data(64, 0);
        data[49] = char(0x7F);
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Int8, "127", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 1, 10, 50);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)49);
    }

    void scan_uint16_alignment2_atRegionEnd() {
        // 2-byte value at last aligned-2 position in [10, 50) = offset 48.
        QByteArray data(64, 0);
        uint16_t v = 0xBEEF;
        std::memcpy(data.data() + 48, &v, 2);
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UInt16, "0xBEEF", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 2, 10, 50);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)48);
    }

    void scan_alignment4_skipsUnaligned() {
        // int32 placed at unaligned offset 18 inside [16, 48). Alignment 4.
        // Aligned positions from 16: 16, 20, 24, 28, 32, 36, 40, 44.
        // Offset 18 is not aligned to 4 from the region start, so should be skipped.
        QByteArray data(64, 0);
        int32_t v = 0xDEADBEEF;
        std::memcpy(data.data() + 18, &v, 4);  // unaligned
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UInt32, "0xDEADBEEF", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 4, 16, 48);
        QCOMPARE(addrs.size(), 0);
    }

    void scan_alignment8_skipsUnaligned() {
        // double placed at offset 12 inside [0, 64). Alignment 8.
        // Aligned positions: 0, 8, 16, 24, 32, 40, 48, 56.
        // Offset 12 is not 8-aligned, so should be skipped.
        QByteArray data(64, 0);
        double v = 99.99;
        std::memcpy(data.data() + 12, &v, 8);  // unaligned
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::Double, "99.99", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 8, 0, 64);
        QCOMPARE(addrs.size(), 0);
    }

    void scan_alignment2_findsAligned_skipsUnaligned() {
        // utf16 "Hi" (4 bytes) at aligned offset 20 and unaligned offset 33.
        // Constraint [16, 48), alignment 2. Should find only offset 20.
        QByteArray data(64, 0);
        uint16_t chars[] = { 'H', 'i' };
        std::memcpy(data.data() + 20, chars, 4);  // aligned to 2
        std::memcpy(data.data() + 33, chars, 4);  // unaligned to 2
        QByteArray pat, mask;
        QVERIFY(serializeValue(ValueType::UTF16, "Hi", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 2, 16, 48);
        QCOMPARE(addrs.size(), 1);
        QCOMPARE(addrs[0], (uint64_t)20);
    }

    void scan_alignment1_overlappingWrites() {
        // Pattern "AA BB" written at 20, then overwritten at 21, plus 25.
        // Second write clobbers offset 20's pattern; only 21 and 25 match.
        QByteArray data(48, 0);
        data[20] = char(0xAA); data[21] = char(0xBB);
        data[21] = char(0xAA); data[22] = char(0xBB);  // overlapping at 21
        data[25] = char(0xAA); data[26] = char(0xBB);
        QByteArray pat, mask;
        QVERIFY(parseSignature("AA BB", pat, mask));
        auto addrs = scanConstrained(data, pat, mask, 1, 16, 32);
        QCOMPARE(addrs.size(), 2);  // 21 and 25 (20 was overwritten)
        QCOMPARE(addrs[0], (uint64_t)21);
        QCOMPARE(addrs[1], (uint64_t)25);
    }
};

QTEST_MAIN(TestScanner)
#include "test_scanner.moc"
