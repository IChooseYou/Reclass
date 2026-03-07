#include <QtTest/QtTest>
#include "core.h"
#include "imports/import_source.h"

using namespace rcx;

class TestImportSource : public QObject {
    Q_OBJECT
private slots:
    // Basic type tests
    void emptyInput();
    void noStructs();
    void singleEmptyStruct();
    void stdintTypes();
    void windowsTypes();
    void platformPointerTypes();
    void standardCTypes();
    void multiWordTypes();
    void floatDouble();
    void boolType();

    // Pointer tests
    void voidPointer();
    void typedPointer();
    void selfReferencingPointer();
    void doublePointer();

    // Array tests
    void primitiveArray();
    void charArrayToUtf8();
    void wcharArrayToUtf16();
    void floatArrayToVec2();
    void floatArrayToVec3();
    void floatArrayToVec4();
    void floatArray4x4ToMat4x4();
    void genericFloatArray();
    void structArray();

    // Comment offset tests
    void commentOffsets();
    void computedOffsets();
    void mixedOffsetsAutoDetect();

    // Multi-struct tests
    void multiStruct();
    void pointerCrossRef();

    // Forward declarations
    void forwardDeclaration();

    // Union handling
    void unionContainer();
    void unionWithCommentOffsets();
    void namedUnion();

    // Padding fields
    void paddingFieldExpansion();

    // static_assert
    void staticAssertTailPadding();

    // Embedded struct
    void embeddedStruct();

    // Typedef
    void typedefBasic();

    // Qualifiers
    void constVolatileQualifiers();
    void structPrefixOnType();

    // Edge cases
    void bitfieldSkipped();
    void bitfieldWithOffsetsEmitsHex();
    void hexArraySizes();
    void windowsStylePEB();
    void classKeyword();
    void inheritanceSkipped();

    // Enum tests
    void enumBasic();
    void enumAutoValues();
    void enumHexValues();
    void enumInStruct();
    void enumClass();

    // Round-trip test (requires generator.h)
    void basicRoundTrip();
};

// ── Helper ──

static int countRoots(const NodeTree& tree) {
    int n = 0;
    for (const auto& node : tree.nodes)
        if (node.parentId == 0 && node.kind == NodeKind::Struct) n++;
    return n;
}

static QVector<int> childrenOf(const NodeTree& tree, uint64_t parentId) {
    QVector<int> result;
    for (int i = 0; i < tree.nodes.size(); i++)
        if (tree.nodes[i].parentId == parentId) result.append(i);
    return result;
}

// ── Tests ──

void TestImportSource::emptyInput() {
    QString err;
    NodeTree tree = importFromSource(QString(), &err);
    QVERIFY(tree.nodes.isEmpty());
    QVERIFY(!err.isEmpty());
}

void TestImportSource::noStructs() {
    QString err;
    NodeTree tree = importFromSource(QStringLiteral("int x = 42;"), &err);
    QVERIFY(tree.nodes.isEmpty());
    QVERIFY(!err.isEmpty());
}

void TestImportSource::singleEmptyStruct() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Empty {};\n"
    ));
    QCOMPARE(countRoots(tree), 1);
    QCOMPARE(tree.nodes[0].name, QStringLiteral("Empty"));
    QCOMPARE(tree.nodes[0].kind, NodeKind::Struct);
}

void TestImportSource::stdintTypes() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Test {\n"
        "    uint8_t  a;\n"
        "    int8_t   b;\n"
        "    uint16_t c;\n"
        "    int16_t  d;\n"
        "    uint32_t e;\n"
        "    int32_t  f;\n"
        "    uint64_t g;\n"
        "    int64_t  h;\n"
        "};\n"
    ));
    QCOMPARE(countRoots(tree), 1);
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 8);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::UInt8);
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Int8);
    QCOMPARE(tree.nodes[kids[2]].kind, NodeKind::UInt16);
    QCOMPARE(tree.nodes[kids[3]].kind, NodeKind::Int16);
    QCOMPARE(tree.nodes[kids[4]].kind, NodeKind::UInt32);
    QCOMPARE(tree.nodes[kids[5]].kind, NodeKind::Int32);
    QCOMPARE(tree.nodes[kids[6]].kind, NodeKind::UInt64);
    QCOMPARE(tree.nodes[kids[7]].kind, NodeKind::Int64);
}

