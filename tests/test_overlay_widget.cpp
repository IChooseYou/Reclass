// Widget-level tests for inline RTTI/TypeHint chips.
//
// What this file pins down:
//   - After applyDocument, RTTI chip text appears inline in the line text
//     at startCol..endCol (rendered natively by Scintilla, so scroll/zoom/
//     resize sync is free).
//   - The chip is visual only — clicking it does NOT fire a signal (RTTI
//     click was deliberately disabled after we tried "auto-create class +
//     retype pointer" semantics and found them annoying).
//
// TypeHint click behavior is covered by the controller-level tests where
// the changeNodeKind call lands; we don't re-verify it from the widget.

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QMouseEvent>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>
#include <cstring>

#include "core.h"
#include "editor.h"
#include "providers/buffer_provider.h"
#include "themes/thememanager.h"

using namespace rcx;

namespace {

constexpr uint64_t kImageBase  = 0x10000;
constexpr uint64_t kStructBase = 0x30000;
constexpr uint32_t kVtableRva  = 0x1000;
constexpr uint32_t kColRva     = 0x1900;
constexpr uint32_t kTdRva      = 0x1100;
constexpr uint32_t kChdRva     = 0x1400;
constexpr uint32_t kBcaRva     = 0x1500;
constexpr uint32_t kBcdRva     = 0x1600;

template<class T>
void writeAt(QByteArray& buf, qsizetype at, T value) {
    std::memcpy(buf.data() + at, &value, sizeof(T));
}

QByteArray buildRttiAddressSpace() {
    QByteArray buf(kStructBase + 0x1000, '\0');
    writeAt<uint64_t>(buf, kImageBase + kVtableRva - 8, kImageBase + kColRva);
    writeAt<uint64_t>(buf, kImageBase + kTdRva + 0, 0xDEADBEEF);
    writeAt<uint64_t>(buf, kImageBase + kTdRva + 8, 0);
    const char* mangled = ".?AVFoo@@";
    std::memcpy(buf.data() + kImageBase + kTdRva + 16, mangled,
                std::strlen(mangled) + 1);
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x00, 0);
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x04, 0);
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x08, 1);
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x0C, kBcaRva);
    writeAt<uint32_t>(buf, kImageBase + kBcaRva + 0, kBcdRva);
    writeAt<uint32_t>(buf, kImageBase + kBcdRva + 0, kTdRva);
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x00, 1);
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x04, 0);
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x08, 0);
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x0C, kTdRva);
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x10, kChdRva);
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x14, (uint32_t)kImageBase);
    return buf;
}

class FakeModuleProvider : public BufferProvider {
public:
    FakeModuleProvider(QByteArray d, const QString& n)
        : BufferProvider(std::move(d), n) {}
    QVector<ModuleEntry> enumerateModules() const override {
        return { ModuleEntry{ QStringLiteral("synthetic.dll"),
                              QStringLiteral("synthetic.dll"),
                              kImageBase, 0x10000 } };
    }
};

// Tree with one Hex64 field whose value is the synthetic vtable.
NodeTree buildTreeWithVtable() {
    NodeTree tree;
    tree.baseAddress = kStructBase;
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = QStringLiteral("Demo");
    root.name = QStringLiteral("demo");
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;
    Node f;
    f.kind = NodeKind::Hex64;
    f.name = QStringLiteral("vtbl");
    f.parentId = rootId;
    f.offset = 0;
    tree.addNode(f);
    return tree;
}

// Tree with a typed Pointer64 whose value reads as 0x00 — fires the
// "(Name class…)" null-RTTI chip.
NodeTree buildTreeWithNullPointer() {
    NodeTree tree;
    tree.baseAddress = kStructBase;
    Node target;
    target.kind = NodeKind::Struct;
    target.structTypeName = QStringLiteral("Target");
    target.name = QStringLiteral("t");
    int ti = tree.addNode(target);
    uint64_t targetId = tree.nodes[ti].id;

    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = QStringLiteral("Host");
    root.name = QStringLiteral("host");
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    Node ptr;
    ptr.kind = NodeKind::Pointer64;
    ptr.name = QStringLiteral("__vptr");
    ptr.parentId = rootId;
    ptr.offset = 0;
    ptr.refId = targetId;
    ptr.collapsed = true;
    tree.addNode(ptr);
    return tree;
}

// Find the (line, chip) where a chip of the given kind lives. Returns
// {-1, nullptr} if none.
struct ChipLocation {
    int line = -1;
    const LineChip* chip = nullptr;
};
ChipLocation findChip(const ComposeResult& r, ChipKind kind) {
    for (int ln = 0; ln < r.meta.size(); ++ln)
        for (const auto& c : r.meta[ln].chips)
            if (c.kind == kind) return { ln, &c };
    return {};
}

} // anon

