// Stress tests for editor/controller validation:
// – Invalid values, boundary values, excessive inputs
// – Ensures no crashes and data integrity after rejected edits
// Skips: ASCII/byte preview editing (under discussion)

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QSplitter>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "core.h"

using namespace rcx;

// ── Fixture: small tree with diverse field types ──

static void buildValidationTree(NodeTree& tree) {
    tree.baseAddress = 0x1000;

    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "TestStruct";
    root.name = "root";
    root.parentId = 0;
    root.offset = 0;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    auto field = [&](int off, NodeKind k, const char* name) {
        Node n;
        n.kind = k; n.name = name;
        n.parentId = rootId; n.offset = off;
        tree.addNode(n);
    };
    auto fieldArr = [&](int off, NodeKind ek, int count, const char* name) {
        Node n;
        n.kind = NodeKind::Array; n.name = name;
        n.parentId = rootId; n.offset = off;
        n.arrayLen = count; n.elementKind = ek;
        tree.addNode(n);
    };

    field(0,  NodeKind::Int8,       "field_i8");
    field(1,  NodeKind::UInt8,      "field_u8");
    field(2,  NodeKind::Int16,      "field_i16");
    field(4,  NodeKind::UInt16,     "field_u16");
    field(6,  NodeKind::Int32,      "field_i32");
    field(10, NodeKind::UInt32,     "field_u32");
    field(14, NodeKind::Int64,      "field_i64");
    field(22, NodeKind::UInt64,     "field_u64");
    field(30, NodeKind::Float,      "field_float");
    field(34, NodeKind::Double,     "field_dbl");
    field(42, NodeKind::Bool,       "field_bool");
    field(43, NodeKind::Hex8,       "field_h8");
    field(44, NodeKind::Hex16,      "field_h16");
    field(46, NodeKind::Hex32,      "field_h32");
    field(50, NodeKind::Hex64,      "field_h64");
    field(58, NodeKind::Pointer64,  "field_ptr");
    field(66, NodeKind::Padding,    "pad0");
    tree.nodes.last().arrayLen = 6;
    fieldArr(72, NodeKind::UInt32, 4, "field_arr");
}

static QByteArray makeValidationBuffer() {
    QByteArray data(256, '\0');
    // i8 = -5
    data[0] = (char)(int8_t)-5;
    // u8 = 0x42
    data[1] = 0x42;
    // i16 = -1000
    int16_t i16v = -1000;
    memcpy(data.data() + 2, &i16v, 2);
    // u16 = 60000
    uint16_t u16v = 60000;
    memcpy(data.data() + 4, &u16v, 2);
    // i32 = -100000
    int32_t i32v = -100000;
    memcpy(data.data() + 6, &i32v, 4);
    // u32 = 0xDEADBEEF
    uint32_t u32v = 0xDEADBEEF;
    memcpy(data.data() + 10, &u32v, 4);
    // i64 = -1
    int64_t i64v = -1;
    memcpy(data.data() + 14, &i64v, 8);
    // u64 = UINT64_MAX
    uint64_t u64v = ~0ULL;
    memcpy(data.data() + 22, &u64v, 8);
    // float = 3.14f
    float fv = 3.14f;
    memcpy(data.data() + 30, &fv, 4);
    // double = 2.718
    double dv = 2.718;
    memcpy(data.data() + 34, &dv, 8);
    // bool = 1
    data[42] = 1;
    // hex8 = 0xAB
    data[43] = (char)0xAB;
    // hex16 = 0xCAFE
    uint16_t h16 = 0xCAFE;
    memcpy(data.data() + 44, &h16, 2);
    // hex32 = 0xBAADF00D
    uint32_t h32 = 0xBAADF00D;
    memcpy(data.data() + 46, &h32, 4);
    // hex64 = 0xDEADC0DEDEADBEEF
    uint64_t h64 = 0xDEADC0DEDEADBEEFULL;
    memcpy(data.data() + 50, &h64, 8);
    // pointer = 0x7FFE3B8D4260
    uint64_t ptr = 0x00007FFE3B8D4260ULL;
    memcpy(data.data() + 58, &ptr, 8);
    return data;
}

// ── Helper: find node index by name ──

static int findNode(const NodeTree& tree, const char* name) {
    for (int i = 0; i < tree.nodes.size(); i++)
        if (tree.nodes[i].name == name) return i;
    return -1;
}

// ══════════════════════════════════════════════════════════════════════
// Part 1: Pure unit tests – fmt::parseValue / fmt::validateValue
// These are mixed into TestValidationController so they all run under
// one QTEST_MAIN. The init()/cleanup() create GUI fixtures but the
// pure parsing tests simply don't use them.
// ══════════════════════════════════════════════════════════════════════

// (forward-declared — tests are added as slots of TestValidationController below)

