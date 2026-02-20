#include "mainwindow.h"
#include "providerregistry.h"
#include "generator.h"
#include "import_reclass_xml.h"
#include "import_source.h"
#include "export_reclass_xml.h"
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
#include <QWindow>
#include <QMouseEvent>
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

// Guard flag to prevent re-entrant crash inside the handler
static volatile LONG s_inCrashHandler = 0;

static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    // Prevent re-entrant crash: if we fault inside the handler, skip the
    // risky dbghelp work and just terminate with what we already printed.
    if (InterlockedCompareExchange(&s_inCrashHandler, 1, 0) != 0) {
        fprintf(stderr, "\n(re-entrant fault inside crash handler — aborting)\n");
        fflush(stderr);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Phase 1: always-safe output (no allocations, no complex APIs)
    fprintf(stderr, "\n=== UNHANDLED EXCEPTION ===\n");
    fprintf(stderr, "Code : 0x%08lX\n", ep->ExceptionRecord->ExceptionCode);
    fprintf(stderr, "Addr : %p\n", ep->ExceptionRecord->ExceptionAddress);
#ifdef _M_X64
    fprintf(stderr, "RIP  : 0x%016llx\n", (unsigned long long)ep->ContextRecord->Rip);
    fprintf(stderr, "RSP  : 0x%016llx\n", (unsigned long long)ep->ContextRecord->Rsp);
#else
    fprintf(stderr, "EIP  : 0x%08lx\n", (unsigned long)ep->ContextRecord->Eip);
#endif
    fflush(stderr);

    // Phase 2: attempt symbol resolution + stack walk
    // Copy context so StackWalk64 can mutate it safely
    CONTEXT ctxCopy = *ep->ContextRecord;

    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(process, NULL, TRUE)) {
        fprintf(stderr, "\n(SymInitialize failed — no stack trace available)\n");
        fprintf(stderr, "=== END CRASH ===\n");
        fflush(stderr);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    STACKFRAME64 frame = {};
    DWORD machineType;
#ifdef _M_X64
    machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctxCopy.Rip;
    frame.AddrFrame.Offset = ctxCopy.Rbp;
    frame.AddrStack.Offset = ctxCopy.Rsp;
#else
    machineType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = ctxCopy.Eip;
    frame.AddrFrame.Offset = ctxCopy.Ebp;
    frame.AddrStack.Offset = ctxCopy.Esp;
#endif
    frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    fprintf(stderr, "\nStack trace:\n");
    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(machineType, process, thread, &frame, &ctxCopy,
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
        // Kill the status bar item frame and panel border
        if (elem == PE_FrameStatusBarItem || elem == PE_PanelStatusBar)
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
    pal.setColor(QPalette::Mid,             theme.hover);
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
    m_mdiArea->setFrameShape(QFrame::NoFrame);
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

    // Eliminate gap between central widget and status bar
    if (auto* ml = layout()) {
        ml->setSpacing(0);
        ml->setContentsMargins(0, 0, 0, 0);
    }
    // Separator line between central widget and status bar is killed in MenuBarStyle::drawControl

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
                syncViewButtons(tab->panes[i].viewMode);
                return;
            }
        }
    });
}

QIcon MainWindow::makeIcon(const QString& svgPath) {
    return QIcon(svgPath);
}

template < typename...Args >
inline QAction* Qt5Qt6AddAction(QMenu* menu, const QString &text, const QKeySequence &shortcut, const QIcon &icon, Args&&...args)
{
    QAction *result = menu->addAction(icon, text);
    if (!shortcut.isEmpty())
        result->setShortcut(shortcut);
    QObject::connect(result, &QAction::triggered, std::forward<Args>(args)...);
    return result;
}

