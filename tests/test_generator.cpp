#include <QtTest/QTest>
#include <QFile>
#include <QTemporaryFile>
#include "core.h"
#include "generator.h"

class TestGenerator : public QObject {
    Q_OBJECT

private:
    // Helper: build a simple struct with a few fields
    rcx::NodeTree makeSimpleStruct() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Player";
        root.structTypeName = "Player";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node f1;
        f1.kind = rcx::NodeKind::Int32;
        f1.name = "health";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        rcx::Node f2;
        f2.kind = rcx::NodeKind::Float;
        f2.name = "speed";
        f2.parentId = rootId;
        f2.offset = 4;
        tree.addNode(f2);

        rcx::Node f3;
        f3.kind = rcx::NodeKind::UInt64;
        f3.name = "id";
        f3.parentId = rootId;
        f3.offset = 8;
        tree.addNode(f3);

        return tree;
    }

private slots:

    // ── Basic struct generation ──

    void testSimpleStruct() {
        auto tree = makeSimpleStruct();
        uint64_t rootId = tree.nodes[0].id;
        QString result = rcx::renderCpp(tree, rootId);

        // Header
        QVERIFY(result.contains("#pragma once"));
        QVERIFY(!result.contains("#include <cstdint>"));
        QVERIFY(!result.contains("#pragma pack"));

        // Struct definition
        QVERIFY(result.contains("struct Player {"));
        QVERIFY(result.contains("int32_t health;"));
        QVERIFY(result.contains("float speed;"));
        QVERIFY(result.contains("uint64_t id;"));
        QVERIFY(result.contains("};"));

        // static_assert - struct is 16 bytes (0+4 + 4+4 + 8+8 = 16)
        QVERIFY(result.contains("static_assert(sizeof(Player) == 0x10"));
    }

    // ── Padding gap detection ──

    void testPaddingGaps() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "GappyStruct";
        root.structTypeName = "GappyStruct";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Field at offset 0, size 4
        rcx::Node f1;
        f1.kind = rcx::NodeKind::UInt32;
        f1.name = "a";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        // Field at offset 8, size 4 (gap of 4 bytes at offset 4)
        rcx::Node f2;
        f2.kind = rcx::NodeKind::UInt32;
        f2.name = "b";
        f2.parentId = rootId;
        f2.offset = 8;
        tree.addNode(f2);

        QString result = rcx::renderCpp(tree, rootId);

        // Should contain a padding field between a and b
        QVERIFY(result.contains("uint8_t _pad"));
        QVERIFY(result.contains("[0x4]"));
        QVERIFY(result.contains("uint32_t a;"));
        QVERIFY(result.contains("uint32_t b;"));
    }

    // ── Tail padding ──

    void testTailPadding() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "TailPad";
        root.structTypeName = "TailPad";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Only field at offset 0, size 1
        rcx::Node f1;
        f1.kind = rcx::NodeKind::UInt8;
        f1.name = "flag";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        // Add another field at offset 16 to make struct bigger
        rcx::Node f2;
        f2.kind = rcx::NodeKind::UInt8;
        f2.name = "end";
        f2.parentId = rootId;
        f2.offset = 16;
        tree.addNode(f2);

        QString result = rcx::renderCpp(tree, rootId);

        // Gap between offset 1 and 16 = 15 bytes padding
        QVERIFY(result.contains("[0xF]"));
        // Total size = 17
        QVERIFY(result.contains("static_assert(sizeof(TailPad) == 0x11"));
    }

    // ── Overlap warning ──

    void testOverlapWarning() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "OverlapStruct";
        root.structTypeName = "OverlapStruct";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Two fields that overlap: both at offset 0, size 8 and size 4
        rcx::Node f1;
        f1.kind = rcx::NodeKind::UInt64;
        f1.name = "wide";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        rcx::Node f2;
        f2.kind = rcx::NodeKind::UInt32;
        f2.name = "narrow";
        f2.parentId = rootId;
        f2.offset = 4; // starts at 4, but wide ends at 8 => overlap
        tree.addNode(f2);

        QString result = rcx::renderCpp(tree, rootId);

        // Should contain overlap warning
        QVERIFY(result.contains("WARNING: overlap"));
    }

    // ── Nested struct ──

    void testNestedStruct() {
        rcx::NodeTree tree;

        // Outer struct
        rcx::Node outer;
        outer.kind = rcx::NodeKind::Struct;
        outer.name = "Outer";
        outer.structTypeName = "Outer";
        outer.parentId = 0;
        int oi = tree.addNode(outer);
        uint64_t outerId = tree.nodes[oi].id;

        // Inner struct as child
        rcx::Node inner;
        inner.kind = rcx::NodeKind::Struct;
        inner.name = "pos";
        inner.structTypeName = "Vec2f";
        inner.parentId = outerId;
        inner.offset = 0;
        int ii = tree.addNode(inner);
        uint64_t innerId = tree.nodes[ii].id;

        // Inner fields
        rcx::Node ix;
        ix.kind = rcx::NodeKind::Float;
        ix.name = "x";
        ix.parentId = innerId;
        ix.offset = 0;
        tree.addNode(ix);

        rcx::Node iy;
        iy.kind = rcx::NodeKind::Float;
        iy.name = "y";
        iy.parentId = innerId;
        iy.offset = 4;
        tree.addNode(iy);

        // Another field in outer after inner
        rcx::Node f2;
        f2.kind = rcx::NodeKind::Int32;
        f2.name = "score";
        f2.parentId = outerId;
        f2.offset = 8;
        tree.addNode(f2);

        QString result = rcx::renderCpp(tree, outerId);

        // Inner struct should be defined before outer
        int innerPos = result.indexOf("struct Vec2f {");
        int outerPos = result.indexOf("struct Outer {");
        QVERIFY(innerPos >= 0);
        QVERIFY(outerPos >= 0);
        QVERIFY(innerPos < outerPos);

        // Inner struct fields
        QVERIFY(result.contains("float x;"));
        QVERIFY(result.contains("float y;"));
        QVERIFY(result.contains("static_assert(sizeof(Vec2f) == 0x8"));

        // Outer struct uses inner type
        QVERIFY(result.contains("Vec2f pos;"));
        QVERIFY(result.contains("int32_t score;"));
    }

    // ── Primitive array ──

    void testPrimitiveArray() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "WithArray";
        root.structTypeName = "WithArray";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node arr;
        arr.kind = rcx::NodeKind::Array;
        arr.name = "data";
        arr.parentId = rootId;
        arr.offset = 0;
        arr.arrayLen = 16;
        arr.elementKind = rcx::NodeKind::UInt32;
        tree.addNode(arr);

        QString result = rcx::renderCpp(tree, rootId);
        QVERIFY(result.contains("uint32_t data[16];"));
    }

    // ── Pointer fields ──

    void testPointerFields() {
        rcx::NodeTree tree;

        // Target struct (separate root)
        rcx::Node target;
        target.kind = rcx::NodeKind::Struct;
        target.name = "Target";
        target.structTypeName = "TargetData";
        target.parentId = 0;
        target.offset = 0x100;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        rcx::Node tf;
        tf.kind = rcx::NodeKind::UInt32;
        tf.name = "value";
        tf.parentId = targetId;
        tf.offset = 0;
        tree.addNode(tf);

        // Main struct with pointers
        rcx::Node main;
        main.kind = rcx::NodeKind::Struct;
        main.name = "Main";
        main.structTypeName = "MainStruct";
        main.parentId = 0;
        int mi = tree.addNode(main);
        uint64_t mainId = tree.nodes[mi].id;

        // ptr64 with reference
        rcx::Node p64;
        p64.kind = rcx::NodeKind::Pointer64;
        p64.name = "pTarget";
        p64.parentId = mainId;
        p64.offset = 0;
        p64.refId = targetId;
        tree.addNode(p64);

        // ptr64 without reference
        rcx::Node p64n;
        p64n.kind = rcx::NodeKind::Pointer64;
        p64n.name = "pVoid";
        p64n.parentId = mainId;
        p64n.offset = 8;
        tree.addNode(p64n);

        // ptr32 with reference
        rcx::Node p32;
        p32.kind = rcx::NodeKind::Pointer32;
        p32.name = "pTarget32";
        p32.parentId = mainId;
        p32.offset = 16;
        p32.refId = targetId;
        tree.addNode(p32);

        QString result = rcx::renderCpp(tree, mainId);

        // ptr64 with target → real C++ pointer
        QVERIFY(result.contains("TargetData* pTarget;"));
        // ptr64 without target → void*
        QVERIFY(result.contains("void* pVoid;"));
        // ptr32 with target → uint32_t with comment
        QVERIFY(result.contains("uint32_t pTarget32;"));
        QVERIFY(result.contains("-> TargetData*"));
        // Forward declaration for TargetData
        QVERIFY(result.contains("struct TargetData;"));
    }

    // ── Vector and matrix types ──

    void testVectorTypes() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Vectors";
        root.structTypeName = "Vectors";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node v2;
        v2.kind = rcx::NodeKind::Vec2;
        v2.name = "pos2d";
        v2.parentId = rootId;
        v2.offset = 0;
        tree.addNode(v2);

        rcx::Node v3;
        v3.kind = rcx::NodeKind::Vec3;
        v3.name = "pos3d";
        v3.parentId = rootId;
        v3.offset = 8;
        tree.addNode(v3);

        rcx::Node v4;
        v4.kind = rcx::NodeKind::Vec4;
        v4.name = "color";
        v4.parentId = rootId;
        v4.offset = 20;
        tree.addNode(v4);

        rcx::Node mat;
        mat.kind = rcx::NodeKind::Mat4x4;
        mat.name = "transform";
        mat.parentId = rootId;
        mat.offset = 36;
        tree.addNode(mat);

        QString result = rcx::renderCpp(tree, rootId);

        QVERIFY(result.contains("float pos2d[2];"));
        QVERIFY(result.contains("float pos3d[3];"));
        QVERIFY(result.contains("float color[4];"));
        QVERIFY(result.contains("float transform[4][4];"));
    }

    // ── String types ──

    void testStringTypes() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Strings";
        root.structTypeName = "Strings";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node utf8;
        utf8.kind = rcx::NodeKind::UTF8;
        utf8.name = "name";
        utf8.parentId = rootId;
        utf8.offset = 0;
        utf8.strLen = 64;
        tree.addNode(utf8);

        rcx::Node utf16;
        utf16.kind = rcx::NodeKind::UTF16;
        utf16.name = "wname";
        utf16.parentId = rootId;
        utf16.offset = 64;
        utf16.strLen = 32;
        tree.addNode(utf16);

        QString result = rcx::renderCpp(tree, rootId);

        QVERIFY(result.contains("char name[64];"));
        QVERIFY(result.contains("wchar_t wname[32];"));
    }

    // ── Full SDK export (multiple root structs) ──

    void testFullSdkExport() {
        rcx::NodeTree tree;

        // Struct A at offset 0
        rcx::Node a;
        a.kind = rcx::NodeKind::Struct;
        a.name = "StructA";
        a.structTypeName = "StructA";
        a.parentId = 0;
        a.offset = 0;
        int ai = tree.addNode(a);
        uint64_t aId = tree.nodes[ai].id;

        rcx::Node af;
        af.kind = rcx::NodeKind::UInt32;
        af.name = "valueA";
        af.parentId = aId;
        af.offset = 0;
        tree.addNode(af);

        // Struct B at offset 0x100
        rcx::Node b;
        b.kind = rcx::NodeKind::Struct;
        b.name = "StructB";
        b.structTypeName = "StructB";
        b.parentId = 0;
        b.offset = 0x100;
        int bi = tree.addNode(b);
        uint64_t bId = tree.nodes[bi].id;

        rcx::Node bf;
        bf.kind = rcx::NodeKind::UInt64;
        bf.name = "valueB";
        bf.parentId = bId;
        bf.offset = 0;
        tree.addNode(bf);

        QString result = rcx::renderCppAll(tree);

        QVERIFY(result.contains("struct StructA {"));
        QVERIFY(result.contains("struct StructB {"));
        QVERIFY(result.contains("uint32_t valueA;"));
        QVERIFY(result.contains("uint64_t valueB;"));
        QVERIFY(result.contains("static_assert(sizeof(StructA) == 0x4"));
        QVERIFY(result.contains("static_assert(sizeof(StructB) == 0x8"));
    }

    // ── Null generator ──

    void testNullGenerator() {
        auto tree = makeSimpleStruct();
        QString result = rcx::renderNull(tree, tree.nodes[0].id);
        QVERIFY(result.isEmpty());
    }

    // ── Invalid root ID ──

    void testInvalidRootId() {
        auto tree = makeSimpleStruct();
        QString result = rcx::renderCpp(tree, 9999);
        QVERIFY(result.isEmpty());
    }

    // ── Non-struct root ──

    void testNonStructRoot() {
        rcx::NodeTree tree;
        rcx::Node n;
        n.kind = rcx::NodeKind::UInt32;
        n.name = "scalar";
        n.parentId = 0;
        tree.addNode(n);

        QString result = rcx::renderCpp(tree, tree.nodes[0].id);
        QVERIFY(result.isEmpty());
    }

    // ── Empty struct ──

    void testEmptyStruct() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Empty";
        root.structTypeName = "Empty";
        root.parentId = 0;
        tree.addNode(root);

        QString result = rcx::renderCpp(tree, tree.nodes[0].id);

        QVERIFY(result.contains("struct Empty {"));
        QVERIFY(result.contains("};"));
        QVERIFY(result.contains("static_assert(sizeof(Empty) == 0x0"));
    }

    // ── Name sanitization ──

    void testNameSanitization() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "my struct-name";
        root.structTypeName = "my struct-name";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node f;
        f.kind = rcx::NodeKind::UInt32;
        f.name = "field with spaces";
        f.parentId = rootId;
        f.offset = 0;
        tree.addNode(f);

        QString result = rcx::renderCpp(tree, rootId);

        // Spaces and dashes should be replaced with underscores
        QVERIFY(result.contains("struct my_struct_name {"));
        QVERIFY(result.contains("uint32_t field_with_spaces;"));
    }

    // ── Export produces valid file content ──

    void testExportToFile() {
        auto tree = makeSimpleStruct();
        uint64_t rootId = tree.nodes[0].id;
        QString text = rcx::renderCpp(tree, rootId);

        QTemporaryFile tmpFile;
        tmpFile.setAutoRemove(true);
        QVERIFY(tmpFile.open());
        tmpFile.write(text.toUtf8());
        tmpFile.close();

        // Read back and verify
        QVERIFY(tmpFile.open());
        QByteArray readBack = tmpFile.readAll();
        tmpFile.close();

        QString readStr = QString::fromUtf8(readBack);
        QVERIFY(readStr.contains("#pragma once"));
        QVERIFY(readStr.contains("struct Player {"));
        QVERIFY(readStr.contains("static_assert"));
    }

    // ── Full SDK with no structs (only primitives) ──

    void testFullSdkNoStructs() {
        rcx::NodeTree tree;
        rcx::Node n;
        n.kind = rcx::NodeKind::UInt32;
        n.name = "scalar";
        n.parentId = 0;
        tree.addNode(n);

        QString result = rcx::renderCppAll(tree);

        // Header present but no struct definitions
        QVERIFY(result.contains("#pragma once"));
        QVERIFY(!result.contains("struct "));
    }

    // ── Deeply nested structs ──

    void testDeeplyNested() {
        rcx::NodeTree tree;

        // A > B > C, each containing one field
        rcx::Node a;
        a.kind = rcx::NodeKind::Struct;
        a.name = "A";
        a.structTypeName = "TypeA";
        a.parentId = 0;
        int ai = tree.addNode(a);
        uint64_t aId = tree.nodes[ai].id;

        rcx::Node b;
        b.kind = rcx::NodeKind::Struct;
        b.name = "b";
        b.structTypeName = "TypeB";
        b.parentId = aId;
        b.offset = 0;
        int bi = tree.addNode(b);
        uint64_t bId = tree.nodes[bi].id;

        rcx::Node c;
        c.kind = rcx::NodeKind::Struct;
        c.name = "c";
        c.structTypeName = "TypeC";
        c.parentId = bId;
        c.offset = 0;
        int ci = tree.addNode(c);
        uint64_t cId = tree.nodes[ci].id;

        rcx::Node leaf;
        leaf.kind = rcx::NodeKind::UInt8;
        leaf.name = "val";
        leaf.parentId = cId;
        leaf.offset = 0;
        tree.addNode(leaf);

        QString result = rcx::renderCpp(tree, aId);

        // TypeC defined first, then TypeB, then TypeA
        int cPos = result.indexOf("struct TypeC {");
        int bPos = result.indexOf("struct TypeB {");
        int aPos = result.indexOf("struct TypeA {");
        QVERIFY(cPos >= 0);
        QVERIFY(bPos >= 0);
        QVERIFY(aPos >= 0);
        QVERIFY(cPos < bPos);
        QVERIFY(bPos < aPos);

        // TypeA contains TypeB, TypeB contains TypeC
        QVERIFY(result.contains("TypeB b;"));
        QVERIFY(result.contains("TypeC c;"));
        QVERIFY(result.contains("uint8_t val;"));
    }
};

QTEST_MAIN(TestGenerator)
#include "test_generator.moc"