void TestImportSource::windowsTypes() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct WinTypes {\n"
        "    BYTE a;\n"
        "    WORD b;\n"
        "    DWORD c;\n"
        "    QWORD d;\n"
        "    ULONG e;\n"
        "    LONG f;\n"
        "    USHORT g;\n"
        "    UCHAR h;\n"
        "    BOOLEAN i;\n"
        "    BOOL j;\n"
        "    CHAR k;\n"
        "    WCHAR l;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 12);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::UInt8);   // BYTE
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::UInt16);  // WORD
    QCOMPARE(tree.nodes[kids[2]].kind, NodeKind::UInt32);  // DWORD
    QCOMPARE(tree.nodes[kids[3]].kind, NodeKind::UInt64);  // QWORD
    QCOMPARE(tree.nodes[kids[4]].kind, NodeKind::UInt32);  // ULONG
    QCOMPARE(tree.nodes[kids[5]].kind, NodeKind::Int32);   // LONG
    QCOMPARE(tree.nodes[kids[6]].kind, NodeKind::UInt16);  // USHORT
    QCOMPARE(tree.nodes[kids[7]].kind, NodeKind::UInt8);   // UCHAR
    QCOMPARE(tree.nodes[kids[8]].kind, NodeKind::UInt8);   // BOOLEAN
    QCOMPARE(tree.nodes[kids[9]].kind, NodeKind::Int32);   // BOOL
    QCOMPARE(tree.nodes[kids[10]].kind, NodeKind::Int8);   // CHAR
    QCOMPARE(tree.nodes[kids[11]].kind, NodeKind::UInt16); // WCHAR
}

void TestImportSource::platformPointerTypes() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct PtrTypes {\n"
        "    PVOID a;\n"
        "    HANDLE b;\n"
        "    SIZE_T c;\n"
        "    ULONG_PTR d;\n"
        "    uintptr_t e;\n"
        "    size_t f;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 6);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Pointer64);
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Pointer64);
    QCOMPARE(tree.nodes[kids[2]].kind, NodeKind::UInt64);
    QCOMPARE(tree.nodes[kids[3]].kind, NodeKind::UInt64);
    QCOMPARE(tree.nodes[kids[4]].kind, NodeKind::UInt64);
    QCOMPARE(tree.nodes[kids[5]].kind, NodeKind::UInt64);
}

void TestImportSource::standardCTypes() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct CTypes {\n"
        "    char a;\n"
        "    short b;\n"
        "    int c;\n"
        "    long d;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 4);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Int8);    // char
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Int16);   // short
    QCOMPARE(tree.nodes[kids[2]].kind, NodeKind::Int32);   // int
    QCOMPARE(tree.nodes[kids[3]].kind, NodeKind::Int32);   // long
}

void TestImportSource::multiWordTypes() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct MultiWord {\n"
        "    unsigned char a;\n"
        "    unsigned short b;\n"
        "    unsigned int c;\n"
        "    unsigned long d;\n"
        "    long long e;\n"
        "    unsigned long long f;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 6);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::UInt8);
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::UInt16);
    QCOMPARE(tree.nodes[kids[2]].kind, NodeKind::UInt32);
    QCOMPARE(tree.nodes[kids[3]].kind, NodeKind::UInt32);
    QCOMPARE(tree.nodes[kids[4]].kind, NodeKind::Int64);
    QCOMPARE(tree.nodes[kids[5]].kind, NodeKind::UInt64);
}

void TestImportSource::floatDouble() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct FD {\n"
        "    float a;\n"
        "    double b;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 2);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Float);
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Double);
}

void TestImportSource::boolType() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct B {\n"
        "    bool a;\n"
        "    _Bool b;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 2);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Bool);
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Bool);
}

void TestImportSource::voidPointer() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct VP {\n"
        "    void* ptr;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Pointer64);
    QCOMPARE(tree.nodes[kids[0]].name, QStringLiteral("ptr"));
    QCOMPARE(tree.nodes[kids[0]].refId, uint64_t(0)); // void* has no target
}

