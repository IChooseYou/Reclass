#include "core.h"
#include "rtti.h"
#include "providers/buffer_provider.h"
#include "providers/provider.h"
#include "providers/snapshot_provider.h"
#include <QtTest/QTest>
#include <QByteArray>
#include <QVector>
#include <cstring>
#include <memory>

#ifdef _WIN32
#  include <windows.h>
#  include <psapi.h>
#endif

using namespace rcx;

// TutorialTest mirrors the selfTest "RcxEditor* (live)" demo flow with
// pure synthetic data, in a headless environment. The point is to isolate
// whether the demo's *plumbing* (typed-pointer node + Itanium-shape vtable +
// tree.initialClass tag + provider hooked to a "module") drives compose to
// produce the auto-RTTI hint on the typed-pointer line.
//
// If this test fails, the issue isn't in the Reclass app — it's in the
// pipeline. If this passes, the live demo failure is environmental
// (process module list missing Reclass.exe at compose time, MinGW vtable
// layout mismatch, etc.) and we instrument the live path next.

namespace {

constexpr uint64_t kImageBase = 0x10000;
constexpr uint64_t kInstanceVa = 0x40000;   // where the "RcxEditor instance" lives

// Itanium synthetic — same shape as test_rtti's fixture, refactored inline
// here so the test is self-contained.
QByteArray buildAddressSpace(const char* mangledName) {
    constexpr uint32_t kVt   = 0x1000;
    constexpr uint32_t kTi   = 0x1100;
    constexpr uint32_t kName = 0x1180;   // outside type_info's own header
    constexpr uint32_t kTiVt = 0x1200;

    QByteArray data(0x50000, '\0');
    auto wq = [&](uint64_t off, uint64_t v) {
        std::memcpy(data.data() + off, &v, 8);
    };
    auto wi = [&](uint64_t off, int64_t v) {
        std::memcpy(data.data() + off, &v, 8);
    };

    // Vtable in module
    wi(kImageBase + kVt - 16, 0);                      // offset_to_top
    wq(kImageBase + kVt - 8, kImageBase + kTi);        // type_info VA
    for (int i = 0; i < 5; i++)
        wq(kImageBase + kVt + i * 8,
           kImageBase + 0x100 + (uint64_t)i * 0x10);   // method ptrs

    // type_info: vtable_ptr at +0, name_ptr at +8
    wq(kImageBase + kTi + 0, kImageBase + kTiVt);
    wq(kImageBase + kTi + 8, kImageBase + kName);
    std::memcpy(data.data() + kImageBase + kName, mangledName,
                std::strlen(mangledName) + 1);
    wq(kImageBase + kTiVt, 0xFEEDFACEULL);

    // Plant vtable VA at the "instance" address (offset 0 of the struct)
    wq(kInstanceVa, kImageBase + kVt);
    return data;
}

// Provider that announces a single "Reclass.exe-like" module covering
// the synthetic image. Without this, findOwningModule returns invalid
// and rttiForVtable short-circuits before walkRtti/walkRttiItanium runs.
class FakeProcessProvider : public BufferProvider {
public:
    FakeProcessProvider(QByteArray d, const QString& n)
        : BufferProvider(std::move(d), n) {}
    QVector<ModuleEntry> enumerateModules() const override {
        return { ModuleEntry{ QStringLiteral("Reclass.exe"),
                              QStringLiteral("Reclass.exe"),
                              kImageBase, 0x10000 } };
    }
};

// In-process Provider that uses the SAME Win32 APIs as the live
// ProcessMemoryProvider plugin — OpenProcess + ReadProcessMemory +
// EnumProcessModulesEx. This is critical for reproducing the live
// failure: a memcpy-based provider succeeds where ReadProcessMemory
// might not, so we have to exercise the actual API path.
class SelfProvider : public Provider {
public:
    SelfProvider() {
#ifdef _WIN32
        // Mirror ProcessMemoryProvider: OpenProcess + cacheModules.
        m_handle = OpenProcess(
            PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
            FALSE, GetCurrentProcessId());
        if (!m_handle) {
            // Fall back to read-only
            m_handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                                   FALSE, GetCurrentProcessId());
        }
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
    ~SelfProvider() override {
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

// A trivially virtual class compiled into THIS test executable.
// MinGW gives it Itanium-style RTTI we can walk.
class ProbeClass {
public:
    virtual ~ProbeClass() = default;
    virtual int probe() { return 42; }
};

// Multi-inheritance probe — mirrors Qt's QWidget shape (extends QObject +
// QPaintDevice). Multi-inheritance generates __vmi_class_type_info instead
// of __si_class_type_info; if the walker fails on this but works on
// ProbeClass, that's the live-demo bug since RcxEditor inherits via
// QWidget which is a multi-inheritance Q_OBJECT.
class MIBase1 {
public:
    virtual ~MIBase1() = default;
    virtual void foo() {}
};
class MIBase2 {
public:
    virtual ~MIBase2() = default;
    virtual void bar() {}
};
class MIDerived : public MIBase1, public MIBase2 {
public:
    void foo() override {}
    void bar() override {}
};

} // anon

class TutorialTest : public QObject {
    Q_OBJECT
private slots:

