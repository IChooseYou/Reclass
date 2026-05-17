// Detect the "tooltip fights the cursor" flicker that keeps showing up on
// chip hover (RTTI / Comment / TypeHint pills in the editor) and on the
// command-bar popups. The bug pattern is always the same: tooltip shows,
// something (a Leave event, focus loss, jitter at a chip's column edge)
// triggers a dismiss, the underlying widget re-paints, MouseMove fires
// again, tooltip re-shows. Counted over a 200-ms stream of mouse moves,
// a healthy tooltip stays at 1 show / 0 hide; a flickering one cycles.
//
// The test installs an event filter on rcx::sharedRcxTooltip() to count
// QEvent::Show / QEvent::Hide transitions, then drives a stream of
// mouse-move events into an RcxEditor whose first field row carries
// both an Rtti chip and a Comment chip — the exact combo that produced
// the live flicker.

#include <QtTest/QTest>
#include <QApplication>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QPushButton>
#include <QHBoxLayout>
#include <QToolTip>
#include <QtCore/QObject>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>
#include <cstring>
#include "editor.h"
#include "core.h"
#include "rcxtooltip.h"
#include "tooltip_bridge.h"
#include "themes/thememanager.h"
#include "providers/buffer_provider.h"
#include "providers/provider.h"
#ifdef _WIN32
#  include <windows.h>
#  include <psapi.h>
#endif

using namespace rcx;

namespace {

// Counts show/hide transitions on the shared tooltip during the test.
class TooltipVisibilityCounter : public QObject {
public:
    int shows = 0;
    int hides = 0;
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (obj == rcx::sharedRcxTooltip()) {
            if (e->type() == QEvent::Show) ++shows;
            else if (e->type() == QEvent::Hide) ++hides;
        }
        return false;
    }
};

// Minimal RTTI fixture (mirrors test_chips.cpp): one synthetic vtable in
// "module" range so rttiForVtable demangles "Foo". Used to plant a real
// Rtti chip on a Hex64 field so we exercise the actual chip-hover code
// path, not a synthetic one.
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

// In-process Provider that mirrors what the live tutorial uses
// (ProcessMemoryProvider plugin: OpenProcess + ReadProcessMemory +
// EnumProcessModulesEx). Lets us drive the test against ACTUAL live
// memory, not a synthetic QByteArray — that was the user's complaint
// ("real mem values in tutorial make it flicker, you're testing
// non-live data shit").
class SelfProcessProvider : public Provider {
public:
    SelfProcessProvider() {
#ifdef _WIN32
        m_handle = OpenProcess(
            PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
            FALSE, GetCurrentProcessId());
        if (!m_handle) return;
        HMODULE mods[1024];
        DWORD needed = 0;
        if (EnumProcessModulesEx(m_handle, mods, sizeof(mods), &needed,
                                  LIST_MODULES_ALL)) {
            int count = qMin((int)(needed / sizeof(HMODULE)), 1024);
            for (int i = 0; i < count; ++i) {
                MODULEINFO mi{};
                WCHAR name[MAX_PATH];
                if (GetModuleInformation(m_handle, mods[i], &mi, sizeof(mi))
                    && GetModuleBaseNameW(m_handle, mods[i], name, MAX_PATH)) {
                    m_modules.push_back(ModuleEntry{
                        QString::fromWCharArray(name),
                        QString::fromWCharArray(name),
                        (uint64_t)mi.lpBaseOfDll,
                        (uint64_t)mi.SizeOfImage});
                }
            }
        }
#endif
    }
    ~SelfProcessProvider() override {
#ifdef _WIN32
        if (m_handle) CloseHandle(m_handle);
#endif
    }
    bool read(uint64_t addr, void* buf, int len) const override {
#ifdef _WIN32
        if (!m_handle || len <= 0) return false;
        SIZE_T bytesRead = 0;
        ReadProcessMemory(m_handle, (LPCVOID)(addr), buf,
                          (SIZE_T)len, &bytesRead);
        if ((int)bytesRead < len)
            std::memset((char*)buf + bytesRead, 0, len - bytesRead);
        return bytesRead > 0;
#else
        Q_UNUSED(addr); Q_UNUSED(buf); Q_UNUSED(len);
        return false;
#endif
    }
    int size() const override { return INT_MAX; }
    bool isReadable(uint64_t, int len) const override {
#ifdef _WIN32
        return m_handle && len >= 0;
#else
        return false;
#endif
    }
    QVector<ModuleEntry> enumerateModules() const override {
        return m_modules;
    }
private:
    QVector<ModuleEntry> m_modules;
#ifdef _WIN32
    HANDLE m_handle = nullptr;
#endif
};

