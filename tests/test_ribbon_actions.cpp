// Ribbon action wiring: RibbonActions ↔ RcxController selection ops.
//
// Fixture: a 4× Hex64 class ("T": h0..h3 at 0/8/16/24) over a 64-byte
// BufferProvider at base 0, one split editor, and a RibbonActions built
// from lambdas that return the fixture's controller/editor — no MainWindow.
// Selection is driven through handleNodeClick (Ctrl toggles), exactly what
// a mouse click does. Runs on the hidden test desktop: never wait for
// window exposure (it never happens off-screen) — show() + a short qWait.
//
// Covers: retype (one macro, pads, selection kept), Add, Insert above
// (ascending blocks, re-resolved anchor), Delete (one macro, undo restores),
// enabled predicates on select / clear / byte-selection / footer, following
// the active controller, pointer-size relabel, byte fills from a ROW
// selection, duplicate-as-one-macro, array grouping, endian swap.

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QAction>
#include <QApplication>
#include <QSplitter>
#include <QMouseEvent>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "core.h"
#include "editor.h"
#include "ribbon_actions.h"
#include "providers/buffer_provider.h"
#include <algorithm>

using namespace rcx;

namespace {

NodeTree buildTree() {
    NodeTree tree;
    tree.baseAddress = 0;   // BufferProvider addresses == tree offsets
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "T";
    root.name = "t";
    root.parentId = 0;
    root.collapsed = false;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;
    for (int i = 0; i < 4; ++i) {
        Node n;
        n.kind = NodeKind::Hex64;
        n.name = QStringLiteral("h%1").arg(i);
        n.parentId = rootId;
        n.offset = i * 8;
        tree.addNode(n);
    }
    return tree;
}

// Recognizable pattern: byte at offset N has value (N + 0x10).
QByteArray buildBuffer() {
    QByteArray data(64, '\0');
    for (int i = 0; i < data.size(); ++i)
        data[i] = (char)(i + 0x10);
    return data;
}

struct Fixture {
    RcxDocument*   doc = nullptr;
    RcxController* ctrl = nullptr;
    QSplitter*     splitter = nullptr;
    RcxEditor*     editor = nullptr;
    uint64_t       rootId = 0;
    QVector<uint64_t> h;   // ids of h0..h3

    void create(NodeTree tree = buildTree()) {
        doc = new RcxDocument();
        doc->tree = std::move(tree);
        doc->provider = std::make_shared<BufferProvider>(buildBuffer(), "test_ribbon");
        splitter = new QSplitter();
        ctrl = new RcxController(doc, nullptr);
        editor = ctrl->addSplitEditor(splitter);
        splitter->resize(1000, 400);
        splitter->show();
        QTest::qWait(50);
        QApplication::processEvents();
        ctrl->refresh();
        QApplication::processEvents();
        rootId = doc->tree.nodes[0].id;
        h.clear();
        for (int ci : doc->tree.childrenOf(rootId)) h.append(doc->tree.nodes[ci].id);
        std::sort(h.begin(), h.end(), [this](uint64_t a, uint64_t b) {
            return node(a).offset < node(b).offset;
        });
    }
    void destroy() {
        delete ctrl;  ctrl = nullptr; editor = nullptr;
        delete splitter; splitter = nullptr;
        delete doc;   doc = nullptr;
    }

    const Node& node(uint64_t id) const { return doc->tree.nodes[doc->tree.indexOfId(id)]; }
    bool exists(uint64_t id) const { return doc->tree.indexOfId(id) >= 0; }

    int lineOf(uint64_t id, LineKind kind = LineKind::Field) const {
        for (int i = 0;; ++i) {
            const LineMeta* lm = editor->metaForLine(i);
            if (!lm) return -1;
            if (lm->nodeId == id && lm->lineKind == kind && !lm->isContinuation && !lm->isMemberLine)
                return i;
        }
    }
    void click(uint64_t id, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        int line = lineOf(id);
        ctrl->handleNodeClick(editor, line, id, mods);
    }
    void clickFooter(uint64_t structId, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        int line = lineOf(structId, LineKind::Footer);
        ctrl->handleNodeClick(editor, line, structId, mods);
    }
    // First composed header/field row of a node (a container's own row); -1 if none.
    int anyLineOf(uint64_t id) const {
        for (int i = 0;; ++i) {
            const LineMeta* lm = editor->metaForLine(i);
            if (!lm) return -1;
            if (lm->nodeId == id && !lm->isContinuation && !lm->isMemberLine && !lm->isArrayElement
                && (lm->lineKind == LineKind::Header || lm->lineKind == LineKind::Field))
                return i;
        }
    }
    // The synthesized element row `elemIdx` of a primitive Array; -1 if none.
    int arrayElemLine(uint64_t arrId, int elemIdx) const {
        for (int i = 0;; ++i) {
            const LineMeta* lm = editor->metaForLine(i);
            if (!lm) return -1;
            if (lm->nodeId == arrId && lm->isArrayElement && lm->arrayElementIdx == elemIdx
                && lm->lineKind == LineKind::Field)
                return i;
        }
    }
    // Test setup only (not undoable): add a node straight into the tree and recompose.
    uint64_t addNode(const Node& n) {
        int i = doc->tree.addNode(n);
        uint64_t id = doc->tree.nodes[i].id;
        ctrl->refresh();
        QApplication::processEvents();
        return id;
    }

    // Root children sorted by offset: (offset, kind, id).
    struct Child { int offset; NodeKind kind; uint64_t id; QString name; };
    QVector<Child> children() const {
        QVector<Child> out;
        for (int ci : doc->tree.childrenOf(rootId)) {
            const Node& n = doc->tree.nodes[ci];
            out.append({n.offset, n.kind, n.id, n.name});
        }
        std::sort(out.begin(), out.end(), [](const Child& a, const Child& b) {
            return a.offset < b.offset;
        });
        return out;
    }
    int undoCount() const { return doc->undoStack.count(); }
};

} // namespace

class TestRibbonActions : public QObject {
    Q_OBJECT

    Fixture m_a;
    RcxController* m_target = nullptr;   // what the ctrl() callback returns
    RibbonActions* m_ra = nullptr;

