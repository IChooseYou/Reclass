// Unit tests for the hex byte-range selection feature in RcxEditor.
//
// What's under test:
//   - byteAddrAt(): click-to-address mapping on hex preview rows
//   - selectAllHexBytes (Ctrl+A): union range over every hex preview row
//   - extendByteSelection (Shift+←/→): keyboard extend / shrink
//   - snapByteSelectionToRow (Shift+↓/↑): jump hi to row boundary
//   - Shift+End / Shift+Home: extend to last hex byte / collapse to anchor
//   - Plain ←/→ on byte selection: no-op (preserves selection on stray keys)
//   - Plain ↓/↑: moves caret for node nav but does NOT clear byte sel
//   - Esc on byte selection: drop selection (first Esc) before node-clear
//   - Ctrl+C / Ctrl+V / Delete: emit byte-{Copy,Paste,ZeroFill}Requested
//   - Ctrl+Shift+C with active sel: copy range as "0xLO..0xHI (N bytes)"
//   - Enter on byte selection: enter hex-overwrite mode (state visible
//     through isEditing() + editEnd() positioning)
//   - setByteSelection(lo, hi): public programmatic setter, rejects empty
//   - clearByteSelection(): drops state cleanly
//   - Hover over hex byte: cursor is I-beam (telegraphs byte-drag mode)
//   - byte selection survives provider refresh (address-based)
//
// Drives the editor through its public surface (mouse events for press +
// drag-upgrade, keyboard events for Ctrl+A etc.) since the byte-selection
// internals are private. Verifies state via the public `byteSelection()`
// accessor and via QSignalSpy on the request signals.

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMouseEvent>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>
#include "editor.h"
#include "core.h"

using namespace rcx;

// ── Test fixture ─────────────────────────────────────────────────────
//
// Tree: 4× Hex64 fields at offsets 0x00..0x18 plus an Int32 at 0x20.
// Base address chosen high enough that byte 0 of the struct is non-zero
// (avoiding the byteAddrAt "0 == no byte" sentinel collision).

static NodeTree makeByteSelTree() {
    NodeTree tree;
    // baseAddress = 0 so the BufferProvider (which treats addr as buffer
    // offset) stays in sync with the tree's address space — needed for
    // the status-hint interp tests that read actual bytes through the
    // provider. The historic byteAddrAt 0-sentinel concern was fixed
    // by the std::optional<uint64_t> refactor (see test_byte_selection_
    // controller.cpp::testByteSelectionWorksAtAddressZero).
    tree.baseAddress = 0;

    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "BS";
    root.name = "bs";
    root.parentId = 0;
    root.offset = 0;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    auto add = [&](int off, NodeKind k, const char* name) {
        Node n; n.kind = k; n.name = name;
        n.parentId = rootId; n.offset = off;
        tree.addNode(n);
    };
    add(0x00, NodeKind::Hex64, "h0");
    add(0x08, NodeKind::Hex64, "h1");
    add(0x10, NodeKind::Hex64, "h2");
    add(0x18, NodeKind::Hex64, "h3");
    add(0x20, NodeKind::Int32, "i");   // non-hex row in the middle of the doc
    return tree;
}

static BufferProvider makeByteSelProvider() {
    QByteArray data(0x40, '\0');
    // Recognizable byte pattern: byte at offset N has value N+0x10
    for (int i = 0; i < data.size(); ++i)
        data[i] = (char)(i + 0x10);
    return BufferProvider(data, "byte_sel.bin");
}

// Find the document line index for a given node by walking m_meta via
// the public metaForLine() accessor. Skips continuation lines and
// non-Field rows.
static int lineForNodeOffset(RcxEditor* ed, uint64_t baseAddr, int offset) {
    for (int i = 0;; ++i) {
        const LineMeta* lm = ed->metaForLine(i);
        if (!lm) return -1;
        if (lm->lineKind == LineKind::Field
            && !lm->isContinuation
            && lm->offsetAddr == baseAddr + (uint64_t)offset)
            return i;
    }
}

// Pixel coord of the first hex digit on a given line (col = value-span start).
static QPoint hexByteCoord(RcxEditor* ed, int line, int byteIdx) {
    auto* sci = ed->scintilla();
    const LineMeta* lm = ed->metaForLine(line);
    Q_ASSERT(lm);
    int len = (int)sci->SendScintilla(
        QsciScintillaBase::SCI_LINELENGTH, (unsigned long)line);
    ColumnSpan vs = RcxEditor::valueSpan(*lm, len,
        lm->effectiveTypeW, lm->effectiveNameW);
    Q_ASSERT(vs.valid);
    // Each byte occupies 3 chars ("XX "); aim for the first digit of byteIdx.
    int col = vs.start + byteIdx * 3;
    long pos = sci->SendScintilla(
        QsciScintillaBase::SCI_FINDCOLUMN, (unsigned long)line, (long)col);
    int x = (int)sci->SendScintilla(
        QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, pos);
    int y = (int)sci->SendScintilla(
        QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, pos);
    // Nudge into the digit (point-from-position lands at left edge of glyph)
    return QPoint(x + 2, y + 4);
}