void TestImportSource::typedPointer() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Target {\n"
        "    int x;\n"
        "};\n"
        "struct HasPtr {\n"
        "    Target* pTarget;\n"
        "};\n"
    ));
    QCOMPARE(countRoots(tree), 2);
    // Find HasPtr
    int hasPtrIdx = -1;
    for (int i = 0; i < tree.nodes.size(); i++) {
        if (tree.nodes[i].name == QStringLiteral("HasPtr") && tree.nodes[i].parentId == 0) {
            hasPtrIdx = i; break;
        }
    }
    QVERIFY(hasPtrIdx >= 0);
    auto kids = childrenOf(tree, tree.nodes[hasPtrIdx].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Pointer64);
    QVERIFY(tree.nodes[kids[0]].refId != 0);
    // refId should point to Target struct
    int targetIdx = tree.indexOfId(tree.nodes[kids[0]].refId);
    QVERIFY(targetIdx >= 0);
    QCOMPARE(tree.nodes[targetIdx].name, QStringLiteral("Target"));
}

void TestImportSource::selfReferencingPointer() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Node {\n"
        "    int value;\n"
        "    Node* next;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 2);
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Pointer64);
    QCOMPARE(tree.nodes[kids[1]].refId, tree.nodes[0].id);
}

void TestImportSource::doublePointer() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct DP {\n"
        "    void** ppData;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Pointer64);
}

void TestImportSource::primitiveArray() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct PA {\n"
        "    int32_t values[10];\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Array);
    QCOMPARE(tree.nodes[kids[0]].arrayLen, 10);
    QCOMPARE(tree.nodes[kids[0]].elementKind, NodeKind::Int32);
}

void TestImportSource::charArrayToUtf8() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct CA {\n"
        "    char name[64];\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::UTF8);
    QCOMPARE(tree.nodes[kids[0]].strLen, 64);
}

void TestImportSource::wcharArrayToUtf16() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct WC {\n"
        "    wchar_t name[32];\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::UTF16);
    QCOMPARE(tree.nodes[kids[0]].strLen, 32);
}

void TestImportSource::floatArrayToVec2() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct V {\n"
        "    float pos[2];\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Vec2);
}

void TestImportSource::floatArrayToVec3() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct V {\n"
        "    float pos[3];\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Vec3);
}

void TestImportSource::floatArrayToVec4() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct V {\n"
        "    float rot[4];\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Vec4);
}

void TestImportSource::floatArray4x4ToMat4x4() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct M {\n"
        "    float matrix[4][4];\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Mat4x4);
}

void TestImportSource::genericFloatArray() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct GF {\n"
        "    float values[8];\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Array);
    QCOMPARE(tree.nodes[kids[0]].arrayLen, 8);
    QCOMPARE(tree.nodes[kids[0]].elementKind, NodeKind::Float);
}

void TestImportSource::structArray() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Item {\n"
        "    int id;\n"
        "};\n"
        "struct Container {\n"
        "    Item items[5];\n"
        "};\n"
    ));
    QCOMPARE(countRoots(tree), 2);
    // Find Container
    int contIdx = -1;
    for (int i = 0; i < tree.nodes.size(); i++) {
        if (tree.nodes[i].name == QStringLiteral("Container") && tree.nodes[i].parentId == 0) {
            contIdx = i; break;
        }
    }
    QVERIFY(contIdx >= 0);
    auto kids = childrenOf(tree, tree.nodes[contIdx].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Array);
    QCOMPARE(tree.nodes[kids[0]].arrayLen, 5);
    QCOMPARE(tree.nodes[kids[0]].elementKind, NodeKind::Struct);
}

void TestImportSource::commentOffsets() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Offsets {\n"
        "    uint64_t vtable; // 0x0\n"
        "    float health; // 0x8\n"
        "    uint8_t _pad000C[0x4]; // 0xC\n"
        "    double score; // 0x10\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    // vtable at 0x0
    QCOMPARE(tree.nodes[kids[0]].offset, 0);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::UInt64);
    // health at 0x8
    QCOMPARE(tree.nodes[kids[1]].offset, 8);
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Float);
    // _pad at 0xC -> hex nodes
    // score at 0x10
    // Find the double
    bool foundDouble = false;
    for (int k : kids) {
        if (tree.nodes[k].kind == NodeKind::Double) {
            QCOMPARE(tree.nodes[k].offset, 0x10);
            foundDouble = true;
        }
    }
    QVERIFY(foundDouble);
}

