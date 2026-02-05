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

void MainWindow::createMenus() {
    // File
    auto* file = menuBar()->addMenu("&File");
    file->addAction("&New",            QKeySequence::New,    this, &MainWindow::newFile);
    file->addAction("&Open...",        QKeySequence::Open,   this, &MainWindow::openFile);
    file->addSeparator();
    file->addAction("&Save",           QKeySequence::Save,   this, &MainWindow::saveFile);
    file->addAction("Save &As...",     QKeySequence::SaveAs, this, &MainWindow::saveFileAs);
    file->addSeparator();
    file->addAction("Load &Binary...", this, &MainWindow::loadBinary);
    file->addSeparator();
    file->addAction("E&xit",           QKeySequence::Quit,   this, &QMainWindow::close);

    // Edit
    auto* edit = menuBar()->addMenu("&Edit");
    edit->addAction("&Undo", QKeySequence::Undo, this, &MainWindow::undo);
    edit->addAction("&Redo", QKeySequence::Redo, this, &MainWindow::redo);

    // View
    auto* view = menuBar()->addMenu("&View");
    view->addAction("Split &Horizontal", this, &MainWindow::splitView);
    view->addAction("&Unsplit",          this, &MainWindow::unsplitView);
    view->addSeparator();
    auto* fontMenu = view->addMenu("&Font");
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
    node->addAction("&Add Field",    QKeySequence(Qt::Key_Insert), this, &MainWindow::addNode);
    node->addAction("&Remove Field", QKeySequence::Delete,         this, &MainWindow::removeNode);
    auto* actType = node->addAction("Change &Type", this, &MainWindow::changeNodeType);
    actType->setText("Change &Type\tT");
    auto* actName = node->addAction("Re&name", this, &MainWindow::renameNodeAction);
    actName->setText("Re&name\tF2");

    // Help
    auto* help = menuBar()->addMenu("&Help");
    help->addAction("&About ReclassX", this, &MainWindow::about);
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
                QString("%1 %2  offset: +0x%3  size: %4 bytes")
                    .arg(kindToString(node.kind))
                    .arg(node.name)
                    .arg(node.offset, 4, 16, QChar('0'))
                    .arg(node.byteSize()));
        } else {
            m_statusLabel->setText("Ready");
        }
    });

    ctrl->refresh();
    return sub;
}