static void sendPress(QWidget* w, const QPoint& p,
                      Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent ev(QEvent::MouseButtonPress, QPointF(p), QPointF(p),
                   Qt::LeftButton, Qt::LeftButton, mods);
    QApplication::sendEvent(w, &ev);
}
static void sendRelease(QWidget* w, const QPoint& p,
                        Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent ev(QEvent::MouseButtonRelease, QPointF(p), QPointF(p),
                   Qt::LeftButton, Qt::NoButton, mods);
    QApplication::sendEvent(w, &ev);
}
static void sendMove(QWidget* w, const QPoint& p,
                     Qt::MouseButtons buttons = Qt::LeftButton) {
    QMouseEvent ev(QEvent::MouseMove, QPointF(p), QPointF(p),
                   Qt::NoButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}
static void sendKey(QWidget* w, int key,
                    Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(w, &press);
}
static void sendDblClick(QWidget* w, const QPoint& p,
                         Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent ev(QEvent::MouseButtonDblClick, QPointF(p), QPointF(p),
                   Qt::LeftButton, Qt::LeftButton, mods);
    QApplication::sendEvent(w, &ev);
}

class TestByteSelection : public QObject {
    Q_OBJECT

    RcxEditor* m_editor = nullptr;
    NodeTree m_tree;
    BufferProvider m_prov{QByteArray()};
    ComposeResult m_result;
    int m_h0Line = -1;
    int m_h1Line = -1;
    int m_h2Line = -1;
    int m_iLine  = -1;

    void refreshDocument() {
        m_result = compose(m_tree, m_prov);
        m_editor->applyDocument(m_result);
        // Hook the provider into the editor so updateByteSelStatus reads
        // the real bytes from m_prov (needed for the 3/5/6-byte interp
        // tests below). Provider lives for the test's lifetime — safe.
        m_editor->setProviderRef(&m_prov, &m_prov, &m_tree);
        m_h0Line = lineForNodeOffset(m_editor, m_tree.baseAddress, 0x00);
        m_h1Line = lineForNodeOffset(m_editor, m_tree.baseAddress, 0x08);
        m_h2Line = lineForNodeOffset(m_editor, m_tree.baseAddress, 0x10);
        m_iLine  = lineForNodeOffset(m_editor, m_tree.baseAddress, 0x20);
    }

private slots:
    void initTestCase() {
        m_editor = new RcxEditor();
        m_editor->resize(1000, 600);
        m_editor->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_editor));
        m_tree = makeByteSelTree();
        m_prov = makeByteSelProvider();
        refreshDocument();
        QVERIFY(m_h0Line >= 0);
        QVERIFY(m_h1Line == m_h0Line + 1);  // hex rows contiguous in this layout
        QVERIFY(m_h2Line == m_h1Line + 1);
        QVERIFY(m_iLine  > m_h2Line);
    }

    void cleanupTestCase() {
        delete m_editor;
    }

    void init() {
        // Fresh document per test so a previous test's selection / edit
        // state can't leak forward.
        refreshDocument();
        m_editor->clearByteSelection();
        QVERIFY(!m_editor->byteSelection().has_value());
    }

    // ── byteSelection() defaults to nullopt ─────────────────────────
    void testInitialStateEmpty() {
        QVERIFY(!m_editor->byteSelection().has_value());
    }

    // ── Ctrl+A on a hex row selects the union of all hex bytes ──────
    void testCtrlASelectsAllHexBytes() {
        // Put cursor on the first hex row
        m_editor->scintilla()->setCursorPosition(m_h0Line, 0);
        sendKey(m_editor->scintilla(), Qt::Key_A, Qt::ControlModifier);

        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        // Selection should cover the union of all 4 hex rows = 32 bytes.
        QCOMPARE(sel->first, m_tree.baseAddress + 0x00ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 0x20ULL);
    }

    // ── Ctrl+A while not on a hex row falls through to siblings ─────
    // (negative test: byteSelection stays empty)
    void testCtrlAOnNonHexRowDoesntSelectBytes() {
        m_editor->scintilla()->setCursorPosition(m_iLine, 0);
        sendKey(m_editor->scintilla(), Qt::Key_A, Qt::ControlModifier);
        QVERIFY(!m_editor->byteSelection().has_value());
    }

    // ── Mouse press + drag upgrades to byte selection past 8px ──────
    void testMouseDragUpgradesToByteSelection() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, /*byteIdx=*/0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, /*byteIdx=*/3);

        // Verify the test fixture: byte 3 should be ≥ 8 px right of byte 0
        QVERIFY((p1 - p0).manhattanLength() >= 8);

        sendPress(vp, p0);                 // arm anchor
        QVERIFY(!m_editor->byteSelection().has_value());  // not upgraded yet
        sendMove(vp, p1, Qt::LeftButton);  // exceeds 8-px threshold → upgrade
        sendRelease(vp, p1);

        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        // Drag from byte 0 → byte 3 is half-open [base+0, base+4)
        QCOMPARE(sel->first, m_tree.baseAddress + 0ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 4ULL);
    }

    // ── Click-without-drag should NOT create a byte selection ───────
    void testClickWithoutDragDoesntSelectBytes() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p = hexByteCoord(m_editor, m_h0Line, 2);
        sendPress(vp, p);
        sendRelease(vp, p);
        QVERIFY(!m_editor->byteSelection().has_value());
    }

    // ── Press on non-hex row doesn't arm byte-drag ──────────────────
    void testPressOnNonHexRowDoesntArm() {
        auto* vp = m_editor->scintilla()->viewport();
        // Click somewhere in the value column of the Int32 row
        const LineMeta* lm = m_editor->metaForLine(m_iLine);
        int lineLen = (int)m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_LINELENGTH, (unsigned long)m_iLine);
        ColumnSpan vs = RcxEditor::valueSpan(*lm, lineLen,
            lm->effectiveTypeW, lm->effectiveNameW);
        long pos = m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_FINDCOLUMN,
            (unsigned long)m_iLine, (long)(vs.start + 1));
        int x = (int)m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, pos);
        int y = (int)m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, pos);
        QPoint p0(x + 2, y + 4);
        QPoint p1(p0.x() + 40, p0.y());

        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);

        QVERIFY(!m_editor->byteSelection().has_value());
    }

    // ── Drag across rows builds a multi-row selection ───────────────
    void testDragAcrossRows() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 4);  // byte 4 of row 0
        QPoint p1 = hexByteCoord(m_editor, m_h1Line, 2);  // byte 2 of row 1

        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);

        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        // Selection spans [base+4, base+0xB) — 4..7 from row 0 + 0..2 from row 1
        QCOMPARE(sel->first, m_tree.baseAddress + 4ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 0xBULL);
    }

    // ── Shift+Right extends the right edge ──────────────────────────
    void testShiftRightExtends() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 1);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        QVERIFY(m_editor->byteSelection().has_value());

        sendKey(m_editor->scintilla(), Qt::Key_Right, Qt::ShiftModifier);
        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        // Original was [0, 2). Shift+Right grows hi by 1 → [0, 3).
        QCOMPARE(sel->first, m_tree.baseAddress + 0ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 3ULL);
    }

    // ── Shift+Left pulls in the right edge, clamping at 1 byte ──────
    void testShiftLeftShrinksClamped() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 3);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        QVERIFY(m_editor->byteSelection().has_value());

        // Selection [0, 4) — shrink 3 times: → [0, 3), [0, 2), [0, 1)
        for (int i = 0; i < 3; ++i)
            sendKey(m_editor->scintilla(), Qt::Key_Left, Qt::ShiftModifier);
        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->second - sel->first, 1ULL);

        // One more Shift+Left must not shrink below 1 byte
        sendKey(m_editor->scintilla(), Qt::Key_Left, Qt::ShiftModifier);
        sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->second - sel->first, 1ULL);
    }

    // ── Double-click on any hex byte → whole-row byte edit mode ─────
    // Sets selection to the entire row (Hex64 = 8 bytes) and enters
    // hex-overwrite inline edit. beginByteEdit clears m_byteSel after
    // capturing the range, so the surviving witness is isEditing().
    void testDblClickHexByteEntersByteEditMode() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p = hexByteCoord(m_editor, m_h1Line, 3);  // mid-row byte
        sendDblClick(vp, p);
        QApplication::processEvents();

        // Inline edit (hex-overwrite mode) is active.
        QVERIFY(m_editor->isEditing());
    }

    // ── Plain LMB click on a hex byte clears the existing selection ─
    // Single click (no modifier) any LMB press → byte selection drops.
    // If the user drags from there, a fresh selection forms via the
    // drag-upgrade path. This test only exercises the click-without-drag
    // case so the prior selection's clearance is visible at release.
    void testPlainClickOnHexByteClearsSelection() {
        auto* vp = m_editor->scintilla()->viewport();
        // Establish a multi-byte selection [0, 4).
        sendPress  (vp, hexByteCoord(m_editor, m_h0Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, m_h0Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, m_h0Line, 3));
        QVERIFY(m_editor->byteSelection().has_value());

        // Plain click on a DIFFERENT hex byte — no drag.
        QPoint p = hexByteCoord(m_editor, m_h1Line, 2);
        sendPress  (vp, p);
        sendRelease(vp, p);

        QVERIFY(!m_editor->byteSelection().has_value());
    }

    // ── Shift+click still extends instead of clearing ──────────────
    void testShiftClickDoesNotClearSelection() {
        auto* vp = m_editor->scintilla()->viewport();
        sendPress  (vp, hexByteCoord(m_editor, m_h0Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, m_h0Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, m_h0Line, 3));
        QVERIFY(m_editor->byteSelection().has_value());

        QPoint p = hexByteCoord(m_editor, m_h0Line, 6);
        sendPress  (vp, p, Qt::ShiftModifier);
        sendRelease(vp, p, Qt::ShiftModifier);

        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());  // not cleared
        QCOMPARE(sel->first,  m_tree.baseAddress + 0ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 7ULL);  // extended through byte 6
    }

    // ── Plain Right is a no-op (preserves multi-byte selection) ─────
    void testPlainRightIsNoOp() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 3);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);

        sendKey(m_editor->scintilla(), Qt::Key_Right);
        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->first,  m_tree.baseAddress + 0ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 4ULL);
    }

    // ── Plain Left is a no-op (preserves multi-byte selection) ──────
    void testPlainLeftIsNoOp() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 1);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 4);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);

        sendKey(m_editor->scintilla(), Qt::Key_Left);
        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->first,  m_tree.baseAddress + 1ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 5ULL);
    }

    // ── Esc on byte selection clears IT first (before node selection) ─
    void testEscClearsByteSelectionFirst() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 2);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        QVERIFY(m_editor->byteSelection().has_value());

        // nodeClicked spy: first Esc must NOT emit (only drops byte sel)
        QSignalSpy spy(m_editor, &RcxEditor::nodeClicked);
        sendKey(m_editor->scintilla(), Qt::Key_Escape);
        QVERIFY(!m_editor->byteSelection().has_value());
        QCOMPARE(spy.count(), 0);

        // Second Esc clears node selection (emits nodeClicked with nodeId=0)
        sendKey(m_editor->scintilla(), Qt::Key_Escape);
        QCOMPARE(spy.count(), 1);
        QList<QVariant> args = spy.takeLast();
        QCOMPARE(args[1].value<uint64_t>(), 0ULL);
    }

    // ── Ctrl+C emits byteCopyHexRequested ───────────────────────────
    void testCtrlCEmitsCopySignal() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 2);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);

        QSignalSpy spy(m_editor, &RcxEditor::byteCopyHexRequested);
        sendKey(m_editor->scintilla(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(spy.count(), 1);
    }

    // ── Ctrl+V emits bytePasteHexRequested ──────────────────────────
    void testCtrlVEmitsPasteSignal() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 2);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);

        QSignalSpy spy(m_editor, &RcxEditor::bytePasteHexRequested);
        sendKey(m_editor->scintilla(), Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(spy.count(), 1);
    }

    // ── Delete emits byteZeroFillRequested ──────────────────────────
    void testDeleteEmitsZeroFillSignal() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 2);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);

        QSignalSpy spy(m_editor, &RcxEditor::byteZeroFillRequested);
        sendKey(m_editor->scintilla(), Qt::Key_Delete);
        QCOMPARE(spy.count(), 1);
    }

    // ── Backspace also zero-fills (matches Delete) ──────────────────
    void testBackspaceEmitsZeroFillSignal() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 2);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);

        QSignalSpy spy(m_editor, &RcxEditor::byteZeroFillRequested);
        sendKey(m_editor->scintilla(), Qt::Key_Backspace);
        QCOMPARE(spy.count(), 1);
    }

    // ── Enter on byte selection enters hex-overwrite inline edit ────
    void testEnterStartsHexOverwriteEdit() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 2);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        QVERIFY(m_editor->byteSelection().has_value());

        sendKey(m_editor->scintilla(), Qt::Key_Return);
        QVERIFY(m_editor->isEditing());
        // beginByteEdit narrows m_byteSel to nullopt and stashes segments.
        QVERIFY(!m_editor->byteSelection().has_value());
        m_editor->cancelInlineEdit();
        QVERIFY(!m_editor->isEditing());
    }

    // ── clearByteSelection() drops state without firing nodeClicked ──
    void testClearByteSelectionPublicAPI() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 2);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        QVERIFY(m_editor->byteSelection().has_value());

        QSignalSpy spy(m_editor, &RcxEditor::nodeClicked);
        m_editor->clearByteSelection();
        QVERIFY(!m_editor->byteSelection().has_value());
        QCOMPARE(spy.count(), 0);

        // Idempotent
        m_editor->clearByteSelection();
        QVERIFY(!m_editor->byteSelection().has_value());
    }

    // ── Click on a non-byte location clears active byte selection ───
    void testNonByteClickClearsByteSelection() {
        auto* vp = m_editor->scintilla()->viewport();
        // First make a byte selection
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 2);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        QVERIFY(m_editor->byteSelection().has_value());

        // Click far left on the Int32 row (not a hex byte)
        QPoint pNonByte(2, hexByteCoord(m_editor, m_iLine, 0).y());
        sendPress(vp, pNonByte);
        sendRelease(vp, pNonByte);

        QVERIFY(!m_editor->byteSelection().has_value());
    }

    // ── Selection survives a refresh (address-based) ────────────────
    void testSelectionSurvivesRefresh() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h1Line, 1);
        QPoint p1 = hexByteCoord(m_editor, m_h1Line, 5);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);

        auto before = m_editor->byteSelection();
        QVERIFY(before.has_value());
        const uint64_t lo = before->first;
        const uint64_t hi = before->second;

        // Refresh: re-apply the composed document
        m_editor->applyDocument(m_result);

        auto after = m_editor->byteSelection();
        QVERIFY(after.has_value());
        QCOMPARE(after->first, lo);
        QCOMPARE(after->second, hi);
    }

    // ── Ctrl+A toggling: when bytes selected, Ctrl+A expands to all ─
    void testCtrlATogglesFromCurrentToAll() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h1Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h1Line, 2);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);

        auto small = m_editor->byteSelection();
        QVERIFY(small.has_value());
        // Drag from byte 0 to byte 2 inclusive = 3 bytes [0, 3)
        QCOMPARE(small->second - small->first, 3ULL);

        // While byte selection is active, Ctrl+A should grow to full union
        sendKey(m_editor->scintilla(), Qt::Key_A, Qt::ControlModifier);
        auto big = m_editor->byteSelection();
        QVERIFY(big.has_value());
        QCOMPARE(big->first, m_tree.baseAddress + 0ULL);
        QCOMPARE(big->second, m_tree.baseAddress + 0x20ULL);
    }

    // ── Shift+Click on a hex byte extends the selection ─────────────
    void testShiftClickExtendsSelection() {
        auto* vp = m_editor->scintilla()->viewport();
        // Initial drag: byte 0 to byte 1 on row h0 → [base+0, base+2)
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 1);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        auto before = m_editor->byteSelection();
        QVERIFY(before.has_value());
        QCOMPARE(before->second - before->first, 2ULL);

        // Shift+Click on byte 5 of the next row (h1) → extend to [base+0, base+0xE)
        // Anchor = lo = base+0; new endpoint = base+8+5 = base+13;
        // half-open hi = base+14.
        QPoint pExtend = hexByteCoord(m_editor, m_h1Line, 5);
        QMouseEvent shiftPress(QEvent::MouseButtonPress, QPointF(pExtend),
                               QPointF(pExtend),
                               Qt::LeftButton, Qt::LeftButton,
                               Qt::ShiftModifier);
        QApplication::sendEvent(vp, &shiftPress);
        QMouseEvent shiftRelease(QEvent::MouseButtonRelease, QPointF(pExtend),
                                 QPointF(pExtend),
                                 Qt::LeftButton, Qt::NoButton,
                                 Qt::ShiftModifier);
        QApplication::sendEvent(vp, &shiftRelease);

        auto after = m_editor->byteSelection();
        QVERIFY(after.has_value());
        QCOMPARE(after->first, m_tree.baseAddress + 0ULL);
        QCOMPARE(after->second, m_tree.baseAddress + 14ULL);
    }

    // ── Shift+Click BEFORE the anchor flips the selection direction ─
    void testShiftClickBeforeAnchor() {
        auto* vp = m_editor->scintilla()->viewport();
        // Drag bytes 4..6 of h0 → [base+4, base+7)
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 4);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 6);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        QVERIFY(m_editor->byteSelection().has_value());

        // Shift+Click on byte 1 — anchor was 4, click is 1 → flip
        // Selection should become [base+1, base+5) (anchor + 1, half-open)
        QPoint pBack = hexByteCoord(m_editor, m_h0Line, 1);
        QMouseEvent shiftPress(QEvent::MouseButtonPress, QPointF(pBack),
                               QPointF(pBack),
                               Qt::LeftButton, Qt::LeftButton,
                               Qt::ShiftModifier);
        QApplication::sendEvent(vp, &shiftPress);
        QMouseEvent shiftRelease(QEvent::MouseButtonRelease, QPointF(pBack),
                                 QPointF(pBack),
                                 Qt::LeftButton, Qt::NoButton,
                                 Qt::ShiftModifier);
        QApplication::sendEvent(vp, &shiftRelease);

        auto after = m_editor->byteSelection();
        QVERIFY(after.has_value());
        QCOMPARE(after->first, m_tree.baseAddress + 1ULL);
        QCOMPARE(after->second, m_tree.baseAddress + 5ULL);
    }

    // ── Shift+Right past last byte clamps to document end ───────────
    // Regression: previously the selection would silently grow past
    // the last hex byte and the user saw "Shift+Right does nothing".
    void testShiftRightClampsAtDocEnd() {
        auto* vp = m_editor->scintilla()->viewport();
        // Start a tiny selection on the LAST hex row (h3)
        const int h3Line = lineForNodeOffset(m_editor, m_tree.baseAddress, 0x18);
        QVERIFY(h3Line >= 0);
        QPoint p0 = hexByteCoord(m_editor, h3Line, 5);
        QPoint p1 = hexByteCoord(m_editor, h3Line, 7);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        auto before = m_editor->byteSelection();
        QVERIFY(before.has_value());
        // Selection should end exactly at the doc's last byte: base + 0x20
        const uint64_t docEnd = m_tree.baseAddress + 0x20ULL;
        QCOMPARE(before->second, docEnd);

        // Shift+Right 5 times — must NOT grow past docEnd.
        for (int i = 0; i < 5; ++i)
            sendKey(m_editor->scintilla(), Qt::Key_Right, Qt::ShiftModifier);
        auto after = m_editor->byteSelection();
        QVERIFY(after.has_value());
        QCOMPARE(after->second, docEnd);
    }

    // ── Shift+Right grows past row boundary into next hex row ───────
    void testShiftRightCrossesRowBoundary() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 6);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 7);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        // Selection: [base+6, base+8) — last two bytes of h0
        auto before = m_editor->byteSelection();
        QVERIFY(before.has_value());
        QCOMPARE(before->second, m_tree.baseAddress + 8ULL);

        // Shift+Right twice — should extend into h1's first 2 bytes
        sendKey(m_editor->scintilla(), Qt::Key_Right, Qt::ShiftModifier);
        sendKey(m_editor->scintilla(), Qt::Key_Right, Qt::ShiftModifier);

        auto after = m_editor->byteSelection();
        QVERIFY(after.has_value());
        QCOMPARE(after->first, m_tree.baseAddress + 6ULL);
        QCOMPARE(after->second, m_tree.baseAddress + 0xAULL);
    }

    // ── Hover over a hex byte sets I-beam cursor ─────────────────────
    // Telegraphs "drag here = byte selection" before the user presses.
    // applyHoverCursor() resolves byteAddrAt() the same way the press
    // handler does, so the cursor matches the press behaviour for free.
    void testHoverHexByteSetsIBeamCursor() {
        auto* vp = m_editor->scintilla()->viewport();
        QPoint hex = hexByteCoord(m_editor, m_h0Line, 1);
        sendMove(vp, hex, Qt::NoButton);
        QApplication::processEvents();
        QCOMPARE(vp->cursor().shape(), Qt::IBeamCursor);
    }

    // ── Status-bar interp for odd-sized selections ─────────────────
    //
    // Captures the last statusHintRequested string after a drag, so we
    // can assert the interpretation labels for 3/5/6/7-byte selections.
    QString lastStatusHint(QSignalSpy& spy) {
        if (spy.isEmpty()) return {};
        return spy.last().at(0).toString();
    }

    void testStatus3ByteShowsRgbAndInt24() {
        auto* vp = m_editor->scintilla()->viewport();
        QSignalSpy spy(m_editor, &RcxEditor::statusHintRequested);
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 2);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        // Bytes at offset 0..2 are 0x10 0x11 0x12 (test buffer pattern).
        // LE int24 = 0x121110, BE int24 = 0x101112.
        QString hint = lastStatusHint(spy);
        QVERIFY2(hint.contains(QStringLiteral("0x121110")), qPrintable(hint));
        QVERIFY2(hint.contains(QStringLiteral("BE 0x101112")), qPrintable(hint));
        QVERIFY2(hint.contains(QStringLiteral("rgb #101112")), qPrintable(hint));
        QVERIFY2(hint.contains(QStringLiteral("i24=")), qPrintable(hint));
    }

    void testStatus6BytesShowsMac() {
        // Drag 6 bytes — must surface the MAC-address shortcut.
        auto* vp = m_editor->scintilla()->viewport();
        QSignalSpy spy(m_editor, &RcxEditor::statusHintRequested);
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 5);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        QString hint = lastStatusHint(spy);
        // Buffer bytes 0..5 are 10..15. MAC formatted as 10:11:12:13:14:15.
        QVERIFY2(hint.contains(QStringLiteral("mac 10:11:12:13:14:15")),
                 qPrintable(hint));
    }

    void testStatus5BytesShowsInt40() {
        auto* vp = m_editor->scintilla()->viewport();
        QSignalSpy spy(m_editor, &RcxEditor::statusHintRequested);
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, 0);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, 4);
        sendPress(vp, p0);
        sendMove(vp, p1, Qt::LeftButton);
        sendRelease(vp, p1);
        QString hint = lastStatusHint(spy);
        // 5-byte selection → i40 label
        QVERIFY2(hint.contains(QStringLiteral("i40=")), qPrintable(hint));
        // LE 0x1413121110 (bytes 10,11,12,13,14)
        QVERIFY2(hint.contains(QStringLiteral("0x1413121110")), qPrintable(hint));
    }

    // ── Shift+Down snaps hi to end of current hex row ───────────────
    // Starting from a sub-row selection [0, 4) on h0 (Hex64), a single
    // Shift+Down should jump hi to the end of h0 = byte 8.
    void testShiftDownSnapsToEndOfCurrentRow() {
        auto* vp = m_editor->scintilla()->viewport();
        sendPress  (vp, hexByteCoord(m_editor, m_h0Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, m_h0Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, m_h0Line, 3));

        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->second, m_tree.baseAddress + 4ULL);

        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::ShiftModifier);

        sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->first,  m_tree.baseAddress + 0ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 8ULL);
    }

    // ── Shift+Down a second time crosses into next hex row ──────────
    // From [0, 8) (entire h0), Shift+Down jumps to end of h1 = byte 16.
    void testShiftDownAdvancesToNextRow() {
        auto* vp = m_editor->scintilla()->viewport();
        sendPress  (vp, hexByteCoord(m_editor, m_h0Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, m_h0Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, m_h0Line, 3));
        // [0, 4) → Shift+Down → [0, 8) → Shift+Down → [0, 16)
        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::ShiftModifier);
        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::ShiftModifier);

        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->first,  m_tree.baseAddress + 0ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 16ULL);
    }

    // ── Shift+Down across a non-hex row is a no-op ──────────────────
    // h3 is the LAST hex preview row (offset 0x18..0x20); the next
    // visible field is an Int32 at 0x20. From end-of-h3 a Shift+Down
    // should NOT cross into the int.
    void testShiftDownStopsAtNonHexRow() {
        auto* vp = m_editor->scintilla()->viewport();
        int h3Line = lineForNodeOffset(m_editor, m_tree.baseAddress, 0x18);
        QVERIFY(h3Line >= 0);

        // Build [0x18, 0x18+4) by dragging on h3 from byte 0 to byte 3,
        // then snap to end of row → [0x18, 0x20).
        sendPress  (vp, hexByteCoord(m_editor, h3Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, h3Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, h3Line, 3));
        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::ShiftModifier);  // → [0x18, 0x20)

        auto before = m_editor->byteSelection();
        QVERIFY(before.has_value());
        QCOMPARE(before->second, m_tree.baseAddress + 0x20ULL);

        // Second Shift+Down should be a no-op — next row is non-hex.
        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::ShiftModifier);
        auto after = m_editor->byteSelection();
        QVERIFY(after.has_value());
        QCOMPARE(after->first,  before->first);
        QCOMPARE(after->second, before->second);
    }

    // ── Shift+Up shrinks hi back to start of current row ────────────
    // From [0, 8) (entire h0), Shift+Up snaps hi to row start, clamped
    // at lo+1 = 1. Result: [0, 1).
    void testShiftUpShrinksToStartOfCurrentRow() {
        auto* vp = m_editor->scintilla()->viewport();
        sendPress  (vp, hexByteCoord(m_editor, m_h0Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, m_h0Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, m_h0Line, 3));
        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::ShiftModifier);  // → [0, 8)

        sendKey(m_editor->scintilla(), Qt::Key_Up, Qt::ShiftModifier);

        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->first,  m_tree.baseAddress + 0ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 1ULL);
    }

    // ── Shift+Up walks back across hex rows ─────────────────────────
    // From [0, 16) (two hex rows), Shift+Up snaps to [0, 8), then to
    // [0, 1) (clamped at lo+1).
    void testShiftUpWalksBackAcrossRows() {
        auto* vp = m_editor->scintilla()->viewport();
        sendPress  (vp, hexByteCoord(m_editor, m_h0Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, m_h0Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, m_h0Line, 3));
        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::ShiftModifier);  // [0, 8)
        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::ShiftModifier);  // [0, 16)

        sendKey(m_editor->scintilla(), Qt::Key_Up, Qt::ShiftModifier);
        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->second, m_tree.baseAddress + 8ULL);

        sendKey(m_editor->scintilla(), Qt::Key_Up, Qt::ShiftModifier);
        sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->second, m_tree.baseAddress + 1ULL);
    }

    // ── Public setter accepts valid ranges, rejects inverted ones ──
    void testSetByteSelectionAcceptsValidRange() {
        QVERIFY(m_editor->setByteSelection(m_tree.baseAddress + 4,
                                            m_tree.baseAddress + 12));
        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->first,  m_tree.baseAddress + 4ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 12ULL);
    }

    void testSetByteSelectionRejectsInverted() {
        QVERIFY(!m_editor->setByteSelection(10, 5));
        QVERIFY(!m_editor->setByteSelection(10, 10));  // empty
    }

    // ── Plain Down/Up moves cursor but preserves byte selection ────
    // Stray keys must not destroy a multi-byte selection. Plain ↓/↑
    // are used for node navigation, but the byte highlight must
    // survive the cursor move.
    void testPlainDownUpPreservesByteSelection() {
        auto* vp = m_editor->scintilla()->viewport();
        sendPress  (vp, hexByteCoord(m_editor, m_h0Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, m_h0Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, m_h0Line, 3));

        auto before = m_editor->byteSelection();
        QVERIFY(before.has_value());

        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::NoModifier);
        auto afterDown = m_editor->byteSelection();
        QVERIFY(afterDown.has_value());
        QCOMPARE(afterDown->first,  before->first);
        QCOMPARE(afterDown->second, before->second);

        sendKey(m_editor->scintilla(), Qt::Key_Up, Qt::NoModifier);
        auto afterUp = m_editor->byteSelection();
        QVERIFY(afterUp.has_value());
        QCOMPARE(afterUp->first,  before->first);
        QCOMPARE(afterUp->second, before->second);
    }

    // ── Ctrl+Shift+C with byte sel active copies the range ─────────
    void testCtrlShiftCCopiesByteSelRange() {
        auto* vp = m_editor->scintilla()->viewport();
        sendPress  (vp, hexByteCoord(m_editor, m_h0Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, m_h0Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, m_h0Line, 3));

        QApplication::clipboard()->clear();
        sendKey(m_editor->scintilla(), Qt::Key_C,
                Qt::ControlModifier | Qt::ShiftModifier);
        QString cb = QApplication::clipboard()->text();
        QVERIFY(cb.contains("0x0"));
        QVERIFY(cb.contains("0x4"));
        QVERIFY(cb.contains("4 bytes"));
    }

    // ── Plain Right/Left with multi-byte selection is a no-op ──────
    // Stray arrow key absorbed; multi-byte selection stays intact.
    void testPlainArrowDoesNotCollapseSelection() {
        auto* vp = m_editor->scintilla()->viewport();
        sendPress  (vp, hexByteCoord(m_editor, m_h0Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, m_h0Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, m_h0Line, 3));

        auto before = m_editor->byteSelection();
        QVERIFY(before.has_value());
        QCOMPARE(before->first,  m_tree.baseAddress + 0ULL);
        QCOMPARE(before->second, m_tree.baseAddress + 4ULL);

        sendKey(m_editor->scintilla(), Qt::Key_Right, Qt::NoModifier);
        auto afterRight = m_editor->byteSelection();
        QVERIFY(afterRight.has_value());
        QCOMPARE(afterRight->first,  before->first);
        QCOMPARE(afterRight->second, before->second);

        sendKey(m_editor->scintilla(), Qt::Key_Left, Qt::NoModifier);
        auto afterLeft = m_editor->byteSelection();
        QVERIFY(afterLeft.has_value());
        QCOMPARE(afterLeft->first,  before->first);
        QCOMPARE(afterLeft->second, before->second);
    }

    // ── Shift+End extends selection to last hex byte in document ────
    // Fixture has 4× Hex64 rows ending at byte 0x20; from [0, 4) on h0
    // Shift+End should jump hi to 0x20 (covers all hex bytes).
    void testShiftEndExtendsToDocEnd() {
        auto* vp = m_editor->scintilla()->viewport();
        sendPress  (vp, hexByteCoord(m_editor, m_h0Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, m_h0Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, m_h0Line, 3));

        sendKey(m_editor->scintilla(), Qt::Key_End, Qt::ShiftModifier);

        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->first,  m_tree.baseAddress + 0ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 0x20ULL);
    }

    // ── Shift+Home collapses selection back to anchor ───────────────
    // From [0, 16) (spans h0 + h1), Shift+Home shrinks hi to lo+1 = 1.
    void testShiftHomeCollapsesToAnchor() {
        auto* vp = m_editor->scintilla()->viewport();
        sendPress  (vp, hexByteCoord(m_editor, m_h0Line, 0));
        sendMove   (vp, hexByteCoord(m_editor, m_h0Line, 3), Qt::LeftButton);
        sendRelease(vp, hexByteCoord(m_editor, m_h0Line, 3));
        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::ShiftModifier);  // [0, 8)
        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::ShiftModifier);  // [0, 16)

        sendKey(m_editor->scintilla(), Qt::Key_Home, Qt::ShiftModifier);

        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->first,  m_tree.baseAddress + 0ULL);
        QCOMPARE(sel->second, m_tree.baseAddress + 1ULL);
    }

    // ── Shift+Down without a byte selection falls through to node nav ─
    // No m_byteSel → handler returns false → Scintilla's natural
    // cursor movement runs (which triggers node multi-select). The
    // byte-selection state remains empty.
    void testShiftDownWithoutByteSelectionIsUntouched() {
        QVERIFY(!m_editor->byteSelection().has_value());
        m_editor->scintilla()->setCursorPosition(m_h0Line, 0);
        sendKey(m_editor->scintilla(), Qt::Key_Down, Qt::ShiftModifier);
        QVERIFY(!m_editor->byteSelection().has_value());
    }
};

QTEST_MAIN(TestByteSelection)
#include "test_byte_selection.moc"