    // ── 1. Verify the editor-demo tree shape composes correctly ──
    // Tree:
    //   class RcxEditor {                ← root, structTypeName="RcxEditor"
    //     QWidgetVTable* __vptr;         ← Pointer64 with refId → vtable struct
    //     ...other fields...
    //   }
    //   struct QWidgetVTable { ... }     ← second root, the refId target
    //
    // tree.initialClass = "RcxEditor" so createTab's auto-focus would
    // land on it. tree.baseAddress = kInstanceVa so the field reads
    // come from the instance's address, not the module base.
    void editorDemoComposesWithRcxEditorAsViewRoot() {
        QByteArray data = buildAddressSpace("9RcxEditor");  // demangles to "RcxEditor"
        FakeProcessProvider prov(std::move(data),
                                 QStringLiteral("Reclass.exe"));

        NodeTree tree;
        tree.baseAddress  = kInstanceVa;
        tree.initialClass = QStringLiteral("RcxEditor");
        tree.pointerSize  = 8;

        // VTable struct (refId target, second root)
        Node vt;
        vt.kind = NodeKind::Struct;
        vt.name = QStringLiteral("VTable");
        vt.structTypeName = QStringLiteral("QWidgetVTable");
        int vti = tree.addNode(vt);
        uint64_t vtId = tree.nodes[vti].id;

        // RcxEditor root (first root)
        Node root;
        root.kind = NodeKind::Struct;
        root.name = QStringLiteral("editor");
        root.structTypeName = QStringLiteral("RcxEditor");
        root.classKeyword = QStringLiteral("class");
        root.collapsed = false;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // __vptr — Pointer64 with refId pointing at the vtable struct.
        // This is the typed-pointer composeNode path that the live demo
        // uses, and the case where my recent compose.cpp edit attaches
        // the RTTI hint.
        Node vptr;
        vptr.kind = NodeKind::Pointer64;
        vptr.name = QStringLiteral("__vptr");
        vptr.parentId = rootId;
        vptr.offset = 0;
        vptr.refId = vtId;
        vptr.collapsed = true;
        tree.addNode(vptr);

        // Compose with viewRootId = RcxEditor's id (mimics what selfTest
        // ends up with after setViewRootId).
        ComposeResult r = compose(tree, prov, rootId);

        // Find the __vptr line and assert the RTTI hint fires.
        bool foundVptr = false;
        for (const auto& lm : r.meta) {
            if (lm.lineKind == LineKind::CommandRow) continue;
            // Pointer header is the first node-bearing line in this view.
            if (lm.nodeKind != NodeKind::Pointer64) continue;
            foundVptr = true;
            QVERIFY2(lm.rttiHintStart >= 0,
                "RTTI hint should fire on typed-pointer header — "
                "compose.cpp's typed-pointer block isn't running, "
                "or rttiForVtable's Itanium fallback didn't catch this candidate.");
            QVERIFY(lm.rttiHint.contains(QStringLiteral("RcxEditor")));
            QCOMPARE(lm.rttiVtableAddr, kImageBase + 0x1000);
            break;
        }
        QVERIFY2(foundVptr, "did not locate the __vptr Pointer64 line");
    }

