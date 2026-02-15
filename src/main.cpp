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
#include "optionsdialog.h"

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
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
#ifdef _WIN32
                setDarkTitleBar(w);
#endif
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
        if (type == CT_MenuItem)
            s = QSize(s.width() + 24, s.height() + 4);
        return s;
    }
    int pixelMetric(PixelMetric metric, const QStyleOption* opt,
                    const QWidget* w) const override {
        // Kill the 1px frame margin Fusion reserves around QMenu contents
        if (metric == PM_MenuPanelWidth)
            return 0;
        return QProxyStyle::pixelMetric(metric, opt, w);
    }
    void drawPrimitive(PrimitiveElement elem, const QStyleOption* opt,
                       QPainter* p, const QWidget* w) const override {
        // Kill Fusion's 3D bevel on QMenu — the OS drop shadow is enough
        if (elem == PE_FrameMenu)
            return;
        QProxyStyle::drawPrimitive(elem, opt, p, w);
    }
    void drawControl(ControlElement element, const QStyleOption* opt,
                     QPainter* p, const QWidget* w) const override {
        // Menu bar items (File, Edit, View…) — direct paint, Fusion ignores palette
        if (element == CE_MenuBarItem) {
            if (auto* mi = qstyleoption_cast<const QStyleOptionMenuItem*>(opt)) {
                if (mi->state & (State_Selected | State_Sunken)) {
                    QStyleOptionMenuItem patched = *mi;
                    patched.state &= ~(State_Selected | State_Sunken);
                    patched.palette.setColor(QPalette::ButtonText,
                        mi->palette.color(QPalette::Link));          // amber text only
                    QProxyStyle::drawControl(element, &patched, p, w);
                    return;
                }
            }
        }
        // Popup menu items — palette patch then delegate to Fusion
        if (element == CE_MenuItem) {
            if (auto* mi = qstyleoption_cast<const QStyleOptionMenuItem*>(opt)) {
                if ((mi->state & State_Selected)
                    && mi->menuItemType != QStyleOptionMenuItem::Separator) {
                    QStyleOptionMenuItem patched = *mi;
                    patched.palette.setColor(QPalette::Highlight,
                        mi->palette.color(QPalette::Mid));           // theme.border
                    patched.palette.setColor(QPalette::HighlightedText,
                        mi->palette.color(QPalette::Link));          // theme.indHoverSpan
                    QProxyStyle::drawControl(element, &patched, p, w);
                    return;
                }
            }
        }
        QProxyStyle::drawControl(element, opt, p, w);
    }
};

static void applyGlobalTheme(const rcx::Theme& theme) {
    QPalette pal;
    pal.setColor(QPalette::Window,          theme.background);
    pal.setColor(QPalette::WindowText,      theme.text);
    pal.setColor(QPalette::Base,            theme.background);
    pal.setColor(QPalette::AlternateBase,   theme.surface);
    pal.setColor(QPalette::Text,            theme.text);
    pal.setColor(QPalette::Button,          theme.button);
    pal.setColor(QPalette::ButtonText,      theme.text);
    pal.setColor(QPalette::Highlight,       theme.selection);
    pal.setColor(QPalette::HighlightedText, theme.text);
    pal.setColor(QPalette::ToolTipBase,     theme.backgroundAlt);
    pal.setColor(QPalette::ToolTipText,     theme.text);
    pal.setColor(QPalette::Mid,             theme.border);
    pal.setColor(QPalette::Dark,            theme.background);
    pal.setColor(QPalette::Light,           theme.textFaint);
    pal.setColor(QPalette::Link,            theme.indHoverSpan);

    // Disabled group: Fusion reads these for disabled menu items, buttons, etc.
    pal.setColor(QPalette::Disabled, QPalette::WindowText,      theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::Text,            theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText,      theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::Light,           theme.background);

    qApp->setPalette(pal);

    qApp->setStyleSheet(QString());
}

class BorderOverlay : public QWidget {
public:
    QColor color;
    explicit BorderOverlay(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setPen(color);
        p.drawRect(0, 0, width() - 1, height() - 1);
    }
};

namespace rcx {

// MainWindow class declaration is in mainwindow.h

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Reclass");
    resize(1200, 800);

