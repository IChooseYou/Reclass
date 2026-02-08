#include "controller.h"
#include "generator.h"
#include "pluginmanager.h"
#include <QApplication>
#include <QMainWindow>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QSplitter>
#include <QStackedWidget>
#include <QPointer>
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
#include <QDockWidget>
#include <QTreeView>
#include <QStandardItemModel>
#include <QListWidget>
#include <QPushButton>
#include "workspace_model.h"
#include <QTableWidget>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QDialog>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexercpp.h>

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
    void selfTest();
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
    void exportCpp();
    void showTypeAliasesDialog();

public:
    // Project Lifecycle API
    QMdiSubWindow* project_new();
    QMdiSubWindow* project_open(const QString& path = {});
    bool project_save(QMdiSubWindow* sub = nullptr, bool saveAs = false);
    void project_close(QMdiSubWindow* sub = nullptr);

private:
    enum ViewMode { VM_Reclass, VM_Rendered };

    QMdiArea* m_mdiArea;
    QLabel*   m_statusLabel;
    PluginManager m_pluginManager;

    struct TabState {
        RcxDocument*              doc;
        RcxController*            ctrl;
        QSplitter*                splitter;
        QStackedWidget*           stack       = nullptr;
        QPointer<QsciScintilla>   rendered;
        ViewMode                  viewMode    = VM_Reclass;
        uint64_t                  lastRenderedRootId      = 0;
        int                       lastRenderedFirstLine   = 0;
    };
    QMap<QMdiSubWindow*, TabState> m_tabs;

    QAction* m_actViewReclass  = nullptr;
    QAction* m_actViewRendered = nullptr;

    void createMenus();
    void createStatusBar();
    void showPluginsDialog();
    QIcon makeIcon(const QString& svgPath);

    RcxController* activeController() const;
    TabState* activeTab();
    QMdiSubWindow* createTab(RcxDocument* doc);
    void updateWindowTitle();

    void setViewMode(ViewMode mode);
    void updateRenderedView(TabState& tab);
    void syncRenderMenuState();
    uint64_t findRootStructForNode(const NodeTree& tree, uint64_t nodeId) const;
    void setupRenderedSci(QsciScintilla* sci);

    // Workspace dock
    QDockWidget*        m_workspaceDock  = nullptr;
    QTreeView*          m_workspaceTree  = nullptr;
    QStandardItemModel* m_workspaceModel = nullptr;
    void createWorkspaceDock();
    void rebuildWorkspaceModel();
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("ReclassX");
    resize(1200, 800);

    m_mdiArea = new QMdiArea(this);
    m_mdiArea->setViewMode(QMdiArea::TabbedView);
    m_mdiArea->setTabsClosable(true);
    m_mdiArea->setTabsMovable(true);
    setCentralWidget(m_mdiArea);

    createWorkspaceDock();
    createMenus();
    createStatusBar();

    // Load plugins
    m_pluginManager.LoadPlugins();

    connect(m_mdiArea, &QMdiArea::subWindowActivated,
            this, [this](QMdiSubWindow*) {
        updateWindowTitle();
        syncRenderMenuState();
        rebuildWorkspaceModel();
    });
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
    file->addAction(makeIcon(":/vsicons/export.svg"), "Export &C++ Header...", this, &MainWindow::exportCpp);
    file->addSeparator();
    file->addAction(makeIcon(":/vsicons/close.svg"), "E&xit", QKeySequence(Qt::Key_Close), this, &QMainWindow::close);

    // Edit
    auto* edit = menuBar()->addMenu("&Edit");
    edit->addAction(makeIcon(":/vsicons/arrow-left.svg"), "&Undo", QKeySequence::Undo, this, &MainWindow::undo);
    edit->addAction(makeIcon(":/vsicons/arrow-right.svg"), "&Redo", QKeySequence::Redo, this, &MainWindow::redo);
    edit->addSeparator();
    edit->addAction(makeIcon(":/vsicons/symbol-structure.svg"), "&Type Aliases...", this, &MainWindow::showTypeAliasesDialog);

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

    view->addSeparator();
    m_actViewRendered = view->addAction(makeIcon(":/vsicons/code.svg"), "&C/C++", this, [this]() { setViewMode(VM_Rendered); });
    m_actViewReclass  = view->addAction(makeIcon(":/vsicons/eye.svg"), "&Reclass View", this, [this]() { setViewMode(VM_Reclass); });
    view->addSeparator();
    view->addAction(m_workspaceDock->toggleViewAction());

    // Node
    auto* node = menuBar()->addMenu("&Node");
    node->addAction(makeIcon(":/vsicons/add.svg"), "&Add Field", QKeySequence(Qt::Key_Insert), this, &MainWindow::addNode);
    node->addAction(makeIcon(":/vsicons/remove.svg"), "&Remove Field", QKeySequence::Delete, this, &MainWindow::removeNode);
    node->addAction(makeIcon(":/vsicons/symbol-structure.svg"), "Change &Type", QKeySequence(Qt::Key_T), this, &MainWindow::changeNodeType);
    node->addAction(makeIcon(":/vsicons/edit.svg"), "Re&name", QKeySequence(Qt::Key_F2), this, &MainWindow::renameNodeAction);
    node->addAction(makeIcon(":/vsicons/files.svg"), "D&uplicate", this, &MainWindow::duplicateNodeAction)->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));

    // Plugins
    auto* plugins = menuBar()->addMenu("&Plugins");
    plugins->addAction("&Manage Plugins...", this, &MainWindow::showPluginsDialog);

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
    // QStackedWidget wraps [0] splitter (Reclass view) and [1] rendered QsciScintilla
    auto* stack = new QStackedWidget;
    auto* splitter = new QSplitter(Qt::Horizontal);
    auto* ctrl = new RcxController(doc, splitter);
    ctrl->addSplitEditor(splitter);

    stack->addWidget(splitter);   // index 0 = Reclass view

    auto* renderedSci = new QsciScintilla;
    setupRenderedSci(renderedSci);
    stack->addWidget(renderedSci); // index 1 = Rendered view
    stack->setCurrentIndex(0);

    auto* sub = m_mdiArea->addSubWindow(stack);
    sub->setWindowTitle(doc->filePath.isEmpty()
                        ? "Untitled" : QFileInfo(doc->filePath).fileName());
    sub->setAttribute(Qt::WA_DeleteOnClose);
    sub->showMaximized();

    m_tabs[sub] = { doc, ctrl, splitter, stack, renderedSci,
                    VM_Reclass, 0, 0 };

    connect(sub, &QObject::destroyed, this, [this, sub]() {
        auto it = m_tabs.find(sub);
        if (it != m_tabs.end()) {
            it->doc->deleteLater();
            m_tabs.erase(it);
        }
        rebuildWorkspaceModel();
    });

    connect(ctrl, &RcxController::nodeSelected,
            this, [this, ctrl, sub](int nodeIdx) {
        if (nodeIdx >= 0 && nodeIdx < ctrl->document()->tree.nodes.size()) {
            auto& node = ctrl->document()->tree.nodes[nodeIdx];
            auto it = m_tabs.find(sub);
            if (it != m_tabs.end() && it->viewMode == VM_Rendered)
                m_statusLabel->setText(
                    QString("Rendered: %1 %2")
                        .arg(kindToString(node.kind))
                        .arg(node.name));
            else
                m_statusLabel->setText(
                    QString("%1 %2  offset: 0x%3  size: %4 bytes")
                        .arg(kindToString(node.kind))
                        .arg(node.name)
                        .arg(node.offset, 4, 16, QChar('0'))
                        .arg(node.byteSize()));
        } else {
            m_statusLabel->setText("Ready");
        }
        // Update rendered view on selection change
        auto it = m_tabs.find(sub);
        if (it != m_tabs.end())
            updateRenderedView(*it);
    });
    connect(ctrl, &RcxController::selectionChanged,
            this, [this](int count) {
        if (count == 0)
            m_statusLabel->setText("Ready");
        else if (count > 1)
            m_statusLabel->setText(QString("%1 nodes selected").arg(count));
    });

    // Update rendered view and workspace on document changes and undo/redo
    connect(doc, &RcxDocument::documentChanged,
            this, [this, sub]() {
        auto it = m_tabs.find(sub);
        if (it != m_tabs.end())
            QTimer::singleShot(0, this, [this, sub]() {
                auto it2 = m_tabs.find(sub);
                if (it2 != m_tabs.end()) updateRenderedView(*it2);
                rebuildWorkspaceModel();
            });
    });
    connect(&doc->undoStack, &QUndoStack::indexChanged,
            this, [this, sub](int) {
        auto it = m_tabs.find(sub);
        if (it != m_tabs.end())
            QTimer::singleShot(0, this, [this, sub]() {
                auto it2 = m_tabs.find(sub);
                if (it2 != m_tabs.end()) updateRenderedView(*it2);
            });
    });

    // Auto-focus on first root struct (don't show all roots)
    for (const auto& n : doc->tree.nodes) {
        if (n.parentId == 0 && n.kind == NodeKind::Struct) {
            ctrl->setViewRootId(n.id);
            break;
        }
    }

    ctrl->refresh();
    rebuildWorkspaceModel();
    return sub;
}

