#include "core.h"
#include "rtti.h"
#include "providers/buffer_provider.h"
#include "providers/provider.h"
#include <QtTest/QTest>
#include <QByteArray>
#include <cstring>

using namespace rcx;

// Tests the *integration* of walkRtti into compose() — i.e. that the auto-
// detect block in composeLeaf attaches an rttiHint to a Hex64 LineMeta when
// its value points at a synthetic vtable. Mirrors the synthetic RTTI buffer
// shape from test_rtti.cpp but adds a tree + provider that exercises the
// full compose path.
//
// The provider's enumerateModules() override is critical: without it
// `findOwningModule` returns invalid and the hint short-circuits.

namespace {

constexpr uint64_t kImageBase = 0x10000;

template<class T>
void writeAt(QByteArray& buf, qsizetype at, T value) {
    std::memcpy(buf.data() + at, &value, sizeof(T));
}

void writeCStr(QByteArray& buf, qsizetype at, const char* s) {
    std::memcpy(buf.data() + at, s, std::strlen(s) + 1);
}

// Construct the same RTTI shape used in test_rtti.cpp (Foo with bases
// Bar/Baz). Address space:
//   [0 .. kImageBase) zeros
//   [kImageBase .. kImageBase + 0x10000) RTTI region
//   [kImageBase + 0x10000 .. ) struct data (Hex64 fields the user lays out)
QByteArray buildAddressSpaceWithRtti() {
    QByteArray rtti(0x10000, '\0');
    constexpr uint32_t vtableRva = 0x1000;
    constexpr uint32_t tdFooRva  = 0x1100;
    constexpr uint32_t tdBarRva  = 0x1200;
    constexpr uint32_t tdBazRva  = 0x1300;
    constexpr uint32_t chdRva    = 0x1400;
    constexpr uint32_t bcaRva    = 0x1500;
    constexpr uint32_t bcdFooRva = 0x1600;
    constexpr uint32_t bcdBarRva = 0x1700;
    constexpr uint32_t bcdBazRva = 0x1800;
    constexpr uint32_t colRva    = 0x1900;

    // Vtable[-1] = COL VA; 5 method pointers; null terminator.
    writeAt<uint64_t>(rtti, vtableRva - 8, kImageBase + colRva);
    for (int i = 0; i < 5; i++) {
        uint64_t mva = kImageBase + 0x100 + (uint64_t)i * 0x10;
        writeAt<uint64_t>(rtti, vtableRva + (qsizetype)i * 8, mva);
    }
    writeAt<uint64_t>(rtti, vtableRva + 5 * 8, (uint64_t)0);

    auto writeTd = [&](uint32_t rva, const char* name) {
        writeAt<uint64_t>(rtti, rva + 0, 0xDEADBEEF);
        writeAt<uint64_t>(rtti, rva + 8, 0);
        writeCStr(rtti, rva + 16, name);
    };
    writeTd(tdFooRva, ".?AVFoo@@");
    writeTd(tdBarRva, ".?AVBar@@");
    writeTd(tdBazRva, ".?AVBaz@@");

    writeAt<uint32_t>(rtti, chdRva + 0x00, 0);
    writeAt<uint32_t>(rtti, chdRva + 0x04, 0);
    writeAt<uint32_t>(rtti, chdRva + 0x08, 3);
    writeAt<uint32_t>(rtti, chdRva + 0x0C, bcaRva);

    writeAt<uint32_t>(rtti, bcaRva + 0, bcdFooRva);
    writeAt<uint32_t>(rtti, bcaRva + 4, bcdBarRva);
    writeAt<uint32_t>(rtti, bcaRva + 8, bcdBazRva);

    writeAt<uint32_t>(rtti, bcdFooRva + 0, tdFooRva);
    writeAt<uint32_t>(rtti, bcdBarRva + 0, tdBarRva);
    writeAt<uint32_t>(rtti, bcdBazRva + 0, tdBazRva);

    writeAt<uint32_t>(rtti, colRva + 0x00, 1);
    writeAt<uint32_t>(rtti, colRva + 0x04, 0);
    writeAt<uint32_t>(rtti, colRva + 0x08, 0);
    writeAt<uint32_t>(rtti, colRva + 0x0C, tdFooRva);
    writeAt<uint32_t>(rtti, colRva + 0x10, chdRva);
    writeAt<uint32_t>(rtti, colRva + 0x14, (uint32_t)kImageBase);

    // Buffer layout:
    //   [0 ..       kImageBase)        zeros
    //   [kImageBase .. kImageBase+0x10000)  RTTI region
    //   [kStructBase .. kStructBase+0x1000) struct fields
    // We place kStructBase = 0x30000 so it sits past the RTTI region, and
    // size the buffer to cover both. (kStructBase here mirrors the constant
    // each test below uses.)
    constexpr uint64_t kStructBaseLayout = 0x30000;
    constexpr int kStructRegion = 0x1000;
    QByteArray full(kStructBaseLayout + kStructRegion, '\0');
    std::memcpy(full.data() + kImageBase, rtti.constData(), rtti.size());
    return full;
}

// Provider that wraps a BufferProvider and reports a single module covering
// the synthetic RTTI region. Counts enumerateModules() invocations so the
// test can assert the per-pass cache works.
class FakeModuleProvider : public BufferProvider {
public:
    FakeModuleProvider(QByteArray data, const QString& name)
        : BufferProvider(std::move(data), name) {}

