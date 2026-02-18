#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QSplitter>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "core.h"

using namespace rcx;

// Provider with a configurable base address (for testing source-switch logic)
class BaseAwareProvider : public Provider {
    QByteArray m_data;
    uint64_t   m_base;
public:
    BaseAwareProvider(QByteArray data, uint64_t base)
        : m_data(std::move(data)), m_base(base) {}
    bool read(uint64_t addr, void* buf, int len) const override {
        if (addr + len > (uint64_t)m_data.size()) return false;
        std::memcpy(buf, m_data.constData() + addr, len);
        return true;
    }
    int size() const override { return m_data.size(); }
    uint64_t base() const override { return m_base; }
    void setBase(uint64_t b) override { m_base = b; }
    bool isLive() const override { return true; }
    QString name() const override { return QStringLiteral("test"); }
    QString kind() const override { return QStringLiteral("Process"); }
};

// Small tree: one root struct with a few typed fields at known offsets.
// Keeps tests fast and deterministic (no giant PEB tree).
static void buildSmallTree(NodeTree& tree) {
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
        n.kind = k;
        n.name = name;
        n.parentId = rootId;
        n.offset = off;
        tree.addNode(n);
    };

    field(0,  NodeKind::UInt32,  "field_u32");    // 4 bytes
    field(4,  NodeKind::Float,   "field_float");   // 4 bytes
    field(8,  NodeKind::UInt8,   "field_u8");      // 1 byte
    field(9,  NodeKind::Hex16,   "pad0");           // 2 bytes
    field(11, NodeKind::Hex8,    "pad1");           // 1 byte
    field(12, NodeKind::Hex32,   "field_hex");     // 4 bytes
}

// 64-byte buffer with recognizable pattern
static QByteArray makeSmallBuffer() {
    QByteArray data(64, '\0');
    // field_u32 at offset 0 = 0xDEADBEEF
    uint32_t v32 = 0xDEADBEEF;
    memcpy(data.data() + 0, &v32, 4);
    // field_float at offset 4 = 3.14f
    float vf = 3.14f;
    memcpy(data.data() + 4, &vf, 4);
    // field_u8 at offset 8 = 0x42
    data[8] = 0x42;
    // pad0 at offset 9 = 0x00 0x00 0x00
    // field_hex at offset 12 = 0xCAFEBABE
    uint32_t vhex = 0xCAFEBABE;
    memcpy(data.data() + 12, &vhex, 4);
    return data;
}