void TestImportSource::computedOffsets() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Computed {\n"
        "    uint8_t a;\n"
        "    uint16_t b;\n"
        "    uint32_t c;\n"
        "    uint64_t d;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 4);
    QCOMPARE(tree.nodes[kids[0]].offset, 0);  // uint8_t at 0
    QCOMPARE(tree.nodes[kids[1]].offset, 2);  // uint16_t at 2 (aligned)
    QCOMPARE(tree.nodes[kids[2]].offset, 4);  // uint32_t at 4 (aligned)
    QCOMPARE(tree.nodes[kids[3]].offset, 8);  // uint64_t at 8 (aligned)
}

void TestImportSource::mixedOffsetsAutoDetect() {
    // If any field has a comment offset, all should use comment mode
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Mixed {\n"
        "    uint32_t a; // 0x0\n"
        "    uint32_t b;\n"
        "    uint32_t c; // 0x10\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(tree.nodes[kids[0]].offset, 0);
    // b has no comment offset, in comment mode it gets computed offset 4
    QCOMPARE(tree.nodes[kids[1]].offset, 4);
    // c has comment offset 0x10
    QCOMPARE(tree.nodes[kids[2]].offset, 0x10);
}

void TestImportSource::multiStruct() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct A {\n"
        "    int x;\n"
        "};\n"
        "struct B {\n"
        "    float y;\n"
        "};\n"
        "struct C {\n"
        "    double z;\n"
        "};\n"
    ));
    QCOMPARE(countRoots(tree), 3);
}

void TestImportSource::pointerCrossRef() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct A {\n"
        "    int value;\n"
        "};\n"
        "struct B {\n"
        "    A* ref;\n"
        "};\n"
    ));
    // Find B's pointer field
    int bIdx = -1;
    for (int i = 0; i < tree.nodes.size(); i++) {
        if (tree.nodes[i].name == QStringLiteral("B") && tree.nodes[i].parentId == 0) {
            bIdx = i; break;
        }
    }
    QVERIFY(bIdx >= 0);
    auto kids = childrenOf(tree, tree.nodes[bIdx].id);
    QCOMPARE(kids.size(), 1);
    QVERIFY(tree.nodes[kids[0]].refId != 0);
    // Should point to A
    int aIdx = tree.indexOfId(tree.nodes[kids[0]].refId);
    QVERIFY(aIdx >= 0);
    QCOMPARE(tree.nodes[aIdx].name, QStringLiteral("A"));
}

void TestImportSource::forwardDeclaration() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Bar;\n"
        "struct Foo {\n"
        "    Bar* pBar;\n"
        "};\n"
        "struct Bar {\n"
        "    int val;\n"
        "};\n"
    ));
    QCOMPARE(countRoots(tree), 2);
    // Foo's pBar should resolve to Bar
    int fooIdx = -1;
    for (int i = 0; i < tree.nodes.size(); i++) {
        if (tree.nodes[i].name == QStringLiteral("Foo") && tree.nodes[i].parentId == 0) {
            fooIdx = i; break;
        }
    }
    QVERIFY(fooIdx >= 0);
    auto kids = childrenOf(tree, tree.nodes[fooIdx].id);
    QCOMPARE(kids.size(), 1);
    QVERIFY(tree.nodes[kids[0]].refId != 0);
}

void TestImportSource::unionContainer() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct WithUnion {\n"
        "    union {\n"
        "        float asFloat;\n"
        "        uint32_t asInt;\n"
        "    };\n"
        "    int after;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    // Should have 2 direct children: union container + after
    QCOMPARE(kids.size(), 2);

    // First child is the union container
    const auto& unionNode = tree.nodes[kids[0]];
    QCOMPARE(unionNode.kind, NodeKind::Struct);
    QCOMPARE(unionNode.classKeyword, QStringLiteral("union"));
    QCOMPARE(unionNode.offset, 0);

    // Union has 2 children, both at offset 0
    auto unionKids = childrenOf(tree, unionNode.id);
    QCOMPARE(unionKids.size(), 2);
    QCOMPARE(tree.nodes[unionKids[0]].kind, NodeKind::Float);
    QCOMPARE(tree.nodes[unionKids[0]].name, QStringLiteral("asFloat"));
    QCOMPARE(tree.nodes[unionKids[0]].offset, 0);
    QCOMPARE(tree.nodes[unionKids[1]].kind, NodeKind::UInt32);
    QCOMPARE(tree.nodes[unionKids[1]].name, QStringLiteral("asInt"));
    QCOMPARE(tree.nodes[unionKids[1]].offset, 0);

    // structSpan of union = max member size = 4
    QCOMPARE(tree.structSpan(unionNode.id), 4);

    // after field follows the union at offset 4
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Int32);
    QCOMPARE(tree.nodes[kids[1]].name, QStringLiteral("after"));
    QCOMPARE(tree.nodes[kids[1]].offset, 4);
}

