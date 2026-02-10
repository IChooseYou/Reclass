#include "mainwindow.h"
#include "generator.h"
#include "mcp/mcp_bridge.h"
#include <QApplication>
#include <QMainWindow>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QSplitter>
#include <QTabWidget>
#include <QTabBar>
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
#include <QProxyStyle>
#include <QDesktopServices>
#include "themes/thememanager.h"
#include "themes/themeeditor.h"

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <dbghelp.h>
#include <cstdio>

static void setDarkTitleBar(QWidget* widget) {
    // Requires Windows 10 1809+ (build 17763)
    auto hwnd = reinterpret_cast<HWND>(widget->winId());
    BOOL dark = TRUE;
    // Attribute 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (build 18985+), 19 for older
    DWORD attr = 20;
    if (FAILED(DwmSetWindowAttribute(hwnd, attr, &dark, sizeof(dark)))) {
        attr = 19;
        DwmSetWindowAttribute(hwnd, attr, &dark, sizeof(dark));
    }
}

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

class DarkApp : public QApplication {
public:
    using QApplication::QApplication;
    bool notify(QObject* receiver, QEvent* event) override {
        if (event->type() == QEvent::WindowActivate && receiver->isWidgetType()) {
            auto* w = static_cast<QWidget*>(receiver);
            if ((w->windowFlags() & Qt::Window) == Qt::Window
                && !w->property("DarkTitleBar").toBool()) {
                w->setProperty("DarkTitleBar", true);
                setDarkTitleBar(w);
            }
        }
        return QApplication::notify(receiver, event);
    }
};

class MenuBarStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    QSize sizeFromContents(ContentsType type, const QStyleOption* opt,
                           const QSize& sz, const QWidget* w) const override {
        QSize s = QProxyStyle::sizeFromContents(type, opt, sz, w);
        if (type == CT_MenuBarItem)
            s.setHeight(s.height() + qRound(s.height() * 0.5));
        return s;
    }
};

static void applyGlobalTheme(const rcx::Theme& theme) {
    QPalette pal;
    pal.setColor(QPalette::Window,          theme.background);
    pal.setColor(QPalette::WindowText,      theme.text);
    pal.setColor(QPalette::Base,            theme.backgroundAlt);
    pal.setColor(QPalette::AlternateBase,   theme.surface);
    pal.setColor(QPalette::Text,            theme.text);
    pal.setColor(QPalette::Button,          theme.button);
    pal.setColor(QPalette::ButtonText,      theme.text);
    pal.setColor(QPalette::Highlight,       theme.hover);
    pal.setColor(QPalette::HighlightedText, theme.text);
    pal.setColor(QPalette::ToolTipBase,     theme.backgroundAlt);
    pal.setColor(QPalette::ToolTipText,     theme.text);
    pal.setColor(QPalette::Mid,             theme.border);
    pal.setColor(QPalette::Dark,            theme.background);
    pal.setColor(QPalette::Light,           theme.textFaint);
    qApp->setPalette(pal);

    qApp->setStyleSheet(QStringLiteral(
        "QMenu {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  padding: 4px 6px;"
        "}"
        "QMenu::item { padding: 4px 24px; }"
        "QMenu::item:selected { background-color: %4; }"
        "QMenu::separator { height: 1px; background: %3; margin: 4px 8px; }"
        "QMenu::item:disabled { color: %5; }"
        "QToolTip {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "}")
        .arg(theme.backgroundAlt.name(), theme.text.name(), theme.border.name(),
             theme.hover.name(), theme.textMuted.name()));
}

namespace rcx {

// MainWindow class declaration is in mainwindow.h

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Reclass");
    resize(1200, 800);

    m_mdiArea = new QMdiArea(this);
    m_mdiArea->setViewMode(QMdiArea::TabbedView);
    m_mdiArea->setTabsClosable(true);
    m_mdiArea->setTabsMovable(true);
    {
        const auto& t = ThemeManager::instance().current();
        m_mdiArea->setStyleSheet(QStringLiteral(
            "QTabBar::tab {"
            "  background: %1; color: %2; padding: 6px 16px; border: none;"
            "}"
            "QTabBar::tab:selected { color: %3; background: %4; }"
            "QTabBar::tab:hover { color: %3; background: %5; }")
            .arg(t.background.name(), t.textMuted.name(), t.text.name(),
                 t.backgroundAlt.name(), t.hover.name()));
    }
    setCentralWidget(m_mdiArea);