    // ── 2. Verify the same flow on a Hex64 baseline ──
    // Sanity check: if Pointer64 fails but Hex64 works, the bug is
    // strictly in the typed-pointer block. If both fail, the issue is
    // in rttiForVtable or its callers.
    void hex64BaselineLightsUp() {
        QByteArray data = buildAddressSpace("9RcxEditor");
        FakeProcessProvider prov(std::move(data),
                                 QStringLiteral("Reclass.exe"));

        NodeTree tree;
        tree.baseAddress = kInstanceVa;
        tree.pointerSize = 8;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = QStringLiteral("R");
        root.structTypeName = QStringLiteral("R");
        root.classKeyword = QStringLiteral("class");
        root.collapsed = false;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::Hex64;
        f.parentId = rootId;
        f.offset = 0;
        f.name = QStringLiteral("vptr_as_hex");
        tree.addNode(f);

        ComposeResult r = compose(tree, prov, rootId);
        bool foundField = false;
        for (const auto& lm : r.meta) {
            if (lm.nodeKind != NodeKind::Hex64) continue;
            foundField = true;
            QVERIFY2(lm.rttiHintStart >= 0,
                "Hex64 RTTI hint should fire — composeLeaf path baseline.");
            QVERIFY(lm.rttiHint.contains(QStringLiteral("RcxEditor")));
            break;
        }
        QVERIFY(foundField);
    }

    // ── 3. initialClass tag is serialized + restored ──
    // Verifies the .rcx save-file round-trip preserves the tag.
    void initialClassRoundTripsThroughJson() {
        NodeTree t;
        t.baseAddress = 0x123456;
        t.initialClass = QStringLiteral("RcxEditor");
        Node root;
        root.kind = NodeKind::Struct;
        root.name = QStringLiteral("editor");
        root.structTypeName = QStringLiteral("RcxEditor");
        t.addNode(root);

        QJsonObject obj = t.toJson();
        QVERIFY(obj.contains(QStringLiteral("initialClass")));
        QCOMPARE(obj["initialClass"].toString(), QStringLiteral("RcxEditor"));

        NodeTree t2 = NodeTree::fromJson(obj);
        QCOMPARE(t2.initialClass, QStringLiteral("RcxEditor"));
    }

    // ── 4. Empty initialClass is omitted from JSON (no churn for legacy projects) ──
    void emptyInitialClassNotSerialized() {
        NodeTree t;
        t.baseAddress = 0x400000;
        // initialClass left default-empty
        Node root;
        root.kind = NodeKind::Struct;
        root.name = QStringLiteral("a");
        t.addNode(root);

        QJsonObject obj = t.toJson();
        QVERIFY(!obj.contains(QStringLiteral("initialClass")));
    }

    // ── 5. ABI tagging propagates through to the hint info ──
    // The compose hint stores rttiVtableAddr; the controller can later
    // hand that to walkRtti and inspect info.abi to render "(Itanium)"
    // in the browser dialog. Verify abi is set on a successful walk.
    void abiTagSetOnItaniumWalk() {
        QByteArray data = buildAddressSpace("9RcxEditor");
        FakeProcessProvider prov(std::move(data),
                                 QStringLiteral("Reclass.exe"));
        auto info = walkRttiItanium(prov, kImageBase + 0x1000);
        QVERIFY(info.ok);
        QCOMPARE(info.abi, QStringLiteral("Itanium"));
        QCOMPARE(info.demangledName, QStringLiteral("RcxEditor"));
    }

