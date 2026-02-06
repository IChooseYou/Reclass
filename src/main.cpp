#include "controller.h"
#include <QApplication>
#include <QMainWindow>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QSplitter>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QAction>
#include <QActionGroup>
#include <QMap>
#include <QTimer>
#include <QDir>
#include <QMetaObject>
#include <QFontDatabase>
#include <QPainter>
#include <QSvgRenderer>
#include <QSettings>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>

static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    fprintf(stderr, "\n=== UNHANDLED EXCEPTION ===\n");
    fprintf(stderr, "Code : 0x%08lX\n", ep->ExceptionRecord->ExceptionCode);
    fprintf(stderr, "Addr : %p\n", ep->ExceptionRecord->ExceptionAddress);

    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, NULL, TRUE);

    CONTEXT* ctx = ep->ContextRecord;
    STACKFRAME64 frame = {};
    DWORD machineType;
#ifdef _M_X64
    machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx->Rip;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrStack.Offset = ctx->Rsp;
#else
    machineType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = ctx->Eip;
    frame.AddrFrame.Offset = ctx->Ebp;
    frame.AddrStack.Offset = ctx->Esp;
#endif
    frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    fprintf(stderr, "\nStack trace:\n");
    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(machineType, process, thread, &frame, ctx,
                         NULL, SymFunctionTableAccess64,
                         SymGetModuleBase64, NULL))
            break;
        if (frame.AddrPC.Offset == 0) break;

        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;

        DWORD64 disp64 = 0;
        DWORD   disp32 = 0;
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);

        bool hasSym  = SymFromAddr(process, frame.AddrPC.Offset, &disp64, sym);
        bool hasLine = SymGetLineFromAddr64(process, frame.AddrPC.Offset,
                                            &disp32, &line);
        if (hasSym && hasLine) {
            fprintf(stderr, "  [%2d] %s+0x%llx  (%s:%lu)\n",
                    i, sym->Name, (unsigned long long)disp64,
                    line.FileName, line.LineNumber);
        } else if (hasSym) {
            fprintf(stderr, "  [%2d] %s+0x%llx\n",
                    i, sym->Name, (unsigned long long)disp64);
        } else {
            fprintf(stderr, "  [%2d] 0x%llx\n",
                    i, (unsigned long long)frame.AddrPC.Offset);
        }
    }

    SymCleanup(process);
    fprintf(stderr, "=== END CRASH ===\n");
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

namespace rcx {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void newFile();
    void openFile();
    void saveFile();
    void saveFileAs();
    void loadBinary();

    void addNode();
    void removeNode();
    void changeNodeType();
    void renameNodeAction();
    void duplicateNodeAction();
    void splitView();
    void unsplitView();

    void undo();
    void redo();
    void about();
    void setEditorFont(const QString& fontName);

private:
    QMdiArea* m_mdiArea;
    QLabel*   m_statusLabel;

    struct TabState {
        RcxDocument*   doc;
        RcxController* ctrl;
        QSplitter*     splitter;
    };
    QMap<QMdiSubWindow*, TabState> m_tabs;

    void createMenus();
    void createStatusBar();
    QIcon makeIcon(const QString& svgPath);

    RcxController* activeController() const;
    TabState* activeTab();
    QMdiSubWindow* createTab(RcxDocument* doc);
    void updateWindowTitle();
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("ReclassX");
    resize(1200, 800);

    m_mdiArea = new QMdiArea(this);
    m_mdiArea->setViewMode(QMdiArea::TabbedView);
    m_mdiArea->setTabsClosable(true);
    m_mdiArea->setTabsMovable(true);
    setCentralWidget(m_mdiArea);

    createMenus();
    createStatusBar();

    connect(m_mdiArea, &QMdiArea::subWindowActivated,
            this, [this](QMdiSubWindow*) { updateWindowTitle(); });
}

QIcon MainWindow::makeIcon(const QString& svgPath) {
    // Render SVG at 14x14 (2px smaller)
    QSvgRenderer renderer(svgPath);
    QPixmap svgPixmap(14, 14);
    svgPixmap.fill(Qt::transparent);
    QPainter svgPainter(&svgPixmap);
    renderer.render(&svgPainter);
    svgPainter.end();
    
    // Center it in a 16x16 canvas
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.drawPixmap(1, 1, svgPixmap);  // Offset by 1px on each side
    painter.end();
    
    return QIcon(pixmap);
}

