#include <QtTest/QTest>
#include "core.h"

using namespace rcx;

class TestCompose : public QObject {
    Q_OBJECT
private slots:
    void testBasicStruct() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f1;
        f1.kind = NodeKind::Hex32;
        f1.name = "field_0";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        Node f2;
        f2.kind = NodeKind::Float;
        f2.name = "value";
        f2.parentId = rootId;
        f2.offset = 4;
        tree.addNode(f2);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // CommandRow + Header + 2 fields + footer = 5 lines
        QCOMPARE(result.meta.size(), 5);

        // Line 0 is CommandRow
        QCOMPARE(result.meta[0].lineKind, LineKind::CommandRow);

        // Header is fold head
        QVERIFY(result.meta[1].foldHead);
        QCOMPARE(result.meta[1].lineKind, LineKind::Header);

        // Fields are not fold heads
        QVERIFY(!result.meta[2].foldHead);
        QVERIFY(!result.meta[3].foldHead);

        // Footer
        QCOMPARE(result.meta[4].lineKind, LineKind::Footer);

        // Offset text
        QCOMPARE(result.meta[1].offsetText, QString("0x0"));
        QCOMPARE(result.meta[2].offsetText, QString("0x0"));
        QCOMPARE(result.meta[3].offsetText, QString("0x4"));