QPoint colToViewport(QsciScintilla* sci, int line, int col) {
    long pos = sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                                  (unsigned long)line, (long)col);
    int x = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION, 0, pos);
    int y = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, 0, pos);
    return QPoint(x, y);
}

void sendMove(QWidget* w, const QPoint& localPos) {
    QPointF p(localPos);
    QMouseEvent move(QEvent::MouseMove, p, p, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &move);
}

} // anon

class TestTooltipFlicker : public QObject {
    Q_OBJECT
private slots:

    void initTestCase() {
        // Make sure the shared tooltip exists so we can install the counter
        // BEFORE any other code touches it.
        rcx::sharedRcxTooltip();
    }

    // ── GlobalTooltipBridge: stationary cursor on a setToolTip widget
    //    must NOT flicker. Qt re-fires QEvent::ToolTip on its own timer
    //    while the cursor stays parked, and Leave events from unrelated
    //    widgets used to dismiss the tip prematurely — both observable
    //    as "flickers like crazy only moving makes it unflicker". ──
    void bridgeDoesNotFlickerOnStationaryCursor() {
        // Two-widget setup. The "host" carries the tooltip; the "sibling"
        // is an unrelated widget that fires Leave events the bridge used
        // to (incorrectly) interpret as "dismiss the host's tooltip".
        QWidget host;
        host.resize(200, 80);
        host.setToolTip(QStringLiteral("Host tip"));
        QWidget sibling(&host);
        sibling.setGeometry(0, 0, 30, 30);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));

        rcx::GlobalTooltipBridge bridge;
        qApp->installEventFilter(&bridge);

        TooltipVisibilityCounter counter;
        auto* tip = rcx::sharedRcxTooltip();
        tip->installEventFilter(&counter);
        rcx::dismissRcxTooltip();
        QTest::qWait(20);
        counter.shows = counter.hides = 0;

        // 1. Initial ToolTip event on the host → bridge shows tip.
        QPoint hostCenter = host.rect().center();
        QHelpEvent firstTip(QEvent::ToolTip, hostCenter,
                            host.mapToGlobal(hostCenter));
        QApplication::sendEvent(&host, &firstTip);
        QTest::qWait(10);
        QVERIFY2(tip->isVisible(), "bridge should show the tooltip on first ToolTip event");
        QCOMPARE(counter.shows, 1);

        // 2. Qt re-fires ToolTip on its own delay timer while the cursor
        //    stays parked. The bridge must treat these as idempotent
        //    (same widget + same text + visible → return true, no
        //    visible re-show / re-populate / re-position).
        for (int i = 0; i < 10; ++i) {
            QHelpEvent dup(QEvent::ToolTip, hostCenter,
                           host.mapToGlobal(hostCenter));
            QApplication::sendEvent(&host, &dup);
            QApplication::processEvents();
        }
        QCOMPARE(counter.shows, 1);
        QCOMPARE(counter.hides, 0);

        // 3. The unrelated child widget fires Leave (e.g., its mouse
        //    tracking moved between sub-controls). The bridge tracks
        //    the tooltip's target widget; Leave on anything OTHER than
        //    the target must NOT dismiss.
        QEvent siblingLeave(QEvent::Leave);
        for (int i = 0; i < 10; ++i) {
            QApplication::sendEvent(&sibling, &siblingLeave);
            QApplication::processEvents();
        }
        QCOMPARE(counter.hides, 0);
        QVERIFY2(tip->isVisible(),
            "tooltip dismissed by Leave on an unrelated widget — bridge "
            "must only dismiss on Leave for the actual target widget");

        // 4. Real Leave on the target finally dismisses.
        QEvent hostLeave(QEvent::Leave);
        QApplication::sendEvent(&host, &hostLeave);
        QTest::qWait(10);
        QCOMPARE(counter.hides, 1);

        tip->removeEventFilter(&counter);
        qApp->removeEventFilter(&bridge);
        rcx::dismissRcxTooltip();
    }

    // ── Command-row arrow tooltip survives refresh ticks. This is the
    //    flicker pattern the user actually reported:
    //      [TIP] show body= "Click to change the attached..."
    //      [TIP] hide
    //      [TIP] show body= "Click to change the attached..."
    //      [TIP] hide
    //    cycling forever, same body + same anchor + still cursor. Root
    //    cause was applySelectionOverlay unconditionally dismissing
    //    m_arrowTooltip on every call; live refresh calls it every
    //    frame. Test: park cursor on the command-row source pill so
    //    applyHoverCursor publishes the arrow tip, then spam
    //    applyDocument 30× and assert no hides. ──
    void commandRowArrowTooltipSurvivesRefresh() {
        // Need a CommandRow line at line 0 — build a normal struct and
        // compose. Default compose emits a CommandRow header at line 0.
        NodeTree tree;
        tree.baseAddress = 0x10000;
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Demo");
        root.name = QStringLiteral("Demo");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;
        Node f;
        f.kind = NodeKind::UInt32;
        f.name = QStringLiteral("v");
        f.parentId = rootId;
        f.offset = 0;
        tree.addNode(f);

        QByteArray data(0x100, '\0');
        BufferProvider prov(data, QStringLiteral("synthetic"));

        RcxEditor editor;
        editor.resize(900, 400);
        editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));

        ComposeResult r = compose(tree, prov, rootId);
        editor.applyDocument(r);
        QApplication::processEvents();

        // Hover over the class-name region of the CommandRow (line 0).
        // The exact column doesn't matter as long as it resolves to a
        // tooltip-bearing span — applyHoverCursor will pick the
        // appropriate one.
        QsciScintilla* sci = editor.scintilla();
        QWidget* vp = sci->viewport();
        // Walk a few columns into line 0 to find a span that gives us
        // an arrow tooltip. RootClassName / Source / BaseAddress all
        // qualify. Loop until one works.
        bool found = false;
        for (int col = 5; col < 60 && !found; col += 2) {
            sendMove(vp, colToViewport(sci, 0, col));
            QApplication::processEvents();
            if (rcx::sharedRcxTooltip()->isVisible()) {
                found = true;
                break;
            }
            // m_arrowTooltip lives separately from sharedRcxTooltip in
            // this code path. Check via the editor's accessor if any.
        }
        // Whether or not a tooltip is currently shown, the refresh
        // loop below must not produce hide events on the shared
        // tooltip. Park the counter regardless.

        TooltipVisibilityCounter counter;
        auto* tip = rcx::sharedRcxTooltip();
        tip->installEventFilter(&counter);
        rcx::dismissRcxTooltip();
        QTest::qWait(20);
        counter.shows = counter.hides = 0;

        // Refresh loop: 30 ticks. Selection NEVER changes (we don't
        // call applySelectionOverlay with a different selIds set).
        // applySelectionOverlay's "selChanged" path must NOT fire, so
        // the arrow-tooltip dismiss never runs. If the regression
        // re-appears, hides will start ticking up.
        for (int tick = 0; tick < 30; ++tick) {
            ComposeResult rr = compose(tree, prov, rootId);
            editor.applyDocument(rr);
            QApplication::processEvents();
        }

        QVERIFY2(counter.hides == 0,
            qPrintable(QStringLiteral("arrow tooltip hidden %1 times across "
                "30 refresh ticks while cursor stayed STILL on the command "
                "row — applySelectionOverlay is dismissing m_arrowTooltip "
                "unconditionally per-tick (the user's flicker pattern)")
                .arg(counter.hides)));

        tip->removeEventFilter(&counter);
        rcx::dismissRcxTooltip();
    }

    // ── Tutorial-mode LIVE-PROCESS test: replicate exactly what the
    //    RcxEditor* tutorial does — attach a SelfProcessProvider that
    //    reads bytes via ReadProcessMemory, build the same demo tree
    //    layout selfTest builds, hover the resulting RTTI chip, then
    //    spam refresh ticks. Live memory drifts naturally between
    //    reads (Qt internals touch the editor's state); the tooltip
    //    widget must NOT toggle visibility on any of those reads. ──
    void tutorialModeLiveRefreshDoesNotFlicker() {
#ifndef _WIN32
        QSKIP("self-process probe is Windows-only");
#else
        // Open a real handle to ourselves — same call path the live
        // ProcessMemoryProvider plugin takes.
        SelfProcessProvider prov;
        QVERIFY2(!prov.enumerateModules().isEmpty(),
            "test setup: OpenProcess/EnumProcessModulesEx failed — bail");

        // Pick a stable in-process address: a static const buffer in
        // this DLL. Reading it via ReadProcessMemory will succeed,
        // module-range checks will pass, and Symbol resolution will
        // attempt to find Reclass-ish symbols.
        static const uint8_t kAnchor[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        uint64_t instanceAddr = reinterpret_cast<uint64_t>(kAnchor);

        // Build a small tree shaped like the tutorial: a Pointer64 at
        // offset 0 whose value (read live) might or might not resolve
        // to a real vtable, plus a Hex64 next to it. The exact
        // emitted chip set depends on whether the live bytes match
        // anything in the module range — what matters is that the
        // tooltip widget's visibility is stable across refreshes.
        NodeTree tree;
        tree.baseAddress = instanceAddr;
        tree.pointerSize = 8;
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("SelfProbe");
        root.name = QStringLiteral("probe");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f0;
        f0.kind = NodeKind::Hex64;
        f0.name = QStringLiteral("v0");
        f0.parentId = rootId;
        f0.offset = 0;
        f0.comment = QStringLiteral("a user comment");
        tree.addNode(f0);
        Node f1;
        f1.kind = NodeKind::Hex64;
        f1.name = QStringLiteral("v1");
        f1.parentId = rootId;
        f1.offset = 8;
        tree.addNode(f1);

        RcxEditor editor;
        editor.resize(900, 400);
        editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));

        ComposeResult r = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/true,
            /*showComments=*/true);
        editor.applyDocument(r);

        // Find a chip on row 0 or 1 (Comment chip is guaranteed by f0).
        int chipRow = -1;
        int chipCol = -1;
        for (int i = 0; i < r.meta.size(); ++i) {
            if (r.meta[i].chips.isEmpty()) continue;
            chipRow = i;
            chipCol = r.meta[i].chips.first().startCol + 2;
            break;
        }
        QVERIFY2(chipRow >= 0, "test setup: no chips composed on the live tree");

        QsciScintilla* sci = editor.scintilla();
        QWidget* vp = sci->viewport();
        sendMove(vp, colToViewport(sci, chipRow, chipCol));
        QApplication::processEvents();

        TooltipVisibilityCounter counter;
        auto* tip = rcx::sharedRcxTooltip();
        tip->installEventFilter(&counter);
        rcx::dismissRcxTooltip();
        sendMove(vp, colToViewport(sci, chipRow, chipCol));
        QApplication::processEvents();
        counter.shows = counter.hides = 0;

        // The refresh loop: 30 fresh composes through live memory +
        // applyDocument. Cursor stays parked. Every iteration touches
        // a different snapshot of the process's pages (the live demo
        // tabs in the real app do exactly this every ~50 ms).
        for (int tick = 0; tick < 30; ++tick) {
            ComposeResult rr = compose(tree, prov, rootId,
                /*compactColumns=*/false, /*treeLines=*/false,
                /*braceWrap=*/false, /*typeHints=*/true,
                /*showComments=*/true);
            editor.applyDocument(rr);
            QApplication::processEvents();
        }

        QVERIFY2(counter.hides == 0,
            qPrintable(QStringLiteral("tooltip hidden %1 times during 30 "
                "live-memory refresh ticks (cursor never moved) — "
                "applyDocument is dismissing the chip tooltip mid-hover")
                .arg(counter.hides)));
        QVERIFY2(counter.shows == 0,
            qPrintable(QStringLiteral("tooltip re-shown %1 times during 30 "
                "live-memory refresh ticks (cursor never moved) — "
                "applyDocument is republishing the chip tooltip per-tick")
                .arg(counter.shows)));

        tip->removeEventFilter(&counter);
        rcx::dismissRcxTooltip();