void MainWindow::newFile() {
    project_new();
}

void MainWindow::selfTest() {
    QString demoPath = QCoreApplication::applicationDirPath() + "/demo.rcx";
    if (QFile::exists(demoPath)) {
        project_open(demoPath);
    } else {
        // Create default demo with a single Ball struct
        auto* doc = new RcxDocument(this);
        doc->tree.baseAddress = 0x00400000;

        Node ball;
        ball.kind = NodeKind::Struct;
        ball.name = "aBall";
        ball.structTypeName = "ball";
        ball.parentId = 0;
        ball.offset = 0;
        int bi = doc->tree.addNode(ball);
        uint64_t ballId = doc->tree.nodes[bi].id;

        { Node n; n.kind = NodeKind::Hex64;  n.name = "field_00";   n.parentId = ballId; n.offset = 0;  doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;  n.name = "field_08";   n.parentId = ballId; n.offset = 8;  doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Vec4;   n.name = "position";   n.parentId = ballId; n.offset = 16; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Vec3;   n.name = "velocity";   n.parentId = ballId; n.offset = 32; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex32;  n.name = "field_2C";   n.parentId = ballId; n.offset = 44; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Float;  n.name = "speed";      n.parentId = ballId; n.offset = 48; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::UInt32; n.name = "color";      n.parentId = ballId; n.offset = 52; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Float;  n.name = "radius";     n.parentId = ballId; n.offset = 56; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex32;  n.name = "field_3C";   n.parentId = ballId; n.offset = 60; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Float;  n.name = "mass";       n.parentId = ballId; n.offset = 64; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;  n.name = "field_44";   n.parentId = ballId; n.offset = 68; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Bool;   n.name = "bouncy";     n.parentId = ballId; n.offset = 76; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex8;   n.name = "field_4D";   n.parentId = ballId; n.offset = 77; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex16;  n.name = "field_4E";   n.parentId = ballId; n.offset = 78; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::UInt32; n.name = "color";      n.parentId = ballId; n.offset = 80; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex32;  n.name = "field_54";   n.parentId = ballId; n.offset = 84; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;  n.name = "field_58";   n.parentId = ballId; n.offset = 88; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;  n.name = "field_60";   n.parentId = ballId; n.offset = 96; doc->tree.addNode(n); }

        // Physics struct (defined at root level)
        Node phys;
        phys.kind = NodeKind::Struct;
        phys.name = "aPhysics";
        phys.structTypeName = "Physics";
        phys.parentId = 0;
        phys.offset = 0;
        int pi = doc->tree.addNode(phys);
        uint64_t physId = doc->tree.nodes[pi].id;

        { Node n; n.kind = NodeKind::Hex64; n.name = "field_00"; n.parentId = physId; n.offset = 0;  doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64; n.name = "field_08"; n.parentId = physId; n.offset = 8;  doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64; n.name = "field_10"; n.parentId = physId; n.offset = 16; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64; n.name = "field_18"; n.parentId = physId; n.offset = 24; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64; n.name = "field_20"; n.parentId = physId; n.offset = 32; doc->tree.addNode(n); }

        // Pointer to Physics in ball struct
        { Node n; n.kind = NodeKind::Pointer64; n.name = "physics"; n.parentId = ballId; n.offset = 104; n.refId = physId; n.collapsed = true; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;  n.name = "field_70";   n.parentId = ballId; n.offset = 112; doc->tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;  n.name = "field_78";   n.parentId = ballId; n.offset = 120; doc->tree.addNode(n); }

        doc->save(demoPath);
        doc->load(demoPath);
        createTab(doc);
    }
}