void MainWindow::createMenus() {
    // File
    auto* file = menuBar()->addMenu("&File");
    file->addAction(makeIcon(":/vsicons/file.svg"), "&New", QKeySequence::New, this, &MainWindow::newFile);
    file->addAction(makeIcon(":/vsicons/folder-opened.svg"), "&Open...", QKeySequence::Open, this, &MainWindow::openFile);
    file->addSeparator();
    file->addAction(makeIcon(":/vsicons/save.svg"), "&Save", QKeySequence::Save, this, &MainWindow::saveFile);
    file->addAction(makeIcon(":/vsicons/save-as.svg"), "Save &As...", QKeySequence::SaveAs, this, &MainWindow::saveFileAs);
    file->addSeparator();
    file->addAction(makeIcon(":/vsicons/file-binary.svg"), "Load &Binary...", this, &MainWindow::loadBinary);
    file->addSeparator();
    file->addAction(makeIcon(":/vsicons/close.svg"), "E&xit", QKeySequence(Qt::Key_Close), this, &QMainWindow::close);

    // Edit
    auto* edit = menuBar()->addMenu("&Edit");
    edit->addAction(makeIcon(":/vsicons/arrow-left.svg"), "&Undo", QKeySequence::Undo, this, &MainWindow::undo);
    edit->addAction(makeIcon(":/vsicons/arrow-right.svg"), "&Redo", QKeySequence::Redo, this, &MainWindow::redo);

    // View
    auto* view = menuBar()->addMenu("&View");
    view->addAction(makeIcon(":/vsicons/split-horizontal.svg"), "Split &Horizontal", this, &MainWindow::splitView);
    view->addAction(makeIcon(":/vsicons/chrome-close.svg"), "&Unsplit", this, &MainWindow::unsplitView);
    view->addSeparator();
    auto* fontMenu = view->addMenu(makeIcon(":/vsicons/text-size.svg"), "&Font");
    auto* fontGroup = new QActionGroup(this);
    fontGroup->setExclusive(true);
    auto* actConsolas = fontMenu->addAction("Consolas");
    actConsolas->setCheckable(true);
    actConsolas->setActionGroup(fontGroup);
    auto* actIosevka = fontMenu->addAction("Iosevka");
    actIosevka->setCheckable(true);
    actIosevka->setActionGroup(fontGroup);
    // Load saved preference
    QSettings settings("ReclassX", "ReclassX");
    QString savedFont = settings.value("font", "Consolas").toString();
    if (savedFont == "Iosevka") actIosevka->setChecked(true);
    else actConsolas->setChecked(true);
    connect(actConsolas, &QAction::triggered, this, [this]() { setEditorFont("Consolas"); });
    connect(actIosevka, &QAction::triggered, this, [this]() { setEditorFont("Iosevka"); });

    // Node
    auto* node = menuBar()->addMenu("&Node");
    node->addAction(makeIcon(":/vsicons/add.svg"), "&Add Field", QKeySequence(Qt::Key_Insert), this, &MainWindow::addNode);
    node->addAction(makeIcon(":/vsicons/remove.svg"), "&Remove Field", QKeySequence::Delete, this, &MainWindow::removeNode);
    node->addAction(makeIcon(":/vsicons/symbol-structure.svg"), "Change &Type", QKeySequence(Qt::Key_T), this, &MainWindow::changeNodeType);
    node->addAction(makeIcon(":/vsicons/edit.svg"), "Re&name", QKeySequence(Qt::Key_F2), this, &MainWindow::renameNodeAction);
    node->addAction(makeIcon(":/vsicons/files.svg"), "D&uplicate", this, &MainWindow::duplicateNodeAction)->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));

    // Help
    auto* help = menuBar()->addMenu("&Help");
    help->addAction(makeIcon(":/vsicons/question.svg"), "&About ReclassX", this, &MainWindow::about);
}

void MainWindow::createStatusBar() {
    m_statusLabel = new QLabel("Ready");
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->setStyleSheet("QStatusBar { background: #252526; color: #858585; }");
}

