// Tests for the four memory-source refresh speedups in
// controller.cpp / snapshot_provider.h:
//   1. viewport-bounded reads        — only re-read pages the user is looking at
//   2. per-page stability backoff    — pages that haven't changed in N ticks
//                                      get re-read at half rate
//   3. skip collapsed-pointer chases — already in collectPointerRanges,
//                                      regression-pin it here
//   4. permanent .rdata page cache   — pages in module-executable regions
//                                      are read once per attach, never again
//
// Plus the adaptive refresh interval (idle backoff + focus/visibility).

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QSplitter>
#include <Qsci/qsciscintilla.h>
#include <atomic>
#include <chrono>
#include "controller.h"
#include "providers/snapshot_provider.h"
#include "core.h"

using namespace rcx;

// ─────────────────────────────────────────────────────────────────────
// Test provider — counts read calls per page so we can assert *which
// pages* the controller decided to re-read on each tick. Two distinct
// "regions": a synthetic module image (executable) at base 0, and a
// synthetic data heap (writable, non-executable) starting after it.
// All reads otherwise behave like a flat 64 KB buffer.
// ─────────────────────────────────────────────────────────────────────
class CountingProvider : public Provider {
public:
    static constexpr uint64_t kModuleBase = 0;
    static constexpr uint64_t kModuleSize = 8 * 4096;     // 32 KB module image
    static constexpr uint64_t kHeapBase   = kModuleSize;  // 32 KB heap follows
    static constexpr uint64_t kHeapSize   = 8 * 4096;
    static constexpr int      kTotalSize  = (int)(kModuleSize + kHeapSize);

    mutable QHash<uint64_t, int> readsPerPage;  // page-aligned addr → count
    mutable std::atomic<int>     totalReads{0};
    QByteArray                   data{kTotalSize, '\0'};

    bool read(uint64_t addr, void* buf, int len) const override {
        if (addr + (uint64_t)len > (uint64_t)data.size()) return false;
        std::memcpy(buf, data.constData() + addr, len);
        // Tally by page so a single 4 KB readBytes call counts as one.
        readsPerPage[addr & ~uint64_t(4095)]++;
        totalReads.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    int size() const override { return (int)data.size(); }
    bool isLive() const override { return true; }
    QString name() const override { return QStringLiteral("counting"); }
    QString kind() const override { return QStringLiteral("Process"); }

    QVector<MemoryRegion> enumerateRegions() const override {
        QVector<MemoryRegion> r;
        // Module image — executable + read-only (.text/.rdata equivalent).
        // Speedup 4 marks pages here as permanent on first read.
        MemoryRegion mod;
        mod.base = kModuleBase;
        mod.size = kModuleSize;
        mod.readable = true;
        mod.writable = false;
        mod.executable = true;
        mod.moduleName = QStringLiteral("synthetic.dll");
        mod.type = RegionType::Image;
        r.append(mod);
        // Data heap — never marked permanent; mutates over the test run.
        MemoryRegion heap;
        heap.base = kHeapBase;
        heap.size = kHeapSize;
        heap.readable = true;
        heap.writable = true;
        heap.executable = false;
        heap.type = RegionType::Private;
        r.append(heap);
        return r;
    }

    void resetCounters() {
        readsPerPage.clear();
        totalReads = 0;
    }
};

// Build a tiny tree spanning both regions: a few fields in the heap
// (so refresh covers the heap pages), plus an optional pointer field
// whose target lives in the module. `pointerCollapsed` controls
// whether the pointer is expanded — needed because the controller
// starts ticking the moment its window is exposed, so any post-
// construction mutation races the first tick.
static void buildTree(NodeTree& tree, bool withPointer,
                      bool pointerCollapsed = true) {
    tree.baseAddress = CountingProvider::kHeapBase;

    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "T";
    root.name = "root";
    root.parentId = 0;
    root.offset = 0;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    auto field = [&](int off, NodeKind k, const char* name) -> uint64_t {
        Node n;
        n.kind = k;
        n.name = name;
        n.parentId = rootId;
        n.offset = off;
        int idx = tree.addNode(n);
        return tree.nodes[idx].id;
    };
    field(0,  NodeKind::UInt32, "u32");
    field(4,  NodeKind::UInt32, "next");

    if (withPointer) {
        Node target;
        target.kind = NodeKind::Struct;
        target.structTypeName = "Target";
        target.name = "Target";
        target.parentId = 0;
        target.offset = 0;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;
        // Give Target a real field — structSpan() returns 0 for an
        // empty struct and the controller's collectPointerRanges
        // bails on zero-span structs (no point reading nothing).
        Node tfield;
        tfield.kind = NodeKind::UInt32;
        tfield.name = "v";
        tfield.parentId = targetId;
        tfield.offset = 0;
        tree.addNode(tfield);

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "ptr";
        ptr.parentId = rootId;
        ptr.offset = 8;
        ptr.refId = targetId;
        ptr.collapsed = pointerCollapsed;
        tree.addNode(ptr);
    }
}

// ─────────────────────────────────────────────────────────────────────

class TestRefreshSpeedups : public QObject {
    Q_OBJECT
private:
    RcxDocument*        m_doc      = nullptr;
    RcxController*      m_ctrl     = nullptr;
    QSplitter*          m_splitter = nullptr;
    RcxEditor*          m_editor   = nullptr;
    CountingProvider*   m_prov     = nullptr;  // raw ptr — owned by m_doc->provider

