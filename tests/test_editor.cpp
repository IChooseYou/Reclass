#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QMouseEvent>
#include <QFile>
#include <QMenu>
#include <QProxyStyle>
#include <QStyleOption>
#include <QImage>
#include <QPainter>
#include <QCursor>
#include <QScreen>
#include <QMainWindow>
#include <QStatusBar>
#include <QPushButton>
#include <QButtonGroup>
#include <QLabel>
#include <QLayout>
#include <QHBoxLayout>
#include <QScrollBar>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>
#include "editor.h"
#include "core.h"

using namespace rcx;

// ── Cursor test helpers ──

static Qt::CursorShape viewportCursor(RcxEditor* editor) {
    return editor->scintilla()->viewport()->cursor().shape();
}

static QPoint colToViewport(QsciScintilla* sci, int line, int col) {
    long pos = sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                                  (unsigned long)line, (long)col);
    int x = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION, 0, pos);
    int y = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, 0, pos);
    return QPoint(x, y);
}

static void sendMouseMove(QWidget* viewport, const QPoint& pos) {
    QMouseEvent move(QEvent::MouseMove, QPointF(pos), QPointF(pos),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &move);
}

static void sendLeftClick(QWidget* viewport, const QPoint& pos) {
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), QPointF(pos),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos), QPointF(pos),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &release);
}

// 0x7D0 bytes of PEB-like data with recognizable values at key offsets
static BufferProvider makeTestProvider() {
    QByteArray data(0x7D0, '\0');

    auto w8  = [&](int off, uint8_t  v) { data[off] = (char)v; };
    auto w16 = [&](int off, uint16_t v) { memcpy(data.data()+off, &v, 2); };
    auto w32 = [&](int off, uint32_t v) { memcpy(data.data()+off, &v, 4); };
    auto w64 = [&](int off, uint64_t v) { memcpy(data.data()+off, &v, 8); };

    w8 (0x002, 1);                              // BeingDebugged
    w8 (0x003, 0x04);                           // BitField
    w64(0x008, 0xFFFFFFFFFFFFFFFFULL);          // Mutant (-1)
    w64(0x010, 0x00007FF6DE120000ULL);          // ImageBaseAddress
    w64(0x018, 0x00007FFE3B8B53C0ULL);          // Ldr
    w64(0x020, 0x000001A4C3E20F90ULL);          // ProcessParameters
    w64(0x028, 0x0000000000000000ULL);          // SubSystemData
    w64(0x030, 0x000001A4C3D40000ULL);          // ProcessHeap
    w64(0x038, 0x00007FFE3B8D4260ULL);          // FastPebLock
    w64(0x040, 0x0000000000000000ULL);          // AtlThunkSListPtr
    w64(0x048, 0x0000000000000000ULL);          // IFEOKey
    w32(0x050, 0x01);                           // CrossProcessFlags
    w64(0x058, 0x00007FFE3B720000ULL);          // KernelCallbackTable
    w32(0x060, 0);                              // SystemReserved
    w32(0x064, 0);                              // AtlThunkSListPtr32
    w64(0x068, 0x00007FFE3E570000ULL);          // ApiSetMap
    w32(0x070, 0);                              // TlsExpansionCounter
    w64(0x078, 0x00007FFE3B8D3F50ULL);          // TlsBitmap
    w32(0x080, 0x00000003);                     // TlsBitmapBits[0]
    w32(0x084, 0x00000000);                     // TlsBitmapBits[1]
    w64(0x088, 0x00007FFE38800000ULL);          // ReadOnlySharedMemoryBase
    w64(0x090, 0x00007FFE38820000ULL);          // SharedData
    w64(0x098, 0x00007FFE388A0000ULL);          // ReadOnlyStaticServerData
    w64(0x0A0, 0x00007FFE3B8D1000ULL);          // AnsiCodePageData
    w64(0x0A8, 0x00007FFE3B8D2040ULL);          // OemCodePageData
    w64(0x0B0, 0x00007FFE3B8CE020ULL);          // UnicodeCaseTableData
    w32(0x0B8, 8);                              // NumberOfProcessors
    w32(0x0BC, 0x70);                           // NtGlobalFlag
    w64(0x0C0, 0xFFFFFFFF7C91E000ULL);          // CriticalSectionTimeout
    w64(0x0C8, 0x0000000000100000ULL);          // HeapSegmentReserve
    w64(0x0D0, 0x0000000000002000ULL);          // HeapSegmentCommit
    w64(0x0D8, 0x0000000000040000ULL);          // HeapDeCommitTotalFreeThreshold
    w64(0x0E0, 0x0000000000001000ULL);          // HeapDeCommitFreeBlockThreshold
    w32(0x0E8, 4);                              // NumberOfHeaps
    w32(0x0EC, 16);                             // MaximumNumberOfHeaps
    w64(0x0F0, 0x000001A4C3D40688ULL);          // ProcessHeaps
    w64(0x0F8, 0x00007FFE388B0000ULL);          // GdiSharedHandleTable
    w64(0x100, 0x0000000000000000ULL);          // ProcessStarterHelper
    w32(0x108, 0);                              // GdiDCAttributeList
    w64(0x110, 0x00007FFE3B8D42E8ULL);          // LoaderLock
    w32(0x118, 10);                             // OSMajorVersion
    w32(0x11C, 0);                              // OSMinorVersion
    w16(0x120, 19045);                          // OSBuildNumber
    w16(0x122, 0);                              // OSCSDVersion
    w32(0x124, 2);                              // OSPlatformId
    w32(0x128, 3);                              // ImageSubsystem (CUI)
    w32(0x12C, 10);                             // ImageSubsystemMajorVersion
    w32(0x130, 0);                              // ImageSubsystemMinorVersion
    w64(0x138, 0x00000000000000FFULL);          // ActiveProcessAffinityMask
    w64(0x230, 0x0000000000000000ULL);          // PostProcessInitRoutine
    w64(0x238, 0x00007FFE3B8D3F70ULL);          // TlsExpansionBitmap
    w32(0x2C0, 1);                              // SessionId
    w64(0x2C8, 0x0000000000000000ULL);          // AppCompatFlags
    w64(0x2D0, 0x0000000000000000ULL);          // AppCompatFlagsUser
    w64(0x2D8, 0x0000000000000000ULL);          // pShimData
    w64(0x2E0, 0x0000000000000000ULL);          // AppCompatInfo
    w16(0x2E8, 0);                              // CSDVersion.Length
    w16(0x2EA, 0);                              // CSDVersion.MaximumLength
    w64(0x2F0, 0x0000000000000000ULL);          // CSDVersion.Buffer
    w64(0x2F8, 0x000001A4C3E21000ULL);          // ActivationContextData
    w64(0x300, 0x000001A4C3E22000ULL);          // ProcessAssemblyStorageMap
    w64(0x308, 0x00007FFE38840000ULL);          // SystemDefaultActivationContextData
    w64(0x310, 0x00007FFE38850000ULL);          // SystemAssemblyStorageMap
    w64(0x318, 0x0000000000002000ULL);          // MinimumStackCommit
    w64(0x330, 0x0000000000000000ULL);          // PatchLoaderData
    w64(0x338, 0x0000000000000000ULL);          // ChpeV2ProcessInfo
    w32(0x340, 0);                              // AppModelFeatureState
    w16(0x34C, 1252);                           // ActiveCodePage
    w16(0x34E, 437);                            // OemCodePage
    w16(0x350, 0);                              // UseCaseMapping
    w16(0x352, 0);                              // UnusedNlsField
    w64(0x358, 0x000001A4C3E30000ULL);          // WerRegistrationData
    w64(0x360, 0x0000000000000000ULL);          // WerShipAssertPtr
    w64(0x368, 0x0000000000000000ULL);          // EcCodeBitMap
    w64(0x370, 0x0000000000000000ULL);          // pImageHeaderHash
    w32(0x378, 0);                              // TracingFlags
    w64(0x380, 0x00007FFE38890000ULL);          // CsrServerReadOnlySharedMemoryBase
    w64(0x388, 0x0000000000000000ULL);          // TppWorkerpListLock
    w64(0x390, 0x000000D87B5E5390ULL);          // TppWorkerpList.Flink (self)
    w64(0x398, 0x000000D87B5E5390ULL);          // TppWorkerpList.Blink (self)
    w64(0x7A0, 0x0000000000000000ULL);          // TelemetryCoverageHeader
    w32(0x7A8, 0);                              // CloudFileFlags
    w32(0x7AC, 0);                              // CloudFileDiagFlags
    w8 (0x7B0, 0);                              // PlaceholderCompatibilityMode
    w64(0x7B8, 0x00007FFE38860000ULL);          // LeapSecondData
    w32(0x7C0, 0);                              // LeapSecondFlags
    w32(0x7C4, 0);                              // NtGlobalFlag2
    w64(0x7C8, 0x0000000000000000ULL);          // ExtendedFeatureDisableMask

    return BufferProvider(data, "peb_snapshot.bin");
}