QMdiSubWindow* MainWindow::createTab(RcxDocument* doc) {
    auto* splitter = new QSplitter(Qt::Horizontal);
    auto* ctrl = new RcxController(doc, splitter);
    ctrl->addSplitEditor(splitter);

    auto* sub = m_mdiArea->addSubWindow(splitter);
    sub->setWindowTitle(doc->filePath.isEmpty()
                        ? "Untitled" : QFileInfo(doc->filePath).fileName());
    sub->setAttribute(Qt::WA_DeleteOnClose);
    sub->showMaximized();

    m_tabs[sub] = { doc, ctrl, splitter };

    connect(sub, &QObject::destroyed, this, [this, sub]() {
        auto it = m_tabs.find(sub);
        if (it != m_tabs.end()) {
            it->doc->deleteLater();
            m_tabs.erase(it);
        }
    });

    connect(ctrl, &RcxController::nodeSelected,
            this, [this, ctrl](int nodeIdx) {
        if (nodeIdx >= 0 && nodeIdx < ctrl->document()->tree.nodes.size()) {
            auto& node = ctrl->document()->tree.nodes[nodeIdx];
            m_statusLabel->setText(
                QString("%1 %2  offset: 0x%3  size: %4 bytes")
                    .arg(kindToString(node.kind))
                    .arg(node.name)
                    .arg(node.offset, 4, 16, QChar('0'))
                    .arg(node.byteSize()));
        } else {
            m_statusLabel->setText("Ready");
        }
    });
    connect(ctrl, &RcxController::selectionChanged,
            this, [this](int count) {
        if (count == 0)
            m_statusLabel->setText("Ready");
        else if (count > 1)
            m_statusLabel->setText(QString("%1 nodes selected").arg(count));
    });

    ctrl->refresh();
    return sub;
}