    // Construct controller in a fully-initialized state so the very
    // first tick fires with the test's intended tree + provider data
    // — no race window between construction and the first refresh.
    void setupWithProvider(bool withPointer,
                           bool pointerCollapsed = true,
                           uint64_t pointerTargetAddr = 0) {
        m_doc = new RcxDocument();
        buildTree(m_doc->tree, withPointer, pointerCollapsed);
        auto prov = std::make_shared<CountingProvider>();
        m_prov = prov.get();
        // Pre-populate pointer bytes BEFORE the controller is born so
        // the first read in the snapshot already has the right target.
        // Pointer lives at root + offset 8 inside the heap region; the
        // CountingProvider's data array is addressed by absolute VA so
        // that's heap-base + 8 in array indices, not just +8.
        if (withPointer && pointerTargetAddr != 0) {
            std::memcpy(m_prov->data.data() + CountingProvider::kHeapBase + 8,
                        &pointerTargetAddr,
                        sizeof(pointerTargetAddr));
        }
        m_doc->provider = prov;

        m_splitter = new QSplitter();
        m_ctrl = new RcxController(m_doc, nullptr);
        m_editor = m_ctrl->addSplitEditor(m_splitter);
        m_splitter->resize(800, 600);
        m_splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_splitter));
        QApplication::processEvents();
        // Drive the timer aggressively so each test runs in <1 s.
        m_ctrl->setRefreshInterval(50);
    }

    // Block until at least one async refresh round-trip lands in the
    // controller. Returns true on success, false on timeout.
    bool waitForOneTick(int timeoutMs = 1500) {
        QSignalSpy sigSpy(m_ctrl, &RcxController::sourceLivenessChanged);
        Q_UNUSED(sigSpy);
        // Easier: spin the event loop until totalReads jumps.
        int before = m_prov->totalReads.load();
        QElapsedTimer t; t.start();
        while (t.elapsed() < timeoutMs) {
            QTest::qWait(10);
            QApplication::processEvents();
            if (m_prov->totalReads.load() > before) {
                // One read happened — let onReadComplete drain too.
                QTest::qWait(20);
                QApplication::processEvents();
                return true;
            }
        }
        return false;
    }