    QAction* act(const char* id) {
        QAction* a = m_ra->action(QString::fromLatin1(id));
        if (!a) qFatal("no action %s", id);
        return a;
    }
    // Click a ribbon button the way the app does: the selection change has
    // already turned the event loop (so the coalesced enabled-refresh ran)
    // before the user can reach the button. QAction::trigger() is a silent
    // no-op on a disabled action, so assert the predicate first — a wrong
    // predicate must fail HERE, not as a mysterious "nothing happened".
    void trig(const char* id) {
        QApplication::processEvents();
        QAction* a = act(id);
        QVERIFY2(a->isEnabled(), qPrintable(QStringLiteral("%1 is disabled").arg(QLatin1String(id))));
        a->trigger();
    }

private slots:
    void init() {
        m_a.create();
        m_target = m_a.ctrl;
        m_ra = new RibbonActions([this]() { return m_target; },
                                 [this]() -> RcxEditor* { return m_target ? m_target->primaryEditor() : nullptr; });
        m_ra->setActiveController(m_a.ctrl);
    }
    void cleanup() {
        delete m_ra; m_ra = nullptr;
        m_a.destroy();
    }

    // ── Every id exists, carries data where promised ──
    void testIdsAndData() {
        const QStringList ids = m_ra->ids();
        QCOMPARE(ids.size(), 48);   // + type.asm   // + home.panels.rtti (moved off the Home tab)
        for (const char* id : {"type.hex64", "type.int32", "type.pointer", "type.custom",
                               "add.4", "add.2048", "insert.64", "sel.delete", "sel.swap",
                               "home.panels.rtti", "edit.undo", "edit.redo"})
            QVERIFY2(m_ra->action(QString::fromLatin1(id)), id);
        QCOMPARE(act("type.int32")->data().toInt(), int(NodeKind::Int32));
        QCOMPARE(act("type.utf16")->data().toInt(), int(NodeKind::UTF16));
        QCOMPARE(act("add.1024")->data().toInt(), 1024);
        QCOMPARE(act("insert.8")->data().toInt(), 8);
        QVERIFY(!m_ra->action(QStringLiteral("nope")));
        // No action ever carries a shortcut: the editor owns the keys.
        for (const QString& id : ids)
            QVERIFY2(m_ra->action(id)->shortcut().isEmpty(), qPrintable(id));
    }

    // ── (1) Retype 3× Hex64 → Int32: kinds + Hex32 pads, ONE undo step, selection kept ──
    void testRetypeThreeToInt32() {
        m_a.click(m_a.h[0]);
        m_a.click(m_a.h[1], Qt::ControlModifier);
        m_a.click(m_a.h[2], Qt::ControlModifier);
        QCOMPARE(m_a.ctrl->selectedIds().size(), 3);
        const int before = m_a.undoCount();

        trig("type.int32");
        QApplication::processEvents();

        QCOMPARE(m_a.undoCount(), before + 1);
        auto ch = m_a.children();
        QCOMPARE(ch.size(), 7);
        const NodeKind expect[7] = {NodeKind::Int32, NodeKind::Hex32, NodeKind::Int32, NodeKind::Hex32,
                                    NodeKind::Int32, NodeKind::Hex32, NodeKind::Hex64};
        for (int i = 0; i < 7; ++i) {
            QCOMPARE(ch[i].offset, i * 4);
            QCOMPARE(int(ch[i].kind), int(expect[i]));
        }
        QCOMPARE(ch[0].id, m_a.h[0]);
        QCOMPARE(ch[2].id, m_a.h[1]);
        QCOMPARE(ch[4].id, m_a.h[2]);
        QCOMPARE(ch[6].id, m_a.h[3]);
        // Selection preserved (batchChangeKind restores it).
        const QSet<uint64_t> sel = m_a.ctrl->selectedIds();
        QCOMPARE(sel.size(), 3);
        QVERIFY(sel.contains(m_a.h[0]) && sel.contains(m_a.h[1]) && sel.contains(m_a.h[2]));
        // One Ctrl+Z reverts all three.
        m_a.doc->undoStack.undo();
        QCOMPARE(m_a.children().size(), 4);
        QCOMPARE(int(m_a.node(m_a.h[1]).kind), int(NodeKind::Hex64));
    }