void MainWindow::newFile() {
    auto* doc = new RcxDocument(this);

    // ══════════════════════════════════════════════════════════════════════════
    // PE Header Demo - Realistic PE32+ (64-bit) executable structure
    // ══════════════════════════════════════════════════════════════════════════
    // Layout:
    //   0x000: DOS Header (64 bytes)
    //   0x040: DOS Stub (64 bytes padding)
    //   0x080: PE Signature (4 bytes)
    //   0x084: File Header (20 bytes)
    //   0x098: Optional Header PE32+ (240 bytes)
    //     - Standard fields (24 bytes)
    //     - Windows fields (88 bytes)
    //     - Data Directories (16 * 8 = 128 bytes)
    //   0x188: Section Headers (4 * 40 = 160 bytes)
    //   0x228: End of headers (total 552 bytes)
    // ══════════════════════════════════════════════════════════════════════════

    QByteArray peData(0x300, '\0');  // 768 bytes
    char* d = peData.data();

    // ── DOS Header (IMAGE_DOS_HEADER) ──
    d[0x00] = 'M'; d[0x01] = 'Z';                    // e_magic
    *(uint16_t*)(d + 0x02) = 0x0090;                 // e_cblp (bytes on last page)
    *(uint16_t*)(d + 0x04) = 0x0003;                 // e_cp (pages in file)
    *(uint16_t*)(d + 0x06) = 0x0000;                 // e_crlc (relocations)
    *(uint16_t*)(d + 0x08) = 0x0004;                 // e_cparhdr (header size in paragraphs)
    *(uint16_t*)(d + 0x0A) = 0x0000;                 // e_minalloc
    *(uint16_t*)(d + 0x0C) = 0xFFFF;                 // e_maxalloc
    *(uint16_t*)(d + 0x0E) = 0x0000;                 // e_ss
    *(uint16_t*)(d + 0x10) = 0x00B8;                 // e_sp
    *(uint16_t*)(d + 0x12) = 0x0000;                 // e_csum
    *(uint16_t*)(d + 0x14) = 0x0000;                 // e_ip
    *(uint16_t*)(d + 0x16) = 0x0000;                 // e_cs
    *(uint16_t*)(d + 0x18) = 0x0040;                 // e_lfarlc
    *(uint16_t*)(d + 0x1A) = 0x0000;                 // e_ovno
    // e_res[4] at 0x1C-0x23 (zeroed)
    *(uint16_t*)(d + 0x24) = 0x0000;                 // e_oemid
    *(uint16_t*)(d + 0x26) = 0x0000;                 // e_oeminfo
    // e_res2[10] at 0x28-0x3B (zeroed)
    *(uint32_t*)(d + 0x3C) = 0x00000080;             // e_lfanew → PE header at 0x80

    // ── PE Signature ──
    const int peOff = 0x80;
    d[peOff+0] = 'P'; d[peOff+1] = 'E'; d[peOff+2] = 0; d[peOff+3] = 0;

    // ── File Header (IMAGE_FILE_HEADER) ──
    const int fhOff = peOff + 4;  // 0x84
    *(uint16_t*)(d + fhOff + 0)  = 0x8664;           // Machine (AMD64)
    *(uint16_t*)(d + fhOff + 2)  = 0x0004;           // NumberOfSections
    *(uint32_t*)(d + fhOff + 4)  = 0x65A3B2C1;       // TimeDateStamp
    *(uint32_t*)(d + fhOff + 8)  = 0x00000000;       // PointerToSymbolTable
    *(uint32_t*)(d + fhOff + 12) = 0x00000000;       // NumberOfSymbols
    *(uint16_t*)(d + fhOff + 16) = 0x00F0;           // SizeOfOptionalHeader (240)
    *(uint16_t*)(d + fhOff + 18) = 0x0022;           // Characteristics (EXECUTABLE|LARGE_ADDRESS_AWARE)

    // ── Optional Header PE32+ (IMAGE_OPTIONAL_HEADER64) ──
    const int ohOff = fhOff + 20;  // 0x98
    *(uint16_t*)(d + ohOff + 0)  = 0x020B;           // Magic (PE32+)
    *(uint8_t*)(d + ohOff + 2)   = 0x0E;             // MajorLinkerVersion
    *(uint8_t*)(d + ohOff + 3)   = 0x00;             // MinorLinkerVersion
    *(uint32_t*)(d + ohOff + 4)  = 0x00012000;       // SizeOfCode
    *(uint32_t*)(d + ohOff + 8)  = 0x00008000;       // SizeOfInitializedData
    *(uint32_t*)(d + ohOff + 12) = 0x00000000;       // SizeOfUninitializedData
    *(uint32_t*)(d + ohOff + 16) = 0x00001000;       // AddressOfEntryPoint
    *(uint32_t*)(d + ohOff + 20) = 0x00001000;       // BaseOfCode

    // Windows-specific fields (PE32+)
    *(uint64_t*)(d + ohOff + 24) = 0x0000000140000000ULL; // ImageBase
    *(uint32_t*)(d + ohOff + 32) = 0x00001000;       // SectionAlignment
    *(uint32_t*)(d + ohOff + 36) = 0x00000200;       // FileAlignment
    *(uint16_t*)(d + ohOff + 40) = 0x0006;           // MajorOperatingSystemVersion
    *(uint16_t*)(d + ohOff + 42) = 0x0000;           // MinorOperatingSystemVersion
    *(uint16_t*)(d + ohOff + 44) = 0x0000;           // MajorImageVersion
    *(uint16_t*)(d + ohOff + 46) = 0x0000;           // MinorImageVersion
    *(uint16_t*)(d + ohOff + 48) = 0x0006;           // MajorSubsystemVersion
    *(uint16_t*)(d + ohOff + 50) = 0x0000;           // MinorSubsystemVersion
    *(uint32_t*)(d + ohOff + 52) = 0x00000000;       // Win32VersionValue
    *(uint32_t*)(d + ohOff + 56) = 0x00025000;       // SizeOfImage
    *(uint32_t*)(d + ohOff + 60) = 0x00000200;       // SizeOfHeaders
    *(uint32_t*)(d + ohOff + 64) = 0x00000000;       // CheckSum
    *(uint16_t*)(d + ohOff + 68) = 0x0003;           // Subsystem (CONSOLE)
    *(uint16_t*)(d + ohOff + 70) = 0x8160;           // DllCharacteristics (DYNAMIC_BASE|NX_COMPAT|TERMINAL_SERVER_AWARE)
    *(uint64_t*)(d + ohOff + 72) = 0x0000000000100000ULL; // SizeOfStackReserve
    *(uint64_t*)(d + ohOff + 80) = 0x0000000000001000ULL; // SizeOfStackCommit
    *(uint64_t*)(d + ohOff + 88) = 0x0000000000100000ULL; // SizeOfHeapReserve
    *(uint64_t*)(d + ohOff + 96) = 0x0000000000001000ULL; // SizeOfHeapCommit
    *(uint32_t*)(d + ohOff + 104) = 0x00000000;      // LoaderFlags
    *(uint32_t*)(d + ohOff + 108) = 0x00000010;      // NumberOfRvaAndSizes (16)

    // ── Data Directories (16 entries × 8 bytes) ──
    const int ddOff = ohOff + 112;  // 0x108
    // Each entry: VirtualAddress (4) + Size (4)
    struct { uint32_t rva; uint32_t size; } dataDirs[16] = {
        {0x00000000, 0x00000000},  // 0: Export
        {0x00014000, 0x000000A0},  // 1: Import
        {0x00000000, 0x00000000},  // 2: Resource
        {0x00000000, 0x00000000},  // 3: Exception
        {0x00000000, 0x00000000},  // 4: Security
        {0x00000000, 0x00000000},  // 5: BaseReloc
        {0x00013000, 0x00000038},  // 6: Debug
        {0x00000000, 0x00000000},  // 7: Architecture
        {0x00000000, 0x00000000},  // 8: GlobalPtr
        {0x00000000, 0x00000000},  // 9: TLS
        {0x00000000, 0x00000000},  // 10: LoadConfig
        {0x00000000, 0x00000000},  // 11: BoundImport
        {0x00014050, 0x00000048},  // 12: IAT
        {0x00000000, 0x00000000},  // 13: DelayImport
        {0x00000000, 0x00000000},  // 14: CLR
        {0x00000000, 0x00000000},  // 15: Reserved
    };
    for (int i = 0; i < 16; i++) {
        *(uint32_t*)(d + ddOff + i*8 + 0) = dataDirs[i].rva;
        *(uint32_t*)(d + ddOff + i*8 + 4) = dataDirs[i].size;
    }

    // ── Section Headers (4 sections × 40 bytes) ──
    const int shOff = ddOff + 128;  // 0x188
    struct SectionDef { const char* name; uint32_t vsize; uint32_t vaddr; uint32_t rawsz; uint32_t rawptr; uint32_t chars; };
    SectionDef sections[4] = {
        {".text",   0x00011234, 0x00001000, 0x00011400, 0x00000200, 0x60000020},  // CODE|EXECUTE|READ
        {".rdata",  0x00002ABC, 0x00013000, 0x00002C00, 0x00011600, 0x40000040},  // INITIALIZED|READ
        {".data",   0x00001000, 0x00016000, 0x00000400, 0x00014200, 0xC0000040},  // INITIALIZED|READ|WRITE
        {".pdata",  0x00000800, 0x00017000, 0x00000800, 0x00014600, 0x40000040},  // INITIALIZED|READ
    };
    for (int i = 0; i < 4; i++) {
        int off = shOff + i * 40;
        memcpy(d + off, sections[i].name, 8);                    // Name[8]
        *(uint32_t*)(d + off + 8)  = sections[i].vsize;          // VirtualSize
        *(uint32_t*)(d + off + 12) = sections[i].vaddr;          // VirtualAddress
        *(uint32_t*)(d + off + 16) = sections[i].rawsz;          // SizeOfRawData
        *(uint32_t*)(d + off + 20) = sections[i].rawptr;         // PointerToRawData
        *(uint32_t*)(d + off + 24) = 0x00000000;                 // PointerToRelocations
        *(uint32_t*)(d + off + 28) = 0x00000000;                 // PointerToLinenumbers
        *(uint16_t*)(d + off + 32) = 0x0000;                     // NumberOfRelocations
        *(uint16_t*)(d + off + 34) = 0x0000;                     // NumberOfLinenumbers
        *(uint32_t*)(d + off + 36) = sections[i].chars;          // Characteristics
    }

    doc->loadData(peData);
    doc->tree.baseAddress = 0x140000000;  // Typical 64-bit image base

    // ══════════════════════════════════════════════════════════════════════════
    // Build Node Tree
    // ══════════════════════════════════════════════════════════════════════════

    auto addField = [&](uint64_t parent, int offset, NodeKind kind, const QString& name) -> uint64_t {
        Node n;
        n.kind = kind;
        n.name = name;
        n.parentId = parent;
        n.offset = offset;
        int idx = doc->tree.addNode(n);
        return doc->tree.nodes[idx].id;
    };

    auto addStruct = [&](uint64_t parent, int offset, const QString& name) -> uint64_t {
        Node n;
        n.kind = NodeKind::Struct;
        n.name = name;
        n.parentId = parent;
        n.offset = offset;
        int idx = doc->tree.addNode(n);
        return doc->tree.nodes[idx].id;
    };

    auto addArray = [&](uint64_t parent, int offset, const QString& name, int count, NodeKind elemKind) -> uint64_t {
        Node n;
        n.kind = NodeKind::Array;
        n.name = name;
        n.parentId = parent;
        n.offset = offset;
        n.arrayLen = count;
        n.elementKind = elemKind;
        int idx = doc->tree.addNode(n);
        return doc->tree.nodes[idx].id;
    };

    // ── Root: IMAGE_DOS_HEADER ──
    uint64_t dosId = addStruct(0, 0x00, "IMAGE_DOS_HEADER");
    addField(dosId, 0x00, NodeKind::UInt16, "e_magic");
    addField(dosId, 0x02, NodeKind::UInt16, "e_cblp");
    addField(dosId, 0x04, NodeKind::UInt16, "e_cp");
    addField(dosId, 0x06, NodeKind::UInt16, "e_crlc");
    addField(dosId, 0x08, NodeKind::UInt16, "e_cparhdr");
    addField(dosId, 0x0A, NodeKind::UInt16, "e_minalloc");
    addField(dosId, 0x0C, NodeKind::UInt16, "e_maxalloc");
    addField(dosId, 0x0E, NodeKind::UInt16, "e_ss");
    addField(dosId, 0x10, NodeKind::UInt16, "e_sp");
    addField(dosId, 0x12, NodeKind::UInt16, "e_csum");
    addField(dosId, 0x14, NodeKind::UInt16, "e_ip");
    addField(dosId, 0x16, NodeKind::UInt16, "e_cs");
    addField(dosId, 0x18, NodeKind::UInt16, "e_lfarlc");
    addField(dosId, 0x1A, NodeKind::UInt16, "e_ovno");
    addField(dosId, 0x3C, NodeKind::UInt32, "e_lfanew");

    // ── PE Signature ──
    addField(0, peOff, NodeKind::UInt32, "PE_Signature");

    // ── IMAGE_FILE_HEADER ──
    uint64_t fhId = addStruct(0, fhOff, "IMAGE_FILE_HEADER");
    addField(fhId, 0, NodeKind::UInt16, "Machine");
    addField(fhId, 2, NodeKind::UInt16, "NumberOfSections");
    addField(fhId, 4, NodeKind::UInt32, "TimeDateStamp");
    addField(fhId, 8, NodeKind::UInt32, "PointerToSymbolTable");
    addField(fhId, 12, NodeKind::UInt32, "NumberOfSymbols");
    addField(fhId, 16, NodeKind::UInt16, "SizeOfOptionalHeader");
    addField(fhId, 18, NodeKind::UInt16, "Characteristics");

    // ── IMAGE_OPTIONAL_HEADER64 ──
    uint64_t ohId = addStruct(0, ohOff, "IMAGE_OPTIONAL_HEADER64");
    addField(ohId, 0, NodeKind::UInt16, "Magic");
    addField(ohId, 2, NodeKind::UInt8, "MajorLinkerVersion");
    addField(ohId, 3, NodeKind::UInt8, "MinorLinkerVersion");
    addField(ohId, 4, NodeKind::UInt32, "SizeOfCode");
    addField(ohId, 8, NodeKind::UInt32, "SizeOfInitializedData");
    addField(ohId, 12, NodeKind::UInt32, "SizeOfUninitializedData");
    addField(ohId, 16, NodeKind::UInt32, "AddressOfEntryPoint");
    addField(ohId, 20, NodeKind::UInt32, "BaseOfCode");
    addField(ohId, 24, NodeKind::UInt64, "ImageBase");
    addField(ohId, 32, NodeKind::UInt32, "SectionAlignment");
    addField(ohId, 36, NodeKind::UInt32, "FileAlignment");
    addField(ohId, 40, NodeKind::UInt16, "MajorOSVersion");
    addField(ohId, 42, NodeKind::UInt16, "MinorOSVersion");
    addField(ohId, 44, NodeKind::UInt16, "MajorImageVersion");
    addField(ohId, 46, NodeKind::UInt16, "MinorImageVersion");
    addField(ohId, 48, NodeKind::UInt16, "MajorSubsystemVersion");
    addField(ohId, 50, NodeKind::UInt16, "MinorSubsystemVersion");
    addField(ohId, 52, NodeKind::UInt32, "Win32VersionValue");
    addField(ohId, 56, NodeKind::UInt32, "SizeOfImage");
    addField(ohId, 60, NodeKind::UInt32, "SizeOfHeaders");
    addField(ohId, 64, NodeKind::UInt32, "CheckSum");
    addField(ohId, 68, NodeKind::UInt16, "Subsystem");
    addField(ohId, 70, NodeKind::UInt16, "DllCharacteristics");
    addField(ohId, 72, NodeKind::UInt64, "SizeOfStackReserve");
    addField(ohId, 80, NodeKind::UInt64, "SizeOfStackCommit");
    addField(ohId, 88, NodeKind::UInt64, "SizeOfHeapReserve");
    addField(ohId, 96, NodeKind::UInt64, "SizeOfHeapCommit");
    addField(ohId, 104, NodeKind::UInt32, "LoaderFlags");
    addField(ohId, 108, NodeKind::UInt32, "NumberOfRvaAndSizes");

    // ── Data Directories Array (16 entries) ──
    uint64_t ddArrId = addArray(ohId, 112, "DataDirectory", 16, NodeKind::Struct);
    const char* ddNames[16] = {
        "Export", "Import", "Resource", "Exception",
        "Security", "BaseReloc", "Debug", "Architecture",
        "GlobalPtr", "TLS", "LoadConfig", "BoundImport",
        "IAT", "DelayImport", "CLR", "Reserved"
    };
    for (int i = 0; i < 16; i++) {
        uint64_t entryId = addStruct(ddArrId, i * 8, QString("[%1] %2").arg(i).arg(ddNames[i]));
        addField(entryId, 0, NodeKind::UInt32, "VirtualAddress");
        addField(entryId, 4, NodeKind::UInt32, "Size");
    }

    // ── Section Headers Array (4 sections) ──
    uint64_t shArrId = addArray(0, shOff, "SectionHeaders", 4, NodeKind::Struct);
    const char* secNames[4] = {".text", ".rdata", ".data", ".pdata"};
    for (int i = 0; i < 4; i++) {
        uint64_t secId = addStruct(shArrId, i * 40, QString("[%1] %2").arg(i).arg(secNames[i]));
        // Name is 8 bytes - show as UTF8 string
        Node nameNode;
        nameNode.kind = NodeKind::UTF8;
        nameNode.name = "Name";
        nameNode.parentId = secId;
        nameNode.offset = 0;
        nameNode.strLen = 8;
        doc->tree.addNode(nameNode);
        addField(secId, 8, NodeKind::UInt32, "VirtualSize");
        addField(secId, 12, NodeKind::UInt32, "VirtualAddress");
        addField(secId, 16, NodeKind::UInt32, "SizeOfRawData");
        addField(secId, 20, NodeKind::UInt32, "PointerToRawData");
        addField(secId, 24, NodeKind::UInt32, "PointerToRelocations");
        addField(secId, 28, NodeKind::UInt32, "PointerToLinenumbers");
        addField(secId, 32, NodeKind::UInt16, "NumberOfRelocations");
        addField(secId, 34, NodeKind::UInt16, "NumberOfLinenumbers");
        addField(secId, 36, NodeKind::UInt32, "Characteristics");
    }

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

    // Load embedded Iosevka font
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

    // Always auto-open PE header demo on startup
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
