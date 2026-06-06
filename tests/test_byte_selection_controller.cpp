// Integration tests for the controller-side byte-selection handlers.
//
// What's under test (controller.cpp lines 657-810):
//   - byteCopyHexRequested  → reads provider, writes hex to clipboard
//   - bytePasteHexRequested → parses clipboard, pushes cmd::WriteBytes
//   - byteZeroFillRequested → pushes cmd::WriteBytes with zeros
//   - byteRangeCommitRequested(addr, bytes) → pushes cmd::WriteBytes
//   - Read-only override blocks all three write paths
//   - The push lands in the undo stack so Ctrl+Z reverses cleanly
//
// The byte selection itself is driven via the editor's public mouse API;
// the controller test treats the editor as a black box and verifies the
// write side-effects through the provider + undo stack.

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QClipboard>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QSplitter>
#include <QTemporaryDir>
#include <QFile>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>
#include "controller.h"
#include "core.h"
#include "providers/buffer_provider.h"

using namespace rcx;

namespace {

// A NodeTree with 4× Hex32 fields covering 16 bytes of a 32-byte buffer.
// Hex32 chosen so multi-byte tests run within a single row (saves wiring
// up the multi-row mouse-event paths — that's covered in test_byte_selection).
NodeTree buildTree() {
    NodeTree tree;
    // baseAddress = 0 so the BufferProvider (which treats addr as buffer
    // offset) stays in sync with the tree's address space. byteAddrAt
    // returns std::optional now so the byte at virtual address 0 is a
    // legitimate selection target — tests can use byte index 0.
    tree.baseAddress = 0;
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
        n.kind = NodeKind::Hex32;
        n.name = QStringLiteral("h%1").arg(i);
        n.parentId = rootId;
        n.offset = i * 4;
        tree.addNode(n);
    }
    return tree;
}

// Recognizable pattern: byte at offset N has value (N + 0x10).
QByteArray buildBuffer() {
    QByteArray data(32, '\0');
    for (int i = 0; i < data.size(); ++i)
        data[i] = (char)(i + 0x10);
    return data;
}

// Find the document line index for a hex row at `offset` from baseAddr.
int lineForOffset(RcxEditor* ed, uint64_t baseAddr, int offset) {
    for (int i = 0;; ++i) {
        const LineMeta* lm = ed->metaForLine(i);
        if (!lm) return -1;
        if (lm->lineKind == LineKind::Field
            && !lm->isContinuation
            && lm->offsetAddr == baseAddr + (uint64_t)offset)
            return i;
    }
}

QPoint hexByteCoord(RcxEditor* ed, int line, int byteIdx) {
    auto* sci = ed->scintilla();
    const LineMeta* lm = ed->metaForLine(line);
    int len = (int)sci->SendScintilla(
        QsciScintillaBase::SCI_LINELENGTH, (unsigned long)line);
    ColumnSpan vs = RcxEditor::valueSpan(*lm, len,
        lm->effectiveTypeW, lm->effectiveNameW);
    int col = vs.start + byteIdx * 3;
    long pos = sci->SendScintilla(
        QsciScintillaBase::SCI_FINDCOLUMN, (unsigned long)line, (long)col);
    int x = (int)sci->SendScintilla(
        QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, pos);
    int y = (int)sci->SendScintilla(
        QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, pos);
    return QPoint(x + 2, y + 4);
}

