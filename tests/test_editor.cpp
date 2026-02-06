#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QFile>
#include <Qsci/qsciscintilla.h>
#include "editor.h"
#include "core.h"

using namespace rcx;

// Load first 0x6000 bytes of the test exe for realistic data
static FileProvider makeTestProvider() {
    QFile exe(QCoreApplication::applicationFilePath());
    if (exe.open(QIODevice::ReadOnly)) {
        QByteArray data = exe.read(0x6000);
        exe.close();
        if (data.size() >= 0x6000)
            return FileProvider(data);
    }
    // Fallback: minimal PE header stub
    QByteArray data(0x6000, '\0');
    data[0] = 'M'; data[1] = 'Z';  // DOS signature
    return FileProvider(data);
}

// Build a PE-like test tree with IMAGE_FILE_HEADER fields
static NodeTree makeTestTree() {
    NodeTree tree;
    tree.baseAddress = 0x140000000;

    // Root struct: IMAGE_FILE_HEADER
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "IMAGE_FILE_HEADER";
    root.name = "FileHeader";
    root.parentId = 0;
    root.offset = 0;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    int offset = 0;

    // IMAGE_FILE_HEADER fields (matches Windows PE format)
    Node machine;
    machine.kind = NodeKind::UInt16;
    machine.name = "Machine";
    machine.parentId = rootId;
    machine.offset = offset;
    tree.addNode(machine);
    offset += 2;

    Node numSections;
    numSections.kind = NodeKind::UInt16;
    numSections.name = "NumberOfSections";
    numSections.parentId = rootId;
    numSections.offset = offset;
    tree.addNode(numSections);
    offset += 2;

    Node timestamp;
    timestamp.kind = NodeKind::Hex32;
    timestamp.name = "TimeDateStamp";
    timestamp.parentId = rootId;
    timestamp.offset = offset;
    tree.addNode(timestamp);
    offset += 4;

    Node ptrSymbols;
    ptrSymbols.kind = NodeKind::Hex32;
    ptrSymbols.name = "PointerToSymbolTable";
    ptrSymbols.parentId = rootId;
    ptrSymbols.offset = offset;
    tree.addNode(ptrSymbols);
    offset += 4;

    Node numSymbols;
    numSymbols.kind = NodeKind::UInt32;
    numSymbols.name = "NumberOfSymbols";
    numSymbols.parentId = rootId;
    numSymbols.offset = offset;
    tree.addNode(numSymbols);
    offset += 4;

    Node optHeaderSize;
    optHeaderSize.kind = NodeKind::UInt16;
    optHeaderSize.name = "SizeOfOptionalHeader";
    optHeaderSize.parentId = rootId;
    optHeaderSize.offset = offset;
    tree.addNode(optHeaderSize);
    offset += 2;

    Node characteristics;
    characteristics.kind = NodeKind::Hex16;
    characteristics.name = "Characteristics";
    characteristics.parentId = rootId;
    characteristics.offset = offset;
    tree.addNode(characteristics);
    offset += 2;

    // 8 Hex64 fields for additional test coverage
    for (int i = 0; i < 8; i++) {
        Node hex;
        hex.kind = NodeKind::Hex64;
        hex.name = QString("Reserved%1").arg(i);
        hex.parentId = rootId;
        hex.offset = offset;
        tree.addNode(hex);
        offset += 8;
    }

    return tree;
}