class TestOverlayWidget : public QObject {
    Q_OBJECT
private:
    RcxEditor* m_editor = nullptr;

private slots:
    void initTestCase() {
        m_editor = new RcxEditor();
        m_editor->resize(900, 600);
        m_editor->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_editor));
    }

    void cleanupTestCase() {
        delete m_editor;
    }

    // ── Resolved RTTI chip appears inline at the right column range ──
    void resolvedRttiChipIsInline() {
        QByteArray data = buildRttiAddressSpace();
        uint64_t vtable = kImageBase + kVtableRva;
        std::memcpy(data.data() + kStructBase, &vtable, 8);
        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));

        NodeTree tree = buildTreeWithVtable();
        ComposeResult r = compose(tree, prov);
        m_editor->applyDocument(r);
        QApplication::processEvents();

        ChipLocation loc = findChip(r, ChipKind::Rtti);
        QVERIFY2(loc.chip, "expected one RTTI chip");
        QCOMPARE(loc.chip->text, QStringLiteral("Foo"));
        QVERIFY(loc.chip->startCol >= 0);
        QVERIFY(loc.chip->endCol > loc.chip->startCol);

        // Verify the chip text actually lives in the Scintilla buffer at
        // that line — proves we're rendering inline, not via a widget.
        QString lineText = m_editor->scintilla()->text(loc.line);
        QVERIFY2(lineText.contains(QStringLiteral("Foo")),
                 qPrintable(QStringLiteral("line ") + QString::number(loc.line)
                            + QStringLiteral(" should contain 'Foo', got: ")
                            + lineText));
    }

    // ── Null-vtable "(Name class…)" chip still emits, just as inline
    //    visual text. Verifies the compose path that fires on a typed
    //    Pointer64 whose backing memory reads as 0x00. ──
    void nullRttiChipAppearsInline() {
        QByteArray data(kStructBase + 64, '\0');
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        NodeTree tree = buildTreeWithNullPointer();
        uint64_t hostId = 0;
        for (const auto& n : tree.nodes)
            if (n.structTypeName == QStringLiteral("Host"))
                { hostId = n.id; break; }
        QVERIFY(hostId != 0);

        ComposeResult r = compose(tree, prov, hostId);
        m_editor->applyDocument(r);
        QApplication::processEvents();

        ChipLocation loc = findChip(r, ChipKind::Rtti);
        QVERIFY2(loc.chip, "expected one null-RTTI chip");
        QCOMPARE(loc.chip->rttiVtableAddr, uint64_t(0));
        QCOMPARE(loc.chip->text, QStringLiteral("(Name class…)"));
        QVERIFY(loc.chip->startCol >= 0);
        QVERIFY(loc.chip->endCol > loc.chip->startCol);
    }

    // ── Clicking the RTTI chip does NOT fire any signal — RTTI click was
    //    deliberately disabled. The chip is read-only data. ──
    void rttiChipClickIsDisabled() {
        QByteArray data = buildRttiAddressSpace();
        uint64_t vtable = kImageBase + kVtableRva;
        std::memcpy(data.data() + kStructBase, &vtable, 8);
        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));

        NodeTree tree = buildTreeWithVtable();
        ComposeResult r = compose(tree, prov);
        m_editor->applyDocument(r);
        QApplication::processEvents();

        ChipLocation loc = findChip(r, ChipKind::Rtti);
        QVERIFY(loc.chip);

        QSignalSpy rttiSpy(m_editor, &RcxEditor::rttiChipClicked);
        QSignalSpy nullSpy(m_editor, &RcxEditor::rttiNullChipClicked);

        auto* sci = m_editor->scintilla();
        long lineStart = sci->SendScintilla(
            QsciScintillaBase::SCI_POSITIONFROMLINE, (unsigned long)loc.line);
        long startBytePos = sci->SendScintilla(
            QsciScintillaBase::SCI_FINDCOLUMN,
            (unsigned long)loc.line, (long)loc.chip->startCol);
        long endBytePos = sci->SendScintilla(
            QsciScintillaBase::SCI_FINDCOLUMN,
            (unsigned long)loc.line, (long)loc.chip->endCol);
        int xStart = (int)sci->SendScintilla(
            QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, startBytePos);
        int xEnd   = (int)sci->SendScintilla(
            QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, endBytePos);
        int y      = (int)sci->SendScintilla(
            QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, lineStart);
        int lh     = (int)sci->SendScintilla(
            QsciScintillaBase::SCI_TEXTHEIGHT, (unsigned long)loc.line);
        QPoint clickAt((xStart + xEnd) / 2, y + lh / 2);

        QTest::mouseClick(sci->viewport(), Qt::LeftButton, Qt::NoModifier, clickAt);
        QApplication::processEvents();

        QCOMPARE(rttiSpy.count(), 0);
        QCOMPARE(nullSpy.count(), 0);
    }
};

QTEST_MAIN(TestOverlayWidget)
#include "test_overlay_widget.moc"