    // ── 6. Real-process Itanium walk against a non-Qt class ──
    // Plain C++ class with one virtual method. Validates the walker on a
    // simple MinGW Itanium vtable (no Q_OBJECT, no MOC, no MI).
    void walkRealItaniumVtableInOwnProcess() {
#ifndef _WIN32
        QSKIP("self-process probe is Windows-only");
#else
        ProbeClass obj;
        uint64_t vptrSlot = reinterpret_cast<uint64_t>(&obj);
        uint64_t vtable = 0;
        std::memcpy(&vtable, reinterpret_cast<void*>(vptrSlot), 8);
        QVERIFY2(vtable != 0, "vptr was null");

        SelfProvider prov;
        QVERIFY2(!prov.enumerateModules().isEmpty(),
            "self-process modules empty — EnumProcessModulesEx failed");

        bool inAnyModule = false;
        for (const auto& m : prov.enumerateModules()) {
            if (vtable >= m.base && vtable < m.base + m.size) {
                inAnyModule = true; break;
            }
        }
        QVERIFY2(inAnyModule,
            qPrintable(QStringLiteral("vtable 0x%1 not in any enumerated module")
                .arg(vtable, 0, 16)));

        auto info = walkRttiItanium(prov, vtable, /*ptrSize=*/8,
                                    /*maxVtableSlots=*/4);
        if (!info.ok) {
            QFAIL(qPrintable(QStringLiteral("walkRttiItanium failed: %1")
                .arg(info.error)));
        }
        QCOMPARE(info.abi, QStringLiteral("Itanium"));
        QVERIFY2(info.demangledName.contains(QStringLiteral("ProbeClass")),
            qPrintable(QStringLiteral("got demangled name: '%1' (raw '%2')")
                .arg(info.demangledName, info.rawName)));
#endif
    }

    // ── 7. Real Q_OBJECT subclass in the test exe ──
    // Reclass's editor demo points at an RcxEditor* — a real Qt QObject
    // subclass with MOC-generated metadata. If walkRttiItanium handles
    // ProbeClass but FAILS on a Qt QObject, that's the live-demo bug.
    // TutorialTest itself IS a QObject (Q_OBJECT macro), so we walk our
    // own vtable. Failures here pinpoint the discrepancy.
    void walkRealQtObjectVtableInOwnProcess() {
#ifndef _WIN32
        QSKIP("self-process probe is Windows-only");
#else
        // `this` is a TutorialTest* with full Qt vtable.
        uint64_t thisAddr = reinterpret_cast<uint64_t>(this);
        uint64_t vtable = 0;
        std::memcpy(&vtable, reinterpret_cast<void*>(thisAddr), 8);
        QVERIFY2(vtable != 0, "QObject vptr was null");

        SelfProvider prov;
        bool inAnyModule = false;
        for (const auto& m : prov.enumerateModules()) {
            if (vtable >= m.base && vtable < m.base + m.size) {
                inAnyModule = true; break;
            }
        }
        QVERIFY2(inAnyModule,
            qPrintable(QStringLiteral("QObject vtable 0x%1 not in any module")
                .arg(vtable, 0, 16)));

        auto info = walkRttiItanium(prov, vtable, /*ptrSize=*/8,
                                    /*maxVtableSlots=*/8);
        if (!info.ok) {
            QFAIL(qPrintable(QStringLiteral(
                "walkRttiItanium failed on a real Q_OBJECT: %1\n"
                "vtable=0x%2  vtable[-8] (type_info*)=0x%3  "
                "vtable[-16] (offset_to_top)=%4")
                .arg(info.error)
                .arg(vtable, 0, 16)
                .arg(readU64Direct(vtable - 8), 0, 16)
                .arg((qint64)readI64Direct(vtable - 16))));
        }
        QCOMPARE(info.abi, QStringLiteral("Itanium"));
        QVERIFY2(info.demangledName.contains(QStringLiteral("TutorialTest"))
                 || info.demangledName.contains(QStringLiteral("QObject")),
            qPrintable(QStringLiteral("got demangled name: '%1' (raw '%2')")
                .arg(info.demangledName, info.rawName)));
#endif
    }