void MainWindow::openFile() {
    project_open();
}

void MainWindow::saveFile() {
    project_save(nullptr, false);
}

void MainWindow::saveFileAs() {
    project_save(nullptr, true);
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

    uint64_t parentId = ctrl->viewRootId();  // default to current view root
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
    QFont f(fontName, 12);
    f.setFixedPitch(true);
    for (auto& state : m_tabs) {
        state.ctrl->setEditorFont(fontName);
        // Also update the rendered view font
        if (state.rendered) {
            state.rendered->setFont(f);
            if (auto* lex = state.rendered->lexer()) {
                lex->setFont(f);
                for (int i = 0; i <= 127; i++)
                    lex->setFont(f, i);
            }
            state.rendered->setMarginsFont(f);
        }
    }
    // Sync workspace tree font
    if (m_workspaceTree)
        m_workspaceTree->setFont(f);
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

// ── Rendered view setup ──

void MainWindow::setupRenderedSci(QsciScintilla* sci) {
    QSettings settings("ReclassX", "ReclassX");
    QString fontName = settings.value("font", "Consolas").toString();
    QFont f(fontName, 12);
    f.setFixedPitch(true);

    sci->setFont(f);
    sci->setReadOnly(true);
    sci->setWrapMode(QsciScintilla::WrapNone);
    sci->setCaretLineVisible(false);
    sci->setPaper(QColor("#1e1e1e"));
    sci->setColor(QColor("#d4d4d4"));
    sci->setTabWidth(4);
    sci->setIndentationsUseTabs(false);
    sci->setCaretForegroundColor(QColor("#d4d4d4"));
    sci->SendScintilla(QsciScintillaBase::SCI_SETEXTRAASCENT, (long)2);
    sci->SendScintilla(QsciScintillaBase::SCI_SETEXTRADESCENT, (long)2);

    // Line number margin
    sci->setMarginType(0, QsciScintilla::NumberMargin);
    sci->setMarginWidth(0, "00000");
    sci->setMarginsBackgroundColor(QColor("#252526"));
    sci->setMarginsForegroundColor(QColor("#858585"));
    sci->setMarginsFont(f);

    // Hide other margins
    sci->setMarginWidth(1, 0);
    sci->setMarginWidth(2, 0);

    // C++ lexer for syntax highlighting
    auto* lexer = new QsciLexerCPP(sci);
    lexer->setFont(f);
    lexer->setColor(QColor("#569cd6"), QsciLexerCPP::Keyword);
    lexer->setColor(QColor("#569cd6"), QsciLexerCPP::KeywordSet2);
    lexer->setColor(QColor("#b5cea8"), QsciLexerCPP::Number);
    lexer->setColor(QColor("#ce9178"), QsciLexerCPP::DoubleQuotedString);
    lexer->setColor(QColor("#ce9178"), QsciLexerCPP::SingleQuotedString);
    lexer->setColor(QColor("#6a9955"), QsciLexerCPP::Comment);
    lexer->setColor(QColor("#6a9955"), QsciLexerCPP::CommentLine);
    lexer->setColor(QColor("#6a9955"), QsciLexerCPP::CommentDoc);
    lexer->setColor(QColor("#d4d4d4"), QsciLexerCPP::Default);
    lexer->setColor(QColor("#d4d4d4"), QsciLexerCPP::Identifier);
    lexer->setColor(QColor("#c586c0"), QsciLexerCPP::PreProcessor);
    lexer->setColor(QColor("#d4d4d4"), QsciLexerCPP::Operator);
    for (int i = 0; i <= 127; i++) {
        lexer->setPaper(QColor("#1e1e1e"), i);
        lexer->setFont(f, i);
    }
    sci->setLexer(lexer);
    sci->setBraceMatching(QsciScintilla::NoBraceMatch);
}

// ── View mode / generator switching ──

void MainWindow::setViewMode(ViewMode mode) {
    auto* tab = activeTab();
    if (!tab) return;
    tab->viewMode = mode;
    if (tab->stack) {
        tab->stack->setCurrentIndex(mode == VM_Rendered ? 1 : 0);
    }
    if (mode == VM_Rendered) {
        updateRenderedView(*tab);
    }
    syncRenderMenuState();
}

void MainWindow::syncRenderMenuState() {
    auto* tab = activeTab();
    bool rendered = tab && tab->viewMode == VM_Rendered;
    if (m_actViewRendered) m_actViewRendered->setEnabled(!rendered);
    if (m_actViewReclass)  m_actViewReclass->setEnabled(rendered);
}

// ── Find the root-level struct ancestor for a node ──

uint64_t MainWindow::findRootStructForNode(const NodeTree& tree, uint64_t nodeId) const {
    QSet<uint64_t> visited;
    uint64_t cur = nodeId;
    uint64_t lastStruct = 0;
    while (cur != 0 && !visited.contains(cur)) {
        visited.insert(cur);
        int idx = tree.indexOfId(cur);
        if (idx < 0) break;
        const Node& n = tree.nodes[idx];
        if (n.kind == NodeKind::Struct)
            lastStruct = n.id;
        if (n.parentId == 0)
            return (n.kind == NodeKind::Struct) ? n.id : lastStruct;
        cur = n.parentId;
    }
    return lastStruct;
}

// ── Update the rendered view for a tab ──

void MainWindow::updateRenderedView(TabState& tab) {
    if (tab.viewMode != VM_Rendered) return;
    if (!tab.rendered) return;

    // Determine which struct to render based on selection
    uint64_t rootId = 0;
    QSet<uint64_t> selIds = tab.ctrl->selectedIds();
    if (selIds.size() >= 1) {
        uint64_t selId = *selIds.begin();
        selId &= ~kFooterIdBit;
        rootId = findRootStructForNode(tab.doc->tree, selId);
    }

    // Generate text
    const QHash<NodeKind, QString>* aliases =
        tab.doc->typeAliases.isEmpty() ? nullptr : &tab.doc->typeAliases;
    QString text;
    if (rootId != 0)
        text = renderCpp(tab.doc->tree, rootId, aliases);
    else
        text = renderCppAll(tab.doc->tree, aliases);

    // Scroll restoration: save if same root, reset if different
    int restoreLine = 0;
    if (rootId != 0 && rootId == tab.lastRenderedRootId) {
        restoreLine = (int)tab.rendered->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
    }
    tab.lastRenderedRootId = rootId;

    // Set text
    tab.rendered->setReadOnly(false);
    tab.rendered->setText(text);
    tab.rendered->setReadOnly(true);

    // Update margin width for line count
    int lineCount = tab.rendered->lines();
    QString marginStr = QString(QString::number(lineCount).size() + 2, '0');
    tab.rendered->setMarginWidth(0, marginStr);

    // Restore scroll
    if (restoreLine > 0) {
        tab.rendered->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                                    (unsigned long)restoreLine);
    }
}

