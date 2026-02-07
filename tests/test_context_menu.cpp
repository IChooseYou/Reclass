#include <QtTest/QTest>
#include <QApplication>
#include <QSplitter>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "core.h"

using namespace rcx;

static void buildTree(NodeTree& tree) {
    tree.baseAddress = 0x1000;

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
};

QTEST_MAIN(TestContextMenu)
#include "test_context_menu.moc"