    // ── 7b. Real-process Itanium walk against a multi-inheritance class ──
    // QWidget (RcxEditor's grandparent) has MI: extends QObject + QPaintDevice.
    // GCC emits __vmi_class_type_info for MI classes, layout differs slightly.
    // If THIS fails but the single-inheritance Qt test passes, that's the
    // RcxEditor-specific bug.
    void walkRealMultiInheritanceVtableInOwnProcess() {
#ifndef _WIN32
        QSKIP("self-process probe is Windows-only");
#else
        MIDerived obj;
        // The primary vtable lives at offset 0 of the object — same as
        // single inheritance for the most-derived subobject.
        uint64_t objAddr = reinterpret_cast<uint64_t>(&obj);
        uint64_t vtable = 0;
        std::memcpy(&vtable, reinterpret_cast<void*>(objAddr), 8);
        QVERIFY(vtable != 0);

        SelfProvider prov;
        bool inAnyModule = false;
        for (const auto& m : prov.enumerateModules()) {
            if (vtable >= m.base && vtable < m.base + m.size) {
                inAnyModule = true; break;
            }
        }
        QVERIFY(inAnyModule);

        auto info = walkRttiItanium(prov, vtable, /*ptrSize=*/8,
                                    /*maxVtableSlots=*/8);
        if (!info.ok) {
            QFAIL(qPrintable(QStringLiteral(
                "MI walkRttiItanium failed: %1\n"
                "vtable=0x%2  vtable[-8]=0x%3  vtable[-16]=%4")
                .arg(info.error)
                .arg(vtable, 0, 16)
                .arg(readU64Direct(vtable - 8), 0, 16)
                .arg((qint64)readI64Direct(vtable - 16))));
        }
        QCOMPARE(info.abi, QStringLiteral("Itanium"));
        QVERIFY2(info.demangledName.contains(QStringLiteral("MIDerived")),
            qPrintable(QStringLiteral("got: '%1' raw '%2'")
                .arg(info.demangledName, info.rawName)));
#endif
    }

    // ── 8. Real-process compose flow with a typed Pointer64 ──
    // This is the closest test to the actual live demo failure: typed
    // Pointer64 with refId, value plants a real Q_OBJECT vtable address,
    // SelfProvider serves the test exe's memory + module list, compose()
    // runs the typed-pointer block we expect to attach the RTTI hint.
    // If THIS fails, the live failure isn't environmental — there's a
    // bug in the typed-pointer compose block we missed.
    void typedPointerComposeOnRealQtObject() {
#ifndef _WIN32
        QSKIP("self-process probe is Windows-only");
#else
        uint64_t thisAddr = reinterpret_cast<uint64_t>(this);
        uint64_t vtable = 0;
        std::memcpy(&vtable, reinterpret_cast<void*>(thisAddr), 8);
        QVERIFY(vtable != 0);

        // Mimic the editor demo tree shape exactly: Pointer64 with refId
        // pointing at a separate "vtable struct" root. baseAddress = the
        // QObject's own address so reading offset 0 gives us the vptr.
        NodeTree tree;
        tree.baseAddress = thisAddr;
        tree.pointerSize = 8;

        Node vtStruct;
        vtStruct.kind = NodeKind::Struct;
        vtStruct.name = QStringLiteral("VTable");
        vtStruct.structTypeName = QStringLiteral("QObjectVTable");
        int vti = tree.addNode(vtStruct);
        uint64_t vtId = tree.nodes[vti].id;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = QStringLiteral("self");
        root.structTypeName = QStringLiteral("TutorialTest");
        root.classKeyword = QStringLiteral("class");
        root.collapsed = false;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node vptr;
        vptr.kind = NodeKind::Pointer64;
        vptr.name = QStringLiteral("__vptr");
        vptr.parentId = rootId;
        vptr.offset = 0;
        vptr.refId = vtId;
        vptr.collapsed = true;
        tree.addNode(vptr);

        SelfProvider prov;
        ComposeResult r = compose(tree, prov, rootId);

        // Locate the typed-pointer line and assert the RTTI hint fired.
        bool foundVptr = false;
        QString gotHint;
        for (const auto& lm : r.meta) {
            if (lm.lineKind == LineKind::CommandRow) continue;
            if (lm.nodeKind != NodeKind::Pointer64) continue;
            foundVptr = true;
            gotHint = lm.rttiHint;
            QVERIFY2(lm.rttiHintStart >= 0,
                qPrintable(QStringLiteral(
                    "typed-pointer composeNode block did NOT attach RTTI hint "
                    "for vtable 0x%1 — this is the live-demo bug")
                    .arg(vtable, 0, 16)));
            QVERIFY2(lm.rttiHint.contains(QStringLiteral("TutorialTest"))
                     || lm.rttiHint.contains(QStringLiteral("QObject"))
                     || !lm.rttiHint.isEmpty(),
                qPrintable(QStringLiteral("hint was: '%1'").arg(lm.rttiHint)));
            break;
        }
        QVERIFY2(foundVptr, "did not locate the __vptr line");
#endif
    }