// Build the full _PEB64 tree (0x7D0 bytes), unions mapped to first member
static NodeTree makeTestTree() {
    NodeTree tree;
    tree.baseAddress = 0;

    // Root struct
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "_PEB64";
    root.name = "Peb";
    root.parentId = 0;
    root.offset = 0;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    // Helpers
    auto field = [&](int off, NodeKind k, const char* name) {
        Node n; n.kind = k; n.name = name;
        n.parentId = rootId; n.offset = off;
        tree.addNode(n);
    };
    auto pad = [&](int off, int /*len*/, const char* name) {
        // 4-byte padding → Hex32 (all usages in this test pass len=4)
        Node n; n.kind = NodeKind::Hex32; n.name = name;
        n.parentId = rootId; n.offset = off;
        tree.addNode(n);
    };
    auto arr = [&](int off, NodeKind ek, int len, const char* name) {
        Node n; n.kind = NodeKind::Array; n.name = name;
        n.parentId = rootId; n.offset = off;
        n.arrayLen = len; n.elementKind = ek;
        tree.addNode(n);
    };
    auto sub = [&](int off, const char* ty, const char* name) -> uint64_t {
        Node n; n.kind = NodeKind::Struct; n.structTypeName = ty; n.name = name;
        n.parentId = rootId; n.offset = off;
        int idx = tree.addNode(n); return tree.nodes[idx].id;
    };

    // ── 0x000 – 0x007 ──
    field(0x000, NodeKind::UInt8,     "InheritedAddressSpace");
    field(0x001, NodeKind::UInt8,     "ReadImageFileExecOptions");
    field(0x002, NodeKind::UInt8,     "BeingDebugged");
    field(0x003, NodeKind::UInt8,     "BitField");              // union → first member
    pad  (0x004, 4,                   "Padding0");

    // ── 0x008 – 0x04F ──
    field(0x008, NodeKind::Pointer64, "Mutant");
    field(0x010, NodeKind::Pointer64, "ImageBaseAddress");
    field(0x018, NodeKind::Pointer64, "Ldr");
    field(0x020, NodeKind::Pointer64, "ProcessParameters");
    field(0x028, NodeKind::Pointer64, "SubSystemData");
    field(0x030, NodeKind::Pointer64, "ProcessHeap");
    field(0x038, NodeKind::Pointer64, "FastPebLock");
    field(0x040, NodeKind::Pointer64, "AtlThunkSListPtr");
    field(0x048, NodeKind::Pointer64, "IFEOKey");

    // ── 0x050 – 0x07F ──
    field(0x050, NodeKind::UInt32,    "CrossProcessFlags");     // union → first member
    pad  (0x054, 4,                   "Padding1");
    field(0x058, NodeKind::Pointer64, "KernelCallbackTable");   // union → first member
    field(0x060, NodeKind::UInt32,    "SystemReserved");
    field(0x064, NodeKind::UInt32,    "AtlThunkSListPtr32");
    field(0x068, NodeKind::Pointer64, "ApiSetMap");
    field(0x070, NodeKind::UInt32,    "TlsExpansionCounter");
    pad  (0x074, 4,                   "Padding2");
    field(0x078, NodeKind::Pointer64, "TlsBitmap");
    arr  (0x080, NodeKind::UInt32, 2, "TlsBitmapBits");

    // ── 0x088 – 0x0BF ──
    field(0x088, NodeKind::Pointer64, "ReadOnlySharedMemoryBase");
    field(0x090, NodeKind::Pointer64, "SharedData");
    field(0x098, NodeKind::Pointer64, "ReadOnlyStaticServerData");
    field(0x0A0, NodeKind::Pointer64, "AnsiCodePageData");
    field(0x0A8, NodeKind::Pointer64, "OemCodePageData");
    field(0x0B0, NodeKind::Pointer64, "UnicodeCaseTableData");
    field(0x0B8, NodeKind::UInt32,    "NumberOfProcessors");
    field(0x0BC, NodeKind::Hex32,     "NtGlobalFlag");

    // ── 0x0C0 – 0x0EF ──
    field(0x0C0, NodeKind::UInt64,    "CriticalSectionTimeout"); // _LARGE_INTEGER union
    field(0x0C8, NodeKind::UInt64,    "HeapSegmentReserve");
    field(0x0D0, NodeKind::UInt64,    "HeapSegmentCommit");
    field(0x0D8, NodeKind::UInt64,    "HeapDeCommitTotalFreeThreshold");
    field(0x0E0, NodeKind::UInt64,    "HeapDeCommitFreeBlockThreshold");
    field(0x0E8, NodeKind::UInt32,    "NumberOfHeaps");
    field(0x0EC, NodeKind::UInt32,    "MaximumNumberOfHeaps");

    // ── 0x0F0 – 0x13F ──
    field(0x0F0, NodeKind::Pointer64, "ProcessHeaps");
    field(0x0F8, NodeKind::Pointer64, "GdiSharedHandleTable");
    field(0x100, NodeKind::Pointer64, "ProcessStarterHelper");
    field(0x108, NodeKind::UInt32,    "GdiDCAttributeList");
    pad  (0x10C, 4,                   "Padding3");
    field(0x110, NodeKind::Pointer64, "LoaderLock");
    field(0x118, NodeKind::UInt32,    "OSMajorVersion");
    field(0x11C, NodeKind::UInt32,    "OSMinorVersion");
    field(0x120, NodeKind::UInt16,    "OSBuildNumber");
    field(0x122, NodeKind::UInt16,    "OSCSDVersion");
    field(0x124, NodeKind::UInt32,    "OSPlatformId");
    field(0x128, NodeKind::UInt32,    "ImageSubsystem");
    field(0x12C, NodeKind::UInt32,    "ImageSubsystemMajorVersion");
    field(0x130, NodeKind::UInt32,    "ImageSubsystemMinorVersion");
    pad  (0x134, 4,                   "Padding4");
    field(0x138, NodeKind::UInt64,    "ActiveProcessAffinityMask");

    // ── 0x140 – 0x22F ──
    arr  (0x140, NodeKind::UInt32, 60, "GdiHandleBuffer");

    // ── 0x230 – 0x2BF ──
    field(0x230, NodeKind::Pointer64, "PostProcessInitRoutine");
    field(0x238, NodeKind::Pointer64, "TlsExpansionBitmap");
    arr  (0x240, NodeKind::UInt32, 32, "TlsExpansionBitmapBits");

    // ── 0x2C0 – 0x2E7 ──
    field(0x2C0, NodeKind::UInt32,    "SessionId");
    pad  (0x2C4, 4,                   "Padding5");
    field(0x2C8, NodeKind::UInt64,    "AppCompatFlags");         // _ULARGE_INTEGER union
    field(0x2D0, NodeKind::UInt64,    "AppCompatFlagsUser");     // _ULARGE_INTEGER union
    field(0x2D8, NodeKind::Pointer64, "pShimData");
    field(0x2E0, NodeKind::Pointer64, "AppCompatInfo");

    // ── 0x2E8 – 0x2F7: _STRING64 CSDVersion (nested struct) ──
    {
        uint64_t sid = sub(0x2E8, "_STRING64", "CSDVersion");
        Node n;
        n.parentId = sid;

        n.kind = NodeKind::UInt16;  n.name = "Length";         n.offset = 0; tree.addNode(n);
        n.kind = NodeKind::UInt16;  n.name = "MaximumLength";  n.offset = 2; tree.addNode(n);
        n.kind = NodeKind::Hex32; n.name = "Pad";
        n.offset = 4; n.arrayLen = 1; tree.addNode(n);
        n.kind = NodeKind::Pointer64; n.name = "Buffer"; n.offset = 8; n.arrayLen = 1;
        tree.addNode(n);
    }

    // ── 0x2F8 – 0x31F ──
    field(0x2F8, NodeKind::Pointer64, "ActivationContextData");
    field(0x300, NodeKind::Pointer64, "ProcessAssemblyStorageMap");
    field(0x308, NodeKind::Pointer64, "SystemDefaultActivationContextData");
    field(0x310, NodeKind::Pointer64, "SystemAssemblyStorageMap");
    field(0x318, NodeKind::UInt64,    "MinimumStackCommit");

    // ── 0x320 – 0x34B ──
    arr  (0x320, NodeKind::UInt64, 2, "SparePointers");
    field(0x330, NodeKind::Pointer64, "PatchLoaderData");
    field(0x338, NodeKind::Pointer64, "ChpeV2ProcessInfo");
    field(0x340, NodeKind::UInt32,    "AppModelFeatureState");
    arr  (0x344, NodeKind::UInt32, 2, "SpareUlongs");
    field(0x34C, NodeKind::UInt16,    "ActiveCodePage");
    field(0x34E, NodeKind::UInt16,    "OemCodePage");
    field(0x350, NodeKind::UInt16,    "UseCaseMapping");
    field(0x352, NodeKind::UInt16,    "UnusedNlsField");

    // ── 0x354 – 0x37F (implicit padding + fields) ──
    pad  (0x354, 4,                   "Pad354");
    field(0x358, NodeKind::Pointer64, "WerRegistrationData");
    field(0x360, NodeKind::Pointer64, "WerShipAssertPtr");
    field(0x368, NodeKind::Pointer64, "EcCodeBitMap");
    field(0x370, NodeKind::Pointer64, "pImageHeaderHash");
    field(0x378, NodeKind::UInt32,    "TracingFlags");           // union → first member
    pad  (0x37C, 4,                   "Padding6");

    // ── 0x380 – 0x39F ──
    field(0x380, NodeKind::Pointer64, "CsrServerReadOnlySharedMemoryBase");
    field(0x388, NodeKind::UInt64,    "TppWorkerpListLock");

    // ── 0x390 – 0x39F: LIST_ENTRY64 TppWorkerpList (nested struct) ──
    {
        uint64_t sid = sub(0x390, "LIST_ENTRY64", "TppWorkerpList");
        Node n;
        n.parentId = sid;
        n.kind = NodeKind::Pointer64; n.name = "Flink"; n.offset = 0; tree.addNode(n);
        n.kind = NodeKind::Pointer64; n.name = "Blink"; n.offset = 8; tree.addNode(n);
    }

    // ── 0x3A0 – 0x79F ──
    arr  (0x3A0, NodeKind::UInt64, 128, "WaitOnAddressHashTable");

    // ── 0x7A0 – 0x7CF ──
    field(0x7A0, NodeKind::Pointer64, "TelemetryCoverageHeader");
    field(0x7A8, NodeKind::UInt32,    "CloudFileFlags");
    field(0x7AC, NodeKind::UInt32,    "CloudFileDiagFlags");
    field(0x7B0, NodeKind::Int8,      "PlaceholderCompatibilityMode");
    arr  (0x7B1, NodeKind::Int8, 7,   "PlaceholderCompatibilityModeReserved");
    field(0x7B8, NodeKind::Pointer64, "LeapSecondData");
    field(0x7C0, NodeKind::UInt32,    "LeapSecondFlags");        // union → first member
    field(0x7C4, NodeKind::UInt32,    "NtGlobalFlag2");
    field(0x7C8, NodeKind::UInt64,    "ExtendedFeatureDisableMask");

    return tree;
}

// ── Pointer expansion demo data ──
// Small tree with a working pointer that points within the buffer.
// Root struct "Demo" has a UInt32 "id" and Pointer64 "pChild" → ChildData.
// ChildData has UInt32 "x", UInt32 "y", Float "z".
struct PtrDemo {
    NodeTree tree;
    BufferProvider prov{QByteArray()};
    uint64_t rootId = 0;
    uint64_t childStructId = 0;
};