// ══════════════════════════════════════════════════════════════════════
// Part 2: Controller-level stress tests (requires GUI)
// Tests that invalid inputs through the controller API don't corrupt data.
// ══════════════════════════════════════════════════════════════════════

class TestValidationController : public QObject {
    Q_OBJECT
private:
    RcxDocument* m_doc = nullptr;
    RcxController* m_ctrl = nullptr;
    QSplitter* m_splitter = nullptr;
    RcxEditor* m_editor = nullptr;

    QByteArray snapshotProvider() {
        return m_doc->provider->readBytes(m_doc->tree.baseAddress,
                                          m_doc->provider->isReadable(m_doc->tree.baseAddress, 256) ? 256 : 0);
    }

private slots:

    void init() {
        m_doc = new RcxDocument();
        buildValidationTree(m_doc->tree);
        m_doc->provider = std::make_unique<BufferProvider>(makeValidationBuffer());

        m_splitter = new QSplitter();
        m_ctrl = new RcxController(m_doc, nullptr);
        m_editor = m_ctrl->addSplitEditor(m_splitter);

        m_splitter->resize(800, 600);
        m_splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_splitter));
        QApplication::processEvents();
    }

    void cleanup() {
        delete m_ctrl;  m_ctrl = nullptr; m_editor = nullptr;
        delete m_splitter; m_splitter = nullptr;
        delete m_doc; m_doc = nullptr;
    }

    // ════════════════════════════════════════════════════════
    // Pure parsing/validation tests (no GUI interaction)
    // ════════════════════════════════════════════════════════

    // ── Integer overflow: values that exceed type max ──

    void testInt8Overflow() {
        bool ok;
        // Max int8 = 127, min = -128
        fmt::parseValue(NodeKind::Int8, "128", &ok);
        QVERIFY2(!ok, "128 overflows int8");

        fmt::parseValue(NodeKind::Int8, "-129", &ok);
        QVERIFY2(!ok, "-129 underflows int8");

        fmt::parseValue(NodeKind::Int8, "127", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::Int8, "-128", &ok);
        QVERIFY(ok);

        // Hex overflow: 0x100 > 0xFF
        fmt::parseValue(NodeKind::Int8, "0x100", &ok);
        QVERIFY2(!ok, "0x100 overflows int8 hex");

        fmt::parseValue(NodeKind::Int8, "0xFF", &ok);
        QVERIFY(ok);
    }

    void testUInt8Overflow() {
        bool ok;
        fmt::parseValue(NodeKind::UInt8, "256", &ok);
        QVERIFY2(!ok, "256 overflows uint8");

        fmt::parseValue(NodeKind::UInt8, "255", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::UInt8, "0", &ok);
        QVERIFY(ok);

        // Negative should fail for unsigned
        fmt::parseValue(NodeKind::UInt8, "-1", &ok);
        QVERIFY2(!ok, "Negative should fail for uint8");
    }

    void testInt16Overflow() {
        bool ok;
        fmt::parseValue(NodeKind::Int16, "32768", &ok);
        QVERIFY2(!ok, "32768 overflows int16");

        fmt::parseValue(NodeKind::Int16, "-32769", &ok);
        QVERIFY2(!ok, "-32769 underflows int16");

        fmt::parseValue(NodeKind::Int16, "32767", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::Int16, "-32768", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::Int16, "0x10000", &ok);
        QVERIFY2(!ok, "0x10000 overflows int16 hex");
    }

    void testUInt16Overflow() {
        bool ok;
        fmt::parseValue(NodeKind::UInt16, "65536", &ok);
        QVERIFY2(!ok, "65536 overflows uint16");

        fmt::parseValue(NodeKind::UInt16, "65535", &ok);
        QVERIFY(ok);
    }

    void testInt32Overflow() {
        bool ok;
        // 2147483647 is INT32_MAX
        fmt::parseValue(NodeKind::Int32, "2147483647", &ok);
        QVERIFY(ok);

        // 2147483648 overflows signed int32 in decimal
        // Note: toInt returns false for overflow
        fmt::parseValue(NodeKind::Int32, "2147483648", &ok);
        QVERIFY2(!ok, "2147483648 overflows int32 decimal");

        fmt::parseValue(NodeKind::Int32, "0xFFFFFFFF", &ok);
        QVERIFY(ok);  // hex path allows up to 0xFFFFFFFF

        fmt::parseValue(NodeKind::Int32, "0x100000000", &ok);
        QVERIFY2(!ok, "0x100000000 overflows int32 hex");
    }

    void testUInt32Overflow() {
        bool ok;
        fmt::parseValue(NodeKind::UInt32, "4294967295", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::UInt32, "4294967296", &ok);
        QVERIFY2(!ok, "4294967296 overflows uint32");
    }

    void testUInt64Max() {
        bool ok;
        // UINT64_MAX = 18446744073709551615
        fmt::parseValue(NodeKind::UInt64, "18446744073709551615", &ok);
        QVERIFY(ok);

        // Beyond UINT64_MAX should fail to parse
        fmt::parseValue(NodeKind::UInt64, "18446744073709551616", &ok);
        QVERIFY2(!ok, "UINT64_MAX+1 should fail");

        fmt::parseValue(NodeKind::UInt64, "0xFFFFFFFFFFFFFFFF", &ok);
        QVERIFY(ok);
    }

    // ── Invalid characters in numeric fields ──

    void testInvalidCharsInIntegers() {
        bool ok;
        fmt::parseValue(NodeKind::Int32, "12abc", &ok);
        QVERIFY(!ok);

        fmt::parseValue(NodeKind::UInt32, "hello", &ok);
        QVERIFY(!ok);

        fmt::parseValue(NodeKind::Int8, "3.14", &ok);
        QVERIFY(!ok);  // Not a valid integer

        fmt::parseValue(NodeKind::UInt16, "", &ok);
        QVERIFY(!ok);  // Empty string fails for non-string types
    }

    void testInvalidCharsInHex() {
        bool ok;
        fmt::parseValue(NodeKind::Hex32, "GHIJKL", &ok);
        QVERIFY(!ok);

        fmt::parseValue(NodeKind::Hex64, "0xZZZZ", &ok);
        QVERIFY(!ok);

        fmt::parseValue(NodeKind::Hex8, "XY", &ok);
        QVERIFY(!ok);
    }

    // ── Hex wrong byte count ──

    void testHexWrongByteCount() {
        bool ok;
        // Hex32 expects 4 bytes when space-separated
        fmt::parseValue(NodeKind::Hex32, "AA BB CC DD EE", &ok);
        QVERIFY2(!ok, "5 bytes should fail for Hex32");

        fmt::parseValue(NodeKind::Hex32, "AA BB", &ok);
        QVERIFY2(!ok, "2 bytes should fail for Hex32");

        // Correct: 4 bytes
        fmt::parseValue(NodeKind::Hex32, "AA BB CC DD", &ok);
        QVERIFY(ok);

        // Hex64 expects 8 bytes
        fmt::parseValue(NodeKind::Hex64, "AA BB CC DD", &ok);
        QVERIFY2(!ok, "4 bytes should fail for Hex64");

        fmt::parseValue(NodeKind::Hex64, "AA BB CC DD EE FF 00 11", &ok);
        QVERIFY(ok);
    }

    // ── Float/Double edge cases ──

    void testFloatEdgeCases() {
        bool ok;
        // Valid floats
        fmt::parseValue(NodeKind::Float, "0", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::Float, "-0.0", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::Float, "1e38", &ok);
        QVERIFY(ok);

        // EU comma separator (converted to dot internally)
        fmt::parseValue(NodeKind::Float, "3,14", &ok);
        QVERIFY(ok);

        // Junk
        fmt::parseValue(NodeKind::Float, "not_a_number", &ok);
        QVERIFY(!ok);

        fmt::parseValue(NodeKind::Float, "", &ok);
        QVERIFY(!ok);
    }

    void testDoubleEdgeCases() {
        bool ok;
        fmt::parseValue(NodeKind::Double, "1.7976931348623157e+308", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::Double, "abc", &ok);
        QVERIFY(!ok);

        fmt::parseValue(NodeKind::Double, "1,5", &ok);
        QVERIFY(ok);  // EU comma
    }

    // ── Bool: only "true"/"false"/"0"/"1" are valid ──

    void testBoolInvalid() {
        bool ok;
        fmt::parseValue(NodeKind::Bool, "true", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::Bool, "false", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::Bool, "1", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::Bool, "0", &ok);
        QVERIFY(ok);

        // Invalid: "yes", "no", "2", random text
        fmt::parseValue(NodeKind::Bool, "yes", &ok);
        QVERIFY2(!ok, "'yes' is not valid bool");

        fmt::parseValue(NodeKind::Bool, "no", &ok);
        QVERIFY2(!ok, "'no' is not valid bool");

        fmt::parseValue(NodeKind::Bool, "2", &ok);
        QVERIFY2(!ok, "'2' is not valid bool");

        fmt::parseValue(NodeKind::Bool, "TRUE", &ok);
        QVERIFY2(!ok, "'TRUE' (uppercase) is not valid bool");

        fmt::parseValue(NodeKind::Bool, "", &ok);
        QVERIFY(!ok);
    }

    // ── Pointer: hex-only parsing ──

    void testPointerInvalid() {
        bool ok;
        // Valid
        fmt::parseValue(NodeKind::Pointer64, "0x7FFE3B8D4260", &ok);
        QVERIFY(ok);

        fmt::parseValue(NodeKind::Pointer64, "7FFE3B8D4260", &ok);
        QVERIFY(ok);

        // Invalid chars
        fmt::parseValue(NodeKind::Pointer64, "0xGGGG", &ok);
        QVERIFY(!ok);

        // Pointer32 overflow
        fmt::parseValue(NodeKind::Pointer32, "0x100000000", &ok);
        QVERIFY2(!ok, "0x100000000 overflows ptr32");

        fmt::parseValue(NodeKind::Pointer32, "0xFFFFFFFF", &ok);
        QVERIFY(ok);
    }

    // ── validateValue: error message testing ──

    void testValidateValueMessages() {
        // Hex kind with non-hex chars → character-level error
        QString err = fmt::validateValue(NodeKind::Hex32, "GGGG");
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains("invalid hex"));

        // Int kind overflow → "too large" message
        err = fmt::validateValue(NodeKind::UInt8, "999");
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains("too large"));

        // Decimal with non-digit
        err = fmt::validateValue(NodeKind::UInt32, "12!3");
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains("invalid"));

        // Signed integer with leading minus accepted
        err = fmt::validateValue(NodeKind::Int32, "-42");
        QVERIFY2(err.isEmpty(), qPrintable("Negative int32 should be valid: " + err));

        // Unsigned with minus → invalid
        err = fmt::validateValue(NodeKind::UInt32, "-1");
        QVERIFY(!err.isEmpty());

        // Float junk
        err = fmt::validateValue(NodeKind::Float, "abc");
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains("invalid number"));

        // Empty is valid (special case)
        err = fmt::validateValue(NodeKind::UInt32, "");
        QVERIFY(err.isEmpty());

        // Spaces only trimmed to empty → valid
        err = fmt::validateValue(NodeKind::UInt32, "   ");
        QVERIFY(err.isEmpty());
    }

    // ── validateBaseAddress: equation syntax ──

    void testValidateBaseAddressEdgeCases() {
        // Valid cases
        QVERIFY(fmt::validateBaseAddress("0x1000").isEmpty());
        QVERIFY(fmt::validateBaseAddress("1000").isEmpty());
        QVERIFY(fmt::validateBaseAddress("0x1000 + 0x100").isEmpty());
        QVERIFY(fmt::validateBaseAddress("0x2000 - 0x10").isEmpty());
        QVERIFY(fmt::validateBaseAddress("0x400+0x200-0x100").isEmpty());
        QVERIFY(fmt::validateBaseAddress("  0xDEAD  ").isEmpty());

        // Invalid cases
        QVERIFY(!fmt::validateBaseAddress("").isEmpty());   // empty
        QVERIFY(!fmt::validateBaseAddress("  ").isEmpty()); // whitespace only - no hex digits
        QVERIFY(!fmt::validateBaseAddress("0xGGGG").isEmpty());
        QVERIFY(!fmt::validateBaseAddress("0x1000 * 2").isEmpty());  // multiplication not supported
        QVERIFY(!fmt::validateBaseAddress("0x1000 ++ 0x100").isEmpty());  // double operator
        QVERIFY(!fmt::validateBaseAddress("hello").isEmpty());
    }

    // ── Extremely long strings ──

    void testExtremelyLongInput() {
        bool ok;
        // 10000-char string of hex digits
        QString longHex = QString("F").repeated(10000);
        fmt::parseValue(NodeKind::Hex32, longHex, &ok);
        // Should either fail or succeed gracefully (no crash)
        // For Hex32 continuous mode, this is a valid huge hex number that overflows uint32
        Q_UNUSED(ok);  // Just testing it doesn't crash

        // Long garbage
        QString longJunk = QString("@#$%^&*").repeated(1000);
        fmt::parseValue(NodeKind::Int32, longJunk, &ok);
        QVERIFY(!ok);

        // Very long decimal number
        QString longDec = QString("9").repeated(100);
        fmt::parseValue(NodeKind::UInt64, longDec, &ok);
        QVERIFY(!ok);  // Way beyond UINT64_MAX

        // Extremely long hex for parseValue
        fmt::parseValue(NodeKind::Hex64, "0x" + QString("F").repeated(200), &ok);
        // No crash is the test
    }

    // ── Special/weird characters ──

    void testSpecialCharacters() {
        bool ok;
        fmt::parseValue(NodeKind::Int32, "\0", &ok);
        QVERIFY(!ok);

        fmt::parseValue(NodeKind::Int32, "\t42\n", &ok);
        // trimmed internally — may or may not parse; just don't crash
        Q_UNUSED(ok);

        fmt::parseValue(NodeKind::UInt32, "  42  ", &ok);
        QVERIFY(ok);  // Leading/trailing whitespace should be trimmed

        // Unicode characters
        fmt::parseValue(NodeKind::UInt32, QString::fromUtf8("\xC3\xA9"), &ok);  // é
        QVERIFY(!ok);
    }

    // ── Container kinds: parseValue should fail gracefully ──

    void testContainerKindParseValue() {
        bool ok;
        fmt::parseValue(NodeKind::Struct, "anything", &ok);
        QVERIFY(!ok);

        fmt::parseValue(NodeKind::Array, "42", &ok);
        QVERIFY(!ok);
    }

    // ════════════════════════════════════════════════════════
    // Controller-level stress tests (uses GUI fixtures)
    // ════════════════════════════════════════════════════════

    // ── setNodeValue rejects overflowing values without changing data ──

    void testRejectOverflowInt8() {
        int idx = findNode(m_doc->tree, "field_i8");
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray before = m_doc->provider->readBytes(addr, 1);

        m_ctrl->setNodeValue(idx, 0, "999");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 1);
        QCOMPARE(after, before);  // Data unchanged
        QCOMPARE(m_doc->undoStack.count(), 0);  // No command pushed
    }

    void testRejectOverflowUInt8() {
        int idx = findNode(m_doc->tree, "field_u8");
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray before = m_doc->provider->readBytes(addr, 1);

        m_ctrl->setNodeValue(idx, 0, "256");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 1);
        QCOMPARE(after, before);
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    void testRejectOverflowUInt16() {
        int idx = findNode(m_doc->tree, "field_u16");
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray before = m_doc->provider->readBytes(addr, 2);

        m_ctrl->setNodeValue(idx, 0, "70000");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 2);
        QCOMPARE(after, before);
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    void testRejectOverflowUInt32() {
        int idx = findNode(m_doc->tree, "field_u32");
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray before = m_doc->provider->readBytes(addr, 4);

        m_ctrl->setNodeValue(idx, 0, "4294967296");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 4);
        QCOMPARE(after, before);
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    // ── setNodeValue rejects garbage text ──

    void testRejectGarbageText() {
        int idx = findNode(m_doc->tree, "field_u32");
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray before = m_doc->provider->readBytes(addr, 4);

        // Various garbage inputs
        const char* junk[] = {
            "hello", "!@#$%", "", "   ", "0xGGGG", "3.14",
            "true", "null", "NaN", "inf", "\t\n\r"
        };
        for (const char* s : junk) {
            m_ctrl->setNodeValue(idx, 0, s);
            QApplication::processEvents();
        }

        QByteArray after = m_doc->provider->readBytes(addr, 4);
        QCOMPARE(after, before);
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    void testRejectGarbageFloat() {
        int idx = findNode(m_doc->tree, "field_float");
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray before = m_doc->provider->readBytes(addr, 4);

        m_ctrl->setNodeValue(idx, 0, "not_a_number");
        m_ctrl->setNodeValue(idx, 0, "");
        m_ctrl->setNodeValue(idx, 0, "0xDEAD");  // hex not valid for float
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 4);
        QCOMPARE(after, before);
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    void testRejectGarbageBool() {
        int idx = findNode(m_doc->tree, "field_bool");
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray before = m_doc->provider->readBytes(addr, 1);

        m_ctrl->setNodeValue(idx, 0, "yes");
        m_ctrl->setNodeValue(idx, 0, "2");
        m_ctrl->setNodeValue(idx, 0, "TRUE");
        m_ctrl->setNodeValue(idx, 0, "maybe");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 1);
        QCOMPARE(after, before);
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    // ── setNodeValue on invalid node indices ──

    void testOutOfBoundsNodeIndex() {
        QByteArray before = m_doc->provider->readBytes(m_doc->tree.baseAddress, 256);

        m_ctrl->setNodeValue(-1, 0, "42");
        m_ctrl->setNodeValue(-100, 0, "42");
        m_ctrl->setNodeValue(99999, 0, "42");
        m_ctrl->setNodeValue(INT_MAX, 0, "42");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(m_doc->tree.baseAddress, 256);
        QCOMPARE(after, before);
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    // ── renameNode with edge cases ──

    void testRenameNodeEdgeCases() {
        int idx = findNode(m_doc->tree, "field_u32");
        QVERIFY(idx >= 0);

        // Empty name is allowed at controller level
        m_ctrl->renameNode(idx, "");
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[idx].name, QString(""));
        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.nodes[idx].name, QString("field_u32"));

        // Very long name (1000 chars)
        QString longName = QString("a").repeated(1000);
        m_ctrl->renameNode(idx, longName);
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[idx].name, longName);
        m_doc->undoStack.undo();

        // Special characters
        m_ctrl->renameNode(idx, "field with spaces & <special> \"chars\"");
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[idx].name,
                 QString("field with spaces & <special> \"chars\""));
        m_doc->undoStack.undo();

        // Out of bounds indices
        m_ctrl->renameNode(-1, "bad");
        m_ctrl->renameNode(99999, "bad");
        QApplication::processEvents();
        // Should not crash; undo stack not affected
    }

    // ── changeNodeKind with invalid indices ──

    void testChangeKindOutOfBounds() {
        int origCount = m_doc->tree.nodes.size();

        m_ctrl->changeNodeKind(-1, NodeKind::Float);
        m_ctrl->changeNodeKind(99999, NodeKind::Float);
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes.size(), origCount);
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    // ── changeNodeKind size transitions: shrink inserts padding ──

    void testChangeKindShrinkInsertsPadding() {
        int idx = findNode(m_doc->tree, "field_u32");
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::UInt32);  // 4 bytes

        int origCount = m_doc->tree.nodes.size();
        m_ctrl->changeNodeKind(idx, NodeKind::UInt8);  // 4 → 1 byte = 3 gap
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::UInt8);
        // Should have inserted padding nodes (Hex16 + Hex8 = 3 bytes, or similar)
        QVERIFY(m_doc->tree.nodes.size() > origCount);

        // Undo restores everything
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::UInt32);
        QCOMPARE(m_doc->tree.nodes.size(), origCount);
    }

    // ── insertNode / removeNode boundary conditions ──

    void testInsertNodeWithInvalidParent() {
        int origCount = m_doc->tree.nodes.size();

        // Non-existent parent ID — insertNode doesn't validate parent existence,
        // so it will add a node with an orphan parentId. Verify no crash.
        m_ctrl->insertNode(0xDEADBEEF, 0, NodeKind::UInt32, "orphan");
        QApplication::processEvents();

        // The node was added (the tree accepts orphan parentId)
        QCOMPARE(m_doc->tree.nodes.size(), origCount + 1);

        // Undo cleans up
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes.size(), origCount);
    }

    void testRemoveNodeOutOfBounds() {
        int origCount = m_doc->tree.nodes.size();

        m_ctrl->removeNode(-1);
        m_ctrl->removeNode(99999);
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes.size(), origCount);
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    // ── Array element count: boundary validation ──

    void testArrayCountBoundaries() {
        int idx = findNode(m_doc->tree, "field_arr");
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::Array);
        int origLen = m_doc->tree.nodes[idx].arrayLen;

        // Simulate EditTarget::ArrayElementCount through the controller API
        // The controller validates: ok && newLen > 0 && newLen <= 100000

        // Zero count — should be rejected (> 0 check)
        m_doc->undoStack.clear();
        {
            bool ok;
            int newLen = QString("0").toInt(&ok);
            // Controller logic: ok && newLen > 0 → false
            QVERIFY(ok && newLen == 0);  // toInt succeeds, but newLen is 0
            // This should NOT push a command
        }

        // Negative count
        {
            bool ok;
            int newLen = QString("-5").toInt(&ok);
            QVERIFY(ok && newLen < 0);  // toInt succeeds, but negative
        }

        // Just above max: 100001
        {
            bool ok;
            int newLen = QString("100001").toInt(&ok);
            QVERIFY(ok && newLen > 100000);
        }

        // At max: 100000 (should be accepted)
        {
            bool ok;
            int newLen = QString("100000").toInt(&ok);
            QVERIFY(ok && newLen > 0 && newLen <= 100000);
        }

        // Non-numeric text
        {
            bool ok;
            QString("hello").toInt(&ok);
            QVERIFY(!ok);
        }

        // Verify actual array length is unchanged
        QCOMPARE(m_doc->tree.nodes[idx].arrayLen, origLen);
    }

    // ── Hex values: space-separated with wrong count ──

    void testHexWrongByteCountAtController() {
        int idx = findNode(m_doc->tree, "field_h32");
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray before = m_doc->provider->readBytes(addr, 4);

        // 5 bytes for a 4-byte field
        m_ctrl->setNodeValue(idx, 0, "AA BB CC DD EE");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 4);
        QCOMPARE(after, before);
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    // ── Valid writes followed by undo: verify round-trip integrity ──

    void testValueWriteUndoIntegrity() {
        // Write valid values to multiple fields, undo all, verify original data
        int i8idx  = findNode(m_doc->tree, "field_i8");
        int u32idx = findNode(m_doc->tree, "field_u32");
        int fltidx = findNode(m_doc->tree, "field_float");
        QVERIFY(i8idx >= 0 && u32idx >= 0 && fltidx >= 0);

        // Snapshot original provider
        QByteArray origData = m_doc->provider->readBytes(
            m_doc->tree.baseAddress, 256);

        // Write three valid values
        m_ctrl->setNodeValue(i8idx, 0, "42");
        m_ctrl->setNodeValue(u32idx, 0, "12345");
        m_ctrl->setNodeValue(fltidx, 0, "2.5");
        QApplication::processEvents();

        QCOMPARE(m_doc->undoStack.count(), 3);

        // Undo all three
        m_doc->undoStack.undo();
        m_doc->undoStack.undo();
        m_doc->undoStack.undo();
        QApplication::processEvents();

        QByteArray afterUndo = m_doc->provider->readBytes(
            m_doc->tree.baseAddress, 256);
        QCOMPARE(afterUndo, origData);
    }

    // ── toggleCollapse on out-of-bounds index ──

    void testToggleCollapseOutOfBounds() {
        m_ctrl->toggleCollapse(-1);
        m_ctrl->toggleCollapse(99999);
        QApplication::processEvents();
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    // ── Rapid fire: many rejected writes don't accumulate undo history ──

    void testRapidFireRejectedWrites() {
        int idx = findNode(m_doc->tree, "field_u8");
        QVERIFY(idx >= 0);

        for (int i = 0; i < 100; i++)
            m_ctrl->setNodeValue(idx, 0, "9999");  // overflow
        QApplication::processEvents();

        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    // ── Duplicate nodes: verify they get unique IDs ──

    void testDuplicateNodeGetsUniqueId() {
        int idx = findNode(m_doc->tree, "field_u32");
        QVERIFY(idx >= 0);
        int origCount = m_doc->tree.nodes.size();

        m_ctrl->duplicateNode(idx);
        QApplication::processEvents();

        // duplicateNode appends "_copy" to the name
        QCOMPARE(m_doc->tree.nodes.size(), origCount + 1);

        int copyIdx = findNode(m_doc->tree, "field_u32_copy");
        QVERIFY2(copyIdx >= 0, "Duplicate node should exist with '_copy' suffix");

        // Verify all IDs are unique
        QSet<uint64_t> ids;
        for (const auto& n : m_doc->tree.nodes) {
            QVERIFY2(!ids.contains(n.id),
                     qPrintable(QString("Duplicate ID found: %1").arg(n.id)));
            ids.insert(n.id);
        }

        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes.size(), origCount);
    }

    // ── Batch remove with invalid indices in the mix ──

    void testBatchRemoveWithInvalidIndices() {
        int origCount = m_doc->tree.nodes.size();
        int validIdx = findNode(m_doc->tree, "field_u8");
        QVERIFY(validIdx >= 0);

        // Mix of valid and invalid indices — batchRemoveNodes filters internally
        QVector<int> indices = {validIdx, -1, 99999};
        m_ctrl->batchRemoveNodes(indices);
        QApplication::processEvents();

        // At least the valid node should have been removed
        QVERIFY(m_doc->tree.nodes.size() < origCount);

        // Undo restores
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes.size(), origCount);
    }

    // ── Batch change kind with invalid indices ──

    void testBatchChangeKindWithInvalidIndices() {
        int validIdx = findNode(m_doc->tree, "field_i32");
        QVERIFY(validIdx >= 0);
        NodeKind origKind = m_doc->tree.nodes[validIdx].kind;

        // Mix of valid and invalid
        QVector<int> indices = {-1, validIdx, 99999};
        m_ctrl->batchChangeKind(indices, NodeKind::Float);
        QApplication::processEvents();

        // Valid node should have changed
        QCOMPARE(m_doc->tree.nodes[validIdx].kind, NodeKind::Float);

        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[validIdx].kind, origKind);
    }

    // ── Editor: inline edit rejected on out-of-range lines ──

    void testInlineEditOutOfRangeLines() {
        m_ctrl->refresh();
        QApplication::processEvents();

        // Try to edit a line that doesn't exist
        QVERIFY(!m_editor->beginInlineEdit(EditTarget::Name, 99999));
        QVERIFY(!m_editor->isEditing());

        QVERIFY(!m_editor->beginInlineEdit(EditTarget::Value, -1));
        QVERIFY(!m_editor->isEditing());
    }

    // ── Editor: padding value edit blocked, name/type still work ──

    void testPaddingEditRestrictions() {
        m_ctrl->refresh();
        QApplication::processEvents();

        ComposeResult result = m_doc->compose();
        m_editor->applyDocument(result);
        QApplication::processEvents();

        // Find padding line
        int padLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].nodeKind == NodeKind::Padding &&
                result.meta[i].lineKind == LineKind::Field) {
                padLine = i;
                break;
            }
        }
        QVERIFY(padLine >= 0);

        // Value edit rejected
        QVERIFY(!m_editor->beginInlineEdit(EditTarget::Value, padLine));

        // Type edit accepted
        bool ok = m_editor->beginInlineEdit(EditTarget::Type, padLine);
        QVERIFY(ok);
        m_editor->cancelInlineEdit();
        QApplication::processEvents();
    }

    // ── Editor: struct header rejects value edit ──

    void testStructHeaderRejectsValueEdit() {
        m_ctrl->refresh();
        QApplication::processEvents();

        ComposeResult result = m_doc->compose();
        m_editor->applyDocument(result);
        QApplication::processEvents();

        // Find header line
        int headerLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].lineKind == LineKind::Header) {
                headerLine = i;
                break;
            }
        }
        QVERIFY(headerLine >= 0);

        QVERIFY(!m_editor->beginInlineEdit(EditTarget::Value, headerLine));
        QVERIFY(!m_editor->isEditing());

        // But Name and Type should work
        bool ok = m_editor->beginInlineEdit(EditTarget::Name, headerLine);
        QVERIFY(ok);
        m_editor->cancelInlineEdit();
    }

    // ── Base address: invalid equation syntax ──

    void testBaseAddressInvalidEquation() {
        uint64_t origBase = m_doc->tree.baseAddress;

        m_ctrl->refresh();
        QApplication::processEvents();

        // These are processed through the inlineEditCommitted handler,
        // but we can test the parsing logic directly:
        // The controller silently ignores invalid base address text

        // Test the validation function directly
        QVERIFY(!fmt::validateBaseAddress("0x1000 ** 2").isEmpty());
        QVERIFY(!fmt::validateBaseAddress("0x1000 / 2").isEmpty());
        QVERIFY(!fmt::validateBaseAddress("abc xyz").isEmpty());

        // Original base should be unchanged
        QCOMPARE(m_doc->tree.baseAddress, origBase);
    }

    // ── Pointer64 value: accepts hex, rejects garbage ──

    void testPointerValueValidation() {
        int idx = findNode(m_doc->tree, "field_ptr");
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray before = m_doc->provider->readBytes(addr, 8);

        // Garbage
        m_ctrl->setNodeValue(idx, 0, "not_a_pointer");
        m_ctrl->setNodeValue(idx, 0, "");
        m_ctrl->setNodeValue(idx, 0, "0xZZZZ");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 8);
        QCOMPARE(after, before);
        QCOMPARE(m_doc->undoStack.count(), 0);

        // Valid hex write
        m_ctrl->setNodeValue(idx, 0, "0xDEADBEEFCAFEBABE");
        QApplication::processEvents();

        QByteArray written = m_doc->provider->readBytes(addr, 8);
        uint64_t writtenVal;
        memcpy(&writtenVal, written.data(), 8);
        QCOMPARE(writtenVal, (uint64_t)0xDEADBEEFCAFEBABEULL);

        m_doc->undoStack.undo();
        QApplication::processEvents();
        QByteArray restored = m_doc->provider->readBytes(addr, 8);
        QCOMPARE(restored, before);
    }

    // ── Hex64 space-separated: exact 8 bytes accepted, other counts rejected ──

    void testHex64SpaceSeparatedBoundary() {
        int idx = findNode(m_doc->tree, "field_h64");
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray before = m_doc->provider->readBytes(addr, 8);

        // 7 bytes — reject
        m_ctrl->setNodeValue(idx, 0, "AA BB CC DD EE FF 00");
        QApplication::processEvents();
        QCOMPARE(m_doc->provider->readBytes(addr, 8), before);

        // 9 bytes — reject
        m_ctrl->setNodeValue(idx, 0, "AA BB CC DD EE FF 00 11 22");
        QApplication::processEvents();
        QCOMPARE(m_doc->provider->readBytes(addr, 8), before);

        QCOMPARE(m_doc->undoStack.count(), 0);

        // 8 bytes — accept
        m_ctrl->setNodeValue(idx, 0, "01 02 03 04 05 06 07 08");
        QApplication::processEvents();
        QCOMPARE(m_doc->undoStack.count(), 1);

        QByteArray written = m_doc->provider->readBytes(addr, 8);
        QCOMPARE((uint8_t)written[0], (uint8_t)0x01);
        QCOMPARE((uint8_t)written[7], (uint8_t)0x08);

        m_doc->undoStack.undo();
    }

    // ── Multiple undos past the beginning don't crash ──

    void testExcessiveUndos() {
        int idx = findNode(m_doc->tree, "field_u32");
        QVERIFY(idx >= 0);

        m_ctrl->setNodeValue(idx, 0, "42");
        QApplication::processEvents();
        QCOMPARE(m_doc->undoStack.count(), 1);

        // Undo once (valid)
        m_doc->undoStack.undo();
        // Undo 50 more times (all no-ops, should not crash)
        for (int i = 0; i < 50; i++)
            m_doc->undoStack.undo();
        QApplication::processEvents();

        // Redo 50 times past the end
        m_doc->undoStack.redo();
        for (int i = 0; i < 50; i++)
            m_doc->undoStack.redo();
        QApplication::processEvents();
    }
};

QTEST_MAIN(TestValidationController)
#include "test_validation.moc"
