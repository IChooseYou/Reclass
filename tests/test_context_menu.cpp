#include <QtTest/QTest>
#include <QApplication>
#include <QSplitter>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "core.h"

using namespace rcx;

static void buildTree(NodeTree& tree) {
    tree.baseAddress = 0;

    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "Player";
    root.name = "Player";
    root.parentId = 0;
    root.offset = 0;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    auto field = [&](int off, NodeKind k, const char* name) {
        Node n;
        n.kind = k;
        n.name = name;
        n.parentId = rootId;
        n.offset = off;
        tree.addNode(n);
    };

    field(0,  NodeKind::Int32,  "health");
    field(4,  NodeKind::Int32,  "armor");
    field(8,  NodeKind::Float,  "speed");
    field(12, NodeKind::Hex32,  "flags");
}

static QByteArray makeBuffer() {
    QByteArray data(128, '\0');
    int32_t health = 100;
    memcpy(data.data() + 0, &health, 4);
    int32_t armor = 50;
    memcpy(data.data() + 4, &armor, 4);
    float speed = 3.5f;
    memcpy(data.data() + 8, &speed, 4);
    uint32_t flags = 0xFF00FF00;
    memcpy(data.data() + 12, &flags, 4);
    return data;
}

class TestContextMenu : public QObject {
    Q_OBJECT
private:
    RcxDocument* m_doc = nullptr;
    RcxController* m_ctrl = nullptr;
    QSplitter* m_splitter = nullptr;
    RcxEditor* m_editor = nullptr;

    int findNode(const QString& name) const {
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == name) return i;
        return -1;
    }

    int countNodes() const { return m_doc->tree.nodes.size(); }