        // Header is expanded by default (fold indicator in line text)
        QVERIFY(!result.meta[1].foldCollapsed);
    }

    void testVec3Continuation() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node v;
        v.kind = NodeKind::Vec3;
        v.name = "pos";
        v.parentId = rootId;
        v.offset = 0;
        tree.addNode(v);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // CommandRow + Header + 3 Vec3 lines + footer = 6 lines
        QCOMPARE(result.meta.size(), 6);

        // Line 2 (first Vec3 component): not continuation
        QVERIFY(!result.meta[2].isContinuation);
        QCOMPARE(result.meta[2].offsetText, QString("0x0"));

        // Lines 3-4: continuation
        QVERIFY(result.meta[3].isContinuation);
        QCOMPARE(result.meta[3].offsetText, QString("  \u00B7"));
        QVERIFY(result.meta[4].isContinuation);
        QCOMPARE(result.meta[4].offsetText, QString("  \u00B7"));

        // Continuation marker
        QVERIFY(result.meta[3].markerMask & (1u << M_CONT));
        QVERIFY(result.meta[4].markerMask & (1u << M_CONT));
    }

    void testPaddingMarker() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "R";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node pad;
        pad.kind = NodeKind::Padding;
        pad.name = "pad";
        pad.parentId = rootId;
        pad.offset = 0;
        tree.addNode(pad);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // CommandRow + Header + padding + footer = 4
        QCOMPARE(result.meta.size(), 4);
        QVERIFY(result.meta[2].markerMask & (1u << M_PAD));
    }

    void testNullPointerMarker() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "R";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "ptr";
        ptr.parentId = rootId;
        ptr.offset = 0;
        tree.addNode(ptr);

        // Provider with zeros (null ptr)
        QByteArray data(64, '\0');
        FileProvider prov(data);
        ComposeResult result = compose(tree, prov);

        QCOMPARE(result.meta.size(), 4);
        QVERIFY(result.meta[2].markerMask & (1u << M_PTR0));
    }

    void testCollapsedStruct() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        root.collapsed = true;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::Hex32;
        f.name = "field";
        f.parentId = rootId;
        f.offset = 0;
        tree.addNode(f);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // Collapsed: CommandRow + header only (no children, no footer)
        QCOMPARE(result.meta.size(), 2);
        QVERIFY(result.meta[1].foldHead);
        QVERIFY(result.meta[1].foldCollapsed);
    }

    void testUnreadablePointerNoRead() {
        // A pointer at an unreadable address should get M_ERR, not M_PTR0
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "R";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "ptr";
        ptr.parentId = rootId;
        ptr.offset = 0;
        tree.addNode(ptr);

        // Provider with only 4 bytes — not enough for Pointer64 (8 bytes)
        QByteArray data(4, '\0');
        FileProvider prov(data);
        ComposeResult result = compose(tree, prov);

        QCOMPARE(result.meta.size(), 4);
        // Should have M_ERR, should NOT have M_PTR0
        QVERIFY(result.meta[2].markerMask & (1u << M_ERR));
        QVERIFY(!(result.meta[2].markerMask & (1u << M_PTR0)));
    }

    void testFoldLevels() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node child;
        child.kind = NodeKind::Struct;
        child.name = "Child";
        child.parentId = rootId;
        child.offset = 0;
        int ci = tree.addNode(child);
        uint64_t childId = tree.nodes[ci].id;

        Node leaf;
        leaf.kind = NodeKind::Hex8;
        leaf.name = "x";
        leaf.parentId = childId;
        leaf.offset = 0;
        tree.addNode(leaf);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // Root header (depth 0, head) -> 0x400 | 0x2000
        QCOMPARE(result.meta[1].foldLevel, 0x400 | 0x2000);
        QCOMPARE(result.meta[1].depth, 0);

        // Child header (depth 1, head) -> 0x401 | 0x2000
        QCOMPARE(result.meta[2].foldLevel, 0x401 | 0x2000);
        QCOMPARE(result.meta[2].depth, 1);

        // Leaf (depth 2, not head) -> 0x402
        QCOMPARE(result.meta[3].foldLevel, 0x402);
        QCOMPARE(result.meta[3].depth, 2);
    }

    void testNestedStruct() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Outer";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f1;
        f1.kind = NodeKind::UInt32;
        f1.name = "flags";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        Node inner;
        inner.kind = NodeKind::Struct;
        inner.name = "Inner";
        inner.parentId = rootId;
        inner.offset = 4;
        int ii = tree.addNode(inner);
        uint64_t innerId = tree.nodes[ii].id;

        Node f2;
        f2.kind = NodeKind::UInt16;
        f2.name = "x";
        f2.parentId = innerId;
        f2.offset = 0;
        tree.addNode(f2);

        Node f3;
        f3.kind = NodeKind::UInt16;
        f3.name = "y";
        f3.parentId = innerId;
        f3.offset = 2;
        tree.addNode(f3);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // CommandRow + Outer header + flags + Inner header + x + y + Inner footer + Outer footer = 8
        QCOMPARE(result.meta.size(), 8);

        // Outer header
        QCOMPARE(result.meta[1].lineKind, LineKind::Header);
        QCOMPARE(result.meta[1].depth, 0);
        QVERIFY(result.meta[1].foldHead);

        // flags field
        QCOMPARE(result.meta[2].lineKind, LineKind::Field);
        QCOMPARE(result.meta[2].depth, 1);

        // Inner header
        QCOMPARE(result.meta[3].lineKind, LineKind::Header);
        QCOMPARE(result.meta[3].depth, 1);
        QVERIFY(result.meta[3].foldHead);
        QCOMPARE(result.meta[3].foldLevel, 0x401 | 0x2000);

        // Inner fields at depth 2
        QCOMPARE(result.meta[4].depth, 2);
        QCOMPARE(result.meta[4].foldLevel, 0x402);
        QCOMPARE(result.meta[5].depth, 2);

        // Inner footer
        QCOMPARE(result.meta[6].lineKind, LineKind::Footer);
        QCOMPARE(result.meta[6].depth, 1);

        // Outer footer
        QCOMPARE(result.meta[7].lineKind, LineKind::Footer);
        QCOMPARE(result.meta[7].depth, 0);
    }

    void testPointerDerefExpansion() {
        NodeTree tree;
        tree.baseAddress = 0;

        // Main struct
        Node main;
        main.kind = NodeKind::Struct;
        main.name = "Main";
        main.parentId = 0;
        main.offset = 0;
        int mi = tree.addNode(main);
        uint64_t mainId = tree.nodes[mi].id;

        Node magic;
        magic.kind = NodeKind::UInt32;
        magic.name = "magic";
        magic.parentId = mainId;
        magic.offset = 0;
        tree.addNode(magic);

        // Template struct (separate root)
        Node tmpl;
        tmpl.kind = NodeKind::Struct;
        tmpl.name = "VTable";
        tmpl.parentId = 0;
        tmpl.offset = 200;  // far away so standalone rendering uses offset 200
        int ti = tree.addNode(tmpl);
        uint64_t tmplId = tree.nodes[ti].id;

        Node fn1;
        fn1.kind = NodeKind::UInt64;
        fn1.name = "fn_one";
        fn1.parentId = tmplId;
        fn1.offset = 0;
        tree.addNode(fn1);

        Node fn2;
        fn2.kind = NodeKind::UInt64;
        fn2.name = "fn_two";
        fn2.parentId = tmplId;
        fn2.offset = 8;
        tree.addNode(fn2);

        // Pointer in Main referencing VTable
        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "vtable_ptr";
        ptr.parentId = mainId;
        ptr.offset = 4;
        ptr.refId = tmplId;
        tree.addNode(ptr);

        // Provider: pointer at offset 4 points to address 100
        QByteArray data(256, '\0');
        uint64_t ptrVal = 100;
        memcpy(data.data() + 4, &ptrVal, 8);
        // Some data at the pointer target
        uint64_t v1 = 0xDEADBEEF;
        memcpy(data.data() + 100, &v1, 8);
        uint64_t v2 = 0xCAFEBABE;
        memcpy(data.data() + 108, &v2, 8);
        FileProvider prov(data);

        ComposeResult result = compose(tree, prov);

        // CommandRow + Main: header + magic + ptr(fold head) + VTable header + fn1 + fn2 + VTable footer + Main footer = 9
        // VTable standalone: header + fn1 + fn2 + footer = 4
        // Total = 13
        QCOMPARE(result.meta.size(), 13);

        // Main header
        QCOMPARE(result.meta[1].lineKind, LineKind::Header);
        QCOMPARE(result.meta[1].depth, 0);

        // magic field
        QCOMPARE(result.meta[2].lineKind, LineKind::Field);
        QCOMPARE(result.meta[2].depth, 1);

        // Pointer as fold head
        QCOMPARE(result.meta[3].lineKind, LineKind::Field);
        QCOMPARE(result.meta[3].depth, 1);
        QVERIFY(result.meta[3].foldHead);
        QCOMPARE(result.meta[3].nodeKind, NodeKind::Pointer64);

        // Expanded VTable header at depth 2
        QCOMPARE(result.meta[4].lineKind, LineKind::Header);
        QCOMPARE(result.meta[4].depth, 2);

        // Expanded fields at depth 3
        QCOMPARE(result.meta[5].depth, 3);
        QCOMPARE(result.meta[6].depth, 3);

        // Expanded VTable footer
        QCOMPARE(result.meta[7].lineKind, LineKind::Footer);
        QCOMPARE(result.meta[7].depth, 2);

        // Main footer
        QCOMPARE(result.meta[8].lineKind, LineKind::Footer);
        QCOMPARE(result.meta[8].depth, 0);
    }

    void testPointerDerefNull() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node main;
        main.kind = NodeKind::Struct;
        main.name = "Main";
        main.parentId = 0;
        main.offset = 0;
        int mi = tree.addNode(main);
        uint64_t mainId = tree.nodes[mi].id;

        Node tmpl;
        tmpl.kind = NodeKind::Struct;
        tmpl.name = "Target";
        tmpl.parentId = 0;
        tmpl.offset = 200;
        int ti = tree.addNode(tmpl);
        uint64_t tmplId = tree.nodes[ti].id;

        Node tf;
        tf.kind = NodeKind::UInt32;
        tf.name = "field";
        tf.parentId = tmplId;
        tf.offset = 0;
        tree.addNode(tf);

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "ptr";
        ptr.parentId = mainId;
        ptr.offset = 0;
        ptr.refId = tmplId;
        tree.addNode(ptr);

        // All zeros = null pointer
        QByteArray data(256, '\0');
        FileProvider prov(data);

        ComposeResult result = compose(tree, prov);

        // CommandRow + Main: header + ptr(fold head, no expansion) + footer = 4
        // Target standalone: header + field + footer = 3
        // Total = 7
        QCOMPARE(result.meta.size(), 7);

        // Pointer is fold head but has no children (null ptr)
        QCOMPARE(result.meta[2].lineKind, LineKind::Field);
        QVERIFY(result.meta[2].foldHead);

        // Next line is Main footer (no expansion)
        QCOMPARE(result.meta[3].lineKind, LineKind::Footer);
        QCOMPARE(result.meta[3].depth, 0);
    }

    void testPointerDerefCollapsed() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node main;
        main.kind = NodeKind::Struct;
        main.name = "Main";
        main.parentId = 0;
        main.offset = 0;
        int mi = tree.addNode(main);
        uint64_t mainId = tree.nodes[mi].id;

        Node tmpl;
        tmpl.kind = NodeKind::Struct;
        tmpl.name = "Target";
        tmpl.parentId = 0;
        tmpl.offset = 200;
        int ti = tree.addNode(tmpl);
        uint64_t tmplId = tree.nodes[ti].id;

        Node tf;
        tf.kind = NodeKind::UInt32;
        tf.name = "field";
        tf.parentId = tmplId;
        tf.offset = 0;
        tree.addNode(tf);

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "ptr";
        ptr.parentId = mainId;
        ptr.offset = 0;
        ptr.refId = tmplId;
        ptr.collapsed = true;  // collapsed
        tree.addNode(ptr);

        // Non-null pointer
        QByteArray data(256, '\0');
        uint64_t ptrVal = 100;
        memcpy(data.data(), &ptrVal, 8);
        FileProvider prov(data);

        ComposeResult result = compose(tree, prov);

        // CommandRow + Main: header + ptr(fold head, collapsed) + footer = 4
        // Target standalone: header + field + footer = 3
        // Total = 7
        QCOMPARE(result.meta.size(), 7);

        // Pointer is fold head
        QVERIFY(result.meta[2].foldHead);

        // No expansion — next is Main footer
        QCOMPARE(result.meta[3].lineKind, LineKind::Footer);
        QCOMPARE(result.meta[3].depth, 0);
    }

    void testPointerDerefCycle() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node main;
        main.kind = NodeKind::Struct;
        main.name = "Main";
        main.parentId = 0;
        main.offset = 0;
        int mi = tree.addNode(main);
        uint64_t mainId = tree.nodes[mi].id;

        // Template struct with a self-referencing pointer
        Node tmpl;
        tmpl.kind = NodeKind::Struct;
        tmpl.name = "Recursive";
        tmpl.parentId = 0;
        tmpl.offset = 200;
        int ti = tree.addNode(tmpl);
        uint64_t tmplId = tree.nodes[ti].id;

        Node tf;
        tf.kind = NodeKind::UInt32;
        tf.name = "data";
        tf.parentId = tmplId;
        tf.offset = 0;
        tree.addNode(tf);

        // Self-referencing pointer inside the template
        Node backPtr;
        backPtr.kind = NodeKind::Pointer64;
        backPtr.name = "self";
        backPtr.parentId = tmplId;
        backPtr.offset = 4;
        backPtr.refId = tmplId;  // points back to same struct
        tree.addNode(backPtr);

        // Pointer in Main → Recursive
        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "ptr";
        ptr.parentId = mainId;
        ptr.offset = 0;
        ptr.refId = tmplId;
        tree.addNode(ptr);

        // Provider: main ptr at offset 0 points to 100
        // Inside expansion: backPtr at offset 100+4=104 also points to 100
        QByteArray data(256, '\0');
        uint64_t ptrVal = 100;
        memcpy(data.data(), &ptrVal, 8);       // main ptr → 100
        memcpy(data.data() + 104, &ptrVal, 8); // backPtr at 104 → 100
        FileProvider prov(data);

        ComposeResult result = compose(tree, prov);

        // Must not infinite-loop. Verify we got a finite result.
        QVERIFY(result.meta.size() > 0);
        QVERIFY(result.meta.size() < 100); // sanity: bounded output

        // First expansion happens: CommandRow + Main header + ptr fold head + Recursive header + data + backPtr fold head
        // Second expansion blocked by cycle guard: no children under backPtr
        // Then: Recursive footer + Main footer
        // Plus standalone Recursive rendering
        // The exact count depends on cycle guard behavior but must be finite
        QCOMPARE(result.meta[1].lineKind, LineKind::Header); // Main header
        QVERIFY(result.meta[2].foldHead);                     // ptr fold head
        QCOMPARE(result.meta[3].lineKind, LineKind::Header); // Recursive header (expansion)
    }

    void testStructFooterSimple() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Sized";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f1;
        f1.kind = NodeKind::UInt32;
        f1.name = "a";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // Footer is the last line
        int lastLine = result.meta.size() - 1;
        QCOMPARE(result.meta[lastLine].lineKind, LineKind::Footer);

        // Footer text should just be "};" (no sizeof)
        QString footerText = result.text.split('\n').last();
        QVERIFY(footerText.contains("};"));
        QVERIFY(!footerText.contains("sizeof"));
    }

    void testLineMetaHasNodeId() {
        using namespace rcx;
        NodeTree tree;
        tree.baseAddress = 0;
        Node root; root.kind = NodeKind::Struct; root.name = "Root"; root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f; f.kind = NodeKind::Hex32; f.name = "x"; f.parentId = rootId; f.offset = 0;
        tree.addNode(f);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        for (int i = 0; i < result.meta.size(); i++) {
            // Skip CommandRow (synthetic line with sentinel nodeId)
            if (result.meta[i].lineKind == LineKind::CommandRow) {
                QCOMPARE(result.meta[i].nodeId, kCommandRowId);
                QCOMPARE(result.meta[i].nodeIdx, -1);
                continue;
            }
            QVERIFY2(result.meta[i].nodeId != 0,
                qPrintable(QString("Line %1 has nodeId=0").arg(i)));
            int ni = result.meta[i].nodeIdx;
            QVERIFY(ni >= 0 && ni < tree.nodes.size());
            QCOMPARE(result.meta[i].nodeId, tree.nodes[ni].id);
        }
    }

};

QTEST_MAIN(TestCompose)
#include "test_compose.moc"