void sendPress(QWidget* w, QPoint p) {
    QMouseEvent ev(QEvent::MouseButtonPress, QPointF(p), QPointF(p),
                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}
void sendMove(QWidget* w, QPoint p) {
    QMouseEvent ev(QEvent::MouseMove, QPointF(p), QPointF(p),
                   Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}
void sendRelease(QWidget* w, QPoint p) {
    QMouseEvent ev(QEvent::MouseButtonRelease, QPointF(p), QPointF(p),
                   Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}
void sendKey(QWidget* w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent ev(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(w, &ev);
}

} // namespace

class TestByteSelController : public QObject {
    Q_OBJECT

    RcxDocument* m_doc = nullptr;
    RcxController* m_ctrl = nullptr;
    QSplitter* m_splitter = nullptr;
    RcxEditor* m_editor = nullptr;
    int m_h0Line = -1;

    // Pin a byte selection on row h0 covering 2 bytes [base+1, base+3).
    // Returns true if the selection took.
    bool armSelection(int byteStart, int byteEndIncl) {
        m_editor->clearByteSelection();
        QPoint p0 = hexByteCoord(m_editor, m_h0Line, byteStart);
        QPoint p1 = hexByteCoord(m_editor, m_h0Line, byteEndIncl);
        auto* vp = m_editor->scintilla()->viewport();
        sendPress(vp, p0);
        sendMove(vp, p1);
        sendRelease(vp, p1);
        return m_editor->byteSelection().has_value();
    }

private slots:
    void init() {
        m_doc = new RcxDocument();
        m_doc->tree = buildTree();
        m_doc->provider = std::make_shared<BufferProvider>(
            buildBuffer(), "test_byte_ctrl");
        m_splitter = new QSplitter();
        m_ctrl = new RcxController(m_doc, nullptr);
        m_editor = m_ctrl->addSplitEditor(m_splitter);
        m_splitter->resize(1000, 400);
        m_splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_splitter));
        QApplication::processEvents();
        // Refresh so applyDocument fires on the editor
        m_ctrl->refresh();
        m_h0Line = lineForOffset(m_editor, m_doc->tree.baseAddress, 0);
        QVERIFY(m_h0Line >= 0);
    }

    void cleanup() {
        delete m_ctrl;
        m_ctrl = nullptr;
        m_editor = nullptr;
        delete m_splitter;
        m_splitter = nullptr;
        delete m_doc;
        m_doc = nullptr;
    }

    // ── Ctrl+C copies the selected bytes as uppercase hex to clipboard ─
    void testCtrlCWritesHexToClipboard() {
        QVERIFY(armSelection(1, 2));  // [base+1, base+3) — bytes 0x11, 0x12

        QApplication::clipboard()->clear();
        sendKey(m_editor->scintilla(), Qt::Key_C, Qt::ControlModifier);
        QApplication::processEvents();

        QString clip = QApplication::clipboard()->text();
        QCOMPARE(clip, QStringLiteral("11 12"));
    }

    // ── Ctrl+V parses clipboard hex and writes via cmd::WriteBytes ──
    void testCtrlVWritesToProviderAndUndoStack() {
        QVERIFY(armSelection(1, 2));  // [base+1, base+3) — write 2 bytes

        // Pre-state: bytes 0x11, 0x12
        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0x11);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(2, 1)[0], (uint8_t)0x12);

        QApplication::clipboard()->setText(QStringLiteral("DE AD"));
        int undoBefore = m_doc->undoStack.count();
        sendKey(m_editor->scintilla(), Qt::Key_V, Qt::ControlModifier);
        QApplication::processEvents();

        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0xDE);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(2, 1)[0], (uint8_t)0xAD);
        // Bytes outside selection untouched
        QCOMPARE((uint8_t)m_doc->provider->readBytes(0, 1)[0], (uint8_t)0x10);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(3, 1)[0], (uint8_t)0x13);

        // Undo restores
        m_doc->undoStack.undo();
        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0x11);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(2, 1)[0], (uint8_t)0x12);
    }

    // ── Ctrl+V truncates over-long clipboard to selection size ──
    void testCtrlVTruncatesLongerClipboard() {
        QVERIFY(armSelection(1, 2));  // [base+1, base+3) — 2-byte sel
        // Clipboard carries 4 bytes; only first 2 should land
        QApplication::clipboard()->setText(QStringLiteral("DE AD BE EF"));
        sendKey(m_editor->scintilla(), Qt::Key_V, Qt::ControlModifier);
        QApplication::processEvents();

        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0xDE);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(2, 1)[0], (uint8_t)0xAD);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(3, 1)[0], (uint8_t)0x13);
    }

    // ── Ctrl+V zero-pads under-long clipboard up to selection size ──
    void testCtrlVZeroPadsShorterClipboard() {
        // Drag covers bytes 1..3 inclusive = 3 bytes [base+1, base+4)
        QVERIFY(armSelection(1, 3));
        QApplication::clipboard()->setText(QStringLiteral("DE"));
        sendKey(m_editor->scintilla(), Qt::Key_V, Qt::ControlModifier);
        QApplication::processEvents();

        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0xDE);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(2, 1)[0], (uint8_t)0x00);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(3, 1)[0], (uint8_t)0x00);
    }

    // ── Delete zero-fills the selection via cmd::WriteBytes ─────────
    void testDeleteZeroFills() {
        // Bytes 1..3 inclusive = 3 bytes
        QVERIFY(armSelection(1, 3));
        int undoBefore = m_doc->undoStack.count();

        sendKey(m_editor->scintilla(), Qt::Key_Delete);
        QApplication::processEvents();

        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0x00);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(2, 1)[0], (uint8_t)0x00);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(3, 1)[0], (uint8_t)0x00);
        // Bytes outside selection untouched
        QCOMPARE((uint8_t)m_doc->provider->readBytes(0, 1)[0], (uint8_t)0x10);

        // Undo restores original bytes
        m_doc->undoStack.undo();
        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0x11);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(2, 1)[0], (uint8_t)0x12);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(3, 1)[0], (uint8_t)0x13);
    }

    // ── Backspace also zero-fills (same handler as Delete) ─────────
    void testBackspaceZeroFills() {
        QVERIFY(armSelection(1, 2));
        sendKey(m_editor->scintilla(), Qt::Key_Backspace);
        QApplication::processEvents();
        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0x00);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(2, 1)[0], (uint8_t)0x00);
    }

    // ── Read-only override blocks paste / delete writes ─────────────
    void testReadOnlyOverrideBlocksWrites() {
        m_ctrl->setReadOnlyOverride(true);
        QVERIFY(armSelection(1, 2));

        QApplication::clipboard()->setText(QStringLiteral("DE AD"));
        int undoBefore = m_doc->undoStack.count();
        sendKey(m_editor->scintilla(), Qt::Key_V, Qt::ControlModifier);
        QApplication::processEvents();
        // No write happened, no undo entry pushed
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0x11);

        sendKey(m_editor->scintilla(), Qt::Key_Delete);
        QApplication::processEvents();
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0x11);

        m_ctrl->setReadOnlyOverride(false);
    }

    // ── Clipboard with non-hex garbage produces a status hint, no write ─
    void testGarbageClipboardRefusedWithStatus() {
        QVERIFY(armSelection(1, 2));

        QApplication::clipboard()->setText(QStringLiteral("hello world!"));
        QSignalSpy hints(m_ctrl, &RcxController::statusHint);
        int undoBefore = m_doc->undoStack.count();
        sendKey(m_editor->scintilla(), Qt::Key_V, Qt::ControlModifier);
        QApplication::processEvents();

        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0x11);
        // At least one status hint should have fired about invalid hex
        QVERIFY(hints.count() >= 1);
    }

    // ── Copy-as-C-array writes `{0xDE, 0xAD}` to the clipboard ─────
    void testCopyAsCArray() {
        QVERIFY(armSelection(1, 2));  // 2 bytes [base+1, base+3) — 0x11, 0x12

        QApplication::clipboard()->clear();
        QMetaObject::invokeMethod(m_editor,
            "byteCopyAsCArrayRequested", Qt::DirectConnection);
        QApplication::processEvents();

        QString clip = QApplication::clipboard()->text();
        QCOMPARE(clip, QStringLiteral("{0x11, 0x12}"));
    }

    // ── Copy-as-C-array wraps long selections every 16 bytes ───────
    void testCopyAsCArrayLineWrap() {
        // Whole 4-byte row is too short to wrap. Select all 16 bytes
        // (Ctrl+A across all hex rows) — should produce a single 16-byte
        // line that doesn't break.
        m_editor->clearByteSelection();
        m_editor->scintilla()->setCursorPosition(m_h0Line, 0);
        sendKey(m_editor->scintilla(), Qt::Key_A, Qt::ControlModifier);
        QApplication::processEvents();
        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->second - sel->first, 16ULL);

        QMetaObject::invokeMethod(m_editor,
            "byteCopyAsCArrayRequested", Qt::DirectConnection);
        QApplication::processEvents();

        QString clip = QApplication::clipboard()->text();
        // 16 bytes — no embedded newline (wrap at 16 means the 17th
        // would start a new line, so exactly 16 stays single-line).
        QVERIFY(!clip.contains(QLatin1Char('\n')));
        // First and last byte values present
        QVERIFY(clip.contains(QStringLiteral("0x10")));
        QVERIFY(clip.contains(QStringLiteral("0x1F")));
    }

    // ── Copy-as-Python writes a bytes literal ──────────────────────
    void testCopyAsPython() {
        QVERIFY(armSelection(1, 2));  // 2 bytes 0x11, 0x12

        QApplication::clipboard()->clear();
        QMetaObject::invokeMethod(m_editor,
            "byteCopyAsPythonRequested", Qt::DirectConnection);
        QApplication::processEvents();

        QString clip = QApplication::clipboard()->text();
        QCOMPARE(clip, QStringLiteral("b'\\x11\\x12'"));
    }

    // ── Save selected bytes to a binary file ───────────────────────
    void testWriteSelectedBytesToFile() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QString path = tmp.path() + QStringLiteral("/out.bin");

        // 4 bytes starting at offset 4: should be 0x14, 0x15, 0x16, 0x17
        QString err;
        QVERIFY2(m_ctrl->writeSelectedBytesToFile(4, 4, path, &err),
                 qPrintable(err));

        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray got = f.readAll();
        QCOMPARE(got.size(), 4);
        QCOMPARE((uint8_t)got[0], (uint8_t)0x14);
        QCOMPARE((uint8_t)got[1], (uint8_t)0x15);
        QCOMPARE((uint8_t)got[2], (uint8_t)0x16);
        QCOMPARE((uint8_t)got[3], (uint8_t)0x17);
    }

    void testWriteSelectedBytesToFileRejectsBadPath() {
        QString err;
        // Path to a non-existent directory should fail with a readable error
        bool ok = m_ctrl->writeSelectedBytesToFile(
            0, 4,
            QStringLiteral("/this/dir/does/not/exist/out.bin"),
            &err);
        QVERIFY(!ok);
        QVERIFY2(err.contains(QStringLiteral("Couldn't open")), qPrintable(err));
    }

    void testWriteSelectedBytesToFileRejectsUnreadable() {
        QString err;
        // Address way past buffer size — provider's isReadable says no
        bool ok = m_ctrl->writeSelectedBytesToFile(0x10000, 4,
            QStringLiteral("ignored.bin"), &err);
        QVERIFY(!ok);
        QVERIFY2(err.contains(QStringLiteral("Couldn't read")), qPrintable(err));
    }

    void testWriteSelectedBytesToFileRejectsZeroLength() {
        QString err;
        QVERIFY(!m_ctrl->writeSelectedBytesToFile(0, 0, "ignored.bin", &err));
        QVERIFY(!err.isEmpty());
    }

    // ── byteAddrAt-at-zero regression ─────────────────────────────
    // Before task #6, byteAddrAt returned 0 as the "no byte here"
    // sentinel — so a struct based at virtual address 0 couldn't have
    // its byte 0 selected. This test pins that behaviour: with
    // baseAddress 0, dragging from byte 0 to byte 1 should produce
    // a valid 2-byte selection [0, 2).
    void testByteSelectionWorksAtAddressZero() {
        // baseAddress is 0 in this fixture's buildTree
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);

        QVERIFY(armSelection(0, 1));  // bytes 0 and 1
        auto sel = m_editor->byteSelection();
        QVERIFY(sel.has_value());
        QCOMPARE(sel->first, 0ULL);
        QCOMPARE(sel->second, 2ULL);

        // Sanity: paste + delete writes still hit the right bytes
        QApplication::clipboard()->setText(QStringLiteral("DE AD"));
        sendKey(m_editor->scintilla(), Qt::Key_V, Qt::ControlModifier);
        QApplication::processEvents();
        QCOMPARE((uint8_t)m_doc->provider->readBytes(0, 1)[0], (uint8_t)0xDE);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0xAD);
    }

    // ── The lenient-paste fix: "0x100" produces two bytes, not one ──
    void testPasteSmallLiteralLeftPads() {
        QVERIFY(armSelection(1, 2));  // 2-byte selection
        QApplication::clipboard()->setText(QStringLiteral("0x100"));
        sendKey(m_editor->scintilla(), Qt::Key_V, Qt::ControlModifier);
        QApplication::processEvents();

        // Per parseLenientHex: "100" → left-pad → "0100" → bytes [01, 00]
        QCOMPARE((uint8_t)m_doc->provider->readBytes(1, 1)[0], (uint8_t)0x01);
        QCOMPARE((uint8_t)m_doc->provider->readBytes(2, 1)[0], (uint8_t)0x00);
    }

    // ── Byte selection mirrors into the controller's row selection ──
    // A multi-row byte selection selects every node row it covers (the
    // grey M_SELECTED rows); clearing the byte selection empties the row
    // selection. Guards the byte→row sync: selIdForLine collects each
    // covered row, byteSelectionRowsChanged → onByteSelectionRows mirrors
    // it into m_selIds.
    void testByteSelectionDrivesRowSelection() {
        // Node ids of the first two hex rows (parent-relative offsets 0, 4).
        auto idAtOffset = [&](int off) -> uint64_t {
            for (const auto& n : m_doc->tree.nodes)
                if (n.parentId != 0 && n.offset == off
                    && n.kind == NodeKind::Hex32)
                    return n.id;
            return 0;
        };
        uint64_t id0 = idAtOffset(0), id1 = idAtOffset(4);
        QVERIFY(id0 != 0 && id1 != 0);

        // [0, 8) spans rows h0 (bytes 0..3) and h1 (bytes 4..7).
        QVERIFY(m_editor->setByteSelection(0, 8));
        QApplication::processEvents();

        QSet<uint64_t> sel = m_ctrl->selectedIds();
        QCOMPARE(sel.size(), 2);
        QVERIFY(sel.contains(id0));
        QVERIFY(sel.contains(id1));

        // Shrinking the selection back onto a single row drops the other
        // row from the selection (covered set changes → re-emit).
        QVERIFY(m_editor->setByteSelection(0, 4));
        QApplication::processEvents();
        QCOMPARE(m_ctrl->selectedIds(), (QSet<uint64_t>{id0}));

        // Clearing the byte selection empties the row selection too.
        m_editor->clearByteSelection();
        QApplication::processEvents();
        QVERIFY(m_ctrl->selectedIds().isEmpty());
    }
};

QTEST_MAIN(TestByteSelController)
#include "test_byte_selection_controller.moc"