// ── Export C++ header to file ──

void MainWindow::exportCpp() {
    auto* tab = activeTab();
    if (!tab) return;

    QString path = QFileDialog::getSaveFileName(this,
        "Export C++ Header", {}, "C++ Header (*.h);;All Files (*)");
    if (path.isEmpty()) return;

    const QHash<NodeKind, QString>* aliases =
        tab->doc->typeAliases.isEmpty() ? nullptr : &tab->doc->typeAliases;
    QString text = renderCppAll(tab->doc->tree, aliases);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Failed",
            "Could not write to: " + path);
        return;
    }
    file.write(text.toUtf8());
    m_statusLabel->setText("Exported to " + QFileInfo(path).fileName());
}

// ── Type Aliases Dialog ──

void MainWindow::showTypeAliasesDialog() {
    auto* tab = activeTab();
    if (!tab) return;

    QDialog dlg(this);
    dlg.setWindowTitle("Type Aliases");
    dlg.resize(500, 400);

    auto* layout = new QVBoxLayout(&dlg);

    auto* table = new QTableWidget(&dlg);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({"NodeKind", "Alias (C type)"});
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->setSelectionMode(QAbstractItemView::SingleSelection);

    // Populate with all NodeKind entries
    int rowCount = static_cast<int>(std::size(kKindMeta));
    table->setRowCount(rowCount);
    for (int i = 0; i < rowCount; i++) {
        const auto& meta = kKindMeta[i];
        auto* kindItem = new QTableWidgetItem(QString::fromLatin1(meta.name));
        kindItem->setFlags(kindItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(i, 0, kindItem);

        QString alias = tab->doc->typeAliases.value(meta.kind);
        table->setItem(i, 1, new QTableWidgetItem(alias));
    }

    layout->addWidget(table);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    // Collect new aliases
    QHash<NodeKind, QString> newAliases;
    for (int i = 0; i < rowCount; i++) {
        QString val = table->item(i, 1)->text().trimmed();
        if (!val.isEmpty())
            newAliases[kKindMeta[i].kind] = val;
    }

    tab->doc->typeAliases = newAliases;
    tab->doc->modified = true;
    tab->ctrl->refresh();
    updateWindowTitle();
}

// ── Project Lifecycle API ──

QMdiSubWindow* MainWindow::project_new() {
    auto* doc = new RcxDocument(this);

    QByteArray data(16, '\0');
    doc->loadData(data);
    doc->tree.baseAddress = 0x00400000;

    Node root;
    root.kind = NodeKind::Struct;
    root.name = "Entity";
    root.structTypeName = "Entity";
    root.parentId = 0;
    root.offset = 0;
    int ri = doc->tree.addNode(root);
    uint64_t rootId = doc->tree.nodes[ri].id;

    { Node n; n.kind = NodeKind::Int32; n.name = "health"; n.parentId = rootId; n.offset = 0;  doc->tree.addNode(n); }
    { Node n; n.kind = NodeKind::Int32; n.name = "armor";  n.parentId = rootId; n.offset = 4;  doc->tree.addNode(n); }
    { Node n; n.kind = NodeKind::Float; n.name = "speed";  n.parentId = rootId; n.offset = 8;  doc->tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex32; n.name = "flags";  n.parentId = rootId; n.offset = 12; doc->tree.addNode(n); }

    auto* sub = createTab(doc);
    rebuildWorkspaceModel();
    return sub;
}

QMdiSubWindow* MainWindow::project_open(const QString& path) {
    QString filePath = path;
    if (filePath.isEmpty()) {
        filePath = QFileDialog::getOpenFileName(this,
            "Open Definition", {}, "ReclassX (*.rcx);;JSON (*.json);;All (*)");
        if (filePath.isEmpty()) return nullptr;
    }

    auto* doc = new RcxDocument(this);
    if (!doc->load(filePath)) {
        QMessageBox::warning(this, "Error", "Failed to load: " + filePath);
        delete doc;
        return nullptr;
    }
    auto* sub = createTab(doc);
    rebuildWorkspaceModel();
    return sub;
}

bool MainWindow::project_save(QMdiSubWindow* sub, bool saveAs) {
    if (!sub) sub = m_mdiArea->activeSubWindow();
    if (!sub || !m_tabs.contains(sub)) return false;
    auto& tab = m_tabs[sub];

    if (saveAs || tab.doc->filePath.isEmpty()) {
        QString path = QFileDialog::getSaveFileName(this,
            "Save Definition", {}, "ReclassX (*.rcx);;JSON (*.json)");
        if (path.isEmpty()) return false;
        tab.doc->save(path);
    } else {
        tab.doc->save(tab.doc->filePath);
    }
    updateWindowTitle();
    return true;
}

void MainWindow::project_close(QMdiSubWindow* sub) {
    if (!sub) sub = m_mdiArea->activeSubWindow();
    if (!sub) return;
    sub->close();
    rebuildWorkspaceModel();
}

// ── Workspace Dock ──

void MainWindow::createWorkspaceDock() {
    m_workspaceDock = new QDockWidget("Workspace", this);
    m_workspaceDock->setObjectName("WorkspaceDock");
    m_workspaceDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    m_workspaceTree = new QTreeView(m_workspaceDock);
    m_workspaceModel = new QStandardItemModel(this);
    m_workspaceModel->setHorizontalHeaderLabels({"Name"});
    m_workspaceTree->setModel(m_workspaceModel);
    m_workspaceTree->setHeaderHidden(true);
    m_workspaceTree->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Match editor font
    {
        QSettings settings("ReclassX", "ReclassX");
        QString fontName = settings.value("font", "Consolas").toString();
        QFont f(fontName, 12);
        f.setFixedPitch(true);
        m_workspaceTree->setFont(f);
    }

    m_workspaceDock->setWidget(m_workspaceTree);
    addDockWidget(Qt::LeftDockWidgetArea, m_workspaceDock);
    m_workspaceDock->hide();

    connect(m_workspaceTree, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        // Data roles: UserRole=QMdiSubWindow*, UserRole+1=structId, UserRole+2=nodeId
        auto subVar = index.data(Qt::UserRole);
        if (!subVar.isValid()) return;

        auto* sub = static_cast<QMdiSubWindow*>(subVar.value<void*>());
        if (!sub || !m_tabs.contains(sub)) return;

        m_mdiArea->setActiveSubWindow(sub);

        auto structIdVar = index.data(Qt::UserRole + 1);
        auto nodeIdVar   = index.data(Qt::UserRole + 2);

        if (structIdVar.isValid()) {
            // Double-clicked a struct: set as view root
            uint64_t structId = structIdVar.toULongLong();
            auto& tree = m_tabs[sub].doc->tree;
            int ni = tree.indexOfId(structId);
            if (ni >= 0) tree.nodes[ni].collapsed = false;
            m_tabs[sub].ctrl->setViewRootId(structId);
            m_tabs[sub].ctrl->scrollToNodeId(structId);
        } else if (nodeIdVar.isValid()) {
            // Double-clicked a field: find its root struct, set as view root, scroll to field
            uint64_t nodeId = nodeIdVar.toULongLong();
            auto& tree = m_tabs[sub].doc->tree;
            // Walk up to find root struct
            uint64_t rootId = 0;
            uint64_t cur = nodeId;
            while (cur != 0) {
                int idx = tree.indexOfId(cur);
                if (idx < 0) break;
                if (tree.nodes[idx].parentId == 0) { rootId = cur; break; }
                cur = tree.nodes[idx].parentId;
            }
            if (rootId != 0) {
                int ri = tree.indexOfId(rootId);
                if (ri >= 0) tree.nodes[ri].collapsed = false;
                m_tabs[sub].ctrl->setViewRootId(rootId);
            }
            m_tabs[sub].ctrl->scrollToNodeId(nodeId);
        } else if (!index.parent().isValid()) {
            // Double-clicked project root: clear view root to show all
            m_tabs[sub].ctrl->setViewRootId(0);
        }
    });
}