void MainWindow::newFile() {
    auto* doc = new RcxDocument(this);

    // ══════════════════════════════════════════════════════════════════════════
    // _PEB64 Demo — Process Environment Block (0x7D0 bytes)
    // ══════════════════════════════════════════════════════════════════════════

    QByteArray pebData(0x7D0, '\0');
    char* d = pebData.data();

    auto w8  = [&](int off, uint8_t  v) { d[off] = (char)v; };
    auto w16 = [&](int off, uint16_t v) { memcpy(d+off, &v, 2); };
    auto w32 = [&](int off, uint32_t v) { memcpy(d+off, &v, 4); };
    auto w64 = [&](int off, uint64_t v) { memcpy(d+off, &v, 8); };

    w8 (0x002, 1);                              // BeingDebugged
    w8 (0x003, 0x04);                           // BitField
    w64(0x008, 0xFFFFFFFFFFFFFFFFULL);          // Mutant (-1)
    w64(0x010, 0x00007FF6DE120000ULL);          // ImageBaseAddress
    w64(0x018, 0x00007FFE3B8B53C0ULL);          // Ldr
    w64(0x020, 0x000001A4C3E20F90ULL);          // ProcessParameters
    w64(0x030, 0x000001A4C3D40000ULL);          // ProcessHeap
    w64(0x038, 0x00007FFE3B8D4260ULL);          // FastPebLock
    w32(0x050, 0x01);                           // CrossProcessFlags
    w64(0x058, 0x00007FFE3B720000ULL);          // KernelCallbackTable
    w64(0x068, 0x00007FFE3E570000ULL);          // ApiSetMap
    w64(0x078, 0x00007FFE3B8D3F50ULL);          // TlsBitmap
    w32(0x080, 0x00000003);                     // TlsBitmapBits[0]
    w64(0x088, 0x00007FFE38800000ULL);          // ReadOnlySharedMemoryBase
    w64(0x090, 0x00007FFE38820000ULL);          // SharedData
    w64(0x0A0, 0x00007FFE3B8D1000ULL);          // AnsiCodePageData
    w64(0x0A8, 0x00007FFE3B8D2040ULL);          // OemCodePageData
    w64(0x0B0, 0x00007FFE3B8CE020ULL);          // UnicodeCaseTableData
    w32(0x0B8, 8);                              // NumberOfProcessors
    w32(0x0BC, 0x70);                           // NtGlobalFlag
    w64(0x0C0, 0xFFFFFFFF7C91E000ULL);          // CriticalSectionTimeout
    w64(0x0C8, 0x0000000000100000ULL);          // HeapSegmentReserve
    w64(0x0D0, 0x0000000000002000ULL);          // HeapSegmentCommit
    w32(0x0E8, 4);                              // NumberOfHeaps
    w32(0x0EC, 16);                             // MaximumNumberOfHeaps
    w64(0x0F0, 0x000001A4C3D40688ULL);          // ProcessHeaps
    w64(0x0F8, 0x00007FFE388B0000ULL);          // GdiSharedHandleTable
    w64(0x110, 0x00007FFE3B8D42E8ULL);          // LoaderLock
    w32(0x118, 10);                             // OSMajorVersion
    w16(0x120, 19045);                          // OSBuildNumber
    w32(0x124, 2);                              // OSPlatformId
    w32(0x128, 3);                              // ImageSubsystem (CUI)
    w32(0x12C, 10);                             // ImageSubsystemMajorVersion
    w64(0x138, 0x00000000000000FFULL);          // ActiveProcessAffinityMask
    w64(0x238, 0x00007FFE3B8D3F70ULL);          // TlsExpansionBitmap
    w32(0x2C0, 1);                              // SessionId
    w64(0x2F8, 0x000001A4C3E21000ULL);          // ActivationContextData
    w64(0x308, 0x00007FFE38840000ULL);          // SystemDefaultActivationContextData
    w64(0x318, 0x0000000000002000ULL);          // MinimumStackCommit
    w16(0x34C, 1252);                           // ActiveCodePage
    w16(0x34E, 437);                            // OemCodePage
    w64(0x358, 0x000001A4C3E30000ULL);          // WerRegistrationData
    w64(0x380, 0x00007FFE38890000ULL);          // CsrServerReadOnlySharedMemoryBase
    w64(0x390, 0x000000D87B5E5390ULL);          // TppWorkerpList.Flink (self)
    w64(0x398, 0x000000D87B5E5390ULL);          // TppWorkerpList.Blink (self)
    w64(0x7B8, 0x00007FFE38860000ULL);          // LeapSecondData

    doc->loadData(pebData);
    doc->tree.baseAddress = 0x000000D87B5E5000ULL;

    // ══════════════════════════════════════════════════════════════════════════
    // Build _PEB64 Node Tree (0x7D0 bytes, unions mapped to first member)
    // ══════════════════════════════════════════════════════════════════════════

    auto addField = [&](uint64_t parent, int offset, NodeKind kind, const QString& name) -> uint64_t {
        Node n; n.kind = kind; n.name = name;
        n.parentId = parent; n.offset = offset;
        int idx = doc->tree.addNode(n);
        return doc->tree.nodes[idx].id;
    };
    auto addPad = [&](uint64_t parent, int offset, int len, const QString& name) {
        Node n; n.kind = NodeKind::Padding; n.name = name;
        n.parentId = parent; n.offset = offset; n.arrayLen = len;
        doc->tree.addNode(n);
    };
    auto addStruct = [&](uint64_t parent, int offset, const QString& typeName, const QString& name, bool collapse = true) -> uint64_t {
        Node n; n.kind = NodeKind::Struct;
        n.structTypeName = typeName; n.name = name;
        n.parentId = parent; n.offset = offset; n.collapsed = collapse;
        int idx = doc->tree.addNode(n);
        return doc->tree.nodes[idx].id;
    };
    auto addArray = [&](uint64_t parent, int offset, const QString& name, int count, NodeKind elemKind) {
        Node n; n.kind = NodeKind::Array; n.name = name;
        n.parentId = parent; n.offset = offset;
        n.arrayLen = count; n.elementKind = elemKind;
        n.collapsed = true;
        doc->tree.addNode(n);
    };

    // Root struct (not collapsed so fields are visible on open)
    uint64_t peb = addStruct(0, 0, "_PEB64", "Peb", false);

    // 0x000 – 0x007
    addField(peb, 0x000, NodeKind::UInt8,     "InheritedAddressSpace");
    addField(peb, 0x001, NodeKind::UInt8,     "ReadImageFileExecOptions");
    addField(peb, 0x002, NodeKind::UInt8,     "BeingDebugged");
    addField(peb, 0x003, NodeKind::UInt8,     "BitField");
    addPad  (peb, 0x004, 4,                   "Padding0");

    // 0x008 – 0x04F
    addField(peb, 0x008, NodeKind::Pointer64, "Mutant");
    addField(peb, 0x010, NodeKind::Pointer64, "ImageBaseAddress");
    addField(peb, 0x018, NodeKind::Pointer64, "Ldr");
    addField(peb, 0x020, NodeKind::Pointer64, "ProcessParameters");
    addField(peb, 0x028, NodeKind::Pointer64, "SubSystemData");
    addField(peb, 0x030, NodeKind::Pointer64, "ProcessHeap");
    addField(peb, 0x038, NodeKind::Pointer64, "FastPebLock");
    addField(peb, 0x040, NodeKind::Pointer64, "AtlThunkSListPtr");
    addField(peb, 0x048, NodeKind::Pointer64, "IFEOKey");

    // 0x050 – 0x07F
    addField(peb, 0x050, NodeKind::UInt32,    "CrossProcessFlags");
    addPad  (peb, 0x054, 4,                   "Padding1");
    addField(peb, 0x058, NodeKind::Pointer64, "KernelCallbackTable");
    addField(peb, 0x060, NodeKind::UInt32,    "SystemReserved");
    addField(peb, 0x064, NodeKind::UInt32,    "AtlThunkSListPtr32");
    addField(peb, 0x068, NodeKind::Pointer64, "ApiSetMap");
    addField(peb, 0x070, NodeKind::UInt32,    "TlsExpansionCounter");
    addPad  (peb, 0x074, 4,                   "Padding2");
    addField(peb, 0x078, NodeKind::Pointer64, "TlsBitmap");
    addArray(peb, 0x080, "TlsBitmapBits", 2, NodeKind::UInt32);

    // 0x088 – 0x0BF
    addField(peb, 0x088, NodeKind::Pointer64, "ReadOnlySharedMemoryBase");
    addField(peb, 0x090, NodeKind::Pointer64, "SharedData");
    addField(peb, 0x098, NodeKind::Pointer64, "ReadOnlyStaticServerData");
    addField(peb, 0x0A0, NodeKind::Pointer64, "AnsiCodePageData");
    addField(peb, 0x0A8, NodeKind::Pointer64, "OemCodePageData");
    addField(peb, 0x0B0, NodeKind::Pointer64, "UnicodeCaseTableData");
    addField(peb, 0x0B8, NodeKind::UInt32,    "NumberOfProcessors");
    addField(peb, 0x0BC, NodeKind::Hex32,     "NtGlobalFlag");

    // 0x0C0 – 0x0EF
    addField(peb, 0x0C0, NodeKind::UInt64,    "CriticalSectionTimeout");
    addField(peb, 0x0C8, NodeKind::UInt64,    "HeapSegmentReserve");
    addField(peb, 0x0D0, NodeKind::UInt64,    "HeapSegmentCommit");
    addField(peb, 0x0D8, NodeKind::UInt64,    "HeapDeCommitTotalFreeThreshold");
    addField(peb, 0x0E0, NodeKind::UInt64,    "HeapDeCommitFreeBlockThreshold");
    addField(peb, 0x0E8, NodeKind::UInt32,    "NumberOfHeaps");
    addField(peb, 0x0EC, NodeKind::UInt32,    "MaximumNumberOfHeaps");

    // 0x0F0 – 0x13F
    addField(peb, 0x0F0, NodeKind::Pointer64, "ProcessHeaps");
    addField(peb, 0x0F8, NodeKind::Pointer64, "GdiSharedHandleTable");
    addField(peb, 0x100, NodeKind::Pointer64, "ProcessStarterHelper");
    addField(peb, 0x108, NodeKind::UInt32,    "GdiDCAttributeList");
    addPad  (peb, 0x10C, 4,                   "Padding3");
    addField(peb, 0x110, NodeKind::Pointer64, "LoaderLock");
    addField(peb, 0x118, NodeKind::UInt32,    "OSMajorVersion");
    addField(peb, 0x11C, NodeKind::UInt32,    "OSMinorVersion");
    addField(peb, 0x120, NodeKind::UInt16,    "OSBuildNumber");
    addField(peb, 0x122, NodeKind::UInt16,    "OSCSDVersion");
    addField(peb, 0x124, NodeKind::UInt32,    "OSPlatformId");
    addField(peb, 0x128, NodeKind::UInt32,    "ImageSubsystem");
    addField(peb, 0x12C, NodeKind::UInt32,    "ImageSubsystemMajorVersion");
    addField(peb, 0x130, NodeKind::UInt32,    "ImageSubsystemMinorVersion");
    addPad  (peb, 0x134, 4,                   "Padding4");
    addField(peb, 0x138, NodeKind::UInt64,    "ActiveProcessAffinityMask");

    // 0x140 – 0x22F
    addArray(peb, 0x140, "GdiHandleBuffer", 60, NodeKind::UInt32);

    // 0x230 – 0x2BF
    addField(peb, 0x230, NodeKind::Pointer64, "PostProcessInitRoutine");
    addField(peb, 0x238, NodeKind::Pointer64, "TlsExpansionBitmap");
    addArray(peb, 0x240, "TlsExpansionBitmapBits", 32, NodeKind::UInt32);

    // 0x2C0 – 0x2E7
    addField(peb, 0x2C0, NodeKind::UInt32,    "SessionId");
    addPad  (peb, 0x2C4, 4,                   "Padding5");
    addField(peb, 0x2C8, NodeKind::UInt64,    "AppCompatFlags");
    addField(peb, 0x2D0, NodeKind::UInt64,    "AppCompatFlagsUser");
    addField(peb, 0x2D8, NodeKind::Pointer64, "pShimData");
    addField(peb, 0x2E0, NodeKind::Pointer64, "AppCompatInfo");

    // 0x2E8 – 0x2F7: _STRING64 CSDVersion
    {
        uint64_t sid = addStruct(peb, 0x2E8, "_STRING64", "CSDVersion");
        addField(sid, 0, NodeKind::UInt16,    "Length");
        addField(sid, 2, NodeKind::UInt16,    "MaximumLength");
        addPad  (sid, 4, 4,                   "Pad");
        addField(sid, 8, NodeKind::Pointer64, "Buffer");
    }

    // 0x2F8 – 0x31F
    addField(peb, 0x2F8, NodeKind::Pointer64, "ActivationContextData");
    addField(peb, 0x300, NodeKind::Pointer64, "ProcessAssemblyStorageMap");
    addField(peb, 0x308, NodeKind::Pointer64, "SystemDefaultActivationContextData");
    addField(peb, 0x310, NodeKind::Pointer64, "SystemAssemblyStorageMap");
    addField(peb, 0x318, NodeKind::UInt64,    "MinimumStackCommit");

    // 0x320 – 0x34B
    addArray(peb, 0x320, "SparePointers", 2, NodeKind::UInt64);
    addField(peb, 0x330, NodeKind::Pointer64, "PatchLoaderData");
    addField(peb, 0x338, NodeKind::Pointer64, "ChpeV2ProcessInfo");
    addField(peb, 0x340, NodeKind::UInt32,    "AppModelFeatureState");
    addArray(peb, 0x344, "SpareUlongs", 2, NodeKind::UInt32);
    addField(peb, 0x34C, NodeKind::UInt16,    "ActiveCodePage");
    addField(peb, 0x34E, NodeKind::UInt16,    "OemCodePage");
    addField(peb, 0x350, NodeKind::UInt16,    "UseCaseMapping");
    addField(peb, 0x352, NodeKind::UInt16,    "UnusedNlsField");

    // 0x354 – 0x37F
    addPad  (peb, 0x354, 4,                   "Pad354");
    addField(peb, 0x358, NodeKind::Pointer64, "WerRegistrationData");
    addField(peb, 0x360, NodeKind::Pointer64, "WerShipAssertPtr");
    addField(peb, 0x368, NodeKind::Pointer64, "EcCodeBitMap");
    addField(peb, 0x370, NodeKind::Pointer64, "pImageHeaderHash");
    addField(peb, 0x378, NodeKind::UInt32,    "TracingFlags");
    addPad  (peb, 0x37C, 4,                   "Padding6");

    // 0x380 – 0x39F
    addField(peb, 0x380, NodeKind::Pointer64, "CsrServerReadOnlySharedMemoryBase");
    addField(peb, 0x388, NodeKind::UInt64,    "TppWorkerpListLock");

    // LIST_ENTRY64 TppWorkerpList
    {
        uint64_t sid = addStruct(peb, 0x390, "LIST_ENTRY64", "TppWorkerpList");
        addField(sid, 0, NodeKind::Pointer64, "Flink");
        addField(sid, 8, NodeKind::Pointer64, "Blink");
    }

    // 0x3A0 – 0x79F
    addArray(peb, 0x3A0, "WaitOnAddressHashTable", 128, NodeKind::UInt64);

    // 0x7A0 – 0x7CF
    addField(peb, 0x7A0, NodeKind::Pointer64, "TelemetryCoverageHeader");
    addField(peb, 0x7A8, NodeKind::UInt32,    "CloudFileFlags");
    addField(peb, 0x7AC, NodeKind::UInt32,    "CloudFileDiagFlags");
    addField(peb, 0x7B0, NodeKind::Int8,      "PlaceholderCompatibilityMode");
    addArray(peb, 0x7B1, "PlaceholderCompatibilityModeReserved", 7, NodeKind::Int8);
    addField(peb, 0x7B8, NodeKind::Pointer64, "LeapSecondData");
    addField(peb, 0x7C0, NodeKind::UInt32,    "LeapSecondFlags");
    addField(peb, 0x7C4, NodeKind::UInt32,    "NtGlobalFlag2");
    addField(peb, 0x7C8, NodeKind::UInt64,    "ExtendedFeatureDisableMask");

    createTab(doc);
}

