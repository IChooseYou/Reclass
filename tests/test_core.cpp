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
        QCOMPARE(rcx::linesForKind(rcx::NodeKind::Vec2),  2);
        QCOMPARE(rcx::linesForKind(rcx::NodeKind::Vec3),  3);
        QCOMPARE(rcx::linesForKind(rcx::NodeKind::Vec4),  4);
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

    void testFileProvider() {
        QByteArray data(16, '\0');
        data[0] = 0x42;
        data[4] = 0x10;
        data[5] = 0x20;

        rcx::FileProvider prov(data);
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
        rcx::FileProvider prov(data);
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
        rcx::FileProvider prov(data);
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
        rcx::FileProvider prov(data);
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

        // kFoldCol (3) + depth*3 = 6
        auto ts = rcx::typeSpanFor(lm);
        QVERIFY(ts.valid);
        QCOMPARE(ts.start, 6);
        QCOMPARE(ts.end, 20);   // 6 + 14 (kColType)

        auto ns = rcx::nameSpanFor(lm);
        QVERIFY(ns.valid);
        QCOMPARE(ns.start, 22); // 6 + 14 + 2
        QCOMPARE(ns.end, 44);   // 22 + 22 (kColName)

        auto vs = rcx::valueSpanFor(lm, 100);
        QVERIFY(vs.valid);
        QCOMPARE(vs.start, 46); // 22 + 22 + 2
        QCOMPARE(vs.end, 78);   // 46 + 32 (kColValue)
    }

    void testColumnSpan_continuation() {
        rcx::LineMeta lm;
        lm.lineKind = rcx::LineKind::Continuation;
        lm.depth = 1;
        lm.isContinuation = true;
        lm.nodeIdx = 0;

        QVERIFY(!rcx::typeSpanFor(lm).valid);
        QVERIFY(!rcx::nameSpanFor(lm).valid);

        auto vs = rcx::valueSpanFor(lm, 100);
        QVERIFY(vs.valid);
        QCOMPARE(vs.start, 6 + 14 + 22 + 4);  // kFoldCol+indent + kColType(14) + kColName(22) + 4
        QCOMPARE(vs.end, 46 + 32);   // start + kColValue
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
        QCOMPARE(ns.start, 19); // 3 + 14 + 2
        QCOMPARE(ns.end, 41);   // 19 + 22 (kColName)

        auto vs = rcx::valueSpanFor(lm, 100);
        QVERIFY(vs.valid);
        QCOMPARE(vs.start, 43); // 19 + 22 + 2
        QCOMPARE(vs.end, 75);   // 43 + 32 (kColValue)
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
};

QTEST_MAIN(TestCore)
#include "test_core.moc"