    // ── 8b. THE LIVE-DEMO BUG: compose against a SnapshotProvider ──
    // The live controller wraps the real provider in a SnapshotProvider
    // for async page-cached refresh. SnapshotProvider must forward
    // enumerateModules() to its m_real — without that, findOwningModule
    // sees an empty module list and rttiForVtable short-circuits before
    // walkRtti even runs. This test wraps SelfProvider in SnapshotProvider
    // exactly like the live demo, plants a real Q_OBJECT vtable, and
    // asserts the RTTI hint still fires.
    void rttiHintFiresWhenComposingThroughSnapshotProvider() {
#ifndef _WIN32
        QSKIP("self-process probe is Windows-only");
#else
        uint64_t thisAddr = reinterpret_cast<uint64_t>(this);
        uint64_t vtable = 0;
        std::memcpy(&vtable, reinterpret_cast<void*>(thisAddr), 8);
        QVERIFY(vtable != 0);

        auto realProv = std::make_shared<SelfProvider>();
        // Reproduces the live scenario EXACTLY: the controller's async
        // refresh only collected pages for the main struct extent (where
        // the QObject instance lives); it did NOT collect pages around
        // the vtable target because the typed pointer is COLLAPSED. So
        // the snapshot has the instance's page but NOT the vtable's page
        // and NOT the type_info / __name pages. Without read-fallthrough
        // in SnapshotProvider, walkRttiItanium reads zeros for vtable[-8]
        // and bails — that's the bug we're regression-testing.
        SnapshotProvider::PageMap pages;
        constexpr uint64_t kPageMask = ~uint64_t(0xFFF);
        constexpr uint64_t kPageSize = 0x1000;
        QByteArray instancePage(kPageSize, '\0');
        realProv->read(thisAddr & kPageMask, instancePage.data(), kPageSize);
        pages[thisAddr & kPageMask] = instancePage;
        // Deliberately omit vtable/type_info/name pages.

        SnapshotProvider snap(realProv, pages, 0x1000);

        // Sanity: SnapshotProvider must forward enumerateModules to real.
        QVERIFY2(!snap.enumerateModules().isEmpty(),
            "SnapshotProvider returned empty module list — RTTI walker "
            "will fail because findOwningModule won't find any module.");

        NodeTree tree;
        tree.baseAddress = thisAddr;
        tree.pointerSize = 8;

        Node vtStruct;
        vtStruct.kind = NodeKind::Struct;
        vtStruct.structTypeName = QStringLiteral("VT");
        int vti = tree.addNode(vtStruct);
        uint64_t vtId = tree.nodes[vti].id;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("R");
        root.classKeyword = QStringLiteral("class");
        root.collapsed = false;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node vptr;
        vptr.kind = NodeKind::Pointer64;
        vptr.name = QStringLiteral("__vptr");
        vptr.parentId = rootId;
        vptr.offset = 0;
        vptr.refId = vtId;
        vptr.collapsed = true;
        tree.addNode(vptr);

        ComposeResult r = compose(tree, snap, rootId);

        bool foundVptr = false;
        for (const auto& lm : r.meta) {
            if (lm.lineKind == LineKind::CommandRow) continue;
            if (lm.nodeKind != NodeKind::Pointer64) continue;
            foundVptr = true;
            QVERIFY2(lm.rttiHintStart >= 0,
                "RTTI hint did NOT fire when composing through "
                "SnapshotProvider — this is the live-demo bug.");
            break;
        }
        QVERIFY(foundVptr);
#endif
    }