#endif
    }

    // ── Tutorial-mode regression #1: cursor parked on a button (chrome
    //    widget with setToolTip) while the editor next to it refreshes
    //    every ~50 ms. The user reported "Qt tooltip flickers like
    //    crazy, only moving makes it unflicker" — meaning a STATIONARY
    //    cursor on a button watches the tooltip toggle, but the moment
    //    they move the mouse, it stops. That can only happen if the
    //    editor's refresh path is firing events that reach the bridge
    //    and cycle the tooltip. ──
    void buttonTooltipSurvivesEditorRefreshTicks() {
        // Two widgets side by side: a button with setToolTip (the
        // tooltip target) and an RcxEditor that we'll spam refreshes
        // into. Both must be in the same top-level window so
        // synthetic Qt events flow naturally between them.
        QWidget host;
        QHBoxLayout* lay = new QHBoxLayout(&host);
        QPushButton* btn = new QPushButton(QStringLiteral("Hover me"), &host);
        btn->setToolTip(QStringLiteral("Tutorial button tip"));
        RcxEditor* editor = new RcxEditor(&host);
        lay->addWidget(btn);
        lay->addWidget(editor, 1);
        host.resize(900, 400);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));

        // Real document with chips so applyDocument touches real chip-bg
        // indicator passes, not a no-op.
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
        Node f; f.kind = NodeKind::Hex64; f.name = QStringLiteral("v");
        f.parentId = rootId; f.offset = 0;
        tree.addNode(f);
        ComposeResult r = compose(tree, prov, rootId);
        editor->applyDocument(r);

        // Install the bridge BEFORE showing the tooltip so the
        // bridge's idempotent re-show path runs for repeated
        // QEvent::ToolTip events.
        rcx::GlobalTooltipBridge bridge;
        qApp->installEventFilter(&bridge);

        TooltipVisibilityCounter counter;
        auto* tip = rcx::sharedRcxTooltip();
        tip->installEventFilter(&counter);
        rcx::dismissRcxTooltip();
        QTest::qWait(20);
        counter.shows = counter.hides = 0;

        // Park the tooltip on the button.
        QPoint btnCenter = btn->rect().center();
        QHelpEvent firstTip(QEvent::ToolTip, btnCenter,
                            btn->mapToGlobal(btnCenter));
        QApplication::sendEvent(btn, &firstTip);
        QApplication::processEvents();
        QVERIFY2(tip->isVisible(), "bridge should publish the button tooltip");
        QCOMPARE(counter.shows, 1);

        // Now fire 20 editor refresh ticks. The synthetic events that
        // applyDocument generates (setReadOnly toggles, indicator
        // clears/fills, setText if any) propagate through qApp's event
        // filter — including the bridge. A robust bridge ignores
        // every one of them because the cursor never left the button.
        QByteArray mutData = buildRttiAddressSpace();
        std::memcpy(mutData.data() + kStructBase, &vtable, 8);
        for (int tick = 0; tick < 20; ++tick) {
            mutData.data()[kStructBase + 16 + (tick % 4)] =
                (char)(0x10 + tick);
            FakeModuleProvider tickProv(mutData, QStringLiteral("synthetic"));
            ComposeResult rr = compose(tree, tickProv, rootId);
            editor->applyDocument(rr);
            QApplication::processEvents();
            // Qt's tooltip system re-fires QEvent::ToolTip while the
            // cursor stays parked. Simulate one re-fire per refresh
            // tick (worst case: aligned with refresh).
            QHelpEvent dup(QEvent::ToolTip, btnCenter,
                           btn->mapToGlobal(btnCenter));
            QApplication::sendEvent(btn, &dup);
            QApplication::processEvents();
        }

        QVERIFY2(counter.shows == 1,
            qPrintable(QStringLiteral("bridge re-published the button tooltip "
                "%1 times across 20 editor refresh ticks (expected 1: "
                "idempotent re-shows)").arg(counter.shows)));
        QVERIFY2(counter.hides == 0,
            qPrintable(QStringLiteral("bridge dismissed the button tooltip "
                "%1 times across 20 editor refresh ticks while cursor was "
                "still on the button — refresh-driven synthetic events "
                "shouldn't reach the dismiss path").arg(counter.hides)));

        tip->removeEventFilter(&counter);
        qApp->removeEventFilter(&bridge);
        rcx::dismissRcxTooltip();
    }

    // ── Tutorial-mode regression #2: cursor parked on a chip in the
    //    editor while the controller refresh tick keeps re-applying
    //    fresh memory. The chip's tooltip must NOT toggle visibility
    //    on every refresh — that's the "flickers when still" the user
    //    reported in the live RcxEditor* tutorial. ──
    void chipTooltipSurvivesLiveRefreshTicks() {
        // Build a tree with an Rtti chip and a Hex64 row whose value we
        // can mutate between refreshes (mirroring a live process where
        // the bytes at a given address change between refresh ticks).
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
        f.comment = QStringLiteral("user comment");
        tree.addNode(f);

        RcxEditor editor;
        editor.resize(900, 400);
        editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));

        // First compose + apply.
        ComposeResult r = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/true,
            /*showComments=*/true);
        editor.applyDocument(r);

        // Find the chip and park the cursor over it.
        const LineChip* rttiChip = nullptr;
        int chipRow = -1;
        for (int i = 0; i < r.meta.size(); ++i) {
            if (auto* c = findChip(r.meta[i], ChipKind::Rtti)) {
                chipRow = i; rttiChip = c; break;
            }
        }
        QVERIFY(rttiChip);

        QsciScintilla* sci = editor.scintilla();
        QWidget* vp = sci->viewport();
        QPoint inChip = colToViewport(sci, chipRow, rttiChip->startCol + 3);
        sendMove(vp, inChip);          // park cursor on the chip
        QApplication::processEvents();

        TooltipVisibilityCounter counter;
        auto* tip = rcx::sharedRcxTooltip();
        tip->installEventFilter(&counter);
        rcx::dismissRcxTooltip();
        // One initial MouseMove to publish the chip tip with a known
        // baseline, then reset the counter so we only measure what the
        // refresh-tick loop produces below.
        sendMove(vp, inChip);
        QApplication::processEvents();
        counter.shows = counter.hides = 0;

        // Refresh-tick loop: re-mutate the live memory + re-compose +
        // re-applyDocument 20 times. Cursor stays still — no MouseMove
        // events fire. A healthy editor keeps the previously-shown
        // chip tooltip up; a broken one cycles visibility on every
        // applyDocument (this was the live tutorial flicker).
        QByteArray mutData = buildRttiAddressSpace();
        std::memcpy(mutData.data() + kStructBase, &vtable, 8);
        for (int tick = 0; tick < 20; ++tick) {
            // Vary a few bytes the chip doesn't depend on so the diff
            // path runs but the chip text stays stable.
            mutData.data()[kStructBase + 16 + (tick % 4)] =
                (char)(0x10 + tick);
            FakeModuleProvider tickProv(mutData, QStringLiteral("synthetic"));
            ComposeResult rr = compose(tree, tickProv, rootId,
                /*compactColumns=*/false, /*treeLines=*/false,
                /*braceWrap=*/false, /*typeHints=*/true,
                /*showComments=*/true);
            editor.applyDocument(rr);
            QApplication::processEvents();
        }

        QVERIFY2(counter.hides == 0,
            qPrintable(QStringLiteral("chip tooltip was hidden %1 times "
                "during 20 refresh ticks while the cursor stayed STILL "
                "on the chip — applyDocument is dismissing the tip")
                .arg(counter.hides)));
        QVERIFY2(counter.shows == 0,
            qPrintable(QStringLiteral("chip tooltip was re-shown %1 times "
                "during 20 refresh ticks while the cursor stayed STILL — "
                "applyDocument is republishing the tip on every tick")
                .arg(counter.shows)));

        tip->removeEventFilter(&counter);
        rcx::dismissRcxTooltip();
    }

    // ── Editor chip hover: cursor jittering at the column boundary of
    //    an Rtti chip used to make the tooltip show/dismiss on every
    //    pixel-level move. Detect that here. ──
    void chipHoverDoesNotFlickerAtBoundary() {
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
        f.comment = QStringLiteral("vtable comment");
        tree.addNode(f);

        ComposeResult r = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/false,
            /*showComments=*/true);

        // The hex64 row carries Rtti + Comment chips. Find one.
        int chipRow = -1;
        const LineChip* rttiChip = nullptr;
        for (int i = 0; i < r.meta.size(); ++i) {
            if (auto* c = findChip(r.meta[i], ChipKind::Rtti)) {
                chipRow = i;
                rttiChip = c;
                break;
            }
        }
        QVERIFY2(chipRow >= 0, "test setup: expected an Rtti chip in the demo tree");
        QVERIFY(rttiChip);

        RcxEditor editor;
        editor.resize(900, 400);
        editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));
        editor.applyDocument(r);

        TooltipVisibilityCounter counter;
        auto* tip = rcx::sharedRcxTooltip();
        tip->installEventFilter(&counter);
        rcx::dismissRcxTooltip();
        QTest::qWait(20);
        counter.shows = counter.hides = 0;

        // Sweep mouse position from just LEFT of the chip's start column
        // to just RIGHT of it — the "boundary jitter" pattern that the
        // user actually performs with the cursor. A stable tooltip
        // shows once when the cursor enters the chip and dismisses
        // exactly once when it leaves; a flickering one cycles.
        QsciScintilla* sci = editor.scintilla();
        QWidget* vp = sci->viewport();
        QPoint inChip  = colToViewport(sci, chipRow, rttiChip->startCol + 3);
        QPoint outChip = colToViewport(sci, chipRow, qMax(0, rttiChip->startCol - 4));

        // 30 oscillations from outside-chip to inside-chip and back.
        // Live bug: cursor jitter at the chip boundary triggered a
        // tooltip dismiss on the outside half of each oscillation and
        // a re-show on the inside half — flicker the user can see.
        // Stable behavior: tooltip shows ONCE on first chip-entry and
        // stays put for the rest of the test. The cursor never leaves
        // the editor viewport here, so no Leave-driven dismissal.
        for (int i = 0; i < 30; ++i) {
            sendMove(vp, outChip);
            QApplication::processEvents();
            sendMove(vp, inChip);
            QApplication::processEvents();
        }

        QVERIFY2(counter.shows <= 2,
            qPrintable(QStringLiteral("chip tooltip flickered: %1 shows over "
                "30 chip-boundary oscillations (expected 1–2 — single "
                "initial show, no re-shows from boundary jitter)")
                .arg(counter.shows)));
        QVERIFY2(counter.hides == 0,
            qPrintable(QStringLiteral("chip tooltip dismissed mid-hover: %1 "
                "hides while cursor was still inside the editor viewport "
                "(should only hide on Leave)")
                .arg(counter.hides)));

        tip->removeEventFilter(&counter);
        rcx::dismissRcxTooltip();
    }
};

QTEST_MAIN(TestTooltipFlicker)
#include "test_tooltip_flicker.moc"