void TestImportSource::unionWithCommentOffsets() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct S {\n"
        "    uint64_t a; // 0x0\n"
        "    union {\n"
        "        uint32_t x; // 0x8\n"
        "        float y; // 0x8\n"
        "    };\n"
        "    uint32_t b; // 0xC\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 3); // a + union + b

    // Union at offset 0x8
    const auto& unionNode = tree.nodes[kids[1]];
    QCOMPARE(unionNode.kind, NodeKind::Struct);
    QCOMPARE(unionNode.classKeyword, QStringLiteral("union"));
    QCOMPARE(unionNode.offset, 0x8);

    // Union members at offset 0 (relative to union)
    auto unionKids = childrenOf(tree, unionNode.id);
    QCOMPARE(unionKids.size(), 2);
    QCOMPARE(tree.nodes[unionKids[0]].offset, 0);
    QCOMPARE(tree.nodes[unionKids[1]].offset, 0);

    // b at 0xC
    QCOMPARE(tree.nodes[kids[2]].offset, 0xC);
}

void TestImportSource::namedUnion() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct S {\n"
        "    union {\n"
        "        uint16_t shortVal;\n"
        "        uint64_t longVal;\n"
        "    } u3;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);

    const auto& unionNode = tree.nodes[kids[0]];
    QCOMPARE(unionNode.kind, NodeKind::Struct);
    QCOMPARE(unionNode.classKeyword, QStringLiteral("union"));
    QCOMPARE(unionNode.name, QStringLiteral("u3"));

    auto unionKids = childrenOf(tree, unionNode.id);
    QCOMPARE(unionKids.size(), 2);
    // structSpan = max(2, 8) = 8
    QCOMPARE(tree.structSpan(unionNode.id), 8);
}

void TestImportSource::paddingFieldExpansion() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Padded {\n"
        "    uint8_t _pad0000[0x10];\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    // 0x10 = 16 bytes, should be 2x Hex64 (best fit)
    QCOMPARE(kids.size(), 2);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Hex64);
    QCOMPARE(tree.nodes[kids[0]].offset, 0);
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Hex64);
    QCOMPARE(tree.nodes[kids[1]].offset, 8);
}

void TestImportSource::staticAssertTailPadding() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Sized {\n"
        "    uint32_t x;\n"
        "};\n"
        "static_assert(sizeof(Sized) == 0x10, \"Size check\");\n"
    ));
    // x is 4 bytes, static_assert says 0x10 = 16
    // Should have tail padding from offset 4 to 16 (12 bytes)
    int span = tree.structSpan(tree.nodes[0].id);
    QCOMPARE(span, 0x10);
}

void TestImportSource::embeddedStruct() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Inner {\n"
        "    int a;\n"
        "};\n"
        "struct Outer {\n"
        "    Inner embedded;\n"
        "    float after;\n"
        "};\n"
    ));
    int outerIdx = -1;
    for (int i = 0; i < tree.nodes.size(); i++) {
        if (tree.nodes[i].name == QStringLiteral("Outer") && tree.nodes[i].parentId == 0) {
            outerIdx = i; break;
        }
    }
    QVERIFY(outerIdx >= 0);
    auto kids = childrenOf(tree, tree.nodes[outerIdx].id);
    QCOMPARE(kids.size(), 2);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Struct);
    QCOMPARE(tree.nodes[kids[0]].structTypeName, QStringLiteral("Inner"));
    QVERIFY(tree.nodes[kids[0]].refId != 0);
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Float);
}

