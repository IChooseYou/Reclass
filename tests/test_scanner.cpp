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
        regions.append({0,  16, true, true, false, "heap"});
        regions.append({16, 16, true, false, true, "code"});

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
        regions.append({0,  16, true, true, false, "data"});
        regions.append({16, 16, true, false, true, "code"});

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
        regions.append({0,  16, true, true,  false, "data"});
        regions.append({16, 16, true, false, true,  "code"});
        regions.append({32, 16, true, true,  true,  "rwx"});

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
        regions.append({0, 16, true, true, true, "Game.exe"});

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
        regs.append({0x1000, 0x2000, true, true, false, "heap"});
        regs.append({0x3000, 0x1000, true, false, true, "code"});

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
        regions.append({0,  16, true, true, false, "region0"});
        regions.append({16, 16, true, true, false, "region1"});
        regions.append({32, 16, true, true, false, "region2"});

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
};

QTEST_MAIN(TestScanner)
#include "test_scanner.moc"
