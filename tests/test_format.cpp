#include <QtTest/QTest>
#include <cstring>
#include "core.h"

using namespace rcx;

class TestFormat : public QObject {
    Q_OBJECT
private slots:
    // The float decimals setting is global: every test starts from the default.
    void init() { fmt::setFloatDecimals(fmt::kDefaultFloatDecimals); }

    void testTypeName() {
        QString s = fmt::typeName(NodeKind::Float);
        QVERIFY(s.trimmed() == "float");
        QCOMPARE(s.size(), 14); // kColType
    }

    void testFmtInt32() {
        // fmtInt32 outputs decimal representation
        QCOMPARE(fmt::fmtInt32(-42), QString("-42"));
        QCOMPARE(fmt::fmtInt32(0),   QString("0"));
    }

    void testFmtFloat() {
        // At the default 3 decimals: at most 3, fewer once the integer part
        // passes the 5-digit budget. A negative value gets '-' in front.
        auto check = [](float v, const char* expected) {
            QString s = fmt::fmtFloat(v);
            QCOMPARE(s, QString(expected));
        };

        // Basic positive/negative
        check( 3.14159f,  "3.142f");
        check(-3.14159f,  "-3.142f");

        // Zero
        check( 0.f,       "0.000f");

        // Small values
        check( 0.02f,     "0.020f");
        check(-0.069f,    "-0.069f");
        check(-0.2777f,   "-0.278f");

        // Values >= 10 — still 3 decimal places
        check( 15.6543f,  "15.654f");
        check(-77.6624f,  "-77.662f");

        // Values >= 100 — 2 decimal places
        check( 500.f,     "500.00f");

        // Values >= 1000 — 1 decimal place
        check( 5000.f,    "5000.0f");

        // Values >= 10000 — 0 decimal places + "."
        check( 50000.f,   "50000.f");

        // Overflow cap
        check( 100000.f,  "99999+f");
        check(-100000.f,  "-99999+f");

        // Special values
        check( 1.f / 0.f, "inff");
        check(-1.f / 0.f, "-inff");
        QCOMPARE(fmt::fmtFloat(std::nanf("")), QString("NaN"));

        // 1.0 exactly
        check( 1.f,       "1.000f");
        check(-1.f,       "-1.000f");
    }

    void testFloatDecimalsSetting() {
        QCOMPARE(fmt::floatDecimals(), 3);

        // 4 decimals reads exactly as floats always did.
        fmt::setFloatDecimals(4);
        QCOMPARE(fmt::fmtFloat(3.14159f), QStringLiteral("3.1416f"));
        QCOMPARE(fmt::fmtFloat(-0.2777f), QStringLiteral("-0.2777f"));
        QCOMPARE(fmt::fmtFloat(15.6543f), QStringLiteral("15.654f"));
        QCOMPARE(fmt::fmtFloat(500.f),    QStringLiteral("500.00f"));
        QCOMPARE(fmt::fmtFloat(50000.f),  QStringLiteral("50000.f"));
        // Negative zero is as wide as any other negative (was "-0.000f").
        QCOMPARE(fmt::fmtFloat(-0.0f), QStringLiteral("-0.0000f"));

        fmt::setFloatDecimals(1);
        QCOMPARE(fmt::fmtFloat(-0.2777f), QStringLiteral("-0.3f"));
        QCOMPARE(fmt::fmtFloat(15.6543f), QStringLiteral("15.7f"));
        QCOMPARE(fmt::fmtFloat(50000.f),  QStringLiteral("50000.f"));

        // 6: a value under 10 shows all six; the integer budget grows to 7.
        fmt::setFloatDecimals(6);
        QCOMPARE(fmt::fmtFloat(0.5f),  QStringLiteral("0.500000f"));
        QCOMPARE(fmt::fmtFloat(15.5f), QStringLiteral("15.50000f"));

        // Clamped to [1, 6].
        fmt::setFloatDecimals(0);
        QCOMPARE(fmt::floatDecimals(), fmt::kMinFloatDecimals);
        fmt::setFloatDecimals(99);
        QCOMPARE(fmt::floatDecimals(), fmt::kMaxFloatDecimals);

        // Rounding that grows a digit gives up a decimal, never the budget.
        fmt::setFloatDecimals(3);
        QCOMPARE(fmt::fmtFloat(9.9996f),   QStringLiteral("10.000f"));
        QCOMPARE(fmt::fmtFloat(99999.9f),  QStringLiteral("99999+f"));
        QCOMPARE(fmt::fmtFloat(-0.0f),     QStringLiteral("-0.000f"));
    }