    createWorkspaceDock();
    createMenus();
    createStatusBar();


    // Larger click targets on menu bar
    {
        menuBar()->setStyle(new MenuBarStyle(menuBar()->style()));
    }

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &MainWindow::applyTheme);

    // Load plugins
    m_pluginManager.LoadPlugins();

    // MCP bridge (stopped by default — user starts via File → Start MCP)
    m_mcp = new McpBridge(this, this);

    connect(m_mdiArea, &QMdiArea::subWindowActivated,
            this, [this](QMdiSubWindow*) {
        updateWindowTitle();
        rebuildWorkspaceModel();
    });

    // Track which split pane has focus (for menu-driven view switching)
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (!now) return;
        auto* tab = activeTab();
        if (!tab) return;
        for (int i = 0; i < tab->panes.size(); ++i) {
            if (tab->panes[i].tabWidget && tab->panes[i].tabWidget->isAncestorOf(now)) {
                tab->activePaneIdx = i;
                return;
            }
        }
    });
}

QIcon MainWindow::makeIcon(const QString& svgPath) {
    return QIcon(svgPath);
}

void MainWindow::createMenus() {
    // File
    auto* file = menuBar()->addMenu("&File");
    file->addAction("&New", this, &MainWindow::newDocument, QKeySequence::New);
    file->addAction("New &Tab", this, &MainWindow::newFile, QKeySequence(Qt::CTRL | Qt::Key_T));
    file->addAction(makeIcon(":/vsicons/folder-opened.svg"), "&Open...", this, &MainWindow::openFile, QKeySequence::Open);
    file->addSeparator();
    file->addAction(makeIcon(":/vsicons/save.svg"), "&Save", this, &MainWindow::saveFile, QKeySequence::Save);
    file->addAction(makeIcon(":/vsicons/save-as.svg"), "Save &As...", this, &MainWindow::saveFileAs, QKeySequence::SaveAs);
    file->addSeparator();
    file->addAction(makeIcon(":/vsicons/export.svg"), "Export &C++ Header...", this, &MainWindow::exportCpp);
    file->addSeparator();
    m_mcpAction = file->addAction("Start &MCP Server", this, &MainWindow::toggleMcp);
    file->addSeparator();
    file->addAction(makeIcon(":/vsicons/close.svg"), "E&xit", this, &QMainWindow::close, QKeySequence(Qt::Key_Close));

    // Edit
    auto* edit = menuBar()->addMenu("&Edit");
    edit->addAction(makeIcon(":/vsicons/arrow-left.svg"), "&Undo", this, &MainWindow::undo, QKeySequence::Undo);
    edit->addAction(makeIcon(":/vsicons/arrow-right.svg"), "&Redo", this, &MainWindow::redo, QKeySequence::Redo);
    edit->addSeparator();
    edit->addAction("&Type Aliases...", this, &MainWindow::showTypeAliasesDialog);

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
    auto* actJetBrains = fontMenu->addAction("JetBrains Mono");
    actJetBrains->setCheckable(true);
    actJetBrains->setActionGroup(fontGroup);
    // Load saved preference
    QSettings settings("ReclassX", "ReclassX");
    QString savedFont = settings.value("font", "JetBrains Mono").toString();
    if (savedFont == "JetBrains Mono") actJetBrains->setChecked(true);
    else actConsolas->setChecked(true);
    connect(actConsolas, &QAction::triggered, this, [this]() { setEditorFont("Consolas"); });
    connect(actJetBrains, &QAction::triggered, this, [this]() { setEditorFont("JetBrains Mono"); });

    // Theme submenu
    auto* themeMenu = view->addMenu("&Theme");
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    auto& tm = ThemeManager::instance();
    auto allThemes = tm.themes();
    for (int i = 0; i < allThemes.size(); i++) {
        auto* act = themeMenu->addAction(allThemes[i].name);
        act->setCheckable(true);
        act->setActionGroup(themeGroup);
        if (i == tm.currentIndex()) act->setChecked(true);
        connect(act, &QAction::triggered, this, [i]() {
            ThemeManager::instance().setCurrent(i);
        });
    }
    themeMenu->addSeparator();
    themeMenu->addAction("Edit Theme...", this, &MainWindow::editTheme);

    view->addSeparator();
    view->addAction(m_workspaceDock->toggleViewAction());

    // Node
    auto* node = menuBar()->addMenu("&Node");
    node->addAction(makeIcon(":/vsicons/add.svg"), "&Add Field", this, &MainWindow::addNode, QKeySequence(Qt::Key_Insert));
    node->addAction(makeIcon(":/vsicons/remove.svg"), "&Remove Field", this, &MainWindow::removeNode, QKeySequence::Delete);
    node->addAction(makeIcon(":/vsicons/symbol-structure.svg"), "Change &Type", this, &MainWindow::changeNodeType, QKeySequence(Qt::Key_T));
    node->addAction(makeIcon(":/vsicons/edit.svg"), "Re&name", this, &MainWindow::renameNodeAction, QKeySequence(Qt::Key_F2));
    node->addAction(makeIcon(":/vsicons/files.svg"), "D&uplicate", this, &MainWindow::duplicateNodeAction)->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));

    // Plugins
    auto* plugins = menuBar()->addMenu("&Plugins");
    plugins->addAction("&Manage Plugins...", this, &MainWindow::showPluginsDialog);

    // Help
    auto* help = menuBar()->addMenu("&Help");
    help->addAction(makeIcon(":/vsicons/question.svg"), "&About Reclass", this, &MainWindow::about);
}