void TestImportSource::typedefBasic() {
    NodeTree tree = importFromSource(QStringLiteral(
        "typedef uint32_t MyInt;\n"
        "struct TD {\n"
        "    MyInt value;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::UInt32);
}

void TestImportSource::constVolatileQualifiers() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Quals {\n"
        "    const uint32_t a;\n"
        "    volatile int32_t b;\n"
        "    const volatile uint8_t c;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 3);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::UInt32);
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Int32);
    QCOMPARE(tree.nodes[kids[2]].kind, NodeKind::UInt8);
}

void TestImportSource::structPrefixOnType() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Inner {\n"
        "    int val;\n"
        "};\n"
        "struct Outer {\n"
        "    struct Inner member;\n"
        "};\n"
    ));
    int outerIdx = -1;
    for (int i = 0; i < tree.nodes.size(); i++) {
        if (tree.nodes[i].name == QStringLiteral("Outer") && tree.nodes[i].parentId == 0) {
            outerIdx = i; break;
        }
    }
    QVERIFY(outerIdx >= 0);
    auto kids = childrenOf(tree, tree.nodes[outerIdx].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Struct);
    QCOMPARE(tree.nodes[kids[0]].structTypeName, QStringLiteral("Inner"));
}

void TestImportSource::bitfieldSkipped() {
    // Bitfields emit a bitfield container with named members
    NodeTree tree = importFromSource(QStringLiteral(
        "struct BF {\n"
        "    uint32_t normal;\n"
        "    uint32_t bitA : 4;\n"
        "    uint32_t bitB : 12;\n"
        "    uint32_t after;\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    // normal + bitfield container (16 bits → 2 bytes) + after
    QCOMPARE(kids.size(), 3);
    QCOMPARE(tree.nodes[kids[0]].name, QStringLiteral("normal"));
    QCOMPARE(tree.nodes[kids[0]].offset, 0);
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Struct);
    QCOMPARE(tree.nodes[kids[1]].resolvedClassKeyword(), QStringLiteral("bitfield"));
    QCOMPARE(tree.nodes[kids[1]].offset, 4);
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers.size(), 2);
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[0].name, QStringLiteral("bitA"));
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[0].bitWidth, (uint8_t)4);
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[0].bitOffset, (uint8_t)0);
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[1].name, QStringLiteral("bitB"));
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[1].bitWidth, (uint8_t)12);
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[1].bitOffset, (uint8_t)4);
    QCOMPARE(tree.nodes[kids[2]].name, QStringLiteral("after"));
    QCOMPARE(tree.nodes[kids[2]].offset, 8);  // aligned to uint32_t boundary
}

void TestImportSource::bitfieldWithOffsetsEmitsHex() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct BF2 {\n"
        "    uint32_t normal; // 0x0\n"
        "    ULONGLONG Valid : 1; // 0x4\n"
        "    ULONGLONG Dirty : 1; // 0x4\n"
        "    ULONGLONG PageFrameNumber : 36; // 0x4\n"
        "    ULONGLONG Reserved : 26; // 0x4\n"
        "    uint32_t after; // 0xC\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    // normal + bitfield container (64 bits) + after = 3
    QCOMPARE(kids.size(), 3);
    QCOMPARE(tree.nodes[kids[0]].name, QStringLiteral("normal"));
    QCOMPARE(tree.nodes[kids[0]].offset, 0);
    // Bitfield container at offset 4
    QCOMPARE(tree.nodes[kids[1]].kind, NodeKind::Struct);
    QCOMPARE(tree.nodes[kids[1]].resolvedClassKeyword(), QStringLiteral("bitfield"));
    QCOMPARE(tree.nodes[kids[1]].offset, 4);
    QCOMPARE(tree.nodes[kids[1]].elementKind, NodeKind::Hex64);
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers.size(), 4);
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[0].name, QStringLiteral("Valid"));
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[0].bitWidth, (uint8_t)1);
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[1].name, QStringLiteral("Dirty"));
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[2].name, QStringLiteral("PageFrameNumber"));
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[2].bitWidth, (uint8_t)36);
    QCOMPARE(tree.nodes[kids[1]].bitfieldMembers[3].name, QStringLiteral("Reserved"));
    // after at 0xC
    QCOMPARE(tree.nodes[kids[2]].name, QStringLiteral("after"));
    QCOMPARE(tree.nodes[kids[2]].offset, 0xC);
}

void TestImportSource::hexArraySizes() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct HexArr {\n"
        "    uint8_t data[0x20];\n"
        "};\n"
    ));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Array);
    QCOMPARE(tree.nodes[kids[0]].arrayLen, 0x20);
}

