#include <QtTest/QTest>
#include "core.h"

class TestCore : public QObject {
    Q_OBJECT
private slots:
    void testSizeForKind() {
        QCOMPARE(rcx::sizeForKind(rcx::NodeKind::Hex8),  1);
        QCOMPARE(rcx::sizeForKind(rcx::NodeKind::Hex16), 2);
        QCOMPARE(rcx::sizeForKind(rcx::NodeKind::Hex32), 4);
        QCOMPARE(rcx::sizeForKind(rcx::NodeKind::Hex64), 8);
        QCOMPARE(rcx::sizeForKind(rcx::NodeKind::Float), 4);
        QCOMPARE(rcx::sizeForKind(rcx::NodeKind::Double), 8);
        QCOMPARE(rcx::sizeForKind(rcx::NodeKind::Vec3),  12);
        QCOMPARE(rcx::sizeForKind(rcx::NodeKind::Mat4x4), 64);
        QCOMPARE(rcx::sizeForKind(rcx::NodeKind::Struct), 0);
    }

    void testLinesForKind() {
        QCOMPARE(rcx::linesForKind(rcx::NodeKind::Hex32), 1);
        QCOMPARE(rcx::linesForKind(rcx::NodeKind::Vec2),  1);
        QCOMPARE(rcx::linesForKind(rcx::NodeKind::Vec3),  1);
        QCOMPARE(rcx::linesForKind(rcx::NodeKind::Vec4),  1);
        QCOMPARE(rcx::linesForKind(rcx::NodeKind::Mat4x4), 4);
    }

    void testKindStringRoundTrip() {
        for (int i = 0; i <= static_cast<int>(rcx::NodeKind::Array); i++) {
            auto kind = static_cast<rcx::NodeKind>(i);
            QString s = rcx::kindToString(kind);
            QCOMPARE(rcx::kindFromString(s), kind);
        }
    }

    void testNodeTree_addAndChildren() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        QCOMPARE(ri, 0);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node child;
        child.kind = rcx::NodeKind::Hex32;
        child.name = "field";
        child.parentId = rootId;
        child.offset = 0;
        tree.addNode(child);

        auto children = tree.childrenOf(rootId);
        QCOMPARE(children.size(), 1);
        QCOMPARE(children[0], 1);