    QVector<ModuleEntry> enumerateModules() const override {
        ++m_enumCalls;
        return { ModuleEntry{ QStringLiteral("synthetic.dll"),
                              QStringLiteral("synthetic.dll"),
                              kImageBase, 0x10000 } };
    }

    int enumCalls() const { return m_enumCalls; }
    void resetEnumCount() const { m_enumCalls = 0; }

private:
    mutable int m_enumCalls = 0;
};

// Build a minimal NodeTree with `nFields` Hex64 fields in a root struct.
// baseAddress is the address of the struct in the provider's address space.
NodeTree buildTreeWithHexFields(uint64_t baseAddress, int nFields) {
    NodeTree tree;
    tree.baseAddress = baseAddress;
    Node root;
    root.kind = NodeKind::Struct;
    root.name = QStringLiteral("Demo");
    root.structTypeName = QStringLiteral("Demo");
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;
    for (int i = 0; i < nFields; i++) {
        Node f;
        f.kind = NodeKind::Hex64;
        f.parentId = rootId;
        f.offset = i * 8;
        f.name = QStringLiteral("field_%1").arg(i);
        tree.addNode(f);
    }
    return tree;
}

} // anon

class TestRttiHint : public QObject {
    Q_OBJECT
private slots:

    void hintAttachesWhenValuePointsAtVtable() {
        QByteArray data = buildAddressSpaceWithRtti();
        // Plant the synthetic vtable VA at the struct's first qword.
        constexpr uint64_t kStructBase = 0x30000;
        uint64_t vtableVa = kImageBase + 0x1000;
        std::memcpy(data.data() + kStructBase, &vtableVa, 8);

        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));
        NodeTree tree = buildTreeWithHexFields(kStructBase, 4);

        ComposeResult r = compose(tree, prov);

        // Find the field at offset 0 (its absAddr == kStructBase).
        bool found = false;
        for (const auto& lm : r.meta) {
            if (lm.lineKind != LineKind::Field) continue;
            if (lm.nodeKind != NodeKind::Hex64) continue;
            if (lm.offsetAddr != kStructBase) continue;
            QVERIFY2(lm.rttiHintStart >= 0,
                "expected rttiHintStart on field whose value is a vtable");
            QVERIFY(lm.rttiHint.contains(QStringLiteral("Foo")));
            QCOMPARE(lm.rttiVtableAddr, vtableVa);
            found = true;
            break;
        }
        QVERIFY2(found, "did not locate the Hex64 field at offset 0");
    }

    void noHintWhenValueOutsideAnyModule() {
        QByteArray data = buildAddressSpaceWithRtti();
        // Plant a value that points outside any module.
        constexpr uint64_t kStructBase = 0x30000;
        uint64_t junk = 0xCAFEBABEDEADBEEFULL;
        std::memcpy(data.data() + kStructBase, &junk, 8);

        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));
        NodeTree tree = buildTreeWithHexFields(kStructBase, 1);

        ComposeResult r = compose(tree, prov);
        for (const auto& lm : r.meta) {
            if (lm.nodeKind == NodeKind::Hex64)
                QCOMPARE(lm.rttiHintStart, -1);
        }
    }

    void noHintForNullValue() {
        QByteArray data = buildAddressSpaceWithRtti();
        constexpr uint64_t kStructBase = 0x30000;
        // data is already zeroed at kStructBase, but be explicit.
        std::memset(data.data() + kStructBase, 0, 8);

        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));
        NodeTree tree = buildTreeWithHexFields(kStructBase, 1);

        ComposeResult r = compose(tree, prov);
        for (const auto& lm : r.meta) {
            if (lm.nodeKind == NodeKind::Hex64)
                QCOMPARE(lm.rttiHintStart, -1);
        }
    }

    void modulesEnumeratedFewTimesNotPerLine() {
        // Lay out 32 fields, each pointing at the same synthetic vtable.
        // Without caching, enumerateModules() would be called once per
        // candidate (32×) plus once inside each walkRtti success path —
        // i.e. >= 64 calls. With per-pass caching it must be a small
        // constant regardless of field count: one call from compose's
        // own cache, plus one inside walkRtti for the single unique
        // successful walk (whose RttiInfo is then memoized).
        constexpr int kFieldCount = 32;
        QByteArray data = buildAddressSpaceWithRtti();
        constexpr uint64_t kStructBase = 0x30000;
        uint64_t vtableVa = kImageBase + 0x1000;
        for (int i = 0; i < kFieldCount; i++)
            std::memcpy(data.data() + kStructBase + i * 8, &vtableVa, 8);

        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));
        NodeTree tree = buildTreeWithHexFields(kStructBase, kFieldCount);

        prov.resetEnumCount();
        ComposeResult r = compose(tree, prov);
        Q_UNUSED(r);
        // Must be O(1) in field count — assert generously (≤ 4) so future
        // implementation tweaks don't trip a brittle exact-match.
        QVERIFY2(prov.enumCalls() <= 4,
            qPrintable(QStringLiteral("enumerateModules called %1× for %2 fields — should be O(1)")
                .arg(prov.enumCalls()).arg(kFieldCount)));
        QVERIFY(prov.enumCalls() < kFieldCount);
    }

    void itaniumAutoDetectFallsBack() {
        // Build an Itanium-shaped vtable in a buffer; compose's MSVC walker
        // will reject it (no COL signature), then the Itanium fallback in
        // rttiForVtable picks it up. Mirrors the Itanium fixture from
        // test_rtti.cpp but stripped to the minimum needed to confirm the
        // fallback chain wires through.
        constexpr uint64_t kImg = 0x10000;
        constexpr uint32_t kVt   = 0x1000;
        constexpr uint32_t kTi   = 0x1100;
        constexpr uint32_t kName = 0x1180;
        constexpr uint32_t kTiVt = 0x1200;
        // Buffer must cover both the RTTI region [kImg .. kImg+0x10000)
        // AND the struct's address (kStructBase = 0x30000) below.
        QByteArray data(0x40000, '\0');
        auto wq = [&](uint64_t off, uint64_t v) {
            std::memcpy(data.data() + off, &v, 8);
        };
        // Vtable shape
        int64_t z = 0;
        std::memcpy(data.data() + kImg + kVt - 16, &z, 8);
        wq(kImg + kVt - 8, kImg + kTi);
        for (int i = 0; i < 5; i++) wq(kImg + kVt + i*8, kImg + 0x100 + i*0x10);
        // type_info
        wq(kImg + kTi + 0, kImg + kTiVt);
        wq(kImg + kTi + 8, kImg + kName);
        // Mangled name
        const char* mangled = "3Foo";
        std::memcpy(data.data() + kImg + kName, mangled, std::strlen(mangled) + 1);
        wq(kImg + kTiVt, 0xFEEDFACE);
        // Plant the vtable VA at struct offset 0 (well past RTTI region)
        constexpr uint64_t kStructBase = 0x30000;
        wq(kStructBase, kImg + kVt);

        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));
        NodeTree tree = buildTreeWithHexFields(kStructBase, 1);

        ComposeResult r = compose(tree, prov);
        bool found = false;
        for (const auto& lm : r.meta) {
            if (lm.nodeKind != NodeKind::Hex64) continue;
            if (lm.offsetAddr != kStructBase) continue;
            QVERIFY2(lm.rttiHintStart >= 0,
                "expected rttiHintStart from Itanium fallback");
            QVERIFY(lm.rttiHint.contains(QStringLiteral("Foo")));
            found = true;
        }
        QVERIFY(found);
    }

    void hintIndependentOfTypeHintsToggle() {
        // The auto-RTTI hint must fire even when typeHints is OFF — the
        // user might want to see RTTI without all the inference noise.
        QByteArray data = buildAddressSpaceWithRtti();
        constexpr uint64_t kStructBase = 0x30000;
        uint64_t vtableVa = kImageBase + 0x1000;
        std::memcpy(data.data() + kStructBase, &vtableVa, 8);

        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));
        NodeTree tree = buildTreeWithHexFields(kStructBase, 1);

        ComposeResult r = compose(tree, prov, /*viewRootId=*/0,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/false,
            /*showComments=*/false, /*symbolLookup=*/{});
        bool anyRtti = false;
        bool anyTypeHint = false;
        for (const auto& lm : r.meta) {
            if (lm.rttiHintStart >= 0) anyRtti = true;
            if (lm.typeHintStart >= 0) anyTypeHint = true;
        }
        QVERIFY2(anyRtti,  "RTTI hint should appear regardless of typeHints");
        QVERIFY2(!anyTypeHint, "typeHints=false should suppress green hints");
    }
};

QTEST_MAIN(TestRttiHint)
#include "test_rtti_hint.moc"