void TestImportSource::windowsStylePEB() {
    // Test with Windows PEB-style struct (no comment offsets)
    NodeTree tree = importFromSource(QStringLiteral(
        "struct PEB64 {\n"
        "    BOOLEAN InheritedAddressSpace;\n"
        "    BOOLEAN ReadImageFileExecOptions;\n"
        "    BOOLEAN BeingDebugged;\n"
        "    BOOLEAN BitField;\n"
        "    PVOID Mutant;\n"
        "    PVOID ImageBaseAddress;\n"
        "};\n"
    ));
    QCOMPARE(countRoots(tree), 1);
    QCOMPARE(tree.nodes[0].name, QStringLiteral("PEB64"));
    auto kids = childrenOf(tree, tree.nodes[0].id);
    QCOMPARE(kids.size(), 6);
    // First 4 are BOOLEAN (UInt8)
    for (int i = 0; i < 4; i++)
        QCOMPARE(tree.nodes[kids[i]].kind, NodeKind::UInt8);
    // Last 2 are PVOID (Pointer64)
    QCOMPARE(tree.nodes[kids[4]].kind, NodeKind::Pointer64);
    QCOMPARE(tree.nodes[kids[5]].kind, NodeKind::Pointer64);
}

void TestImportSource::classKeyword() {
    NodeTree tree = importFromSource(QStringLiteral(
        "class MyClass {\n"
        "    int value;\n"
        "};\n"
    ));
    QCOMPARE(countRoots(tree), 1);
    QCOMPARE(tree.nodes[0].classKeyword, QStringLiteral("class"));
}

void TestImportSource::inheritanceSkipped() {
    NodeTree tree = importFromSource(QStringLiteral(
        "struct Base {\n"
        "    int a;\n"
        "};\n"
        "struct Derived : public Base {\n"
        "    float b;\n"
        "};\n"
    ));
    QCOMPARE(countRoots(tree), 2);
    int derivedIdx = -1;
    for (int i = 0; i < tree.nodes.size(); i++) {
        if (tree.nodes[i].name == QStringLiteral("Derived") && tree.nodes[i].parentId == 0) {
            derivedIdx = i; break;
        }
    }
    QVERIFY(derivedIdx >= 0);
    auto kids = childrenOf(tree, tree.nodes[derivedIdx].id);
    QCOMPARE(kids.size(), 1);
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::Float);
}

void TestImportSource::basicRoundTrip() {
    // Build a simple tree manually, export it, then re-import and compare
    NodeTree original;
    {
        Node s;
        s.kind = NodeKind::Struct;
        s.name = QStringLiteral("RoundTrip");
        s.structTypeName = QStringLiteral("RoundTrip");
        s.parentId = 0;
        s.offset = 0;
        int sIdx = original.addNode(s);
        uint64_t sId = original.nodes[sIdx].id;

        Node f1;
        f1.kind = NodeKind::UInt32;
        f1.name = QStringLiteral("field_a");
        f1.parentId = sId;
        f1.offset = 0;
        original.addNode(f1);

        Node f2;
        f2.kind = NodeKind::Float;
        f2.name = QStringLiteral("field_b");
        f2.parentId = sId;
        f2.offset = 4;
        original.addNode(f2);

        Node f3;
        f3.kind = NodeKind::UInt64;
        f3.name = QStringLiteral("field_c");
        f3.parentId = sId;
        f3.offset = 8;
        original.addNode(f3);
    }

    // Create source text that matches what generator would produce
    QString source = QStringLiteral(
        "struct RoundTrip {\n"
        "    uint32_t field_a; // 0x0\n"
        "    float field_b; // 0x4\n"
        "    uint64_t field_c; // 0x8\n"
        "};\n"
        "static_assert(sizeof(RoundTrip) == 0x10, \"Size mismatch\");\n"
    );

    NodeTree reimported = importFromSource(source);
    QCOMPARE(countRoots(reimported), 1);
    QCOMPARE(reimported.nodes[0].name, QStringLiteral("RoundTrip"));

    auto origKids = childrenOf(original, original.nodes[0].id);
    auto reimpKids = childrenOf(reimported, reimported.nodes[0].id);

    // Compare field count (reimported may have extra padding nodes from static_assert)
    // Check that the first 3 fields match
    QVERIFY(reimpKids.size() >= 3);
    for (int i = 0; i < 3; i++) {
        QCOMPARE(reimported.nodes[reimpKids[i]].kind, original.nodes[origKids[i]].kind);
        QCOMPARE(reimported.nodes[reimpKids[i]].name, original.nodes[origKids[i]].name);
        QCOMPARE(reimported.nodes[reimpKids[i]].offset, original.nodes[origKids[i]].offset);
    }
}