void MainWindow::openFile() {
    QString path = QFileDialog::getOpenFileName(this,
        "Open Definition", {}, "ReclassX (*.rcx);;JSON (*.json);;All (*)");
    if (path.isEmpty()) return;

    auto* doc = new RcxDocument(this);
    if (!doc->load(path)) {
        QMessageBox::warning(this, "Error", "Failed to load: " + path);
        delete doc;
        return;
    }
    createTab(doc);
}

void MainWindow::saveFile() {
    auto* tab = activeTab();
    if (!tab) return;
    if (tab->doc->filePath.isEmpty()) { saveFileAs(); return; }
    tab->doc->save(tab->doc->filePath);
    updateWindowTitle();
}

void MainWindow::saveFileAs() {
    auto* tab = activeTab();
    if (!tab) return;
    QString path = QFileDialog::getSaveFileName(this,
        "Save Definition", {}, "ReclassX (*.rcx);;JSON (*.json)");
    if (path.isEmpty()) return;
    tab->doc->save(path);
    updateWindowTitle();
}

void MainWindow::loadBinary() {
    auto* tab = activeTab();
    if (!tab) return;
    QString path = QFileDialog::getOpenFileName(this,
        "Load Binary Data", {}, "All Files (*)");
    if (path.isEmpty()) return;
    tab->doc->loadData(path);
}