void MainWindow::createMenus() {
    // File
    auto* file = m_titleBar->menuBar()->addMenu("&File");
    Qt5Qt6AddAction(file, "New &Class",  QKeySequence::New, QIcon(), this, &MainWindow::newClass);
    Qt5Qt6AddAction(file, "New &Struct", QKeySequence(Qt::CTRL | Qt::Key_T), QIcon(), this, &MainWindow::newStruct);
    Qt5Qt6AddAction(file, "New &Enum",   QKeySequence(Qt::CTRL | Qt::Key_E), QIcon(), this, &MainWindow::newEnum);
    Qt5Qt6AddAction(file, "&Open...", QKeySequence::Open, makeIcon(":/vsicons/folder-opened.svg"), this, &MainWindow::openFile);
    file->addSeparator();
    Qt5Qt6AddAction(file, "&Save", QKeySequence::Save, makeIcon(":/vsicons/save.svg"), this, &MainWindow::saveFile);
    Qt5Qt6AddAction(file, "Save &As...", QKeySequence::SaveAs, makeIcon(":/vsicons/save-as.svg"), this, &MainWindow::saveFileAs);
    file->addSeparator();
    m_sourceMenu = file->addMenu("Current Tab So&urce");
    connect(m_sourceMenu, &QMenu::aboutToShow, this, &MainWindow::populateSourceMenu);
    file->addSeparator();
    Qt5Qt6AddAction(file, "&Unload Project", QKeySequence(Qt::CTRL | Qt::Key_W), QIcon(), this, &MainWindow::closeFile);
    file->addSeparator();
    Qt5Qt6AddAction(file, "Export &C++ Header...", QKeySequence::UnknownKey, makeIcon(":/vsicons/export.svg"), this, &MainWindow::exportCpp);
    Qt5Qt6AddAction(file, "Export ReClass &XML...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::exportReclassXmlAction);
    Qt5Qt6AddAction(file, "Import from &Source...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::importFromSource);
    Qt5Qt6AddAction(file, "&Import ReClass XML...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::importReclassXml);
    // Examples submenu — scan once at init
    {
        QDir exDir(QCoreApplication::applicationDirPath() + "/examples");
        QStringList rcxFiles = exDir.entryList({"*.rcx"}, QDir::Files, QDir::Name);
        if (!rcxFiles.isEmpty()) {
            auto* examples = file->addMenu("&Examples");
            for (const QString& fn : rcxFiles) {
                QString fullPath = exDir.absoluteFilePath(fn);
                examples->addAction(fn, this, [this, fullPath]() { project_open(fullPath); });
            }
        }
    }
    file->addSeparator();
    const auto itemName = QSettings("Reclass", "Reclass").value("autoStartMcp", false).toBool() ? "Stop &MCP Server" : "Start &MCP Server";
    m_mcpAction = Qt5Qt6AddAction(file, itemName, QKeySequence::UnknownKey, QIcon(), this, &MainWindow::toggleMcp);
    file->addSeparator();
    Qt5Qt6AddAction(file, "&Options...", QKeySequence::UnknownKey, makeIcon(":/vsicons/settings-gear.svg"), this, &MainWindow::showOptionsDialog);
    file->addSeparator();
    Qt5Qt6AddAction(file, "E&xit", QKeySequence(Qt::Key_Close), makeIcon(":/vsicons/close.svg"), this, &QMainWindow::close);

    // Edit
    auto* edit = m_titleBar->menuBar()->addMenu("&Edit");
    Qt5Qt6AddAction(edit, "&Undo", QKeySequence::Undo, makeIcon(":/vsicons/arrow-left.svg"), this, &MainWindow::undo);
    Qt5Qt6AddAction(edit, "&Redo", QKeySequence::Redo, makeIcon(":/vsicons/arrow-right.svg"), this, &MainWindow::redo);
    edit->addSeparator();
    Qt5Qt6AddAction(edit, "&Type Aliases...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::showTypeAliasesDialog);

    // View
    auto* view = m_titleBar->menuBar()->addMenu("&View");
    Qt5Qt6AddAction(view, "Split &Horizontal", QKeySequence::UnknownKey, makeIcon(":/vsicons/split-horizontal.svg"), this, &MainWindow::splitView);
    Qt5Qt6AddAction(view, "&Unsplit", QKeySequence::UnknownKey, makeIcon(":/vsicons/chrome-close.svg"), this, &MainWindow::unsplitView);
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
    Qt5Qt6AddAction(themeMenu, "Edit Theme...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::editTheme);

    view->addSeparator();
    view->addAction(m_workspaceDock->toggleViewAction());

    // Plugins
    auto* plugins = m_titleBar->menuBar()->addMenu("&Plugins");
    Qt5Qt6AddAction(plugins, "&Manage Plugins...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::showPluginsDialog);

    // Help
    auto* help = m_titleBar->menuBar()->addMenu("&Help");
    Qt5Qt6AddAction(help, "&About Reclass", QKeySequence::UnknownKey, makeIcon(":/vsicons/question.svg"), this, &MainWindow::about);
}

// ── Themed resize grip (replaces ugly default QSizeGrip) ──
// Positioned as a direct child of MainWindow at the bottom-right corner,
// NOT inside the status bar layout (which is font-height dependent).
class ResizeGrip : public QWidget {
public:
    static constexpr int kSize = 16;    // widget size
    static constexpr int kPad  = 4;     // padding from window corner (identical right & bottom)

    explicit ResizeGrip(QWidget* parent) : QWidget(parent) {
        setFixedSize(kSize, kSize);
        setCursor(Qt::SizeFDiagCursor);
        m_color = rcx::ThemeManager::instance().current().textFaint;
    }
    void setGripColor(const QColor& c) { m_color = c; update(); }

    // Call from parent's resizeEvent to pin to bottom-right corner
    void reposition() {
        QWidget* w = parentWidget();
        if (w) move(w->width() - kSize - kPad, w->height() - kSize - kPad);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(m_color);
        // 6 dots in a triangle pointing bottom-right (VS2022 style)
        // Dot grid is centered within the widget: same inset from right and bottom
        const double r = 1.0, s = 4.0;
        const double inset = 4.0;
        double bx = width()  - inset;
        double by = height() - inset;
        // bottom row: 3 dots
        p.drawEllipse(QPointF(bx,         by), r, r);
        p.drawEllipse(QPointF(bx - s,     by), r, r);
        p.drawEllipse(QPointF(bx - 2 * s, by), r, r);
        // middle row: 2 dots
        p.drawEllipse(QPointF(bx,         by - s), r, r);
        p.drawEllipse(QPointF(bx - s,     by - s), r, r);
        // top row: 1 dot
        p.drawEllipse(QPointF(bx,         by - 2 * s), r, r);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            window()->windowHandle()->startSystemResize(Qt::BottomEdge | Qt::RightEdge);
            e->accept();
        }
    }
private:
    QColor m_color;
};

// ── Custom-painted view tab button (no CSS) ──
class ViewTabButton : public QPushButton {
public:
    static constexpr int kAccentH = 2;   // accent line height in pixels
    static constexpr int kPadLR  = 12;   // horizontal padding
    static constexpr int kPadBot = 4;    // extra bottom padding

    QColor colBg, colBgChecked, colBgHover, colBgPressed;
    QColor colText, colTextMuted, colAccent;

    explicit ViewTabButton(const QString& text, QWidget* parent = nullptr)
        : QPushButton(text, parent) {
        setCheckable(true);
        setFlat(true);
        setCursor(Qt::PointingHandCursor);
        setContentsMargins(0, 0, 0, 0);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    }

    QSize sizeHint() const override {
        QFontMetrics fm(font());
        int w = fm.horizontalAdvance(text()) + 2 * kPadLR;
        int h = qRound((fm.height() + kAccentH + kPadBot) * 1.33);
        return QSize(w, h);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        // Background
        QColor bg = colBg;
        if (isDown())          bg = colBgPressed;
        else if (underMouse()) bg = colBgHover;
        else if (isChecked())  bg = colBgChecked;
        p.fillRect(rect(), bg);

        // Accent line at y=0 when checked
        if (isChecked())
            p.fillRect(0, 0, width(), kAccentH, colAccent);

        // Text
        p.setPen(isChecked() || underMouse() || isDown() ? colText : colTextMuted);
        p.setFont(font());
        QRect textRect(kPadLR, kAccentH, width() - 2 * kPadLR, height() - kAccentH);
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text());
    }

    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
};

// ── Borderless status bar with manual child layout ──
// QStatusBarLayout hardcodes 2px margins that can't be overridden.
// We bypass it entirely: children are placed manually in resizeEvent,
// and addWidget() is NOT used. Instead, create children as direct
// children and call manualLayout() to position them.
class FlatStatusBar : public QStatusBar {
public:
    QWidget* tabRow   = nullptr;   // set by createStatusBar
    QLabel*  label    = nullptr;   // set by createStatusBar

    explicit FlatStatusBar(QWidget* parent = nullptr) : QStatusBar(parent) {
        setSizeGripEnabled(false);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().window());
    }
    void resizeEvent(QResizeEvent* e) override {
        QStatusBar::resizeEvent(e);
        manualLayout();
    }
    void showEvent(QShowEvent* e) override {
        QStatusBar::showEvent(e);
        manualLayout();
    }
private:
    void manualLayout() {
        if (!tabRow || !label) return;
        int h = height();
        int tw = tabRow->sizeHint().width();
        tabRow->setGeometry(0, 0, tw, h);
        label->setGeometry(tw, 0, width() - tw, h);
    }
};

void MainWindow::createStatusBar() {
    // Replace the default QStatusBar with our borderless, manually-laid-out one.
    // QStatusBarLayout hardcodes 2px margins; we bypass addWidget entirely.
    auto* sb = new FlatStatusBar;
    setStatusBar(sb);

    m_statusLabel = new QLabel("Ready", sb);
    m_statusLabel->setContentsMargins(10, 0, 0, 0);

    // View toggle buttons (Reclass / C/C++) — custom painted, no CSS
    m_viewBtnGroup = new QButtonGroup(this);
    m_viewBtnGroup->setExclusive(true);

    m_btnReclass = new ViewTabButton("Reclass");
    m_btnReclass->setChecked(true);

    m_btnRendered = new ViewTabButton("C/C++");

    m_viewBtnGroup->addButton(m_btnReclass, 0);
    m_viewBtnGroup->addButton(m_btnRendered, 1);

    // Wrap buttons in a zero-margin container — direct child of status bar
    auto* tabRow = new QWidget(sb);
    auto* tabLay = new QHBoxLayout(tabRow);
    tabLay->setContentsMargins(0, 0, 0, 0);
    tabLay->setSpacing(0);
    tabLay->addWidget(m_btnReclass);
    tabLay->addWidget(m_btnRendered);

    sb->tabRow = tabRow;
    sb->label  = m_statusLabel;

    connect(m_viewBtnGroup, &QButtonGroup::idClicked, this, [this](int id) {
        setViewMode(id == 1 ? VM_Rendered : VM_Reclass);
    });

    // Grip is a direct child of the main window, NOT in the status bar layout.
    // Positioned via reposition() in resizeEvent — immune to font/margin changes.
    auto* grip = new ResizeGrip(this);
    grip->setObjectName("resizeGrip");
    grip->raise();

    {
        const auto& t = ThemeManager::instance().current();
        QPalette sbPal = statusBar()->palette();
        sbPal.setColor(QPalette::Window, t.background);
        sbPal.setColor(QPalette::WindowText, t.textDim);
        statusBar()->setPalette(sbPal);
        statusBar()->setAutoFillBackground(true);

        auto applyViewTabColors = [&](ViewTabButton* btn) {
            btn->colBg        = t.background;
            btn->colBgChecked = t.backgroundAlt;
            btn->colBgHover   = t.hover;
            btn->colBgPressed = t.hover.darker(130);
            btn->colText      = t.text;
            btn->colTextMuted = t.textMuted;
            btn->colAccent    = t.indHoverSpan;
        };
        applyViewTabColors(static_cast<ViewTabButton*>(m_btnReclass));
        applyViewTabColors(static_cast<ViewTabButton*>(m_btnRendered));
    }

    // Sync status bar font with editor font at startup
    {
        QString fontName = QSettings("Reclass", "Reclass").value("font", "JetBrains Mono").toString();
        QFont f(fontName, 12);
        f.setFixedPitch(true);
        statusBar()->setFont(f);
        m_btnReclass->setFont(f);
        m_btnRendered->setFont(f);
    }
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
    pane.tabWidget->tabBar()->setVisible(false);
    pane.tabWidget->setDocumentMode(true);  // kill QTabWidget frame border

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

    // Connect per-pane page switching (driven by status bar buttons via setViewMode)
    QTabWidget* tw = pane.tabWidget;
    connect(tw, &QTabWidget::currentChanged, this, [this, tw](int index) {
        SplitPane* p = findPaneByTabWidget(tw);
        if (!p) return;

        p->viewMode = (index == 1) ? VM_Rendered : VM_Reclass;

        // Sync status bar buttons if this is the active pane
        auto* tab = activeTab();
        if (tab && &tab->panes[tab->activePaneIdx] == p)
            syncViewButtons(p->viewMode);

        if (index == 1) {
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
    splitter->setHandleWidth(1);
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

    // Give every controller the shared document list for cross-tab type visibility
    ctrl->setProjectDocuments(&m_allDocs);
    rebuildAllDocs();

    connect(sub, &QObject::destroyed, this, [this, sub]() {
        auto it = m_tabs.find(sub);
        if (it != m_tabs.end()) {
            it->doc->deleteLater();
            m_tabs.erase(it);
        }
        rebuildAllDocs();
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

// Build a minimal empty struct for new documents
static void buildEmptyStruct(NodeTree& tree, const QString& classKeyword = QString()) {
    Node root;
    root.kind = NodeKind::Struct;
    root.name = "instance";
    root.structTypeName = "Unnamed";
    root.classKeyword = classKeyword;
    root.parentId = 0;
    root.offset = 0;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    for (int i = 0; i < 16; i++) {
        Node n;
        n.kind = NodeKind::Hex64;
        n.name = QStringLiteral("field_%1").arg(i * 8, 2, 16, QChar('0'));
        n.parentId = rootId;
        n.offset = i * 8;
        tree.addNode(n);
    }
}

void MainWindow::newClass() {
    project_new(QStringLiteral("class"));
}

void MainWindow::newStruct() {
    project_new();
}

void MainWindow::newEnum() {
    project_new(QStringLiteral("enum"));
}

static void buildEditorDemo(NodeTree& tree, uintptr_t editorAddr) {
    tree.nodes.clear();
    tree.invalidateIdCache();
    tree.m_nextId = 1;
    tree.baseAddress = static_cast<uint64_t>(editorAddr);

    // ── Root struct: RcxEditor ──
    Node root;
    root.kind = NodeKind::Struct;
    root.name = QStringLiteral("editor");
    root.structTypeName = QStringLiteral("RcxEditor");
    root.classKeyword = QStringLiteral("class");
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    // ── VTable struct definition (separate root) ──
    Node vtStruct;
    vtStruct.kind = NodeKind::Struct;
    vtStruct.name = QStringLiteral("VTable");
    vtStruct.structTypeName = QStringLiteral("QWidgetVTable");
    int vti = tree.addNode(vtStruct);
    uint64_t vtId = tree.nodes[vti].id;

    // VTable entries — these are real virtual function pointers from QObject/QWidget
    static const char* vfNames[] = {
        "deleting_dtor", "metaObject", "qt_metacast", "qt_metacall",
        "event", "eventFilter", "timerEvent", "childEvent",
        "customEvent", "connectNotify", "disconnectNotify", "devType",
        "setVisible", "sizeHint", "minimumSizeHint", "heightForWidth",
    };
    for (int i = 0; i < 16; i++) {
        Node fn;
        fn.kind = NodeKind::FuncPtr64;
        fn.name = QString::fromLatin1(vfNames[i]);
        fn.parentId = vtId;
        fn.offset = i * 8;
        tree.addNode(fn);
    }

    // ── RcxEditor fields ──
    // offset 0: vtable pointer → QWidgetVTable
    {
        Node n;
        n.kind = NodeKind::Pointer64;
        n.name = QStringLiteral("__vptr");
        n.parentId = rootId;
        n.offset = 0;
        n.refId = vtId;
        tree.addNode(n);
    }
    // offset 8: QObjectData* d_ptr (QObject internals)
    {
        Node n;
        n.kind = NodeKind::Pointer64;
        n.name = QStringLiteral("d_ptr");
        n.parentId = rootId;
        n.offset = 8;
        tree.addNode(n);
    }
    // The rest of the object: raw memory visible as Hex64 fields
    // QWidget base is large (~200+ bytes), then RcxEditor members follow.
    // Lay out enough to cover the interesting editor state.
    for (int off = 16; off < 512; off += 8) {
        Node n;
        n.kind = NodeKind::Hex64;
        n.name = QStringLiteral("field_%1").arg(off, 3, 16, QLatin1Char('0'));
        n.parentId = rootId;
        n.offset = off;
        tree.addNode(n);
    }
}

void MainWindow::selfTest() {
#ifdef Q_OS_WIN
    // Create a new project, then point it at the live editor object
    project_new();

    auto* ctrl = activeController();
    if (!ctrl || ctrl->editors().isEmpty()) return;

    auto* editor = ctrl->editors().first();
    auto* doc = ctrl->document();

    // Build a tree describing RcxEditor, based at the real object address
    buildEditorDemo(doc->tree, reinterpret_cast<uintptr_t>(editor));

    // Attach process memory to self — provider base will be set to the editor address
    DWORD pid = GetCurrentProcessId();
    QString target = QString("%1:Reclass.exe").arg(pid);
    ctrl->attachViaPlugin(QStringLiteral("processmemory"), target);
#else
    project_new();
#endif
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

    // Kill the 1px separator line between central widget and status bar
    setStyleSheet("QMainWindow::separator { height: 0px; width: 0px; }");

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
    // View toggle buttons in status bar
    {
        auto applyColors = [&](ViewTabButton* btn) {
            btn->colBg        = theme.background;
            btn->colBgChecked = theme.backgroundAlt;
            btn->colBgHover   = theme.hover;
            btn->colBgPressed = theme.hover.darker(130);
            btn->colText      = theme.text;
            btn->colTextMuted = theme.textMuted;
            btn->colAccent    = theme.indHoverSpan;
            btn->update();
        };
        applyColors(static_cast<ViewTabButton*>(m_btnReclass));
        applyColors(static_cast<ViewTabButton*>(m_btnRendered));
    }
    // Resize grip (direct child of main window, not in status bar)
    if (auto* w = findChild<QWidget*>("resizeGrip"))
        static_cast<ResizeGrip*>(w)->setGripColor(theme.textFaint);

    // Workspace tree: text color matches menu bar
    if (m_workspaceTree) {
        QPalette tp = m_workspaceTree->palette();
        tp.setColor(QPalette::Text, theme.textDim);
        m_workspaceTree->setPalette(tp);
    }

    // Dock titlebar: restyle label + close button
    if (m_dockTitleLabel)
        m_dockTitleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme.textDim.name()));
    if (m_dockCloseBtn)
        m_dockCloseBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: none; padding: 0px 4px 2px 4px; font-size: 12px; }"
            "QToolButton:hover { color: %2; }")
            .arg(theme.textDim.name(), theme.indHoverSpan.name()));

    // Rendered C/C++ views: update lexer colors, paper, margins
    for (auto& tab : m_tabs) {
        for (auto& pane : tab.panes) {
            auto* sci = pane.rendered;
            if (!sci) continue;
            if (auto* lexer = qobject_cast<QsciLexerCPP*>(sci->lexer())) {
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
                for (int i = 0; i <= 127; i++)
                    lexer->setPaper(theme.background, i);
            }
            sci->setPaper(theme.background);
            sci->setColor(theme.text);
            sci->setCaretForegroundColor(theme.text);
            sci->setCaretLineBackgroundColor(theme.hover);
            sci->setSelectionBackgroundColor(theme.selection);
            sci->setSelectionForegroundColor(theme.text);
            sci->setMarginsBackgroundColor(theme.backgroundAlt);
            sci->setMarginsForegroundColor(theme.textDim);
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
    current.refreshMs = QSettings("Reclass", "Reclass").value("refreshMs", 660).toInt();

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

    if (r.refreshMs != current.refreshMs) {
        QSettings("Reclass", "Reclass").setValue("refreshMs", r.refreshMs);
        for (auto& tab : m_tabs)
            tab.ctrl->setRefreshInterval(r.refreshMs);
    }
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
    // Sync dock titlebar font
    if (m_dockTitleLabel)
        m_dockTitleLabel->setFont(f);
    // Sync status bar font
    statusBar()->setFont(f);
    m_btnReclass->setFont(f);
    m_btnRendered->setFont(f);
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
    syncViewButtons(mode);
}

void MainWindow::syncViewButtons(ViewMode mode) {
    QSignalBlocker block(m_viewBtnGroup);
    if (mode == VM_Rendered) m_btnRendered->setChecked(true);
    else                     m_btnReclass->setChecked(true);
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

// ── Export ReClass XML ──

void MainWindow::exportReclassXmlAction() {
    auto* tab = activeTab();
    if (!tab) return;

    QString path = QFileDialog::getSaveFileName(this,
        "Export ReClass XML", {}, "ReClass XML (*.reclass);;All Files (*)");
    if (path.isEmpty()) return;

    QString error;
    if (!rcx::exportReclassXml(tab->doc->tree, path, &error)) {
        QMessageBox::warning(this, "Export Failed",
            error.isEmpty() ? QStringLiteral("Could not export") : error);
        return;
    }

    int classCount = 0;
    for (const auto& n : tab->doc->tree.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) classCount++;

    m_statusLabel->setText(QStringLiteral("Exported %1 classes to %2")
        .arg(classCount).arg(QFileInfo(path).fileName()));
}

// ── Import ReClass XML ──

void MainWindow::importReclassXml() {
    QString filePath = QFileDialog::getOpenFileName(this,
        "Import ReClass XML", {},
        "ReClass XML (*.reclass *.MemeCls *.xml);;All Files (*)");
    if (filePath.isEmpty()) return;

    QString error;
    NodeTree tree = rcx::importReclassXml(filePath, &error);
    if (tree.nodes.isEmpty()) {
        QMessageBox::warning(this, "Import Failed", error.isEmpty()
            ? QStringLiteral("No data found in file") : error);
        return;
    }

    // Count root structs for status message
    int classCount = 0;
    for (const auto& n : tree.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) classCount++;

    auto* doc = new RcxDocument(this);
    doc->tree = std::move(tree);

    m_mdiArea->closeAllSubWindows();
    createTab(doc);
    rebuildWorkspaceModel();
    m_statusLabel->setText(QStringLiteral("Imported %1 classes from %2")
        .arg(classCount).arg(QFileInfo(filePath).fileName()));
}

// ── Import from Source ──

void MainWindow::importFromSource() {
    QDialog dlg(this);
    dlg.setWindowTitle("Import from Source");
    dlg.resize(700, 600);

    auto* layout = new QVBoxLayout(&dlg);

    auto* sci = new QsciScintilla(&dlg);
    setupRenderedSci(sci);
    sci->setReadOnly(false);
    sci->setMarginWidth(0, "00000");
    layout->addWidget(sci);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText("Import");
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    QString source = sci->text();
    if (source.trimmed().isEmpty()) return;

    QString error;
    NodeTree tree = rcx::importFromSource(source, &error);
    if (tree.nodes.isEmpty()) {
        QMessageBox::warning(this, "Import Failed", error.isEmpty()
            ? QStringLiteral("No struct definitions found") : error);
        return;
    }

    int classCount = 0;
    for (const auto& n : tree.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) classCount++;

    auto* doc = new RcxDocument(this);
    doc->tree = std::move(tree);

    m_mdiArea->closeAllSubWindows();
    createTab(doc);
    rebuildWorkspaceModel();
    m_statusLabel->setText(QStringLiteral("Imported %1 classes from source").arg(classCount));
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

QMdiSubWindow* MainWindow::project_new(const QString& classKeyword) {
    auto* doc = new RcxDocument(this);

    QByteArray data(256, '\0');
    doc->loadData(data);
    doc->tree.baseAddress = 0x00400000;

    buildEmptyStruct(doc->tree, classKeyword);

    auto* sub = createTab(doc);
    rebuildWorkspaceModel();
    return sub;
}

QMdiSubWindow* MainWindow::project_open(const QString& path) {
    QString filePath = path;
    if (filePath.isEmpty()) {
        filePath = QFileDialog::getOpenFileName(this,
            "Open Definition", {},
            "All Supported (*.rcx *.json *.reclass *.MemeCls *.xml)"
            ";;Reclass (*.rcx)"
            ";;JSON (*.json)"
            ";;ReClass XML (*.reclass *.MemeCls *.xml)"
            ";;All (*)");
        if (filePath.isEmpty()) return nullptr;
    }

    // Detect if this is an XML-based ReClass file by checking first bytes
    bool isXml = false;
    {
        QFile probe(filePath);
        if (probe.open(QIODevice::ReadOnly)) {
            QByteArray head = probe.read(64);
            isXml = head.trimmed().startsWith("<?xml") || head.trimmed().startsWith("<ReClass")
                    || head.trimmed().startsWith("<MemeCls");
        }
    }

    if (isXml) {
        QString error;
        NodeTree tree = rcx::importReclassXml(filePath, &error);
        if (tree.nodes.isEmpty()) {
            QMessageBox::warning(this, "Import Failed", error.isEmpty()
                ? QStringLiteral("No data found in file") : error);
            return nullptr;
        }
        auto* doc = new RcxDocument(this);
        doc->tree = std::move(tree);
        m_mdiArea->closeAllSubWindows();
        auto* sub = createTab(doc);
        rebuildWorkspaceModel();
        int classCount = 0;
        for (const auto& n : doc->tree.nodes)
            if (n.parentId == 0 && n.kind == NodeKind::Struct) classCount++;
        m_statusLabel->setText(QStringLiteral("Imported %1 classes from %2")
            .arg(classCount).arg(QFileInfo(filePath).fileName()));
        return sub;
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
    m_workspaceDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);

    // Custom titlebar: label + ✕ close button (matches MDI tab style)
    {
        const auto& t = ThemeManager::instance().current();

        auto* titleBar = new QWidget(m_workspaceDock);
        auto* layout = new QHBoxLayout(titleBar);
        layout->setContentsMargins(6, 2, 2, 2);
        layout->setSpacing(0);

        m_dockTitleLabel = new QLabel("Project Tree", titleBar);
        m_dockTitleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(t.textDim.name()));
        {
            QString fontName = QSettings("Reclass", "Reclass").value("font", "JetBrains Mono").toString();
            QFont f(fontName, 12);
            f.setFixedPitch(true);
            m_dockTitleLabel->setFont(f);
        }
        layout->addWidget(m_dockTitleLabel);

        layout->addStretch();

        m_dockCloseBtn = new QToolButton(titleBar);
        m_dockCloseBtn->setText(QStringLiteral("\u2715"));
        m_dockCloseBtn->setAutoRaise(true);
        m_dockCloseBtn->setCursor(Qt::PointingHandCursor);
        m_dockCloseBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: none; padding: 0px 4px 2px 4px; font-size: 12px; }"
            "QToolButton:hover { color: %2; }")
            .arg(t.textDim.name(), t.indHoverSpan.name()));
        connect(m_dockCloseBtn, &QToolButton::clicked, m_workspaceDock, &QDockWidget::close);
        layout->addWidget(m_dockCloseBtn);

        m_workspaceDock->setTitleBarWidget(titleBar);
    }

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

        // Right-click on "Project" group → New Class / New Struct / New Enum
        if (structId == rcx::kGroupSentinel) {
            QMenu menu;
            auto* actClass  = menu.addAction("New Class");
            auto* actStruct = menu.addAction("New Struct");
            auto* actEnum   = menu.addAction("New Enum");
            QAction* chosen = menu.exec(m_workspaceTree->viewport()->mapToGlobal(pos));
            if (chosen == actClass)       newClass();
            else if (chosen == actStruct) newStruct();
            else if (chosen == actEnum)   newEnum();
            return;
        }

        if (structId == 0) return;

        auto subVar = index.data(Qt::UserRole);
        if (!subVar.isValid()) return;
        auto* sub = static_cast<QMdiSubWindow*>(subVar.value<void*>());
        if (!sub || !m_tabs.contains(sub)) return;

        auto& tab = m_tabs[sub];
        int ni = tab.doc->tree.indexOfId(structId);
        if (ni < 0) return;
        QString kw = tab.doc->tree.nodes[ni].resolvedClassKeyword();

        QMenu menu;
        QAction* actConvert = nullptr;
        // class↔struct conversion only (no enum conversion)
        if (kw == QStringLiteral("class"))
            actConvert = menu.addAction("Convert to Struct");
        else if (kw == QStringLiteral("struct"))
            actConvert = menu.addAction("Convert to Class");
        auto* actDelete = menu.addAction(QIcon(":/vsicons/remove.svg"), "Delete");

        QAction* chosen = menu.exec(m_workspaceTree->viewport()->mapToGlobal(pos));
        if (chosen == actDelete) {
            QString typeName = tab.doc->tree.nodes[ni].structTypeName.isEmpty()
                ? tab.doc->tree.nodes[ni].name
                : tab.doc->tree.nodes[ni].structTypeName;
            if (typeName.isEmpty()) typeName = QStringLiteral("(unnamed)");

            // Collect detailed reference info
            QStringList refDetails;
            for (const auto& n : tab.doc->tree.nodes) {
                if (n.refId == structId) {
                    QString ownerName;
                    uint64_t pid = n.parentId;
                    while (pid != 0) {
                        int pi = tab.doc->tree.indexOfId(pid);
                        if (pi < 0) break;
                        if (tab.doc->tree.nodes[pi].parentId == 0) {
                            ownerName = tab.doc->tree.nodes[pi].structTypeName.isEmpty()
                                ? tab.doc->tree.nodes[pi].name
                                : tab.doc->tree.nodes[pi].structTypeName;
                            break;
                        }
                        pid = tab.doc->tree.nodes[pi].parentId;
                    }
                    QString fieldDesc = ownerName.isEmpty()
                        ? n.name
                        : QStringLiteral("%1::%2").arg(ownerName, n.name);
                    refDetails << QStringLiteral("  \u2022 %1 (%2)")
                        .arg(fieldDesc, kindToString(n.kind));
                }
            }

            QString msg;
            if (refDetails.isEmpty()) {
                msg = QString("Delete '%1'?").arg(typeName);
            } else {
                msg = QString("Delete '%1'?\n\n"
                              "The following %2 field(s) reference this type "
                              "and will become untyped (void):\n\n%3")
                    .arg(typeName)
                    .arg(refDetails.size())
                    .arg(refDetails.join('\n'));
            }

            auto answer = QMessageBox::question(this, "Delete Type", msg,
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) return;

            tab.ctrl->deleteRootStruct(structId);
            rebuildWorkspaceModel();
        } else if (chosen && chosen == actConvert) {
            QString newKw = kw == QStringLiteral("class")
                ? QStringLiteral("struct") : QStringLiteral("class");
            QString oldKw = tab.doc->tree.nodes[ni].resolvedClassKeyword();
            tab.doc->undoStack.push(new rcx::RcxCommand(tab.ctrl,
                rcx::cmd::ChangeClassKeyword{structId, oldKw, newKw}));
            rebuildWorkspaceModel();
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

void MainWindow::rebuildAllDocs() {
    m_allDocs.clear();
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it)
        m_allDocs.append(it.value().doc);
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

void MainWindow::populateSourceMenu() {
    m_sourceMenu->clear();
    auto* ctrl = activeController();

    m_sourceMenu->addAction("File", this, [this]() {
        if (auto* c = activeController()) c->selectSource(QStringLiteral("File"));
    });

    const auto& providers = ProviderRegistry::instance().providers();
    for (const auto& prov : providers) {
        QString name = prov.name;
        m_sourceMenu->addAction(name, this, [this, name]() {
            if (auto* c = activeController()) c->selectSource(name);
        });
    }

    if (ctrl && !ctrl->savedSources().isEmpty()) {
        m_sourceMenu->addSeparator();
        for (int i = 0; i < ctrl->savedSources().size(); i++) {
            const auto& e = ctrl->savedSources()[i];
            auto* act = m_sourceMenu->addAction(
                QStringLiteral("%1 '%2'").arg(e.kind, e.displayName),
                this, [this, i]() {
                    if (auto* c = activeController()) c->switchSource(i);
                });
            act->setCheckable(true);
            act->setChecked(i == ctrl->activeSourceIndex());
        }
        m_sourceMenu->addSeparator();
        m_sourceMenu->addAction("Clear All", this, [this]() {
            if (auto* c = activeController()) c->clearSources();
        });
    }
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
    if (auto* w = findChild<QWidget*>("resizeGrip")) {
        auto* grip = static_cast<ResizeGrip*>(w);
        grip->reposition();
        grip->raise();
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

    window.show();

    // Auto-open demo project from saved .rcx file
    QMetaObject::invokeMethod(&window, "selfTest");

    return app.exec();
}

// MainWindow Q_OBJECT is now in mainwindow.h; AUTOMOC handles moc generation.