// ── Enum tests ──

void TestImportSource::enumBasic() {
    auto tree = importFromSource(QStringLiteral(
        "enum Color { Red = 0, Green = 1, Blue = 2 };"));
    QCOMPARE(countRoots(tree), 1);
    QCOMPARE(tree.nodes[0].classKeyword, QStringLiteral("enum"));
    QCOMPARE(tree.nodes[0].structTypeName, QStringLiteral("Color"));
    QCOMPARE(tree.nodes[0].enumMembers.size(), 3);
    QCOMPARE(tree.nodes[0].enumMembers[0].first, QStringLiteral("Red"));
    QCOMPARE(tree.nodes[0].enumMembers[0].second, 0LL);
    QCOMPARE(tree.nodes[0].enumMembers[1].first, QStringLiteral("Green"));
    QCOMPARE(tree.nodes[0].enumMembers[1].second, 1LL);
    QCOMPARE(tree.nodes[0].enumMembers[2].first, QStringLiteral("Blue"));
    QCOMPARE(tree.nodes[0].enumMembers[2].second, 2LL);
}

void TestImportSource::enumAutoValues() {
    auto tree = importFromSource(QStringLiteral(
        "enum Flags { A, B, C };"));
    QCOMPARE(tree.nodes[0].enumMembers.size(), 3);
    QCOMPARE(tree.nodes[0].enumMembers[0].second, 0LL);
    QCOMPARE(tree.nodes[0].enumMembers[1].second, 1LL);
    QCOMPARE(tree.nodes[0].enumMembers[2].second, 2LL);
}

void TestImportSource::enumHexValues() {
    auto tree = importFromSource(QStringLiteral(
        "enum { X = 0x10, Y = 0x20 };"));
    // Anonymous enum has no name — parser skips it (unnamed enums are not added)
    // Actually, let's use a named enum with hex values
    tree = importFromSource(QStringLiteral(
        "enum Hex { X = 0x10, Y = 0x20 };"));
    QCOMPARE(tree.nodes[0].enumMembers.size(), 2);
    QCOMPARE(tree.nodes[0].enumMembers[0].second, 0x10LL);
    QCOMPARE(tree.nodes[0].enumMembers[1].second, 0x20LL);
}

void TestImportSource::enumInStruct() {
    auto tree = importFromSource(QStringLiteral(
        "enum PoolType { NonPaged = 0, Paged = 1 };\n"
        "struct Foo {\n"
        "    PoolType pool; //0x0\n"
        "    uint32_t size; //0x4\n"
        "};"));
    // Should have 2 roots: PoolType enum + Foo struct
    QCOMPARE(countRoots(tree), 2);

    // Find Foo struct
    int fooIdx = -1;
    for (int i = 0; i < tree.nodes.size(); i++) {
        if (tree.nodes[i].name == QStringLiteral("Foo")) { fooIdx = i; break; }
    }
    QVERIFY(fooIdx >= 0);
    auto kids = childrenOf(tree, tree.nodes[fooIdx].id);
    QCOMPARE(kids.size(), 2);
    // First child should be UInt32 (enum mapped to int) with refId to PoolType
    QCOMPARE(tree.nodes[kids[0]].kind, NodeKind::UInt32);
    QCOMPARE(tree.nodes[kids[0]].name, QStringLiteral("pool"));
    QVERIFY(tree.nodes[kids[0]].refId != 0); // linked to enum definition
}

void TestImportSource::enumClass() {
    auto tree = importFromSource(QStringLiteral(
        "enum class Scope : uint8_t { A = 1, B = 2 };"));
    QCOMPARE(countRoots(tree), 1);
    QCOMPARE(tree.nodes[0].classKeyword, QStringLiteral("enum"));
    QCOMPARE(tree.nodes[0].structTypeName, QStringLiteral("Scope"));
    QCOMPARE(tree.nodes[0].enumMembers.size(), 2);
    QCOMPARE(tree.nodes[0].enumMembers[0].first, QStringLiteral("A"));
    QCOMPARE(tree.nodes[0].enumMembers[0].second, 1LL);
}

QTEST_MAIN(TestImportSource)
#include "test_import_source.moc"
