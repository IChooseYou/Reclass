// Tests for the null-vtable RTTI chip and its rename-current-tab-class
// click handler.
//
// When the user has a typed Pointer64 field whose backing memory reads
// as 0x00 (uninitialized, process not attached, fresh struct), compose
// emits an overlay-only Rtti chip with text "(Name class…)" and
// rttiVtableAddr = 0. Clicking it should NOT auto-create a class —
// instead it fires rttiNullChipClicked, and MainWindow's slot opens
// inline rename on the current tab's root struct name. This file
// verifies the compose side; the rename plumbing is covered by
// test_editor's existing inline-edit tests.

#include <QtTest/QTest>
#include <QByteArray>
#include "core.h"
#include "providers/buffer_provider.h"

using namespace rcx;

namespace {

const LineChip* firstChipOfKind(const ComposeResult& r, ChipKind k) {
    for (const auto& lm : r.meta)
        if (auto* c = findChip(lm, k))
            return c;
    return nullptr;
}

int countChips(const ComposeResult& r, ChipKind k) {
    int n = 0;
    for (const auto& lm : r.meta)
        for (const auto& c : lm.chips)
            if (c.kind == k) ++n;
    return n;
}

// The null-pointer "(Name class…)" CTA is intentionally gated on
// Provider::isLive() so it never sprouts on a flat file (where every pointer
// reads 0). These tests exercise the live-memory case, so they need a
// provider that reports live.
class LiveBufferProvider : public BufferProvider {
public:
    using BufferProvider::BufferProvider;
    bool isLive() const override { return true; }
};

} // anon

class TestOverlayNullRtti : public QObject {
    Q_OBJECT
private slots:

    // ── Typed Pointer64 with value 0x00 → "(Name class…)" chip emits
    //    with rttiVtableAddr = 0 and a valid startCol range. ──
    void nullTypedPointerEmitsNameClassChip() {
        // Target struct (typed pointer points at this).
        NodeTree tree;
        tree.baseAddress = 0x1000;

        Node target;
        target.kind = NodeKind::Struct;
        target.structTypeName = QStringLiteral("Target");
        target.name = QStringLiteral("t");
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        Node host;
        host.kind = NodeKind::Struct;
        host.structTypeName = QStringLiteral("Host");
        host.name = QStringLiteral("host");
        int hi = tree.addNode(host);
        uint64_t hostId = tree.nodes[hi].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = QStringLiteral("__vptr");
        ptr.parentId = hostId;
        ptr.offset = 0;
        ptr.refId = targetId;
        ptr.collapsed = true;
        tree.addNode(ptr);

        // Buffer all zeros → pointer reads as 0x00. Live source so the CTA fires.
        QByteArray data(0x2000, '\0');
        LiveBufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult r = compose(tree, prov, hostId);
        const LineChip* c = firstChipOfKind(r, ChipKind::Rtti);
        QVERIFY2(c, "null-vtable chip should emit on a typed Pointer64 with value 0x00");
        QCOMPARE(c->text, QStringLiteral("(Name class…)"));
        QCOMPARE(c->rttiVtableAddr, uint64_t(0));
        QVERIFY2(c->startCol >= 0, "null-RTTI chip must have a startCol set");
        QVERIFY2(c->endCol > c->startCol, "endCol must be past startCol");
    }

    // ── Untyped void Pointer64 (refId == 0) with value 0x00 also emits
    //    the chip — composeLeaf handles raw pointer fields, not just
    //    typed ones. ──
    void nullVoidPointerEmitsNameClassChip() {
        NodeTree tree;
        tree.baseAddress = 0x1000;

        Node host;
        host.kind = NodeKind::Struct;
        host.structTypeName = QStringLiteral("Host");
        host.name = QStringLiteral("h");
        int hi = tree.addNode(host);
        uint64_t hostId = tree.nodes[hi].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = QStringLiteral("opaque");
        ptr.parentId = hostId;
        ptr.offset = 0;
        ptr.refId = 0;          // void pointer
        tree.addNode(ptr);

        QByteArray data(0x2000, '\0');
        LiveBufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult r = compose(tree, prov, hostId);
        const LineChip* c = firstChipOfKind(r, ChipKind::Rtti);
        QVERIFY2(c, "untyped void pointer with value 0x00 should also get the chip");
        QCOMPARE(c->text, QStringLiteral("(Name class…)"));
        QCOMPARE(c->rttiVtableAddr, uint64_t(0));
    }

    // ── Hex64 with value 0x00 does NOT get a chip. We restrict the
    //    null-pointer call-to-action to pointer-kinded fields so a
    //    fresh struct (60+ zero-byte hex64 rows) doesn't sprout 60
    //    chips. ──
    void nullHex64DoesNotGetChip() {
        NodeTree tree;
        tree.baseAddress = 0x1000;

        Node host;
        host.kind = NodeKind::Struct;
        host.structTypeName = QStringLiteral("H");
        host.name = QStringLiteral("h");
        int hi = tree.addNode(host);
        uint64_t hostId = tree.nodes[hi].id;

        Node hex;
        hex.kind = NodeKind::Hex64;
        hex.name = QStringLiteral("data");
        hex.parentId = hostId;
        hex.offset = 0;
        tree.addNode(hex);

        QByteArray data(0x2000, '\0');
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult r = compose(tree, prov, hostId);
        QCOMPARE(countChips(r, ChipKind::Rtti), 0);
    }

    // ── showRtti=false suppresses the null chip too (same toggle as
    //    resolved-RTTI chips). ──
    void showRttiOffSuppressesNullChip() {
        NodeTree tree;
        tree.baseAddress = 0x1000;
        Node host;
        host.kind = NodeKind::Struct;
        host.structTypeName = QStringLiteral("H");
        host.name = QStringLiteral("h");
        int hi = tree.addNode(host);
        uint64_t hostId = tree.nodes[hi].id;
        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = QStringLiteral("p");
        ptr.parentId = hostId;
        ptr.offset = 0;
        tree.addNode(ptr);

        QByteArray data(0x2000, '\0');
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult r = compose(tree, prov, hostId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/false,
            /*showComments=*/true, /*symbolLookup=*/{},
            /*showRtti=*/false);
        QCOMPARE(countChips(r, ChipKind::Rtti), 0);
    }

    // ── A NON-live source (flat file / BufferProvider) must NOT get the null
    //    CTA, even with showRtti on — otherwise every pointer of an all-zero
    //    file sprouts one. Gated on Provider::isLive(). ──
    void nonLiveSourceSuppressesNullChip() {
        NodeTree tree;
        tree.baseAddress = 0x1000;
        Node host;
        host.kind = NodeKind::Struct;
        host.structTypeName = QStringLiteral("H");
        host.name = QStringLiteral("h");
        int hi = tree.addNode(host);
        uint64_t hostId = tree.nodes[hi].id;
        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = QStringLiteral("p");
        ptr.parentId = hostId;
        ptr.offset = 0;
        tree.addNode(ptr);

        QByteArray data(0x2000, '\0');
        BufferProvider prov(std::move(data), QStringLiteral("file"));  // isLive()==false

        ComposeResult r = compose(tree, prov, hostId);  // showRtti defaults on
        QCOMPARE(countChips(r, ChipKind::Rtti), 0);
    }
};

QTEST_MAIN(TestOverlayNullRtti)
#include "test_overlay_null_rtti.moc"