    // ── 9. typedPointer compose flow with showComments=true and symbolLookup ──
    // Identical to test 8 but with the live-demo's compose flags:
    // showComments=true (so symbol annotations get baked into ptrText)
    // and a symbolLookup callback that pretends to resolve PDB symbols.
    // If THIS fails but #8 passes, the bug is in how the typed-pointer
    // hint code interacts with already-baked-in symbol comments.
    void typedPointerComposeWithSymbolsEnabled() {
#ifndef _WIN32
        QSKIP("self-process probe is Windows-only");
#else
        uint64_t thisAddr = reinterpret_cast<uint64_t>(this);
        uint64_t vtable = 0;
        std::memcpy(&vtable, reinterpret_cast<void*>(thisAddr), 8);
        QVERIFY(vtable != 0);

        NodeTree tree;
        tree.baseAddress = thisAddr;
        tree.pointerSize = 8;

        Node vtStruct;
        vtStruct.kind = NodeKind::Struct;
        vtStruct.name = QStringLiteral("VTable");
        vtStruct.structTypeName = QStringLiteral("QObjectVTable");
        int vti = tree.addNode(vtStruct);
        uint64_t vtId = tree.nodes[vti].id;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = QStringLiteral("self");
        root.structTypeName = QStringLiteral("TutorialTest");
        root.classKeyword = QStringLiteral("class");
        root.collapsed = false;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node vptr;
        vptr.kind = NodeKind::Pointer64;
        vptr.name = QStringLiteral("__vptr");
        vptr.parentId = rootId;
        vptr.offset = 0;
        vptr.refId = vtId;
        vptr.collapsed = true;
        tree.addNode(vptr);

        SelfProvider prov;
        // showComments=true to bake in symbol annotations like the live demo
        // does, and a symbolLookup that mimics what the controller wires up.
        SymbolLookupFn symLookup = [&prov](uint64_t addr) -> QString {
            return prov.getSymbol(addr);
        };
        ComposeResult r = compose(tree, prov, rootId,
                                  /*compactColumns=*/false,
                                  /*treeLines=*/false,
                                  /*braceWrap=*/false,
                                  /*typeHints=*/false,
                                  /*showComments=*/true,
                                  symLookup);

        bool foundVptr = false;
        for (const auto& lm : r.meta) {
            if (lm.lineKind == LineKind::CommandRow) continue;
            if (lm.nodeKind != NodeKind::Pointer64) continue;
            foundVptr = true;
            QVERIFY2(lm.rttiHintStart >= 0,
                qPrintable(QStringLiteral(
                    "with showComments=true the typed-pointer hint did NOT fire — "
                    "this matches the live-demo behaviour and is the real bug.")));
            break;
        }
        QVERIFY(foundVptr);
#endif
    }

private:
#ifdef _WIN32
    static uint64_t readU64Direct(uint64_t addr) {
        uint64_t v = 0;
        std::memcpy(&v, reinterpret_cast<void*>(addr), 8);
        return v;
    }
    static int64_t readI64Direct(uint64_t addr) {
        int64_t v = 0;
        std::memcpy(&v, reinterpret_cast<void*>(addr), 8);
        return v;
    }
#endif
};

QTEST_MAIN(TutorialTest)
#include "test_tutorial.moc"
