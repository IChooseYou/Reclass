// Unit tests for the unified tail-chip model (LineMeta::chips).
//
// Each compose pass attaches a vector of chips to every Field/Header
// LineMeta that has annotations to display: Enum, TypeHint, Rtti,
// Comment. The editor reads these for indicator colors, hit-testing,
// and inline-edit span lookup.
//
// These tests target the chip *data* path — they don't run the editor
// because chip rendering is a Scintilla concern verified by test_editor.
// What we lock down here:
//   - Each chip kind emits when its preconditions hold and is suppressed
//     when its View toggle is false.
//   - Chip text carries the right glyph prefix ((), [], {}, /).
//   - startCol / endCol bracket the chip's text in the rendered line.
//   - Chip order on a single line is Enum → TypeHint → Rtti → Comment
//     (the order the editor's indicator pass and click router expect).

#include <QtTest/QTest>
#include <QByteArray>
#include <cstring>
#include "core.h"
#include "providers/buffer_provider.h"

using namespace rcx;

namespace {

// Lay out an address space with: a synthetic vtable in module range so
// the RTTI walker's MSVC path catches it, plus a struct region past the
// module that holds whatever fields the test plants.
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

// Mini RTTI fixture — just enough for walkRtti's MSVC path to demangle a
// single class name "Foo". Mirrors test_rtti_hint.cpp's shape.
QByteArray buildRttiAddressSpace() {
    QByteArray buf(kStructBase + 0x1000, '\0');
    // Vtable[-1] = COL VA
    writeAt<uint64_t>(buf, kImageBase + kVtableRva - 8, kImageBase + kColRva);
    // Type descriptor with mangled name ".?AVFoo@@" (MSVC mangling for class Foo)
    writeAt<uint64_t>(buf, kImageBase + kTdRva + 0, 0xDEADBEEF);
    writeAt<uint64_t>(buf, kImageBase + kTdRva + 8, 0);
    const char* mangled = ".?AVFoo@@";
    std::memcpy(buf.data() + kImageBase + kTdRva + 16, mangled,
                std::strlen(mangled) + 1);
    // Class hierarchy descriptor: 1 base
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x00, 0);
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x04, 0);
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x08, 1);
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x0C, kBcaRva);
    // Base-class array → single base
    writeAt<uint32_t>(buf, kImageBase + kBcaRva + 0, kBcdRva);
    // Base-class descriptor → type descriptor for Foo itself
    writeAt<uint32_t>(buf, kImageBase + kBcdRva + 0, kTdRva);
    // Complete object locator: signature, offset, cdOffset, type-desc, chd, imageBase
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

// Helper: count chips of a given kind across all meta lines.
int countChips(const ComposeResult& r, ChipKind k) {
    int n = 0;
    for (const auto& lm : r.meta)
        for (const auto& c : lm.chips)
            if (c.kind == k) ++n;
    return n;
}

// Helper: find first line that has a chip of `k`, returning the chip.
const LineChip* firstChipOfKind(const ComposeResult& r, ChipKind k) {
    for (const auto& lm : r.meta)
        if (auto* c = findChip(lm, k))
            return c;
    return nullptr;
}

} // anon

class TestChips : public QObject {
    Q_OBJECT
private slots:

