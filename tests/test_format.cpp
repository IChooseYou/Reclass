#include <QtTest/QTest>
#include "core.h"

using namespace rcx;

class TestFormat : public QObject {
    Q_OBJECT
private slots:
    void testTypeName() {
        QString s = fmt::typeName(NodeKind::Float);
        QVERIFY(s.trimmed() == "float");
        QCOMPARE(s.size(), 14); // kColType
    }

    void testFmtInt32() {
        // fmtInt32 outputs hex representation (0xffffffd6 for -42)
        QCOMPARE(fmt::fmtInt32(-42), QString("0xffffffd6"));
        QCOMPARE(fmt::fmtInt32(0),   QString("0x0"));
    }

    void testFmtFloat() {
        QString s = fmt::fmtFloat(3.14159f);
        QVERIFY(s.contains("3.14"));
    }

    void testFmtBool() {
        QCOMPARE(fmt::fmtBool(1), QString("true"));
        QCOMPARE(fmt::fmtBool(0), QString("false"));
    }

    void testFmtPointer64_null() {
        QCOMPARE(fmt::fmtPointer64(0), QString("-> NULL"));
    }

    void testFmtPointer64_nonNull() {
        QString s = fmt::fmtPointer64(0x400000);
        QVERIFY(s.startsWith("-> 0x"));
        QVERIFY(s.contains("400000"));
    }

    void testFmtOffsetMargin_primary() {
        QCOMPARE(fmt::fmtOffsetMargin(0x10, false), QString("10"));
        QCOMPARE(fmt::fmtOffsetMargin(0, false),    QString("0"));
    }

    void testFmtOffsetMargin_continuation() {
        QCOMPARE(fmt::fmtOffsetMargin(0x10, true), QString("  \u00B7"));
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
        QCOMPARE(fmt::indent(1), QString("   "));
        QCOMPARE(fmt::indent(3), QString("         "));
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
        // Hex parsing produces native-endian bytes (matches display which reads native-endian)
        QByteArray b = fmt::parseValue(NodeKind::Hex32, "DEADBEEF", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 4);
        // Value 0xDEADBEEF stored as native-endian (little-endian on x86)
        uint32_t v;
        memcpy(&v, b.data(), 4);
        QCOMPARE(v, (uint32_t)0xDEADBEEF);
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
        // Hex32 with 0x prefix should work (native-endian, matches display)
        QByteArray b = fmt::parseValue(NodeKind::Hex32, "0xDEADBEEF", &ok);
        QVERIFY(ok);
        uint32_t v32;
        memcpy(&v32, b.data(), 4);
        QCOMPARE(v32, (uint32_t)0xDEADBEEF);

        // Pointer64 with 0x prefix
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
        // Vec2 subLine=2 (out of bounds) should return "?"
        QByteArray data(16, '\0');
        BufferProvider prov(data);
        Node n;
        n.kind = NodeKind::Vec2;
        n.name = "v";
        QCOMPARE(fmt::readValue(n, prov, 0, 2), QString("?"));
        QCOMPARE(fmt::readValue(n, prov, 0, -1), QString("?"));

        // Vec3 subLine=3 (out of bounds)
        n.kind = NodeKind::Vec3;
        QCOMPARE(fmt::readValue(n, prov, 0, 3), QString("?"));

        // Vec3 subLine=2 (valid)
        QVERIFY(fmt::readValue(n, prov, 0, 2) != QString("?"));
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

        // Vec2 out of bounds → "?"
        n.kind = NodeKind::Vec2;
        QCOMPARE(fmt::editableValue(n, prov, 0, 2), QString("?"));
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
};

QTEST_MAIN(TestFormat)
#include "test_format.moc"