void MainWindow::rebuildWorkspaceModel() {
    m_workspaceModel->clear();

    auto* sub = m_mdiArea->activeSubWindow();
    if (!sub || !m_tabs.contains(sub)) return;

    TabState& tab = m_tabs[sub];
    QString tabName = tab.doc->filePath.isEmpty()
        ? "Untitled" : QFileInfo(tab.doc->filePath).fileName();

    buildWorkspaceModel(m_workspaceModel, tab.doc->tree, tabName,
                        static_cast<void*>(sub));
    m_workspaceTree->expandAll();
}

void MainWindow::showPluginsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle("Plugins");
    dialog.resize(600, 400);

    auto* layout = new QVBoxLayout(&dialog);

    auto* list = new QListWidget();
    layout->addWidget(list);

    auto refreshList = [&]() {
        list->clear();

        // Populate plugin list
        for (IPlugin* plugin : m_pluginManager.plugins()) {
            QString typeStr;
            switch (plugin->Type())
            {
            case IPlugin::ProviderPlugin: typeStr = "Provider"; break;
            default: typeStr = "Unknown"; break;
            }

            QString text = QString("%1 v%2\n  %3\n  Type: %4\n  Author: %5")
                               .arg(QString::fromStdString(plugin->Name()))
                               .arg(QString::fromStdString(plugin->Version()))
                               .arg(QString::fromStdString(plugin->Description()))
                               .arg(typeStr)
                               .arg(QString::fromStdString(plugin->Author()));

            auto* item = new QListWidgetItem(plugin->Icon(), text);
            item->setData(Qt::UserRole, QString::fromStdString(plugin->Name()));
            list->addItem(item);
        }

        if (m_pluginManager.plugins().isEmpty()) {
            list->addItem("No plugins loaded");
        }
    };

    refreshList();

    // Button row
    auto* btnLayout = new QHBoxLayout();

    auto* btnLoad = new QPushButton("Load Plugin...");
    connect(btnLoad, &QPushButton::clicked, [&, refreshList]() {
        QString path = QFileDialog::getOpenFileName(&dialog, "Load Plugin",
                                                    QCoreApplication::applicationDirPath() + "/Plugins",
                                                    "Plugins (*.dll *.so *.dylib);;All Files (*)");

        if (!path.isEmpty()) {
            if (m_pluginManager.LoadPluginFromPath(path)) {
                refreshList();
                m_statusLabel->setText("Plugin loaded successfully");
            } else {
                QMessageBox::warning(&dialog, "Failed to Load Plugin",
                                     "Could not load the selected plugin.\nCheck the console for details.");
            }
        }
    });

    auto* btnUnload = new QPushButton("Unload Selected");
    connect(btnUnload, &QPushButton::clicked, [&, list, refreshList]() {
        auto* item = list->currentItem();
        if (!item) {
            QMessageBox::information(&dialog, "No Selection", "Please select a plugin to unload.");
            return;
        }

        QString pluginName = item->data(Qt::UserRole).toString();
        if (pluginName.isEmpty()) return;

        auto reply = QMessageBox::question(&dialog, "Unload Plugin",
                                           QString("Are you sure you want to unload '%1'?").arg(pluginName),
                                           QMessageBox::Yes | QMessageBox::No);

        if (reply == QMessageBox::Yes) {
            if (m_pluginManager.UnloadPlugin(pluginName)) {
                refreshList();
                m_statusLabel->setText("Plugin unloaded");
            } else {
                QMessageBox::warning(&dialog, "Failed to Unload",
                                     "Could not unload the selected plugin.");
            }
        }
    });

    auto* btnClose = new QPushButton("Close");
    connect(btnClose, &QPushButton::clicked, &dialog, &QDialog::accept);

    btnLayout->addWidget(btnLoad);
    btnLayout->addWidget(btnUnload);
    btnLayout->addStretch();
    btnLayout->addWidget(btnClose);

    layout->addLayout(btnLayout);

    dialog.exec();
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

    // Auto-open demo project from saved .rcx file
    QMetaObject::invokeMethod(&window, "selfTest");

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
