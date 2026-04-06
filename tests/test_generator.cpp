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

    // ── Basic struct generation (Vergilius-style) ──

    void testSimpleStruct() {
        auto tree = makeSimpleStruct();
        uint64_t rootId = tree.nodes[0].id;
        QString result = rcx::renderCpp(tree, rootId, nullptr, true);

        // Header
        QVERIFY(result.contains("#pragma once"));

        // Size comment on closing brace
        QVERIFY(result.contains("// sizeof 0x10"));

        // Struct definition (brace on new line)
        QVERIFY(result.contains("struct Player\n{"));
        QVERIFY(result.contains("int32_t health;"));
        QVERIFY(result.contains("float speed;"));
        QVERIFY(result.contains("uint64_t id;"));
        QVERIFY(result.contains("};"));

        // Offset comments
        QVERIFY(result.contains("// 0x0"));
        QVERIFY(result.contains("// 0x4"));
        QVERIFY(result.contains("// 0x8"));

        // static_assert
        QVERIFY(result.contains("static_assert(sizeof(Player) == 0x10"));

        // Without emitAsserts, static_assert should not appear
        QString noAsserts = rcx::renderCpp(tree, rootId);
        QVERIFY(!noAsserts.contains("static_assert"));
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

        QString result = rcx::renderCpp(tree, rootId, nullptr, true);

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

    // ── Union members should NOT produce overlap warnings ──

    void testUnionNoOverlapWarning() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "TestUnion";
        root.structTypeName = "TestUnion";
        root.classKeyword = "union";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Two union members at offset 0
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
        f2.offset = 0;
        tree.addNode(f2);

        QString result = rcx::renderCpp(tree, rootId);

        // Vergilius-style: union keyword, brace on new line
        QVERIFY(result.contains("union TestUnion\n{"));
        QVERIFY(result.contains("uint64_t wide;"));
        QVERIFY(result.contains("uint32_t narrow;"));
        // Union members overlap by design — no warning
        QVERIFY(!result.contains("WARNING"));
        // No padding in unions
        QVERIFY(!result.contains("_pad"));
    }

    // ── Nested struct: named sub-type referenced by name ──

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

        QString result = rcx::renderCpp(tree, outerId, nullptr, true);

        // Vergilius-style: named sub-types referenced by name with struct prefix
        // No separate top-level definition for Vec2f in renderCpp
        QVERIFY(result.contains("struct Outer\n{"));
        QVERIFY(result.contains("struct Vec2f pos;"));
        QVERIFY(result.contains("int32_t score;"));
        QVERIFY(result.contains("static_assert(sizeof(Outer) == 0xC"));
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

        // Vergilius-style: struct prefix on pointer targets
        QVERIFY(result.contains("struct TargetData* pTarget;"));
        // ptr64 without target → void*
        QVERIFY(result.contains("void* pVoid;"));
        // ptr32 with target → struct X* (Vergilius-style, no forward decl needed)
        QVERIFY(result.contains("struct TargetData* pTarget32;"));
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

        QString result = rcx::renderCppAll(tree, nullptr, true);

        // Vergilius-style: brace on new line
        QVERIFY(result.contains("struct StructA\n{"));
        QVERIFY(result.contains("struct StructB\n{"));
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

        QString result = rcx::renderCpp(tree, tree.nodes[0].id, nullptr, true);

        QVERIFY(result.contains("struct Empty\n{"));
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
        QVERIFY(result.contains("struct my_struct_name\n{"));
        QVERIFY(result.contains("uint32_t field_with_spaces;"));
    }

    // ── Export produces valid file content ──

    void testExportToFile() {
        auto tree = makeSimpleStruct();
        uint64_t rootId = tree.nodes[0].id;
        QString text = rcx::renderCpp(tree, rootId, nullptr, true);

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
        QVERIFY(readStr.contains("struct Player\n{"));
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

    // ── Deeply nested structs: referenced by name ──

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

        // Vergilius-style: named sub-types referenced by name with struct prefix
        // Only the root type gets a top-level definition
        QVERIFY(result.contains("struct TypeA\n{"));
        QVERIFY(result.contains("struct TypeB b;"));
    }

    // ── Inline anonymous struct/union ──

    void testInlineAnonymousStruct() {
        rcx::NodeTree tree;

        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "_MMPFN";
        root.structTypeName = "_MMPFN";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Anonymous union at offset 0 (no structTypeName)
        rcx::Node anonUnion;
        anonUnion.kind = rcx::NodeKind::Struct;
        anonUnion.name = "";
        anonUnion.structTypeName = "";
        anonUnion.classKeyword = "union";
        anonUnion.parentId = rootId;
        anonUnion.offset = 0;
        int ui = tree.addNode(anonUnion);
        uint64_t unionId = tree.nodes[ui].id;

        // Union member 1: named struct reference
        rcx::Node listEntry;
        listEntry.kind = rcx::NodeKind::Struct;
        listEntry.name = "ListEntry";
        listEntry.structTypeName = "_LIST_ENTRY";
        listEntry.parentId = unionId;
        listEntry.offset = 0;
        tree.addNode(listEntry);

        // Union member 2: a simple field
        rcx::Node flags;
        flags.kind = rcx::NodeKind::UInt64;
        flags.name = "Flags";
        flags.parentId = unionId;
        flags.offset = 0;
        tree.addNode(flags);

        // Field after the anonymous union
        rcx::Node pfn;
        pfn.kind = rcx::NodeKind::UInt64;
        pfn.name = "PfnCount";
        pfn.parentId = rootId;
        pfn.offset = 0x10;
        tree.addNode(pfn);

        QString result = rcx::renderCpp(tree, rootId);

        // Anonymous union should be inlined, not a top-level anon_XXXX
        QVERIFY(!result.contains("anon_"));
        QVERIFY(result.contains("union\n    {"));
        QVERIFY(result.contains("struct _LIST_ENTRY ListEntry;"));
        QVERIFY(result.contains("uint64_t Flags;"));
        QVERIFY(result.contains("};"));
        QVERIFY(result.contains("uint64_t PfnCount;"));
    }

    // ── Opaque types: no stub definition ──

    void testOpaqueTypeNoStub() {
        rcx::NodeTree tree;

        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Container";
        root.structTypeName = "Container";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Named struct child with no children of its own (opaque reference)
        rcx::Node opaque;
        opaque.kind = rcx::NodeKind::Struct;
        opaque.name = "entry";
        opaque.structTypeName = "_LIST_ENTRY";
        opaque.parentId = rootId;
        opaque.offset = 0;
        tree.addNode(opaque);

        QString result = rcx::renderCpp(tree, rootId);

        // Should reference by name with struct prefix, no stub body
        QVERIFY(result.contains("struct _LIST_ENTRY entry;"));
        // Should NOT have a separate _LIST_ENTRY definition with padding
        QVERIFY(!result.contains("struct _LIST_ENTRY\n{"));
        QVERIFY(!result.contains("uint8_t _pad"));
    }
    // ── Static field node generator tests ──

    void testStaticFieldNotInStructBody() {
        rcx::NodeTree tree;

        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "MyStruct";
        root.structTypeName = "MyStruct";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node f1;
        f1.kind = rcx::NodeKind::UInt32;
        f1.name = "e_lfanew";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        rcx::Node sf;
        sf.kind = rcx::NodeKind::Struct;
        sf.name = "nt_hdr";
        sf.structTypeName = "IMAGE_NT_HEADERS";
        sf.parentId = rootId;
        sf.offset = 0;
        sf.isStatic = true;
        sf.offsetExpr = QStringLiteral("base + e_lfanew");
        tree.addNode(sf);

        QString result = rcx::renderCpp(tree, rootId);

        // Static field should NOT appear as a member in the struct body
        QVERIFY2(!result.contains("IMAGE_NT_HEADERS nt_hdr;"),
                 qPrintable("Static field should not be in struct body:\n" + result));

        // Static field SHOULD appear as a comment
        QVERIFY2(result.contains("// static:"),
                 qPrintable("Static field comment missing:\n" + result));
        QVERIFY2(result.contains("nt_hdr"),
                 qPrintable("Static field name missing from comment:\n" + result));
        QVERIFY2(result.contains("base + e_lfanew"),
                 qPrintable("Static field expression missing from comment:\n" + result));
    }

    void testStaticFieldCommentFormat() {
        rcx::NodeTree tree;

        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Test";
        root.structTypeName = "Test";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node f1;
        f1.kind = rcx::NodeKind::UInt64;
        f1.name = "base_field";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        rcx::Node sf;
        sf.kind = rcx::NodeKind::Hex64;
        sf.name = "ptr";
        sf.parentId = rootId;
        sf.offset = 0;
        sf.isStatic = true;
        sf.offsetExpr = QStringLiteral("base + 0xFF");
        tree.addNode(sf);

        QString result = rcx::renderCpp(tree, rootId);

        // The regular field should be in the struct body
        QVERIFY(result.contains("uint64_t base_field;"));

        // Static field emitted as comment after struct body
        QVERIFY(result.contains("// static:"));
        QVERIFY(result.contains("@ base + 0xFF"));
    }

    void testStructSizeUnchangedByStaticField() {
        rcx::NodeTree tree;

        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Small";
        root.structTypeName = "Small";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node f1;
        f1.kind = rcx::NodeKind::UInt32;
        f1.name = "x";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        rcx::Node sf;
        sf.kind = rcx::NodeKind::Struct;
        sf.name = "big_static";
        sf.parentId = rootId;
        sf.offset = 0;
        sf.isStatic = true;
        sf.offsetExpr = QStringLiteral("base");
        tree.addNode(sf);

        QString result = rcx::renderCpp(tree, rootId, nullptr, true);

        // static_assert should use only the regular field size (4 bytes)
        QVERIFY2(result.contains("sizeof(Small) == 0x4"),
                 qPrintable("Expected sizeof(Small) == 0x4:\n" + result));
    }

    // ═══════════════════════════════════════════════════════════
    // ── Rust backend tests ──
    // ═══════════════════════════════════════════════════════════

    void testRustSimpleStruct() {
        auto tree = makeSimpleStruct();
        uint64_t rootId = tree.nodes[0].id;
        QString result = rcx::renderRust(tree, rootId, nullptr, true);

        QVERIFY(result.contains("// Generated by Reclass 2027"));
        QVERIFY(result.contains("#[repr(C)]"));
        QVERIFY(result.contains("pub struct Player {"));
        QVERIFY(result.contains("pub health: i32,"));
        QVERIFY(result.contains("pub speed: f32,"));
        QVERIFY(result.contains("pub id: u64,"));
        QVERIFY(result.contains("// 0x0"));
        QVERIFY(result.contains("// 0x4"));
        QVERIFY(result.contains("// 0x8"));
        QVERIFY(result.contains("core::mem::size_of::<Player>() == 0x10"));

        // Without asserts
        QString noAsserts = rcx::renderRust(tree, rootId);
        QVERIFY(!noAsserts.contains("size_of"));
    }

    void testRustPadding() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Padded";
        root.structTypeName = "Padded";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node f1;
        f1.kind = rcx::NodeKind::UInt32;
        f1.name = "a";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        rcx::Node f2;
        f2.kind = rcx::NodeKind::UInt32;
        f2.name = "b";
        f2.parentId = rootId;
        f2.offset = 8;
        tree.addNode(f2);

        QString result = rcx::renderRust(tree, rootId);
        QVERIFY(result.contains("pub _pad"));
        QVERIFY(result.contains("[u8; 0x4]"));
    }

    void testRustPointers() {
        rcx::NodeTree tree;

        rcx::Node target;
        target.kind = rcx::NodeKind::Struct;
        target.name = "Target";
        target.structTypeName = "Target";
        target.parentId = 0;
        target.offset = 0x100;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        rcx::Node tf;
        tf.kind = rcx::NodeKind::UInt32;
        tf.name = "val";
        tf.parentId = targetId;
        tf.offset = 0;
        tree.addNode(tf);

        rcx::Node main;
        main.kind = rcx::NodeKind::Struct;
        main.name = "PtrTest";
        main.structTypeName = "PtrTest";
        main.parentId = 0;
        int mi = tree.addNode(main);
        uint64_t mainId = tree.nodes[mi].id;

        rcx::Node p1;
        p1.kind = rcx::NodeKind::Pointer64;
        p1.name = "typed";
        p1.parentId = mainId;
        p1.offset = 0;
        p1.refId = targetId;
        tree.addNode(p1);

        rcx::Node p2;
        p2.kind = rcx::NodeKind::Pointer64;
        p2.name = "untyped";
        p2.parentId = mainId;
        p2.offset = 8;
        tree.addNode(p2);

        QString result = rcx::renderRust(tree, mainId);
        QVERIFY(result.contains("pub typed: *mut Target,"));
        QVERIFY(result.contains("pub untyped: *mut core::ffi::c_void,"));
    }

    void testRustVectors() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Vecs";
        root.structTypeName = "Vecs";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node v2;
        v2.kind = rcx::NodeKind::Vec2;
        v2.name = "pos";
        v2.parentId = rootId;
        v2.offset = 0;
        tree.addNode(v2);

        rcx::Node v4;
        v4.kind = rcx::NodeKind::Vec4;
        v4.name = "color";
        v4.parentId = rootId;
        v4.offset = 8;
        tree.addNode(v4);

        QString result = rcx::renderRust(tree, rootId);
        QVERIFY(result.contains("pub pos: [f32; 2],"));
        QVERIFY(result.contains("pub color: [f32; 4],"));
    }

    void testRustFuncPtr() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "FP";
        root.structTypeName = "FP";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node fp;
        fp.kind = rcx::NodeKind::FuncPtr64;
        fp.name = "callback";
        fp.parentId = rootId;
        fp.offset = 0;
        tree.addNode(fp);

        QString result = rcx::renderRust(tree, rootId);
        QVERIFY(result.contains("pub callback: Option<unsafe extern \"C\" fn()>,"));
    }

    void testRustAll() {
        auto tree = makeSimpleStruct();
        QString result = rcx::renderRustAll(tree, nullptr, true);
        QVERIFY(result.contains("#[repr(C)]"));
        QVERIFY(result.contains("pub struct Player {"));
        QVERIFY(result.contains("core::mem::size_of::<Player>()"));
    }

    // ═══════════════════════════════════════════════════════════
    // ── #define offsets backend tests ──
    // ═══════════════════════════════════════════════════════════

    void testDefineSimpleStruct() {
        auto tree = makeSimpleStruct();
        uint64_t rootId = tree.nodes[0].id;
        QString result = rcx::renderDefines(tree, rootId);

        QVERIFY(result.contains("#pragma once"));
        QVERIFY(result.contains("// Player"));
        QVERIFY(result.contains("#define Player_health 0x0"));
        QVERIFY(result.contains("#define Player_speed 0x4"));
        QVERIFY(result.contains("#define Player_id 0x8"));
    }

    void testDefineSkipsHex() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "HexTest";
        root.structTypeName = "HexTest";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node h;
        h.kind = rcx::NodeKind::Hex32;
        h.name = "padding";
        h.parentId = rootId;
        h.offset = 0;
        tree.addNode(h);

        rcx::Node f;
        f.kind = rcx::NodeKind::UInt32;
        f.name = "real_field";
        f.parentId = rootId;
        f.offset = 4;
        tree.addNode(f);

        QString result = rcx::renderDefines(tree, rootId);
        QVERIFY(!result.contains("padding"));
        QVERIFY(result.contains("#define HexTest_real_field 0x4"));
    }

    void testDefineAll() {
        auto tree = makeSimpleStruct();
        QString result = rcx::renderDefinesAll(tree);
        QVERIFY(result.contains("#pragma once"));
        QVERIFY(result.contains("#define Player_health 0x0"));
    }

    // ═══════════════════════════════════════════════════════════
    // ── Format dispatch tests ──
    // ═══════════════════════════════════════════════════════════

    void testCodeFormatDispatch() {
        auto tree = makeSimpleStruct();
        uint64_t rootId = tree.nodes[0].id;

        QString cpp = rcx::renderCode(rcx::CodeFormat::CppHeader, tree, rootId);
        QVERIFY(cpp.contains("struct Player"));

        QString rust = rcx::renderCode(rcx::CodeFormat::RustStruct, tree, rootId);
        QVERIFY(rust.contains("pub struct Player"));

        QString defs = rcx::renderCode(rcx::CodeFormat::DefineOffsets, tree, rootId);
        QVERIFY(defs.contains("#define Player_health"));
    }

    void testCodeFormatAllDispatch() {
        auto tree = makeSimpleStruct();

        QString cpp = rcx::renderCodeAll(rcx::CodeFormat::CppHeader, tree);
        QVERIFY(cpp.contains("struct Player"));

        QString rust = rcx::renderCodeAll(rcx::CodeFormat::RustStruct, tree);
        QVERIFY(rust.contains("pub struct Player"));

        QString defs = rcx::renderCodeAll(rcx::CodeFormat::DefineOffsets, tree);
        QVERIFY(defs.contains("#define Player_health"));
    }

    // ═══════════════════════════════════════════════════════════
    // ── Scope tests (Current + Deps) ──
    // ═══════════════════════════════════════════════════════════

    void testTreeScopeIncludesReferencedTypes() {
        rcx::NodeTree tree;

        // Target struct (referenced by pointer)
        rcx::Node target;
        target.kind = rcx::NodeKind::Struct;
        target.name = "Target";
        target.structTypeName = "Target";
        target.parentId = 0;
        target.offset = 0x100;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        rcx::Node tf;
        tf.kind = rcx::NodeKind::UInt32;
        tf.name = "val";
        tf.parentId = targetId;
        tf.offset = 0;
        tree.addNode(tf);

        // Main struct with a pointer to Target
        rcx::Node main;
        main.kind = rcx::NodeKind::Struct;
        main.name = "Main";
        main.structTypeName = "Main";
        main.parentId = 0;
        int mi = tree.addNode(main);
        uint64_t mainId = tree.nodes[mi].id;

        rcx::Node ptr;
        ptr.kind = rcx::NodeKind::Pointer64;
        ptr.name = "pTarget";
        ptr.parentId = mainId;
        ptr.offset = 0;
        ptr.refId = targetId;
        tree.addNode(ptr);

        // "Current" scope: only Main, no Target definition
        QString current = rcx::renderCpp(tree, mainId);
        QVERIFY(current.contains("struct Main\n{"));
        QVERIFY(!current.contains("struct Target\n{"));

        // "Current + Deps" scope: Main AND Target definitions
        QString withDeps = rcx::renderCppTree(tree, mainId);
        QVERIFY(withDeps.contains("struct Main\n{"));
        QVERIFY(withDeps.contains("struct Target\n{"));

        // Same for Rust
        QString rustDeps = rcx::renderRustTree(tree, mainId);
        QVERIFY(rustDeps.contains("pub struct Main {"));
        QVERIFY(rustDeps.contains("pub struct Target {"));

        // Same for #define
        QString defDeps = rcx::renderDefinesTree(tree, mainId);
        QVERIFY(defDeps.contains("#define Main_pTarget"));
        QVERIFY(defDeps.contains("#define Target_val"));
    }

    void testTreeScopeDispatch() {
        rcx::NodeTree tree;

        rcx::Node a;
        a.kind = rcx::NodeKind::Struct;
        a.name = "A";
        a.structTypeName = "A";
        a.parentId = 0;
        int ai = tree.addNode(a);
        uint64_t aId = tree.nodes[ai].id;

        rcx::Node af;
        af.kind = rcx::NodeKind::UInt32;
        af.name = "x";
        af.parentId = aId;
        af.offset = 0;
        tree.addNode(af);

        // renderCodeTree should work for all formats
        QString cpp = rcx::renderCodeTree(rcx::CodeFormat::CppHeader, tree, aId);
        QVERIFY(cpp.contains("struct A"));

        QString rust = rcx::renderCodeTree(rcx::CodeFormat::RustStruct, tree, aId);
        QVERIFY(rust.contains("pub struct A"));

        QString defs = rcx::renderCodeTree(rcx::CodeFormat::DefineOffsets, tree, aId);
        QVERIFY(defs.contains("#define A_x"));

        QString cs = rcx::renderCodeTree(rcx::CodeFormat::CSharpStruct, tree, aId);
        QVERIFY(cs.contains("public unsafe struct A"));

        QString py = rcx::renderCodeTree(rcx::CodeFormat::PythonCtypes, tree, aId);
        QVERIFY(py.contains("class A(ctypes.Structure)"));
    }

    // ── C# backend ──

    void testCSharpSimpleStruct() {
        auto tree = makeSimpleStruct();
        uint64_t rootId = tree.nodes[0].id;
        QString result = rcx::renderCSharp(tree, rootId);

        QVERIFY(result.contains("using System.Runtime.InteropServices;"));
        QVERIFY(result.contains("[StructLayout(LayoutKind.Explicit, Size = 0x10)]"));
        QVERIFY(result.contains("public unsafe struct Player"));
        QVERIFY(result.contains("[FieldOffset(0x0)] public int health;"));
        QVERIFY(result.contains("[FieldOffset(0x4)] public float speed;"));
        QVERIFY(result.contains("[FieldOffset(0x8)] public ulong id;"));
    }

    void testCSharpPointers() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Foo";
        root.structTypeName = "Foo";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node p;
        p.kind = rcx::NodeKind::Pointer64;
        p.name = "ptr";
        p.parentId = rootId;
        p.offset = 0;
        tree.addNode(p);

        QString result = rcx::renderCSharp(tree, rootId);
        QVERIFY(result.contains("IntPtr ptr"));
    }

    void testCSharpAll() {
        auto tree = makeSimpleStruct();
        QString result = rcx::renderCSharpAll(tree);
        QVERIFY(result.contains("public unsafe struct Player"));
        QVERIFY(result.contains("[StructLayout("));
    }

    void testCSharpEnum() {
        rcx::NodeTree tree;
        rcx::Node e;
        e.kind = rcx::NodeKind::Struct;
        e.name = "Color";
        e.structTypeName = "Color";
        e.classKeyword = "enum";
        e.parentId = 0;
        e.offset = 0;
        e.enumMembers = {{"Red", 0}, {"Green", 1}, {"Blue", 2}};
        tree.addNode(e);

        QString result = rcx::renderCSharpAll(tree);
        QVERIFY(result.contains("public enum Color : long"));
        QVERIFY(result.contains("Red = 0"));
        QVERIFY(result.contains("Green = 1"));
        QVERIFY(result.contains("Blue = 2"));
    }

    void testCSharpVectors() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Xform";
        root.structTypeName = "Xform";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node v;
        v.kind = rcx::NodeKind::Vec3;
        v.name = "position";
        v.parentId = rootId;
        v.offset = 0;
        tree.addNode(v);

        QString result = rcx::renderCSharp(tree, rootId);
        QVERIFY(result.contains("public fixed float position[3]"));
    }

    // ── Python ctypes backend ──

    void testPythonSimpleStruct() {
        auto tree = makeSimpleStruct();
        uint64_t rootId = tree.nodes[0].id;
        QString result = rcx::renderPython(tree, rootId);

        QVERIFY(result.contains("import ctypes"));
        QVERIFY(result.contains("class Player(ctypes.Structure)"));
        QVERIFY(result.contains("_fields_ = ["));
        QVERIFY(result.contains("(\"health\", ctypes.c_int32)"));
        QVERIFY(result.contains("(\"speed\", ctypes.c_float)"));
        QVERIFY(result.contains("(\"id\", ctypes.c_uint64)"));
    }

    void testPythonPointers() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Bar";
        root.structTypeName = "Bar";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node p;
        p.kind = rcx::NodeKind::Pointer64;
        p.name = "ptr";
        p.parentId = rootId;
        p.offset = 0;
        tree.addNode(p);

        QString result = rcx::renderPython(tree, rootId);
        QVERIFY(result.contains("(\"ptr\", ctypes.c_void_p)"));
    }

    void testPythonTypedPointers() {
        rcx::NodeTree tree;
        rcx::Node target;
        target.kind = rcx::NodeKind::Struct;
        target.name = "Target";
        target.structTypeName = "Target";
        target.parentId = 0;
        target.offset = 0;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Holder";
        root.structTypeName = "Holder";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node p;
        p.kind = rcx::NodeKind::Pointer64;
        p.name = "ref";
        p.parentId = rootId;
        p.offset = 0;
        p.refId = targetId;
        tree.addNode(p);

        QString result = rcx::renderPython(tree, rootId);
        QVERIFY(result.contains("ctypes.POINTER(Target)"));
    }

    void testPythonAll() {
        auto tree = makeSimpleStruct();
        QString result = rcx::renderPythonAll(tree);
        QVERIFY(result.contains("class Player(ctypes.Structure)"));
    }

    void testPythonEnum() {
        rcx::NodeTree tree;
        rcx::Node e;
        e.kind = rcx::NodeKind::Struct;
        e.name = "Status";
        e.structTypeName = "Status";
        e.classKeyword = "enum";
        e.parentId = 0;
        e.offset = 0;
        e.enumMembers = {{"Active", 1}, {"Inactive", 0}};
        tree.addNode(e);

        QString result = rcx::renderPythonAll(tree);
        QVERIFY(result.contains("class Status:"));
        QVERIFY(result.contains("Active = 1"));
        QVERIFY(result.contains("Inactive = 0"));
    }

    void testPythonVectors() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Pos";
        root.structTypeName = "Pos";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node v;
        v.kind = rcx::NodeKind::Vec4;
        v.name = "color";
        v.parentId = rootId;
        v.offset = 0;
        tree.addNode(v);

        QString result = rcx::renderPython(tree, rootId);
        QVERIFY(result.contains("(\"color\", ctypes.c_float * 4)"));
    }

    void testCSharpDispatch() {
        auto tree = makeSimpleStruct();
        uint64_t rootId = tree.nodes[0].id;
        QString result = rcx::renderCode(rcx::CodeFormat::CSharpStruct, tree, rootId);
        QVERIFY(result.contains("[StructLayout("));
    }

    void testPythonDispatch() {
        auto tree = makeSimpleStruct();
        uint64_t rootId = tree.nodes[0].id;
        QString result = rcx::renderCode(rcx::CodeFormat::PythonCtypes, tree, rootId);
        QVERIFY(result.contains("ctypes.Structure"));
    }
    // ── Hex128, enum, union, pointer, bitfield tests ──

    void testHex128CppOutput() {
        rcx::NodeTree tree;
        rcx::Node root; root.kind = rcx::NodeKind::Struct;
        root.structTypeName = "Big"; root.name = "big";
        int ri = tree.addNode(root);
        rcx::Node f; f.kind = rcx::NodeKind::Hex128;
        f.name = "bigfield"; f.parentId = tree.nodes[ri].id; f.offset = 0;
        tree.addNode(f);
        QString cpp = rcx::renderCpp(tree, tree.nodes[ri].id);
        QVERIFY(cpp.contains("cstdint"));
        // Hex128 is emitted as padding (uint8_t[0x10])
        QVERIFY(cpp.contains("uint8_t") || cpp.contains("0x10"));
    }

    void testHex128RustOutput() {
        rcx::NodeTree tree;
        rcx::Node root; root.kind = rcx::NodeKind::Struct;
        root.structTypeName = "Big"; root.name = "big";
        int ri = tree.addNode(root);
        rcx::Node f; f.kind = rcx::NodeKind::Hex128;
        f.name = "bigfield"; f.parentId = tree.nodes[ri].id; f.offset = 0;
        tree.addNode(f);
        QString rs = rcx::renderRust(tree, tree.nodes[ri].id);
        QVERIFY(rs.contains("u8") || rs.contains("0x10"));
    }

    void testEnumCppOutput() {
        rcx::NodeTree tree;
        rcx::Node root; root.kind = rcx::NodeKind::Struct;
        root.structTypeName = "Colors"; root.name = "colors";
        root.classKeyword = QStringLiteral("enum");
        root.enumMembers = {{QStringLiteral("Red"), 0}, {QStringLiteral("Green"), 1}};
        tree.addNode(root);
        QString cpp = rcx::renderCpp(tree, tree.nodes[0].id);
        QVERIFY(cpp.contains("enum Colors"));
        QVERIFY(cpp.contains("Red = 0"));
        QVERIFY(cpp.contains("Green = 1"));
    }

    void testUnionCppOutput() {
        rcx::NodeTree tree;
        rcx::Node root; root.kind = rcx::NodeKind::Struct;
        root.structTypeName = "MyUnion"; root.name = "u";
        root.classKeyword = QStringLiteral("union");
        int ri = tree.addNode(root);
        rcx::Node f1; f1.kind = rcx::NodeKind::Int32; f1.name = "i";
        f1.parentId = tree.nodes[ri].id; f1.offset = 0; tree.addNode(f1);
        rcx::Node f2; f2.kind = rcx::NodeKind::Float; f2.name = "f";
        f2.parentId = tree.nodes[ri].id; f2.offset = 0; tree.addNode(f2);
        QString cpp = rcx::renderCpp(tree, tree.nodes[ri].id);
        QVERIFY(cpp.contains("union MyUnion"));
    }

    void testPythonUnionOutput() {
        rcx::NodeTree tree;
        rcx::Node root; root.kind = rcx::NodeKind::Struct;
        root.structTypeName = "MyUnion"; root.name = "u";
        root.classKeyword = QStringLiteral("union");
        int ri = tree.addNode(root);
        rcx::Node f1; f1.kind = rcx::NodeKind::Int32; f1.name = "i";
        f1.parentId = tree.nodes[ri].id; f1.offset = 0; tree.addNode(f1);
        QString py = rcx::renderPython(tree, tree.nodes[ri].id);
        QVERIFY(py.contains("ctypes.Union"));
    }

    void testPointerFieldCpp() {
        rcx::NodeTree tree;
        rcx::Node target; target.kind = rcx::NodeKind::Struct;
        target.structTypeName = "Target"; target.name = "t";
        int ti = tree.addNode(target);
        rcx::Node root; root.kind = rcx::NodeKind::Struct;
        root.structTypeName = "HasPtr"; root.name = "hp";
        int ri = tree.addNode(root);
        rcx::Node ptr; ptr.kind = rcx::NodeKind::Pointer64;
        ptr.name = "target_ptr"; ptr.parentId = tree.nodes[ri].id;
        ptr.offset = 0; ptr.refId = tree.nodes[ti].id;
        tree.addNode(ptr);
        QString cpp = rcx::renderCpp(tree, tree.nodes[ri].id);
        QVERIFY(cpp.contains("struct Target* target_ptr"));
    }

    void testCSharpStructLayoutSize() {
        auto tree = makeSimpleStruct();
        QString cs = rcx::renderCSharp(tree, tree.nodes[0].id);
        QVERIFY(cs.contains("[StructLayout("));
        QVERIFY(cs.contains("FieldOffset"));
    }

    void testAlignCommentsNoMarkers() {
        // Verify alignComments handles strings without markers gracefully
        // (indirectly: generate code for a struct with no fields)
        rcx::NodeTree tree;
        rcx::Node root; root.kind = rcx::NodeKind::Struct;
        root.structTypeName = "Empty"; root.name = "e";
        tree.addNode(root);
        QString cpp = rcx::renderCpp(tree, tree.nodes[0].id);
        QVERIFY(!cpp.isEmpty());
        QVERIFY(cpp.contains("Empty"));
    }
    void testForwardDeclarationForPointerTarget() {
        // A points to B, A is emitted first → B must be forward-declared
        rcx::NodeTree tree;
        rcx::Node structB;
        structB.kind = rcx::NodeKind::Struct;
        structB.structTypeName = "TargetB";
        structB.name = "b";
        structB.parentId = 0;
        int bi = tree.addNode(structB);
        uint64_t bId = tree.nodes[bi].id;

        // Add a field to B so it's non-empty
        rcx::Node bf;
        bf.kind = rcx::NodeKind::Int32; bf.name = "val";
        bf.parentId = bId; bf.offset = 0;
        tree.addNode(bf);

        rcx::Node structA;
        structA.kind = rcx::NodeKind::Struct;
        structA.structTypeName = "StructA";
        structA.name = "a";
        structA.parentId = 0;
        int ai = tree.addNode(structA);
        uint64_t aId = tree.nodes[ai].id;

        // A has a pointer to B
        rcx::Node ptr;
        ptr.kind = rcx::NodeKind::Pointer64;
        ptr.name = "ptr_to_b";
        ptr.parentId = aId;
        ptr.offset = 0;
        ptr.refId = bId;
        tree.addNode(ptr);

        // Generate C++ for A only → should forward-declare B
        QString cpp = rcx::renderCpp(tree, aId);
        // Must contain "struct TargetB;" forward declaration
        QVERIFY2(cpp.contains("struct TargetB;"),
            qPrintable("Missing forward declaration. Output:\n" + cpp));
        QVERIFY(cpp.contains("struct TargetB* ptr_to_b"));
    }

    void testCppIncludesCstdint() {
        auto tree = makeSimpleStruct();
        QString cpp = rcx::renderCpp(tree, tree.nodes[0].id);
        QVERIFY(cpp.contains("#include <cstdint>"));
    }

    void testRustAllowDeadCode() {
        auto tree = makeSimpleStruct();
        QString rs = rcx::renderRust(tree, tree.nodes[0].id);
        QVERIFY(rs.contains("#[allow(dead_code)]"));
    }

    void testCSharpNullableDisable() {
        auto tree = makeSimpleStruct();
        QString cs = rcx::renderCSharp(tree, tree.nodes[0].id);
        QVERIFY(cs.contains("#nullable disable"));
    }
};

QTEST_MAIN(TestGenerator)
#include "test_generator.moc"