    // ── Enum chip emits on int field whose refId resolves to an enum,
    //    and is suppressed by showEnumChips=false ──
    void enumChipFiresAndCanBeSuppressed() {
        NodeTree tree;
        tree.baseAddress = kStructBase;

        Node enumNode;
        enumNode.kind = NodeKind::Struct;
        enumNode.classKeyword = QStringLiteral("enum");
        enumNode.structTypeName = QStringLiteral("Status");
        enumNode.name = QStringLiteral("Status");
        enumNode.enumMembers = {
            {QStringLiteral("READY"),   0},
            {QStringLiteral("RUNNING"), 1},
            {QStringLiteral("DONE"),    2},
        };
        int ei = tree.addNode(enumNode);
        uint64_t enumId = tree.nodes[ei].id;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        root.name = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node field;
        field.kind = NodeKind::UInt32;
        field.name = QStringLiteral("status");
        field.parentId = rootId;
        field.offset = 0;
        field.refId = enumId;
        tree.addNode(field);

        QByteArray data(kStructBase + 16, '\0');
        uint32_t v = 1;  // RUNNING
        std::memcpy(data.data() + kStructBase, &v, 4);

        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        // Default: enum chip fires.
        ComposeResult r = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(r, ChipKind::Enum);
        QVERIFY2(c, "enum chip should fire on int field with refId→enum");
        QVERIFY2(c->text.contains(QStringLiteral("RUNNING")),
            qPrintable(QStringLiteral("expected RUNNING in chip text, got: ")
                + c->text));
        QCOMPARE(c->enumCurrentValue, (int64_t)1);
        QCOMPARE(c->enumRefNodeId, enumId);
        QVERIFY(c->startCol >= 0);
        QVERIFY(c->endCol > c->startCol);

        // Toggle off: chip suppressed.
        ComposeResult r2 = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/false,
            /*showComments=*/true, /*symbolLookup=*/{},
            /*showRtti=*/true, /*showEnumChips=*/false);
        QCOMPARE(countChips(r2, ChipKind::Enum), 0);
    }

    // ── TypeHint chip fires on hex node with strong inference and is
    //    always emitted as an overlay-only chip (no inline text). The
    //    previous typeHints flag was retired when chips moved to the
    //    ChipOverlay widget — overlays are unobtrusive enough that
    //    always-on is fine. ──
    void typeHintChipFiresAsOverlay() {
        NodeTree tree;
        tree.baseAddress = kStructBase;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node hex;
        hex.kind = NodeKind::Hex64;
        hex.name = QStringLiteral("payload");
        hex.parentId = rootId;
        hex.offset = 0;
        tree.addNode(hex);

        // Plant two int32s side by side — inferTypes treats this as
        // int32×2 with strong confidence.
        QByteArray data(kStructBase + 16, '\0');
        int32_t a = 14;
        int32_t b = 20;
        std::memcpy(data.data() + kStructBase + 0, &a, 4);
        std::memcpy(data.data() + kStructBase + 4, &b, 4);
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult r = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(r, ChipKind::TypeHint);
        QVERIFY2(c, "type-inference chip should fire on hex node with strong inference");
        // Inline rendering: chip text is appended to lineText, startCol/endCol
        // bracket the range, indicator pass styles it as a clickable pill.
        QVERIFY2(c->startCol >= 0, "TypeHint chip must have a startCol set");
        QVERIFY2(c->endCol > c->startCol, "endCol must be past startCol");
        QVERIFY2(!c->text.contains('['),
            qPrintable(QStringLiteral("chip text should be plain (no brackets), got: ") + c->text));
        QVERIFY(!c->typeHintKinds.isEmpty());
    }

    // ── RTTI chip fires when a hex64's value points at a known vtable,
    //    and showRtti=false suppresses it ──
    void rttiChipFiresAndCanBeSuppressed() {
        QByteArray data = buildRttiAddressSpace();
        uint64_t vtable = kImageBase + kVtableRva;
        std::memcpy(data.data() + kStructBase, &vtable, 8);

        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));

        NodeTree tree;
        tree.baseAddress = kStructBase;
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Demo");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::Hex64;
        f.name = QStringLiteral("vtbl");
        f.parentId = rootId;
        f.offset = 0;
        tree.addNode(f);

        ComposeResult rOn = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(rOn, ChipKind::Rtti);
        QVERIFY2(c, "RTTI chip should fire on hex64 whose value is a vtable");
        // Plain demangled name — indicator pass styles it as a pill, no
        // "{RTTI: …}" prefix needed. Symbol suffix dropped per
        // annotation-merge rule (RTTI supersedes Symbol).
        QCOMPARE(c->text, QStringLiteral("Foo"));
        QCOMPARE(c->rttiVtableAddr, vtable);
        QVERIFY2(c->startCol >= 0, "RTTI chip must have a startCol set");
        QVERIFY2(c->endCol > c->startCol, "endCol must be past startCol");

        ComposeResult rOff = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/false,
            /*showComments=*/true, /*symbolLookup=*/{},
            /*showRtti=*/false);
        QCOMPARE(countChips(rOff, ChipKind::Rtti), 0);
    }

    // ── Comment chip fires for Node::comment, suppressed by showComments=false ──
    void commentChipFiresAndCanBeSuppressed() {
        NodeTree tree;
        tree.baseAddress = kStructBase;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::UInt32;
        f.name = QStringLiteral("count");
        f.parentId = rootId;
        f.offset = 0;
        f.comment = QStringLiteral("ref count from header");
        tree.addNode(f);

        QByteArray data(kStructBase + 16, '\0');
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult rOn = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(rOn, ChipKind::Comment);
        QVERIFY2(c, "comment chip should fire when Node::comment is set");
        // Comment chip text is the raw comment — the green pill carries
        // the "this is a comment" signal, no glyph prefix needed.
        QCOMPARE(c->text, QStringLiteral("ref count from header"));

        // Suppressed when showComments=false.
        ComposeResult rOff = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/false,
            /*showComments=*/false);
        QCOMPARE(countChips(rOff, ChipKind::Comment), 0);
    }

    // ── Multi-line newlines in a comment collapse to a middle-dot
    //    separator (defensive against phantom Scintilla rows) ──
    void multilineCommentStaysOnOneLine() {
        NodeTree tree;
        tree.baseAddress = kStructBase;
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("X");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;
        Node f;
        f.kind = NodeKind::UInt32;
        f.name = QStringLiteral("v");
        f.parentId = rootId;
        f.offset = 0;
        f.comment = QStringLiteral("first line\nsecond line\nthird");
        tree.addNode(f);
        QByteArray data(kStructBase + 16, '\0');
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));
        ComposeResult r = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(r, ChipKind::Comment);
        QVERIFY(c);
        QVERIFY2(!c->text.contains(QChar('\n')),
            qPrintable(QStringLiteral("comment chip text must not contain newlines: ")
                + c->text));
        QVERIFY(c->text.contains(QStringLiteral("first line")));
    }

    // ── Chip render order on a single line: Enum → TypeHint → Rtti → Comment ──
    // Build a row that triggers Comment + TypeHint + Rtti at once. (Enum
    // can't coexist with hex64 because Enum requires int kind; TypeHint /
    // Rtti both want hex64. Verify the three that *can* coexist.)
    void chipsAppearInDefinedOrder() {
        QByteArray data = buildRttiAddressSpace();
        uint64_t vtable = kImageBase + kVtableRva;
        std::memcpy(data.data() + kStructBase, &vtable, 8);
        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));

        NodeTree tree;
        tree.baseAddress = kStructBase;
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Demo");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;
        Node f;
        f.kind = NodeKind::Hex64;
        f.name = QStringLiteral("vtbl");
        f.parentId = rootId;
        f.offset = 0;
        f.comment = QStringLiteral("the vtable");
        tree.addNode(f);

        ComposeResult r = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/true,
            /*showComments=*/true);

        // Find the field row (must have at least Rtti+Comment to be useful).
        const LineMeta* row = nullptr;
        for (const auto& lm : r.meta) {
            if (findChip(lm, ChipKind::Rtti) && findChip(lm, ChipKind::Comment)) {
                row = &lm; break;
            }
        }
        QVERIFY2(row, "expected a row carrying both Rtti + Comment chips");

        // Order check: for INLINE chips only (overlay chips have
        // startCol == -1 — they don't participate in the linear chip
        // strip), every adjacent pair by ChipKind enum order should
        // have monotonic startCol. Locks the editor's per-row click
        // router into a deterministic left-to-right layout.
        int prevStart = -1;
        ChipKind prevKind = ChipKind::Enum;
        for (const auto& c : row->chips) {
            if (c.startCol < 0) continue;  // skip overlay-only chips
            if (prevStart >= 0) {
                QVERIFY2(prevStart < c.startCol,
                    qPrintable(QStringLiteral("chip startCol non-monotonic: %1 then %2")
                        .arg(prevStart).arg(c.startCol)));
                QVERIFY2(static_cast<int>(prevKind) <= static_cast<int>(c.kind),
                    qPrintable(QStringLiteral("chip order broken: kind %1 followed by %2")
                        .arg((int)prevKind).arg((int)c.kind)));
            }
            prevStart = c.startCol;
            prevKind = c.kind;
        }

        // The Comment chip must be the last INLINE chip on the row —
        // user-authored text is always rightmost so the comment-edit
        // flow can strip from chip.startCol → end-of-line.
        const LineChip* lastInline = nullptr;
        for (const auto& c : row->chips) {
            if (c.startCol < 0) continue;
            lastInline = &c;
        }
        QVERIFY(lastInline);
        QCOMPARE(lastInline->kind, ChipKind::Comment);
    }

    // ── Chip startCol/endCol bracket their text in the rendered line ──
    void chipSpansMatchRenderedText() {
        NodeTree tree;
        tree.baseAddress = kStructBase;
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;
        Node f;
        f.kind = NodeKind::UInt32;
        f.name = QStringLiteral("v");
        f.parentId = rootId;
        f.offset = 0;
        f.comment = QStringLiteral("this is a comment");
        tree.addNode(f);
        QByteArray data(kStructBase + 16, '\0');
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));
        ComposeResult r = compose(tree, prov, rootId);

        // Walk to the field's line. Its line text + startCol/endCol should
        // bracket the chip's text exactly.
        bool checked = false;
        for (int i = 0; i < r.meta.size(); ++i) {
            const auto& lm = r.meta[i];
            const LineChip* c = findChip(lm, ChipKind::Comment);
            if (!c) continue;
            int lineStart = (i < r.lineStarts.size()) ? r.lineStarts[i] : -1;
            int lineEnd   = (i + 1 < r.lineStarts.size())
                ? r.lineStarts[i + 1] - 1
                : r.text.size();
            QVERIFY(lineStart >= 0);
            QString lineText = r.text.mid(lineStart, lineEnd - lineStart);
            QVERIFY2(c->endCol <= lineText.size(),
                qPrintable(QStringLiteral("endCol %1 must be <= line length %2")
                    .arg(c->endCol).arg(lineText.size())));
            QString slice = lineText.mid(c->startCol, c->endCol - c->startCol);
            QCOMPARE(slice, c->text);
            checked = true;
        }
        QVERIFY2(checked, "test should have hit the comment-chip line");
    }
};

QTEST_MAIN(TestChips)
#include "test_chips.moc"