class TestEditor : public QObject {
    Q_OBJECT
private:
    RcxEditor* m_editor = nullptr;
    ComposeResult m_result;

private slots:
    void initTestCase() {
        m_editor = new RcxEditor();
        m_editor->resize(800, 600);
        m_editor->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_editor));

        NodeTree tree = makeTestTree();
        FileProvider prov = makeTestProvider();
        m_result = compose(tree, prov);
        m_editor->applyDocument(m_result);
    }

    void cleanupTestCase() {
        delete m_editor;
    }

    // ── Test: CommandRow at line 0 rejects non-ADDR edits ──
    void testCommandRowLineRejectsEdits() {
        m_editor->applyDocument(m_result);

        // Line 0 should be the CommandRow
        const LineMeta* lm = m_editor->metaForLine(0);
        QVERIFY(lm);
        QCOMPARE(lm->lineKind, LineKind::CommandRow);
        QCOMPARE(lm->nodeId, kCommandRowId);
        QCOMPARE(lm->nodeIdx, -1);

        // Type/Name/Value should be rejected on CommandRow
        QVERIFY(!m_editor->beginInlineEdit(EditTarget::Type, 0));
        QVERIFY(!m_editor->beginInlineEdit(EditTarget::Name, 0));
        QVERIFY(!m_editor->beginInlineEdit(EditTarget::Value, 0));
        QVERIFY(!m_editor->isEditing());

        // Set CommandRow text with an ADDR value (simulates controller.updateCommandRow)
        m_editor->setCommandRowText(
            QStringLiteral(" * SRC: File : 0x140000000"));

        // BaseAddress should be ALLOWED on CommandRow (ADDR field)
        bool ok = m_editor->beginInlineEdit(EditTarget::BaseAddress, 0);
        QVERIFY2(ok, "BaseAddress edit should be allowed on CommandRow");
        QVERIFY(m_editor->isEditing());
        m_editor->cancelInlineEdit();

        // Source should be ALLOWED on CommandRow (SRC field)
        ok = m_editor->beginInlineEdit(EditTarget::Source, 0);
        QVERIFY2(ok, "Source edit should be allowed on CommandRow");
        QVERIFY(m_editor->isEditing());
        m_editor->cancelInlineEdit();
    }

    // ── Test: inline edit lifecycle (begin → commit → re-edit) ──
    void testInlineEditReEntry() {
        // Move cursor to line 2 (first field inside struct; line 0=CommandRow, 1=header)
        m_editor->scintilla()->setCursorPosition(2, 0);

        // Should not be editing
        QVERIFY(!m_editor->isEditing());

        // Begin edit on Name column
        bool ok = m_editor->beginInlineEdit(EditTarget::Name, 2);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        // Cancel the edit
        m_editor->cancelInlineEdit();
        QVERIFY(!m_editor->isEditing());

        // Re-apply document (simulates controller refresh)
        m_editor->applyDocument(m_result);

        // Should be able to edit again
        ok = m_editor->beginInlineEdit(EditTarget::Name, 2);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        // Cancel again
        m_editor->cancelInlineEdit();
        QVERIFY(!m_editor->isEditing());
    }

    // ── Test: commit inline edit then re-edit same line ──
    void testCommitThenReEdit() {
        m_editor->applyDocument(m_result);
        m_editor->scintilla()->setCursorPosition(2, 0);

        // Begin value edit
        bool ok = m_editor->beginInlineEdit(EditTarget::Value, 2);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        // Simulate Enter key → commit (via signal spy)
        QSignalSpy spy(m_editor, &RcxEditor::inlineEditCommitted);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(m_editor->scintilla(), &enter);

        // Should have emitted commit signal and exited edit mode
        QCOMPARE(spy.count(), 1);
        QVERIFY(!m_editor->isEditing());

        // Re-apply document (simulates refresh)
        m_editor->applyDocument(m_result);

        // Must be able to edit the same line again
        ok = m_editor->beginInlineEdit(EditTarget::Value, 2);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        m_editor->cancelInlineEdit();
    }

    // ── Test: mouse click during edit commits it ──
    void testMouseClickCommitsEdit() {
        m_editor->applyDocument(m_result);

        bool ok = m_editor->beginInlineEdit(EditTarget::Name, 2);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        // Simulate mouse click on viewport — should commit (save), not cancel
        QSignalSpy commitSpy(m_editor, &RcxEditor::inlineEditCommitted);
        QSignalSpy cancelSpy(m_editor, &RcxEditor::inlineEditCancelled);
        QMouseEvent click(QEvent::MouseButtonPress, QPointF(10, 10),
                          QPointF(10, 10), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_editor->scintilla()->viewport(), &click);

        QVERIFY(!m_editor->isEditing());
        QCOMPARE(commitSpy.count(), 1);
        QCOMPARE(cancelSpy.count(), 0);
    }

    // ── Test: FocusOut during edit commits it ──
    void testFocusOutCommitsEdit() {
        m_editor->applyDocument(m_result);

        // Give focus to the scintilla widget first
        m_editor->scintilla()->setFocus();
        QApplication::processEvents();

        bool ok = m_editor->beginInlineEdit(EditTarget::Name, 2);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        QSignalSpy commitSpy(m_editor, &RcxEditor::inlineEditCommitted);
        QSignalSpy cancelSpy(m_editor, &RcxEditor::inlineEditCancelled);

        // Create a dummy widget and transfer focus to it (triggers real FocusOut)
        QWidget dummy;
        dummy.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dummy));
        dummy.setFocus();
        QApplication::processEvents();  // process focus change + deferred timer

        QVERIFY(!m_editor->isEditing());
        QCOMPARE(commitSpy.count(), 1);
        QCOMPARE(cancelSpy.count(), 0);

        // Restore focus to editor for subsequent tests
        m_editor->scintilla()->setFocus();
        QApplication::processEvents();
    }

    // ── Test: type edit begins and can be cancelled ──
    void testTypeEditCancel() {
        m_editor->applyDocument(m_result);

        // Begin type edit on a field line
        bool ok = m_editor->beginInlineEdit(EditTarget::Type, 2);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        // Process deferred events (showTypeAutocomplete is deferred via QTimer)
        QApplication::processEvents();

        // First Escape closes autocomplete popup (if active) or cancels edit
        QKeyEvent esc1(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(m_editor->scintilla(), &esc1);

        // If autocomplete was open, first Esc only closed popup; need second Esc
        if (m_editor->isEditing()) {
            QKeyEvent esc2(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(m_editor->scintilla(), &esc2);
        }
        QVERIFY(!m_editor->isEditing());
    }

    // ── Test: edit on header line (Name and Type valid, Value invalid) ──
    void testHeaderLineEdit() {
        m_editor->applyDocument(m_result);

        // Line 1 should be the struct header (line 0 is CommandRow)
        const LineMeta* lm = m_editor->metaForLine(1);
        QVERIFY(lm);
        QCOMPARE(lm->lineKind, LineKind::Header);

        // Type edit on header should succeed (has typename IMAGE_FILE_HEADER)
        bool ok = m_editor->beginInlineEdit(EditTarget::Type, 1);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());
        m_editor->cancelInlineEdit();

        // Name edit on header should succeed
        ok = m_editor->beginInlineEdit(EditTarget::Name, 1);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());
        m_editor->cancelInlineEdit();
    }

    // ── Test: footer line rejects all edits ──
    void testFooterLineEdit() {
        m_editor->applyDocument(m_result);

        // Find the footer line
        int footerLine = -1;
        for (int i = 0; i < m_result.meta.size(); i++) {
            if (m_result.meta[i].lineKind == LineKind::Footer) {
                footerLine = i;
                break;
            }
        }
        QVERIFY(footerLine >= 0);

        QVERIFY(!m_editor->beginInlineEdit(EditTarget::Type, footerLine));
        QVERIFY(!m_editor->beginInlineEdit(EditTarget::Name, footerLine));
        QVERIFY(!m_editor->beginInlineEdit(EditTarget::Value, footerLine));
        QVERIFY(!m_editor->isEditing());
    }

    // ── Test: showTypeAutocomplete populates list (check via SCI_AUTOCACTIVE) ──
    void testTypeAutocompleteShows() {
        m_editor->applyDocument(m_result);

        bool ok = m_editor->beginInlineEdit(EditTarget::Type, 2);
        QVERIFY(ok);

        // Process deferred timer (autocomplete is deferred)
        QApplication::processEvents();

        // Check if the user list is active
        long active = m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_AUTOCACTIVE);
        QVERIFY2(active != 0, "Autocomplete list should be active after type edit begins");

        // Cancel
        m_editor->cancelInlineEdit();
        m_editor->applyDocument(m_result);
    }

    // ── Test: parseValue accepts space-separated hex bytes ──
    void testParseValueHexWithSpaces() {
        bool ok;

        // Hex8 with spaces (single byte, but test the .remove(' '))
        QByteArray b = fmt::parseValue(NodeKind::Hex8, "4D", &ok);
        QVERIFY(ok);
        QCOMPARE((uint8_t)b[0], (uint8_t)0x4D);

        // Hex32 with space-separated bytes (raw byte order, no endian conversion)
        b = fmt::parseValue(NodeKind::Hex32, "DE AD BE EF", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 4);
        QCOMPARE((uint8_t)b[0], (uint8_t)0xDE);
        QCOMPARE((uint8_t)b[1], (uint8_t)0xAD);
        QCOMPARE((uint8_t)b[2], (uint8_t)0xBE);
        QCOMPARE((uint8_t)b[3], (uint8_t)0xEF);

        // Hex64 with space-separated bytes
        b = fmt::parseValue(NodeKind::Hex64, "4D 5A 90 00 00 00 00 00", &ok);
        QVERIFY(ok);
        QCOMPARE(b.size(), 8);
        QCOMPARE((uint8_t)b[0], (uint8_t)0x4D);
        QCOMPARE((uint8_t)b[1], (uint8_t)0x5A);
        QCOMPARE((uint8_t)b[7], (uint8_t)0x00);

        // Hex64 continuous - stores as native-endian (numeric value preserved)
        b = fmt::parseValue(NodeKind::Hex64, "4D5A900000000000", &ok);
        QVERIFY(ok);
        uint64_t v64;
        memcpy(&v64, b.data(), 8);
        QCOMPARE(v64, (uint64_t)0x4D5A900000000000);

        // Hex64 with 0x prefix and spaces
        b = fmt::parseValue(NodeKind::Hex64, "0x4D 5A 90 00 00 00 00 00", &ok);
        QVERIFY(ok);
    }

    // ── Test: type autocomplete accepts typed input and commits ──
    void testTypeAutocompleteTypingAndCommit() {
        m_editor->applyDocument(m_result);

        bool ok = m_editor->beginInlineEdit(EditTarget::Type, 2);
        QVERIFY(ok);

        // Process deferred autocomplete
        QApplication::processEvents();

        // Verify autocomplete is active
        long active = m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_AUTOCACTIVE);
        QVERIFY2(active != 0, "Autocomplete should be active");

        // Simulate typing 'i' — filters to typeName entries starting with 'i'
        QKeyEvent keyI(QEvent::KeyPress, Qt::Key_I, Qt::NoModifier, "i");
        QApplication::sendEvent(m_editor->scintilla(), &keyI);

        // Still editing
        QVERIFY(m_editor->isEditing());

        // Simulate Enter to select from autocomplete (handled synchronously)
        QSignalSpy spy(m_editor, &RcxEditor::inlineEditCommitted);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(m_editor->scintilla(), &enter);

        // Should have committed immediately (no deferred timer for type edits)
        QCOMPARE(spy.count(), 1);
        QVERIFY(!m_editor->isEditing());

        // The committed text should be a valid typeName starting with 'i'
        QList<QVariant> args = spy.first();
        QString committedText = args.at(3).toString();
        QVERIFY2(committedText.startsWith('i'),
                 qPrintable("Expected typeName starting with 'i', got: " + committedText));

        m_editor->applyDocument(m_result);
    }

    // ── Test: type edit click-away commits original (no change) ──
    void testTypeEditClickAwayNoChange() {
        m_editor->applyDocument(m_result);

        bool ok = m_editor->beginInlineEdit(EditTarget::Type, 2);
        QVERIFY(ok);

        // Process deferred autocomplete
        QApplication::processEvents();

        // Click away on viewport — should commit (not cancel)
        QSignalSpy commitSpy(m_editor, &RcxEditor::inlineEditCommitted);
        QMouseEvent click(QEvent::MouseButtonPress, QPointF(10, 10),
                          QPointF(10, 10), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_editor->scintilla()->viewport(), &click);

        QVERIFY(!m_editor->isEditing());
        QCOMPARE(commitSpy.count(), 1);

        // The committed text should be the original typeName (no change)
        QList<QVariant> args = commitSpy.first();
        QString committedText = args.at(3).toString();
        QVERIFY2(committedText == "uint16_t",
                 qPrintable("Expected 'uint16_t', got: " + committedText));

        m_editor->applyDocument(m_result);
    }

    // ── Test: column span hit-testing for cursor shape ──
    void testColumnSpanHitTest() {
        m_editor->applyDocument(m_result);

        // Line 2 is a field line (UInt16), verify spans are valid (line 0=CommandRow, 1=header)
        const LineMeta* lm = m_editor->metaForLine(2);
        QVERIFY(lm);
        QCOMPARE(lm->lineKind, LineKind::Field);

        // Type span should be valid for field lines
        ColumnSpan ts = RcxEditor::typeSpan(*lm);
        QVERIFY(ts.valid);
        QVERIFY(ts.start < ts.end);

        // Name span should be valid for field lines
        ColumnSpan ns = RcxEditor::nameSpan(*lm);
        QVERIFY(ns.valid);
        QVERIFY(ns.start < ns.end);

        // Value span should be valid for field lines
        QString lineText;
        int len = (int)m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_LINELENGTH, (unsigned long)2);
        QVERIFY(len > 0);
        ColumnSpan vs = RcxEditor::valueSpan(*lm, len);
        QVERIFY(vs.valid);
        QVERIFY(vs.start < vs.end);

        // Footer line should have no valid type/name spans
        int footerLine = -1;
        for (int i = 0; i < m_result.meta.size(); i++) {
            if (m_result.meta[i].lineKind == LineKind::Footer) {
                footerLine = i;
                break;
            }
        }
        QVERIFY(footerLine >= 0);
        const LineMeta* flm = m_editor->metaForLine(footerLine);
        QVERIFY(flm);
        ColumnSpan fts = RcxEditor::typeSpan(*flm);
        QVERIFY(!fts.valid);
        ColumnSpan fns = RcxEditor::nameSpan(*flm);
        QVERIFY(!fns.valid);
        ColumnSpan fvs = RcxEditor::valueSpan(*flm, 10);
        QVERIFY(!fvs.valid);
    }

    // ── Test: selectedNodeIndices ──
    void testSelectedNodeIndices() {
        m_editor->applyDocument(m_result);

        // Put cursor on first field line (line 2; 0=CommandRow, 1=header)
        m_editor->scintilla()->setCursorPosition(2, 0);
        QSet<int> sel = m_editor->selectedNodeIndices();
        QCOMPARE(sel.size(), 1);

        // The node index should match the first field
        const LineMeta* lm = m_editor->metaForLine(2);
        QVERIFY(lm);
        QVERIFY(sel.contains(lm->nodeIdx));
    }

    // ── Test: header line no longer contains "// base:" ──
    void testBaseAddressDisplay() {
        NodeTree tree = makeTestTree();
        tree.baseAddress = 0x10;
        FileProvider prov = makeTestProvider();
        ComposeResult result = compose(tree, prov);

        m_editor->applyDocument(result);

        // Line 1 should be the struct header (line 0 is CommandRow)
        const LineMeta* lm = m_editor->metaForLine(1);
        QVERIFY(lm);
        QCOMPARE(lm->lineKind, LineKind::Header);
        QVERIFY(lm->isRootHeader);

        // Get header line text — should NOT contain "// base:" (consolidated into cmd bar)
        QString lineText;
        int len = (int)m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_LINELENGTH, (unsigned long)1);
        if (len > 0) {
            QByteArray buf(len + 1, '\0');
            m_editor->scintilla()->SendScintilla(
                QsciScintillaBase::SCI_GETLINE, (unsigned long)1, (void*)buf.data());
            lineText = QString::fromUtf8(buf.constData(), len).trimmed();
        }

        QVERIFY2(!lineText.contains("// base:"),
                 qPrintable("Header should no longer contain '// base:', got: " + lineText));
        QVERIFY2(lineText.contains("struct"),
                 qPrintable("Header should contain 'struct', got: " + lineText));

        m_editor->applyDocument(m_result);
    }

    // ── Test: CommandRow ADDR span is valid ──
    void testBaseAddressSpan() {
        m_editor->applyDocument(m_result);

        // Set CommandRow text with ADDR value (simulates controller)
        m_editor->setCommandRowText(
            QStringLiteral(" * SRC: File : 0x140000000"));

        // Line 0 is CommandRow
        const LineMeta* lm = m_editor->metaForLine(0);
        QVERIFY(lm);
        QCOMPARE(lm->lineKind, LineKind::CommandRow);

        // Get CommandRow line text
        QString lineText;
        int len = (int)m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_LINELENGTH, (unsigned long)0);
        if (len > 0) {
            QByteArray buf(len + 1, '\0');
            m_editor->scintilla()->SendScintilla(
                QsciScintillaBase::SCI_GETLINE, (unsigned long)0, (void*)buf.data());
            lineText = QString::fromUtf8(buf.constData(), len);
            while (lineText.endsWith('\n') || lineText.endsWith('\r'))
                lineText.chop(1);
        }

        // ADDR span should be valid (uses commandRowAddrSpan)
        ColumnSpan as = commandRowAddrSpan(lineText);
        QVERIFY2(as.valid, "ADDR span should be valid on CommandRow");
        QVERIFY(as.start < as.end);

        // The span should cover the hex address
        QString spanText = lineText.mid(as.start, as.end - as.start);
        QVERIFY2(spanText.contains("0x") || spanText.startsWith("0X"),
                 qPrintable("Span should contain hex address, got: " + spanText));

        m_editor->applyDocument(m_result);
    }

    // ── Test: base address edit begins on CommandRow (line 0) ──
    void testBaseAddressEditBegins() {
        m_editor->applyDocument(m_result);

        // Set CommandRow text with ADDR value (simulates controller)
        m_editor->setCommandRowText(
            QStringLiteral(" * SRC: File : 0x140000000"));

        // Begin base address edit on line 0 (CommandRow ADDR field)
        bool ok = m_editor->beginInlineEdit(EditTarget::BaseAddress, 0);
        QVERIFY2(ok, "Should be able to begin base address edit on CommandRow");
        QVERIFY(m_editor->isEditing());

        // Cancel and reset
        m_editor->cancelInlineEdit();
        m_editor->applyDocument(m_result);
    }
};

QTEST_MAIN(TestEditor)
#include "test_editor.moc"