    void testByteDragThenFloatConvertsExactlyTwentyFourBytes() {
        auto* sci = m_a.editor->scintilla();
        auto* vp = sci->viewport();
        auto point = [&](uint64_t id, int byte) {
            const int line = m_a.lineOf(id);
            const auto& lm = *m_a.editor->metaForLine(line);
            const auto span = RcxEditor::valueSpan(lm, 0, lm.effectiveTypeW, lm.effectiveNameW);
            const long pos = sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                (unsigned long)line, (long)(span.start + byte * 3));
            return QPoint((int)sci->SendScintilla(
                QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, pos) + 2,
                (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, pos) + 4);
        };
        const QPoint anchor = point(m_a.h[2], 7);
        const QPoint first(2, point(m_a.h[0], 0).y());
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(anchor), QPointF(anchor),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(first), QPointF(first),
            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(first), QPointF(first),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &release);
        QCOMPARE(m_a.editor->byteSelectionRange(), (QPair<uint64_t, uint64_t>{0, 24}));
        const int before = m_a.undoCount();
        trig("type.float");
        const auto children = m_a.children();
        QCOMPARE(children.size(), 7);
        for (int i = 0; i < 6; ++i) {
            QCOMPARE(children[i].kind, NodeKind::Float);
            QCOMPARE(children[i].offset, i * 4);
        }
        QCOMPARE(children[6].id, m_a.h[3]);
        QCOMPARE(children[6].offset, 24);
        QCOMPARE(children[6].kind, NodeKind::Hex64);
        QVERIFY(!m_a.editor->hasByteSelection());
        QCOMPARE(m_a.undoCount(), before + 1);
        m_a.doc->undoStack.undo();
        QCOMPARE(m_a.children().size(), 4);
        for (uint64_t id : m_a.h) QCOMPARE(m_a.node(id).kind, NodeKind::Hex64);
    }

    // Single-node retype goes through the quick-type rule: hex → smaller hex pads.
    void testRetypeSingleHexShrinkPads() {
        m_a.click(m_a.h[1]);
        const int before = m_a.undoCount();
        trig("type.hex32");
        QCOMPARE(m_a.undoCount(), before + 1);
        auto ch = m_a.children();
        QCOMPARE(ch.size(), 5);
        QCOMPARE(int(ch[1].kind), int(NodeKind::Hex32));
        QCOMPARE(ch[1].id, m_a.h[1]);
        QCOMPARE(ch[2].offset, 12);
        QCOMPARE(int(ch[2].kind), int(NodeKind::Hex32));
    }

    // ── (2) Add 64 → 8 new Hex64 at the end, one undo step ──
    void testAdd64Appends8Hex64() {
        const int before = m_a.undoCount();
        trig("add.64");
        QCOMPARE(m_a.undoCount(), before + 1);
        auto ch = m_a.children();
        QCOMPARE(ch.size(), 12);
        for (int i = 4; i < 12; ++i) {
            QCOMPARE(ch[i].offset, i * 8);
            QCOMPARE(int(ch[i].kind), int(NodeKind::Hex64));
        }
        // The originals are untouched.
        for (int i = 0; i < 4; ++i) QCOMPARE(ch[i].id, m_a.h[i]);
        m_a.doc->undoStack.undo();
        QCOMPARE(m_a.children().size(), 4);
    }

    // Add with a remainder: 4 → hex8 ×4 (nothing else fits).
    void testAdd4AppendsFourHex8() {
        trig("add.4");
        auto ch = m_a.children();
        QCOMPARE(ch.size(), 8);
        for (int i = 4; i < 8; ++i) {
            QCOMPARE(ch[i].offset, 32 + (i - 4));
            QCOMPARE(int(ch[i].kind), int(NodeKind::Hex8));
        }
    }

    // ── (3) Insert above h1: 8 → one Hex64 at 8, h1..h3 shift; 64 → 8 ascending blocks ──
    void testInsertAboveShiftsAndAscends() {
        m_a.click(m_a.h[1]);
        int before = m_a.undoCount();
        trig("insert.8");
        QCOMPARE(m_a.undoCount(), before + 1);
        auto ch = m_a.children();
        QCOMPARE(ch.size(), 5);
        QCOMPARE(ch[0].id, m_a.h[0]);
        QCOMPARE(ch[1].offset, 8);
        QCOMPARE(int(ch[1].kind), int(NodeKind::Hex64));
        QVERIFY(ch[1].id != m_a.h[1]);
        QCOMPARE(m_a.node(m_a.h[1]).offset, 16);
        QCOMPARE(m_a.node(m_a.h[2]).offset, 24);
        QCOMPARE(m_a.node(m_a.h[3]).offset, 32);

        // h1 is still selected (anchor re-resolves to its new offset).
        QVERIFY(m_a.ctrl->selectedIds().contains(m_a.h[1]));
        before = m_a.undoCount();
        trig("insert.64");
        QCOMPARE(m_a.undoCount(), before + 1);
        ch = m_a.children();
        QCOMPARE(ch.size(), 13);
        // Blocks 2..9 (indices) sit at 16, 24, …, 72 — ascending, contiguous.
        for (int i = 2; i < 10; ++i) {
            QCOMPARE(ch[i].offset, 16 + (i - 2) * 8);
            QCOMPARE(int(ch[i].kind), int(NodeKind::Hex64));
        }
        QCOMPARE(m_a.node(m_a.h[1]).offset, 80);
        QCOMPARE(m_a.node(m_a.h[3]).offset, 96);
        // Undo the second step only: back to the 5-child layout.
        m_a.doc->undoStack.undo();
        QCOMPARE(m_a.children().size(), 5);
        QCOMPARE(m_a.node(m_a.h[1]).offset, 16);
    }

    // Insert with a remainder above the FIRST field: 4 → four Hex8 at 0..3.
    void testInsert4AboveFirst() {
        m_a.click(m_a.h[0]);
        trig("insert.4");
        auto ch = m_a.children();
        QCOMPARE(ch.size(), 8);
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(ch[i].offset, i);
            QCOMPARE(int(ch[i].kind), int(NodeKind::Hex8));
        }
        QCOMPARE(m_a.node(m_a.h[0]).offset, 4);
    }

    // Footer-only selection: Insert falls back to append.
    void testInsertOnFooterAppends() {
        m_a.clickFooter(m_a.rootId);
        m_ra->refreshEnabled();
        QVERIFY(act("insert.8")->isEnabled());
        QVERIFY(!act("type.int32")->isEnabled());   // footer is not a leaf
        trig("insert.8");
        auto ch = m_a.children();
        QCOMPARE(ch.size(), 5);
        QCOMPARE(ch[4].offset, 32);
        QCOMPARE(int(ch[4].kind), int(NodeKind::Hex64));
    }

    // ── (4) Delete h0 + h2: both gone, siblings shifted, one step, undo restores ──
    void testDeleteTwoIsOneStep() {
        m_a.click(m_a.h[0]);
        m_a.click(m_a.h[2], Qt::ControlModifier);
        const int before = m_a.undoCount();
        trig("sel.delete");
        QCOMPARE(m_a.undoCount(), before + 1);
        QVERIFY(!m_a.exists(m_a.h[0]));
        QVERIFY(!m_a.exists(m_a.h[2]));
        auto ch = m_a.children();
        QCOMPARE(ch.size(), 2);
        QCOMPARE(ch[0].id, m_a.h[1]); QCOMPARE(ch[0].offset, 0);
        QCOMPARE(ch[1].id, m_a.h[3]); QCOMPARE(ch[1].offset, 8);
        QCOMPARE(m_a.ctrl->selectedIds().size(), 0);

        m_a.doc->undoStack.undo();
        ch = m_a.children();
        QCOMPARE(ch.size(), 4);
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(ch[i].id, m_a.h[i]);
            QCOMPARE(ch[i].offset, i * 8);
        }
    }

    // ── (5) Enabled predicates flip on select / clear / byte selection ──
    void testEnabledPredicates() {
        m_ra->refreshEnabled();
        QVERIFY(!act("insert.8")->isEnabled());
        QVERIFY(!act("sel.delete")->isEnabled());
        QVERIFY(!act("sel.duplicate")->isEnabled());
        QVERIFY(!act("type.int32")->isEnabled());
        QVERIFY(!act("type.custom")->isEnabled());
        QVERIFY(!act("sel.zero")->isEnabled());
        QVERIFY(!act("sel.swap")->isEnabled());
        QVERIFY(act("add.64")->isEnabled());
        QVERIFY(!act("edit.undo")->isEnabled());
        QVERIFY(!act("edit.redo")->isEnabled());

        m_a.click(m_a.h[0]);
        m_ra->refreshEnabled();
        QVERIFY(act("insert.8")->isEnabled());
        QVERIFY(act("sel.delete")->isEnabled());
        QVERIFY(act("sel.duplicate")->isEnabled());
        QVERIFY(act("type.int32")->isEnabled());
        QVERIFY(act("type.custom")->isEnabled());     // exactly one
        QVERIFY(act("type.ptrclass")->isEnabled());   // 8-byte leaf, no refId
        QVERIFY(act("type.array")->isEnabled());
        QVERIFY(act("type.class")->isEnabled());      // node span is a region
        QVERIFY(act("sel.zero")->isEnabled());        // writable + node region
        QVERIFY(act("sel.swap")->isEnabled());        // Hex64 is a scalar
        QVERIFY(!act("sel.comment")->isEnabled());    // comments hidden by default
        m_a.ctrl->setShowComments(true);
        m_ra->refreshEnabled();
        QVERIFY(act("sel.comment")->isEnabled());

        m_a.click(m_a.h[1], Qt::ControlModifier);
        m_ra->refreshEnabled();
        QVERIFY(act("type.int32")->isEnabled());
        QVERIFY(!act("type.custom")->isEnabled());    // needs exactly one

        m_a.ctrl->clearSelection();
        m_ra->refreshEnabled();
        QVERIFY(!act("insert.8")->isEnabled());
        QVERIFY(!act("sel.delete")->isEnabled());
        QVERIFY(!act("type.int32")->isEnabled());
        QVERIFY(!act("sel.zero")->isEnabled());
        QVERIFY(act("add.64")->isEnabled());

        // A byte selection alone enables the fills (and Break Class).
        QVERIFY(m_a.editor->setByteSelection(8, 16));
        m_ra->refreshEnabled();
        QVERIFY(m_ra->lastSummary().byteSel);
        QVERIFY(act("sel.zero")->isEnabled());
        QVERIFY(act("sel.ff")->isEnabled());
        QVERIFY(act("sel.random")->isEnabled());
        QVERIFY(act("type.class")->isEnabled());
        m_a.editor->clearByteSelection();

        // Read-only override kills the fills even with a region.
        m_a.click(m_a.h[0]);
        m_a.ctrl->setReadOnlyOverride(true);
        m_ra->refreshEnabled();
        QVERIFY(!act("sel.zero")->isEnabled());
        m_a.ctrl->setReadOnlyOverride(false);

        // Undo/redo follow the stack.
        trig("add.8");
        m_ra->refreshEnabled();
        QVERIFY(act("edit.undo")->isEnabled());
        QVERIFY(!act("edit.redo")->isEnabled());
        trig("edit.undo");
        QCOMPARE(m_a.children().size(), 4);
        m_ra->refreshEnabled();
        QVERIFY(act("edit.redo")->isEnabled());
        trig("edit.redo");
        QCOMPARE(m_a.children().size(), 5);
    }

    // The controller's signals drive a coalesced refresh (no manual call).
    void testScheduledRefreshFollowsSelection() {
        QSignalSpy spy(m_ra, &RibbonActions::refreshed);
        QApplication::processEvents();
        spy.clear();
        QVERIFY(!act("sel.delete")->isEnabled());
        m_a.click(m_a.h[2]);                 // emits selectionChanged
        m_a.click(m_a.h[3], Qt::ControlModifier);
        QCOMPARE(spy.count(), 0);            // coalesced: nothing until the loop turns
        QApplication::processEvents();
        QCOMPARE(spy.count(), 1);            // exactly one refresh for two clicks
        QVERIFY(act("sel.delete")->isEnabled());
        // Undo-stack activity refreshes too (undo/redo enabled state).
        spy.clear();
        trig("add.8");
        QApplication::processEvents();
        QVERIFY(spy.count() >= 1);
        QVERIFY(act("edit.undo")->isEnabled());
    }

    // ── (6) Two controllers: after setActiveController(B) the actions land in B only ──
    void testFollowsActiveController() {
        Fixture b;
        b.create();
        m_target = b.ctrl;
        m_ra->setActiveController(b.ctrl);
        QCOMPARE(m_ra->activeController(), b.ctrl);

        trig("add.64");
        QCOMPARE(b.children().size(), 12);
        QCOMPARE(m_a.children().size(), 4);

        // B's selection drives the predicates, A's doesn't.
        m_a.click(m_a.h[0]);
        QApplication::processEvents();
        QVERIFY(!act("sel.delete")->isEnabled());
        b.click(b.h[0]);
        QApplication::processEvents();
        QVERIFY(act("sel.delete")->isEnabled());

        // Switching back detaches B: its clicks no longer refresh.
        m_target = m_a.ctrl;
        m_ra->setActiveController(m_a.ctrl);
        QApplication::processEvents();
        QSignalSpy spy(m_ra, &RibbonActions::refreshed);
        b.click(b.h[1]);
        QApplication::processEvents();
        QCOMPARE(spy.count(), 0);

        // Destroying the active controller detaches cleanly.
        m_target = b.ctrl;
        m_ra->setActiveController(b.ctrl);
        m_target = nullptr;
        b.destroy();
        QApplication::processEvents();
        QVERIFY(m_ra->activeController() == nullptr);
        QVERIFY(!act("add.64")->isEnabled());   // no document at all
        m_target = m_a.ctrl;
        m_ra->setActiveController(m_a.ctrl);
    }

    // ── (7) pointerSize 4 → the Pointer button relabels and yields Pointer32 ──
    void testPointerSizeFollowsTree() {
        QCOMPARE(act("type.pointer")->data().toInt(), int(NodeKind::Pointer64));
        QVERIFY(act("type.pointer")->toolTip().contains(QStringLiteral("ptr64")));
        m_a.doc->tree.pointerSize = 4;
        m_ra->refreshEnabled();
        QCOMPARE(m_ra->lastSummary().ptrSize, 4);
        QCOMPARE(act("type.pointer")->data().toInt(), int(NodeKind::Pointer32));
        QVERIFY(act("type.pointer")->toolTip().contains(QStringLiteral("ptr32")));
        QCOMPARE(act("type.funcptr")->data().toInt(), int(NodeKind::FuncPtr32));

        m_a.click(m_a.h[0]);
        trig("type.pointer");
        auto ch = m_a.children();
        QCOMPARE(int(m_a.node(m_a.h[0]).kind), int(NodeKind::Pointer32));
        QCOMPARE(ch.size(), 5);
        QCOMPARE(ch[1].offset, 4);
        QCOMPARE(int(ch[1].kind), int(NodeKind::Hex32));   // pad for the freed 4 bytes
    }

    // ── (8) Zero-fill from a ROW selection: bytes 8..15 → 0, one WriteBytes, undo restores ──
    void testZeroFillFromRowSelection() {
        m_a.click(m_a.h[1]);
        QVERIFY(!m_a.editor->hasByteSelection());
        const int before = m_a.undoCount();
        trig("sel.zero");
        QCOMPARE(m_a.undoCount(), before + 1);
        const QByteArray mid = m_a.doc->provider->readBytes(8, 8);
        QCOMPARE(mid, QByteArray(8, '\0'));
        // Neighbours intact.
        QCOMPARE((uchar)m_a.doc->provider->readBytes(7, 1)[0], (uchar)0x17);
        QCOMPARE((uchar)m_a.doc->provider->readBytes(16, 1)[0], (uchar)0x20);
        m_a.doc->undoStack.undo();
        QCOMPARE((uchar)m_a.doc->provider->readBytes(8, 1)[0], (uchar)0x18);
        QCOMPARE((uchar)m_a.doc->provider->readBytes(15, 1)[0], (uchar)0x1F);
    }

    void testFFAndRandomFills() {
        // Byte selection wins over the row selection.
        m_a.click(m_a.h[0]);
        QVERIFY(m_a.editor->setByteSelection(20, 24));
        trig("sel.ff");
        QCOMPARE(m_a.doc->provider->readBytes(20, 4), QByteArray(4, char(0xFF)));
        QCOMPARE((uchar)m_a.doc->provider->readBytes(0, 1)[0], (uchar)0x10);   // h0 untouched
        const int before = m_a.undoCount();
        trig("sel.random");
        QCOMPARE(m_a.undoCount(), before + 1);   // random bytes may collide with FF; only the step must exist
        m_a.doc->undoStack.undo();
        QCOMPARE(m_a.doc->provider->readBytes(20, 4), QByteArray(4, char(0xFF)));
        m_a.doc->undoStack.undo();
        QCOMPARE((uchar)m_a.doc->provider->readBytes(20, 1)[0], (uchar)0x24);
    }

    // Duplicate two fields = ONE undo step, copies interleave after their source.
    void testDuplicateTwoIsOneStep() {
        m_a.click(m_a.h[0]);
        m_a.click(m_a.h[2], Qt::ControlModifier);
        const int before = m_a.undoCount();
        trig("sel.duplicate");
        QCOMPARE(m_a.undoCount(), before + 1);
        auto ch = m_a.children();
        QCOMPARE(ch.size(), 6);
        QCOMPARE(ch[0].id, m_a.h[0]);
        QCOMPARE(ch[1].name, QStringLiteral("h0_copy")); QCOMPARE(ch[1].offset, 8);
        QCOMPARE(ch[2].id, m_a.h[1]);                    QCOMPARE(ch[2].offset, 16);
        QCOMPARE(ch[3].id, m_a.h[2]);                    QCOMPARE(ch[3].offset, 24);
        QCOMPARE(ch[4].name, QStringLiteral("h2_copy")); QCOMPARE(ch[4].offset, 32);
        QCOMPARE(ch[5].id, m_a.h[3]);                    QCOMPARE(ch[5].offset, 40);
        m_a.doc->undoStack.undo();
        QCOMPARE(m_a.children().size(), 4);
    }

    // Array: one leaf → Array[1]; two contiguous → Array[2] occupying both.
    void testMakeArray() {
        m_a.click(m_a.h[3]);
        int before = m_a.undoCount();
        trig("type.array");
        QCOMPARE(m_a.undoCount(), before + 1);
        {
            const Node& n = m_a.node(m_a.h[3]);
            QCOMPARE(int(n.kind), int(NodeKind::Array));
            QCOMPARE(int(n.elementKind), int(NodeKind::Hex64));
            QCOMPARE(n.arrayLen, 1);
            QCOMPARE(n.byteSize(), 8);
        }
        m_a.click(m_a.h[0]);
        m_a.click(m_a.h[1], Qt::ControlModifier);
        before = m_a.undoCount();
        trig("type.array");
        QCOMPARE(m_a.undoCount(), before + 1);
        QVERIFY(!m_a.exists(m_a.h[1]));
        {
            const Node& n = m_a.node(m_a.h[0]);
            QCOMPARE(int(n.kind), int(NodeKind::Array));
            QCOMPARE(int(n.elementKind), int(NodeKind::Hex64));
            QCOMPARE(n.arrayLen, 2);
            QCOMPARE(n.byteSize(), 16);
        }
        auto ch = m_a.children();
        QCOMPARE(ch.size(), 3);
        QCOMPARE(ch[1].id, m_a.h[2]); QCOMPARE(ch[1].offset, 16);   // untouched
        m_a.doc->undoStack.undo();
        QVERIFY(m_a.exists(m_a.h[1]));
        QCOMPARE(int(m_a.node(m_a.h[0]).kind), int(NodeKind::Hex64));

        // Non-contiguous → refused, nothing pushed.
        m_a.click(m_a.h[0]);
        m_a.click(m_a.h[2], Qt::ControlModifier);
        before = m_a.undoCount();
        QVERIFY(!m_a.ctrl->makeArrayFromSelection());
        QCOMPARE(m_a.undoCount(), before);
    }

    // Swap toggles the big-endian flag of every selected scalar in one step.
    void testSwapTogglesBigEndian() {
        m_a.click(m_a.h[0]);
        m_a.click(m_a.h[3], Qt::ControlModifier);
        const int before = m_a.undoCount();
        trig("sel.swap");
        QCOMPARE(m_a.undoCount(), before + 1);
        QVERIFY(m_a.node(m_a.h[0]).bigEndian);
        QVERIFY(!m_a.node(m_a.h[1]).bigEndian);
        QVERIFY(m_a.node(m_a.h[3]).bigEndian);
        trig("sel.swap");
        QVERIFY(!m_a.node(m_a.h[0]).bigEndian);
        QVERIFY(!m_a.node(m_a.h[3]).bigEndian);
    }

    // Big endian is a STATE: the button is checkable and shows whether the
    // selection is currently displayed byte-swapped (it used to be a verb with
    // no feedback at all, so you could not tell which way the next click went).
    void testBigEndianIsCheckable() {
        QVERIFY(act("sel.swap")->isCheckable());
        m_a.click(m_a.h[0]);
        m_a.click(m_a.h[3], Qt::ControlModifier);
        m_ra->refreshEnabled();
        QVERIFY(act("sel.swap")->isEnabled());
        QVERIFY(!act("sel.swap")->isChecked());
        QVERIFY(!m_ra->lastSummary().allBigEndian);
        trig("sel.swap");
        m_ra->refreshEnabled();
        QVERIFY2(act("sel.swap")->isChecked(), "both scalars swapped -> checked");
        QVERIFY(m_ra->lastSummary().allBigEndian);
        // One of the two back to little-endian -> no longer checked.
        m_a.click(m_a.h[0]);
        m_ra->refreshEnabled();
        trig("sel.swap");
        m_a.click(m_a.h[0]);
        m_a.click(m_a.h[3], Qt::ControlModifier);
        m_ra->refreshEnabled();
        QVERIFY(!act("sel.swap")->isChecked());
        QVERIFY(!m_ra->lastSummary().allBigEndian);
    }

    // RTTI used to sit on the Home tab, permanently enabled — it only means
    // anything for exactly one selected pointer-sized field.
    void testRttiEnabledOnlyForOnePointerField() {
        m_ra->refreshEnabled();
        QVERIFY(!act("home.panels.rtti")->isEnabled());        // nothing selected
        m_a.click(m_a.h[0]);
        m_ra->refreshEnabled();
        QVERIFY(act("home.panels.rtti")->isEnabled());         // one 8-byte leaf
        m_a.click(m_a.h[1], Qt::ControlModifier);
        m_ra->refreshEnabled();
        QVERIFY2(!act("home.panels.rtti")->isEnabled(), "two fields have no single vtable");
    }

    // Ptr→Class: two 8-byte leaves become typed pointers to two NEW classes, one step.
    void testPtrToClassTwoLeaves() {
        m_a.click(m_a.h[1]);
        m_a.click(m_a.h[2], Qt::ControlModifier);
        const int before = m_a.undoCount();
        const int rootsBefore = [&] {
            int n = 0;
            for (const auto& nd : m_a.doc->tree.nodes) if (nd.parentId == 0) ++n;
            return n;
        }();
        trig("type.ptrclass");
        QCOMPARE(m_a.undoCount(), before + 1);
        QCOMPARE(int(m_a.node(m_a.h[1]).kind), int(NodeKind::Pointer64));
        QCOMPARE(int(m_a.node(m_a.h[2]).kind), int(NodeKind::Pointer64));
        QVERIFY(m_a.node(m_a.h[1]).refId != 0);
        QVERIFY(m_a.node(m_a.h[2]).refId != 0);
        QVERIFY(m_a.node(m_a.h[1]).refId != m_a.node(m_a.h[2]).refId);
        int rootsAfter = 0;
        for (const auto& nd : m_a.doc->tree.nodes) if (nd.parentId == 0) ++rootsAfter;
        QCOMPARE(rootsAfter, rootsBefore + 2);
        m_a.doc->undoStack.undo();
        QCOMPARE(int(m_a.node(m_a.h[1]).kind), int(NodeKind::Hex64));
        QCOMPARE(m_a.node(m_a.h[1]).refId, uint64_t(0));
    }

    // The editor key path still routes through the same ops (extraction kept behaviour).
    void testKeySignalsShareTheOps() {
        m_a.click(m_a.h[0]);
        m_a.click(m_a.h[1], Qt::ControlModifier);
        int before = m_a.undoCount();
        emit m_a.editor->quickTypeChangeRequested(m_a.doc->tree.indexOfId(m_a.h[0]), NodeKind::UInt64);
        QCOMPARE(m_a.undoCount(), before + 1);
        QCOMPARE(int(m_a.node(m_a.h[0]).kind), int(NodeKind::UInt64));
        QCOMPARE(int(m_a.node(m_a.h[1]).kind), int(NodeKind::UInt64));
        QCOMPARE(m_a.ctrl->selectedIds().size(), 2);

        before = m_a.undoCount();
        emit m_a.editor->duplicateSelectedRequested();
        QCOMPARE(m_a.undoCount(), before + 1);
        QCOMPARE(m_a.children().size(), 6);

        emit m_a.editor->appendBytesRequested(m_a.rootId, 16);
        QCOMPARE(m_a.children().size(), 8);

        m_a.click(m_a.h[0]);
        m_a.click(m_a.h[1], Qt::ControlModifier);
        before = m_a.undoCount();
        emit m_a.editor->deleteSelectedRequested();
        QCOMPARE(m_a.undoCount(), before + 1);
        QVERIFY(!m_a.exists(m_a.h[0]));
        QVERIFY(!m_a.exists(m_a.h[1]));
    }

    // ── Review F1: every trigger is refused while an inline edit is open ──
    // Nothing signals when an edit BEGINS, so the enabled state goes stale;
    // the trigger-time guard must protect the typed text regardless.
    void testTriggersBlockedWhileInlineEditing() {
        // Give h3 a name column to edit (hex rows show an ASCII preview instead).
        m_a.ctrl->changeNodeKind(m_a.doc->tree.indexOfId(m_a.h[3]), NodeKind::UInt64);
        m_a.click(m_a.h[0]);
        QApplication::processEvents();
        QVERIFY(act("type.int32")->isEnabled());
        const int line3 = m_a.lineOf(m_a.h[3]);
        QVERIFY(line3 >= 0);
        QVERIFY(m_a.editor->beginInlineEdit(EditTarget::Name, line3));
        QVERIFY(m_a.editor->isEditing());
        QVERIFY(act("type.int32")->isEnabled());   // stale on purpose — no edit-started signal exists

        QSignalSpy hint(m_ra, &RibbonActions::statusHint);
        const int before = m_a.undoCount();
        act("type.int32")->trigger();
        QCOMPARE(int(m_a.node(m_a.h[0]).kind), int(NodeKind::Hex64));
        QCOMPARE(m_a.undoCount(), before);
        QCOMPARE(hint.count(), 1);
        act("add.8")->trigger();
        QCOMPARE(m_a.children().size(), 4);
        act("edit.undo")->trigger();
        QCOMPARE(int(m_a.node(m_a.h[3]).kind), int(NodeKind::UInt64));   // the retype stayed
        QCOMPARE(hint.count(), 3);
        QVERIFY(m_a.editor->isEditing());          // the edit survived every click

        // An explicit refresh sees the edit…
        m_ra->refreshEnabled();
        QVERIFY(m_ra->lastSummary().editing);
        QVERIFY(!act("type.int32")->isEnabled());
        QVERIFY(!act("add.8")->isEnabled());
        QVERIFY(!act("edit.undo")->isEnabled());
        // …and ending it (editor signal → scheduleRefresh) re-enables without a manual refresh.
        m_a.editor->cancelInlineEdit();
        QVERIFY(!m_a.editor->isEditing());
        QApplication::processEvents();
        QVERIFY(!m_ra->lastSummary().editing);
        QVERIFY(act("type.int32")->isEnabled());
        trig("type.int32");
        QCOMPARE(int(m_a.node(m_a.h[0]).kind), int(NodeKind::Int32));
    }

    // ── Review F2: enums / bitfields take members, never raw Hex64/Hex8 bytes ──
    void testAppendRefusesEnumAndBitfield() {
        NodeTree t;
        t.baseAddress = 0;
        Node e;
        e.kind = NodeKind::Struct;
        e.classKeyword = QStringLiteral("enum");
        e.structTypeName = "E";
        e.name = "e";
        e.collapsed = false;
        e.elementKind = NodeKind::UInt32;
        e.enumMembers.append({QStringLiteral("A"), 0});
        e.enumMembers.append({QStringLiteral("B"), 1});
        t.addNode(e);
        Fixture en;
        en.create(t);
        m_target = en.ctrl;
        m_ra->setActiveController(en.ctrl);
        QVERIFY(m_ra->lastSummary().targetStruct);
        QVERIFY(m_ra->lastSummary().targetIsEnum);
        QVERIFY(!act("add.64")->isEnabled());

        QSignalSpy hint(en.ctrl, &RcxController::statusHint);
        const int before = en.undoCount();
        en.ctrl->appendBytes(0, 64);
        QCOMPARE(en.undoCount(), before);
        QVERIFY(en.doc->tree.childrenOf(en.rootId).isEmpty());
        QCOMPARE(hint.count(), 1);

        // Its footer is not an Insert target either (footer-only → append fallback).
        const int fl = en.lineOf(en.rootId, LineKind::Footer);
        QVERIFY2(fl >= 0, "enum root should compose a footer row");
        en.ctrl->handleNodeClick(en.editor, fl, en.rootId, Qt::NoModifier);
        m_ra->refreshEnabled();
        QVERIFY(m_ra->lastSummary().anyFooter);
        QCOMPARE(m_ra->lastSummary().footerTargetId, uint64_t(0));
        QVERIFY(!act("insert.8")->isEnabled());
        m_target = m_a.ctrl;
        m_ra->setActiveController(m_a.ctrl);
        en.destroy();

        // A bitfield inside the normal class refuses too.
        Node bf;
        bf.kind = NodeKind::Struct;
        bf.classKeyword = QStringLiteral("bitfield");
        bf.name = "flags";
        bf.parentId = m_a.rootId;
        bf.offset = 32;
        bf.elementKind = NodeKind::UInt32;
        BitfieldMember bm;
        bm.name = "a"; bm.bitOffset = 0; bm.bitWidth = 1;
        bf.bitfieldMembers.append(bm);
        const uint64_t bfId = m_a.addNode(bf);
        QSignalSpy hint2(m_a.ctrl, &RcxController::statusHint);
        const int before2 = m_a.undoCount();
        m_a.ctrl->appendBytes(bfId, 8);
        QCOMPARE(m_a.undoCount(), before2);
        QVERIFY(m_a.doc->tree.childrenOf(bfId).isEmpty());
        QCOMPARE(hint2.count(), 1);
    }

    // ── Review F3: an embedded container row can be deleted but not duplicated ──
    void testDuplicateDisabledForContainerRow() {
        Node s;
        s.kind = NodeKind::Struct;
        s.structTypeName = "Inner";
        s.name = "inner";
        s.parentId = m_a.rootId;
        s.offset = 32;
        s.collapsed = false;
        const uint64_t sId = m_a.addNode(s);
        Node x;
        x.kind = NodeKind::Hex32; x.name = "x"; x.parentId = sId; x.offset = 0;
        m_a.addNode(x);

        const int line = m_a.anyLineOf(sId);
        QVERIFY2(line >= 0, "embedded struct should have its own row");
        m_a.ctrl->handleNodeClick(m_a.editor, line, sId, Qt::NoModifier);
        m_ra->refreshEnabled();
        QCOMPARE(m_ra->lastSummary().deletableCount, 1);
        QCOMPARE(m_ra->lastSummary().duplicableCount, 0);
        QVERIFY(act("sel.delete")->isEnabled());
        QVERIFY(!act("sel.duplicate")->isEnabled());

        QSignalSpy hint(m_a.ctrl, &RcxController::statusHint);
        const int before = m_a.undoCount();
        m_a.ctrl->duplicateSelection();
        QCOMPARE(m_a.undoCount(), before);
        QCOMPARE(hint.count(), 1);

        // A leaf next to it is duplicable again.
        m_a.click(m_a.h[0]);
        m_ra->refreshEnabled();
        QCOMPARE(m_ra->lastSummary().duplicableCount, 1);
        QVERIFY(act("sel.duplicate")->isEnabled());
    }

    // ── Review F4: an array-element row deletes its Array — button and key agree ──
    void testArrayElementRowIsDeletable() {
        Node a;
        a.kind = NodeKind::Array;
        a.elementKind = NodeKind::Hex8;
        a.arrayLen = 4;
        a.name = "arr";
        a.parentId = m_a.rootId;
        a.offset = 32;
        a.collapsed = false;
        const uint64_t arrId = m_a.addNode(a);
        const int line = m_a.arrayElemLine(arrId, 1);
        QVERIFY2(line >= 0, "expanded primitive array should compose element rows");
        m_a.ctrl->handleNodeClick(m_a.editor, line, arrId, Qt::NoModifier);
        m_ra->refreshEnabled();
        QVERIFY(m_ra->lastSummary().anyArrayElem);
        QCOMPARE(m_ra->lastSummary().plainCount, 0);
        QCOMPARE(m_ra->lastSummary().deletableCount, 1);
        QVERIFY(act("sel.delete")->isEnabled());
        QVERIFY(!act("type.int32")->isEnabled());

        const int before = m_a.undoCount();
        trig("sel.delete");
        QVERIFY(!m_a.exists(arrId));
        QCOMPARE(m_a.undoCount(), before + 1);
        m_a.doc->undoStack.undo();
        QVERIFY(m_a.exists(arrId));

        // Same through the Delete key.
        const int line2 = m_a.arrayElemLine(arrId, 2);
        QVERIFY(line2 >= 0);
        m_a.ctrl->handleNodeClick(m_a.editor, line2, arrId, Qt::NoModifier);
        emit m_a.editor->deleteSelectedRequested();
        QVERIFY(!m_a.exists(arrId));
    }

    // ── Review F5: several selected footers → the lowest-offset one is the Insert target ──
    void testFooterTargetIsLowestOffset() {
        Node s;
        s.kind = NodeKind::Struct;
        s.structTypeName = "Inner";
        s.name = "inner";
        s.parentId = m_a.rootId;
        s.offset = 32;
        s.collapsed = false;
        const uint64_t sId = m_a.addNode(s);
        Node x;
        x.kind = NodeKind::Hex32; x.name = "x"; x.parentId = sId; x.offset = 0;
        m_a.addNode(x);
        QVERIFY(m_a.lineOf(sId, LineKind::Footer) >= 0);
        QVERIFY(m_a.lineOf(m_a.rootId, LineKind::Footer) >= 0);

        // inner (offset 32) first, then root (offset 0): root wins.
        m_a.clickFooter(sId);
        m_a.clickFooter(m_a.rootId, Qt::ControlModifier);
        m_ra->refreshEnabled();
        QCOMPARE(m_a.ctrl->selectedIds().size(), 2);
        QCOMPARE(m_ra->lastSummary().footerTargetId, m_a.rootId);
        // Reverse click order: same answer.
        m_a.ctrl->clearSelection();
        m_a.clickFooter(m_a.rootId);
        m_a.clickFooter(sId, Qt::ControlModifier);
        m_ra->refreshEnabled();
        QCOMPARE(m_ra->lastSummary().footerTargetId, m_a.rootId);
        // And Insert on that selection appends to the root, not to inner.
        const int innerBefore = m_a.doc->tree.childrenOf(sId).size();
        trig("insert.8");
        QCOMPARE(m_a.doc->tree.childrenOf(sId).size(), innerBefore);
        auto ch = m_a.children();
        QCOMPARE(ch.size(), 6);
        QCOMPARE(int(ch.last().kind), int(NodeKind::Hex64));
    }

    // ── Review F6: Swap only offers kinds the formatter actually byte-swaps ──
    void testSwapRefusesEightBitKinds() {
        QVERIFY(!isEndianSwappable(NodeKind::UInt8));
        QVERIFY(!isEndianSwappable(NodeKind::Int8));
        QVERIFY(!isEndianSwappable(NodeKind::Hex8));
        QVERIFY(!isEndianSwappable(NodeKind::Bool));
        QVERIFY(!isEndianSwappable(NodeKind::Pointer64));
        QVERIFY(!isEndianSwappable(NodeKind::Vec2));
        QVERIFY(isEndianSwappable(NodeKind::UInt16));
        QVERIFY(isEndianSwappable(NodeKind::Int128));
        QVERIFY(isEndianSwappable(NodeKind::Float16));
        QVERIFY(isEndianSwappable(NodeKind::Hex128));

        m_a.ctrl->changeNodeKind(m_a.doc->tree.indexOfId(m_a.h[0]), NodeKind::UInt8);
        m_a.click(m_a.h[0]);
        m_ra->refreshEnabled();
        QVERIFY(!m_ra->lastSummary().anyScalar);
        QVERIFY(!act("sel.swap")->isEnabled());
        QSignalSpy hint(m_a.ctrl, &RcxController::statusHint);
        const int before = m_a.undoCount();
        m_a.ctrl->toggleBigEndianSelection();
        QCOMPARE(m_a.undoCount(), before);
        QCOMPARE(hint.count(), 1);
        QVERIFY(!m_a.node(m_a.h[0]).bigEndian);

        m_a.ctrl->changeNodeKind(m_a.doc->tree.indexOfId(m_a.h[1]), NodeKind::UInt16);
        m_a.click(m_a.h[1]);
        m_ra->refreshEnabled();
        QVERIFY(act("sel.swap")->isEnabled());
        trig("sel.swap");
        QVERIFY(m_a.node(m_a.h[1]).bigEndian);
    }

    // ── Review F7: Custom… on a selected-but-folded field is refused, not misdirected ──
    void testCustomTypeRefusedWhenRowHidden() {
        Node s;
        s.kind = NodeKind::Struct;
        s.structTypeName = "Inner";
        s.name = "inner";
        s.parentId = m_a.rootId;
        s.offset = 32;
        s.collapsed = false;
        const uint64_t sId = m_a.addNode(s);
        Node x;
        x.kind = NodeKind::Hex32; x.name = "x"; x.parentId = sId; x.offset = 0;
        const uint64_t xId = m_a.addNode(x);

        m_a.click(xId);
        m_a.ctrl->toggleCollapse(m_a.doc->tree.indexOfId(sId));   // fold inner: x keeps its selection, loses its row
        QApplication::processEvents();
        QVERIFY(m_a.ctrl->selectedIds().contains(xId));
        QCOMPARE(m_a.lineOf(xId), -1);
        m_ra->refreshEnabled();
        QVERIFY(act("type.custom")->isEnabled());

        QSignalSpy hint(m_ra, &RibbonActions::statusHint);
        act("type.custom")->trigger();
        QCOMPARE(hint.count(), 1);
        QVERIFY(!m_a.editor->isEditing());
    }

    // ── Review F8: Delete on a selected root removes the root (key and button) ──
    // A root class composes no header row of its own (that is the command
    // row), so its footer `}` is the row a user selects to mean "this class"
    // — and the row the pre-ribbon Delete key decoded to the root.
    void testDeleteRootFromFooterRow() {
        QCOMPARE(m_a.lineOf(m_a.rootId, LineKind::Header), -1);   // documents the premise
        m_a.clickFooter(m_a.rootId);
        m_ra->refreshEnabled();
        QVERIFY(m_ra->lastSummary().anyFooter);
        QCOMPARE(m_ra->lastSummary().deletableCount, 1);
        QVERIFY(act("sel.delete")->isEnabled());
        QVERIFY(!act("sel.duplicate")->isEnabled());

        const int before = m_a.undoCount();
        trig("sel.delete");
        QVERIFY(!m_a.exists(m_a.rootId));
        for (uint64_t id : m_a.h) QVERIFY(!m_a.exists(id));
        QCOMPARE(m_a.undoCount(), before + 1);   // deleteRootStruct = one macro
        m_a.doc->undoStack.undo();
        QVERIFY(m_a.exists(m_a.rootId));
        QCOMPARE(m_a.children().size(), 4);

        // The Delete key takes the same path.
        m_a.clickFooter(m_a.rootId);
        emit m_a.editor->deleteSelectedRequested();
        QVERIFY(!m_a.exists(m_a.rootId));
        m_a.doc->undoStack.undo();
        QVERIFY(m_a.exists(m_a.rootId));

        // A typed pointer at another class must not dangle: deleting the
        // pointee root clears its refId (deleteRootStruct, not a bare Remove).
        m_a.click(m_a.h[1]);
        m_a.ctrl->convertToTypedPointer(m_a.h[1]);
        const uint64_t cls = m_a.node(m_a.h[1]).refId;
        QVERIFY(cls != 0);
        m_a.ctrl->setViewRootId(cls);
        QApplication::processEvents();
        m_a.clickFooter(cls);
        emit m_a.editor->deleteSelectedRequested();
        QVERIFY(!m_a.exists(cls));
        QCOMPARE(m_a.node(m_a.h[1]).refId, uint64_t(0));
        QCOMPARE(m_a.ctrl->viewRootId(), m_a.rootId);        // view re-targeted to the surviving root
        m_a.doc->undoStack.undo();
        QVERIFY(m_a.exists(cls));
        QCOMPARE(m_a.node(m_a.h[1]).refId, cls);
        m_a.ctrl->setViewRootId(m_a.rootId);
        QApplication::processEvents();

        // Nothing deletable → a hint, not silence.
        m_a.ctrl->clearSelection();
        QSignalSpy hint(m_a.ctrl, &RcxController::statusHint);
        const int before2 = m_a.undoCount();
        m_a.ctrl->deleteSelection();
        QCOMPARE(hint.count(), 1);
        QCOMPARE(m_a.undoCount(), before2);
    }
};

QTEST_MAIN(TestRibbonActions)
#include "test_ribbon_actions.moc"