    // Frameless window with system menu (Alt+Space) and min/max/close support
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowSystemMenuHint
                   | Qt::WindowMinMaxButtonsHint);

    // Custom title bar (replaces native menu bar area in QMainWindow)
    m_titleBar = new TitleBarWidget(this);
    m_titleBar->applyTheme(ThemeManager::instance().current());
    setMenuWidget(m_titleBar);

#ifdef _WIN32
    // 1px top margin preserves DWM drop shadow on the frameless window
    {
        auto hwnd = reinterpret_cast<HWND>(winId());
        MARGINS margins = {0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
    }
#endif

    // Border overlay — draws a 1px colored border on top of everything
    auto* overlay = new BorderOverlay(this);
    m_borderOverlay = overlay;
    overlay->color = ThemeManager::instance().current().borderFocused;
    overlay->setGeometry(rect());
    overlay->raise();
    overlay->show();

    m_mdiArea = new QMdiArea(this);
    m_mdiArea->setViewMode(QMdiArea::TabbedView);
    m_mdiArea->setTabsClosable(true);
    m_mdiArea->setTabsMovable(true);
    {
        const auto& t = ThemeManager::instance().current();
        m_mdiArea->setStyleSheet(QStringLiteral(
            "QTabBar::tab {"
            "  background: %1; color: %2; padding: 0px 16px; border: none; height: 24px;"
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

    // Restore menu bar title case setting (after menus are created)
    {
        QSettings s("Reclass", "Reclass");
        m_titleBar->setMenuBarTitleCase(s.value("menuBarTitleCase", true).toBool());
        if (s.value("showIcon", false).toBool())
            m_titleBar->setShowIcon(true);
    }

    // MenuBarStyle is set as app style in main() — covers both QMenuBar and QMenu

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &MainWindow::applyTheme);

    // Load plugins
    m_pluginManager.LoadPlugins();

    // Start MCP bridge
    m_mcp = new McpBridge(this, this);
    if (QSettings("Reclass", "Reclass").value("autoStartMcp", false).toBool())
        m_mcp->start();

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
    auto* file = m_titleBar->menuBar()->addMenu("&File");
    file->addAction("&New", QKeySequence::New, this, &MainWindow::newDocument);
    file->addAction("New &Tab", QKeySequence(Qt::CTRL | Qt::Key_T), this, &MainWindow::newFile);
    file->addAction(makeIcon(":/vsicons/folder-opened.svg"), "&Open...", QKeySequence::Open, this, &MainWindow::openFile);
    file->addSeparator();
    file->addAction(makeIcon(":/vsicons/save.svg"), "&Save", QKeySequence::Save, this, &MainWindow::saveFile);
    file->addAction(makeIcon(":/vsicons/save-as.svg"), "Save &As...", QKeySequence::SaveAs, this, &MainWindow::saveFileAs);
    file->addSeparator();
    file->addAction("&Unload Project", QKeySequence(Qt::CTRL | Qt::Key_W), this, &MainWindow::closeFile);
    file->addSeparator();
    file->addAction(makeIcon(":/vsicons/export.svg"), "Export &C++ Header...", this, &MainWindow::exportCpp);
    file->addSeparator();
    m_mcpAction = file->addAction(QSettings("Reclass", "Reclass").value("autoStartMcp", false).toBool() ? "Stop &MCP Server" : "Start &MCP Server", this, &MainWindow::toggleMcp);
    file->addSeparator();
    file->addAction(makeIcon(":/vsicons/settings-gear.svg"), "&Options...", this, &MainWindow::showOptionsDialog);
    file->addSeparator();
    file->addAction(makeIcon(":/vsicons/close.svg"), "E&xit", QKeySequence(Qt::Key_Close), this, &QMainWindow::close);

    // Edit
    auto* edit = m_titleBar->menuBar()->addMenu("&Edit");
    edit->addAction(makeIcon(":/vsicons/arrow-left.svg"), "&Undo", QKeySequence::Undo, this, &MainWindow::undo);
    edit->addAction(makeIcon(":/vsicons/arrow-right.svg"), "&Redo", QKeySequence::Redo, this, &MainWindow::redo);
    edit->addSeparator();
    edit->addAction("&Type Aliases...", this, &MainWindow::showTypeAliasesDialog);

    // View
    auto* view = m_titleBar->menuBar()->addMenu("&View");
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
    QSettings settings("Reclass", "Reclass");
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

    // Plugins
    auto* plugins = m_titleBar->menuBar()->addMenu("&Plugins");
    plugins->addAction("&Manage Plugins...", this, &MainWindow::showPluginsDialog);

    // Help
    auto* help = m_titleBar->menuBar()->addMenu("&Help");
    help->addAction(makeIcon(":/vsicons/question.svg"), "&About Reclass", this, &MainWindow::about);
}

void MainWindow::createStatusBar() {
    m_statusLabel = new QLabel("Ready");
    m_statusLabel->setContentsMargins(10, 0, 0, 0);
    statusBar()->setContentsMargins(0, 4, 0, 4);
    statusBar()->addWidget(m_statusLabel, 1);
    {
        const auto& t = ThemeManager::instance().current();
        QPalette sbPal = statusBar()->palette();
        sbPal.setColor(QPalette::Window, t.background);
        sbPal.setColor(QPalette::WindowText, t.textDim);
        statusBar()->setPalette(sbPal);
        statusBar()->setAutoFillBackground(true);
    }
}

void MainWindow::applyTabWidgetStyle(QTabWidget* tw) {
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

void MainWindow::styleTabCloseButtons() {
    auto* tabBar = m_mdiArea->findChild<QTabBar*>();
    if (!tabBar) return;

    const auto& t = ThemeManager::instance().current();
    QString style = QStringLiteral(
        "QToolButton { color: %1; border: none; padding: 0px 4px 2px 4px; font-size: 12px; }"
        "QToolButton:hover { color: %2; }")
        .arg(t.textDim.name(), t.indHoverSpan.name());

    auto subs = m_mdiArea->subWindowList();
    for (int i = 0; i < tabBar->count() && i < subs.size(); i++) {
        auto* existing = qobject_cast<QToolButton*>(
            tabBar->tabButton(i, QTabBar::RightSide));
        if (existing && existing->text() == QStringLiteral("\u2715")) {
            // Already our button, just restyle
            existing->setStyleSheet(style);
            continue;
        }
        // Replace with ✕ text button
        auto* btn = new QToolButton(tabBar);
        btn->setText(QStringLiteral("\u2715"));
        btn->setAutoRaise(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(style);
        QMdiSubWindow* sub = subs[i];
        connect(btn, &QToolButton::clicked, sub, &QMdiSubWindow::close);
        tabBar->setTabButton(i, QTabBar::RightSide, btn);
    }
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

static QString rootName(const NodeTree& tree, uint64_t viewRootId = 0) {
    if (viewRootId != 0) {
        int idx = tree.indexOfId(viewRootId);
        if (idx >= 0) {
            const auto& n = tree.nodes[idx];
            if (!n.structTypeName.isEmpty()) return n.structTypeName;
            if (!n.name.isEmpty()) return n.name;
        }
    }
    for (const auto& n : tree.nodes) {
        if (n.parentId == 0 && n.kind == NodeKind::Struct) {
            if (!n.structTypeName.isEmpty()) return n.structTypeName;
            if (!n.name.isEmpty()) return n.name;
        }
    }
    return QStringLiteral("Untitled");
}

QMdiSubWindow* MainWindow::createTab(RcxDocument* doc) {
    auto* splitter = new QSplitter(Qt::Horizontal);
    auto* ctrl = new RcxController(doc, splitter);

    auto* sub = m_mdiArea->addSubWindow(splitter);
    sub->setWindowIcon(QIcon());  // suppress app icon in MDI tabs
    sub->setWindowTitle(doc->filePath.isEmpty()
                        ? rootName(doc->tree) : QFileInfo(doc->filePath).fileName());
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
                if (it2 != m_tabs.end()) {
                    updateAllRenderedPanes(*it2);
                    if (it2->doc->filePath.isEmpty())
                        sub->setWindowTitle(rootName(it2->doc->tree, it2->ctrl->viewRootId()));
                }
                rebuildWorkspaceModel();
                updateWindowTitle();
            });
    });
    connect(&doc->undoStack, &QUndoStack::indexChanged,
            this, [this, sub](int) {
        auto it = m_tabs.find(sub);
        if (it != m_tabs.end())
            QTimer::singleShot(0, this, [this, sub]() {
                auto it2 = m_tabs.find(sub);
                if (it2 != m_tabs.end()) {
                    updateAllRenderedPanes(*it2);
                    if (it2->doc->filePath.isEmpty())
                        sub->setWindowTitle(rootName(it2->doc->tree, it2->ctrl->viewRootId()));
                }
                updateWindowTitle();
                rebuildWorkspaceModel();
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
    styleTabCloseButtons();
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

    // float[4] scores at offset 112
    { Node n; n.kind = NodeKind::Array; n.name = "scores"; n.parentId = ballId; n.offset = 112; n.elementKind = NodeKind::Float; n.arrayLen = 4; tree.addNode(n); }

    // Material[2] materials at offset 128 (112 + 16 for float[4])
    { Node n; n.kind = NodeKind::Array; n.name = "materials"; n.parentId = ballId; n.offset = 128; n.elementKind = NodeKind::Struct; n.arrayLen = 2; n.refId = matId; tree.addNode(n); }

    // Unnamed struct (128 bytes of hex64 fields)
    Node unnamed;
    unnamed.kind = NodeKind::Struct;
    unnamed.name = "instance";
    unnamed.structTypeName = "Unnamed";
    unnamed.parentId = 0;
    unnamed.offset = 0;
    int ui = tree.addNode(unnamed);
    uint64_t unnamedId = tree.nodes[ui].id;

    for (int i = 0; i < 16; i++) {
        Node n;
        n.kind = NodeKind::Hex64;
        n.name = QStringLiteral("field_%1").arg(i * 8, 2, 16, QChar('0'));
        n.parentId = unnamedId;
        n.offset = i * 8;
        tree.addNode(n);
    }
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
    if (sub) sub->setWindowTitle(rootName(doc->tree, ctrl->viewRootId()));
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

void MainWindow::closeFile() {
    project_close();
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
    if (primary && primary->isEditing()) return;
    QSet<uint64_t> ids = ctrl->selectedIds();
    QVector<int> indices;
    for (uint64_t id : ids) {
        int idx = ctrl->document()->tree.indexOfId(id & ~kFooterIdBit);
        if (idx >= 0) indices.append(idx);
    }
    if (indices.size() > 1)
        ctrl->batchRemoveNodes(indices);
    else if (indices.size() == 1)
        ctrl->removeNode(indices.first());
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

    {
        QPalette dlgPal = dlg.palette();
        dlgPal.setColor(QPalette::Window, ThemeManager::instance().current().background);
        dlg.setPalette(dlgPal);
        dlg.setAutoFillBackground(true);
    }
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
        m_statusLabel->setText("MCP server listening on pipe: ReclassMcpBridge");
    }
}

void MainWindow::applyTheme(const Theme& theme) {
    applyGlobalTheme(theme);

    // Custom title bar
    m_titleBar->applyTheme(theme);

    // Update border overlay color
    updateBorderColor(isActiveWindow() ? theme.borderFocused : theme.border);

    // MDI area tabs
    m_mdiArea->setStyleSheet(QStringLiteral(
        "QTabBar::tab {"
        "  background: %1; color: %2; padding: 0px 16px; border: none; height: 24px;"
        "}"
        "QTabBar::tab:selected { color: %3; background: %4; }"
        "QTabBar::tab:hover { color: %3; background: %5; }")
        .arg(theme.background.name(), theme.textMuted.name(), theme.text.name(),
             theme.backgroundAlt.name(), theme.hover.name()));

    // Re-style ✕ close buttons on MDI tabs
    styleTabCloseButtons();

    // Status bar
    {
        QPalette sbPal = statusBar()->palette();
        sbPal.setColor(QPalette::Window, theme.background);
        sbPal.setColor(QPalette::WindowText, theme.textDim);
        statusBar()->setPalette(sbPal);
    }

    // Workspace tree: text color matches menu bar
    if (m_workspaceTree) {
        QPalette tp = m_workspaceTree->palette();
        tp.setColor(QPalette::Text, theme.textDim);
        m_workspaceTree->setPalette(tp);
    }

    // Split pane tab widgets
    for (auto& state : m_tabs) {
        for (auto& pane : state.panes) {
            if (pane.tabWidget) applyTabWidgetStyle(pane.tabWidget);
        }
    }
}

void MainWindow::editTheme() {
    auto& tm = ThemeManager::instance();
    int idx = tm.currentIndex();
    ThemeEditor dlg(idx, this);
    if (dlg.exec() == QDialog::Accepted) {
        tm.updateTheme(dlg.selectedIndex(), dlg.result());
    } else {
        tm.revertPreview();
    }
}

// TODO: when adding more and more options, this func becomes very clunky. Fix
void MainWindow::showOptionsDialog() {
    auto& tm = ThemeManager::instance();
    OptionsResult current;
    current.themeIndex = tm.currentIndex();
    current.fontName = QSettings("Reclass", "Reclass").value("font", "JetBrains Mono").toString();
    current.menuBarTitleCase = m_titleBar->menuBarTitleCase();
    current.showIcon = QSettings("Reclass", "Reclass").value("showIcon", false).toBool();
    current.safeMode = QSettings("Reclass", "Reclass").value("safeMode", false).toBool();
    current.autoStartMcp = QSettings("Reclass", "Reclass").value("autoStartMcp", false).toBool();

    OptionsDialog dlg(current, this);
    if (dlg.exec() != QDialog::Accepted) return; // OptionsDialog doesn't apply anything. Only apply on OK

    auto r = dlg.result();

    if (r.themeIndex != current.themeIndex)
        tm.setCurrent(r.themeIndex);

    if (r.fontName != current.fontName)
        setEditorFont(r.fontName);

    if (r.menuBarTitleCase != current.menuBarTitleCase) {
        m_titleBar->setMenuBarTitleCase(r.menuBarTitleCase);
        QSettings("Reclass", "Reclass").setValue("menuBarTitleCase", r.menuBarTitleCase);
    }

    if (r.showIcon != current.showIcon) {
        m_titleBar->setShowIcon(r.showIcon);
        QSettings("Reclass", "Reclass").setValue("showIcon", r.showIcon);
    }

    if (r.safeMode != current.safeMode)
        QSettings("Reclass", "Reclass").setValue("safeMode", r.safeMode);

    if (r.autoStartMcp != current.autoStartMcp)
        QSettings("Reclass", "Reclass").setValue("autoStartMcp", r.autoStartMcp);
}

void MainWindow::setEditorFont(const QString& fontName) {
    QSettings settings("Reclass", "Reclass");
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
    QString title;
    auto* sub = m_mdiArea->activeSubWindow();
    if (sub && m_tabs.contains(sub)) {
        auto& tab = m_tabs[sub];
        QString name = tab.doc->filePath.isEmpty()
                       ? rootName(tab.doc->tree, tab.ctrl->viewRootId())
                       : QFileInfo(tab.doc->filePath).fileName();
        if (tab.doc->modified) name += " *";
        title = name + " - Reclass";
    } else {
        title = "Reclass";
    }
    setWindowTitle(title);
}

// ── Rendered view setup ──

void MainWindow::setupRenderedSci(QsciScintilla* sci) {
    QSettings settings("Reclass", "Reclass");
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
            "Open Definition", {}, "Reclass (*.rcx);;JSON (*.json);;All (*)");
        if (filePath.isEmpty()) return nullptr;
    }

    auto* doc = new RcxDocument(this);
    if (!doc->load(filePath)) {
        QMessageBox::warning(this, "Error", "Failed to load: " + filePath);
        delete doc;
        return nullptr;
    }

    // Close all existing tabs so the project replaces the current state
    m_mdiArea->closeAllSubWindows();

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
            "Save Definition", {}, "Reclass (*.rcx);;JSON (*.json)");
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
    m_workspaceDock = new QDockWidget("Project Tree", this);
    m_workspaceDock->setObjectName("WorkspaceDock");
    m_workspaceDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    m_workspaceTree = new QTreeView(m_workspaceDock);
    m_workspaceModel = new QStandardItemModel(this);
    m_workspaceModel->setHorizontalHeaderLabels({"Name"});
    m_workspaceTree->setModel(m_workspaceModel);
    m_workspaceTree->setHeaderHidden(true);
    m_workspaceTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_workspaceTree->setExpandsOnDoubleClick(false);
    m_workspaceTree->setMouseTracking(true);

    m_workspaceTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_workspaceTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QModelIndex index = m_workspaceTree->indexAt(pos);
        if (!index.isValid()) return;

        auto structIdVar = index.data(Qt::UserRole + 1);
        uint64_t structId = structIdVar.isValid() ? structIdVar.toULongLong() : 0;
        if (structId == 0 || structId == rcx::kGroupSentinel) return;

        auto subVar = index.data(Qt::UserRole);
        if (!subVar.isValid()) return;
        auto* sub = static_cast<QMdiSubWindow*>(subVar.value<void*>());
        if (!sub || !m_tabs.contains(sub)) return;

        QMenu menu;
        auto* deleteAction = menu.addAction(QIcon(":/vsicons/remove.svg"), "Delete");
        if (menu.exec(m_workspaceTree->viewport()->mapToGlobal(pos)) == deleteAction) {
            auto& tab = m_tabs[sub];
            int ni = tab.doc->tree.indexOfId(structId);
            if (ni >= 0) {
                tab.ctrl->removeNode(ni);
                rebuildWorkspaceModel();
            }
        }
    });

    m_workspaceDock->setWidget(m_workspaceTree);
    addDockWidget(Qt::LeftDockWidgetArea, m_workspaceDock);
    m_workspaceDock->hide();

    connect(m_workspaceTree, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        auto structIdVar = index.data(Qt::UserRole + 1);
        uint64_t structId = structIdVar.isValid() ? structIdVar.toULongLong() : 0;

        if (structId == rcx::kGroupSentinel) {
            // "Project" folder: toggle expand/collapse
            m_workspaceTree->setExpanded(index, !m_workspaceTree->isExpanded(index));
            return;
        }

        auto subVar = index.data(Qt::UserRole);
        if (!subVar.isValid()) return;
        auto* sub = static_cast<QMdiSubWindow*>(subVar.value<void*>());
        if (!sub || !m_tabs.contains(sub)) return;

        m_mdiArea->setActiveSubWindow(sub);

        // Type/Enum node: navigate to it
        auto& tree = m_tabs[sub].doc->tree;
        int ni = tree.indexOfId(structId);
        if (ni >= 0) tree.nodes[ni].collapsed = false;
        m_tabs[sub].ctrl->setViewRootId(structId);
        m_tabs[sub].ctrl->scrollToNodeId(structId);
    });
}