void MainWindow::addNode() {
    auto* ctrl = activeController();
    if (!ctrl) return;

    uint64_t parentId = 0;
    auto* primary = ctrl->primaryEditor();
    if (primary && primary->isEditing()) return;
    if (primary) {
        int ni = primary->currentNodeIndex();
        if (ni >= 0) {
            auto& node = ctrl->document()->tree.nodes[ni];
            if (node.kind == NodeKind::Struct || node.kind == NodeKind::Array)
                parentId = node.id;
            else
                parentId = node.parentId;
        }
    }
    ctrl->insertNode(parentId, -1, NodeKind::Hex64, "newField");
}

void MainWindow::removeNode() {
    auto* ctrl = activeController();
    if (!ctrl) return;
    auto* primary = ctrl->primaryEditor();
    if (!primary || primary->isEditing()) return;
    QSet<int> indices = primary->selectedNodeIndices();
    if (indices.size() > 1) {
        ctrl->batchRemoveNodes(indices.values());
    } else if (indices.size() == 1) {
        ctrl->removeNode(*indices.begin());
    }
}

void MainWindow::changeNodeType() {
    auto* ctrl = activeController();
    if (!ctrl) return;
    auto* primary = ctrl->primaryEditor();
    if (!primary) return;
    primary->beginInlineEdit(EditTarget::Type);
}