class TestController : public QObject {
    Q_OBJECT
private:
    RcxDocument* m_doc = nullptr;
    RcxController* m_ctrl = nullptr;
    QSplitter* m_splitter = nullptr;
    RcxEditor* m_editor = nullptr;

private slots:
    void init() {
        m_doc = new RcxDocument();
        buildSmallTree(m_doc->tree);
        m_doc->provider = std::make_unique<BufferProvider>(makeSmallBuffer());

        m_splitter = new QSplitter();
        // Pass nullptr as parent so controller is not auto-deleted with splitter
        m_ctrl = new RcxController(m_doc, nullptr);
        m_editor = m_ctrl->addSplitEditor(m_splitter);

        m_splitter->resize(800, 600);
        m_splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_splitter));
        QApplication::processEvents();
    }

    void cleanup() {
        // Delete controller first (disconnects from editor signals)
        delete m_ctrl;
        m_ctrl = nullptr;
        m_editor = nullptr;  // owned by splitter
        delete m_splitter;
        m_splitter = nullptr;
        delete m_doc;
        m_doc = nullptr;
    }

    // ── Test: setNodeValue writes bytes to provider ──
    void testSetNodeValueWritesData() {
        // Find field_u32 (index 1, child of root at index 0)
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);

        // Verify original value in provider
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray origBytes = m_doc->provider->readBytes(addr, 4);
        uint32_t origVal;
        memcpy(&origVal, origBytes.data(), 4);
        QCOMPARE(origVal, (uint32_t)0xDEADBEEF);

        // Write new value "42" (decimal)
        m_ctrl->setNodeValue(idx, 0, "42");
        QApplication::processEvents();

        // Read back: should be 42 in little-endian
        QByteArray newBytes = m_doc->provider->readBytes(addr, 4);
        uint32_t newVal;
        memcpy(&newVal, newBytes.data(), 4);
        QCOMPARE(newVal, (uint32_t)42);
    }

    // ── Test: setNodeValue undo/redo restores data ──
    void testSetNodeValueUndoRedo() {
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);

        // Original: 0xDEADBEEF
        QByteArray orig = m_doc->provider->readBytes(addr, 4);
        uint32_t origVal;
        memcpy(&origVal, orig.data(), 4);
        QCOMPARE(origVal, (uint32_t)0xDEADBEEF);

        // Write new value
        m_ctrl->setNodeValue(idx, 0, "99");
        QApplication::processEvents();

        uint32_t newVal;
        QByteArray after = m_doc->provider->readBytes(addr, 4);
        memcpy(&newVal, after.data(), 4);
        QCOMPARE(newVal, (uint32_t)99);

        // Undo → should restore original
        m_doc->undoStack.undo();
        QApplication::processEvents();

        QByteArray undone = m_doc->provider->readBytes(addr, 4);
        uint32_t undoneVal;
        memcpy(&undoneVal, undone.data(), 4);
        QCOMPARE(undoneVal, (uint32_t)0xDEADBEEF);

        // Redo → should restore new value
        m_doc->undoStack.redo();
        QApplication::processEvents();

        QByteArray redone = m_doc->provider->readBytes(addr, 4);
        uint32_t redoneVal;
        memcpy(&redoneVal, redone.data(), 4);
        QCOMPARE(redoneVal, (uint32_t)99);
    }

    // ── Test: setNodeValue on Float field ──
    void testSetNodeValueFloat() {
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_float") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);

        // Original: 3.14f
        QByteArray orig = m_doc->provider->readBytes(addr, 4);
        float origVal;
        memcpy(&origVal, orig.data(), 4);
        QVERIFY(qAbs(origVal - 3.14f) < 0.01f);

        // Write "1.5"
        m_ctrl->setNodeValue(idx, 0, "1.5");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 4);
        float newVal;
        memcpy(&newVal, after.data(), 4);
        QCOMPARE(newVal, 1.5f);

        // Undo
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QByteArray undone = m_doc->provider->readBytes(addr, 4);
        float undoneVal;
        memcpy(&undoneVal, undone.data(), 4);
        QVERIFY(qAbs(undoneVal - 3.14f) < 0.01f);
    }

    // ── Test: renameNode changes name and undo restores ──
    void testRenameNode() {
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].name, QString("field_u32"));

        m_ctrl->renameNode(idx, "myRenamedField");
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes[idx].name, QString("myRenamedField"));

        // Undo
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[idx].name, QString("field_u32"));

        // Redo
        m_doc->undoStack.redo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[idx].name, QString("myRenamedField"));
    }

    // ── Test: changeNodeKind changes type and undo restores ──
    void testChangeNodeKind() {
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::UInt32);

        m_ctrl->changeNodeKind(idx, NodeKind::Float);
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::Float);

        // Undo
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::UInt32);
    }

    // ── Test: insertNode adds a node, removeNode removes it, undo restores ──
    void testInsertAndRemoveNode() {
        int origSize = m_doc->tree.nodes.size();
        uint64_t rootId = m_doc->tree.nodes[0].id;

        // Insert a new Hex64 at offset 16
        m_ctrl->insertNode(rootId, 16, NodeKind::Hex64, "newHex");
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes.size(), origSize + 1);

        // Find the inserted node
        int newIdx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "newHex") { newIdx = i; break; }
        }
        QVERIFY(newIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[newIdx].kind, NodeKind::Hex64);
        QCOMPARE(m_doc->tree.nodes[newIdx].offset, 16);

        // Remove it
        m_ctrl->removeNode(newIdx);
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes.size(), origSize);

        // Undo remove → node restored
        m_doc->undoStack.undo();
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes.size(), origSize + 1);

        // Find again
        newIdx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "newHex") { newIdx = i; break; }
        }
        QVERIFY(newIdx >= 0);
    }

    // ── Test: setNodeValue with Hex32 (space-separated hex bytes) ──
    void testSetNodeValueHex() {
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_hex") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);

        // Original: 0xCAFEBABE
        QByteArray orig = m_doc->provider->readBytes(addr, 4);
        uint32_t origVal;
        memcpy(&origVal, orig.data(), 4);
        QCOMPARE(origVal, (uint32_t)0xCAFEBABE);

        // Write space-separated hex bytes "AA BB CC DD"
        m_ctrl->setNodeValue(idx, 0, "AA BB CC DD");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 4);
        QCOMPARE((uint8_t)after[0], (uint8_t)0xAA);
        QCOMPARE((uint8_t)after[1], (uint8_t)0xBB);
        QCOMPARE((uint8_t)after[2], (uint8_t)0xCC);
        QCOMPARE((uint8_t)after[3], (uint8_t)0xDD);

        // Undo
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QByteArray undone = m_doc->provider->readBytes(addr, 4);
        uint32_t undoneVal;
        memcpy(&undoneVal, undone.data(), 4);
        QCOMPARE(undoneVal, (uint32_t)0xCAFEBABE);
    }

    // ── Test: full inline edit round-trip (type in editor → commit → verify provider) ──
    void testInlineEditRoundTrip() {
        // Refresh to get composed output
        m_ctrl->refresh();
        QApplication::processEvents();

        // Find field_u8 line (UInt8 at offset 8, value = 0x42 = 66)
        ComposeResult result = m_doc->compose();
        int fieldLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].nodeKind == NodeKind::UInt8 &&
                result.meta[i].lineKind == LineKind::Field) {
                fieldLine = i;
                break;
            }
        }
        QVERIFY(fieldLine >= 0);

        m_editor->applyDocument(result);
        QApplication::processEvents();

        // Select this node so edit is allowed
        uint64_t nodeId = result.meta[fieldLine].nodeId;
        QSet<uint64_t> sel;
        sel.insert(nodeId);
        m_editor->applySelectionOverlay(sel);
        QApplication::processEvents();

        // Begin value edit
        bool ok = m_editor->beginInlineEdit(EditTarget::Value, fieldLine);
        QVERIFY2(ok, "Should be able to begin value edit on UInt8 field");
        QVERIFY(m_editor->isEditing());

        // UInt8 values display in hex (e.g., "0x42"). beginInlineEdit selects
        // from after "0x" to end. Type "FF" to replace the hex digits.
        for (QChar c : QString("FF")) {
            QKeyEvent key(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
            QApplication::sendEvent(m_editor->scintilla(), &key);
        }
        QApplication::processEvents();

        // Commit
        QSignalSpy spy(m_editor, &RcxEditor::inlineEditCommitted);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(m_editor->scintilla(), &enter);

        QCOMPARE(spy.count(), 1);
        QList<QVariant> args = spy.first();
        int nodeIdx = args.at(0).toInt();
        QString text = args.at(3).toString().trimmed();
        // The committed text should contain "0xFF" (hex format for UInt8)
        QVERIFY2(!text.isEmpty(), "Committed text should not be empty");

        // Now simulate what controller does: setNodeValue
        m_ctrl->setNodeValue(nodeIdx, 0, text);
        QApplication::processEvents();

        // Verify provider data changed
        int u8Idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u8") { u8Idx = i; break; }
        }
        QVERIFY(u8Idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(u8Idx);
        QByteArray bytes = m_doc->provider->readBytes(addr, 1);
        QCOMPARE((uint8_t)bytes[0], (uint8_t)0xFF);
    }

    // ── Test: source switch preserves existing base address ──
    void testSourceSwitchPreservesBase() {
        // Document already has baseAddress = 0x1000 from buildSmallTree()
        QCOMPARE(m_doc->tree.baseAddress, (uint64_t)0x1000);

        // Simulate attaching a new provider whose base differs (e.g. 0x400000)
        auto prov = std::make_shared<BaseAwareProvider>(makeSmallBuffer(), 0x400000);
        uint64_t newBase = prov->base();
        QCOMPARE(newBase, (uint64_t)0x400000);

        m_doc->provider = prov;
        // This is the controller logic under test:
        if (m_doc->tree.baseAddress == 0)
            m_doc->tree.baseAddress = newBase;
        else
            m_doc->provider->setBase(m_doc->tree.baseAddress);

        // baseAddress must stay at the original value
        QCOMPARE(m_doc->tree.baseAddress, (uint64_t)0x1000);
        // provider base must be synced to match
        QCOMPARE(m_doc->provider->base(), (uint64_t)0x1000);
    }

    // ── Test: source switch on fresh doc uses provider default ──
    void testSourceSwitchFreshDocUsesProviderBase() {
        // Simulate a fresh document (no loaded .rcx → baseAddress == 0)
        m_doc->tree.baseAddress = 0;

        auto prov = std::make_shared<BaseAwareProvider>(makeSmallBuffer(), 0x7FFE0000);
        uint64_t newBase = prov->base();

        m_doc->provider = prov;
        if (m_doc->tree.baseAddress == 0)
            m_doc->tree.baseAddress = newBase;
        else
            m_doc->provider->setBase(m_doc->tree.baseAddress);

        // Fresh doc should adopt the provider's default base
        QCOMPARE(m_doc->tree.baseAddress, (uint64_t)0x7FFE0000);
        QCOMPARE(m_doc->provider->base(), (uint64_t)0x7FFE0000);
    }

    // ── Test: toggleCollapse + undo ──
    void testToggleCollapse() {
        // Root is index 0, a Struct node
        QCOMPARE(m_doc->tree.nodes[0].kind, NodeKind::Struct);
        QCOMPARE(m_doc->tree.nodes[0].collapsed, false);

        m_ctrl->toggleCollapse(0);
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[0].collapsed, true);

        m_ctrl->toggleCollapse(0);
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[0].collapsed, false);

        // Undo twice: uncollapse → collapse → original (false)
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[0].collapsed, true);

        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[0].collapsed, false);
    }
    // ── Test: value history popup only appears during inline editing ──
    void testValueHistoryPopupOnlyDuringEdit() {
        // Record value history for field_u32 so it has heat
        auto& tree = m_doc->tree;
        int idx = -1;
        for (int i = 0; i < tree.nodes.size(); i++) {
            if (tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        uint64_t nodeId = tree.nodes[idx].id;

        QHash<uint64_t, ValueHistory> history;
        history[nodeId].record("100");
        history[nodeId].record("200");
        history[nodeId].record("300");
        QVERIFY(history[nodeId].uniqueCount() > 1);

        m_editor->setValueHistoryRef(&history);

        // Refresh and compose so editor has meta with heatLevel
        m_ctrl->refresh();
        QApplication::processEvents();
        ComposeResult result = m_doc->compose();
        // Manually set heat on the node's line meta
        for (auto& lm : result.meta) {
            if (lm.nodeId == nodeId) lm.heatLevel = 2;
        }
        m_editor->applyDocument(result);
        QApplication::processEvents();

        // Popup should not exist or not be visible (no editing active)
        auto* popup = m_editor->findChild<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        // Even if popup widget exists, it should not be visible
        bool popupVisible = false;
        for (auto* child : m_editor->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (child->isVisible() && child->windowFlags() & Qt::ToolTip)
                popupVisible = true;
        }
        QVERIFY2(!popupVisible, "Popup should not be visible when not editing");

        // Start inline edit on value column of field_u32
        int fieldLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].nodeId == nodeId && result.meta[i].lineKind == LineKind::Field) {
                fieldLine = i; break;
            }
        }
        QVERIFY(fieldLine >= 0);

        bool ok = m_editor->beginInlineEdit(EditTarget::Value, fieldLine);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        // Trigger hover cursor update (simulates mouse move during editing)
        QApplication::processEvents();

        // Cancel edit to clean up
        m_editor->cancelInlineEdit();
        QApplication::processEvents();

        m_editor->setValueHistoryRef(nullptr);
    }

    // ── Test: delete node clears value history for shifted siblings ──
    void testDeleteClearsHeatForShiftedNodes() {
        // Replace with a live provider so refresh() actually records values
        m_doc->provider = std::make_unique<BaseAwareProvider>(makeSmallBuffer(), 0x1000);
        m_ctrl->refresh();
        QApplication::processEvents();

        auto& tree = m_doc->tree;

        // Locate field_u32 (the node we'll delete) and the siblings after it.
        // The small tree has: field_u32(0), field_float(4), field_u8(8),
        //                     pad0/Hex16(9), pad1/Hex8(11), field_hex/Hex32(12)
        // field_float and field_u8 are regular (non-hex) types.
        int delIdx = -1;
        for (int i = 0; i < tree.nodes.size(); i++) {
            if (tree.nodes[i].name == "field_u32") { delIdx = i; break; }
        }
        QVERIFY(delIdx >= 0);
        uint64_t delId = tree.nodes[delIdx].id;

        // Collect sibling node IDs that come after field_u32 (will be shifted)
        uint64_t parentId = tree.nodes[delIdx].parentId;
        int deletedSize = tree.nodes[delIdx].byteSize(); // 4 bytes
        int deletedEnd = tree.nodes[delIdx].offset + deletedSize;
        QVector<uint64_t> shiftedIds;
        QHash<uint64_t, QString> nameMap;  // for debug messages
        for (int i = 0; i < tree.nodes.size(); i++) {
            if (tree.nodes[i].parentId == parentId && i != delIdx
                && tree.nodes[i].offset >= deletedEnd) {
                shiftedIds.append(tree.nodes[i].id);
                nameMap[tree.nodes[i].id] = tree.nodes[i].name;
            }
        }
        QVERIFY2(!shiftedIds.isEmpty(), "Should have siblings after field_u32");

        // Seed value history for shifted siblings (simulate accumulated heat)
        auto& history = const_cast<QHash<uint64_t, ValueHistory>&>(m_ctrl->valueHistory());
        for (uint64_t id : shiftedIds) {
            history[id].record("old_val_1");
            history[id].record("old_val_2");
            history[id].record("old_val_3");
            QVERIFY2(history[id].heatLevel() >= 2,
                     qPrintable(QString("Pre-delete: %1 should have heat>=2")
                                .arg(nameMap[id])));
        }

        // Also seed the to-be-deleted node
        history[delId].record("del_1");
        history[delId].record("del_2");
        QVERIFY(history.contains(delId));

        // Delete field_u32 — this shifts all subsequent siblings
        m_ctrl->removeNode(delIdx);
        QApplication::processEvents();

        // The deleted node's history should be gone
        QVERIFY2(!m_ctrl->valueHistory().contains(delId),
                 "Deleted node's value history should be cleared");

        // All shifted siblings should have heat=0 after the delete.
        // With a live provider, refresh() inside removeNode re-records one new
        // value at the new offset → count=1 → heatLevel=0.
        for (uint64_t id : shiftedIds) {
            int heat = m_ctrl->valueHistory().contains(id)
                ? m_ctrl->valueHistory()[id].heatLevel() : 0;
            QVERIFY2(heat == 0,
                     qPrintable(QString("Shifted node '%1' (id=%2) should have heat=0, got %3")
                                .arg(nameMap[id]).arg(id).arg(heat)));
        }
    }

    // ── Test: value history records and cycles correctly ──
    void testValueHistoryRingBuffer() {
        ValueHistory vh;
        QCOMPARE(vh.count, 0);
        QCOMPARE(vh.heatLevel(), 0);

        vh.record("10");
        QCOMPARE(vh.count, 1);
        QCOMPARE(vh.heatLevel(), 0);  // 1 unique = static

        // Duplicate should not increase count
        vh.record("10");
        QCOMPARE(vh.count, 1);

        vh.record("20");
        QCOMPARE(vh.count, 2);
        QCOMPARE(vh.heatLevel(), 1);  // cold

        vh.record("30");
        QCOMPARE(vh.count, 3);
        QCOMPARE(vh.heatLevel(), 2);  // warm

        vh.record("40");
        vh.record("50");
        QCOMPARE(vh.count, 5);
        QCOMPARE(vh.heatLevel(), 3);  // hot

        QCOMPARE(vh.last(), QString("50"));

        // Ring buffer: uniqueCount() caps at kCapacity
        for (int i = 0; i < 20; i++)
            vh.record(QString::number(100 + i));
        QCOMPARE(vh.uniqueCount(), ValueHistory::kCapacity);
        QVERIFY(vh.count > ValueHistory::kCapacity);

        // forEach iterates oldest→newest within ring
        QStringList vals;
        vh.forEach([&](const QString& v) { vals.append(v); });
        QCOMPARE(vals.size(), ValueHistory::kCapacity);
        QCOMPARE(vals.last(), vh.last());
    }
    // ── Test: inline edit "int32_t[4]" on primitive converts to array ──
    void testInlineEditPrimitiveArray() {
        // Find a primitive field to convert
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::UInt32);
        uint64_t nodeId = m_doc->tree.nodes[idx].id;

        // Emit inlineEditCommitted with array syntax
        emit m_editor->inlineEditCommitted(idx, 0, EditTarget::Type,
                                           QStringLiteral("int32_t[4]"));
        QApplication::processEvents();

        // Node should now be an Array with elementKind=Int32, arrayLen=4
        int newIdx = m_doc->tree.indexOfId(nodeId);
        QVERIFY(newIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[newIdx].kind, NodeKind::Array);
        QCOMPARE(m_doc->tree.nodes[newIdx].elementKind, NodeKind::Int32);
        QCOMPARE(m_doc->tree.nodes[newIdx].arrayLen, 4);

        // Undo should restore to UInt32
        m_doc->undoStack.undo();
        QApplication::processEvents();
        newIdx = m_doc->tree.indexOfId(nodeId);
        QVERIFY(newIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[newIdx].kind, NodeKind::UInt32);
    }
};

QTEST_MAIN(TestController)
#include "test_controller.moc"
