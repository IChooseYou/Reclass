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

        // CommandRow + 2 fields + root footer = 4
        QCOMPARE(result.meta.size(), 4);

        // Line 0 is CommandRow
        QCOMPARE(result.meta[0].lineKind, LineKind::CommandRow);

        // Fields at depth 1
        QVERIFY(!result.meta[1].foldHead);
        QCOMPARE(result.meta[1].depth, 1);
        QVERIFY(!result.meta[2].foldHead);
        QCOMPARE(result.meta[2].depth, 1);

        // Offset text
        QCOMPARE(result.meta[1].offsetText, QString("0000 "));
        QCOMPARE(result.meta[2].offsetText, QString("0004 "));

        // Line 3 is root footer
        QCOMPARE(result.meta[3].lineKind, LineKind::Footer);
    }

    void testVec3SingleLine() {
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

        // CommandRow + 1 Vec3 line + root footer = 3
        QCOMPARE(result.meta.size(), 3);

        // Line 1: single Vec3 line, not continuation, depth 1
        QVERIFY(!result.meta[1].isContinuation);
        QCOMPARE(result.meta[1].offsetText, QString("0000 "));
        QCOMPARE(result.meta[1].depth, 1);
        QCOMPARE(result.meta[1].nodeKind, NodeKind::Vec3);

        // Line 2 is root footer
        QCOMPARE(result.meta[2].lineKind, LineKind::Footer);
    }

    void testHexNodeCompose() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "R";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node hex;
        hex.kind = NodeKind::Hex8;
        hex.name = "pad";
        hex.parentId = rootId;
        hex.offset = 0;
        tree.addNode(hex);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // CommandRow + hex node + root footer = 3
        QCOMPARE(result.meta.size(), 3);
        QCOMPARE(result.meta[1].depth, 1);

        // Line 2 is root footer
        QCOMPARE(result.meta[2].lineKind, LineKind::Footer);
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
        BufferProvider prov(data);
        ComposeResult result = compose(tree, prov);

        // CommandRow + ptr + root footer = 3
        QCOMPARE(result.meta.size(), 3);
        // No ambient validation markers — M_PTR0 is no longer set
        QVERIFY(!(result.meta[1].markerMask & (1u << M_PTR0)));
        QCOMPARE(result.meta[1].depth, 1);

        // Line 2 is root footer
        QCOMPARE(result.meta[2].lineKind, LineKind::Footer);
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

        // Collapsed root: isRootHeader overrides collapse, so children + footer still render
        // CommandRow + field + root footer = 3
        QCOMPARE(result.meta.size(), 3);
        QCOMPARE(result.meta[1].lineKind, LineKind::Field);
        QCOMPARE(result.meta[1].depth, 1);
        QCOMPARE(result.meta[2].lineKind, LineKind::Footer);
    }

    void testUnreadablePointerNoRead() {
        // No ambient validation — neither M_ERR nor M_PTR0 set
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
        BufferProvider prov(data);
        ComposeResult result = compose(tree, prov);

        // CommandRow + ptr + root footer = 3
        QCOMPARE(result.meta.size(), 3);
        // No ambient validation markers
        QVERIFY(!(result.meta[1].markerMask & (1u << M_ERR)));
        QVERIFY(!(result.meta[1].markerMask & (1u << M_PTR0)));
        QCOMPARE(result.meta[1].depth, 1);

        // Line 2 is root footer
        QCOMPARE(result.meta[2].lineKind, LineKind::Footer);
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

        // Child header (depth 1, fold head) — root header no longer emitted
        QCOMPARE(result.meta[1].foldLevel, 0x401 | 0x2000);
        QCOMPARE(result.meta[1].depth, 1);
        QVERIFY(result.meta[1].foldHead);

        // Leaf (depth 2, not head)
        QCOMPARE(result.meta[2].foldLevel, 0x402);
        QCOMPARE(result.meta[2].depth, 2);
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

        // CommandRow + flags + Inner header + x + y + Inner footer + root footer = 7
        QCOMPARE(result.meta.size(), 7);

        // flags field (depth 1)
        QCOMPARE(result.meta[1].lineKind, LineKind::Field);
        QCOMPARE(result.meta[1].depth, 1);

        // Inner header (depth 1, fold head)
        QCOMPARE(result.meta[2].lineKind, LineKind::Header);
        QCOMPARE(result.meta[2].depth, 1);
        QVERIFY(result.meta[2].foldHead);
        QCOMPARE(result.meta[2].foldLevel, 0x401 | 0x2000);

        // Inner fields at depth 2
        QCOMPARE(result.meta[3].depth, 2);
        QCOMPARE(result.meta[3].foldLevel, 0x402);
        QCOMPARE(result.meta[4].depth, 2);

        // Inner footer
        QCOMPARE(result.meta[5].lineKind, LineKind::Footer);
        QCOMPARE(result.meta[5].depth, 1);

        // Root footer
        QCOMPARE(result.meta[6].lineKind, LineKind::Footer);
        QCOMPARE(result.meta[6].depth, 0);
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
        BufferProvider prov(data);

        ComposeResult result = compose(tree, prov);

        // CommandRow + magic + ptr(merged fold header) + fn1 + fn2 + ptr footer + Main footer = 7
        // VTable standalone: header + fn1 + fn2 + footer = 4
        // Total = 11
        QCOMPARE(result.meta.size(), 11);

        // magic field (depth 1)
        QCOMPARE(result.meta[1].lineKind, LineKind::Field);
        QCOMPARE(result.meta[1].depth, 1);

        // Pointer as merged fold header: "VTable* ptr {"
        QCOMPARE(result.meta[2].lineKind, LineKind::Header);
        QCOMPARE(result.meta[2].depth, 1);
        QVERIFY(result.meta[2].foldHead);
        QCOMPARE(result.meta[2].nodeKind, NodeKind::Pointer64);

        // Expanded fields at depth 2 (struct header merged into pointer)
        QCOMPARE(result.meta[3].depth, 2);
        QCOMPARE(result.meta[4].depth, 2);

        // Pointer fold footer
        QCOMPARE(result.meta[5].lineKind, LineKind::Footer);
        QCOMPARE(result.meta[5].depth, 1);
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
        BufferProvider prov(data);

        ComposeResult result = compose(tree, prov);

        // CommandRow + ptr(merged fold header) + target field + ptr footer + Main footer = 5
        // Target standalone: header + field + footer = 3
        // Total = 8  (null ptr still shows template preview)
        QCOMPARE(result.meta.size(), 8);

        // Pointer as merged fold header (expanded — shows template at offset 0)
        QCOMPARE(result.meta[1].lineKind, LineKind::Header);
        QCOMPARE(result.meta[1].depth, 1);
        QVERIFY(result.meta[1].foldHead);

        // Target field shown as template preview
        QCOMPARE(result.meta[2].lineKind, LineKind::Field);
        QCOMPARE(result.meta[2].depth, 2);

        // Pointer fold footer
        QCOMPARE(result.meta[3].lineKind, LineKind::Footer);
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
        BufferProvider prov(data);

        ComposeResult result = compose(tree, prov);

        // CommandRow + ptr(fold head, collapsed) + Main footer = 3
        // Target standalone: header + field + footer = 3
        // Total = 6
        QCOMPARE(result.meta.size(), 6);

        // Pointer is fold head (depth 1)
        QVERIFY(result.meta[1].foldHead);
        QCOMPARE(result.meta[1].depth, 1);
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
        BufferProvider prov(data);

        ComposeResult result = compose(tree, prov);

        // Must not infinite-loop. Verify we got a finite result.
        QVERIFY(result.meta.size() > 0);
        QVERIFY(result.meta.size() < 100); // sanity: bounded output

        // CommandRow + ptr merged header + data + self merged header
        // Second expansion blocked by cycle guard: no children under self
        // Then: self footer + ptr footer + Main footer + standalone Recursive rendering
        QVERIFY(result.meta[1].foldHead);                     // ptr merged fold head
        QCOMPARE(result.meta[1].lineKind, LineKind::Header); // ptr merged header
        QCOMPARE(result.meta[2].lineKind, LineKind::Field);  // data field (first child of Recursive)
    }

    void testStructFooterSimple() {
        // Root footer is suppressed; test nested struct footer instead
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node inner;
        inner.kind = NodeKind::Struct;
        inner.name = "Inner";
        inner.parentId = rootId;
        inner.offset = 0;
        int ii = tree.addNode(inner);
        uint64_t innerId = tree.nodes[ii].id;

        Node f1;
        f1.kind = NodeKind::UInt32;
        f1.name = "a";
        f1.parentId = innerId;
        f1.offset = 0;
        tree.addNode(f1);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // Find a footer line (nested struct footer)
        int footerLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].lineKind == LineKind::Footer) {
                footerLine = i;
                break;
            }
        }
        QVERIFY2(footerLine >= 0, "Should have a footer for nested struct");

        // Footer text should contain "};" (no sizeof)
        QStringList lines = result.text.split('\n');
        QVERIFY(lines[footerLine].contains("};"));
        QVERIFY(!lines[footerLine].contains("sizeof"));
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

    // ═════════════════════════════════════════════════════════════
    // Array tests
    // ═════════════════════════════════════════════════════════════

    void testArrayHeaderFormat() {
        // Array header must show "elemType[count]" text and proper metadata
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "data";
        arr.parentId = rootId;
        arr.offset = 0;
        arr.elementKind = NodeKind::Int32;
        arr.arrayLen = 10;
        tree.addNode(arr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // Find the array header line
        int headerLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].isArrayHeader) {
                headerLine = i;
                break;
            }
        }
        QVERIFY(headerLine >= 0);

        // Metadata must be correct
        const LineMeta& lm = result.meta[headerLine];
        QCOMPARE(lm.lineKind, LineKind::Header);
        QVERIFY(lm.isArrayHeader);
        QCOMPARE(lm.elementKind, NodeKind::Int32);
        QCOMPARE(lm.arrayCount, 10);
        QVERIFY(lm.foldHead);
        QVERIFY(!lm.foldCollapsed);

        // Text must contain "int32_t[10]" and the name
        QStringList lines = result.text.split('\n');
        QVERIFY(headerLine < lines.size());
        QString text = lines[headerLine];
        QVERIFY2(text.contains("int32_t[10]"),
                 qPrintable("Header should contain 'int32_t[10]': " + text));
        QVERIFY2(text.contains("data"),
                 qPrintable("Header should contain 'data': " + text));
        QVERIFY2(text.contains("{"),
                 qPrintable("Expanded header should contain '{': " + text));
    }

    void testArrayHeaderCharTypes() {
        // UInt8 array → "uint8_t[N]", UInt16 → "uint16_t[N]"
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node arr1;
        arr1.kind = NodeKind::Array;
        arr1.name = "str";
        arr1.parentId = rootId;
        arr1.offset = 0;
        arr1.elementKind = NodeKind::UInt8;
        arr1.arrayLen = 64;
        tree.addNode(arr1);

        Node arr2;
        arr2.kind = NodeKind::Array;
        arr2.name = "wstr";
        arr2.parentId = rootId;
        arr2.offset = 64;
        arr2.elementKind = NodeKind::UInt16;
        arr2.arrayLen = 32;
        tree.addNode(arr2);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        QStringList lines = result.text.split('\n');
        bool foundChar = false, foundWchar = false;
        for (int i = 0; i < result.meta.size(); i++) {
            if (!result.meta[i].isArrayHeader) continue;
            QString text = lines[i];
            if (text.contains("uint8_t[64]")) foundChar = true;
            if (text.contains("uint16_t[32]")) foundWchar = true;
        }
        QVERIFY2(foundChar, "Should have 'uint8_t[64]' header");
        QVERIFY2(foundWchar, "Should have 'uint16_t[32]' header");
    }

    void testArraySpansClickable() {
        // Element type and count spans must cover the correct text regions
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "numbers";
        arr.parentId = rootId;
        arr.offset = 0;
        arr.elementKind = NodeKind::UInt32;
        arr.arrayLen = 5;
        tree.addNode(arr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        int headerLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].isArrayHeader) { headerLine = i; break; }
        }
        QVERIFY(headerLine >= 0);

        QStringList lines = result.text.split('\n');
        QString lineText = lines[headerLine];
        const LineMeta& lm = result.meta[headerLine];

        // Element type span must be valid and cover "uint32_t"
        ColumnSpan typeSpan = arrayElemTypeSpanFor(lm, lineText);
        QVERIFY2(typeSpan.valid, "arrayElemTypeSpanFor must return a valid span");
        QVERIFY(typeSpan.start < typeSpan.end);
        QString typeText = lineText.mid(typeSpan.start, typeSpan.end - typeSpan.start);
        QVERIFY2(typeText.contains("uint32_t"),
                 qPrintable("Type span should cover 'uint32_t', got: '" + typeText + "'"));

        // Element count span must be valid and cover "5"
        ColumnSpan countSpan = arrayElemCountSpanFor(lm, lineText);
        QVERIFY2(countSpan.valid, "arrayElemCountSpanFor must return a valid span");
        QVERIFY(countSpan.start < countSpan.end);
        QString countText = lineText.mid(countSpan.start, countSpan.end - countSpan.start);
        QCOMPARE(countText, QString("5"));
    }

    void testArrayWithStructChildren() {
        // Array with struct children renders separators and child fields
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Array container
        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "items";
        arr.parentId = rootId;
        arr.offset = 0;
        arr.elementKind = NodeKind::Int32;
        arr.arrayLen = 2;
        int ai = tree.addNode(arr);
        uint64_t arrId = tree.nodes[ai].id;

        // Two struct children inside the array (representing elements)
        Node elem0;
        elem0.kind = NodeKind::Struct;
        elem0.name = "Item";
        elem0.parentId = arrId;
        elem0.offset = 0;
        int e0i = tree.addNode(elem0);
        uint64_t elem0Id = tree.nodes[e0i].id;

        Node f0;
        f0.kind = NodeKind::UInt32;
        f0.name = "value";
        f0.parentId = elem0Id;
        f0.offset = 0;
        tree.addNode(f0);

        Node elem1;
        elem1.kind = NodeKind::Struct;
        elem1.name = "Item";
        elem1.parentId = arrId;
        elem1.offset = 4;
        int e1i = tree.addNode(elem1);
        uint64_t elem1Id = tree.nodes[e1i].id;

        Node f1;
        f1.kind = NodeKind::UInt32;
        f1.name = "value";
        f1.parentId = elem1Id;
        f1.offset = 0;
        tree.addNode(f1);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // Must have content between header and footer (not empty!)
        QVERIFY2(result.meta.size() > 4,
                 qPrintable(QString("Array should have content, got %1 lines")
                            .arg(result.meta.size())));

        // Check for [0] and [1] separators
        bool found0 = false, found1 = false;
        int fieldCount = 0;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].lineKind == LineKind::ArrayElementSeparator) {
                if (result.meta[i].arrayElementIdx == 0) found0 = true;
                if (result.meta[i].arrayElementIdx == 1) found1 = true;
            }
            // Count fields belonging to array children
            if (result.meta[i].lineKind == LineKind::Field &&
                result.meta[i].depth >= 2)
                fieldCount++;
        }
        QVERIFY2(found0, "Array should have [0] separator");
        QVERIFY2(found1, "Array should have [1] separator");
        QVERIFY2(fieldCount >= 2, "Array children should have field lines");
    }

    void testArrayCollapsedNoChildren() {
        // Collapsed array: header only, no children or footer
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "data";
        arr.parentId = rootId;
        arr.offset = 0;
        arr.elementKind = NodeKind::Float;
        arr.arrayLen = 100;
        arr.collapsed = true;
        int ai = tree.addNode(arr);
        uint64_t arrId = tree.nodes[ai].id;

        // Child that should NOT appear when collapsed
        Node child;
        child.kind = NodeKind::Float;
        child.name = "elem";
        child.parentId = arrId;
        child.offset = 0;
        tree.addNode(child);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // CommandRow + Array header(collapsed) + root footer = 3
        QCOMPARE(result.meta.size(), 3);

        // Array header is collapsed (at index 1)
        int arrLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].isArrayHeader) { arrLine = i; break; }
        }
        QVERIFY(arrLine >= 0);
        QCOMPARE(arrLine, 1);
        QVERIFY(result.meta[arrLine].foldCollapsed);

        // Header text should NOT contain "{"
        QStringList lines = result.text.split('\n');
        QVERIFY2(!lines[arrLine].contains("{"),
                 qPrintable("Collapsed header should not have '{': " + lines[arrLine]));
    }

    void testArrayCountRecompose() {
        // After changing arrayLen and recomposing, the text shows the new count
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "buf";
        arr.parentId = rootId;
        arr.offset = 0;
        arr.elementKind = NodeKind::UInt8;
        arr.arrayLen = 10;
        int ai = tree.addNode(arr);

        NullProvider prov;

        // First compose: should show [10]
        ComposeResult r1 = compose(tree, prov);
        QStringList lines1 = r1.text.split('\n');
        bool found10 = false;
        for (const QString& l : lines1) {
            if (l.contains("[10]")) { found10 = true; break; }
        }
        QVERIFY2(found10, "First compose should show [10]");

        // Change count and recompose
        tree.nodes[ai].arrayLen = 42;
        ComposeResult r2 = compose(tree, prov);
        QStringList lines2 = r2.text.split('\n');
        bool found42 = false;
        bool still10Header = false;
        for (int i = 0; i < r2.meta.size(); i++) {
            if (r2.meta[i].isArrayHeader && lines2[i].contains("uint8_t[42]")) found42 = true;
            if (r2.meta[i].isArrayHeader && lines2[i].contains("uint8_t[10]")) still10Header = true;
        }
        QVERIFY2(found42, "Recomposed header should show uint8_t[42]");
        QVERIFY2(!still10Header, "Recomposed header should NOT still show uint8_t[10]");

        // Spans must still work after recompose
        int headerLine = -1;
        for (int i = 0; i < r2.meta.size(); i++) {
            if (r2.meta[i].isArrayHeader) { headerLine = i; break; }
        }
        QVERIFY(headerLine >= 0);
        ColumnSpan countSpan = arrayElemCountSpanFor(r2.meta[headerLine], lines2[headerLine]);
        QVERIFY2(countSpan.valid, "Count span must be valid after recompose");
        QString countText = lines2[headerLine].mid(countSpan.start, countSpan.end - countSpan.start);
        QCOMPARE(countText, QString("42"));
    }

    void testPrimitiveArrayElements() {
        // Expanded primitive array should synthesize element lines dynamically
        NodeTree tree;
        tree.baseAddress = 0x1000;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "values";
        arr.parentId = rootId;
        arr.offset = 0;
        arr.elementKind = NodeKind::UInt32;
        arr.arrayLen = 4;
        tree.addNode(arr);

        // Buffer with known values: 0x11, 0x22, 0x33, 0x44
        QByteArray data(64, '\0');
        uint32_t v0 = 0x11, v1 = 0x22, v2 = 0x33, v3 = 0x44;
        memcpy(data.data() + 0, &v0, 4);
        memcpy(data.data() + 4, &v1, 4);
        memcpy(data.data() + 8, &v2, 4);
        memcpy(data.data() + 12, &v3, 4);
        BufferProvider prov(data);

        ComposeResult result = compose(tree, prov);
        QStringList lines = result.text.split('\n');

        // Find array header
        int headerLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].isArrayHeader) { headerLine = i; break; }
        }
        QVERIFY2(headerLine >= 0, "Array header must exist");
        QVERIFY2(lines[headerLine].contains("uint32_t[4]"),
                 qPrintable("Header should contain 'uint32_t[4]': " + lines[headerLine]));

        // Count element field lines (depth >= 2, lineKind == Field)
        int elemCount = 0;
        bool found0 = false, found3 = false;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].lineKind == LineKind::Field && result.meta[i].depth >= 2) {
                elemCount++;
                // Type column should have combined type+index: "uint32_t[0]"
                if (lines[i].contains("uint32_t[0]")) found0 = true;
                if (lines[i].contains("uint32_t[3]")) found3 = true;
                // isArrayElement flag must be set
                QVERIFY2(result.meta[i].isArrayElement,
                         qPrintable("Element line must have isArrayElement=true: " + lines[i]));
            }
        }
        QCOMPARE(elemCount, 4);
        QVERIFY2(found0, "Should have uint32_t[0] element");
        QVERIFY2(found3, "Should have uint32_t[3] element");

        // Check footer exists
        bool hasFooter = false;
        for (int i = headerLine + 1; i < result.meta.size(); i++) {
            if (result.meta[i].lineKind == LineKind::Footer && result.meta[i].nodeKind == NodeKind::Array) {
                hasFooter = true;
                break;
            }
        }
        QVERIFY2(hasFooter, "Array should have footer line");
    }

    void testPrimitiveArrayCollapsed() {
        // Collapsed primitive array should show NO element lines
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "data";
        arr.parentId = rootId;
        arr.offset = 0;
        arr.elementKind = NodeKind::UInt16;
        arr.arrayLen = 8;
        arr.collapsed = true;
        tree.addNode(arr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // No field lines at depth >= 2 (no synthesized elements)
        int elemFields = 0;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].lineKind == LineKind::Field && result.meta[i].depth >= 2)
                elemFields++;
        }
        QCOMPARE(elemFields, 0);
    }

    void testStructArrayStillUsesChildren() {
        // Struct array with manual children should still render child nodes, not synthesize
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "items";
        arr.parentId = rootId;
        arr.offset = 0;
        arr.elementKind = NodeKind::Struct;
        arr.arrayLen = 1;
        int ai = tree.addNode(arr);
        uint64_t arrId = tree.nodes[ai].id;

        // One struct child
        Node elem;
        elem.kind = NodeKind::Struct;
        elem.name = "Item";
        elem.parentId = arrId;
        elem.offset = 0;
        int ei = tree.addNode(elem);
        uint64_t elemId = tree.nodes[ei].id;

        Node field;
        field.kind = NodeKind::UInt32;
        field.name = "val";
        field.parentId = elemId;
        field.offset = 0;
        tree.addNode(field);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // Should have the child struct's field rendered
        bool hasVal = false;
        QStringList lines = result.text.split('\n');
        for (int i = 0; i < lines.size(); i++) {
            if (lines[i].contains("val")) { hasVal = true; break; }
        }
        QVERIFY2(hasVal, "Struct array child field 'val' should be rendered");
    }

    // ═════════════════════════════════════════════════════════════
    // Pointer tests
    // ═════════════════════════════════════════════════════════════

    void testPointerDefaultVoid() {
        // Pointer64 with no refId should display as "void*"
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "ptr";
        ptr.parentId = rootId;
        ptr.offset = 0;
        // refId defaults to 0 (void*)
        tree.addNode(ptr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // Find the pointer line
        int ptrLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].nodeKind == NodeKind::Pointer64 &&
                result.meta[i].lineKind == LineKind::Field) {
                ptrLine = i;
                break;
            }
        }
        QVERIFY(ptrLine >= 0);

        QStringList lines = result.text.split('\n');
        QString text = lines[ptrLine];
        QVERIFY2(text.contains("void*"),
                 qPrintable("Pointer with no refId should show 'void*': " + text));

        // pointerTargetName should be empty (void)
        QVERIFY(result.meta[ptrLine].pointerTargetName.isEmpty());

        // Should NOT be a fold head (no deref expansion for void*)
        QVERIFY(!result.meta[ptrLine].foldHead);
    }

    void testPointer32DefaultVoid() {
        // Same for Pointer32
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer32;
        ptr.name = "ptr32";
        ptr.parentId = rootId;
        ptr.offset = 0;
        tree.addNode(ptr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        QStringList lines = result.text.split('\n');
        bool foundPtr32 = false;
        for (const QString& l : lines) {
            if (l.contains("void*")) { foundPtr32 = true; break; }
        }
        QVERIFY2(foundPtr32, "Pointer32 with no refId should show 'void*'");
    }

    void testPointerDisplaysTargetName() {
        // Pointer64 with refId displays "TargetName*"
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Target struct with a structTypeName
        Node target;
        target.kind = NodeKind::Struct;
        target.name = "PlayerData";
        target.structTypeName = "PlayerData";
        target.parentId = 0;
        target.offset = 200;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        Node tf;
        tf.kind = NodeKind::UInt32;
        tf.name = "health";
        tf.parentId = targetId;
        tf.offset = 0;
        tree.addNode(tf);

        // Pointer referencing the target (collapsed to prevent expansion)
        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "player";
        ptr.parentId = rootId;
        ptr.offset = 0;
        ptr.refId = targetId;
        ptr.collapsed = true;
        tree.addNode(ptr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // Find the pointer line (root children at depth 0 due to root suppression)
        int ptrLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].nodeKind == NodeKind::Pointer64 &&
                result.meta[i].lineKind == LineKind::Field) {
                ptrLine = i;
                break;
            }
        }
        QVERIFY(ptrLine >= 0);

        QStringList lines = result.text.split('\n');
        QVERIFY2(lines[ptrLine].contains("PlayerData*"),
                 qPrintable("Should show 'PlayerData*': " + lines[ptrLine]));

        // pointerTargetName metadata
        QCOMPARE(result.meta[ptrLine].pointerTargetName, QString("PlayerData"));

        // Pointer with refId is a fold head (even if collapsed)
        QVERIFY(result.meta[ptrLine].foldHead);
        QVERIFY(result.meta[ptrLine].foldCollapsed);
    }

    void testPointerTargetUsesNameWhenNoTypeName() {
        // If target struct has no structTypeName, use its name field
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node target;
        target.kind = NodeKind::Struct;
        target.name = "MyStruct";
        // structTypeName left empty
        target.parentId = 0;
        target.offset = 200;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "sptr";
        ptr.parentId = rootId;
        ptr.offset = 0;
        ptr.refId = targetId;
        ptr.collapsed = true;
        tree.addNode(ptr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        QStringList lines = result.text.split('\n');
        bool found = false;
        for (const QString& l : lines) {
            if (l.contains("MyStruct*")) { found = true; break; }
        }
        QVERIFY2(found, "Should use struct name when structTypeName is empty");
    }

    void testPointerSpans() {
        // pointerKindSpanFor and pointerTargetSpanFor must find correct regions
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node target;
        target.kind = NodeKind::Struct;
        target.name = "VTable";
        target.structTypeName = "VTable";
        target.parentId = 0;
        target.offset = 200;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "vtbl";
        ptr.parentId = rootId;
        ptr.offset = 0;
        ptr.refId = targetId;
        ptr.collapsed = true;
        tree.addNode(ptr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        int ptrLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].nodeKind == NodeKind::Pointer64 &&
                result.meta[i].lineKind == LineKind::Field) {
                ptrLine = i;
                break;
            }
        }
        QVERIFY(ptrLine >= 0);

        QStringList lines = result.text.split('\n');
        QString lineText = lines[ptrLine];
        const LineMeta& lm = result.meta[ptrLine];

        // Kind span: no longer applicable in "Type*" format
        ColumnSpan kindSpan = pointerKindSpanFor(lm, lineText);
        QVERIFY2(!kindSpan.valid, "pointerKindSpanFor should return invalid in Type* format");

        // Target span: covers "VTable" (before the '*')
        ColumnSpan targetSpan = pointerTargetSpanFor(lm, lineText);
        QVERIFY2(targetSpan.valid, "pointerTargetSpanFor must return valid span");
        QString targetText = lineText.mid(targetSpan.start, targetSpan.end - targetSpan.start).trimmed();
        QCOMPARE(targetText, QString("VTable"));
    }

    void testPointerVoidSpans() {
        // void* pointer should have valid target span but no kind span
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "vptr";
        ptr.parentId = rootId;
        ptr.offset = 0;
        tree.addNode(ptr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        int ptrLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].nodeKind == NodeKind::Pointer64 &&
                result.meta[i].lineKind == LineKind::Field) {
                ptrLine = i;
                break;
            }
        }
        QVERIFY(ptrLine >= 0);

        QStringList lines = result.text.split('\n');
        QString lineText = lines[ptrLine];
        const LineMeta& lm = result.meta[ptrLine];

        // Kind span: no longer applicable in "Type*" format
        ColumnSpan kindSpan = pointerKindSpanFor(lm, lineText);
        QVERIFY2(!kindSpan.valid, "Kind span should be invalid in Type* format");

        // Target span: "void" (before the '*')
        ColumnSpan targetSpan = pointerTargetSpanFor(lm, lineText);
        QVERIFY2(targetSpan.valid, "void* pointer should have valid target span");
        QString targetText = lineText.mid(targetSpan.start, targetSpan.end - targetSpan.start).trimmed();
        QCOMPARE(targetText, QString("void"));
    }

    void testPointerToPointerChain() {
        // StructB* → StructB { StructC* } → StructC { field }
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // StructC (innermost target)
        Node structC;
        structC.kind = NodeKind::Struct;
        structC.name = "InnerData";
        structC.structTypeName = "InnerData";
        structC.parentId = 0;
        structC.offset = 300;
        int ci = tree.addNode(structC);
        uint64_t structCId = tree.nodes[ci].id;

        Node cf;
        cf.kind = NodeKind::UInt64;
        cf.name = "payload";
        cf.parentId = structCId;
        cf.offset = 0;
        tree.addNode(cf);

        // StructB (middle target, contains ptr to C)
        Node structB;
        structB.kind = NodeKind::Struct;
        structB.name = "Wrapper";
        structB.structTypeName = "Wrapper";
        structB.parentId = 0;
        structB.offset = 200;
        int bi = tree.addNode(structB);
        uint64_t structBId = tree.nodes[bi].id;

        Node bf;
        bf.kind = NodeKind::UInt32;
        bf.name = "flags";
        bf.parentId = structBId;
        bf.offset = 0;
        tree.addNode(bf);

        Node bptr;
        bptr.kind = NodeKind::Pointer64;
        bptr.name = "inner";
        bptr.parentId = structBId;
        bptr.offset = 4;
        bptr.refId = structCId;  // points to InnerData
        tree.addNode(bptr);

        // Root's pointer to StructB
        Node rptr;
        rptr.kind = NodeKind::Pointer64;
        rptr.name = "wrapper_ptr";
        rptr.parentId = rootId;
        rptr.offset = 0;
        rptr.refId = structBId;
        tree.addNode(rptr);

        // Provider: rptr at 0 → addr 100, bptr at 100+4=104 → addr 150
        QByteArray data(400, '\0');
        uint64_t val1 = 100;
        memcpy(data.data(), &val1, 8);       // rptr → 100
        uint64_t val2 = 150;
        memcpy(data.data() + 104, &val2, 8); // bptr at 104 → 150
        BufferProvider prov(data);

        ComposeResult result = compose(tree, prov);

        // Must finish (no infinite loop)
        QVERIFY(result.meta.size() > 0);
        QVERIFY(result.meta.size() < 200);

        // Check that Wrapper* and InnerData* both appear in text
        bool foundWrapper = false, foundInner = false;
        QStringList lines = result.text.split('\n');
        for (const QString& l : lines) {
            if (l.contains("Wrapper*")) foundWrapper = true;
            if (l.contains("InnerData*")) foundInner = true;
        }
        QVERIFY2(foundWrapper, "Should display 'Wrapper*'");
        QVERIFY2(foundInner, "Should display 'InnerData*'");

        // The chain: Root → Wrapper*(fold head) → Wrapper expanded →
        //   InnerData*(fold head) → InnerData expanded
        int foldHeadCount = 0;
        for (const LineMeta& lm : result.meta) {
            if (lm.foldHead && lm.nodeKind == NodeKind::Pointer64)
                foldHeadCount++;
        }
        // At least 2 fold-head pointers in the expansion chain (rptr + bptr)
        // Plus standalone renderings of StructB and StructC
        QVERIFY2(foldHeadCount >= 2,
                 qPrintable(QString("Expected >=2 pointer fold heads, got %1")
                            .arg(foldHeadCount)));
    }

    void testPointerMutualCycleAtoB() {
        // A→B→A: Main has ptr to StructB, StructB has ptr back to Main
        // Must not infinite-loop
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

        Node mf;
        mf.kind = NodeKind::UInt32;
        mf.name = "tag";
        mf.parentId = mainId;
        mf.offset = 0;
        tree.addNode(mf);

        // StructB
        Node structB;
        structB.kind = NodeKind::Struct;
        structB.name = "StructB";
        structB.parentId = 0;
        structB.offset = 200;
        int bi = tree.addNode(structB);
        uint64_t structBId = tree.nodes[bi].id;

        Node bf;
        bf.kind = NodeKind::UInt32;
        bf.name = "data";
        bf.parentId = structBId;
        bf.offset = 0;
        tree.addNode(bf);

        // Main → StructB pointer
        Node ptrToB;
        ptrToB.kind = NodeKind::Pointer64;
        ptrToB.name = "to_b";
        ptrToB.parentId = mainId;
        ptrToB.offset = 4;
        ptrToB.refId = structBId;
        tree.addNode(ptrToB);

        // StructB → Main pointer (creates cycle!)
        Node ptrToMain;
        ptrToMain.kind = NodeKind::Pointer64;
        ptrToMain.name = "back";
        ptrToMain.parentId = structBId;
        ptrToMain.offset = 4;
        ptrToMain.refId = mainId;
        tree.addNode(ptrToMain);

        // Provider: Main.to_b at offset 4 → addr 100
        //           StructB expanded at 100: back at 100+4=104 → addr 50
        //           Main expanded at 50: to_b at 50+4=54 → addr 100 (same as before → cycle!)
        QByteArray data(300, '\0');
        uint64_t val1 = 100;
        memcpy(data.data() + 4, &val1, 8);     // Main.to_b → 100
        uint64_t val2 = 50;
        memcpy(data.data() + 104, &val2, 8);   // StructB.back at 104 → 50
        uint64_t val3 = 100;
        memcpy(data.data() + 54, &val3, 8);    // Main.to_b at 54 → 100 (cycle)
        BufferProvider prov(data);

        ComposeResult result = compose(tree, prov);

        // MUST terminate with bounded output
        QVERIFY(result.meta.size() > 0);
        QVERIFY2(result.meta.size() < 100,
                 qPrintable(QString("Cycle should be bounded, got %1 lines")
                            .arg(result.meta.size())));

        // Both StructB* and Main* should appear
        bool foundToB = false, foundToMain = false;
        QStringList lines = result.text.split('\n');
        for (const QString& l : lines) {
            if (l.contains("StructB*")) foundToB = true;
            if (l.contains("Main*")) foundToMain = true;
        }
        QVERIFY2(foundToB, "Should display 'StructB*'");
        QVERIFY2(foundToMain, "Should display 'Main*'");

        // The first expansion of each pointer works;
        // the cycle is caught on the second attempt.
        // Main root header is suppressed, and pointer deref uses isArrayChild=true
        // (which also skips headers), so we verify cycle detection by bounded output above.
    }

    void testAllStructsResolvedAsPointerTargets() {
        // Multiple structs in the tree; pointers to each should display the name
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Create several structs
        QStringList structNames = {"Alpha", "Bravo", "Charlie", "Delta"};
        QVector<uint64_t> structIds;
        for (int i = 0; i < structNames.size(); i++) {
            Node s;
            s.kind = NodeKind::Struct;
            s.name = structNames[i];
            s.structTypeName = structNames[i];
            s.parentId = 0;
            s.offset = 1000 + i * 100;
            int si = tree.addNode(s);
            structIds << tree.nodes[si].id;

            // Give each struct a field
            Node f;
            f.kind = NodeKind::UInt32;
            f.name = "x";
            f.parentId = tree.nodes[si].id;
            f.offset = 0;
            tree.addNode(f);
        }

        // Create a pointer to each struct
        for (int i = 0; i < structIds.size(); i++) {
            Node ptr;
            ptr.kind = NodeKind::Pointer64;
            ptr.name = QString("ptr_%1").arg(structNames[i].toLower());
            ptr.parentId = rootId;
            ptr.offset = i * 8;
            ptr.refId = structIds[i];
            ptr.collapsed = true;  // don't expand
            tree.addNode(ptr);
        }

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // Every struct name should appear in a "Name*" format
        QStringList lines = result.text.split('\n');
        for (const QString& sname : structNames) {
            QString expected = QString("%1*").arg(sname);
            bool found = false;
            for (const QString& l : lines) {
                if (l.contains(expected)) { found = true; break; }
            }
            QVERIFY2(found, qPrintable(QString("Should display '%1'").arg(expected)));
        }
    }

    void testPointerRefIdToDeletedStruct() {
        // If refId points to a non-existent node, degrade to void*
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "dangling";
        ptr.parentId = rootId;
        ptr.offset = 0;
        ptr.refId = 99999;  // non-existent ID
        tree.addNode(ptr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // Should not crash, and degrade to void
        QStringList lines = result.text.split('\n');
        bool foundVoid = false;
        for (const QString& l : lines) {
            if (l.contains("void*")) { foundVoid = true; break; }
        }
        QVERIFY2(foundVoid, "Dangling refId should degrade to void*");
    }

    void testPointerCollapsedNoExpansion() {
        // Collapsed pointer with valid non-null target must NOT expand
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node target;
        target.kind = NodeKind::Struct;
        target.name = "Heavy";
        target.parentId = 0;
        target.offset = 200;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        // Many children in target - would inflate output if expanded
        for (int i = 0; i < 10; i++) {
            Node f;
            f.kind = NodeKind::UInt64;
            f.name = QString("f%1").arg(i);
            f.parentId = targetId;
            f.offset = i * 8;
            tree.addNode(f);
        }

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "heavy_ptr";
        ptr.parentId = rootId;
        ptr.offset = 0;
        ptr.refId = targetId;
        ptr.collapsed = true;  // COLLAPSED
        tree.addNode(ptr);

        // Non-null pointer value
        QByteArray data(300, '\0');
        uint64_t ptrVal = 100;
        memcpy(data.data(), &ptrVal, 8);
        BufferProvider prov(data);

        ComposeResult result = compose(tree, prov);

        // Count lines belonging to depth > 1 inside Root
        // (There should be NONE because the pointer is collapsed)
        int expandedLines = 0;
        for (const LineMeta& lm : result.meta) {
            // Lines at depth >= 2 would be inside the pointer expansion
            if (lm.depth >= 2 && lm.nodeIdx >= 0 &&
                tree.nodes[lm.nodeIdx].parentId == targetId)
                expandedLines++;
        }

        // Standalone Heavy rendering adds lines at depth 1,
        // but pointer expansion at depth >= 2 should be zero
        QCOMPARE(expandedLines, 0);
    }

    void testPointerWidthComputation() {
        // Type column must be wide enough for "LongStructName*"
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node target;
        target.kind = NodeKind::Struct;
        target.name = "VeryLongStructNameForTesting";
        target.structTypeName = "VeryLongStructNameForTesting";
        target.parentId = 0;
        target.offset = 200;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "lptr";
        ptr.parentId = rootId;
        ptr.offset = 0;
        ptr.refId = targetId;
        ptr.collapsed = true;
        tree.addNode(ptr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // The text must contain the FULL target name, not truncated
        QStringList lines = result.text.split('\n');
        bool foundFull = false;
        for (const QString& l : lines) {
            if (l.contains("VeryLongStructNameForTesting*")) {
                foundFull = true;
                break;
            }
        }
        QVERIFY2(foundFull,
                 "Type column should be wide enough for long pointer target names");

        // Layout type width should accommodate the long name
        // "VeryLongStructNameForTesting*" = 29 chars
        QVERIFY2(result.layout.typeW >= 29,
                 qPrintable(QString("typeW=%1, should be >= 29").arg(result.layout.typeW)));
    }

    // ═════════════════════════════════════════════════════════════
    // Class keyword + alignment tests
    // ═════════════════════════════════════════════════════════════

    void testClassKeywordJsonRoundTrip() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        root.classKeyword = "class";
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::Hex32;
        f.name = "x";
        f.parentId = rootId;
        f.offset = 0;
        tree.addNode(f);

        // Save and reload
        QJsonObject json = tree.toJson();
        NodeTree tree2 = NodeTree::fromJson(json);

        // Find the root struct in the reloaded tree
        bool found = false;
        for (const auto& n : tree2.nodes) {
            if (n.kind == NodeKind::Struct && n.name == "Root") {
                QCOMPARE(n.classKeyword, QString("class"));
                QCOMPARE(n.resolvedClassKeyword(), QString("class"));
                found = true;
                break;
            }
        }
        QVERIFY2(found, "Root struct should exist after JSON round-trip");
    }

    void testClassKeywordDefaultsToStruct() {
        NodeTree tree;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        // classKeyword left empty
        tree.addNode(root);

        QJsonObject json = tree.toJson();
        NodeTree tree2 = NodeTree::fromJson(json);

        for (const auto& n : tree2.nodes) {
            if (n.kind == NodeKind::Struct) {
                QVERIFY(n.classKeyword.isEmpty());
                QCOMPARE(n.resolvedClassKeyword(), QString("struct"));
                break;
            }
        }
    }

    void testCommandRowRootNameSpan() {
        // Name span should cover the class name in the merged command row
        QString text = "source\u25BE \u00B7 0x0 \u00B7 struct MyClass {";
        ColumnSpan nameSpan = commandRowRootNameSpan(text);
        QVERIFY(nameSpan.valid);

        QString nameText = text.mid(nameSpan.start, nameSpan.end - nameSpan.start);
        QVERIFY2(nameText.trimmed() == "MyClass",
                 qPrintable("Name span should be 'MyClass', got: '" + nameText.trimmed() + "'"));
    }

    void testTextIsNonEmpty() {
        // Verify composed text is actually generated (not empty)
        NodeTree tree;
        tree.baseAddress = 0x1000;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "TestStruct";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Mix of types including pointers and arrays
        Node f1;
        f1.kind = NodeKind::UInt64;
        f1.name = "id";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "next";
        ptr.parentId = rootId;
        ptr.offset = 8;
        tree.addNode(ptr);

        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "buf";
        arr.parentId = rootId;
        arr.offset = 16;
        arr.elementKind = NodeKind::Hex8;
        arr.arrayLen = 16;
        arr.collapsed = true;
        tree.addNode(arr);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        QVERIFY2(!result.text.isEmpty(), "Composed text must not be empty");
        QVERIFY2(result.meta.size() >= 5,
                 qPrintable(QString("Expected >= 5 lines, got %1").arg(result.meta.size())));

        // Every line should have text content
        QStringList lines = result.text.split('\n');
        QCOMPARE(lines.size(), result.meta.size());
        for (int i = 0; i < lines.size(); i++) {
            QVERIFY2(!lines[i].isEmpty(),
                     qPrintable(QString("Line %1 is empty").arg(i)));
        }
    }

};

QTEST_MAIN(TestCompose)
#include "test_compose.moc"