void MainWindow::rebuildWorkspaceModel() {
    QVector<rcx::TabInfo> tabs;
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        TabState& tab = it.value();
        QString name = tab.doc->filePath.isEmpty()
            ? rootName(tab.doc->tree, tab.ctrl->viewRootId())
            : QFileInfo(tab.doc->filePath).fileName();
        tabs.append({ &tab.doc->tree, name, static_cast<void*>(it.key()) });
    }
    rcx::buildProjectExplorer(m_workspaceModel, tabs);
    m_workspaceTree->expandToDepth(1);
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

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange) {
        const auto& t = ThemeManager::instance().current();
        updateBorderColor(isActiveWindow() ? t.borderFocused : t.border);
    }
    if (event->type() == QEvent::WindowStateChange)
        m_titleBar->updateMaximizeIcon();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (m_borderOverlay) {
        m_borderOverlay->setGeometry(rect());
        m_borderOverlay->raise();
    }
}

void MainWindow::updateBorderColor(const QColor& color) {
    static_cast<BorderOverlay*>(m_borderOverlay)->color = color;
    m_borderOverlay->update();
}

} // namespace rcx

// ── Entry point ──

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#endif

    DarkApp app(argc, argv);
    app.setApplicationName("Reclass");
    app.setOrganizationName("Reclass");
    app.setStyle(new MenuBarStyle("Fusion")); // Fusion + generous menu sizing

    // Load embedded fonts
    int fontId = QFontDatabase::addApplicationFont(":/fonts/JetBrainsMono.ttf");
    if (fontId == -1)
        qWarning("Failed to load embedded JetBrains Mono font");
    // Apply saved font preference before creating any editors
    {
        QSettings settings("Reclass", "Reclass");
        QString savedFont = settings.value("font", "JetBrains Mono").toString();
        rcx::RcxEditor::setGlobalFontName(savedFont);
    }

    // Global theme
    applyGlobalTheme(rcx::ThemeManager::instance().current());

    rcx::MainWindow window;
    window.setWindowIcon(QIcon(":/icons/class.png"));

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