void MainWindow::createStatusBar() {
    m_statusLabel = new QLabel("Ready");
    m_statusLabel->setContentsMargins(10, 0, 0, 0);
    statusBar()->addWidget(m_statusLabel, 1);
    {
        const auto& t = ThemeManager::instance().current();
        statusBar()->setStyleSheet(QStringLiteral(
            "QStatusBar { background: %1; color: %2; }")
            .arg(t.backgroundAlt.name(), t.textDim.name()));
    }

    QSettings settings("ReclassX", "ReclassX");
    QString fontName = settings.value("font", "JetBrains Mono").toString();
    QFont f(fontName, 12);
    f.setFixedPitch(true);
    statusBar()->setFont(f);
}

void MainWindow::applyTabWidgetStyle(QTabWidget* tw) {
    QSettings settings("ReclassX", "ReclassX");
    QString fontName = settings.value("font", "JetBrains Mono").toString();
    QFont tabFont(fontName, 12);
    tabFont.setFixedPitch(true);
    tw->tabBar()->setFont(tabFont);
    const auto& t = ThemeManager::instance().current();
    tw->setStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: none; }"
        "QTabBar::tab {"
        "  background: %1; color: %2; padding: 4px 12px; border: none; min-width: 60px;"
        "}"
        "QTabBar::tab:selected { color: %3; }"
        "QTabBar::tab:hover { color: %3; background: %4; }")
        .arg(t.background.name(), t.textMuted.name(),
             t.text.name(), t.hover.name()));
    tw->tabBar()->setExpanding(false);
}

MainWindow::SplitPane MainWindow::createSplitPane(TabState& tab) {
    SplitPane pane;

    pane.tabWidget = new QTabWidget;
    pane.tabWidget->setTabPosition(QTabWidget::South);
    applyTabWidgetStyle(pane.tabWidget);

    // Create editor via controller (parent = tabWidget for ownership)
    pane.editor = tab.ctrl->addSplitEditor(pane.tabWidget);
    pane.tabWidget->addTab(pane.editor, "Reclass");     // index 0

    // Create per-pane rendered C++ view
    pane.rendered = new QsciScintilla;
    setupRenderedSci(pane.rendered);
    pane.tabWidget->addTab(pane.rendered, "C/C++");     // index 1

    pane.tabWidget->setCurrentIndex(0);
    pane.viewMode = VM_Reclass;

    // Add to splitter
    tab.splitter->addWidget(pane.tabWidget);

    // Connect per-pane tab bar switching
    QTabWidget* tw = pane.tabWidget;
    connect(tw, &QTabWidget::currentChanged, this, [this, tw](int index) {
        // Find which pane this QTabWidget belongs to
        SplitPane* p = findPaneByTabWidget(tw);
        if (!p) return;

        if (index == 1) p->viewMode = VM_Rendered;
        else            p->viewMode = VM_Reclass;

        if (index == 1) {
            // Find the TabState that owns this pane and update rendered view
            for (auto& tab : m_tabs) {
                for (auto& pane : tab.panes) {
                    if (&pane == p) {
                        updateRenderedView(tab, pane);
                        break;
                    }
                }
            }
        }
    });

    return pane;
}