void MainWindow::renameNodeAction() {
    auto* ctrl = activeController();
    if (!ctrl) return;
    auto* primary = ctrl->primaryEditor();
    if (!primary) return;
    primary->beginInlineEdit(EditTarget::Name);
}

void MainWindow::duplicateNodeAction() {
    auto* ctrl = activeController();
    if (!ctrl) return;
    auto* primary = ctrl->primaryEditor();
    if (!primary || primary->isEditing()) return;
    int ni = primary->currentNodeIndex();
    if (ni >= 0) ctrl->duplicateNode(ni);
}

void MainWindow::splitView() {
    auto* tab = activeTab();
    if (!tab) return;
    tab->ctrl->addSplitEditor(tab->splitter);
}

void MainWindow::unsplitView() {
    auto* tab = activeTab();
    if (!tab) return;
    auto editors = tab->ctrl->editors();
    if (editors.size() > 1)
        tab->ctrl->removeSplitEditor(editors.last());
}

void MainWindow::undo() {
    auto* tab = activeTab();
    if (tab) tab->doc->undoStack.undo();
}

void MainWindow::redo() {
    auto* tab = activeTab();
    if (tab) tab->doc->undoStack.redo();
}

void MainWindow::about() {
    QMessageBox::about(this, "About ReclassX",
        "ReclassX - Structured Binary Editor\n"
        "Built with Qt 6 + QScintilla\n\n"
        "Margin-driven UI with offset display,\n"
        "fold markers, and status flags.");
}