    void testDoubleDecimals() {
        QCOMPARE(fmt::fmtDouble(51.5),      QStringLiteral("51.5"));
        QCOMPARE(fmt::fmtDouble(0.0333333), QStringLiteral("0.033"));
        QCOMPARE(fmt::fmtDouble(42.0),      QStringLiteral("42.0"));
        QCOMPARE(fmt::fmtDouble(-0.27777),  QStringLiteral("-0.278"));
        QCOMPARE(fmt::fmtDouble(-0.0),      QStringLiteral("-0.0"));   // the sign bit, like floats
        fmt::setFloatDecimals(6);
        QCOMPARE(fmt::fmtDouble(0.0333333), QStringLiteral("0.033333"));
    }

    void testFmtBool() {
        QCOMPARE(fmt::fmtBool(1), QString("true"));
        QCOMPARE(fmt::fmtBool(0), QString("false"));
    }

    void testFmtPointer64_null() {
        QCOMPARE(fmt::fmtPointer64(0), QStringLiteral("nullptr"));
    }

    void testFmtPointer64_nonNull() {
        QString s = fmt::fmtPointer64(0x400000);
        QVERIFY(s.startsWith("0x"));
        QVERIFY(s.contains("400000"));
    }

    void testFmtOffsetMargin_primary() {
        QCOMPARE(fmt::fmtOffsetMargin(0x10, false), QString("00000010 "));
        QCOMPARE(fmt::fmtOffsetMargin(0, false),    QString("00000000 "));
    }

    void testFmtOffsetMargin_continuation() {
        QCOMPARE(fmt::fmtOffsetMargin(0x10, true), QString("  \u00B7 "));
    }

    void testFmtOffsetMargin_kernelAddr() {
        QCOMPARE(fmt::fmtOffsetMargin(0xFFFFF80012345678ULL, false, 16),
                 QString("FFFFF80012345678 "));
        QCOMPARE(fmt::fmtOffsetMargin(0x10, false, 16),
                 QString("0000000000000010 "));
        QCOMPARE(fmt::fmtOffsetMargin(0x10, false, 4),
                 QString("0010 "));
    }

    void testFmtStructHeader() {
        Node n;
        n.kind = NodeKind::Struct;
        n.name = "Test";
        // Expanded header should contain opening brace
        QString s = fmt::fmtStructHeader(n, 0, /*collapsed=*/false);
        QVERIFY(s.contains("struct"));
        QVERIFY(s.contains("Test"));
        QVERIFY(s.contains("{"));

        // Collapsed header should not contain opening brace
        QString collapsed = fmt::fmtStructHeader(n, 0, /*collapsed=*/true);
        QVERIFY(collapsed.contains("struct"));
        QVERIFY(collapsed.contains("Test"));
        QVERIFY(!collapsed.contains("{"));
    }

    void testFmtStructFooter() {
        Node n;
        n.kind = NodeKind::Struct;
        n.name = "Test";
        QString s = fmt::fmtStructFooter(n, 0);
        QVERIFY(s.contains("};"));
        // When no size, footer is just "};" without name
    }

    void testIndent() {
        QCOMPARE(fmt::indent(0), QString(""));
        QCOMPARE(fmt::indent(1), QString("  "));
        QCOMPARE(fmt::indent(3), QString("      "));
    }

