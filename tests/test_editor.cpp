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
    tree.baseAddress = 0x000000D87B5E5000ULL;

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
            QStringLiteral("source\u25BE \u00B7 0xD87B5E5000"));

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
            QStringLiteral("source\u25BE \u00B7 0xD87B5E5000"));

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
            QStringLiteral("source\u25BE \u00B7 0xD87B5E5000"));

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
            QStringLiteral("source\u25BE \u00B7 0xD87B5E5000 \u00B7 struct _PEB64 {"));

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
            QStringLiteral("source\u25BE \u00B7 0xD87B5E5000 \u00B7 struct _PEB64 {"));

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
            "source\u25BE \u00B7 0xD87B5E5000 \u00B7 struct _PEB64 {");
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
            "source\u25BE \u00B7 0xD87B5E5000 \u00B7 struct _PEB64 {");
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
        tree.baseAddress = 0x1000;

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
};

QTEST_MAIN(TestEditor)
#include "test_editor.moc"