private slots:
    void cleanup() {
        delete m_ctrl;     m_ctrl = nullptr;
        m_editor = nullptr;
        delete m_splitter; m_splitter = nullptr;
        delete m_doc;      m_doc = nullptr;
        m_prov = nullptr;
    }

    // ── Speedup 4: permanent .rdata cache ───────────────────────────
    // After the first refresh, pages in the module's executable region
    // get marked permanent on the SnapshotProvider. Subsequent ticks
    // must not re-read them.
    void permanentPagesMarkedAfterModuleRead() {
        // Build with pointer ALREADY uncollapsed and target bytes
        // already written so as soon as the controller's
        // collectPointerRanges runs (tick 2 — tick 1 doesn't have a
        // snapshot to read pointer values from yet) the module page
        // gets requested and classifyPermanentPages catches it.
        uint64_t ptrTargetAddr = CountingProvider::kModuleBase + 4096;
        setupWithProvider(/*withPointer=*/true,
                          /*pointerCollapsed=*/false,
                          /*pointerTargetAddr=*/ptrTargetAddr);
        // Tick 1: bootstraps snapshot from the main struct extent.
        // Tick 2: collectPointerRanges adds the module page (pointer
        //         is uncollapsed and target span > 0).
        // Tick 3: module page is now permanent → skipped.
        QVERIFY(waitForOneTick());            // tick 1
        QTest::qWait(120);
        QApplication::processEvents();
        QVERIFY(waitForOneTick(2000));        // tick 2 — module page read
        QTest::qWait(120);
        QApplication::processEvents();
        QVERIFY(m_ctrl->snapshotProv() != nullptr);
        QVERIFY2(m_ctrl->snapshotProv()->isPermanent(ptrTargetAddr & ~uint64_t(4095)),
                 "module page should be marked permanent after tick 2");
        // Tick 3+: module page must NOT be re-read.
        m_prov->resetCounters();
        QVERIFY(waitForOneTick(2000));
        QTest::qWait(120);
        QApplication::processEvents();
        int modulePageReads = m_prov->readsPerPage.value(ptrTargetAddr & ~uint64_t(4095), 0);
        QCOMPARE(modulePageReads, 0);
    }

    // ── Speedup 3: collapsed pointers don't have their targets read ─
    // Pre-existing behavior in collectPointerRanges (controller.cpp:5408).
    // Pin it: with the pointer collapsed, the module page is not requested.
    void collapsedPointerSkipsTarget() {
        setupWithProvider(/*withPointer=*/true);
        // Pointer at offset 8 points into the module image, but stays
        // collapsed. The module page must never appear in read calls.
        uint64_t ptrTargetAddr = CountingProvider::kModuleBase + 4096;
        std::memcpy(m_prov->data.data() + 8, &ptrTargetAddr, sizeof(ptrTargetAddr));
        QVERIFY(waitForOneTick());
        QTest::qWait(120);
        QApplication::processEvents();
        int modulePageReads = m_prov->readsPerPage.value(ptrTargetAddr & ~uint64_t(4095), 0);
        QCOMPARE(modulePageReads, 0);
    }

    // ── Speedup 2: per-page stability backoff ───────────────────────
    // If a heap page goes >= kStabilityThreshold ticks unchanged, it
    // becomes "stable" and re-reads halve. We assert the stability
    // counter climbs as expected when bytes stay constant.
    void pageStabilityClimbsWhenIdle() {
        setupWithProvider(/*withPointer=*/false);
        uint64_t heapPage = CountingProvider::kHeapBase & ~uint64_t(4095);
        // Drive several ticks with no data mutation.
        for (int i = 0; i < 6; ++i) {
            QVERIFY(waitForOneTick());
        }
        // Stability counter for the heap page should be > 0; exact value
        // depends on how many real ticks landed (race with QTest timing),
        // but it must be at least 1.
        QVERIFY(m_ctrl->pageStability(heapPage) >= 1);
    }

    // ── Speedup 1: viewport-bounded reads ──────────────────────────
    // After the initial first-snapshot tick, only pages that intersect
    // the visible viewport (plus a 2-page overscan) should be re-read.
    // The heap is 8 pages; the viewport on a 600px window only covers
    // a couple of lines at offset 0, so far-end heap pages should NOT
    // be re-read on tick 2+.
    //
    // Note: this is "should generally hold" — depending on font size
    // and window size the viewport could span more. We assert that at
    // least one page beyond viewport+overscan is skipped.
    void viewportBoundsReReads() {
        setupWithProvider(/*withPointer=*/false);
        // First tick: read everything (firstSnapshot path).
        QVERIFY(waitForOneTick());
        QTest::qWait(60);
        // Subsequent tick: should skip far-end heap pages.
        m_prov->resetCounters();
        QVERIFY(waitForOneTick(2000));
        QTest::qWait(60);
        QApplication::processEvents();
        // The last heap page is well outside the viewport+overscan
        // window for an 800x600 editor on the first few lines.
        uint64_t lastHeapPage = (CountingProvider::kHeapBase
                                  + CountingProvider::kHeapSize - 4096);
        int reads = m_prov->readsPerPage.value(lastHeapPage, 0);
        QCOMPARE(reads, 0);
    }

    // ── Adaptive refresh: focus / visibility / idle backoff ────────
    void adaptiveBackoffWidensInterval() {
        setupWithProvider(/*withPointer=*/false);
        m_ctrl->setRefreshInterval(50);  // base = 50 ms
        // Force many idle ticks to trip kIdleBackoffTicks (8).
        for (int i = 0; i < 12; ++i) {
            QVERIFY(waitForOneTick());
        }
        // Interval should have widened past base.
        QVERIFY(m_ctrl->refreshIntervalMs() > 50);
    }

    void focusOutWidensInterval() {
        setupWithProvider(/*withPointer=*/false);
        m_ctrl->setRefreshInterval(50);
        m_ctrl->setWindowState(/*focused=*/false, /*visible=*/true);
        // Blur cap is max(base, 1500) = 1500 here.
        QCOMPARE(m_ctrl->refreshIntervalMs(), 1500);
        QVERIFY(m_ctrl->refreshTimerActive());
    }

    void minimizePausesTimer() {
        setupWithProvider(/*withPointer=*/false);
        m_ctrl->setRefreshInterval(50);
        QVERIFY(m_ctrl->refreshTimerActive());
        m_ctrl->setWindowState(/*focused=*/false, /*visible=*/false);
        QVERIFY(!m_ctrl->refreshTimerActive());
        // Restore visibility → timer resumes.
        m_ctrl->setWindowState(/*focused=*/true, /*visible=*/true);
        QVERIFY(m_ctrl->refreshTimerActive());
        QCOMPARE(m_ctrl->refreshIntervalMs(), 50);
    }

    // ── SnapshotProvider permanent-page primitives (pure unit) ─────
    void snapshotProviderPermanentSet() {
        SnapshotProvider sp(/*real=*/{}, {}, /*mainExtent=*/0);
        QVERIFY(!sp.isPermanent(0x1000));
        sp.markPermanent(0x1000 + 17);          // page-aligns the input
        QVERIFY(sp.isPermanent(0x1000));
        QVERIFY(sp.isPermanent(0x1FFF));
        QVERIFY(!sp.isPermanent(0x2000));
        sp.clearPermanent();
        QVERIFY(!sp.isPermanent(0x1000));
    }

    void snapshotProviderMergeKeepsExisting() {
        SnapshotProvider::PageMap initial;
        initial[0x0000] = QByteArray(4096, '\xAA');
        initial[0x1000] = QByteArray(4096, '\xBB');
        SnapshotProvider sp(/*real=*/{}, initial, /*mainExtent=*/8192);

        // Merge a fresh page that overwrites only one slot — the other
        // existing page must survive (Speedup 1: skipped pages keep
        // their previous bytes).
        SnapshotProvider::PageMap fresh;
        fresh[0x1000] = QByteArray(4096, '\xCC');
        sp.mergePages(fresh, /*mainExtent=*/8192);

        char buf[4];
        QVERIFY(sp.read(0x0000, buf, 4));
        QCOMPARE((unsigned char)buf[0], (unsigned char)0xAA);
        QVERIFY(sp.read(0x1000, buf, 4));
        QCOMPARE((unsigned char)buf[0], (unsigned char)0xCC);
    }
};

QTEST_MAIN(TestRefreshSpeedups)
#include "test_refresh_speedups.moc"