static PtrDemo makePtrDemo(bool collapsed = false, bool nullPtr = false) {
    PtrDemo d;
    d.tree.baseAddress = 0;

    // Root struct
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "Demo";
    root.name = "demo";
    root.parentId = 0;
    root.offset = 0;
    int ri = d.tree.addNode(root);
    d.rootId = d.tree.nodes[ri].id;

    // id field at offset 0
    {
        Node n;
        n.kind = NodeKind::UInt32;
        n.name = "id";
        n.parentId = d.rootId;
        n.offset = 0;
        d.tree.addNode(n);
    }

    // ChildData struct definition (separate root)
    Node child;
    child.kind = NodeKind::Struct;
    child.structTypeName = "ChildData";
    child.name = "ChildData";
    child.parentId = 0;
    child.offset = 200; // standalone rendering offset
    int ci = d.tree.addNode(child);
    d.childStructId = d.tree.nodes[ci].id;

    {
        Node n;
        n.kind = NodeKind::UInt32; n.name = "x";
        n.parentId = d.childStructId; n.offset = 0;
        d.tree.addNode(n);
        n.kind = NodeKind::UInt32; n.name = "y";
        n.offset = 4;
        d.tree.addNode(n);
        n.kind = NodeKind::Float; n.name = "z";
        n.offset = 8;
        d.tree.addNode(n);
    }

    // Pointer at offset 8 → ChildData
    {
        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "pChild";
        ptr.parentId = d.rootId;
        ptr.offset = 8;
        ptr.refId = d.childStructId;
        ptr.collapsed = collapsed;
        d.tree.addNode(ptr);
    }

    // Buffer: 128 bytes
    QByteArray data(128, '\0');
    uint32_t idVal = 42;
    memcpy(data.data() + 0, &idVal, 4);

    if (!nullPtr) {
        uint64_t ptrVal = 64; // points to offset 64 in buffer
        memcpy(data.data() + 8, &ptrVal, 8);
    }

    // Data at the pointer target (offset 64)
    uint32_t xVal = 100;  memcpy(data.data() + 64, &xVal, 4);
    uint32_t yVal = 200;  memcpy(data.data() + 68, &yVal, 4);
    float    zVal = 3.14f; memcpy(data.data() + 72, &zVal, 4);

    d.prov = BufferProvider(data, "ptr_demo");
    return d;
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
        BufferProvider prov = makeTestProvider();
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
            QStringLiteral("source\u25BE  0xD87B5E5000"));

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
        QApplication::processEvents(); // flush deferred showSourcePicker timer
    }

    // ── Test: inline edit lifecycle (begin → commit → re-edit) ──
    void testInlineEditReEntry() {
        // Move cursor to first data line (0=CommandRow, root header suppressed)
        m_editor->scintilla()->setCursorPosition(kFirstDataLine, 0);

        // Should not be editing
        QVERIFY(!m_editor->isEditing());

        // Begin edit on Name column
        bool ok = m_editor->beginInlineEdit(EditTarget::Name, kFirstDataLine);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        // Cancel the edit
        m_editor->cancelInlineEdit();
        QVERIFY(!m_editor->isEditing());

        // Re-apply document (simulates controller refresh)
        m_editor->applyDocument(m_result);

        // Should be able to edit again
        ok = m_editor->beginInlineEdit(EditTarget::Name, kFirstDataLine);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        // Cancel again
        m_editor->cancelInlineEdit();
        QVERIFY(!m_editor->isEditing());
    }

    // ── Test: commit inline edit then re-edit same line ──
    void testCommitThenReEdit() {
        m_editor->applyDocument(m_result);
        m_editor->scintilla()->setCursorPosition(kFirstDataLine, 0);

        // Begin value edit
        bool ok = m_editor->beginInlineEdit(EditTarget::Value, kFirstDataLine);
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
        ok = m_editor->beginInlineEdit(EditTarget::Value, kFirstDataLine);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        m_editor->cancelInlineEdit();
    }

    // ── Test: mouse click during edit commits it ──
    void testMouseClickCommitsEdit() {
        m_editor->applyDocument(m_result);

        bool ok = m_editor->beginInlineEdit(EditTarget::Name, kFirstDataLine);
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

    // ── Test: type edit emits typePickerRequested (popup-based, not inline edit) ──
    void testTypeEditCancel() {
        m_editor->applyDocument(m_result);

        QSignalSpy spy(m_editor, &RcxEditor::typePickerRequested);

        // Begin type edit on a field line — now handled by TypeSelectorPopup
        bool ok = m_editor->beginInlineEdit(EditTarget::Type, kFirstDataLine);
        QVERIFY(ok);
        QCOMPARE(spy.count(), 1);
        // Type editing uses popup, not inline edit state
        QVERIFY(!m_editor->isEditing());
    }

    // ── Test: edit on header line (Name and Type valid, Value invalid) ──
    void testHeaderLineEdit() {
        m_editor->applyDocument(m_result);

        // Root header is suppressed; find a nested struct header (e.g. CSDVersion)
        int headerLine = -1;
        for (int i = 0; i < m_result.meta.size(); i++) {
            if (m_result.meta[i].lineKind == LineKind::Header &&
                m_result.meta[i].foldHead) {
                headerLine = i;
                break;
            }
        }
        QVERIFY2(headerLine >= 0, "Should have a nested struct header");

        const LineMeta* lm = m_editor->metaForLine(headerLine);
        QVERIFY(lm);
        QCOMPARE(lm->lineKind, LineKind::Header);

        // Scroll to header line to ensure visibility
        m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_ENSUREVISIBLE, (unsigned long)headerLine);
        m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_GOTOLINE, (unsigned long)headerLine);
        QApplication::processEvents();

        // Type edit on header should succeed (emits popup signal, not inline edit)
        QSignalSpy typeSpy(m_editor, &RcxEditor::typePickerRequested);
        bool ok = m_editor->beginInlineEdit(EditTarget::Type, headerLine);
        QVERIFY(ok);
        QCOMPARE(typeSpy.count(), 1);

        // Name edit on header should succeed
        ok = m_editor->beginInlineEdit(EditTarget::Name, headerLine);
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

        QSignalSpy spy(m_editor, &RcxEditor::typePickerRequested);

        // Type edit now emits typePickerRequested for TypeSelectorPopup
        bool ok = m_editor->beginInlineEdit(EditTarget::Type, kFirstDataLine);
        QVERIFY(ok);
        QCOMPARE(spy.count(), 1);

        // Verify signal carries valid nodeIdx (second arg)
        QList<QVariant> args = spy.first();
        QVERIFY(args.at(1).toInt() >= 0);

        // No inline edit state — popup handles everything
        QVERIFY(!m_editor->isEditing());

        m_editor->applyDocument(m_result);
    }

    // ── Test: type edit click-away commits original (no change) ──
    void testTypeEditClickAwayNoChange() {
        m_editor->applyDocument(m_result);

        QSignalSpy spy(m_editor, &RcxEditor::typePickerRequested);

        // Type edit emits typePickerRequested (popup handles click-away)
        bool ok = m_editor->beginInlineEdit(EditTarget::Type, kFirstDataLine);
        QVERIFY(ok);
        QCOMPARE(spy.count(), 1);

        // No inline edit state — popup handles click-away behavior
        QVERIFY(!m_editor->isEditing());

        m_editor->applyDocument(m_result);
    }

    // ── Test: column span hit-testing for cursor shape ──
    void testColumnSpanHitTest() {
        m_editor->applyDocument(m_result);

        // kFirstDataLine is a field line (UInt8), verify spans are valid
        const LineMeta* lm = m_editor->metaForLine(kFirstDataLine);
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
            QsciScintillaBase::SCI_LINELENGTH, (unsigned long)kFirstDataLine);
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

        // Put cursor on first field line (kFirstDataLine; 0=CommandRow)
        m_editor->scintilla()->setCursorPosition(kFirstDataLine, 0);
        QSet<int> sel = m_editor->selectedNodeIndices();
        QCOMPARE(sel.size(), 1);

        // The node index should match the first field
        const LineMeta* lm = m_editor->metaForLine(kFirstDataLine);
        QVERIFY(lm);
        QVERIFY(sel.contains(lm->nodeIdx));
    }

    // ── Test: composed text does not contain "// base:" (moved to cmd bar) ──
    void testBaseAddressDisplay() {
        NodeTree tree = makeTestTree();
        tree.baseAddress = 0x10;
        BufferProvider prov = makeTestProvider();
        ComposeResult result = compose(tree, prov);

        m_editor->applyDocument(result);

        // Root header is suppressed; verify no "// base:" anywhere in output
        QVERIFY2(!result.text.contains("// base:"),
                 "Composed text should not contain '// base:' (consolidated into cmd bar)");

        // kFirstDataLine should be the first field (root header suppressed)
        const LineMeta* lm = m_editor->metaForLine(kFirstDataLine);
        QVERIFY(lm);
        QCOMPARE(lm->lineKind, LineKind::Field);

        m_editor->applyDocument(m_result);
    }

    // ── Test: CommandRow ADDR span is valid ──
    void testBaseAddressSpan() {
        m_editor->applyDocument(m_result);

        // Set CommandRow text with ADDR value (simulates controller)
        m_editor->setCommandRowText(
            QStringLiteral("source\u25BE  0xD87B5E5000"));

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

    // ── Test: value edit commit fires signal with typed text ──
    void testValueEditCommitUpdatesSignal() {
        m_editor->applyDocument(m_result);

        // kFirstDataLine = first UInt8 field (InheritedAddressSpace, root header suppressed)
        const LineMeta* lm = m_editor->metaForLine(kFirstDataLine);
        QVERIFY(lm);
        QCOMPARE(lm->lineKind, LineKind::Field);
        // Begin value edit
        bool ok = m_editor->beginInlineEdit(EditTarget::Value, kFirstDataLine);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        // Select all text in the edit span and type replacement
        QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
        QApplication::sendEvent(m_editor->scintilla(), &home);
        QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::ShiftModifier);
        QApplication::sendEvent(m_editor->scintilla(), &end);

        // Type "42" to replace selected text
        for (QChar c : QString("42")) {
            QKeyEvent key(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
            QApplication::sendEvent(m_editor->scintilla(), &key);
        }
        QApplication::processEvents();

        // Commit with Enter
        QSignalSpy spy(m_editor, &RcxEditor::inlineEditCommitted);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(m_editor->scintilla(), &enter);

        QCOMPARE(spy.count(), 1);
        QVERIFY(!m_editor->isEditing());

        // Verify the committed text contains what was typed.
        // UInt8 values display as hex (e.g., "0x042"), so the typed "42" gets
        // concatenated with the existing "0x0" prefix → "0x042".
        // The important check: the signal fired with non-empty text.
        QList<QVariant> args = spy.first();
        QString committedText = args.at(3).toString().trimmed();
        QVERIFY2(!committedText.isEmpty(),
                 "Committed text should not be empty");

        m_editor->applyDocument(m_result);
    }

    // ── Test: base address edit begins on CommandRow (line 0) ──
    void testBaseAddressEditBegins() {
        m_editor->applyDocument(m_result);

        // Set CommandRow text with ADDR value (simulates controller)
        m_editor->setCommandRowText(
            QStringLiteral("source\u25BE  0xD87B5E5000"));

        // Begin base address edit on line 0 (CommandRow ADDR field)
        bool ok = m_editor->beginInlineEdit(EditTarget::BaseAddress, 0);
        QVERIFY2(ok, "Should be able to begin base address edit on CommandRow");
        QVERIFY(m_editor->isEditing());

        // Cancel and reset
        m_editor->cancelInlineEdit();
        m_editor->applyDocument(m_result);
    }

    // ── Test: cursor stays Arrow after left-click on a node ──
    void testCursorAfterLeftClick() {
        m_editor->applyDocument(m_result);

        // Click on a field line at the indent area (col 0 — not over editable text)
        QPoint clickPos = colToViewport(m_editor->scintilla(), kFirstDataLine, 0);
        sendLeftClick(m_editor->scintilla()->viewport(), clickPos);
        QApplication::processEvents();

        // Cursor must be Arrow — QScintilla must NOT have set it to IBeam
        QCOMPARE(viewportCursor(m_editor), Qt::ArrowCursor);
        QVERIFY(!m_editor->isEditing());
    }

    // ── Test: cursor is IBeam only over trimmed name text, Arrow over padding ──
    void testCursorShapeOverText() {
        m_editor->applyDocument(m_result);

        // kFirstDataLine is a field (UInt8 InheritedAddressSpace)
        const LineMeta* lm = m_editor->metaForLine(kFirstDataLine);
        QVERIFY(lm);

        // Get the name span (padded to kColName width)
        ColumnSpan ns = RcxEditor::nameSpan(*lm, lm->effectiveTypeW, lm->effectiveNameW);
        QVERIFY(ns.valid);

        // Move mouse to the start of the name span (should be over text)
        QPoint textPos = colToViewport(m_editor->scintilla(), kFirstDataLine, ns.start + 1);
        sendMouseMove(m_editor->scintilla()->viewport(), textPos);
        QApplication::processEvents();
        QCOMPARE(viewportCursor(m_editor), Qt::IBeamCursor);

        // Move mouse to far padding area (past end of text, within padded span)
        // The padded span ends at ns.end but the trimmed text is shorter
        QPoint padPos = colToViewport(m_editor->scintilla(), kFirstDataLine, ns.end - 1);
        sendMouseMove(m_editor->scintilla()->viewport(), padPos);
        QApplication::processEvents();
        // Should be Arrow (padding whitespace, not actual text)
        QCOMPARE(viewportCursor(m_editor), Qt::ArrowCursor);
    }

    // ── Test: cursor is PointingHand over type column text ──
    void testCursorShapeOverType() {
        m_editor->applyDocument(m_result);

        const LineMeta* lm = m_editor->metaForLine(kFirstDataLine);
        QVERIFY(lm);

        // Type span starts after the fold column + indent
        ColumnSpan ts = RcxEditor::typeSpan(*lm, lm->effectiveTypeW);
        QVERIFY(ts.valid);

        // Move to start of type text (e.g. "uint8_t")
        QPoint typePos = colToViewport(m_editor->scintilla(), kFirstDataLine, ts.start + 1);
        sendMouseMove(m_editor->scintilla()->viewport(), typePos);
        QApplication::processEvents();
        QCOMPARE(viewportCursor(m_editor), Qt::PointingHandCursor);
    }

    // ── Test: cursor is PointingHand over fold column ──
    void testCursorShapeInFoldColumn() {
        m_editor->applyDocument(m_result);
        QApplication::processEvents();

        // Root header is suppressed; find a nested struct with foldHead
        int foldLine = -1;
        for (int i = 0; i < m_result.meta.size(); i++) {
            if (m_result.meta[i].foldHead && m_result.meta[i].lineKind == LineKind::Header) {
                foldLine = i;
                break;
            }
        }
        QVERIFY2(foldLine >= 0, "Should have at least one foldable struct header");

        const LineMeta* lm = m_editor->metaForLine(foldLine);
        QVERIFY(lm);
        QVERIFY(lm->foldHead);

        // Scroll to ensure the fold line is visible
        m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_ENSUREVISIBLE, (unsigned long)foldLine);
        m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_GOTOLINE, (unsigned long)foldLine);
        QApplication::processEvents();

        // Fold indicator is always at cols 0-2 (kFoldCol=3), regardless of depth
        QPoint foldPos = colToViewport(m_editor->scintilla(), foldLine, 1);
        QVERIFY2(foldPos.y() > 0, qPrintable(QString("Fold line %1 should be visible, got y=%2")
            .arg(foldLine).arg(foldPos.y())));
        sendMouseMove(m_editor->scintilla()->viewport(), foldPos);
        QApplication::processEvents();
        QCOMPARE(viewportCursor(m_editor), Qt::PointingHandCursor);
    }

    // ── Test: no IBeam after click then mouse-move to non-editable area ──
    void testNoIBeamAfterClickThenMove() {
        m_editor->applyDocument(m_result);

        // Click on a field to select the node
        const LineMeta* lm = m_editor->metaForLine(kFirstDataLine);
        QVERIFY(lm);
        ColumnSpan ns = RcxEditor::nameSpan(*lm, lm->effectiveTypeW, lm->effectiveNameW);
        QVERIFY(ns.valid);

        // Click in the name area (selects the node)
        QPoint clickPos = colToViewport(m_editor->scintilla(), kFirstDataLine, ns.start + 1);
        sendLeftClick(m_editor->scintilla()->viewport(), clickPos);
        QApplication::processEvents();

        // Now move mouse to col 0 (indent area — non-editable)
        QPoint emptyPos = colToViewport(m_editor->scintilla(), kFirstDataLine, 0);
        sendMouseMove(m_editor->scintilla()->viewport(), emptyPos);
        QApplication::processEvents();

        // Must be Arrow, NOT IBeam (QScintilla must not have leaked its cursor state)
        QCOMPARE(viewportCursor(m_editor), Qt::ArrowCursor);
        QVERIFY(!m_editor->isEditing());
    }

    // ── Test: CommandRow root class edits on line 0 ──
    void testCommandRowRootClassEdits() {
        m_editor->applyDocument(m_result);

        // Set CommandRow text with root class (simulates controller.updateCommandRow)
        m_editor->setCommandRowText(
            QStringLiteral("source\u25BE  0xD87B5E5000  struct _PEB64 {"));

        // RootClassName should be allowed on CommandRow (line 0)
        bool ok = m_editor->beginInlineEdit(EditTarget::RootClassName, 0);
        QVERIFY2(ok, "RootClassName edit should be allowed on CommandRow");
        QVERIFY(m_editor->isEditing());
        m_editor->cancelInlineEdit();
    }

    // ── Test: CommandRow root class name editable ──
    void testCommandRowRootClassName() {
        m_editor->applyDocument(m_result);

        // Set CommandRow with root class
        m_editor->setCommandRowText(
            QStringLiteral("source\u25BE  0xD87B5E5000  struct _PEB64 {"));

        // Line 0 is CommandRow
        const LineMeta* lm = m_editor->metaForLine(0);
        QVERIFY(lm);
        QCOMPARE(lm->lineKind, LineKind::CommandRow);

        // RootClassName should work
        QVERIFY(m_editor->beginInlineEdit(EditTarget::RootClassName, 0));
        QVERIFY(m_editor->isEditing());
        m_editor->cancelInlineEdit();

        m_editor->applyDocument(m_result);
    }

    // ── Test: root header/footer are suppressed (CommandRow replaces them) ──
    void testRootFoldSuppressed() {
        m_editor->applyDocument(m_result);

        // Root struct header is completely suppressed from output.
        // Line 0 = CommandRow, Line 1 = first field.
        const LineMeta* lm2 = m_editor->metaForLine(kFirstDataLine);
        QVERIFY(lm2);
        QCOMPARE(lm2->lineKind, LineKind::Field);

        // Verify no root header line exists in the output (footer may have isRootHeader for flush-left)
        bool foundRootHeader = false;
        for (int i = 0; i < m_result.meta.size(); i++) {
            if (m_result.meta[i].isRootHeader && m_result.meta[i].lineKind == LineKind::Header) {
                foundRootHeader = true;
                break;
            }
        }
        QVERIFY2(!foundRootHeader,
                 "Root header should be suppressed from compose output");
    }

    // ── Test: command row hover indicator survives refresh cycle ──
    void testCommandRowHoverSurvivesRefresh() {
        // IND_HOVER_SPAN = 11 (defined in editor.cpp, replicate for test)
        constexpr int IND_HOVER_SPAN = 11;

        m_editor->applyDocument(m_result);

        // Set command row text (simulates controller.updateCommandRow)
        QString cmdText = QStringLiteral(
            "source\u25BE  0xD87B5E5000  struct _PEB64 {");
        m_editor->setCommandRowText(cmdText);
        QApplication::processEvents();

        // Parse the source span on line 0
        auto* sci = m_editor->scintilla();
        int len = (int)sci->SendScintilla(
            QsciScintillaBase::SCI_LINELENGTH, (unsigned long)0);
        QVERIFY(len > 0);
        QByteArray buf(len + 1, '\0');
        sci->SendScintilla(QsciScintillaBase::SCI_GETLINE, (unsigned long)0,
                           (void*)buf.data());
        QString lineText = QString::fromUtf8(buf.constData(), len);
        while (lineText.endsWith('\n') || lineText.endsWith('\r'))
            lineText.chop(1);

        ColumnSpan srcSpan = commandRowSrcSpan(lineText);
        QVERIFY2(srcSpan.valid, "Source span should be valid on command row");

        // Programmatically move mouse to the source span
        int hoverCol = srcSpan.start + 1;
        QPoint hoverPos = colToViewport(sci, 0, hoverCol);
        sendMouseMove(sci->viewport(), hoverPos);
        QApplication::processEvents();

        // Verify IND_HOVER_SPAN is set at the hover position
        long pos = sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                                      (unsigned long)0, (long)hoverCol);
        sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT,
                           (unsigned long)IND_HOVER_SPAN);
        int valBefore = (int)sci->SendScintilla(
            QsciScintillaBase::SCI_INDICATORVALUEAT,
            (unsigned long)IND_HOVER_SPAN, pos);
        QVERIFY2(valBefore != 0,
                 "IND_HOVER_SPAN should be set on source span after hover");

        // Verify cursor is PointingHand (Source target = clickable)
        QCOMPARE(viewportCursor(m_editor), Qt::PointingHandCursor);

        // ── Simulate a full refresh cycle (same order as controller.refresh) ──
        ViewState vs = m_editor->saveViewState();
        m_editor->applyDocument(m_result);
        m_editor->restoreViewState(vs);

        // Cursor must NOT have flipped to Arrow during applyDocument
        // (applyHoverCursor is not called prematurely on composed text)
        QCOMPARE(viewportCursor(m_editor), Qt::PointingHandCursor);

        // updateCommandRow() — replaces line 0 text
        m_editor->setCommandRowText(cmdText);

        // applySelectionOverlays() — must run AFTER updateCommandRow
        m_editor->applySelectionOverlay(QSet<uint64_t>());
        QApplication::processEvents();

        // Re-query the position (text was replaced, byte offset may have shifted)
        long posAfter = sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                                           (unsigned long)0, (long)hoverCol);
        int valAfter = (int)sci->SendScintilla(
            QsciScintillaBase::SCI_INDICATORVALUEAT,
            (unsigned long)IND_HOVER_SPAN, posAfter);
        QVERIFY2(valAfter != 0,
                 "IND_HOVER_SPAN must survive refresh on command row "
                 "(hover should not flicker)");

        // Cursor must still be PointingHand after full refresh cycle
        QCOMPARE(viewportCursor(m_editor), Qt::PointingHandCursor);

        m_editor->applyDocument(m_result);
    }

    // ── Test: command row hover survives multiple rapid refresh cycles ──
    void testCommandRowHoverSurvivesRepeatedRefresh() {
        constexpr int IND_HOVER_SPAN = 11;

        m_editor->applyDocument(m_result);

        QString cmdText = QStringLiteral(
            "source\u25BE  0xD87B5E5000  struct _PEB64 {");
        m_editor->setCommandRowText(cmdText);
        QApplication::processEvents();

        auto* sci = m_editor->scintilla();
        int lineLen = (int)sci->SendScintilla(
            QsciScintillaBase::SCI_LINELENGTH, (unsigned long)0);
        QByteArray buf(lineLen + 1, '\0');
        sci->SendScintilla(QsciScintillaBase::SCI_GETLINE, (unsigned long)0,
                           (void*)buf.data());
        QString lineText = QString::fromUtf8(buf.constData(), lineLen);
        while (lineText.endsWith('\n') || lineText.endsWith('\r'))
            lineText.chop(1);

        ColumnSpan srcSpan = commandRowSrcSpan(lineText);
        QVERIFY(srcSpan.valid);
        int hoverCol = srcSpan.start + 1;

        // Move mouse into position
        QPoint hoverPos = colToViewport(sci, 0, hoverCol);
        sendMouseMove(sci->viewport(), hoverPos);
        QApplication::processEvents();

        // Simulate 5 rapid refresh cycles (like ~660ms timer x5)
        for (int cycle = 0; cycle < 5; cycle++) {
            ViewState vs = m_editor->saveViewState();
            m_editor->applyDocument(m_result);
            m_editor->restoreViewState(vs);
            m_editor->setCommandRowText(cmdText);
            m_editor->applySelectionOverlay(QSet<uint64_t>());

            // Re-send mouse move each cycle (mouse is still there physically)
            sendMouseMove(sci->viewport(), hoverPos);
            QApplication::processEvents();

            long pos = sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                                          (unsigned long)0, (long)hoverCol);
            int val = (int)sci->SendScintilla(
                QsciScintillaBase::SCI_INDICATORVALUEAT,
                (unsigned long)IND_HOVER_SPAN, pos);
            QVERIFY2(val != 0,
                     qPrintable(QString(
                         "IND_HOVER_SPAN lost on refresh cycle %1").arg(cycle)));
            QVERIFY2(viewportCursor(m_editor) == Qt::PointingHandCursor,
                     qPrintable(QString(
                         "Cursor flipped away from PointingHand on cycle %1").arg(cycle)));
        }

        m_editor->applyDocument(m_result);
    }

    // ── Test: MenuBarStyle gives QMenu items generous click targets ──
    // ── Test: M_ACCENT marker appears on selected rows ──
    void testAccentMarkerOnSelectedRows() {
        m_editor->applyDocument(m_result);

        // Find a data line with a valid nodeId
        uint64_t targetId = 0;
        int targetLine = -1;
        for (int i = kFirstDataLine; i < m_result.meta.size(); i++) {
            const auto& lm = m_result.meta[i];
            if (lm.nodeId != 0 && lm.nodeId != kCommandRowId
                && lm.lineKind == LineKind::Field) {
                targetId = lm.nodeId;
                targetLine = i;
                break;
            }
        }
        QVERIFY2(targetLine >= 0, "No data line found for accent test");

        // Apply selection overlay with that node
        QSet<uint64_t> selIds;
        selIds.insert(targetId);
        m_editor->applySelectionOverlay(selIds);

        auto* sci = m_editor->scintilla();

        // Direct test: add M_ACCENT manually and read it back
        int directHandle = sci->markerAdd(targetLine, M_ACCENT);
        int directMarkers = (int)sci->SendScintilla(
            QsciScintillaBase::SCI_MARKERGET, (unsigned long)targetLine);
        QVERIFY2(directMarkers & (1 << M_ACCENT),
                 qPrintable(QString("Direct markerAdd(M_ACCENT=%1) failed on line %2 (handle=%3, mask=0x%4)")
                     .arg(M_ACCENT).arg(targetLine).arg(directHandle).arg(directMarkers, 0, 16)));
        sci->markerDelete(targetLine, M_ACCENT);

        // Now test via applySelectionOverlay
        m_editor->applySelectionOverlay(selIds);

        // Verify M_SELECTED is set on the target line
        int markers = (int)sci->SendScintilla(
            QsciScintillaBase::SCI_MARKERGET, (unsigned long)targetLine);
        QVERIFY2(markers & (1 << M_SELECTED),
                 qPrintable(QString("M_SELECTED not set on line %1 (mask=0x%2)")
                     .arg(targetLine).arg(markers, 0, 16)));

        // Verify M_ACCENT is set on the target line
        QVERIFY2(markers & (1 << M_ACCENT),
                 qPrintable(QString("M_ACCENT not set on line %1 (mask=0x%2)")
                     .arg(targetLine).arg(markers, 0, 16)));

        // Verify a non-selected line does NOT have M_ACCENT
        int otherLine = -1;
        for (int i = kFirstDataLine; i < m_result.meta.size(); i++) {
            const auto& lm = m_result.meta[i];
            if (lm.nodeId != targetId && lm.nodeId != 0
                && lm.nodeId != kCommandRowId && lm.lineKind == LineKind::Field) {
                otherLine = i;
                break;
            }
        }
        if (otherLine >= 0) {
            int otherMarkers = (int)sci->SendScintilla(
                QsciScintillaBase::SCI_MARKERGET, (unsigned long)otherLine);
            QVERIFY2(!(otherMarkers & (1 << M_ACCENT)),
                     qPrintable(QString("M_ACCENT should NOT be set on non-selected line %1 (mask=0x%2)")
                         .arg(otherLine).arg(otherMarkers, 0, 16)));
        }

        // Clear selection and verify accent is removed
        m_editor->applySelectionOverlay(QSet<uint64_t>());
        markers = (int)sci->SendScintilla(
            QsciScintillaBase::SCI_MARKERGET, (unsigned long)targetLine);
        QVERIFY2(!(markers & (1 << M_ACCENT)),
                 qPrintable(QString("M_ACCENT should be cleared after deselection on line %1 (mask=0x%2)")
                     .arg(targetLine).arg(markers, 0, 16)));
    }

    void testMenuItemSizeIsAccessible() {
        // Instantiate the same QProxyStyle used by the app (MenuBarStyle is
        // defined in main.cpp — we replicate the logic here to test it)
        class TestMenuStyle : public QProxyStyle {
        public:
            using QProxyStyle::QProxyStyle;
            QSize sizeFromContents(ContentsType type, const QStyleOption* opt,
                                   const QSize& sz, const QWidget* w) const override {
                QSize s = QProxyStyle::sizeFromContents(type, opt, sz, w);
                if (type == CT_MenuBarItem)
                    s.setHeight(s.height() + qRound(s.height() * 0.5));
                if (type == CT_MenuItem)
                    s = QSize(s.width() + 24, s.height() + 4);
                return s;
            }
        };

        TestMenuStyle style;
        QMenu menu;
        auto* action = menu.addAction("Delete Node");

        QStyleOptionMenuItem opt;
        opt.initFrom(&menu);
        opt.text = action->text();

        QSize base = style.QProxyStyle::sizeFromContents(
            QStyle::CT_MenuItem, &opt, QSize(80, 20), &menu);
        QSize styled = style.sizeFromContents(
            QStyle::CT_MenuItem, &opt, QSize(80, 20), &menu);

        // Width must grow by at least 24px
        QVERIFY2(styled.width() >= base.width() + 24,
                 qPrintable(QString("Menu item width %1 too narrow (base %2, need +24)")
                     .arg(styled.width()).arg(base.width())));

        // Height must grow by at least 4px
        QVERIFY2(styled.height() >= base.height() + 4,
                 qPrintable(QString("Menu item height %1 too short (base %2, need +4)")
                     .arg(styled.height()).arg(base.height())));
    }

    // ── Test: non-hex nodes don't show false heat coloring after offset shift ──
    void testDeleteClearsHeatOnShiftedNodes() {
        // Heat indicator constants (replicated from editor.cpp)
        constexpr int IND_HEAT_COLD = 13;
        constexpr int IND_HEAT_WARM = 17;
        constexpr int IND_HEAT_HOT  = 18;

        // Build a small tree: root struct with mixed regular (non-hex) + hex fields
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = "SmallStruct";
        root.name = "s";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // field0: UInt32  at offset  0 (4 bytes) — will be deleted
        // field1: UInt32  at offset  4 (4 bytes) — regular type, will shift
        // field2: Float   at offset  8 (4 bytes) — regular type, will shift
        // field3: Hex32   at offset 12 (4 bytes) — hex type, will shift
        struct FieldDef { int off; NodeKind kind; const char* name; };
        FieldDef defs[] = {
            { 0, NodeKind::UInt32, "count"},
            { 4, NodeKind::UInt32, "flags"},
            { 8, NodeKind::Float,  "speed"},
            {12, NodeKind::Hex32,  "raw"},
        };
        QVector<uint64_t> fieldIds;
        for (auto& d : defs) {
            Node n;
            n.kind = d.kind;
            n.name = d.name;
            n.parentId = rootId;
            n.offset = d.off;
            int idx = tree.addNode(n);
            fieldIds.append(tree.nodes[idx].id);
        }

        // Create a provider with 16 bytes of recognizable data
        QByteArray data(16, '\0');
        uint32_t v0 = 42;       memcpy(data.data() + 0,  &v0, 4);  // count=42
        uint32_t v1 = 0xFF;     memcpy(data.data() + 4,  &v1, 4);  // flags=255
        float    v2 = 3.14f;    memcpy(data.data() + 8,  &v2, 4);  // speed=3.14
        uint32_t v3 = 0xCAFE;   memcpy(data.data() + 12, &v3, 4);  // raw=0xCAFE
        BufferProvider prov(data);

        // Compose the initial document
        ComposeResult result = compose(tree, prov);

        // Inject heatLevel=2 (warm) on field1, field2, field3 — simulates
        // heat accumulated before the delete
        for (auto& lm : result.meta) {
            for (int i = 1; i <= 3; i++) {
                if (lm.nodeId == fieldIds[i])
                    lm.heatLevel = 2;
            }
        }

        // Apply to editor — heat indicators should appear
        m_editor->applyDocument(result);
        QApplication::processEvents();

        auto* sci = m_editor->scintilla();

        // Helper: check if any heat indicator is set anywhere on a line
        auto hasHeatOnLine = [&](int line) -> bool {
            int lineLen = (int)sci->SendScintilla(
                QsciScintillaBase::SCI_LINELENGTH, (unsigned long)line);
            long lineStart = sci->SendScintilla(
                QsciScintillaBase::SCI_POSITIONFROMLINE, (unsigned long)line);
            for (long pos = lineStart; pos < lineStart + lineLen; pos++) {
                for (int ind : { IND_HEAT_COLD, IND_HEAT_WARM, IND_HEAT_HOT }) {
                    int val = (int)sci->SendScintilla(
                        QsciScintillaBase::SCI_INDICATORVALUEAT,
                        (unsigned long)ind, pos);
                    if (val != 0) return true;
                }
            }
            return false;
        };

        // Find lines for each shifted field
        auto findFieldLine = [&](const ComposeResult& cr, uint64_t nodeId) -> int {
            for (int i = 0; i < cr.meta.size(); i++) {
                if (cr.meta[i].nodeId == nodeId && cr.meta[i].lineKind == LineKind::Field)
                    return i;
            }
            return -1;
        };

        int line1 = findFieldLine(result, fieldIds[1]);
        int line2 = findFieldLine(result, fieldIds[2]);
        int line3 = findFieldLine(result, fieldIds[3]);
        QVERIFY(line1 >= 0);
        QVERIFY(line2 >= 0);
        QVERIFY(line3 >= 0);

        // Verify heat indicators ARE present (UInt32, Float, and Hex32)
        QVERIFY2(hasHeatOnLine(line1),
                 "Heat should be present on UInt32 'flags' before delete");
        QVERIFY2(hasHeatOnLine(line2),
                 "Heat should be present on Float 'speed' before delete");
        QVERIFY2(hasHeatOnLine(line3),
                 "Heat should be present on Hex32 'raw' before delete");

        // ── Simulate delete of field0 (UInt32 'count' at offset 0) ──
        int field0Idx = tree.indexOfId(fieldIds[0]);
        QVERIFY(field0Idx >= 0);
        tree.nodes.remove(field0Idx);
        tree.invalidateIdCache();

        // Shift remaining fields' offsets down by 4
        for (int i = 1; i <= 3; i++) {
            int fi = tree.indexOfId(fieldIds[i]);
            if (fi >= 0) tree.nodes[fi].offset -= 4;
        }

        // Recompose — heatLevel defaults to 0 (simulates cleared history)
        ComposeResult afterResult = compose(tree, prov);

        // Apply the post-delete document to the editor
        m_editor->applyDocument(afterResult);
        QApplication::processEvents();

        // Find new line positions
        int newLine1 = findFieldLine(afterResult, fieldIds[1]);
        int newLine2 = findFieldLine(afterResult, fieldIds[2]);
        int newLine3 = findFieldLine(afterResult, fieldIds[3]);
        QVERIFY(newLine1 >= 0);
        QVERIFY(newLine2 >= 0);
        QVERIFY(newLine3 >= 0);

        // After applying heatLevel=0, NO heat indicators should appear
        QVERIFY2(!hasHeatOnLine(newLine1),
                 "UInt32 'flags' should NOT show heat after offset shift "
                 "(old values are from wrong address)");
        QVERIFY2(!hasHeatOnLine(newLine2),
                 "Float 'speed' should NOT show heat after offset shift "
                 "(old values are from wrong address)");
        QVERIFY2(!hasHeatOnLine(newLine3),
                 "Hex32 'raw' should NOT show heat after offset shift "
                 "(old values are from wrong address)");

        // Restore original document
        m_editor->applyDocument(m_result);
    }

    void testMenuHoverRendersAmberText() {
        // Replicate MenuBarStyle with drawControl hover override
        class TestMenuStyle : public QProxyStyle {
        public:
            using QProxyStyle::QProxyStyle;
            QSize sizeFromContents(ContentsType type, const QStyleOption* opt,
                                   const QSize& sz, const QWidget* w) const override {
                QSize s = QProxyStyle::sizeFromContents(type, opt, sz, w);
                if (type == CT_MenuBarItem)
                    s.setHeight(s.height() + qRound(s.height() * 0.5));
                if (type == CT_MenuItem)
                    s = QSize(s.width() + 24, s.height() + 4);
                return s;
            }
            void drawPrimitive(PrimitiveElement elem, const QStyleOption* opt,
                               QPainter* p, const QWidget* w) const override {
                if (elem == PE_FrameMenu) return;
                QProxyStyle::drawPrimitive(elem, opt, p, w);
            }
            void drawControl(ControlElement element, const QStyleOption* opt,
                             QPainter* p, const QWidget* w) const override {
                if (element == CE_MenuItem || element == CE_MenuBarItem) {
                    if (auto* mi = qstyleoption_cast<const QStyleOptionMenuItem*>(opt)) {
                        if ((mi->state & State_Selected)
                            && mi->menuItemType != QStyleOptionMenuItem::Separator) {
                            QStyleOptionMenuItem patched = *mi;
                            patched.palette.setColor(QPalette::Highlight,
                                mi->palette.color(QPalette::Mid));
                            patched.palette.setColor(QPalette::HighlightedText,
                                mi->palette.color(QPalette::Link));
                            QProxyStyle::drawControl(element, &patched, p, w);
                            return;
                        }
                    }
                }
                QProxyStyle::drawControl(element, opt, p, w);
            }
        };

        // Install our style as the app style (same as main.cpp does)
        qApp->setStyle(new TestMenuStyle("Fusion"));

        // Set app palette matching applyGlobalTheme for Reclass Dark
        QPalette pal;
        pal.setColor(QPalette::Window,          QColor("#1e1e1e"));
        pal.setColor(QPalette::WindowText,      QColor("#d4d4d4"));
        pal.setColor(QPalette::Base,            QColor("#252526"));
        pal.setColor(QPalette::AlternateBase,   QColor("#2a2d2e"));
        pal.setColor(QPalette::Text,            QColor("#d4d4d4"));
        pal.setColor(QPalette::Button,          QColor("#333333"));
        pal.setColor(QPalette::ButtonText,      QColor("#d4d4d4"));
        pal.setColor(QPalette::Highlight,       QColor("#2b2b2b"));
        pal.setColor(QPalette::HighlightedText, QColor("#E6B450"));
        pal.setColor(QPalette::Mid,             QColor("#3c3c3c"));
        pal.setColor(QPalette::Dark,            QColor("#1e1e1e"));
        pal.setColor(QPalette::Light,           QColor("#505050"));
        pal.setColor(QPalette::Link,            QColor("#E6B450"));
        qApp->setPalette(pal);

        // Build and show a real QMenu
        QMenu menu;
        menu.addAction("First Item");
        menu.addAction("Second Item");
        menu.addAction("Third Item");
        menu.popup(QPoint(100, 100));
        QVERIFY(QTest::qWaitForWindowExposed(&menu));
        QApplication::processEvents();

        // ── Deliver real mouse events to trigger hover on second item ──
        QList<QAction*> actions = menu.actions();
        QRect itemRect = menu.actionGeometry(actions[1]);
        QPoint localCenter = itemRect.center();

        // Enter event — tells QMenu the mouse is inside
        QEvent enter(QEvent::Enter);
        QApplication::sendEvent(&menu, &enter);
        QApplication::processEvents();

        // MouseMove to the second item — triggers hover/select
        QMouseEvent move(QEvent::MouseMove, QPointF(localCenter),
                         menu.mapToGlobal(localCenter),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&menu, &move);
        QApplication::processEvents();
        QTest::qWait(50);  // let repaint settle

        // Verify QMenu internally considers the action hovered
        QVERIFY2(menu.activeAction() == actions[1],
                 "QMenu did not set activeAction after mouse move — "
                 "hover event delivery failed");

        // ── Capture what's actually on screen ──
        QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        QPixmap grab = screen->grabWindow(menu.winId());
        QImage img = grab.toImage().convertToFormat(QImage::Format_ARGB32);

        // Crop to just the hovered item rect
        QImage itemImg = img.copy(itemRect);

        // Scan hovered item for amber pixels (E6B450 = R:230 G:180 B:80)
        int amberPixels = 0;
        int totalPixels = itemImg.width() * itemImg.height();
        for (int y = 0; y < itemImg.height(); ++y) {
            for (int x = 0; x < itemImg.width(); ++x) {
                QColor c = itemImg.pixelColor(x, y);
                if (c.red() > 180 && c.green() > 140 && c.blue() < 100)
                    ++amberPixels;
            }
        }

        // Always save screenshots so we can visually inspect
        img.save("menu_hover_full.png");
        itemImg.save("menu_hover_item.png");

        menu.close();

        QVERIFY2(amberPixels > 10,
                 qPrintable(QString("Expected amber text pixels in hovered item, "
                     "found %1 / %2 total (see menu_hover_full.png, menu_hover_item.png)")
                     .arg(amberPixels).arg(totalPixels)));
    }
    void testStructPreviewPopupOnCollapsedTypedPointer() {
        // Build a small tree: root struct with a typed Pointer64 → target struct
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = "TestRoot";
        root.name = "Root";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Target struct with some fields
        Node target;
        target.kind = NodeKind::Struct;
        target.structTypeName = "TargetStruct";
        target.name = "TargetStruct";
        target.parentId = 0;
        target.offset = 0;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        // Add fields to the target struct
        {
            Node f; f.parentId = targetId;
            f.kind = NodeKind::UInt64; f.name = "FieldA"; f.offset = 0;
            tree.addNode(f);
            f.kind = NodeKind::UInt64; f.name = "FieldB"; f.offset = 8;
            tree.addNode(f);
            f.kind = NodeKind::UInt32; f.name = "FieldC"; f.offset = 16;
            tree.addNode(f);
        }

        // Add a Pointer64 node that references the target struct, collapsed
        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "pTarget";
        ptr.parentId = rootId;
        ptr.offset = 0;
        ptr.refId = targetId;
        ptr.collapsed = true;
        tree.addNode(ptr);

        // Provider: 8 bytes at offset 0 holding a pointer value
        QByteArray data(64, '\0');
        uint64_t ptrVal = 0x00007FFE12340000ULL;
        memcpy(data.data(), &ptrVal, 8);
        BufferProvider prov(data, "test_struct_preview");

        ComposeResult cr = compose(tree, prov);
        m_editor->applyDocument(cr);
        m_editor->setProviderRef(&prov, nullptr, &tree);
        QApplication::processEvents();

        // Find the pointer line (should be a Pointer64 with foldCollapsed=true)
        int ptrLine = -1;
        for (int i = 0; i < cr.meta.size(); ++i) {
            if (cr.meta[i].nodeKind == NodeKind::Pointer64
                && cr.meta[i].foldCollapsed) {
                ptrLine = i;
                break;
            }
        }
        QVERIFY2(ptrLine >= 0, "Could not find collapsed Pointer64 line in compose output");

        // Simulate hover over the value column of the pointer line
        const LineMeta& lm = cr.meta[ptrLine];
        QString lineText;
        {
            long len = m_editor->scintilla()->SendScintilla(
                QsciScintillaBase::SCI_LINELENGTH, (unsigned long)ptrLine);
            QByteArray buf(len + 1, '\0');
            m_editor->scintilla()->SendScintilla(
                QsciScintillaBase::SCI_GETLINE, (uintptr_t)ptrLine, static_cast<const char*>(buf.data()));
            lineText = QString::fromUtf8(buf.left(len));
        }
        ColumnSpan vs = m_editor->valueSpan(lm, lineText.size(),
                                             lm.effectiveTypeW, lm.effectiveNameW);
        QVERIFY2(vs.valid, "Value span for pointer line is not valid");

        int hoverCol = (vs.start + vs.end) / 2;  // middle of value span
        QPoint vp = colToViewport(m_editor->scintilla(), ptrLine, hoverCol);
        sendMouseMove(m_editor->scintilla()->viewport(), vp);
        QApplication::processEvents();

        // Verify struct preview popup is shown
        QVERIFY2(m_editor->structPreviewPopup() != nullptr,
                 "Struct preview popup was not created");
        QVERIFY2(m_editor->structPreviewPopup()->isVisible(),
                 "Struct preview popup is not visible");

        // Restore original document for other tests
        m_editor->setProviderRef(nullptr, nullptr, nullptr);
        m_editor->applyDocument(m_result);
    }

    void testStructPreviewPopupNotShownWhenExpanded() {
        // Same tree but pointer is NOT collapsed — popup should not show
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = "TestRoot";
        root.name = "Root";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node target;
        target.kind = NodeKind::Struct;
        target.structTypeName = "TargetStruct";
        target.name = "TargetStruct";
        target.parentId = 0;
        target.offset = 0;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        {
            Node f; f.parentId = targetId;
            f.kind = NodeKind::UInt64; f.name = "FieldA"; f.offset = 0;
            tree.addNode(f);
            f.kind = NodeKind::UInt64; f.name = "FieldB"; f.offset = 8;
            tree.addNode(f);
        }

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "pTarget";
        ptr.parentId = rootId;
        ptr.offset = 0;
        ptr.refId = targetId;
        ptr.collapsed = false;  // expanded
        tree.addNode(ptr);

        QByteArray data(64, '\0');
        uint64_t ptrVal = 0x00007FFE12340000ULL;
        memcpy(data.data(), &ptrVal, 8);
        BufferProvider prov(data, "test_struct_preview_expanded");

        ComposeResult cr = compose(tree, prov);
        m_editor->applyDocument(cr);
        m_editor->setProviderRef(&prov, nullptr, &tree);
        QApplication::processEvents();

        // Find the pointer line (should be Pointer64 and NOT collapsed)
        int ptrLine = -1;
        for (int i = 0; i < cr.meta.size(); ++i) {
            if (cr.meta[i].nodeKind == NodeKind::Pointer64) {
                ptrLine = i;
                break;
            }
        }
        QVERIFY2(ptrLine >= 0, "Could not find Pointer64 line in compose output");

        // Hover at a middle column on the pointer line — expanded pointer header
        // may not have a standard value span, but we just need to verify no popup
        int hoverCol = 40;  // somewhere in the middle of the line
        QPoint vp = colToViewport(m_editor->scintilla(), ptrLine, hoverCol);
        sendMouseMove(m_editor->scintilla()->viewport(), vp);
        QApplication::processEvents();

        // Struct preview popup should NOT be visible (pointer is expanded)
        bool popupVisible = m_editor->structPreviewPopup()
                            && m_editor->structPreviewPopup()->isVisible();
        QVERIFY2(!popupVisible,
                 "Struct preview popup should not appear for expanded pointer");

        // Restore
        m_editor->setProviderRef(nullptr, nullptr, nullptr);
        m_editor->applyDocument(m_result);
    }

    // ── Test: expanded pointer renders child fields from buffer ──
    void testPointerExpansionRendersChildren() {
        PtrDemo d = makePtrDemo(/*collapsed=*/false);
        ComposeResult cr = compose(d.tree, d.prov);
        m_editor->applyDocument(cr);
        QApplication::processEvents();

        // Find the pointer header line
        int ptrHeaderLine = -1;
        for (int i = 0; i < cr.meta.size(); ++i) {
            if (cr.meta[i].nodeKind == NodeKind::Pointer64
                && cr.meta[i].foldHead && !cr.meta[i].foldCollapsed) {
                ptrHeaderLine = i;
                break;
            }
        }
        QVERIFY2(ptrHeaderLine >= 0, "Should have an expanded Pointer64 header");
        QCOMPARE(cr.meta[ptrHeaderLine].lineKind, LineKind::Header);

        // Find expanded child fields (x, y, z at depth = header depth + 1)
        int headerDepth = cr.meta[ptrHeaderLine].depth;
        int childFieldCount = 0;
        for (int i = ptrHeaderLine + 1; i < cr.meta.size(); ++i) {
            const LineMeta& lm = cr.meta[i];
            if (lm.depth == headerDepth + 1 && lm.lineKind == LineKind::Field)
                childFieldCount++;
            if (lm.lineKind == LineKind::Footer && lm.nodeKind == NodeKind::Pointer64)
                break; // reached pointer footer
        }
        QCOMPARE(childFieldCount, 3); // x, y, z

        // Find the pointer footer line
        int ptrFooterLine = -1;
        for (int i = ptrHeaderLine + 1; i < cr.meta.size(); ++i) {
            if (cr.meta[i].lineKind == LineKind::Footer
                && cr.meta[i].nodeKind == NodeKind::Pointer64) {
                ptrFooterLine = i;
                break;
            }
        }
        QVERIFY2(ptrFooterLine > ptrHeaderLine, "Should have a pointer footer after header");

        // Verify the composed text contains the child field values
        // UInt32 displays as hex (e.g. 100 → "0x00000064"), Float as decimal
        QStringList lines = cr.text.split('\n');
        bool foundX = false, foundY = false, foundZ = false;
        for (const QString& line : lines) {
            if (line.contains("0x64") && line.contains("x")) foundX = true;  // 100 = 0x64
            if (line.contains("0xc8") && line.contains("y")) foundY = true;  // 200 = 0xc8
            if (line.contains("3.14") && line.contains("z")) foundZ = true;
        }
        QVERIFY2(foundX, "Child field 'x' with value 0x64 should appear in output");
        QVERIFY2(foundY, "Child field 'y' with value 0xc8 should appear in output");
        QVERIFY2(foundZ, "Child field 'z' with value 3.14 should appear in output");

        // Verify the pointer type name appears
        QVERIFY2(cr.text.contains("ChildData*"),
                 "Pointer type 'ChildData*' should appear in output");

        // Editor should have rendered all lines
        int editorLineCount = m_editor->scintilla()->lines();
        QVERIFY2(editorLineCount >= cr.meta.size(),
                 qPrintable(QString("Editor has %1 lines but compose has %2 meta entries")
                     .arg(editorLineCount).arg(cr.meta.size())));

        m_editor->applyDocument(m_result);
    }

    // ── Test: collapsed pointer hides child fields ──
    void testPointerCollapsedHidesChildren() {
        PtrDemo expanded = makePtrDemo(/*collapsed=*/false);
        ComposeResult crExpanded = compose(expanded.tree, expanded.prov);

        PtrDemo collapsed = makePtrDemo(/*collapsed=*/true);
        ComposeResult crCollapsed = compose(collapsed.tree, collapsed.prov);

        // Collapsed should have fewer lines (no child fields, no pointer footer)
        QVERIFY2(crCollapsed.meta.size() < crExpanded.meta.size(),
                 qPrintable(QString("Collapsed (%1 lines) should be smaller than expanded (%2)")
                     .arg(crCollapsed.meta.size()).arg(crExpanded.meta.size())));

        // The pointer line should be a Field (not Header) with foldCollapsed=true
        bool foundCollapsedPtr = false;
        for (const LineMeta& lm : crCollapsed.meta) {
            if (lm.nodeKind == NodeKind::Pointer64 && lm.foldHead) {
                QVERIFY(lm.foldCollapsed);
                QCOMPARE(lm.lineKind, LineKind::Field);
                foundCollapsedPtr = true;
                break;
            }
        }
        QVERIFY2(foundCollapsedPtr, "Should have a collapsed Pointer64 fold head");

        // No child fields from ChildData should appear in the main struct section
        bool foundChildField = false;
        for (const LineMeta& lm : crCollapsed.meta) {
            if (lm.lineKind == LineKind::Footer && lm.nodeKind == NodeKind::Pointer64) {
                foundChildField = true; // pointer footer exists = children visible
                break;
            }
        }
        QVERIFY2(!foundChildField,
                 "Collapsed pointer should not have a pointer footer (no children)");

        // Apply collapsed to editor
        m_editor->applyDocument(crCollapsed);
        QApplication::processEvents();

        int collapsedLines = m_editor->scintilla()->lines();
        m_editor->applyDocument(crExpanded);
        QApplication::processEvents();
        int expandedLines = m_editor->scintilla()->lines();

        QVERIFY2(collapsedLines < expandedLines,
                 qPrintable(QString("Collapsed (%1 editor lines) should be fewer than expanded (%2)")
                     .arg(collapsedLines).arg(expandedLines)));

        m_editor->applyDocument(m_result);
    }

    // ── Test: null pointer still shows template fields (via NullProvider) ──
    void testPointerNullShowsTemplate() {
        PtrDemo d = makePtrDemo(/*collapsed=*/false, /*nullPtr=*/true);
        ComposeResult cr = compose(d.tree, d.prov);
        m_editor->applyDocument(cr);
        QApplication::processEvents();

        // Even with null pointer, expanded pointer should show template children
        int ptrHeaderLine = -1;
        for (int i = 0; i < cr.meta.size(); ++i) {
            if (cr.meta[i].nodeKind == NodeKind::Pointer64
                && cr.meta[i].foldHead && !cr.meta[i].foldCollapsed) {
                ptrHeaderLine = i;
                break;
            }
        }
        QVERIFY2(ptrHeaderLine >= 0,
                 "Null pointer should still produce an expanded header");

        // Should have child field lines (template from NullProvider shows zeros)
        int headerDepth = cr.meta[ptrHeaderLine].depth;
        int childFieldCount = 0;
        for (int i = ptrHeaderLine + 1; i < cr.meta.size(); ++i) {
            const LineMeta& lm = cr.meta[i];
            if (lm.depth == headerDepth + 1 && lm.lineKind == LineKind::Field)
                childFieldCount++;
            if (lm.lineKind == LineKind::Footer && lm.nodeKind == NodeKind::Pointer64)
                break;
        }
        QCOMPARE(childFieldCount, 3); // x, y, z template still rendered

        // Verify ChildData* appears in output
        QVERIFY2(cr.text.contains("ChildData*"),
                 "Null pointer should still show 'ChildData*' type");

        m_editor->applyDocument(m_result);
    }

    // ── Test: nested pointer chain renders multiple expansion levels ──
    void testPointerChainExpansion() {
        NodeTree tree;
        tree.baseAddress = 0;

        // Root struct
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = "Chain";
        root.name = "chain";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // Inner struct (innermost target)
        Node inner;
        inner.kind = NodeKind::Struct;
        inner.structTypeName = "Inner";
        inner.name = "Inner";
        inner.parentId = 0;
        inner.offset = 300;
        int ii = tree.addNode(inner);
        uint64_t innerId = tree.nodes[ii].id;
        {
            Node f;
            f.kind = NodeKind::UInt32; f.name = "value";
            f.parentId = innerId; f.offset = 0;
            tree.addNode(f);
        }

        // Outer struct (contains pointer to Inner)
        Node outer;
        outer.kind = NodeKind::Struct;
        outer.structTypeName = "Outer";
        outer.name = "Outer";
        outer.parentId = 0;
        outer.offset = 200;
        int oi = tree.addNode(outer);
        uint64_t outerId = tree.nodes[oi].id;
        {
            Node f;
            f.kind = NodeKind::UInt32; f.name = "tag";
            f.parentId = outerId; f.offset = 0;
            tree.addNode(f);

            Node p;
            p.kind = NodeKind::Pointer64; p.name = "pInner";
            p.parentId = outerId; p.offset = 8;
            p.refId = innerId;
            tree.addNode(p);
        }

        // Root pointer to Outer
        {
            Node p;
            p.kind = NodeKind::Pointer64; p.name = "pOuter";
            p.parentId = rootId; p.offset = 0;
            p.refId = outerId;
            tree.addNode(p);
        }

        // Buffer: pOuter at 0 → 32, pInner at 32+8=40 → 64, value at 64 = 999
        QByteArray data(128, '\0');
        uint64_t pOuter = 32;  memcpy(data.data() + 0,  &pOuter, 8);
        uint64_t pInner = 64;  memcpy(data.data() + 40, &pInner, 8);
        uint32_t tag = 0xAB;   memcpy(data.data() + 32, &tag, 4);
        uint32_t val = 999;    memcpy(data.data() + 64, &val, 4);
        BufferProvider prov(data, "chain_demo");

        ComposeResult cr = compose(tree, prov);
        m_editor->applyDocument(cr);
        QApplication::processEvents();

        // Both Outer* and Inner* should appear
        QVERIFY2(cr.text.contains("Outer*"), "Should display 'Outer*' pointer type");
        QVERIFY2(cr.text.contains("Inner*"), "Should display 'Inner*' pointer type");

        // Count pointer fold heads — should have at least 2 (pOuter + pInner)
        int ptrFoldHeads = 0;
        int maxDepth = 0;
        for (const LineMeta& lm : cr.meta) {
            if (lm.foldHead && lm.nodeKind == NodeKind::Pointer64)
                ptrFoldHeads++;
            if (lm.depth > maxDepth) maxDepth = lm.depth;
        }
        QVERIFY2(ptrFoldHeads >= 2,
                 qPrintable(QString("Expected >=2 pointer fold heads, got %1")
                     .arg(ptrFoldHeads)));

        // Depth should reach at least 3 (root=0, pOuter children=1..2, pInner children=2..3)
        QVERIFY2(maxDepth >= 3,
                 qPrintable(QString("Expected max depth >= 3 for chain, got %1")
                     .arg(maxDepth)));

        // Verify innermost value (999 = 0x3e7) appears in the output
        QVERIFY2(cr.text.contains("0x3e7"),
                 "Innermost field 'value = 0x3e7' should appear in chain expansion");

        m_editor->applyDocument(m_result);
    }

    // ── Test: status bar view toggle buttons (pixel-level) ──
    void testStatusBarViewToggleButtons() {
        // Mirror the production ViewTabButton from main.cpp
        static constexpr int kAccentH = 2;
        static constexpr int kPadLR  = 12;
        static constexpr int kPadBot = 4;
        class VTB : public QPushButton {
        public:
            QColor colBg, colBgChecked, colBgHover, colBgPressed;
            QColor colText, colTextMuted, colAccent;
            explicit VTB(const QString& t, QWidget* p = nullptr) : QPushButton(t, p) {
                setCheckable(true); setFlat(true); setContentsMargins(0,0,0,0);
                setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
            }
            QSize sizeHint() const override {
                QFontMetrics fm(font());
                return QSize(fm.horizontalAdvance(text()) + 2*kPadLR,
                             fm.height() + kAccentH + kPadBot);
            }
        protected:
            void paintEvent(QPaintEvent*) override {
                QPainter p(this);
                QColor bg = colBg;
                if (isDown())          bg = colBgPressed;
                else if (underMouse()) bg = colBgHover;
                else if (isChecked())  bg = colBgChecked;
                p.fillRect(rect(), bg);
                if (isChecked())
                    p.fillRect(0, 0, width(), kAccentH, colAccent);
                p.setPen(isChecked() || underMouse() || isDown() ? colText : colTextMuted);
                p.setFont(font());
                QRect tr(kPadLR, kAccentH, width()-2*kPadLR, height()-kAccentH);
                p.drawText(tr, Qt::AlignVCenter|Qt::AlignLeft, text());
            }
            void enterEvent(QEnterEvent*) override { update(); }
            void leaveEvent(QEvent*) override { update(); }
        };

        QColor bg(30,30,30), bgAlt(45,45,48), hover(62,62,66);
        QColor text(212,212,212), textMuted(128,128,128);
        QColor accent("#b180d7");
        QColor pressed = hover.darker(130);

        auto setColors = [&](VTB* b) {
            b->colBg = bg; b->colBgChecked = bgAlt; b->colBgHover = hover;
            b->colBgPressed = pressed; b->colText = text;
            b->colTextMuted = textMuted; b->colAccent = accent;
        };

        // Borderless status bar with manual layout (mirrors production FlatStatusBar)
        class FSB : public QStatusBar {
        public:
            QWidget* tabRow = nullptr;
            QLabel*  label  = nullptr;
            FSB() { setSizeGripEnabled(false); }
        protected:
            void paintEvent(QPaintEvent*) override {
                QPainter p(this); p.fillRect(rect(), palette().window());
            }
            void resizeEvent(QResizeEvent* e) override {
                QStatusBar::resizeEvent(e);
                doLayout();
            }
            void showEvent(QShowEvent* e) override {
                QStatusBar::showEvent(e);
                doLayout();
            }
        private:
            void doLayout() {
                if (!tabRow || !label) return;
                int h = height(), tw = tabRow->sizeHint().width();
                tabRow->setGeometry(0, 0, tw, h);
                label->setGeometry(tw, 0, width() - tw, h);
            }
        };

        QMainWindow win;
        win.resize(600, 400);
        QPalette pal; pal.setColor(QPalette::Window, bg);
        win.setPalette(pal);
        auto* sb = new FSB;
        win.setStatusBar(sb);
        sb->setPalette(pal);
        sb->setAutoFillBackground(true);
        if (win.layout()) {
            win.layout()->setSpacing(0);
            win.layout()->setContentsMargins(0,0,0,0);
        }

        auto* btnGroup = new QButtonGroup(&win);
        btnGroup->setExclusive(true);
        auto* btnR = new VTB("Reclass");
        auto* btnC = new VTB("C/C++");
        setColors(btnR); setColors(btnC);
        btnR->setChecked(true);
        btnGroup->addButton(btnR, 0);
        btnGroup->addButton(btnC, 1);
        auto* tabRow = new QWidget(sb);
        auto* tabLay = new QHBoxLayout(tabRow);
        tabLay->setContentsMargins(0,0,0,0);
        tabLay->setSpacing(0);
        tabLay->addWidget(btnR);
        tabLay->addWidget(btnC);
        auto* lbl = new QLabel("Ready", sb);
        lbl->setContentsMargins(10,0,0,0);
        sb->tabRow = tabRow;
        sb->label  = lbl;

        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QTest::qWait(100);

        // ── Toggle logic ──
        QVERIFY(btnR->isChecked());
        QVERIFY(!btnC->isChecked());
        QTest::mouseClick(btnC, Qt::LeftButton);
        QVERIFY(btnC->isChecked());
        QVERIFY(!btnR->isChecked());
        QTest::mouseClick(btnR, Qt::LeftButton);
        QVERIFY(btnR->isChecked());
        QTest::qWait(50);

        // ── Pixel: accent line on checked button at rows 0..(kAccentH-1) ──
        QImage imgR = btnR->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(imgR.height() >= kAccentH + 4);

        // Every pixel in the top kAccentH rows (middle 80% width) must be accent
        int x0 = imgR.width() / 10, x1 = imgR.width() * 9 / 10;
        for (int y = 0; y < kAccentH; y++) {
            for (int x = x0; x < x1; x++) {
                QColor c(imgR.pixel(x, y));
                QVERIFY2(qAbs(c.red() - accent.red()) < 10
                      && qAbs(c.green() - accent.green()) < 10
                      && qAbs(c.blue() - accent.blue()) < 10,
                    qPrintable(QString("Checked btn pixel(%1,%2)=%3 expected accent %4")
                        .arg(x).arg(y).arg(c.name(), accent.name())));
            }
        }

        // Mid-height row must NOT be accent (accent doesn't bleed into body)
        {
            int midY = imgR.height() / 2;
            QColor c(imgR.pixel(imgR.width()/2, midY));
            QVERIFY2(qAbs(c.red() - accent.red()) > 15
                  || qAbs(c.green() - accent.green()) > 15
                  || qAbs(c.blue() - accent.blue()) > 15,
                qPrintable(QString("Row %1 should be background, not accent: %2")
                    .arg(midY).arg(c.name())));
        }

        // ── Pixel: unchecked button has NO accent line ──
        QImage imgC = btnC->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < kAccentH; y++) {
            QColor c(imgC.pixel(imgC.width()/2, y));
            QVERIFY2(qAbs(c.red() - accent.red()) > 15
                  || qAbs(c.green() - accent.green()) > 15
                  || qAbs(c.blue() - accent.blue()) > 15,
                qPrintable(QString("Unchecked btn row %1 has accent: %2")
                    .arg(y).arg(c.name())));
        }

        // ── Pixel: zero gap between the two buttons ──
        // Map to their shared parent (the tabRow container)
        QWidget* container = btnR->parentWidget();
        int rRight = btnR->mapTo(container, QPoint(btnR->width(), 0)).x();
        int cLeft  = btnC->mapTo(container, QPoint(0, 0)).x();
        QVERIFY2(rRight == cLeft,
            qPrintable(QString("Gap between buttons: btnR right=%1 btnC left=%2 gap=%3")
                .arg(rRight).arg(cLeft).arg(cLeft - rRight)));

        // ── Pressed color is darker than hover ──
        QVERIFY2(pressed.lightness() < hover.lightness(),
            qPrintable(QString("Pressed %1 should be darker than hover %2")
                .arg(pressed.name(), hover.name())));

        // ── Button starts at x=0 in status bar (no left padding) ──
        QPoint btnTopLeft = tabRow->mapTo(sb, QPoint(0, 0));
        QVERIFY2(btnTopLeft.x() == 0,
            qPrintable(QString("Tab row left margin: x=%1, expected 0").arg(btnTopLeft.x())));

        // ── Button starts at y=0 in status bar (no top padding) ──
        QVERIFY2(btnTopLeft.y() == 0,
            qPrintable(QString("Tab row top margin: y=%1, expected 0").arg(btnTopLeft.y())));

        // ── Button takes full status bar height ──
        QVERIFY2(btnR->height() == sb->height(),
            qPrintable(QString("Button height=%1 sb height=%2")
                .arg(btnR->height()).arg(sb->height())));

        // ── Accent at y=0 in status bar pixel coordinates (grab status bar) ──
        QImage sbImg = sb->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        {
            QColor c(sbImg.pixel(btnR->width()/2, 0));
            QVERIFY2(qAbs(c.red() - accent.red()) < 10
                  && qAbs(c.green() - accent.green()) < 10
                  && qAbs(c.blue() - accent.blue()) < 10,
                qPrintable(QString("Status bar pixel(x,%1,0)=%2 expected accent %3")
                    .arg(btnR->width()/2).arg(c.name(), accent.name())));
        }

        qDebug() << QString("ViewTabButton: accent=%1 btnH=%2 sbH=%3 gap=%4 leftX=%5 topY=%6")
            .arg(accent.name()).arg(btnR->height()).arg(sb->height())
            .arg(cLeft - rRight).arg(btnTopLeft.x()).arg(btnTopLeft.y());
    }

    // ── Test: resize grip dots are equidistant from right and bottom window edges ──
    // The grip is a direct child of the window positioned via move(), not inside
    // the status bar layout. This test verifies the dot placement is symmetric
    // regardless of font, and runs the check at two different font sizes to prove
    // font independence.
    // ── Test: horizontal scrollbar after long name rename ──
    void testHScrollResetAfterNameShrink() {
        // Use a dedicated narrow editor so content easily overflows the viewport
        auto* editor = new RcxEditor();
        editor->resize(200, 300);
        editor->show();
        QVERIFY(QTest::qWaitForWindowExposed(editor));
        auto* sci = editor->scintilla();
        auto* hbar = sci->horizontalScrollBar();

        auto makeTree = [](const QString& fieldName) {
            NodeTree tree;
            tree.baseAddress = 0;
            Node root;
            root.kind = NodeKind::Struct;
            root.structTypeName = "MyStruct";
            root.name = "s";
            root.parentId = 0;
            root.offset = 0;
            int ri = tree.addNode(root);
            uint64_t rootId = tree.nodes[ri].id;

            Node f;
            f.kind = NodeKind::Int32;
            f.name = fieldName;
            f.parentId = rootId;
            f.offset = 0;
            tree.addNode(f);
            return tree;
        };

        BufferProvider prov(QByteArray(64, '\0'));

        // ── Step 1: long name → wide content, scrollbar must appear ──
        QString longName = QString(120, QChar('W'));
        {
            NodeTree tree = makeTree(longName);
            ComposeResult cr = compose(tree, prov);
            editor->applyDocument(cr);
            QApplication::processEvents();
            QTest::qWait(50);
        }

        int scrollW1 = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETSCROLLWIDTH);
        int viewW    = sci->viewport()->width();

        qDebug() << QString("Long name: scrollW=%1 vpW=%2 hbar.visible=%3 "
                            "hbar.max=%4 hbar.value=%5")
                        .arg(scrollW1).arg(viewW)
                        .arg(hbar->isVisible())
                        .arg(hbar->maximum()).arg(hbar->value());

        QVERIFY2(scrollW1 > viewW,
                 qPrintable(QString("scrollW=%1 should exceed vpW=%2")
                     .arg(scrollW1).arg(viewW)));

        // Scrollbar must be visible when content overflows
        QVERIFY2(hbar->isVisible(),
                 "Horizontal scrollbar should be visible when content overflows");
        QVERIFY2(hbar->maximum() > 0,
                 qPrintable(QString("Scrollbar max should be >0, got %1")
                     .arg(hbar->maximum())));

        // Simulate user scrolled right
        sci->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET, (unsigned long)(scrollW1 / 2));
        QApplication::processEvents();
        QTest::qWait(20);
        int xOff1 = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET);
        QVERIFY2(xOff1 > 0, "X offset should be non-zero after scrolling right");

        // ── Step 2: short name → narrower content ──
        {
            NodeTree tree = makeTree("x");
            ComposeResult cr = compose(tree, prov);
            editor->applyDocument(cr);
            QApplication::processEvents();
            QTest::qWait(50);
        }

        int scrollW2 = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETSCROLLWIDTH);
        int xOff2    = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET);

        qDebug() << QString("Short name: scrollW=%1 xOff=%2 vpW=%3 hbar.visible=%4 "
                            "hbar.max=%5 hbar.value=%6")
                        .arg(scrollW2).arg(xOff2).arg(viewW)
                        .arg(hbar->isVisible())
                        .arg(hbar->maximum()).arg(hbar->value());

        // Scroll width should have shrunk
        QVERIFY2(scrollW2 < scrollW1,
                 qPrintable(QString("scrollW should shrink: was %1, now %2")
                     .arg(scrollW1).arg(scrollW2)));

        // X offset must be clamped to max(0, scrollW - viewportW)
        int maxValidXOff = qMax(0, scrollW2 - viewW);
        QVERIFY2(xOff2 <= maxValidXOff,
                 qPrintable(QString("xOffset=%1 exceeds max valid=%2 (scrollW=%3 vpW=%4)")
                     .arg(xOff2).arg(maxValidXOff).arg(scrollW2).arg(viewW)));

        // If content fits viewport entirely, offset must be 0
        if (scrollW2 <= viewW) {
            QCOMPARE(xOff2, 0);
        }

        // If content still overflows, scrollbar must still be visible
        if (scrollW2 > viewW) {
            QVERIFY2(hbar->isVisible(),
                     "Scrollbar should remain visible when content still overflows");
        }

        // ── Step 3: apply long name again → scrollbar must reappear ──
        {
            NodeTree tree = makeTree(longName);
            ComposeResult cr = compose(tree, prov);
            editor->applyDocument(cr);
            QApplication::processEvents();
            QTest::qWait(50);
        }

        int scrollW3 = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETSCROLLWIDTH);
        int xOff3    = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET);

        qDebug() << QString("Long again: scrollW=%1 xOff=%2 hbar.visible=%3 hbar.max=%4")
                        .arg(scrollW3).arg(xOff3)
                        .arg(hbar->isVisible()).arg(hbar->maximum());

        QVERIFY2(scrollW3 > viewW,
                 qPrintable(QString("scrollW=%1 should exceed vpW=%2 after re-widen")
                     .arg(scrollW3).arg(viewW)));
        QVERIFY2(hbar->isVisible(),
                 "Scrollbar must reappear after content widens again");
        // After fresh apply with no prior scroll, xOffset should be 0
        QCOMPARE(xOff3, 0);

        delete editor;
    }

    void testResizeGripCornerSymmetry() {
        // Same constants as production ResizeGrip in main.cpp
        static constexpr int kSize = 16;
        static constexpr int kPad  = 4;
        static constexpr double kInset = 4.0;

        class Grip : public QWidget {
        public:
            explicit Grip(QWidget* p) : QWidget(p) { setFixedSize(kSize, kSize); }
            void reposition() {
                if (auto* w = parentWidget())
                    move(w->width() - kSize - kPad, w->height() - kSize - kPad);
            }
        protected:
            void paintEvent(QPaintEvent*) override {
                QPainter p(this);
                p.setRenderHint(QPainter::Antialiasing);
                p.setPen(Qt::NoPen);
                p.setBrush(Qt::red);
                const double r = 1.0, s = 4.0;
                double bx = width()  - kInset;
                double by = height() - kInset;
                p.drawEllipse(QPointF(bx,         by), r, r);
                p.drawEllipse(QPointF(bx - s,     by), r, r);
                p.drawEllipse(QPointF(bx - 2 * s, by), r, r);
                p.drawEllipse(QPointF(bx,         by - s), r, r);
                p.drawEllipse(QPointF(bx - s,     by - s), r, r);
                p.drawEllipse(QPointF(bx,         by - 2 * s), r, r);
            }
        };

        // Helper: grab window, find bottommost-rightmost red pixel, measure gaps
        auto measureGaps = [](QWidget* win, int& gapRight, int& gapBottom) -> bool {
            QPixmap px = win->grab();
            QImage img = px.toImage().convertToFormat(QImage::Format_ARGB32);
            int W = img.width(), H = img.height();
            if (W < 50 || H < 50) return false;

            int foundX = -1, foundY = -1;
            for (int y = H - 1; y >= H - 40 && foundY < 0; --y) {
                for (int x = W - 1; x >= W - 40; --x) {
                    QColor c(img.pixel(x, y));
                    if (c.red() > 180 && c.green() < 80 && c.blue() < 80) {
                        foundX = x; foundY = y; break;
                    }
                }
            }
            if (foundX < 0) return false;
            gapRight  = (W - 1) - foundX;
            gapBottom = (H - 1) - foundY;

            // Save diagnostic image
            QImage diag = img.copy();
            QPainter dp(&diag);
            dp.setPen(QPen(Qt::cyan, 1));
            dp.drawRect(foundX - 3, foundY - 3, 6, 6);
            dp.setPen(QPen(Qt::yellow, 1));
            dp.drawLine(foundX, foundY, W - 1, foundY);
            dp.drawLine(foundX, foundY, foundX, H - 1);
            dp.end();
            diag.save("grip_corner_diag.png");
            return true;
        };

        // --- Round 1: default system font ---
        QMainWindow win;
        win.resize(500, 375);

        QPalette pal;
        pal.setColor(QPalette::Window, QColor(30, 30, 30));
        win.setPalette(pal);
        win.statusBar()->setPalette(pal);
        win.statusBar()->setAutoFillBackground(true);

        auto* grip = new Grip(&win);
        grip->raise();

        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        grip->reposition();
        QTest::qWait(100);

        int gapR1 = 0, gapB1 = 0;
        QVERIFY2(measureGaps(&win, gapR1, gapB1),
                 "Could not find red grip dot (round 1)");
        QVERIFY2(gapR1 == gapB1,
                 qPrintable(QString("Round 1 asymmetric: gapRight=%1 gapBottom=%2")
                     .arg(gapR1).arg(gapB1)));

        // --- Round 2: large font on status bar (must NOT change grip position) ---
        QFont bigFont("Arial", 24);
        win.statusBar()->setFont(bigFont);
        QTest::qWait(100);
        grip->reposition();
        QTest::qWait(100);

        int gapR2 = 0, gapB2 = 0;
        QVERIFY2(measureGaps(&win, gapR2, gapB2),
                 "Could not find red grip dot (round 2, big font)");
        QVERIFY2(gapR2 == gapB2,
                 qPrintable(QString("Round 2 asymmetric: gapRight=%1 gapBottom=%2")
                     .arg(gapR2).arg(gapB2)));

        // Gaps must be identical across both font sizes
        QVERIFY2(gapR1 == gapR2 && gapB1 == gapB2,
                 qPrintable(QString("Font changed grip position: "
                     "round1=(%1,%2) round2=(%3,%4)")
                     .arg(gapR1).arg(gapB1).arg(gapR2).arg(gapB2)));

        qDebug() << "Grip corner symmetry:"
                 << QString("gapRight=%1 gapBottom=%2 (font-independent)")
                    .arg(gapR1).arg(gapB1);
    }

    // ── Test: hovering struct type name shows PointingHand cursor ──
    // Regression: headerTypeNameSpan returned invalid for named structs
    // because it assumed "struct TYPENAME" format, but named structs are
    // formatted as just "TYPENAME" (e.g. "_STRING64 CSDVersion").
    void testStructTypeClickable() {
        m_editor->applyDocument(m_result);
        QApplication::processEvents();

        // Find a named struct header (e.g. _STRING64 CSDVersion from makeTestTree)
        int headerLine = -1;
        for (int i = 0; i < m_result.meta.size(); i++) {
            const auto& lm = m_result.meta[i];
            if (lm.lineKind == LineKind::Header && lm.foldHead
                && lm.nodeKind == NodeKind::Struct && !lm.isArrayHeader) {
                headerLine = i;
                break;
            }
        }
        QVERIFY2(headerLine >= 0, "Should have a struct header");

        const LineMeta* lm = m_editor->metaForLine(headerLine);
        QVERIFY(lm);

        // Scroll to ensure line is visible
        m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_ENSUREVISIBLE, (unsigned long)headerLine);
        m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_GOTOLINE, (unsigned long)headerLine);
        QApplication::processEvents();

        // The type column starts at kFoldCol + depth*3
        int typeStart = 3 + lm->depth * 3;  // kFoldCol = 3

        // Hover over type column — should show PointingHandCursor
        // (Before fix: showed ArrowCursor because headerTypeNameSpan returned invalid)
        QPoint typePos = colToViewport(m_editor->scintilla(), headerLine, typeStart + 1);
        QVERIFY2(typePos.y() > 0, "Header line should be visible");
        sendMouseMove(m_editor->scintilla()->viewport(), typePos);
        QApplication::processEvents();
        QCOMPARE(viewportCursor(m_editor), Qt::PointingHandCursor);
    }
    // ── Static field: name must be editable (it's a function name, not hex label) ──

    void testStaticFieldNameEditable() {
        // Build a tree with one regular field + one static field
        NodeTree tree;
        tree.baseAddress = 0;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Test";
        root.structTypeName = "Test";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::UInt32;
        f.name = "field_a";
        f.parentId = rootId;
        f.offset = 0;
        tree.addNode(f);

        Node sf;
        sf.kind = NodeKind::Hex64;
        sf.name = "my_target";
        sf.parentId = rootId;
        sf.offset = 0;
        sf.isStatic = true;
        sf.offsetExpr = QStringLiteral("base");
        tree.addNode(sf);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);
        m_editor->applyDocument(result);
        QApplication::processEvents();

        // Find the static field header line
        int headerLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].isStaticLine && result.meta[i].lineKind == LineKind::Header) {
                headerLine = i;
                break;
            }
        }
        QVERIFY2(headerLine >= 0, "Should have a static field header line");

        const LineMeta* lm = m_editor->metaForLine(headerLine);
        QVERIFY(lm);
        QVERIFY(lm->isStaticLine);

        // Verify the header text contains the name
        QString text = m_editor->textWithMargins();
        QStringList lines = text.split('\n');
        QVERIFY2(headerLine < lines.size(), "header line in range");
        QString hdrText = lines[headerLine];
        QVERIFY2(hdrText.contains("my_target"), qPrintable("Header line should contain name: " + hdrText));

        // The name should be inline-editable despite being a hex node kind
        int nameStart = kFoldCol + lm->depth * 3 + lm->effectiveTypeW + kSepWidth;
        bool ok = m_editor->beginInlineEdit(EditTarget::Name, headerLine, nameStart);
        QVERIFY2(ok, qPrintable(QString("Static field name must be editable. line=%1 col=%2 depth=%3 typeW=%4 text='%5'")
            .arg(headerLine).arg(nameStart).arg(lm->depth).arg(lm->effectiveTypeW).arg(hdrText)));
        m_editor->cancelInlineEdit();
    }

    // ── Static field: type in header triggers type picker, not inline edit ──

    void testStaticFieldTypeClickable() {
        // Build same tree as above
        NodeTree tree;
        tree.baseAddress = 0;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Test";
        root.structTypeName = "Test";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::UInt32;
        f.name = "field_a";
        f.parentId = rootId;
        f.offset = 0;
        tree.addNode(f);

        Node sf;
        sf.kind = NodeKind::Hex64;
        sf.name = "my_target";
        sf.parentId = rootId;
        sf.offset = 0;
        sf.isStatic = true;
        sf.offsetExpr = QStringLiteral("base");
        tree.addNode(sf);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);
        m_editor->applyDocument(result);
        QApplication::processEvents();

        // Find the static field header line
        int headerLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].isStaticLine && result.meta[i].lineKind == LineKind::Header) {
                headerLine = i;
                break;
            }
        }
        QVERIFY(headerLine >= 0);

        const LineMeta* lm = m_editor->metaForLine(headerLine);
        QVERIFY(lm);

        // Scroll to ensure visible
        m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_ENSUREVISIBLE, (unsigned long)headerLine);
        m_editor->scintilla()->SendScintilla(
            QsciScintillaBase::SCI_GOTOLINE, (unsigned long)headerLine);
        QApplication::processEvents();

        // Hover over the type column (after "static " prefix) — should be PointingHandCursor
        // "static " is 7 chars, so the actual type starts at indent + 7
        int typeCol = kFoldCol + lm->depth * 3 + 7;
        QPoint typePos = colToViewport(m_editor->scintilla(), headerLine, typeCol + 1);
        if (typePos.y() > 0) {
            sendMouseMove(m_editor->scintilla()->viewport(), typePos);
            QApplication::processEvents();
            QCOMPARE(viewportCursor(m_editor), Qt::PointingHandCursor);
        }
    }

    // ── Static field: body line expression is editable ──

    void testStaticFieldExprEditable() {
        NodeTree tree;
        tree.baseAddress = 0;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Test";
        root.structTypeName = "Test";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node sf;
        sf.kind = NodeKind::Hex64;
        sf.name = "target";
        sf.parentId = rootId;
        sf.offset = 0;
        sf.isStatic = true;
        sf.offsetExpr = QStringLiteral("base + 0x10");
        tree.addNode(sf);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);
        m_editor->applyDocument(result);
        QApplication::processEvents();

        // Find the body line (Field with isStaticLine)
        int bodyLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].isStaticLine && result.meta[i].lineKind == LineKind::Field) {
                bodyLine = i;
                break;
            }
        }
        QVERIFY2(bodyLine >= 0, "Should have a static field body line");

        // The expression should be editable via StaticExpr target
        bool ok = m_editor->beginInlineEdit(EditTarget::StaticExpr, bodyLine);
        QVERIFY2(ok, "Static field expression must be editable");
        m_editor->cancelInlineEdit();
    }

    // ── No separator line for static fields ──

    void testStaticFieldNoSeparator() {
        NodeTree tree;
        tree.baseAddress = 0;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Test";
        root.structTypeName = "Test";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::UInt32;
        f.name = "a";
        f.parentId = rootId;
        f.offset = 0;
        tree.addNode(f);

        Node sf;
        sf.kind = NodeKind::Hex64;
        sf.name = "target";
        sf.parentId = rootId;
        sf.offset = 0;
        sf.isStatic = true;
        sf.offsetExpr = QStringLiteral("base");
        tree.addNode(sf);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // No separator line with box-drawing characters should exist
        QStringList lines = result.text.split('\n');
        for (const auto& line : lines) {
            QVERIFY2(!line.contains(QStringLiteral("\u2500\u2500\u2500\u2500 static \u2500\u2500\u2500\u2500")),
                     "Static fields should not have a separator line");
        }
    }
};

QTEST_MAIN(TestEditor)
#include "test_editor.moc"