void MainWindow::setEditorFont(const QString& fontName) {
    QSettings settings("ReclassX", "ReclassX");
    settings.setValue("font", fontName);
    // Notify all controllers to refresh fonts
    for (auto& state : m_tabs) {
        state.ctrl->setEditorFont(fontName);
    }
}

RcxController* MainWindow::activeController() const {
    auto* sub = m_mdiArea->activeSubWindow();
    if (sub && m_tabs.contains(sub))
        return m_tabs[sub].ctrl;
    return nullptr;
}

MainWindow::TabState* MainWindow::activeTab() {
    auto* sub = m_mdiArea->activeSubWindow();
    if (sub && m_tabs.contains(sub))
        return &m_tabs[sub];
    return nullptr;
}

void MainWindow::updateWindowTitle() {
    auto* sub = m_mdiArea->activeSubWindow();
    if (sub && m_tabs.contains(sub)) {
        auto& tab = m_tabs[sub];
        QString name = tab.doc->filePath.isEmpty() ? "Untitled"
                       : QFileInfo(tab.doc->filePath).fileName();
        if (tab.doc->modified) name += " *";
        setWindowTitle(name + " - ReclassX");
    } else {
        setWindowTitle("ReclassX");
    }
}

} // namespace rcx

// ── Entry point ──

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#endif

    QApplication app(argc, argv);
    app.setApplicationName("ReclassX");
    app.setOrganizationName("ReclassX");
    app.setStyle("Fusion"); // Fusion style respects dark palette well

    // Load embedded fonts
    int fontId = QFontDatabase::addApplicationFont(":/fonts/Iosevka-Regular.ttf");
    if (fontId == -1)
        qWarning("Failed to load embedded Iosevka font");
    // Apply saved font preference before creating any editors
    {
        QSettings settings("ReclassX", "ReclassX");
        QString savedFont = settings.value("font", "Consolas").toString();
        rcx::RcxEditor::setGlobalFontName(savedFont);
    }

    // Global dark palette
    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window,          QColor("#1e1e1e"));
    darkPalette.setColor(QPalette::WindowText,      QColor("#d4d4d4"));
    darkPalette.setColor(QPalette::Base,            QColor("#252526"));
    darkPalette.setColor(QPalette::AlternateBase,   QColor("#2a2d2e"));
    darkPalette.setColor(QPalette::Text,            QColor("#d4d4d4"));
    darkPalette.setColor(QPalette::Button,          QColor("#333333"));
    darkPalette.setColor(QPalette::ButtonText,      QColor("#d4d4d4"));
    darkPalette.setColor(QPalette::Highlight,       QColor("#264f78"));
    darkPalette.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    darkPalette.setColor(QPalette::ToolTipBase,     QColor("#252526"));
    darkPalette.setColor(QPalette::ToolTipText,     QColor("#d4d4d4"));
    darkPalette.setColor(QPalette::Mid,             QColor("#3c3c3c"));
    darkPalette.setColor(QPalette::Dark,            QColor("#1e1e1e"));
    darkPalette.setColor(QPalette::Light,           QColor("#505050"));
    app.setPalette(darkPalette);

    rcx::MainWindow window;

    bool screenshotMode = app.arguments().contains("--screenshot");
    if (screenshotMode)
        window.setWindowOpacity(0.0);
    window.show();

    // Always auto-open PEB64 demo on startup
    QMetaObject::invokeMethod(&window, "newFile");

    if (screenshotMode) {
        QString out = "screenshot.png";
        int idx = app.arguments().indexOf("--screenshot");
        if (idx + 1 < app.arguments().size())
            out = app.arguments().at(idx + 1);

        QTimer::singleShot(1000, [&window, out]() {
            QDir().mkpath(QFileInfo(out).absolutePath());
            window.grab().save(out);
            ::_exit(0);  // immediate exit — no need for clean shutdown in screenshot mode
        });
    }

    return app.exec();
}

#include "main.moc"