    void testParseValueInt32() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::Int32, "-42", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 4);
        int32_t v;
        memcpy(&v, b.data(), 4);
        QCOMPARE(v, -42);
    }

    void testParseValueFloat() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::Float, "3.14", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 4);
        float v;
        memcpy(&v, b.data(), 4);
        QVERIFY(qAbs(v - 3.14f) < 0.01f);
    }

    void testParseValueHex32() {
        bool ok;
        // Hex kinds parse as memory-order bytes (matches hex-preview display).
        // "DEADBEEF" stores bytes [DE, AD, BE, EF] in memory, which round-trips
        // with the hex preview that shows "DE AD BE EF".
        QByteArray b = fmt::parseValue(NodeKind::Hex32, "DEADBEEF", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 4);
        QCOMPARE((uint8_t)b[0], (uint8_t)0xDE);
        QCOMPARE((uint8_t)b[1], (uint8_t)0xAD);
        QCOMPARE((uint8_t)b[2], (uint8_t)0xBE);
        QCOMPARE((uint8_t)b[3], (uint8_t)0xEF);
    }

    void testParseValueBool() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::Bool, "true", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 1);
        QCOMPARE((uint8_t)b[0], (uint8_t)1);

        b = fmt::parseValue(NodeKind::Bool, "false", &ok);
        QVERIFY(ok);
        QCOMPARE((uint8_t)b[0], (uint8_t)0);

        // Unknown token should fail
        fmt::parseValue(NodeKind::Bool, "banana", &ok);
        QVERIFY(!ok);
    }

    void testParseValueHex0xPrefix() {
        bool ok;
        // Hex32 with 0x prefix: prefix is stripped, rest parsed as memory bytes
        QByteArray b = fmt::parseValue(NodeKind::Hex32, "0xDEADBEEF", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 4);
        QCOMPARE((uint8_t)b[0], (uint8_t)0xDE);
        QCOMPARE((uint8_t)b[3], (uint8_t)0xEF);

        // Pointer64 with 0x prefix — pointer kinds still parse as integer
        b = fmt::parseValue(NodeKind::Pointer64, "0x0000000000400000", &ok);
        QVERIFY(ok);
        uint64_t v64;
        memcpy(&v64, b.data(), 8);
        QCOMPARE(v64, (uint64_t)0x400000);
    }

    void testParseValueOverflow() {
        bool ok;
        // UInt8: 300 exceeds uint8_t max (255) → should fail
        fmt::parseValue(NodeKind::UInt8, "300", &ok);
        QVERIFY(!ok);

        // UInt8: 255 should succeed
        QByteArray b = fmt::parseValue(NodeKind::UInt8, "255", &ok);
        QVERIFY(ok);
        QCOMPARE((uint8_t)b[0], (uint8_t)255);

        // Int8: 200 exceeds int8_t max (127) → should fail
        fmt::parseValue(NodeKind::Int8, "200", &ok);
        QVERIFY(!ok);

        // Int8: -129 below min → should fail
        fmt::parseValue(NodeKind::Int8, "-129", &ok);
        QVERIFY(!ok);

        // Int8: -128 is valid
        b = fmt::parseValue(NodeKind::Int8, "-128", &ok);
        QVERIFY(ok);
        int8_t sv;
        memcpy(&sv, b.data(), 1);
        QCOMPARE(sv, (int8_t)-128);

        // UInt16: 70000 exceeds uint16_t max → should fail
        fmt::parseValue(NodeKind::UInt16, "70000", &ok);
        QVERIFY(!ok);

        // Hex8: 0x1FF exceeds uint8_t → should fail
        fmt::parseValue(NodeKind::Hex8, "1FF", &ok);
        QVERIFY(!ok);

        // Hex16: 0x1FFFF exceeds uint16_t → should fail
        fmt::parseValue(NodeKind::Hex16, "1FFFF", &ok);
        QVERIFY(!ok);
    }

    void testSignedHexRoundTrip() {
        bool ok;
        // Int8: 0xFF should parse as -1 (two's complement)
        QByteArray b = fmt::parseValue(NodeKind::Int8, "0xFF", &ok);
        QVERIFY(ok);
        int8_t sv8;
        memcpy(&sv8, b.data(), 1);
        QCOMPARE(sv8, (int8_t)-1);

        // Int8: 0x80 should parse as -128
        b = fmt::parseValue(NodeKind::Int8, "0x80", &ok);
        QVERIFY(ok);
        memcpy(&sv8, b.data(), 1);
        QCOMPARE(sv8, (int8_t)-128);

        // Int16: 0xFFFF should parse as -1
        b = fmt::parseValue(NodeKind::Int16, "0xFFFF", &ok);
        QVERIFY(ok);
        int16_t sv16;
        memcpy(&sv16, b.data(), 2);
        QCOMPARE(sv16, (int16_t)-1);

        // Int32: 0xFFFFFFFF should parse as -1
        b = fmt::parseValue(NodeKind::Int32, "0xFFFFFFFF", &ok);
        QVERIFY(ok);
        int32_t sv32;
        memcpy(&sv32, b.data(), 4);
        QCOMPARE(sv32, (int32_t)-1);

        // Int8: 0x1FF should fail (exceeds byte range)
        fmt::parseValue(NodeKind::Int8, "0x1FF", &ok);
        QVERIFY(!ok);

        // Int16: 0x1FFFF should fail (exceeds 16-bit range)
        fmt::parseValue(NodeKind::Int16, "0x1FFFF", &ok);
        QVERIFY(!ok);
    }

    void testReadValueBoundsCheck() {
        // Vec2 single-line: subLine=0 returns all components
        QByteArray data(16, '\0');
        BufferProvider prov(data);
        Node n;
        n.kind = NodeKind::Vec2;
        n.name = "v";
        QVERIFY(fmt::readValue(n, prov, 0, 0).contains(","));

        // Vec3 single-line: subLine=0 returns 3 comma-separated values
        n.kind = NodeKind::Vec3;
        QCOMPARE(fmt::readValue(n, prov, 0, 0).count(','), 2);

        // Vec4 single-line: subLine=0 returns 4 comma-separated values
        n.kind = NodeKind::Vec4;
        QCOMPARE(fmt::readValue(n, prov, 0, 0).count(','), 3);
    }

    // Vector and matrix components keep a column for the sign: a value
    // turning negative never pushes the components after it over.
    void testVectorAndMatrixComponentsKeepASignColumn() {
        auto put = [](QByteArray& d, const std::initializer_list<float>& vals) {
            int i = 0;
            for (float v : vals) { std::memcpy(d.data() + i, &v, 4); i += 4; }
        };
        Node n;
        n.name = "v";
        QByteArray a(64, '\0'), b(64, '\0');
        put(a, { 1.5f, -2.25f, 0.5f, 3.0f});
        put(b, {-1.5f,  2.25f, -0.5f, -3.0f});
        BufferProvider pa(a), pb(b);

        n.kind = NodeKind::Vec3;
        const QString va = fmt::readValue(n, pa, 0, 0);
        const QString vb = fmt::readValue(n, pb, 0, 0);
        QCOMPARE(va, QStringLiteral("[ 1.500f, -2.250f,  0.500f]"));   // brackets, like a matrix row
        QCOMPARE(vb, QStringLiteral("[-1.500f,  2.250f, -0.500f]"));
        for (int i = 0; i < va.size(); ++i)                       // every comma stays put
            QCOMPARE(va[i] == QLatin1Char(','), vb[i] == QLatin1Char(','));
        // What an edit starts from carries no padding and no brackets.
        const QString edit = fmt::editableValue(n, pa, 0, 0);
        QVERIFY(!edit.startsWith(QLatin1Char(' ')));
        QVERIFY(!edit.contains(QLatin1Char('[')));

        n.kind = NodeKind::Mat4x4;
        const QString ra = fmt::readValue(n, pa, 0, 0);
        const QString rb = fmt::readValue(n, pb, 0, 0);
        QCOMPARE(ra, QStringLiteral("row0 [ 1.500f, -2.250f,  0.500f,  3.000f]"));
        QCOMPARE(ra.size(), rb.size());
        for (int i = 0; i < ra.size(); ++i)
            QCOMPARE(ra[i] == QLatin1Char(','), rb[i] == QLatin1Char(','));

        // A vector reads exactly as a matrix row does, past the "rowN ".
        n.kind = NodeKind::Vec4;
        QCOMPARE(fmt::readValue(n, pa, 0, 0), ra.mid(5));

        // Negative zero takes no less room than any other negative.
        QByteArray z(12, '\0');
        put(z, {-0.0f, 0.25f, -1.0f});
        BufferProvider pz(z);
        n.kind = NodeKind::Vec3;
        const QString vz = fmt::readValue(n, pz, 0, 0);
        QCOMPARE(vz, QStringLiteral("[-0.000f,  0.250f, -1.000f]"));
        for (int i = 0; i < vz.size(); ++i)
            QCOMPARE(vz[i] == QLatin1Char(','), va[i] == QLatin1Char(','));
    }

    // One matrix shares one component width across all four rows: a value
    // reaching 10 widens every row alike, so the columns stay straight.
    void testMatrixRowsShareOneComponentWidth() {
        QByteArray m(64, '\0');
        const float big = 12.5f, small = 0.5f;
        std::memcpy(m.data() + 60, &big, 4);     // row 3, column 3
        std::memcpy(m.data() + 0, &small, 4);    // row 0, column 0
        BufferProvider pm(m);
        Node n;
        n.kind = NodeKind::Mat4x4;
        QStringList rows;
        for (int r = 0; r < 4; ++r) rows << fmt::readValue(n, pm, 0, r);
        for (const QString& row : rows) {
            QCOMPARE(row.size(), rows[0].size());
            for (int i = 5; i < row.size(); ++i)
                QCOMPARE(row[i] == QLatin1Char(','), rows[0][i] == QLatin1Char(','));
        }
        QCOMPARE(rows[0], QStringLiteral("row0 [  0.500f,   0.000f,   0.000f,   0.000f]"));
        QCOMPARE(rows[3], QStringLiteral("row3 [  0.000f,   0.000f,   0.000f,  12.500f]"));
    }

    void testEditableKeepsEveryDigit() {
        QByteArray d(16, '\0');
        const float f = 0.27777f;
        const double g = 0.123456789;
        std::memcpy(d.data(), &f, 4);
        std::memcpy(d.data() + 8, &g, 8);
        BufferProvider p(d);
        Node n;
        n.kind = NodeKind::Float;
        QVERIFY2(fmt::editableValue(n, p, 0, 0).startsWith(QStringLiteral("0.27777")),
                 qPrintable(fmt::editableValue(n, p, 0, 0)));
        n.kind = NodeKind::Double;
        QVERIFY2(fmt::editableValue(n, p, 8, 0).startsWith(QStringLiteral("0.123456789")),
                 qPrintable(fmt::editableValue(n, p, 8, 0)));
        // Every digit, and no more than it takes to read back the same value.
        QCOMPARE(fmt::fmtFloatExact(1.52f), QStringLiteral("1.52"));
        QCOMPARE(fmt::fmtFloatExact(0.27777f), QStringLiteral("0.27777"));
        QCOMPARE(fmt::fmtFloatExact(-0.5f), QStringLiteral("-0.5"));
        QCOMPARE(fmt::fmtDoubleExact(0.0333333), QStringLiteral("0.0333333"));
    }

    void testEditableValueBasic() {
        QByteArray data(16, '\0');
        // Write a known float value
        float val = 3.14f;
        memcpy(data.data(), &val, 4);
        BufferProvider prov(data);

        Node n;
        n.kind = NodeKind::Float;
        n.name = "f";
        QString s = fmt::editableValue(n, prov, 0, 0);
        QVERIFY(s.contains("3.14"));

        // Vec2 single-line: returns comma-separated values
        n.kind = NodeKind::Vec2;
        QString vec2 = fmt::editableValue(n, prov, 0, 0);
        QVERIFY(vec2.contains(","));
    }

    void testParseValueEmptyString() {
        bool ok;
        // Empty UTF8 should succeed (caller pads)
        QByteArray b = fmt::parseValue(NodeKind::UTF8, "", &ok);
        QVERIFY(ok);
        QVERIFY(b.isEmpty());

        // Empty non-string should fail
        fmt::parseValue(NodeKind::Int32, "", &ok);
        QVERIFY(!ok);
    }

    void testFmtStructFooterSimple() {
        Node n;
        n.kind = NodeKind::Struct;
        n.name = "Test";

        // Footer is always just "};" (no sizeof comment)
        QString s = fmt::fmtStructFooter(n, 0, 0x14);
        QVERIFY(s.contains("};"));
        QVERIFY(!s.contains("sizeof"));  // No sizeof comment
    }
    void testFmtFloatEdgeCases() {
        QCOMPARE(fmt::fmtFloat(std::numeric_limits<float>::quiet_NaN()), QStringLiteral("NaN"));
        QCOMPARE(fmt::fmtFloat(std::numeric_limits<float>::infinity()), QStringLiteral("inff"));
        QCOMPARE(fmt::fmtFloat(-std::numeric_limits<float>::infinity()), QStringLiteral("-inff"));
        // Normal float should contain 'f' suffix
        QVERIFY(fmt::fmtFloat(3.14f).contains('f'));
        // -0.0f should display with minus sign
        QVERIFY(fmt::fmtFloat(-0.0f).startsWith('-'));
    }

    void testFmtDoubleIntegerValue() {
        // Double with integer value should still have decimal point
        QString s = fmt::fmtDouble(42.0);
        QVERIFY(s.contains('.'));
    }

    void testFmtBoolValues() {
        QCOMPARE(fmt::fmtBool(1), QStringLiteral("true"));
        QCOMPARE(fmt::fmtBool(0), QStringLiteral("false"));
    }

    void testValidateValueEmpty() {
        // Empty string should be OK (some contexts allow empty)
        QVERIFY(fmt::validateValue(NodeKind::Int32, "").isEmpty());
    }

    void testValidateValueHexOverflow() {
        // Value too large for Int8 should produce error
        QString err = fmt::validateValue(NodeKind::Int8, "999");
        QVERIFY(!err.isEmpty());
    }

    void testParseValueBoolStrings() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::Bool, "true", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 1);
        QCOMPARE((uint8_t)b[0], (uint8_t)1);

        b = fmt::parseValue(NodeKind::Bool, "false", &ok);
        QVERIFY(ok);
        QCOMPARE((uint8_t)b[0], (uint8_t)0);
    }
    void testParseValueHex128() {
        bool ok;
        // Space-separated 16 bytes
        QByteArray b = fmt::parseValue(NodeKind::Hex128,
            "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 16);
        QCOMPARE((uint8_t)b[0], (uint8_t)0x00);
        QCOMPARE((uint8_t)b[15], (uint8_t)0xFF);
    }

    void testParseValueHex128TooShort() {
        bool ok;
        // Only 8 bytes — should fail (expects 16)
        QByteArray b = fmt::parseValue(NodeKind::Hex128,
            "00 11 22 33 44 55 66 77", &ok);
        QVERIFY(!ok);
    }

    void testReadValueHex128() {
        // Build a 16-byte buffer and read it as Hex128
        QByteArray data(16, '\0');
        data[0] = 0x41;  // 'A'
        data[15] = (char)0xFF;
        BufferProvider prov(data);
        Node n;
        n.kind = NodeKind::Hex128;
        // Display mode should show hex value
        QString val = fmt::readValue(n, prov, 0, 0);
        QVERIFY(!val.isEmpty());
        // Editable mode should show space-separated hex bytes
        QString edit = fmt::editableValue(n, prov, 0, 0);
        QVERIFY(edit.contains(' '));
        QVERIFY(edit.size() >= 47);  // 16*3-1 = 47 chars
    }

    void testFmtFloatVerySmall() {
        // Very small floats should not show "0.0000f" when nonzero
        QString s = fmt::fmtFloat(1e-7f);
        QVERIFY(s.contains('f'));
        QVERIFY(s.size() <= 9);
    }

    void testFmtDoubleVeryLarge() {
        QString s = fmt::fmtDouble(1e308);
        QVERIFY(!s.isEmpty());
        QVERIFY(s.contains('.') || s.contains('e') || s.contains('E'));
    }

    void testFmtDoubleNegativeZero() {
        QString s = fmt::fmtDouble(-0.0);
        // Qt's QString::number may or may not preserve -0.0
        QVERIFY(!s.isEmpty());
    }

    void testFmtDoubleNanInf() {
        QString sNan = fmt::fmtDouble(std::numeric_limits<double>::quiet_NaN());
        QVERIFY(!sNan.isEmpty());
        QString sInf = fmt::fmtDouble(std::numeric_limits<double>::infinity());
        QVERIFY(!sInf.isEmpty());
    }

    void testParseValueUtf8Emoji() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::UTF8, QStringLiteral("\"hello\""), &ok);
        QVERIFY(ok);
        QCOMPARE(b, QByteArray("hello"));
    }

    void testParseValueHex16SpaceSeparated() {
        bool ok;
        QByteArray b = fmt::parseValue(NodeKind::Hex16, "AB CD", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 2);
        QCOMPARE((uint8_t)b[0], (uint8_t)0xAB);
        QCOMPARE((uint8_t)b[1], (uint8_t)0xCD);
    }

    void testValidateValueHex128() {
        QString err = fmt::validateValue(NodeKind::Hex128,
            "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF");
        QVERIFY(err.isEmpty());
    }

    void testKindFromTypeNameUnknown() {
        bool ok = true;
        NodeKind k = kindFromTypeName(QStringLiteral("nonsense"), &ok);
        QVERIFY(!ok);
        QCOMPARE(k, NodeKind::Hex8);
    }

    void testAllTypeNamesForUI() {
        QStringList names = allTypeNamesForUI();
        QCOMPARE(names.size(), (int)std::size(kKindMeta));
        // No duplicates
        QSet<QString> s(names.begin(), names.end());
        QCOMPARE(s.size(), names.size());
    }

    void testIsValidPrimitivePtrTarget() {
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Hex8));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Pointer64));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Struct));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::FuncPtr64));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Int32));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Float));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Bool));
    }
};

QTEST_MAIN(TestFormat)
#include "test_format.moc"