MainWindow::SplitPane* MainWindow::findPaneByTabWidget(QTabWidget* tw) {
    for (auto& tab : m_tabs) {
        for (auto& pane : tab.panes) {
            if (pane.tabWidget == tw)
                return &pane;
        }
    }
    return nullptr;
}

MainWindow::SplitPane* MainWindow::findActiveSplitPane() {
    auto* tab = activeTab();
    if (!tab || tab->panes.isEmpty()) return nullptr;
    int idx = qBound(0, tab->activePaneIdx, tab->panes.size() - 1);
    return &tab->panes[idx];
}

RcxEditor* MainWindow::activePaneEditor() {
    auto* pane = findActiveSplitPane();
    return pane ? pane->editor : nullptr;
}

QMdiSubWindow* MainWindow::createTab(RcxDocument* doc) {
    auto* splitter = new QSplitter(Qt::Horizontal);
    auto* ctrl = new RcxController(doc, splitter);

    auto* sub = m_mdiArea->addSubWindow(splitter);
    sub->setWindowTitle(doc->filePath.isEmpty()
                        ? "Untitled" : QFileInfo(doc->filePath).fileName());
    sub->setAttribute(Qt::WA_DeleteOnClose);
    sub->showMaximized();

    m_tabs[sub] = { doc, ctrl, splitter, {}, 0 };
    auto& tab = m_tabs[sub];

    // Create the initial split pane
    tab.panes.append(createSplitPane(tab));

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
            auto* ap = findActiveSplitPane();
            if (ap && ap->viewMode == VM_Rendered)
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
        // Update all rendered panes on selection change
        auto it = m_tabs.find(sub);
        if (it != m_tabs.end())
            updateAllRenderedPanes(*it);
    });
    connect(ctrl, &RcxController::selectionChanged,
            this, [this](int count) {
        if (count == 0)
            m_statusLabel->setText("Ready");
        else if (count > 1)
            m_statusLabel->setText(QString("%1 nodes selected").arg(count));
    });

    // Update rendered panes and workspace on document changes and undo/redo
    connect(doc, &RcxDocument::documentChanged,
            this, [this, sub]() {
        auto it = m_tabs.find(sub);
        if (it != m_tabs.end())
            QTimer::singleShot(0, this, [this, sub]() {
                auto it2 = m_tabs.find(sub);
                if (it2 != m_tabs.end()) updateAllRenderedPanes(*it2);
                rebuildWorkspaceModel();
            });
    });
    connect(&doc->undoStack, &QUndoStack::indexChanged,
            this, [this, sub](int) {
        auto it = m_tabs.find(sub);
        if (it != m_tabs.end())
            QTimer::singleShot(0, this, [this, sub]() {
                auto it2 = m_tabs.find(sub);
                if (it2 != m_tabs.end()) updateAllRenderedPanes(*it2);
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

// Build Ball + Material demo structs into a tree
static void buildBallDemo(NodeTree& tree) {
    // Ball struct (128 bytes = 0x80)
    Node ball;
    ball.kind = NodeKind::Struct;
    ball.name = "aBall";
    ball.structTypeName = "Ball";
    ball.parentId = 0;
    ball.offset = 0;
    int bi = tree.addNode(ball);
    uint64_t ballId = tree.nodes[bi].id;

    { Node n; n.kind = NodeKind::Hex64;  n.name = "field_00";   n.parentId = ballId; n.offset = 0;  tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex64;  n.name = "field_08";   n.parentId = ballId; n.offset = 8;  tree.addNode(n); }
    { Node n; n.kind = NodeKind::Vec4;   n.name = "position";   n.parentId = ballId; n.offset = 16; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Vec3;   n.name = "velocity";   n.parentId = ballId; n.offset = 32; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex32;  n.name = "field_2C";   n.parentId = ballId; n.offset = 44; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Float;  n.name = "speed";      n.parentId = ballId; n.offset = 48; tree.addNode(n); }
    { Node n; n.kind = NodeKind::UInt32; n.name = "color";      n.parentId = ballId; n.offset = 52; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Float;  n.name = "radius";     n.parentId = ballId; n.offset = 56; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex32;  n.name = "field_3C";   n.parentId = ballId; n.offset = 60; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Float;  n.name = "mass";       n.parentId = ballId; n.offset = 64; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex64;  n.name = "field_44";   n.parentId = ballId; n.offset = 68; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Bool;   n.name = "bouncy";     n.parentId = ballId; n.offset = 76; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex8;   n.name = "field_4D";   n.parentId = ballId; n.offset = 77; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex16;  n.name = "field_4E";   n.parentId = ballId; n.offset = 78; tree.addNode(n); }
    { Node n; n.kind = NodeKind::UInt32; n.name = "color";      n.parentId = ballId; n.offset = 80; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex32;  n.name = "field_54";   n.parentId = ballId; n.offset = 84; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex64;  n.name = "field_58";   n.parentId = ballId; n.offset = 88; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex64;  n.name = "field_60";   n.parentId = ballId; n.offset = 96; tree.addNode(n); }

    // Material struct (renamed from Physics, 40 bytes = 0x28)
    Node mat;
    mat.kind = NodeKind::Struct;
    mat.name = "aMaterial";
    mat.structTypeName = "Material";
    mat.parentId = 0;
    mat.offset = 0;
    int mi = tree.addNode(mat);
    uint64_t matId = tree.nodes[mi].id;

    { Node n; n.kind = NodeKind::Hex64; n.name = "field_00"; n.parentId = matId; n.offset = 0;  tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex64; n.name = "field_08"; n.parentId = matId; n.offset = 8;  tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex64; n.name = "field_10"; n.parentId = matId; n.offset = 16; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex64; n.name = "field_18"; n.parentId = matId; n.offset = 24; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex64; n.name = "field_20"; n.parentId = matId; n.offset = 32; tree.addNode(n); }

    // Pointer to Material in Ball struct
    { Node n; n.kind = NodeKind::Pointer64; n.name = "material"; n.parentId = ballId; n.offset = 104; n.refId = matId; n.collapsed = true; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex64;  n.name = "field_70";   n.parentId = ballId; n.offset = 112; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Hex64;  n.name = "field_78";   n.parentId = ballId; n.offset = 120; tree.addNode(n); }
}

void MainWindow::newFile() {
    project_new();
}

void MainWindow::newDocument() {
    auto* tab = activeTab();
    if (!tab) {
        project_new();
        return;
    }
    auto* doc = tab->doc;
    auto* ctrl = tab->ctrl;

    // Clear everything
    doc->undoStack.clear();
    doc->tree = NodeTree();
    doc->tree.baseAddress = 0x00400000;
    doc->filePath.clear();
    doc->typeAliases.clear();
    doc->modified = false;

    // Build Ball + Material structs
    buildBallDemo(doc->tree);

    // Cross-platform writable buffer, zeroed (256 bytes covers Ball + spare)
    QByteArray data(256, '\0');
    doc->provider = std::make_shared<BufferProvider>(data);

    // Focus on Ball struct
    ctrl->setViewRootId(0);
    for (const auto& n : doc->tree.nodes) {
        if (n.parentId == 0 && n.kind == NodeKind::Struct) {
            ctrl->setViewRootId(n.id);
            break;
        }
    }
    ctrl->clearSelection();
    emit doc->documentChanged();

    auto* sub = m_mdiArea->activeSubWindow();
    if (sub) sub->setWindowTitle("Untitled");
    updateWindowTitle();
    rebuildWorkspaceModel();
}

void MainWindow::selfTest() {
    project_new();
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

void MainWindow::addNode() {
    auto* ctrl = activeController();
    if (!ctrl) return;

    uint64_t parentId = ctrl->viewRootId();  // default to current view root
    auto* primary = activePaneEditor();
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
    auto* primary = activePaneEditor();
    if (!primary || primary->isEditing()) return;
    QSet<int> indices = primary->selectedNodeIndices();
    if (indices.size() > 1) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        ctrl->batchRemoveNodes(indices.values());
#else
        ctrl->batchRemoveNodes(indices.values().toVector());
#endif
    } else if (indices.size() == 1) {
        ctrl->removeNode(*indices.begin());
    }
}

void MainWindow::changeNodeType() {
    auto* ctrl = activeController();
    if (!ctrl) return;
    auto* primary = activePaneEditor();
    if (!primary) return;
    primary->beginInlineEdit(EditTarget::Type);
}

void MainWindow::renameNodeAction() {
    auto* ctrl = activeController();
    if (!ctrl) return;
    auto* primary = activePaneEditor();
    if (!primary) return;
    primary->beginInlineEdit(EditTarget::Name);
}

void MainWindow::duplicateNodeAction() {
    auto* ctrl = activeController();
    if (!ctrl) return;
    auto* primary = activePaneEditor();
    if (!primary || primary->isEditing()) return;
    int ni = primary->currentNodeIndex();
    if (ni >= 0) ctrl->duplicateNode(ni);
}

void MainWindow::splitView() {
    auto* tab = activeTab();
    if (!tab) return;
    tab->panes.append(createSplitPane(*tab));
}

void MainWindow::unsplitView() {
    auto* tab = activeTab();
    if (!tab || tab->panes.size() <= 1) return;
    auto pane = tab->panes.takeLast();
    tab->ctrl->removeSplitEditor(pane.editor);
    pane.tabWidget->deleteLater();
    tab->activePaneIdx = qBound(0, tab->activePaneIdx, tab->panes.size() - 1);
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
    QDialog dlg(this);
    dlg.setWindowTitle("About Reclass");
    dlg.setFixedSize(260, 120);
    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(12);

    auto* buildLabel = new QLabel(
        QStringLiteral("<span style='color:%1;font-size:11px;'>"
                       "Build&ensp;" __DATE__ "&ensp;" __TIME__ "</span>")
            .arg(ThemeManager::instance().current().textDim.name()));
    buildLabel->setAlignment(Qt::AlignCenter);
    lay->addWidget(buildLabel);

    auto* ghBtn = new QPushButton("GitHub");
    ghBtn->setCursor(Qt::PointingHandCursor);
    {
        const auto& t = ThemeManager::instance().current();
        ghBtn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: %1; color: %2; border: 1px solid %3;"
            "  border-radius: 4px; padding: 5px 16px; font-size: 12px;"
            "}"
            "QPushButton:hover { background: %4; border-color: %5; }")
            .arg(t.indCmdPill.name(), t.text.name(), t.border.name(),
                 t.button.name(), t.textFaint.name()));
    }
    connect(ghBtn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/IChooseYou/Reclass"));
    });
    lay->addWidget(ghBtn, 0, Qt::AlignCenter);

    dlg.setStyleSheet(QStringLiteral("QDialog { background: %1; }")
        .arg(ThemeManager::instance().current().background.name()));
    dlg.exec();
}

void MainWindow::toggleMcp() {
    if (m_mcp->isRunning()) {
        m_mcp->stop();
        m_mcpAction->setText("Start &MCP Server");
        m_statusLabel->setText("MCP server stopped");
    } else {
        m_mcp->start();
        m_mcpAction->setText("Stop &MCP Server");
        m_statusLabel->setText("MCP server listening on pipe: rcx-mcp");
    }
}

void MainWindow::applyTheme(const Theme& theme) {
    applyGlobalTheme(theme);

    // MDI area tabs
    m_mdiArea->setStyleSheet(QStringLiteral(
        "QTabBar::tab {"
        "  background: %1; color: %2; padding: 6px 16px; border: none;"
        "}"
        "QTabBar::tab:selected { color: %3; background: %4; }"
        "QTabBar::tab:hover { color: %3; background: %5; }")
        .arg(theme.background.name(), theme.textMuted.name(), theme.text.name(),
             theme.backgroundAlt.name(), theme.hover.name()));

    // Status bar
    statusBar()->setStyleSheet(QStringLiteral(
        "QStatusBar { background: %1; color: %2; }")
        .arg(theme.backgroundAlt.name(), theme.textDim.name()));

    // Split pane tab widgets
    for (auto& state : m_tabs) {
        for (auto& pane : state.panes) {
            if (pane.tabWidget) applyTabWidgetStyle(pane.tabWidget);
        }
    }
}

void MainWindow::editTheme() {
    auto& tm = ThemeManager::instance();
    Theme edited = tm.current();
    ThemeEditor dlg(edited, this);
    if (dlg.exec() == QDialog::Accepted) {
        edited = dlg.result();
        int idx = tm.currentIndex();
        if (idx < tm.themes().size() && idx >= 0) {
            tm.updateTheme(idx, edited);
        }
    }
}

void MainWindow::setEditorFont(const QString& fontName) {
    QSettings settings("ReclassX", "ReclassX");
    settings.setValue("font", fontName);
    QFont f(fontName, 12);
    f.setFixedPitch(true);
    for (auto& state : m_tabs) {
        state.ctrl->setEditorFont(fontName);
        for (auto& pane : state.panes) {
            // Update rendered view font
            if (pane.rendered) {
                pane.rendered->setFont(f);
                if (auto* lex = pane.rendered->lexer()) {
                    lex->setFont(f);
                    for (int i = 0; i <= 127; i++)
                        lex->setFont(f, i);
                }
                pane.rendered->setMarginsFont(f);
            }
            // Update per-pane tab bar font
            if (pane.tabWidget)
                applyTabWidgetStyle(pane.tabWidget);
        }
    }
    // Sync workspace tree font
    if (m_workspaceTree)
        m_workspaceTree->setFont(f);
    // Sync status bar font
    statusBar()->setFont(f);
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

MainWindow::TabState* MainWindow::tabByIndex(int index) {
    auto subs = m_mdiArea->subWindowList();
    if (index < 0 || index >= subs.size()) return nullptr;
    auto* sub = subs[index];
    if (m_tabs.contains(sub))
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
        setWindowTitle(name + " - Reclass");
    } else {
        setWindowTitle("Reclass");
    }
}

// ── Rendered view setup ──

void MainWindow::setupRenderedSci(QsciScintilla* sci) {
    QSettings settings("ReclassX", "ReclassX");
    QString fontName = settings.value("font", "JetBrains Mono").toString();
    QFont f(fontName, 12);
    f.setFixedPitch(true);

    sci->setFont(f);
    sci->setReadOnly(false);
    sci->setWrapMode(QsciScintilla::WrapNone);
    sci->setTabWidth(4);
    sci->setIndentationsUseTabs(false);
    sci->SendScintilla(QsciScintillaBase::SCI_SETEXTRAASCENT, (long)2);
    sci->SendScintilla(QsciScintillaBase::SCI_SETEXTRADESCENT, (long)2);

    // Line number margin
    sci->setMarginType(0, QsciScintilla::NumberMargin);
    sci->setMarginWidth(0, "00000");
    const auto& theme = ThemeManager::instance().current();
    sci->setMarginsBackgroundColor(theme.backgroundAlt);
    sci->setMarginsForegroundColor(theme.textDim);
    sci->setMarginsFont(f);

    // Hide other margins
    sci->setMarginWidth(1, 0);
    sci->setMarginWidth(2, 0);

    // C++ lexer for syntax highlighting — must be set BEFORE colors below,
    // because setLexer() resets caret line, selection, and paper colors.
    auto* lexer = new QsciLexerCPP(sci);
    lexer->setFont(f);
    lexer->setColor(theme.syntaxKeyword, QsciLexerCPP::Keyword);
    lexer->setColor(theme.syntaxKeyword, QsciLexerCPP::KeywordSet2);
    lexer->setColor(theme.syntaxNumber, QsciLexerCPP::Number);
    lexer->setColor(theme.syntaxString, QsciLexerCPP::DoubleQuotedString);
    lexer->setColor(theme.syntaxString, QsciLexerCPP::SingleQuotedString);
    lexer->setColor(theme.syntaxComment, QsciLexerCPP::Comment);
    lexer->setColor(theme.syntaxComment, QsciLexerCPP::CommentLine);
    lexer->setColor(theme.syntaxComment, QsciLexerCPP::CommentDoc);
    lexer->setColor(theme.text, QsciLexerCPP::Default);
    lexer->setColor(theme.text, QsciLexerCPP::Identifier);
    lexer->setColor(theme.syntaxPreproc, QsciLexerCPP::PreProcessor);
    lexer->setColor(theme.text, QsciLexerCPP::Operator);
    for (int i = 0; i <= 127; i++) {
        lexer->setPaper(theme.background, i);
        lexer->setFont(f, i);
    }
    sci->setLexer(lexer);
    sci->setBraceMatching(QsciScintilla::NoBraceMatch);

    // Colors applied AFTER setLexer() — the lexer resets these on attach
    sci->setPaper(theme.background);
    sci->setColor(theme.text);
    sci->setCaretForegroundColor(theme.text);
    sci->setCaretLineVisible(true);
    sci->setCaretLineBackgroundColor(theme.hover);
    sci->setSelectionBackgroundColor(theme.selection);
    sci->setSelectionForegroundColor(theme.text);
}

// ── View mode / generator switching ──

void MainWindow::setViewMode(ViewMode mode) {
    auto* pane = findActiveSplitPane();
    if (!pane) return;
    pane->viewMode = mode;
    int idx = (mode == VM_Rendered) ? 1 : 0;
    pane->tabWidget->setCurrentIndex(idx);
    // The QTabWidget::currentChanged signal will handle updating the rendered view
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

// ── Update the rendered view for a single pane ──

void MainWindow::updateRenderedView(TabState& tab, SplitPane& pane) {
    if (pane.viewMode != VM_Rendered) return;
    if (!pane.rendered) return;

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
    if (rootId != 0 && rootId == pane.lastRenderedRootId) {
        restoreLine = (int)pane.rendered->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
    }
    pane.lastRenderedRootId = rootId;

    // Set text
    pane.rendered->setText(text);

    // Update margin width for line count
    int lineCount = pane.rendered->lines();
    QString marginStr = QString(QString::number(lineCount).size() + 2, '0');
    pane.rendered->setMarginWidth(0, marginStr);

    // Restore scroll
    if (restoreLine > 0) {
        pane.rendered->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                                     (unsigned long)restoreLine);
    }
}

void MainWindow::updateAllRenderedPanes(TabState& tab) {
    for (auto& pane : tab.panes) {
        if (pane.viewMode == VM_Rendered)
            updateRenderedView(tab, pane);
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

    // Cross-platform writable buffer, zeroed (256 bytes covers Ball struct + spare)
    QByteArray data(256, '\0');
    doc->loadData(data);
    doc->tree.baseAddress = 0x00400000;

    // Build Ball + Material demo structs
    buildBallDemo(doc->tree);

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
        QString fontName = settings.value("font", "JetBrains Mono").toString();
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

    DarkApp app(argc, argv);
    app.setApplicationName("ReclassX");
    app.setOrganizationName("ReclassX");
    app.setStyle("Fusion"); // Fusion style respects dark palette well

    // Load embedded fonts
    int fontId = QFontDatabase::addApplicationFont(":/fonts/JetBrainsMono.ttf");
    if (fontId == -1)
        qWarning("Failed to load embedded JetBrains Mono font");
    // Apply saved font preference before creating any editors
    {
        QSettings settings("ReclassX", "ReclassX");
        QString savedFont = settings.value("font", "JetBrains Mono").toString();
        rcx::RcxEditor::setGlobalFontName(savedFont);
    }

    // Global theme
    applyGlobalTheme(rcx::ThemeManager::instance().current());

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
            ::_Exit(0);  // immediate exit — no need for clean shutdown in screenshot mode
        });
    }

    return app.exec();
}

// MainWindow Q_OBJECT is now in mainwindow.h; AUTOMOC handles moc generation.