        auto roots = tree.childrenOf(0);
        QCOMPARE(roots.size(), 1);
        QCOMPARE(roots[0], 0);
    }

    void testNodeTree_depth() {
        rcx::NodeTree tree;
        rcx::Node a; a.kind = rcx::NodeKind::Struct; a.name = "A"; a.parentId = 0;
        int ai = tree.addNode(a);
        uint64_t aId = tree.nodes[ai].id;
        rcx::Node b; b.kind = rcx::NodeKind::Struct; b.name = "B"; b.parentId = aId;
        int bi = tree.addNode(b);
        uint64_t bId = tree.nodes[bi].id;
        rcx::Node c; c.kind = rcx::NodeKind::Hex8; c.name = "c"; c.parentId = bId;
        tree.addNode(c);

        QCOMPARE(tree.depthOf(0), 0);
        QCOMPARE(tree.depthOf(1), 1);
        QCOMPARE(tree.depthOf(2), 2);
    }

    void testNodeTree_computeOffset() {
        rcx::NodeTree tree;
        tree.baseAddress = 0x1000;
        rcx::Node root; root.kind = rcx::NodeKind::Struct; root.name = "R";
        root.parentId = 0; root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node f; f.kind = rcx::NodeKind::Hex32; f.name = "f";
        f.parentId = rootId; f.offset = 16;
        tree.addNode(f);

        QCOMPARE(tree.computeOffset(1), 16);
    }

    void testNodeTree_jsonRoundTrip() {
        rcx::NodeTree tree;
        tree.baseAddress = 0xDEAD;
        rcx::Node root; root.kind = rcx::NodeKind::Struct; root.name = "Test";
        root.parentId = 0; root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node child; child.kind = rcx::NodeKind::Float; child.name = "val";
        child.parentId = rootId; child.offset = 8;
        tree.addNode(child);

        QJsonObject json = tree.toJson();
        rcx::NodeTree tree2 = rcx::NodeTree::fromJson(json);

        QCOMPARE(tree2.baseAddress, (uint64_t)0xDEAD);
        QCOMPARE(tree2.nodes.size(), 2);
        QCOMPARE(tree2.nodes[0].name, QString("Test"));
        QCOMPARE(tree2.nodes[1].kind, rcx::NodeKind::Float);
        QCOMPARE(tree2.nodes[1].offset, 8);
    }

    void testBufferProvider() {
        QByteArray data(16, '\0');
        data[0] = 0x42;
        data[4] = 0x10;
        data[5] = 0x20;

        rcx::BufferProvider prov(data);
        QVERIFY(prov.isValid());
        QCOMPARE(prov.size(), 16);
        QCOMPARE(prov.readU8(0), (uint8_t)0x42);
        QCOMPARE(prov.readU16(4), (uint16_t)0x2010);
    }

    void testNullProvider() {
        rcx::NullProvider prov;
        QVERIFY(!prov.isValid());
        QVERIFY(!prov.isReadable(0, 1));
        QCOMPARE(prov.readU8(0), (uint8_t)0);
        QCOMPARE(prov.readU32(0), (uint32_t)0);
    }

    void testIsReadable() {
        QByteArray data(16, '\0');
        rcx::BufferProvider prov(data);
        QVERIFY(prov.isReadable(0, 4));
        QVERIFY(prov.isReadable(0, 16));
        QVERIFY(!prov.isReadable(0, 17));
        QVERIFY(!prov.isReadable(15, 2));
        QVERIFY(prov.isReadable(15, 1));
    }

    void testStableNodeIds() {
        rcx::NodeTree tree;
        rcx::Node a; a.kind = rcx::NodeKind::Struct; a.name = "A"; a.parentId = 0;
        int ai = tree.addNode(a);
        QCOMPARE(tree.nodes[ai].id, (uint64_t)1);

        rcx::Node b; b.kind = rcx::NodeKind::Hex32; b.name = "B"; b.parentId = tree.nodes[ai].id;
        int bi = tree.addNode(b);
        QCOMPARE(tree.nodes[bi].id, (uint64_t)2);

        QCOMPARE(tree.indexOfId(1), 0);
        QCOMPARE(tree.indexOfId(2), 1);
        QCOMPARE(tree.indexOfId(99), -1);
    }

    void testByteSizeDynamic() {
        rcx::Node n;
        n.kind = rcx::NodeKind::UTF8;
        n.strLen = 128;
        QCOMPARE(n.byteSize(), 128);

        n.kind = rcx::NodeKind::UTF16;
        n.strLen = 32;
        QCOMPARE(n.byteSize(), 64); // 32 * 2

        n.kind = rcx::NodeKind::Float;
        QCOMPARE(n.byteSize(), 4); // falls back to sizeForKind
    }

    void testSubtreeCycleSafe() {
        rcx::NodeTree tree;
        rcx::Node a; a.kind = rcx::NodeKind::Struct; a.name = "A"; a.parentId = 0;
        int ai = tree.addNode(a);
        uint64_t aId = tree.nodes[ai].id;

        // Create a child that points back to A's id as parent — not a cycle per se,
        // but test that subtree collection terminates
        rcx::Node b; b.kind = rcx::NodeKind::Hex8; b.name = "B"; b.parentId = aId;
        tree.addNode(b);

        // Should return both nodes without hanging
        auto sub = tree.subtreeIndices(aId);
        QCOMPARE(sub.size(), 2);
        QVERIFY(sub.contains(0));
        QVERIFY(sub.contains(1));
    }

    void testIsReadableOverflow() {
        QByteArray data(16, '\0');
        rcx::BufferProvider prov(data);
        // Normal cases
        QVERIFY(prov.isReadable(0, 16));
        QVERIFY(!prov.isReadable(0, 17));
        // Large address
        QVERIFY(!prov.isReadable(0xFFFFFFFFFFFFFFFFULL, 1));
        // Negative len
        QVERIFY(!prov.isReadable(0, -1));
        // Zero len is readable
        QVERIFY(prov.isReadable(0, 0));
        QVERIFY(prov.isReadable(16, 0));
    }

    void testAlignmentFor() {
        QCOMPARE(rcx::alignmentFor(rcx::NodeKind::Hex8),  1);
        QCOMPARE(rcx::alignmentFor(rcx::NodeKind::Hex16), 2);
        QCOMPARE(rcx::alignmentFor(rcx::NodeKind::Hex32), 4);
        QCOMPARE(rcx::alignmentFor(rcx::NodeKind::Hex64), 8);
        QCOMPARE(rcx::alignmentFor(rcx::NodeKind::Float), 4);
        QCOMPARE(rcx::alignmentFor(rcx::NodeKind::Double), 8);
        QCOMPARE(rcx::alignmentFor(rcx::NodeKind::Vec3),  4);
        QCOMPARE(rcx::alignmentFor(rcx::NodeKind::Mat4x4), 4);
        QCOMPARE(rcx::alignmentFor(rcx::NodeKind::UTF8),  1);
        QCOMPARE(rcx::alignmentFor(rcx::NodeKind::UTF16), 2);
        QCOMPARE(rcx::alignmentFor(rcx::NodeKind::Struct), 1);
    }

    void testDepthOfCycle() {
        rcx::NodeTree tree;
        // Create two nodes that reference each other as parents
        rcx::Node a; a.kind = rcx::NodeKind::Struct; a.name = "A"; a.parentId = 0;
        int ai = tree.addNode(a);
        uint64_t aId = tree.nodes[ai].id;

        rcx::Node b; b.kind = rcx::NodeKind::Struct; b.name = "B"; b.parentId = aId;
        int bi = tree.addNode(b);
        uint64_t bId = tree.nodes[bi].id;

        // Manually create a cycle: A's parent → B
        tree.nodes[ai].parentId = bId;
        tree.invalidateIdCache();

        // Should not hang — cycle detection terminates
        int d = tree.depthOf(ai);
        QVERIFY(d < 100);
    }

    void testComputeOffsetCycle() {
        rcx::NodeTree tree;
        rcx::Node a; a.kind = rcx::NodeKind::Struct; a.name = "A"; a.parentId = 0; a.offset = 10;
        int ai = tree.addNode(a);
        uint64_t aId = tree.nodes[ai].id;

        rcx::Node b; b.kind = rcx::NodeKind::Struct; b.name = "B"; b.parentId = aId; b.offset = 20;
        int bi = tree.addNode(b);
        uint64_t bId = tree.nodes[bi].id;

        // Create cycle: A → B → A
        tree.nodes[ai].parentId = bId;
        tree.invalidateIdCache();

        // Should not hang
        int off = tree.computeOffset(ai);
        Q_UNUSED(off);
        QVERIFY(true); // reaching here means no hang
    }

    void testProviderWrite() {
        QByteArray data(16, '\0');
        rcx::BufferProvider prov(data);
        QVERIFY(prov.isWritable());

        QByteArray patch;
        patch.append((char)0x42);
        patch.append((char)0x43);
        QVERIFY(prov.writeBytes(0, patch));
        QCOMPARE(prov.readU8(0), (uint8_t)0x42);
        QCOMPARE(prov.readU8(1), (uint8_t)0x43);

        // Write past end should fail
        QVERIFY(!prov.writeBytes(15, patch));

        // NullProvider is not writable
        rcx::NullProvider np;
        QVERIFY(!np.isWritable());
    }

    void testComputeOffsetLarge() {
        // Verify computeOffset returns int64_t that doesn't overflow
        rcx::NodeTree tree;
        rcx::Node root; root.kind = rcx::NodeKind::Struct; root.name = "R";
        root.parentId = 0; root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node child; child.kind = rcx::NodeKind::Hex8; child.name = "f";
        child.parentId = rootId; child.offset = 0x7FFFFFFF; // max int32
        tree.addNode(child);

        int64_t off = tree.computeOffset(1);
        QCOMPARE(off, (int64_t)0x7FFFFFFF);
    }

    void testKindMetaCompleteness() {
        // Every NodeKind enum value must have a KindMeta entry
        for (int i = 0; i <= static_cast<int>(rcx::NodeKind::Array); i++) {
            auto kind = static_cast<rcx::NodeKind>(i);
            const rcx::KindMeta* m = rcx::kindMeta(kind);
            QVERIFY2(m != nullptr,
                qPrintable(QString("Missing KindMeta for kind %1").arg(i)));
            QCOMPARE(m->kind, kind);
            QVERIFY(m->name != nullptr);
            QVERIFY(m->typeName != nullptr);
            QVERIFY(m->lines >= 1);
            QVERIFY(m->align >= 1);
        }
        // sizeForKind/linesForKind/alignmentFor must agree with table
        for (const auto& m : rcx::kKindMeta) {
            QCOMPARE(rcx::sizeForKind(m.kind), m.size);
            QCOMPARE(rcx::linesForKind(m.kind), m.lines);
            QCOMPARE(rcx::alignmentFor(m.kind), m.align);
        }
    }

    void testColumnSpan_field() {
        rcx::LineMeta lm;
        lm.lineKind = rcx::LineKind::Field;
        lm.depth = 1;
        lm.isContinuation = false;
        lm.nodeIdx = 0;

        // kFoldCol + depth*kTreeIndent
        int ind = rcx::kFoldCol + 1 * rcx::kTreeIndent;
        auto ts = rcx::typeSpanFor(lm);
        QVERIFY(ts.valid);
        QCOMPARE(ts.start, ind);
        QCOMPARE(ts.end, ind + 14);   // + kColType

        auto ns = rcx::nameSpanFor(lm);
        QVERIFY(ns.valid);
        QCOMPARE(ns.start, ind + 14 + 1); // + kColType + kSepWidth
        QCOMPARE(ns.end, ind + 14 + 1 + 22);   // + kColName

        auto vs = rcx::valueSpanFor(lm, 100);
        QVERIFY(vs.valid);
        QCOMPARE(vs.start, ind + 14 + 22 + 2); // + kColType + kColName + 2*kSepWidth
        QCOMPARE(vs.end, ind + 14 + 22 + 2 + rcx::kColValue);
    }

    void testColumnSpan_continuation() {
        rcx::LineMeta lm;
        lm.lineKind = rcx::LineKind::Continuation;
        lm.depth = 1;
        lm.isContinuation = true;
        lm.nodeIdx = 0;

        QVERIFY(!rcx::typeSpanFor(lm).valid);
        QVERIFY(!rcx::nameSpanFor(lm).valid);

        int ind2 = rcx::kFoldCol + 1 * rcx::kTreeIndent;
        auto vs = rcx::valueSpanFor(lm, 100);
        QVERIFY(vs.valid);
        QCOMPARE(vs.start, ind2 + 14 + 22 + 2);  // indent + kColType + kColName + 2*kSepWidth
        QCOMPARE(vs.end, ind2 + 14 + 22 + 2 + rcx::kColValue);
    }

    void testColumnSpan_headerFooter() {
        rcx::LineMeta lm;
        lm.lineKind = rcx::LineKind::Header;
        lm.depth = 0;
        lm.nodeIdx = 0;

        QVERIFY(!rcx::typeSpanFor(lm).valid);
        QVERIFY(!rcx::nameSpanFor(lm).valid);
        QVERIFY(!rcx::valueSpanFor(lm, 40).valid);

        lm.lineKind = rcx::LineKind::Footer;
        QVERIFY(!rcx::typeSpanFor(lm).valid);
        QVERIFY(!rcx::nameSpanFor(lm).valid);
        QVERIFY(!rcx::valueSpanFor(lm, 40).valid);
    }

    void testColumnSpan_depth0() {
        rcx::LineMeta lm;
        lm.lineKind = rcx::LineKind::Field;
        lm.depth = 0;
        lm.isContinuation = false;
        lm.nodeIdx = 0;

        // kFoldCol (3) + depth*3(0) = 3
        auto ts = rcx::typeSpanFor(lm);
        QVERIFY(ts.valid);
        QCOMPARE(ts.start, 3);
        QCOMPARE(ts.end, 17);   // 3 + 14 (kColType)

        auto ns = rcx::nameSpanFor(lm);
        QVERIFY(ns.valid);
        QCOMPARE(ns.start, 18); // 3 + 14 + 1 (kSepWidth)
        QCOMPARE(ns.end, 40);   // 18 + 22 (kColName)

        auto vs = rcx::valueSpanFor(lm, 100);
        QVERIFY(vs.valid);
        QCOMPARE(vs.start, 41); // 18 + 22 + 1 (kSepWidth)
        QCOMPARE(vs.end, 41 + rcx::kColValue);   // start + kColValue
    }

    void testNodeIdJsonRoundTrip() {
        rcx::NodeTree tree;
        rcx::Node n; n.kind = rcx::NodeKind::Float; n.name = "x"; n.parentId = 0;
        tree.addNode(n);
        tree.addNode(n);

        QJsonObject json = tree.toJson();
        rcx::NodeTree t2 = rcx::NodeTree::fromJson(json);
        QCOMPARE(t2.nodes[0].id, tree.nodes[0].id);
        QCOMPARE(t2.nodes[1].id, tree.nodes[1].id);
        QVERIFY(t2.m_nextId >= 3);
    }

    void testStructSpan() {
        using namespace rcx;
        NodeTree tree;
        tree.baseAddress = 0;

        // Struct with UInt32 (offset 0, 4 bytes) + UInt64 (offset 4, 8 bytes)
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f1;
        f1.kind = NodeKind::UInt32;
        f1.name = "a";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        Node f2;
        f2.kind = NodeKind::UInt64;
        f2.name = "b";
        f2.parentId = rootId;
        f2.offset = 4;
        tree.addNode(f2);

        // Span = max(0+4, 4+8) = 12
        QCOMPARE(tree.structSpan(rootId), 12);

        // Nested struct: inner at offset 0 with a UInt64 at offset 0 (size 8)
        NodeTree tree2;
        Node outer;
        outer.kind = NodeKind::Struct;
        outer.name = "Outer";
        outer.parentId = 0;
        int oi = tree2.addNode(outer);
        uint64_t outerId = tree2.nodes[oi].id;

        Node inner;
        inner.kind = NodeKind::Struct;
        inner.name = "Inner";
        inner.parentId = outerId;
        inner.offset = 0;
        int ii = tree2.addNode(inner);
        uint64_t innerId = tree2.nodes[ii].id;

        Node leaf;
        leaf.kind = NodeKind::UInt64;
        leaf.name = "x";
        leaf.parentId = innerId;
        leaf.offset = 0;
        tree2.addNode(leaf);

        // Inner span = 8, outer span = max(0+8) = 8
        QCOMPARE(tree2.structSpan(innerId), 8);
        QCOMPARE(tree2.structSpan(outerId), 8);

        // Empty struct = 0
        NodeTree tree3;
        Node empty;
        empty.kind = NodeKind::Struct;
        empty.name = "Empty";
        empty.parentId = 0;
        int ei = tree3.addNode(empty);
        QCOMPARE(tree3.structSpan(tree3.nodes[ei].id), 0);

        // Primitive array (no children) should return its declared size
        NodeTree tree4;
        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "data";
        arr.parentId = 0;
        arr.arrayLen = 16;
        arr.elementKind = NodeKind::UInt32;  // 16 * 4 = 64 bytes
        int ai = tree4.addNode(arr);
        QCOMPARE(tree4.structSpan(tree4.nodes[ai].id), 64);

        // Struct containing primitive array - span includes array size
        NodeTree tree5;
        Node container;
        container.kind = NodeKind::Struct;
        container.name = "Container";
        container.parentId = 0;
        int ci = tree5.addNode(container);
        uint64_t containerId = tree5.nodes[ci].id;

        Node arr2;
        arr2.kind = NodeKind::Array;
        arr2.name = "items";
        arr2.parentId = containerId;
        arr2.offset = 8;
        arr2.arrayLen = 10;
        arr2.elementKind = NodeKind::UInt64;  // 10 * 8 = 80 bytes
        tree5.addNode(arr2);

        // Container span = array offset (8) + array size (80) = 88
        QCOMPARE(tree5.structSpan(containerId), 88);
    }
    void testNormalizePreferAncestors() {
        using namespace rcx;
        NodeTree tree;
        // Root -> A -> leaf
        Node root; root.kind = NodeKind::Struct; root.name = "R"; root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node a; a.kind = NodeKind::Struct; a.name = "A"; a.parentId = rootId;
        int ai = tree.addNode(a);
        uint64_t aId = tree.nodes[ai].id;

        Node leaf; leaf.kind = NodeKind::Hex8; leaf.name = "x"; leaf.parentId = aId;
        int li = tree.addNode(leaf);
        uint64_t leafId = tree.nodes[li].id;

        // Select root + leaf: leaf should be pruned (root is ancestor)
        QSet<uint64_t> sel = {rootId, leafId};
        QSet<uint64_t> norm = tree.normalizePreferAncestors(sel);
        QCOMPARE(norm.size(), 1);
        QVERIFY(norm.contains(rootId));

        // Select A + leaf: leaf pruned (A is ancestor)
        sel = {aId, leafId};
        norm = tree.normalizePreferAncestors(sel);
        QCOMPARE(norm.size(), 1);
        QVERIFY(norm.contains(aId));

        // Select root + A: A pruned (root is ancestor)
        sel = {rootId, aId};
        norm = tree.normalizePreferAncestors(sel);
        QCOMPARE(norm.size(), 1);
        QVERIFY(norm.contains(rootId));

        // Select only leaf: nothing pruned
        sel = {leafId};
        norm = tree.normalizePreferAncestors(sel);
        QCOMPARE(norm.size(), 1);
        QVERIFY(norm.contains(leafId));
    }

    void testNormalizePreferDescendants() {
        using namespace rcx;
        NodeTree tree;
        Node root; root.kind = NodeKind::Struct; root.name = "R"; root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node a; a.kind = NodeKind::UInt32; a.name = "a"; a.parentId = rootId;
        int ai = tree.addNode(a);
        uint64_t aId = tree.nodes[ai].id;

        Node b; b.kind = NodeKind::UInt32; b.name = "b"; b.parentId = rootId; b.offset = 4;
        int bi = tree.addNode(b);
        uint64_t bId = tree.nodes[bi].id;

        // Select root + a + b: root dropped (has selected descendants)
        QSet<uint64_t> sel = {rootId, aId, bId};
        QSet<uint64_t> norm = tree.normalizePreferDescendants(sel);
        QCOMPARE(norm.size(), 2);
        QVERIFY(norm.contains(aId));
        QVERIFY(norm.contains(bId));
        QVERIFY(!norm.contains(rootId));

        // Select root + a: root dropped, a kept
        sel = {rootId, aId};
        norm = tree.normalizePreferDescendants(sel);
        QCOMPARE(norm.size(), 1);
        QVERIFY(norm.contains(aId));

        // Select only root: nothing dropped (no descendants selected)
        sel = {rootId};
        norm = tree.normalizePreferDescendants(sel);
        QCOMPARE(norm.size(), 1);
        QVERIFY(norm.contains(rootId));
    }

    // ── ValueHistory tests ──

    void testValueHistory_empty() {
        rcx::ValueHistory h;
        QCOMPARE(h.heatLevel(), 0);
        QCOMPARE(h.uniqueCount(), 0);
        QCOMPARE(h.last(), QString());
    }

    void testValueHistory_singleValue() {
        rcx::ValueHistory h;
        h.record("42");
        QCOMPARE(h.heatLevel(), 0);  // only 1 unique → static
        QCOMPARE(h.uniqueCount(), 1);
        QCOMPARE(h.last(), QString("42"));
    }

    void testValueHistory_duplicateIgnored() {
        rcx::ValueHistory h;
        h.record("42");
        h.record("42");
        h.record("42");
        QCOMPARE(h.count, 1);
        QCOMPARE(h.heatLevel(), 0);
    }

    void testValueHistory_heatLevels() {
        rcx::ValueHistory h;
        h.record("a");
        QCOMPARE(h.heatLevel(), 0);  // 1 unique

        h.record("b");
        QCOMPARE(h.heatLevel(), 1);  // 2 unique → cold

        h.record("c");
        QCOMPARE(h.heatLevel(), 2);  // 3 unique → warm

        h.record("d");
        QCOMPARE(h.heatLevel(), 2);  // 4 unique → warm

        h.record("e");
        QCOMPARE(h.heatLevel(), 3);  // 5 unique → hot
    }

    void testValueHistory_ringWrap() {
        rcx::ValueHistory h;
        // Fill beyond capacity
        for (int i = 0; i < 15; i++)
            h.record(QString::number(i));

        QCOMPARE(h.count, 15);
        QCOMPARE(h.uniqueCount(), 10);  // capped at kCapacity
        QCOMPARE(h.heatLevel(), 3);     // hot
        QCOMPARE(h.last(), QString("14"));

        // Verify oldest values were pushed out, newest 10 remain
        QStringList collected;
        h.forEach([&](const QString& v) { collected.append(v); });
        QCOMPARE(collected.size(), 10);
        QCOMPARE(collected.first(), QString("5"));   // oldest surviving
        QCOMPARE(collected.last(), QString("14"));    // newest
    }

    void testValueHistory_forEach() {
        rcx::ValueHistory h;
        h.record("x");
        h.record("y");
        h.record("z");

        QStringList items;
        h.forEach([&](const QString& v) { items.append(v); });
        QCOMPARE(items.size(), 3);
        QCOMPARE(items[0], QString("x"));
        QCOMPARE(items[1], QString("y"));
        QCOMPARE(items[2], QString("z"));
    }

    void testValueHistory_oscillation() {
        // Values that oscillate (A → B → A → B) should still count each unique transition
        rcx::ValueHistory h;
        h.record("A");
        h.record("B");
        h.record("A");
        h.record("B");
        QCOMPARE(h.count, 4);       // 4 transitions
        QCOMPARE(h.heatLevel(), 2); // warm (count=4 → 3-4 range)
    }

    // ── Test: comment field JSON round-trip ──
    void testCommentJsonRoundTrip() {
        rcx::Node n;
        n.id = 42;
        n.kind = rcx::NodeKind::Int32;
        n.name = QStringLiteral("health");
        n.comment = QStringLiteral("player HP");

        QJsonObject json = n.toJson();
        QCOMPARE(json["comment"].toString(), QStringLiteral("player HP"));

        rcx::Node loaded = rcx::Node::fromJson(json);
        QCOMPARE(loaded.comment, QStringLiteral("player HP"));
        QCOMPARE(loaded.name, QStringLiteral("health"));
        QCOMPARE(loaded.kind, rcx::NodeKind::Int32);
    }

    // ── Test: empty comment not serialized ──
    void testEmptyCommentNotSerialized() {
        rcx::Node n;
        n.id = 1;
        n.kind = rcx::NodeKind::Hex64;
        n.comment = QString();

        QJsonObject json = n.toJson();
        QVERIFY(!json.contains("comment"));

        rcx::Node loaded = rcx::Node::fromJson(json);
        QVERIFY(loaded.comment.isEmpty());
    }

    // ── Test: NodeTree save/load round-trip preserves comments ──
    void testCommentTreeRoundTrip() {
        rcx::NodeTree tree;
        tree.baseAddress = 0x400000;

        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = QStringLiteral("Test");
        root.structTypeName = QStringLiteral("TestStruct");
        int ri = tree.addNode(root);

        rcx::Node field;
        field.kind = rcx::NodeKind::Int32;
        field.name = QStringLiteral("score");
        field.parentId = tree.nodes[ri].id;
        field.offset = 0;
        field.comment = QStringLiteral("game score value");
        tree.addNode(field);

        rcx::Node field2;
        field2.kind = rcx::NodeKind::Float;
        field2.name = QStringLiteral("speed");
        field2.parentId = tree.nodes[ri].id;
        field2.offset = 4;
        field2.comment = QString();  // no comment
        tree.addNode(field2);

        // Serialize and deserialize
        QJsonObject json = tree.toJson();
        rcx::NodeTree loaded = rcx::NodeTree::fromJson(json);

        QCOMPARE(loaded.nodes.size(), tree.nodes.size());
        // Find the 'score' node and check comment
        bool foundScore = false;
        bool foundSpeed = false;
        for (const auto& n : loaded.nodes) {
            if (n.name == QStringLiteral("score")) {
                QCOMPARE(n.comment, QStringLiteral("game score value"));
                foundScore = true;
            }
            if (n.name == QStringLiteral("speed")) {
                QVERIFY(n.comment.isEmpty());
                foundSpeed = true;
            }
        }
        QVERIFY(foundScore);
        QVERIFY(foundSpeed);
    }
    // ── New helpers tests ──

    void testHex128Size() {
        QCOMPARE(rcx::sizeForKind(rcx::NodeKind::Hex128), 16);
    }

    void testKindMetaO1() {
        // Verify O(1) lookup matches every entry
        for (const auto& m : rcx::kKindMeta) {
            const auto* result = rcx::kindMeta(m.kind);
            QVERIFY(result != nullptr);
            QCOMPARE(result->kind, m.kind);
            QCOMPARE(result->size, m.size);
        }
    }

    void testIsPointerKind() {
        QVERIFY(rcx::isPointerKind(rcx::NodeKind::Pointer32));
        QVERIFY(rcx::isPointerKind(rcx::NodeKind::Pointer64));
        QVERIFY(!rcx::isPointerKind(rcx::NodeKind::FuncPtr64));
        QVERIFY(!rcx::isPointerKind(rcx::NodeKind::Hex64));
    }

    void testIsContainerKind() {
        QVERIFY(rcx::isContainerKind(rcx::NodeKind::Struct));
        QVERIFY(rcx::isContainerKind(rcx::NodeKind::Array));
        QVERIFY(!rcx::isContainerKind(rcx::NodeKind::Hex64));
        QVERIFY(!rcx::isContainerKind(rcx::NodeKind::Pointer64));
    }

    void testIsStringKind() {
        QVERIFY(rcx::isStringKind(rcx::NodeKind::UTF8));
        QVERIFY(rcx::isStringKind(rcx::NodeKind::UTF16));
        QVERIFY(!rcx::isStringKind(rcx::NodeKind::Hex8));
    }

    void testNodeIsUnionBitfieldEnum() {
        rcx::Node n;
        n.kind = rcx::NodeKind::Struct;
        n.classKeyword = QStringLiteral("union");
        QVERIFY(n.isUnion());
        QVERIFY(!n.isBitfield());
        QVERIFY(!n.isEnum());

        n.classKeyword = QStringLiteral("bitfield");
        QVERIFY(n.isBitfield());

        n.classKeyword = QStringLiteral("enum");
        QVERIFY(n.isEnum());

        n.classKeyword.clear();  // defaults to "struct"
        QVERIFY(!n.isUnion());
        QVERIFY(!n.isEnum());
    }

    void testKindFromTypeNameRoundTrip() {
        for (const auto& m : rcx::kKindMeta) {
            bool ok = false;
            rcx::NodeKind result = rcx::kindFromTypeName(QString::fromLatin1(m.typeName), &ok);
            QVERIFY(ok);
            QCOMPARE(result, m.kind);
        }
    }

    void testValueHistoryDedup() {
        rcx::ValueHistory vh;
        vh.record(QStringLiteral("42"));
        vh.record(QStringLiteral("42"));  // same value, should not increment
        QCOMPARE(vh.uniqueCount(), 1);
        QCOMPARE(vh.last(), QStringLiteral("42"));
    }

    void testValueHistoryRingOverflow() {
        rcx::ValueHistory vh;
        for (int i = 0; i < 15; i++)
            vh.record(QString::number(i));
        QCOMPARE(vh.uniqueCount(), rcx::ValueHistory::kCapacity);
        QCOMPARE(vh.last(), QStringLiteral("14"));
    }

    void testStructSpanCycleDetection() {
        rcx::NodeTree tree;
        rcx::Node a; a.kind = rcx::NodeKind::Struct; a.name = "A";
        int ai = tree.addNode(a);
        rcx::Node b; b.kind = rcx::NodeKind::Struct; b.name = "B";
        b.parentId = tree.nodes[ai].id;
        b.refId = tree.nodes[ai].id;  // cycle: B references A
        tree.addNode(b);
        // Should not infinite loop
        int span = tree.structSpan(tree.nodes[ai].id);
        QVERIFY(span >= 0);
    }

    void testDepthOfOrphan() {
        rcx::NodeTree tree;
        rcx::Node n;
        n.parentId = 99999;  // nonexistent parent
        int idx = tree.addNode(n);
        QCOMPARE(tree.depthOf(idx), 0);
    }

    void testComputeOffsetNested() {
        rcx::NodeTree tree;
        tree.baseAddress = 0;
        rcx::Node root; root.kind = rcx::NodeKind::Struct; root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node child; child.kind = rcx::NodeKind::Struct;
        child.parentId = rootId; child.offset = 16;
        int ci = tree.addNode(child);
        uint64_t childId = tree.nodes[ci].id;

        rcx::Node leaf; leaf.kind = rcx::NodeKind::UInt32;
        leaf.parentId = childId; leaf.offset = 8;
        int li = tree.addNode(leaf);

        QCOMPARE(tree.computeOffset(li), (int64_t)(0 + 16 + 8));
    }

    void testNormalizePreferAncestorsBasic() {
        rcx::NodeTree tree;
        rcx::Node root; root.kind = rcx::NodeKind::Struct;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node child; child.kind = rcx::NodeKind::UInt32;
        child.parentId = rootId;
        int ci = tree.addNode(child);
        uint64_t childId = tree.nodes[ci].id;

        QSet<uint64_t> ids = {rootId, childId};
        auto result = tree.normalizePreferAncestors(ids);
        QCOMPARE(result.size(), 1);
        QVERIFY(result.contains(rootId));
    }

    void testNormalizePreferDescendantsBasic() {
        rcx::NodeTree tree;
        rcx::Node root; root.kind = rcx::NodeKind::Struct;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node child; child.kind = rcx::NodeKind::UInt32;
        child.parentId = rootId;
        int ci = tree.addNode(child);
        uint64_t childId = tree.nodes[ci].id;

        QSet<uint64_t> ids = {rootId, childId};
        auto result = tree.normalizePreferDescendants(ids);
        QCOMPARE(result.size(), 1);
        QVERIFY(result.contains(childId));
    }

    void testNodeJsonRoundTrip() {
        // Build a node with all optional fields populated
        rcx::Node n;
        n.id = 42;
        n.kind = rcx::NodeKind::Struct;
        n.name = QStringLiteral("TestNode");
        n.structTypeName = QStringLiteral("TestType");
        n.classKeyword = QStringLiteral("class");
        n.parentId = 10;
        n.offset = 64;
        n.isRelative = true;
        n.arrayLen = 5;
        n.strLen = 128;
        n.refId = 99;
        n.elementKind = rcx::NodeKind::Float;
        n.ptrDepth = 2;
        n.comment = QStringLiteral("test comment");
        n.enumMembers = {{QStringLiteral("A"), 0}, {QStringLiteral("B"), 1}};
        n.bitfieldMembers = {{QStringLiteral("bit0"), 0, 1}, {QStringLiteral("bits"), 1, 3}};

        QJsonObject json = n.toJson();
        rcx::Node n2 = rcx::Node::fromJson(json);

        QCOMPARE(n2.id, n.id);
        QCOMPARE(n2.kind, n.kind);
        QCOMPARE(n2.name, n.name);
        QCOMPARE(n2.structTypeName, n.structTypeName);
        QCOMPARE(n2.classKeyword, n.classKeyword);
        QCOMPARE(n2.parentId, n.parentId);
        QCOMPARE(n2.offset, n.offset);
        QCOMPARE(n2.isRelative, n.isRelative);
        QCOMPARE(n2.arrayLen, n.arrayLen);
        QCOMPARE(n2.strLen, n.strLen);
        QCOMPARE(n2.refId, n.refId);
        QCOMPARE(n2.elementKind, n.elementKind);
        QCOMPARE(n2.ptrDepth, n.ptrDepth);
        QCOMPARE(n2.comment, n.comment);
        QCOMPARE(n2.enumMembers.size(), n.enumMembers.size());
        QCOMPARE(n2.enumMembers[0].first, QStringLiteral("A"));
        QCOMPARE(n2.enumMembers[1].second, (int64_t)1);
        QCOMPARE(n2.bitfieldMembers.size(), n.bitfieldMembers.size());
        QCOMPARE(n2.bitfieldMembers[0].name, QStringLiteral("bit0"));
        QCOMPARE(n2.bitfieldMembers[1].bitWidth, (uint8_t)3);
    }

    void testNodeTreeJsonRoundTrip() {
        rcx::NodeTree tree;
        tree.baseAddress = 0x7FF600000000ULL;
        tree.baseAddressFormula = QStringLiteral("<app.exe> + 0x100");
        tree.pointerSize = 4;

        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = QStringLiteral("root");
        root.structTypeName = QStringLiteral("Root");
        tree.addNode(root);
        uint64_t rootId = tree.nodes[0].id;

        rcx::Node child;
        child.kind = rcx::NodeKind::Int32;
        child.name = QStringLiteral("x");
        child.parentId = rootId;
        child.offset = 4;
        tree.addNode(child);

        QJsonObject json = tree.toJson();
        rcx::NodeTree tree2 = rcx::NodeTree::fromJson(json);

        QCOMPARE(tree2.baseAddress, tree.baseAddress);
        QCOMPARE(tree2.baseAddressFormula, tree.baseAddressFormula);
        QCOMPARE(tree2.pointerSize, tree.pointerSize);
        QCOMPARE(tree2.nodes.size(), tree.nodes.size());
        QCOMPARE(tree2.nodes[1].name, QStringLiteral("x"));
        QCOMPARE(tree2.nodes[1].offset, 4);
    }

    void testStructSpanLeafShortCircuit() {
        // structSpan on a non-container node should return byteSize directly
        rcx::NodeTree tree;
        rcx::Node n;
        n.kind = rcx::NodeKind::UInt64;
        n.offset = 0;
        int ni = tree.addNode(n);
        QCOMPARE(tree.structSpan(tree.nodes[ni].id), 8);
    }

    void testSubtreeIndices() {
        rcx::NodeTree tree;
        rcx::Node root; root.kind = rcx::NodeKind::Struct;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;
        rcx::Node c1; c1.kind = rcx::NodeKind::Int32; c1.parentId = rootId;
        tree.addNode(c1);
        rcx::Node c2; c2.kind = rcx::NodeKind::Float; c2.parentId = rootId;
        tree.addNode(c2);
        auto sub = tree.subtreeIndices(rootId);
        QCOMPARE(sub.size(), 3);  // root + 2 children
    }

    void testAddNodeAutoId() {
        rcx::NodeTree tree;
        rcx::Node n1; n1.id = 0;  // 0 = auto-assign
        int i1 = tree.addNode(n1);
        rcx::Node n2; n2.id = 0;
        int i2 = tree.addNode(n2);
        QVERIFY(tree.nodes[i1].id != tree.nodes[i2].id);
        QVERIFY(tree.nodes[i1].id > 0);
        QVERIFY(tree.nodes[i2].id > 0);
    }

    void testReserveIdMonotonic() {
        rcx::NodeTree tree;
        uint64_t a = tree.reserveId();
        uint64_t b = tree.reserveId();
        uint64_t c = tree.reserveId();
        QVERIFY(b > a);
        QVERIFY(c > b);
    }

    void testChildrenOfEmpty() {
        rcx::NodeTree tree;
        auto kids = tree.childrenOf(999);
        QCOMPARE(kids.size(), 0);
    }

    void testIndexOfIdNotFound() {
        rcx::NodeTree tree;
        rcx::Node n; tree.addNode(n);
        QCOMPARE(tree.indexOfId(99999), -1);
    }

    void testNodeByteSizeUtf8() {
        rcx::Node n;
        n.kind = rcx::NodeKind::UTF8;
        n.strLen = 32;
        QCOMPARE(n.byteSize(), 32);
    }

    void testNodeByteSizeUtf16() {
        rcx::Node n;
        n.kind = rcx::NodeKind::UTF16;
        n.strLen = 16;
        QCOMPARE(n.byteSize(), 32);  // 16 * 2
    }

    void testNodeByteSizeArray() {
        rcx::Node n;
        n.kind = rcx::NodeKind::Array;
        n.elementKind = rcx::NodeKind::Int32;
        n.arrayLen = 10;
        QCOMPARE(n.byteSize(), 40);  // 10 * 4
    }

    void testNodeByteSizeBitfield() {
        rcx::Node n;
        n.kind = rcx::NodeKind::Struct;
        n.classKeyword = QStringLiteral("bitfield");
        n.elementKind = rcx::NodeKind::Hex32;
        QCOMPARE(n.byteSize(), 4);  // container size
    }

    void testHelperFunctions() {
        // Batch test all the inline helpers
        QVERIFY(rcx::isHexNode(rcx::NodeKind::Hex8));
        QVERIFY(rcx::isHexNode(rcx::NodeKind::Hex128));
        QVERIFY(!rcx::isHexNode(rcx::NodeKind::Int32));
        QVERIFY(rcx::isVectorKind(rcx::NodeKind::Vec2));
        QVERIFY(rcx::isVectorKind(rcx::NodeKind::Vec4));
        QVERIFY(!rcx::isVectorKind(rcx::NodeKind::Float));
        QVERIFY(rcx::isMatrixKind(rcx::NodeKind::Mat4x4));
        QVERIFY(!rcx::isMatrixKind(rcx::NodeKind::Vec4));
        QVERIFY(rcx::isFuncPtr(rcx::NodeKind::FuncPtr32));
        QVERIFY(rcx::isFuncPtr(rcx::NodeKind::FuncPtr64));
        QVERIFY(!rcx::isFuncPtr(rcx::NodeKind::Pointer64));
    }

    // Regression: the unsaved-changes dialog used to only show the
    // first root class of a multi-class document (project_new() reuses
    // the active doc and adds a new root struct — so UnnamedClass0 and
    // UnnamedClass1 end up in the same tree). rootClassNames must
    // enumerate ALL root structs so the dialog can list them.
    void testRootClassNames_multiClass() {
        rcx::NodeTree tree;
        rcx::Node a; a.kind = rcx::NodeKind::Struct;
        a.name = "UnnamedClass0"; a.parentId = 0;
        tree.addNode(a);
        rcx::Node b; b.kind = rcx::NodeKind::Struct;
        b.name = "UnnamedClass1"; b.parentId = 0;
        tree.addNode(b);
        // A nested struct under UnnamedClass0 — should NOT appear in
        // the root list.
        rcx::Node nested; nested.kind = rcx::NodeKind::Struct;
        nested.name = "Nested"; nested.parentId = tree.nodes[0].id;
        tree.addNode(nested);

        auto names = rcx::rootClassNames(tree);
        QCOMPARE(names.size(), 2);
        QCOMPARE(names[0], QString("UnnamedClass0"));
        QCOMPARE(names[1], QString("UnnamedClass1"));
    }

    void testRootClassNames_prefersStructTypeName() {
        rcx::NodeTree tree;
        rcx::Node a; a.kind = rcx::NodeKind::Struct;
        a.name = "raw_name";
        a.structTypeName = "MyStruct";  // typed C++ name wins
        a.parentId = 0;
        tree.addNode(a);

        auto names = rcx::rootClassNames(tree);
        QCOMPARE(names.size(), 1);
        QCOMPARE(names[0], QString("MyStruct"));
    }

    void testRootClassNames_emptyTreeYieldsUntitled() {
        rcx::NodeTree tree;
        auto names = rcx::rootClassNames(tree);
        QCOMPARE(names.size(), 1);
        QCOMPARE(names[0], QString("Untitled"));
    }

    void testRootClassNames_dedupesByName() {
        rcx::NodeTree tree;
        rcx::Node a; a.kind = rcx::NodeKind::Struct;
        a.name = "Foo"; a.parentId = 0;
        tree.addNode(a);
        rcx::Node b; b.kind = rcx::NodeKind::Struct;
        b.name = "Foo"; b.parentId = 0;  // duplicate name
        tree.addNode(b);

        auto names = rcx::rootClassNames(tree);
        QCOMPARE(names.size(), 1);
    }

    void testRootClassNames_skipsNonStructRoots() {
        rcx::NodeTree tree;
        // A bare-hex root (not a Struct) shouldn't show up.
        rcx::Node lone; lone.kind = rcx::NodeKind::Hex64;
        lone.name = "stray"; lone.parentId = 0;
        tree.addNode(lone);
        rcx::Node s; s.kind = rcx::NodeKind::Struct;
        s.name = "Real"; s.parentId = 0;
        tree.addNode(s);

        auto names = rcx::rootClassNames(tree);
        QCOMPARE(names.size(), 1);
        QCOMPARE(names[0], QString("Real"));
    }

    // ── findOverlaps ──────────────────────────────────────────────
    // Helper: build a struct with `kinds` at `offsets`. Returns the
    // child node ids in declaration order so tests can assert which
    // pairs the overlap detector finds.
    static QVector<uint64_t> buildStructWithFields(
        rcx::NodeTree& tree, const QVector<QPair<int, rcx::NodeKind>>& fields)
    {
        rcx::Node r; r.kind = rcx::NodeKind::Struct;
        r.name = "S"; r.parentId = 0;
        int ri = tree.addNode(r);
        uint64_t rootId = tree.nodes[ri].id;
        QVector<uint64_t> ids;
        int i = 0;
        for (const auto& [off, k] : fields) {
            rcx::Node n;
            n.kind = k;
            n.name = QStringLiteral("f%1").arg(i++);
            n.parentId = rootId;
            n.offset = off;
            int ni = tree.addNode(n);
            ids.append(tree.nodes[ni].id);
        }
        return ids;
    }

    void testFindOverlaps_cleanLayout() {
        rcx::NodeTree tree;
        buildStructWithFields(tree, {
            {0,  rcx::NodeKind::UInt32},  // [0, 4)
            {4,  rcx::NodeKind::UInt32},  // [4, 8)
            {8,  rcx::NodeKind::UInt64},  // [8, 16)
        });
        QVERIFY(tree.findOverlaps().isEmpty());
    }

    void testFindOverlaps_twoFieldsAtSameOffset() {
        rcx::NodeTree tree;
        auto ids = buildStructWithFields(tree, {
            {0, rcx::NodeKind::UInt32},   // [0, 4)
            {0, rcx::NodeKind::UInt32},   // [0, 4) — same range
        });
        auto overlaps = tree.findOverlaps();
        QCOMPARE(overlaps.size(), 1);
        QCOMPARE(overlaps[0].aId, ids[0]);
        QCOMPARE(overlaps[0].bId, ids[1]);
    }

    void testFindOverlaps_partialOverlap() {
        rcx::NodeTree tree;
        auto ids = buildStructWithFields(tree, {
            {0, rcx::NodeKind::UInt32},   // [0, 4)
            {2, rcx::NodeKind::UInt32},   // [2, 6) — straddles boundary
        });
        auto overlaps = tree.findOverlaps();
        QCOMPARE(overlaps.size(), 1);
        QCOMPARE(overlaps[0].aId, ids[0]);
        QCOMPARE(overlaps[0].bId, ids[1]);
    }

    void testFindOverlaps_oneFieldEntirelyContained() {
        rcx::NodeTree tree;
        auto ids = buildStructWithFields(tree, {
            {0, rcx::NodeKind::UInt64},   // [0, 8) — big one
            {2, rcx::NodeKind::UInt8},    // [2, 3) — inside the big one
        });
        auto overlaps = tree.findOverlaps();
        QCOMPARE(overlaps.size(), 1);
        QCOMPARE(overlaps[0].aId, ids[0]);
        QCOMPARE(overlaps[0].bId, ids[1]);
    }

    void testFindOverlaps_adjacentFieldsAreClean() {
        rcx::NodeTree tree;
        buildStructWithFields(tree, {
            {0, rcx::NodeKind::UInt8},    // [0, 1)
            {1, rcx::NodeKind::UInt8},    // [1, 2) — touches but no overlap
        });
        QVERIFY(tree.findOverlaps().isEmpty());
    }

    void testFindOverlaps_unionChildrenIgnored() {
        // Union deliberately overlaps its children; findOverlaps must
        // recognise this and report no issues.
        rcx::NodeTree tree;
        rcx::Node u; u.kind = rcx::NodeKind::Struct;
        u.classKeyword = "union";
        u.name = "U"; u.parentId = 0;
        int ui = tree.addNode(u);
        uint64_t uid = tree.nodes[ui].id;
        for (int i = 0; i < 3; ++i) {
            rcx::Node n; n.kind = rcx::NodeKind::UInt32;
            n.name = QStringLiteral("v%1").arg(i);
            n.parentId = uid; n.offset = 0;
            tree.addNode(n);
        }
        QVERIFY(tree.findOverlaps().isEmpty());
    }

    void testFindOverlaps_multiplePairsOneLongField() {
        // One big field at [0,16) overlaps three smaller ones at 0, 4, 8.
        // Detector must report all three pairs (UInt64 vs each smaller).
        rcx::NodeTree tree;
        auto ids = buildStructWithFields(tree, {
            {0, rcx::NodeKind::UInt128},  // [0, 16) — covers the others
            {0, rcx::NodeKind::UInt32},   // [0, 4)
            {4, rcx::NodeKind::UInt32},   // [4, 8)
            {8, rcx::NodeKind::UInt32},   // [8, 12)
        });
        auto overlaps = tree.findOverlaps();
        QCOMPARE(overlaps.size(), 3);
        // First field (UInt128) is at offset 0 → sorted first, paired
        // against each of the others.
        for (const auto& p : overlaps)
            QCOMPARE(p.aId, ids[0]);
    }

    void testFindOverlaps_rootLevelStructsExcluded() {
        // Root-level structs (parentId == 0) are independent classes,
        // not siblings. Multiple root structs at offset 0 must NOT be
        // reported as overlapping each other.
        rcx::NodeTree tree;
        rcx::Node a; a.kind = rcx::NodeKind::Struct;
        a.name = "A"; a.parentId = 0;
        tree.addNode(a);
        rcx::Node b; b.kind = rcx::NodeKind::Struct;
        b.name = "B"; b.parentId = 0;
        tree.addNode(b);
        QVERIFY(tree.findOverlaps().isEmpty());
    }

    void testFindOverlaps_independentParents() {
        // Two sibling groups under different parents — overlap in one
        // group must not affect the other.
        rcx::NodeTree tree;
        rcx::Node a; a.kind = rcx::NodeKind::Struct;
        a.name = "A"; a.parentId = 0;
        int ai = tree.addNode(a);
        uint64_t aid = tree.nodes[ai].id;
        rcx::Node b; b.kind = rcx::NodeKind::Struct;
        b.name = "B"; b.parentId = 0;
        int bi = tree.addNode(b);
        uint64_t bid = tree.nodes[bi].id;

        // A: overlap pair
        auto mk = [&](uint64_t parent, int off, rcx::NodeKind k) {
            rcx::Node n; n.kind = k; n.parentId = parent; n.offset = off;
            n.name = "x";
            return tree.nodes[tree.addNode(n)].id;
        };
        uint64_t a1 = mk(aid, 0, rcx::NodeKind::UInt32);  // [0,4)
        uint64_t a2 = mk(aid, 2, rcx::NodeKind::UInt32);  // [2,6) — overlaps
        mk(bid, 0, rcx::NodeKind::UInt32);                // clean
        mk(bid, 4, rcx::NodeKind::UInt32);                // clean

        auto overlaps = tree.findOverlaps();
        QCOMPARE(overlaps.size(), 1);
        QCOMPARE(overlaps[0].aId, a1);
        QCOMPARE(overlaps[0].bId, a2);
        QCOMPARE(overlaps[0].parentId, aid);
    }

    void testFieldPath_simple() {
        rcx::NodeTree tree;
        rcx::Node root;
        root.kind = rcx::NodeKind::Struct;
        root.name = ""; root.structTypeName = "Player";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node child;
        child.kind = rcx::NodeKind::UInt32;
        child.name = "Health";
        child.parentId = rootId;
        int ci = tree.addNode(child);
        uint64_t childId = tree.nodes[ci].id;

        QCOMPARE(tree.fieldPath(childId), QString("Player.Health"));
        // The root itself returns just its top-level label.
        QCOMPARE(tree.fieldPath(rootId), QString("Player"));
    }

    void testFieldPath_nested() {
        rcx::NodeTree tree;
        rcx::Node player; player.kind = rcx::NodeKind::Struct;
        player.structTypeName = "Player"; player.parentId = 0;
        int pi = tree.addNode(player);
        uint64_t pId = tree.nodes[pi].id;

        rcx::Node stats; stats.kind = rcx::NodeKind::Struct;
        stats.name = "Stats"; stats.parentId = pId;
        int si = tree.addNode(stats);
        uint64_t sId = tree.nodes[si].id;

        rcx::Node hp; hp.kind = rcx::NodeKind::Float;
        hp.name = "Health"; hp.parentId = sId;
        int hi = tree.addNode(hp);
        uint64_t hId = tree.nodes[hi].id;

        QCOMPARE(tree.fieldPath(hId), QString("Player.Stats.Health"));
    }

    void testFieldPath_unknownId() {
        rcx::NodeTree tree;
        // Nothing added — any id is unknown.
        QCOMPARE(tree.fieldPath(0xDEADBEEF), QString());
    }

    void testNodeIdForPath_roundtrip() {
        rcx::NodeTree tree;
        rcx::Node player; player.kind = rcx::NodeKind::Struct;
        player.structTypeName = "Player"; player.parentId = 0;
        int pi = tree.addNode(player);
        uint64_t pId = tree.nodes[pi].id;

        rcx::Node stats; stats.kind = rcx::NodeKind::Struct;
        stats.name = "Stats"; stats.parentId = pId;
        int si = tree.addNode(stats);
        uint64_t sId = tree.nodes[si].id;

        rcx::Node hp; hp.kind = rcx::NodeKind::Float;
        hp.name = "Health"; hp.parentId = sId;
        int hi = tree.addNode(hp);
        uint64_t hId = tree.nodes[hi].id;

        // Forward → reverse → equality
        QString path = tree.fieldPath(hId);
        QCOMPARE(tree.nodeIdForPath(path), hId);
        QCOMPARE(tree.nodeIdForPath("Player.Stats"), sId);
        QCOMPARE(tree.nodeIdForPath("Player"), pId);
    }

    void testNodeIdForPath_misses() {
        rcx::NodeTree tree;
        rcx::Node root; root.kind = rcx::NodeKind::Struct;
        root.structTypeName = "Player"; root.parentId = 0;
        tree.addNode(root);

        QCOMPARE(tree.nodeIdForPath(""), (uint64_t)0);
        QCOMPARE(tree.nodeIdForPath("Nope"), (uint64_t)0);
        QCOMPARE(tree.nodeIdForPath("Player.Missing"), (uint64_t)0);
    }

    void testFieldPath_customSeparator() {
        rcx::NodeTree tree;
        rcx::Node a; a.kind = rcx::NodeKind::Struct;
        a.structTypeName = "Pkt"; a.parentId = 0;
        int ai = tree.addNode(a);
        uint64_t aId = tree.nodes[ai].id;
        rcx::Node b; b.kind = rcx::NodeKind::UInt16;
        b.name = "len"; b.parentId = aId;
        int bi = tree.addNode(b);

        QCOMPARE(tree.fieldPath(tree.nodes[bi].id, QChar('/')),
                 QString("Pkt/len"));
    }
};

QTEST_MAIN(TestCore)
#include "test_core.moc"