private slots:
    void init() {
        m_doc = new RcxDocument();
        buildTree(m_doc->tree);
        m_doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        m_splitter = new QSplitter();
        m_ctrl = new RcxController(m_doc, nullptr);
        m_editor = m_ctrl->addSplitEditor(m_splitter);

        m_splitter->resize(800, 600);
        m_splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_splitter));
        QApplication::processEvents();
    }

    void cleanup() {
        delete m_ctrl;  m_ctrl = nullptr;
        m_editor = nullptr;
        delete m_splitter;  m_splitter = nullptr;
        delete m_doc;  m_doc = nullptr;
    }

    // ── Insert adds exactly one node ──

    void testInsertAddsOneNode() {
        int before = countNodes();
        uint64_t rootId = m_doc->tree.nodes[0].id;
        m_ctrl->insertNode(rootId, 16, NodeKind::Hex64, "inserted");
        QApplication::processEvents();

        QCOMPARE(countNodes(), before + 1);

        int idx = findNode("inserted");
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::Hex64);
        QCOMPARE(m_doc->tree.nodes[idx].offset, 16);
        QCOMPARE(m_doc->tree.nodes[idx].parentId, rootId);
    }

    // ── Insert at auto-offset places after last sibling ──

    void testInsertAutoOffset() {
        uint64_t rootId = m_doc->tree.nodes[0].id;

        // Last child is "flags" at offset 12, size 4 → end = 16
        m_ctrl->insertNode(rootId, -1, NodeKind::Hex64, "autoPlaced");
        QApplication::processEvents();

        int idx = findNode("autoPlaced");
        QVERIFY(idx >= 0);
        // Hex64 is 8-byte aligned, next aligned offset after 16 is 16
        QCOMPARE(m_doc->tree.nodes[idx].offset, 16);
    }

    // ── Duplicate creates exactly one copy ──

    void testDuplicateAddsOneNode() {
        int flagsIdx = findNode("flags");
        QVERIFY(flagsIdx >= 0);
        int before = countNodes();

        m_ctrl->duplicateNode(flagsIdx);
        QApplication::processEvents();

        QCOMPARE(countNodes(), before + 1);

        int copyIdx = findNode("flags_copy");
        QVERIFY2(copyIdx >= 0, "Expected a node named 'flags_copy'");
        QCOMPARE(m_doc->tree.nodes[copyIdx].kind, NodeKind::Hex32);
        QCOMPARE(m_doc->tree.nodes[copyIdx].offset, 16);  // flags(12) + 4 = 16
    }

    // ── Duplicate preserves original node unchanged ──

    void testDuplicatePreservesOriginal() {
        int flagsIdx = findNode("flags");
        QVERIFY(flagsIdx >= 0);
        NodeKind origKind = m_doc->tree.nodes[flagsIdx].kind;
        int origOffset = m_doc->tree.nodes[flagsIdx].offset;
        QString origName = m_doc->tree.nodes[flagsIdx].name;

        m_ctrl->duplicateNode(flagsIdx);
        QApplication::processEvents();

        // Original should be unchanged (re-find in case index shifted)
        flagsIdx = findNode("flags");
        QVERIFY(flagsIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[flagsIdx].kind, origKind);
        QCOMPARE(m_doc->tree.nodes[flagsIdx].offset, origOffset);
        QCOMPARE(m_doc->tree.nodes[flagsIdx].name, origName);
    }

    // ── Duplicate undo removes the copy ──

    void testDuplicateUndo() {
        int before = countNodes();
        int flagsIdx = findNode("flags");
        QVERIFY(flagsIdx >= 0);

        m_ctrl->duplicateNode(flagsIdx);
        QApplication::processEvents();
        QCOMPARE(countNodes(), before + 1);

        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(countNodes(), before);
        QCOMPARE(findNode("flags_copy"), -1);
    }

    // ── Duplicate on struct is no-op ──

    void testDuplicateStructNoOp() {
        int rootIdx = findNode("Player");
        QVERIFY(rootIdx >= 0);
        int before = countNodes();

        m_ctrl->duplicateNode(rootIdx);
        QApplication::processEvents();

        QCOMPARE(countNodes(), before);
    }

    // ── Insert at root level (parentId=0) ──

    void testInsertAtRootLevel() {
        int before = countNodes();
        m_ctrl->insertNode(0, -1, NodeKind::Hex64, "rootField");
        QApplication::processEvents();

        QCOMPARE(countNodes(), before + 1);
        int idx = findNode("rootField");
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].parentId, (uint64_t)0);
    }

    // ── Append 128 bytes adds exactly 16 Hex64 nodes ──

    void testAppend128Bytes() {
        int before = countNodes();

        // Simulate what "Append 128 bytes" does
        m_ctrl->document()->undoStack.beginMacro("Append 128 bytes");
        for (int i = 0; i < 16; i++)
            m_ctrl->insertNode(0, -1, NodeKind::Hex64,
                               QStringLiteral("field_%1").arg(i));
        m_ctrl->document()->undoStack.endMacro();
        QApplication::processEvents();

        QCOMPARE(countNodes(), before + 16);

        // All should be root-level Hex64
        int foundCount = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const auto& n = m_doc->tree.nodes[i];
            if (n.name.startsWith("field_") && n.parentId == 0
                && n.kind == NodeKind::Hex64) {
                foundCount++;
            }
        }
        QCOMPARE(foundCount, 16);
    }

    // ── Append 128 bytes undo removes all 16 at once ──

    void testAppend128BytesUndo() {
        int before = countNodes();

        m_ctrl->document()->undoStack.beginMacro("Append 128 bytes");
        for (int i = 0; i < 16; i++)
            m_ctrl->insertNode(0, -1, NodeKind::Hex64,
                               QStringLiteral("field_%1").arg(i));
        m_ctrl->document()->undoStack.endMacro();
        QApplication::processEvents();
        QCOMPARE(countNodes(), before + 16);

        // Single undo undoes the entire macro
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(countNodes(), before);
    }

    // ── Insert child into struct ──

    void testInsertChildIntoStruct() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        int before = countNodes();

        m_ctrl->insertNode(rootId, 0, NodeKind::Hex64, "childField");
        QApplication::processEvents();

        QCOMPARE(countNodes(), before + 1);
        int idx = findNode("childField");
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].parentId, rootId);
        QCOMPARE(m_doc->tree.nodes[idx].offset, 0);
    }

    // ── Remove node then undo restores it ──

    void testRemoveAndUndoNode() {
        int flagsIdx = findNode("flags");
        QVERIFY(flagsIdx >= 0);
        int before = countNodes();

        m_ctrl->removeNode(flagsIdx);
        QApplication::processEvents();
        QCOMPARE(countNodes(), before - 1);
        QCOMPARE(findNode("flags"), -1);

        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(countNodes(), before);
        QVERIFY(findNode("flags") >= 0);
    }

    // ── Multiple duplicates each add exactly one ──

    void testMultipleDuplicates() {
        int before = countNodes();
        int healthIdx = findNode("health");
        QVERIFY(healthIdx >= 0);

        m_ctrl->duplicateNode(healthIdx);
        QApplication::processEvents();
        QCOMPARE(countNodes(), before + 1);

        int copyIdx = findNode("health_copy");
        QVERIFY(copyIdx >= 0);

        m_ctrl->duplicateNode(copyIdx);
        QApplication::processEvents();
        QCOMPARE(countNodes(), before + 2);

        int copy2Idx = findNode("health_copy_copy");
        QVERIFY(copy2Idx >= 0);
    }

    // ── Duplicate copy has correct parent ──

    void testDuplicateCopyParent() {
        int healthIdx = findNode("health");
        QVERIFY(healthIdx >= 0);
        uint64_t parentId = m_doc->tree.nodes[healthIdx].parentId;

        m_ctrl->duplicateNode(healthIdx);
        QApplication::processEvents();

        int copyIdx = findNode("health_copy");
        QVERIFY(copyIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[copyIdx].parentId, parentId);
    }

    // ── Insert struct at root then add children ──

    void testInsertStructAndChildren() {
        int before = countNodes();

        m_ctrl->insertNode(0, -1, NodeKind::Struct, "NewClass");
        QApplication::processEvents();
        QCOMPARE(countNodes(), before + 1);

        int structIdx = findNode("NewClass");
        QVERIFY(structIdx >= 0);
        uint64_t structId = m_doc->tree.nodes[structIdx].id;

        m_ctrl->insertNode(structId, 0, NodeKind::Int32, "x");
        m_ctrl->insertNode(structId, -1, NodeKind::Int32, "y");
        QApplication::processEvents();
        QCOMPARE(countNodes(), before + 3);

        int xIdx = findNode("x");
        int yIdx = findNode("y");
        QVERIFY(xIdx >= 0);
        QVERIFY(yIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[xIdx].parentId, structId);
        QCOMPARE(m_doc->tree.nodes[yIdx].parentId, structId);
    }

    // ── Batch remove deletes multiple nodes ──

    void testBatchRemove() {
        int healthIdx = findNode("health");
        int armorIdx = findNode("armor");
        QVERIFY(healthIdx >= 0);
        QVERIFY(armorIdx >= 0);
        int before = countNodes();

        m_ctrl->batchRemoveNodes({healthIdx, armorIdx});
        QApplication::processEvents();
        QCOMPARE(countNodes(), before - 2);
        QCOMPARE(findNode("health"), -1);
        QCOMPARE(findNode("armor"), -1);

        // Undo restores both
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(countNodes(), before);
        QVERIFY(findNode("health") >= 0);
        QVERIFY(findNode("armor") >= 0);
    }

    // ── Insert with invalid parent still works (root-level) ──

    void testInsertInvalidParent() {
        int before = countNodes();
        // parentId=999 doesn't exist, but insertNode doesn't validate parent
        m_ctrl->insertNode(999, 0, NodeKind::Hex32, "orphan");
        QApplication::processEvents();
        QCOMPARE(countNodes(), before + 1);
    }

    // ── Duplicate out-of-range index is no-op ──

    void testDuplicateInvalidIndex() {
        int before = countNodes();
        m_ctrl->duplicateNode(-1);
        m_ctrl->duplicateNode(9999);
        QApplication::processEvents();
        QCOMPARE(countNodes(), before);
    }

    // ── Remove out-of-range index is no-op ──

    void testRemoveInvalidIndex() {
        int before = countNodes();
        m_ctrl->removeNode(-1);
        m_ctrl->removeNode(9999);
        QApplication::processEvents();
        QCOMPARE(countNodes(), before);
    }

    // ── Change to Ptr* creates class and sets refId ──

    void testChangeToPtrStarCreatesClassAndSetsRef() {
        // Add a Hex64 node to the root struct
        uint64_t rootId = m_doc->tree.nodes[0].id;
        m_ctrl->insertNode(rootId, 16, NodeKind::Hex64, "ptrField");
        QApplication::processEvents();

        int ptrIdx = findNode("ptrField");
        QVERIFY(ptrIdx >= 0);
        uint64_t ptrNodeId = m_doc->tree.nodes[ptrIdx].id;
        int before = countNodes();

        // Convert to typed pointer
        m_ctrl->convertToTypedPointer(ptrNodeId);
        QApplication::processEvents();

        // Re-find after tree mutation
        ptrIdx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].id == ptrNodeId) { ptrIdx = i; break; }
        }
        QVERIFY(ptrIdx >= 0);

        // Verify: node kind changed to Pointer64
        QCOMPARE(m_doc->tree.nodes[ptrIdx].kind, NodeKind::Pointer64);

        // Verify: node.refId != 0
        uint64_t refId = m_doc->tree.nodes[ptrIdx].refId;
        QVERIFY(refId != 0);

        // Verify: a new Struct node exists with the refId as its id
        int structIdx = m_doc->tree.indexOfId(refId);
        QVERIFY(structIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[structIdx].kind, NodeKind::Struct);

        // Verify: the new struct has children (Hex64 fields)
        auto children = m_doc->tree.childrenOf(refId);
        QVERIFY(children.size() == 16);
        for (int ci : children)
            QCOMPARE(m_doc->tree.nodes[ci].kind, NodeKind::Hex64);

        // Verify: total nodes increased by 1 struct + 16 children = 17
        QCOMPARE(countNodes(), before + 17);

        // Verify: undo restores the original Hex64 kind and refId==0
        m_doc->undoStack.undo();
        QApplication::processEvents();

        ptrIdx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].id == ptrNodeId) { ptrIdx = i; break; }
        }
        QVERIFY(ptrIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[ptrIdx].kind, NodeKind::Hex64);
        QCOMPARE(m_doc->tree.nodes[ptrIdx].refId, (uint64_t)0);
        QCOMPARE(countNodes(), before);
    }

    // ── Extract byte selection into a new class ─────────────────────
    //
    // Builds a clean fixture: root struct with 4× Hex64 at offsets
    // 0/8/16/24. Selecting [4, 20) crosses three rows (Hex64@0 right
    // half, Hex64@8 fully, Hex64@16 left half) and exercises every
    // split path.
    void testExtractByteSelection_MidRowAcrossRows() {
        // Wipe the init() fixture and lay out 4 contiguous Hex64s.
        m_doc->tree.nodes.clear();
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = "Holder";
        root.name = "h";
        root.parentId = 0;
        m_doc->tree.addNode(root);
        uint64_t rootId = m_doc->tree.nodes[0].id;
        for (int i = 0; i < 4; ++i) {
            Node n; n.kind = NodeKind::Hex64;
            n.name = QString("h%1").arg(i);
            n.parentId = rootId;
            n.offset = i * 8;
            m_doc->tree.addNode(n);
        }
        int before = countNodes();
        QCOMPARE(before, 5);  // root + 4 fields

        // Selection [4, 20) — Hex64@0 right half (4 bytes), Hex64@8
        // fully (8 bytes), Hex64@16 left half (4 bytes). Total 16.
        m_ctrl->extractByteSelectionToNewClass(4, 20);
        QApplication::processEvents();

        // A new root struct named UnnamedClass0 exists.
        int extractedRootIdx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); ++i) {
            const auto& n = m_doc->tree.nodes[i];
            if (n.parentId == 0 && n.kind == NodeKind::Struct
                && n.structTypeName == "UnnamedClass0") {
                extractedRootIdx = i;
                break;
            }
        }
        QVERIFY2(extractedRootIdx >= 0, "UnnamedClass0 root struct missing");
        uint64_t extractedRootId = m_doc->tree.nodes[extractedRootIdx].id;

        // UnnamedClass0 should contain greedy hex packing of 16 bytes:
        // Hex64 + Hex64 (8 + 8 = 16).
        int extSpan = m_doc->tree.structSpan(extractedRootId);
        QCOMPARE(extSpan, 16);

        // The original Holder should have:
        //   pad at 0 (4 bytes, e.g. Hex32),
        //   embedded Struct at 4 (refId = extractedRootId, span 16),
        //   pad at 20 (4 bytes),
        //   Hex64 at 24 (untouched).
        auto childrenOfHolder = m_doc->tree.childrenOf(rootId);
        QVector<Node> holderKids;
        for (int ci : childrenOfHolder) holderKids.append(m_doc->tree.nodes[ci]);
        std::sort(holderKids.begin(), holderKids.end(),
                  [](const Node& a, const Node& b) { return a.offset < b.offset; });

        QCOMPARE(holderKids.size(), 4);
        // [0, 4): pad
        QVERIFY(isHexNode(holderKids[0].kind));
        QCOMPARE(holderKids[0].offset, 0);
        QCOMPARE(holderKids[0].byteSize(), 4);
        // [4, 20): embedded Struct with refId
        QCOMPARE(holderKids[1].kind, NodeKind::Struct);
        QCOMPARE(holderKids[1].offset, 4);
        QCOMPARE(holderKids[1].refId, extractedRootId);
        QCOMPARE(m_doc->tree.structSpan(holderKids[1].id), 16);
        // [20, 24): pad
        QVERIFY(isHexNode(holderKids[2].kind));
        QCOMPARE(holderKids[2].offset, 20);
        QCOMPARE(holderKids[2].byteSize(), 4);
        // [24, 32): original Hex64
        QCOMPARE(holderKids[3].kind, NodeKind::Hex64);
        QCOMPARE(holderKids[3].offset, 24);
    }

    // Selection fully inside a single Hex64 splits that row into
    // left pad + struct + right pad.
    void testExtractByteSelection_InsideSingleRow() {
        m_doc->tree.nodes.clear();
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = "Holder";
        root.parentId = 0;
        m_doc->tree.addNode(root);
        uint64_t rootId = m_doc->tree.nodes[0].id;
        Node h; h.kind = NodeKind::Hex64; h.parentId = rootId; h.offset = 0;
        m_doc->tree.addNode(h);

        m_ctrl->extractByteSelectionToNewClass(2, 5);  // 3 bytes mid-row
        QApplication::processEvents();

        auto kids = m_doc->tree.childrenOf(rootId);
        // leftPad 2 bytes → 1 Hex16; struct (1); rightPad 3 bytes → Hex16 + Hex8 (2)
        QCOMPARE(kids.size(), 4);
        QVector<Node> k;
        for (int ci : kids) k.append(m_doc->tree.nodes[ci]);
        std::sort(k.begin(), k.end(),
                  [](const Node& a, const Node& b) { return a.offset < b.offset; });
        QCOMPARE(k[0].offset, 0); QCOMPARE(k[0].byteSize(), 2);  // Hex16 pad [0,2)
        QCOMPARE(k[1].offset, 2); QCOMPARE(k[1].kind, NodeKind::Struct);  // extracted [2,5)
        QCOMPARE(k[2].offset, 5); QCOMPARE(k[2].byteSize(), 2);  // Hex16 [5,7)
        QCOMPARE(k[3].offset, 7); QCOMPARE(k[3].byteSize(), 1);  // Hex8  [7,8)
    }

    // Row-aligned selection: no left or right pads needed.
    void testExtractByteSelection_RowAlignedNoPads() {
        m_doc->tree.nodes.clear();
        Node root; root.kind = NodeKind::Struct;
        root.structTypeName = "Holder"; root.parentId = 0;
        m_doc->tree.addNode(root);
        uint64_t rootId = m_doc->tree.nodes[0].id;
        for (int i = 0; i < 3; ++i) {
            Node n; n.kind = NodeKind::Hex64;
            n.parentId = rootId; n.offset = i * 8;
            m_doc->tree.addNode(n);
        }

        // Selection [0, 16) — exactly the first two Hex64 rows.
        m_ctrl->extractByteSelectionToNewClass(0, 16);
        QApplication::processEvents();

        auto kids = m_doc->tree.childrenOf(rootId);
        QCOMPARE(kids.size(), 2);  // embedded struct + remaining Hex64
        QVector<Node> k;
        for (int ci : kids) k.append(m_doc->tree.nodes[ci]);
        std::sort(k.begin(), k.end(),
                  [](const Node& a, const Node& b) { return a.offset < b.offset; });
        QCOMPARE(k[0].kind, NodeKind::Struct);
        QCOMPARE(k[0].offset, 0);
        QCOMPARE(m_doc->tree.structSpan(k[0].id), 16);
        QCOMPARE(k[1].kind, NodeKind::Hex64);
        QCOMPARE(k[1].offset, 16);
    }

    // Undo restores the pre-extract tree state in one step (the
    // implementation wraps every insert/remove in a single macro).
    void testExtractByteSelection_UndoRestoresOriginal() {
        m_doc->tree.nodes.clear();
        Node root; root.kind = NodeKind::Struct;
        root.structTypeName = "Holder"; root.parentId = 0;
        m_doc->tree.addNode(root);
        uint64_t rootId = m_doc->tree.nodes[0].id;
        for (int i = 0; i < 3; ++i) {
            Node n; n.kind = NodeKind::Hex64;
            n.parentId = rootId; n.offset = i * 8;
            m_doc->tree.addNode(n);
        }
        int beforeCount = countNodes();
        QVector<int> beforeOffsets;
        QVector<NodeKind> beforeKinds;
        for (int ci : m_doc->tree.childrenOf(rootId)) {
            beforeOffsets.append(m_doc->tree.nodes[ci].offset);
            beforeKinds.append(m_doc->tree.nodes[ci].kind);
        }

        m_ctrl->extractByteSelectionToNewClass(4, 20);
        QApplication::processEvents();
        QVERIFY(countNodes() != beforeCount);  // mutation happened

        m_doc->undoStack.undo();
        QApplication::processEvents();

        QCOMPARE(countNodes(), beforeCount);
        QVector<int> afterOffsets;
        QVector<NodeKind> afterKinds;
        for (int ci : m_doc->tree.childrenOf(rootId)) {
            afterOffsets.append(m_doc->tree.nodes[ci].offset);
            afterKinds.append(m_doc->tree.nodes[ci].kind);
        }
        QCOMPARE(afterOffsets, beforeOffsets);
        QCOMPARE(afterKinds, beforeKinds);
    }

    // Fully-contained typed (non-hex) siblings are preserved by name + kind
    // when moved into the extracted class. Hex partials still get repacked.
    void testExtractByteSelection_PreservesTypedFields() {
        m_doc->tree.nodes.clear();
        Node root; root.kind = NodeKind::Struct;
        root.structTypeName = "Mixed"; root.parentId = 0;
        m_doc->tree.addNode(root);
        uint64_t rootId = m_doc->tree.nodes[0].id;
        Node h1; h1.kind = NodeKind::Hex64; h1.parentId = rootId; h1.offset = 0;
        m_doc->tree.addNode(h1);
        Node iv;  iv.kind = NodeKind::Int32; iv.parentId = rootId; iv.offset = 8;
        iv.name = "iv";
        m_doc->tree.addNode(iv);
        Node h2; h2.kind = NodeKind::Hex64; h2.parentId = rootId; h2.offset = 12;
        m_doc->tree.addNode(h2);

        // Selection [4, 16) — left half of h1 stays, Int32 (fully
        // contained) moves to new class, right half of h2 stays.
        // Extract size = 12.
        m_ctrl->extractByteSelectionToNewClass(4, 16);
        QApplication::processEvents();

        // Find the UnnamedClass0.
        int extractedRootIdx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); ++i) {
            const auto& n = m_doc->tree.nodes[i];
            if (n.parentId == 0 && n.kind == NodeKind::Struct
                && n.structTypeName == "UnnamedClass0") {
                extractedRootIdx = i; break;
            }
        }
        QVERIFY(extractedRootIdx >= 0);
        uint64_t exId = m_doc->tree.nodes[extractedRootIdx].id;

        // The new class should contain: hex pad [0,4) + Int32 "iv" at 4 + hex pad [8,12).
        QVector<Node> exKids;
        for (int ci : m_doc->tree.childrenOf(exId))
            exKids.append(m_doc->tree.nodes[ci]);
        std::sort(exKids.begin(), exKids.end(),
                  [](const Node& a, const Node& b) { return a.offset < b.offset; });
        QCOMPARE(m_doc->tree.structSpan(exId), 12);

        // Locate the Int32 — it MUST keep its name and kind.
        bool foundIv = false;
        for (const Node& n : exKids) {
            if (n.kind == NodeKind::Int32 && n.name == "iv") {
                foundIv = true;
                QCOMPARE(n.offset, 4);  // 8 (original) - 4 (relLo) = 4
                break;
            }
        }
        QVERIFY2(foundIv, "Int32 'iv' was not preserved in the extracted class");
    }

    // Regression: multiple root structs in the tree (vtable + main
    // class, like the tutorial) must not interfere. The VTable struct's
    // children have offsets 0/8/16/... that numerically overlap with
    // the user's selection in the main class's frame, but they belong
    // to a completely different address space. Filtered out via
    // viewRootId; selection on the main class must succeed.
    void testExtractByteSelection_IgnoresUnrelatedRootStructs() {
        m_doc->tree.nodes.clear();
        // Root #1: VTable struct with FuncPtr64 children at 0/8/16/...
        Node vt; vt.kind = NodeKind::Struct;
        vt.structTypeName = "VTable"; vt.parentId = 0;
        m_doc->tree.addNode(vt);
        uint64_t vtId = m_doc->tree.nodes[0].id;
        for (int i = 0; i < 4; ++i) {
            Node fn; fn.kind = NodeKind::FuncPtr64;
            fn.parentId = vtId; fn.offset = i * 8;
            m_doc->tree.addNode(fn);
        }
        // Root #2: Main class — also has fields at 0/8/16/... offsets.
        Node main; main.kind = NodeKind::Struct;
        main.structTypeName = "Main"; main.parentId = 0;
        m_doc->tree.addNode(main);
        uint64_t mainId = m_doc->tree.nodes[5].id;
        for (int i = 0; i < 4; ++i) {
            Node h; h.kind = NodeKind::Hex64;
            h.parentId = mainId; h.offset = i * 8;
            m_doc->tree.addNode(h);
        }

        // Pin the controller's view to Main — otherwise the extract
        // method has no way to know which root the user is looking at.
        m_ctrl->setViewRootId(mainId);

        // Selection [0, 16) within Main. Should extract two Hex64s
        // even though VTable's FuncPtr64s share those offsets.
        m_ctrl->extractByteSelectionToNewClass(0, 16);
        QApplication::processEvents();

        int extractedRootIdx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); ++i) {
            const auto& n = m_doc->tree.nodes[i];
            if (n.parentId == 0 && n.kind == NodeKind::Struct
                && n.structTypeName == "UnnamedClass0") {
                extractedRootIdx = i; break;
            }
        }
        QVERIFY2(extractedRootIdx >= 0, "UnnamedClass0 missing — VTable's FuncPtr64s falsely triggered cross-parent refusal");
        uint64_t exId = m_doc->tree.nodes[extractedRootIdx].id;
        QCOMPARE(m_doc->tree.structSpan(exId), 16);

        // VTable is untouched.
        auto vtKids = m_doc->tree.childrenOf(vtId);
        QCOMPARE(vtKids.size(), 4);
    }

    // Partial overlap on a typed field is still refused (splitting a
    // typed field's bytes isn't meaningful).
    void testExtractByteSelection_RefusesPartialNonHex() {
        m_doc->tree.nodes.clear();
        Node root; root.kind = NodeKind::Struct;
        root.structTypeName = "Mixed"; root.parentId = 0;
        m_doc->tree.addNode(root);
        uint64_t rootId = m_doc->tree.nodes[0].id;
        Node h1; h1.kind = NodeKind::Hex64; h1.parentId = rootId; h1.offset = 0;
        m_doc->tree.addNode(h1);
        Node iv;  iv.kind = NodeKind::Int32; iv.parentId = rootId; iv.offset = 8;
        iv.name = "iv";
        m_doc->tree.addNode(iv);

        int before = countNodes();
        // Selection [4, 10) — Int32 [8,12) only partially contained.
        m_ctrl->extractByteSelectionToNewClass(4, 10);
        QApplication::processEvents();
        // Tree unchanged.
        QCOMPARE(countNodes(), before);
        for (const auto& nd : m_doc->tree.nodes)
            QVERIFY(nd.structTypeName != QStringLiteral("UnnamedClass0"));
    }
};

QTEST_MAIN(TestContextMenu)
#include "test_context_menu.moc"
