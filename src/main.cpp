#include "mainwindow.h"
#include "providerregistry.h"
#include "generator.h"
#include "imports/import_reclass_xml.h"
#include "imports/import_source.h"
#include "imports/export_reclass_xml.h"
#include "imports/import_pdb.h"
#include "imports/import_pdb_dialog.h"
#include "mcp/mcp_bridge.h"
#include <QApplication>
#include <QMainWindow>
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
#include <QProgressDialog>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexercpp.h>
#include <QProxyStyle>
#include <QDesktopServices>
#include <QClipboard>
#include <QGuiApplication>
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

    // Phase 1.5: write a full minidump next to the executable
    {
        // Build dump path: <exe_dir>/reclass_crash_<YYYYMMDD_HHMMSS>.dmp
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        // Strip exe filename to get directory
        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) *(lastSlash + 1) = L'\0';

        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t dumpPath[MAX_PATH];
        _snwprintf_s(dumpPath, MAX_PATH,
                   L"%sreclass_crash_%04d%02d%02d_%02d%02d%02d.dmp",
                   exePath, st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);

        HANDLE hFile = CreateFileW(dumpPath, GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId          = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers    = FALSE;

            // MiniDumpWithFullMemory: captures entire process address space
            // so we can inspect all heap objects, Qt state, node trees, etc.
            BOOL ok = MiniDumpWriteDump(
                GetCurrentProcess(), GetCurrentProcessId(), hFile,
                static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory
                                          | MiniDumpWithHandleData
                                          | MiniDumpWithThreadInfo
                                          | MiniDumpWithUnloadedModules),
                &mei, NULL, NULL);
            CloseHandle(hFile);

            if (ok) {
                fprintf(stderr, "Dump : %ls\n", dumpPath);
            } else {
                fprintf(stderr, "Dump : FAILED (error %lu)\n", GetLastError());
            }
        } else {
            fprintf(stderr, "Dump : could not create file (error %lu)\n", GetLastError());
        }
        fflush(stderr);
    }

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
    void polish(QWidget* w) override {
        if (qobject_cast<QMenu*>(w)) {
            w->setWindowFlag(Qt::FramelessWindowHint, true);
            // Layered window — gives full pixel control; DWM won't clip edges.
            // (The DwmSetWindowAttribute conflict noted in RcxTooltip doesn't
            //  apply here: DarkApp::notify only fires on WindowActivate, which
            //  popups never receive.)
            w->setAttribute(Qt::WA_TranslucentBackground);
        }
        QProxyStyle::polish(w);
    }
    using QProxyStyle::polish;
    QSize sizeFromContents(ContentsType type, const QStyleOption* opt,
                           const QSize& sz, const QWidget* w) const override {
        QSize s = QProxyStyle::sizeFromContents(type, opt, sz, w);
        if (type == CT_MenuBarItem)
            s.setHeight(s.height() + qRound(s.height() * 0.5));
        if (type == CT_MenuItem)
            s = QSize(s.width() + 24, s.height() + 4);
        if (type == CT_ItemViewItem)
            s.setHeight(s.height() + 4);
        // Dock tab bar: fixed height, reasonable padding
        if (type == CT_TabBarTab) {
            if (auto* tabBar = qobject_cast<const QTabBar*>(w)) {
                if (tabBar->parent() && qobject_cast<const QMainWindow*>(tabBar->parent())) {
                    s.setHeight(28);
                }
            }
        }
        return s;
    }
    int pixelMetric(PixelMetric metric, const QStyleOption* opt,
                    const QWidget* w) const override {
        // 1px border drawn in PE_FrameMenu
        if (metric == PM_MenuPanelWidth)
            return 1;
        // Inset menu items from border so hover rect doesn't touch edges
        if (metric == PM_MenuHMargin)
            return 3;
        // Thin draggable separator between dock widgets / central widget
        if (metric == PM_DockWidgetSeparatorExtent)
            return 1;
        return QProxyStyle::pixelMetric(metric, opt, w);
    }
    void drawPrimitive(PrimitiveElement elem, const QStyleOption* opt,
                       QPainter* p, const QWidget* w) const override {
        // Opaque fill + 1px border at the true widget edge.
        // WA_TranslucentBackground (set in polish) makes this a layered window,
        // so DWM doesn't clip any edges.
        if (elem == PE_FrameMenu) {
            QRect r = opt->rect;
            p->fillRect(r, opt->palette.color(QPalette::Window));
            p->setPen(opt->palette.color(QPalette::Dark));
            int x2 = r.right(), y2 = r.bottom();
            p->drawLine(r.left(), r.top(), x2, r.top());     // top
            p->drawLine(r.left(), y2,      x2, y2);           // bottom
            p->drawLine(r.left(), r.top(), r.left(), y2);     // left
            p->drawLine(x2,       r.top(), x2, y2);           // right
            return;
        }
        // Kill the status bar item frame and panel border
        if (elem == PE_FrameStatusBarItem || elem == PE_PanelStatusBar)
            return;
        // Transparent menu bar background (no CSS needed)
        if (elem == PE_PanelMenuBar)
            return;
        // Item-view row background — patch Highlight so the row bg matches CE_ItemViewItem
        if (elem == PE_PanelItemViewRow) {
            if (auto* vi = qstyleoption_cast<const QStyleOptionViewItem*>(opt)) {
                QStyleOptionViewItem patched = *vi;
                patched.palette.setColor(QPalette::Highlight,
                    vi->palette.color(QPalette::Mid));
                QProxyStyle::drawPrimitive(elem, &patched, p, w);
                return;
            }
        }
        QProxyStyle::drawPrimitive(elem, opt, p, w);
    }
    void drawControl(ControlElement element, const QStyleOption* opt,
                     QPainter* p, const QWidget* w) const override {
        // Suppress Fusion's CE_MenuBarEmptyArea — it fills with palette.window()
        // bypassing PE_PanelMenuBar.  TitleBarWidget paints the background.
        if (element == CE_MenuBarEmptyArea)
            return;
        // Menu bar items — fully owned painting (Fusion fills full rect, hiding border)
        if (element == CE_MenuBarItem) {
            if (auto* mi = qstyleoption_cast<const QStyleOptionMenuItem*>(opt)) {
                QRect area = mi->rect.adjusted(0, 0, 0, -1); // leave 1px for border
                bool selected = mi->state & State_Selected;
                bool sunken   = mi->state & State_Sunken;

                // Only fill background for hover/pressed — non-hovered stays
                // transparent so the parent's border line shows through.
                if (sunken)
                    p->fillRect(area, mi->palette.color(QPalette::Highlight).darker(130));
                else if (selected)
                    p->fillRect(area, mi->palette.color(QPalette::Highlight));

                QColor fg = (selected || sunken)
                    ? mi->palette.color(QPalette::Link)
                    : mi->palette.color(QPalette::ButtonText);
                p->setPen(fg);
                p->drawText(area, Qt::AlignCenter | Qt::TextShowMnemonic, mi->text);
                return; // never delegate to Fusion
            }
        }
        // Popup menu items
        if (element == CE_MenuItem) {
            if (auto* mi = qstyleoption_cast<const QStyleOptionMenuItem*>(opt)) {
                // Subtle separator — single line using surface color
                if (mi->menuItemType == QStyleOptionMenuItem::Separator) {
                    int y = mi->rect.center().y();
                    p->setPen(mi->palette.color(QPalette::AlternateBase));
                    p->drawLine(mi->rect.left() + 4, y, mi->rect.right() - 4, y);
                    return;
                }
                // Hover highlight — flat fill (no Fusion border) then delegate
                // for text/icon/arrow with Selected cleared
                if ((mi->state & State_Selected)) {
                    p->fillRect(mi->rect, mi->palette.color(QPalette::Highlight));
                    QStyleOptionMenuItem patched = *mi;
                    patched.state &= ~State_Selected;
                    patched.palette.setColor(QPalette::Text,
                        mi->palette.color(QPalette::Link));          // theme.indHoverSpan
                    QProxyStyle::drawControl(element, &patched, p, w);
                    return;
                }
            }
        }
        // Item views — visible hover + themed selection (Fusion's hover is invisible on dark bg)
        if (element == CE_ItemViewItem) {
            if (auto* vi = qstyleoption_cast<const QStyleOptionViewItem*>(opt)) {
                bool hovered  = vi->state & State_MouseOver;
                bool selected = vi->state & State_Selected;
                if (hovered && !selected)
                    p->fillRect(vi->rect, vi->palette.color(QPalette::Mid));
                QStyleOptionViewItem patched = *vi;
                patched.palette.setColor(QPalette::Highlight,
                    vi->palette.color(QPalette::Mid));               // theme.hover
                patched.palette.setColor(QPalette::HighlightedText,
                    vi->palette.color(QPalette::Text));
                QProxyStyle::drawControl(element, &patched, p, w);
                return;
            }
        }
        // Dock tab bar shape — background, accent line, hover, borders
        // (No stylesheet on dock tab bars — we handle it all here)
        if (element == CE_TabBarTabShape) {
            if (auto* tab = qstyleoption_cast<const QStyleOptionTab*>(opt)) {
                auto* tabBar = qobject_cast<const QTabBar*>(w);
                if (tabBar && tabBar->parent() && qobject_cast<QMainWindow*>(tabBar->parent())) {
                    bool selected = tab->state & State_Selected;
                    bool hovered  = tab->state & State_MouseOver;
                    // Background
                    QColor bg = tab->palette.color(QPalette::Window);      // theme.background
                    if (hovered && !selected)
                        bg = tab->palette.color(QPalette::Mid);            // theme.hover
                    p->fillRect(tab->rect, bg);
                    // Selected accent line on top (2px)
                    if (selected) {
                        p->fillRect(QRect(tab->rect.left(), tab->rect.top(),
                                          tab->rect.width(), 2),
                                    tab->palette.color(QPalette::Link));   // theme.indHoverSpan
                    }
                    // Bottom border (1px separator between tabs and content)
                    p->setPen(tab->palette.color(QPalette::Dark));         // theme.border
                    p->drawLine(tab->rect.bottomLeft(), tab->rect.bottomRight());
                    return;
                }
            }
        }
        // Dock tab bar label — middle-elide long names and use editor font
        if (element == CE_TabBarTabLabel) {
            if (auto* tab = qstyleoption_cast<const QStyleOptionTab*>(opt)) {
                // Only apply to dock tab bars (parent is QMainWindow)
                auto* tabBar = qobject_cast<const QTabBar*>(w);
                if (tabBar && tabBar->parent() && qobject_cast<QMainWindow*>(tabBar->parent())) {
                    // Find tab index for this rect
                    int tabIdx = -1;
                    for (int i = 0; i < tabBar->count(); ++i) {
                        if (tabBar->tabRect(i).contains(tab->rect.center())) {
                            tabIdx = i;
                            break;
                        }
                    }
                    // Leave space for pin+close buttons on right
                    int btnWidth = 0;
                    if (tabIdx >= 0) {
                        auto* btn = tabBar->tabButton(tabIdx, QTabBar::RightSide);
                        if (btn) btnWidth = btn->sizeHint().width() + 4;
                    }
                    QRect textRect = tab->rect.adjusted(8, 0, -(8 + btnWidth), 0);

                    // Use editor font from settings
                    QSettings s("Reclass", "Reclass");
                    QFont f(s.value("font", "JetBrains Mono").toString(), 10);
                    f.setFixedPitch(true);
                    p->setFont(f);

                    QFontMetrics fm(f);
                    // Get original (un-elided) text from the tab bar
                    QString text = (tabIdx >= 0) ? tabBar->tabText(tabIdx) : tab->text;
                    int maxW = textRect.width();

                    // Middle-elide if too long
                    if (fm.horizontalAdvance(text) > maxW) {
                        int ellipsisW = fm.horizontalAdvance(QStringLiteral("\u2026"));
                        int avail = maxW - ellipsisW;
                        if (avail > 0) {
                            int half = avail / 2;
                            QString left, right;
                            for (int i = 0; i < text.size(); ++i) {
                                if (fm.horizontalAdvance(text.left(i + 1)) > half) {
                                    left = text.left(i);
                                    break;
                                }
                            }
                            if (left.isEmpty()) left = text.left(1);
                            for (int i = text.size() - 1; i >= 0; --i) {
                                if (fm.horizontalAdvance(text.mid(i)) > half) {
                                    right = text.mid(i + 1);
                                    break;
                                }
                            }
                            if (right.isEmpty()) right = text.right(1);
                            text = left + QStringLiteral("\u2026") + right;
                        } else {
                            text = QStringLiteral("\u2026");
                        }
                    }

                    bool selected = tab->state & State_Selected;
                    QColor fg = selected ? tab->palette.color(QPalette::Text)
                                         : tab->palette.color(QPalette::WindowText);
                    p->setPen(fg);
                    p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
                    return;
                }
            }
        }
        QProxyStyle::drawControl(element, opt, p, w);
    }
};

#include "dock_tab_buttons.h"

static void applyGlobalTheme(const rcx::Theme& theme) {
    QPalette pal;
    pal.setColor(QPalette::Window,          theme.background);
    pal.setColor(QPalette::WindowText,      theme.text);
    pal.setColor(QPalette::Base,            theme.background);
    pal.setColor(QPalette::AlternateBase,   theme.surface);
    pal.setColor(QPalette::Text,            theme.text);
    pal.setColor(QPalette::Button,          theme.button);
    pal.setColor(QPalette::ButtonText,      theme.text);
    pal.setColor(QPalette::Highlight,       theme.selected);
    pal.setColor(QPalette::HighlightedText, theme.text);
    pal.setColor(QPalette::ToolTipBase,     theme.backgroundAlt);
    pal.setColor(QPalette::ToolTipText,     theme.text);
    pal.setColor(QPalette::Mid,             theme.hover);
    pal.setColor(QPalette::Dark,            theme.border);
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

#ifdef __APPLE__
void applyMacTitleBarTheme(QWidget* window, const Theme& theme);
#endif

// MainWindow class declaration is in mainwindow.h

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Reclass");
    resize(1200, 800);

#ifndef __APPLE__
    // Frameless window with system menu (Alt+Space) and min/max/close support.
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowSystemMenuHint
                   | Qt::WindowMinMaxButtonsHint);

    // Custom title bar (replaces native menu bar area in QMainWindow)
    m_titleBar = new TitleBarWidget(this);
    m_titleBar->applyTheme(ThemeManager::instance().current());
    setMenuWidget(m_titleBar);
    m_menuBar = m_titleBar->menuBar();
#else
    setWindowTitle(QStringLiteral("Reclass"));
    setUnifiedTitleAndToolBarOnMac(true);
    m_menuBar = menuBar();
    m_menuBar->setNativeMenuBar(true);
    applyMacTitleBarTheme(this, ThemeManager::instance().current());
#endif

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

    // Central placeholder — will be replaced by start page after construction
    m_centralPlaceholder = new QWidget(this);
    m_centralPlaceholder->setFixedSize(0, 0);
    m_centralPlaceholder->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setCentralWidget(m_centralPlaceholder);
    setDockNestingEnabled(true);
    // Give left/right docks full height (corners belong to left/right, not top/bottom)
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    setTabPosition(Qt::TopDockWidgetArea, QTabWidget::North);

    createWorkspaceDock();
    createScannerDock();
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
        m_menuBarTitleCase = s.value("menuBarTitleCase", false).toBool();
        applyMenuBarTitleCase(m_menuBarTitleCase);
        if (m_titleBar && s.value("showIcon", false).toBool())
            m_titleBar->setShowIcon(true);
    }

    // MenuBarStyle is set as app style in main() — covers both QMenuBar and QMenu

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &MainWindow::applyTheme);

    // Apply theme once at startup (the signal only fires on change, not initial load)
    applyTheme(ThemeManager::instance().current());

    // Load plugins
    m_pluginManager.LoadPlugins();

    // Start MCP bridge
    m_mcp = new McpBridge(this, this);
    if (QSettings("Reclass", "Reclass").value("autoStartMcp", true).toBool())
        m_mcp->start();

    // Active doc tracking is handled per dock in createTab() via visibilityChanged

    // Ensure border overlay is on top after initial layout settles
    QTimer::singleShot(0, this, [this]() {
        if (m_borderOverlay) {
            m_borderOverlay->setGeometry(rect());
            m_borderOverlay->raise();
        }
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

void MainWindow::applyMenuBarTitleCase(bool titleCase) {
    m_menuBarTitleCase = titleCase;
    if (m_titleBar) {
        m_titleBar->setMenuBarTitleCase(titleCase);
        return;
    }
    if (!m_menuBar) return;

    for (QAction* action : m_menuBar->actions()) {
        QString text = action->text();
        QString clean = text;
        clean.remove('&');

        if (titleCase) {
            action->setText("&" + clean.toUpper());
        } else {
            QString result;
            bool capitalizeNext = true;
            for (int i = 0; i < clean.length(); ++i) {
                QChar ch = clean[i];
                if (ch.isLetter()) {
                    result += capitalizeNext ? ch.toUpper() : ch.toLower();
                    capitalizeNext = false;
                } else {
                    result += ch;
                    if (ch.isSpace()) capitalizeNext = true;
                }
            }
            action->setText("&" + result);
        }
    }
}

void MainWindow::createMenus() {
    // File
    auto* file = m_menuBar->addMenu("&File");
    Qt5Qt6AddAction(file, "New &Class",  QKeySequence::New, QIcon(), this, &MainWindow::newClass);
    Qt5Qt6AddAction(file, "New &Struct", QKeySequence(Qt::CTRL | Qt::Key_T), QIcon(), this, &MainWindow::newStruct);
    Qt5Qt6AddAction(file, "New &Enum",   QKeySequence(Qt::CTRL | Qt::Key_E), QIcon(), this, &MainWindow::newEnum);
    Qt5Qt6AddAction(file, "&Open...", QKeySequence::Open, makeIcon(":/vsicons/folder-opened.svg"), this, &MainWindow::openFile);
    m_recentFilesMenu = file->addMenu("Recent &Files");
    updateRecentFilesMenu();
    file->addSeparator();
    Qt5Qt6AddAction(file, "&Save", QKeySequence::Save, makeIcon(":/vsicons/save.svg"), this, &MainWindow::saveFile);
    Qt5Qt6AddAction(file, "Save &As...", QKeySequence::SaveAs, makeIcon(":/vsicons/save-as.svg"), this, &MainWindow::saveFileAs);
    file->addSeparator();
    auto* importMenu = file->addMenu("&Import");
    Qt5Qt6AddAction(importMenu, "From &Source...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::importFromSource);
    Qt5Qt6AddAction(importMenu, "ReClass &XML...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::importReclassXml);
    Qt5Qt6AddAction(importMenu, "&PDB...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::importPdb);
    auto* exportMenu = file->addMenu("E&xport");
    Qt5Qt6AddAction(exportMenu, "&C++ Header...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::exportCpp);
    Qt5Qt6AddAction(exportMenu, "ReClass &XML...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::exportReclassXmlAction);
    // Examples submenu — scan once at init
    {
#ifdef __APPLE__
        QDir exDir(QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../Resources/examples"));
#else
        QDir exDir(QCoreApplication::applicationDirPath() + "/examples");
#endif
        QStringList rcxFiles = exDir.entryList({"*.rcx"}, QDir::Files, QDir::Name);
        if (!rcxFiles.isEmpty()) {
            auto* examples = file->addMenu("E&xamples");
            for (const QString& fn : rcxFiles) {
                QString fullPath = exDir.absoluteFilePath(fn);
                examples->addAction(fn, this, [this, fullPath]() { project_open(fullPath); });
            }
        }
    }
    file->addSeparator();
    Qt5Qt6AddAction(file, "&Close Project", QKeySequence(Qt::CTRL | Qt::Key_W), QIcon(), this, &MainWindow::closeFile);
    file->addSeparator();
    Qt5Qt6AddAction(file, "E&xit", QKeySequence(Qt::Key_Close), makeIcon(":/vsicons/close.svg"), this, &QMainWindow::close);

    // Edit
    auto* edit = m_menuBar->addMenu("&Edit");
    Qt5Qt6AddAction(edit, "&Undo", QKeySequence::Undo, makeIcon(":/vsicons/arrow-left.svg"), this, &MainWindow::undo);
    Qt5Qt6AddAction(edit, "&Redo", QKeySequence::Redo, makeIcon(":/vsicons/arrow-right.svg"), this, &MainWindow::redo);

    // View
    auto* view = m_menuBar->addMenu("&View");
    Qt5Qt6AddAction(view, "&Reset Windows", QKeySequence::UnknownKey, QIcon(), this, [this](bool) {
        // Re-tabify all doc docks into a single group
        if (m_docDocks.size() < 2) return;
        auto* first = m_docDocks.first();
        for (int i = 1; i < m_docDocks.size(); ++i) {
            tabifyDockWidget(first, m_docDocks[i]);
            m_docDocks[i]->show();
        }
        if (m_activeDocDock) m_activeDocDock->raise();
        QTimer::singleShot(0, this, [this]() { setupDockTabBars(); });
    });
    view->addSeparator();
    m_sourceMenu = view->addMenu("&Data Source");
    connect(m_sourceMenu, &QMenu::aboutToShow, this, &MainWindow::populateSourceMenu);
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
    auto* actCompact = view->addAction("Compact &Columns");
    actCompact->setCheckable(true);
    actCompact->setChecked(settings.value("compactColumns", true).toBool());
    connect(actCompact, &QAction::triggered, this, [this](bool checked) {
        QSettings("Reclass", "Reclass").setValue("compactColumns", checked);
        for (auto& tab : m_tabs)
            tab.ctrl->setCompactColumns(checked);
    });

    auto* actTreeLines = view->addAction("&Tree Lines");
    actTreeLines->setCheckable(true);
    actTreeLines->setChecked(settings.value("treeLines", false).toBool());
    connect(actTreeLines, &QAction::triggered, this, [this](bool checked) {
        QSettings("Reclass", "Reclass").setValue("treeLines", checked);
        for (auto& tab : m_tabs)
            tab.ctrl->setTreeLines(checked);
    });

    auto* actRelOfs = view->addAction("R&elative Offsets");
    actRelOfs->setCheckable(true);
    actRelOfs->setChecked(settings.value("relativeOffsets", true).toBool());
    connect(actRelOfs, &QAction::triggered, this, [this](bool checked) {
        QSettings("Reclass", "Reclass").setValue("relativeOffsets", checked);
        for (auto& tab : m_tabs)
            for (auto& pane : tab.panes)
                pane.editor->setRelativeOffsets(checked);
    });

    view->addSeparator();
    view->addAction(m_workspaceDock->toggleViewAction());
    {
        auto* scanAct = m_scannerDock->toggleViewAction();
        scanAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
        view->addAction(scanAct);
    }

    // Tools
    auto* tools = m_menuBar->addMenu("&Tools");
    Qt5Qt6AddAction(tools, "&Type Aliases...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::showTypeAliasesDialog);
    tools->addSeparator();
    const auto mcpName = QSettings("Reclass", "Reclass").value("autoStartMcp", true).toBool() ? "Stop &MCP Server" : "Start &MCP Server";
    m_mcpAction = Qt5Qt6AddAction(tools, mcpName, QKeySequence::UnknownKey, QIcon(), this, &MainWindow::toggleMcp);
    tools->addSeparator();
    Qt5Qt6AddAction(tools, "&Options...", QKeySequence::UnknownKey, makeIcon(":/vsicons/settings-gear.svg"), this, &MainWindow::showOptionsDialog);

    // Plugins
    auto* plugins = m_menuBar->addMenu("&Plugins");
    Qt5Qt6AddAction(plugins, "&Manage Plugins...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::showPluginsDialog);

    // Help
    auto* help = m_menuBar->addMenu("&Help");
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

// ── Dock title-bar grip (VS2022-style dot pattern) ──
class DockGripWidget : public QWidget {
public:
    explicit DockGripWidget(QWidget* parent) : QWidget(parent) {
        setFixedWidth(6);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        m_color = rcx::ThemeManager::instance().current().textFaint;
    }
    void setGripColor(const QColor& c) { m_color = c; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(m_color);
        const double r = 0.75, s = 3.0;
        double cx = width() / 2.0;
        double cy = height() / 2.0;
        // 2 columns x 3 rows, centered
        for (int row = -1; row <= 1; row++) {
            p.drawEllipse(QPointF(cx - s * 0.5, cy + row * s), r, r);
            p.drawEllipse(QPointF(cx + s * 0.5, cy + row * s), r, r);
        }
    }
private:
    QColor m_color;
};

// ── Custom-painted view tab button (no CSS) ──
class ViewTabButton : public QPushButton {
public:
    static constexpr int kAccentH = 3;   // accent line height in pixels
    static constexpr int kPadLR  = 12;   // horizontal padding
    static constexpr int kPadBot = 4;    // extra bottom padding

    int baselineY = -1;  // set by FlatStatusBar for cross-widget text alignment

    QColor colBg, colBgChecked, colBgHover, colBgPressed;
    QColor colText, colTextMuted, colAccent, colBorder;

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

        // Top border (continuous with status bar hairline)
        if (colBorder.isValid())
            p.fillRect(0, 0, width(), 1, colBorder);

        // Accent line at y=0 when checked (paints over border)
        if (isChecked())
            p.fillRect(0, 0, width(), kAccentH, colAccent);

        // Text — use shared baseline if set, otherwise fall back to VCenter
        p.setPen(isChecked() || underMouse() || isDown() ? colText : colTextMuted);
        p.setFont(font());
        if (baselineY >= 0) {
            p.drawText(kPadLR, baselineY, text());
        } else {
            QRect textRect(kPadLR, kAccentH, width() - 2 * kPadLR, height() - kAccentH);
            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text());
        }
    }

    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
};

// ── Shimmer label — gradient text sweep for MCP activity ──
class ShimmerLabel : public QWidget {
public:
    explicit ShimmerLabel(QWidget* parent = nullptr) : QWidget(parent) {
        m_timer.setInterval(30);
        connect(&m_timer, &QTimer::timeout, this, [this]() {
            m_phase += 0.012f;
            if (m_phase > 1.0f) m_phase -= 1.0f;
            update();
        });
    }

    void setText(const QString& t) { m_text = t; update(); }
    QString text() const { return m_text; }

    void setShimmerActive(bool on) {
        if (m_shimmer == on) return;
        m_shimmer = on;
        if (on) { m_phase = 0.0f; m_timer.start(); }
        else    { m_timer.stop(); }
        update();
    }
    bool shimmerActive() const { return m_shimmer; }

    void setAlignment(Qt::Alignment a) { m_align = a; update(); }

    // Colours configurable from theme
    QColor colBase;     // dim text (normal)
    QColor colBright;   // highlight sweep

protected:
    void paintEvent(QPaintEvent*) override {
        if (m_text.isEmpty()) return;
        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setFont(font());

        QRect r = contentsRect();

        if (!m_shimmer) {
            QColor c = colBase.isValid() ? colBase
                                         : palette().color(QPalette::WindowText);
            p.setPen(c);
            p.drawText(r, m_align, m_text);
            return;
        }

        // Shimmer: sweeping glow band behind text + bright text
        QColor bright = colBright.isValid() ? colBright : QColor(255, 200, 80);

        // 1. Sweeping glow band (semi-transparent background highlight)
        qreal bandW = width() * 0.20;
        qreal bandCenter = -bandW + (width() + 2 * bandW) * m_phase;
        QLinearGradient bgGrad(bandCenter - bandW, 0, bandCenter + bandW, 0);
        QColor glow = bright;
        glow.setAlpha(35);
        bgGrad.setColorAt(0.0, Qt::transparent);
        bgGrad.setColorAt(0.5, glow);
        bgGrad.setColorAt(1.0, Qt::transparent);
        p.fillRect(rect(), QBrush(bgGrad));

        // 2. Text in bright color
        p.setPen(bright);
        p.drawText(r, m_align, m_text);
    }

private:
    QString      m_text;
    bool         m_shimmer = false;
    float        m_phase   = 0.0f;
    Qt::Alignment m_align  = Qt::AlignLeft | Qt::AlignVCenter;
    QTimer       m_timer;
};

// ── Borderless status bar with manual child layout ──
// QStatusBarLayout hardcodes 2px margins that can't be overridden.
// We bypass it entirely: children are placed manually in resizeEvent,
// and addWidget() is NOT used. Instead, create children as direct
// children and call manualLayout() to position them.
class FlatStatusBar : public QStatusBar {
public:
    QWidget*       tabRow = nullptr;   // set by createStatusBar
    ShimmerLabel*  label  = nullptr;   // set by createStatusBar

    void setDividerColor(const QColor& c) { m_div = c; update(); }
    void setTopLineColor(const QColor& c) { m_top = c; update(); }

    explicit FlatStatusBar(QWidget* parent = nullptr) : QStatusBar(parent) {
        setSizeGripEnabled(false);
    }

    QSize sizeHint() const override {
        const int tabH  = tabRow ? tabRow->sizeHint().height() : 0;
        const int textH = fontMetrics().height();
        const int base  = qMax(tabH, textH + 6);
        const int h     = qRound(base * 1.15);
        return { QStatusBar::sizeHint().width(), h };
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().window());

        // Top hairline separator
        if (m_top.isValid())
            p.fillRect(0, 0, width(), 1, m_top);

        // Vertical divider between tabRow and label
        if (m_div.isValid() && m_divX >= 0)
            p.fillRect(m_divX, 4, 1, height() - 8, m_div);
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
    QColor m_div, m_top;
    int m_divX = -1;

    void manualLayout() {
        if (!label) return;
        const int h = height();
        const int gutter = 6;
        if (tabRow) {
            const int tw = tabRow->sizeHint().width();
            tabRow->setGeometry(0, 0, tw, h);
            m_divX = tw;
            label->setGeometry(tw + 1 + gutter, 0,
                               qMax(0, width() - (tw + 1 + gutter)), h);
        } else {
            m_divX = -1;
            label->setGeometry(gutter, 0, qMax(0, width() - gutter), h);
        }
        label->setContentsMargins(0, 0, 0, 0);
        label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    }
};

void MainWindow::createStatusBar() {
    // Replace the default QStatusBar with our borderless, manually-laid-out one.
    // QStatusBarLayout hardcodes 2px margins; we bypass addWidget entirely.
    auto* sb = new FlatStatusBar;
    setStatusBar(sb);

    m_statusLabel = new ShimmerLabel(sb);
    m_statusLabel->setText("");
    m_statusLabel->setContentsMargins(0, 0, 0, 0);
    m_statusLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    // View toggle is now per-pane via QTabWidget tab bar (Reclass / C/C++ tabs)
    sb->tabRow = nullptr;
    sb->label  = m_statusLabel;

    sb->setMinimumHeight(sb->fontMetrics().height() + 6);

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

        sb->setTopLineColor(t.border);
        sb->setDividerColor(t.border);

        m_statusLabel->colBase   = t.textDim;
        m_statusLabel->colBright = t.indHoverSpan;
    }

}

void MainWindow::setAppStatus(const QString& text) {
    m_appStatus = text;
    if (!m_mcpBusy) {
        m_statusLabel->setText(text);
        m_statusLabel->setShimmerActive(false);
    }
}

void MainWindow::setMcpStatus(const QString& text) {
    // Cancel any pending clear — new activity extends the shimmer
    if (m_mcpClearTimer) m_mcpClearTimer->stop();
    m_mcpBusy = true;
    m_statusLabel->setText(text);
    m_statusLabel->setShimmerActive(true);
}

void MainWindow::clearMcpStatus() {
    // Delay the clear so the shimmer stays visible for at least 750ms
    if (!m_mcpClearTimer) {
        m_mcpClearTimer = new QTimer(this);
        m_mcpClearTimer->setSingleShot(true);
        connect(m_mcpClearTimer, &QTimer::timeout, this, [this]() {
            m_mcpBusy = false;
            m_statusLabel->setText(m_appStatus);
            m_statusLabel->setShimmerActive(false);
        });
    }
    m_mcpClearTimer->start(750);
}



MainWindow::SplitPane MainWindow::createSplitPane(TabState& tab) {
    SplitPane pane;

    pane.tabWidget = new QTabWidget;
    pane.tabWidget->setTabPosition(QTabWidget::South);
    pane.tabWidget->tabBar()->setVisible(true);
    pane.tabWidget->setDocumentMode(true);  // kill QTabWidget frame border

    // Style to match the top dock tab bar, with accent line on selected tab
    {
        const auto& t = ThemeManager::instance().current();
        pane.tabWidget->setStyleSheet(QStringLiteral(
            "QTabBar { border: none; }"
            "QTabBar::tab {"
            "  background: %1; color: %2; padding: 0px 16px; border: none; border-radius: 0px; height: 24px;"
            "}"
            "QTabBar::tab:selected { color: %3; background: %4;"
            "  border-top: 3px solid %6; padding-top: -3px; }"
            "QTabBar::tab:hover { color: %3; background: %5; }")
            .arg(t.background.name(), t.textMuted.name(), t.text.name(),
                 t.backgroundAlt.name(), t.hover.name(), t.indHoverSpan.name()));
    }

    // Create editor via controller (parent = tabWidget for ownership)
    pane.editor = tab.ctrl->addSplitEditor(pane.tabWidget);
    pane.editor->setRelativeOffsets(
        QSettings("Reclass", "Reclass").value("relativeOffsets", true).toBool());
    pane.tabWidget->addTab(pane.editor, "Reclass");     // index 0

    // Create per-pane rendered C++ view with find bar
    pane.renderedContainer = new QWidget;
    auto* rvLayout = new QVBoxLayout(pane.renderedContainer);
    rvLayout->setContentsMargins(0, 0, 0, 0);
    rvLayout->setSpacing(0);
    pane.rendered = new QsciScintilla;
    setupRenderedSci(pane.rendered);
    rvLayout->addWidget(pane.rendered);

    // Find bar with prev/next buttons (hidden by default)
    pane.findContainer = new QWidget;
    auto* fcLayout = new QHBoxLayout(pane.findContainer);
    fcLayout->setContentsMargins(4, 1, 4, 1);
    fcLayout->setSpacing(2);
    const auto& fbTheme = ThemeManager::instance().current();
    auto* ccPrevBtn = new QToolButton;
    ccPrevBtn->setText(QStringLiteral("\u25C0"));
    ccPrevBtn->setFixedSize(24, 24);
    auto* ccNextBtn = new QToolButton;
    ccNextBtn->setText(QStringLiteral("\u25B6"));
    ccNextBtn->setFixedSize(24, 24);
    auto* ccCloseBtn = new QToolButton;
    ccCloseBtn->setText(QStringLiteral("\u2715"));
    ccCloseBtn->setFixedSize(24, 24);
    QString btnCss = QStringLiteral(
        "QToolButton { background: %1; color: %2; border: 1px solid %3; border-radius: 2px; }"
        "QToolButton:hover { background: %4; }"
        "QToolButton:pressed { background: %5; }")
            .arg(fbTheme.background.name(), fbTheme.text.name(), fbTheme.border.name(),
                 fbTheme.hover.name(), fbTheme.backgroundAlt.name());
    ccPrevBtn->setStyleSheet(btnCss);
    ccNextBtn->setStyleSheet(btnCss);
    ccCloseBtn->setStyleSheet(btnCss);
    pane.findBar = new QLineEdit;
    pane.findBar->setPlaceholderText("Find...");
    pane.findBar->setFixedHeight(24);
    pane.findBar->setStyleSheet(
        QStringLiteral("QLineEdit { background: %1; color: %2; border: 1px solid %3;"
                        " padding: 2px 6px; font-size: 13px; }")
            .arg(fbTheme.backgroundAlt.name(), fbTheme.text.name(), fbTheme.border.name()));
    fcLayout->addWidget(ccPrevBtn);
    fcLayout->addWidget(ccNextBtn);
    fcLayout->addWidget(ccCloseBtn);
    fcLayout->addWidget(pane.findBar);
    pane.findContainer->setVisible(false);
    rvLayout->addWidget(pane.findContainer);

    // Ctrl+F to show find bar
    QsciScintilla* sci = pane.rendered;
    QLineEdit* fb = pane.findBar;
    QWidget* fc = pane.findContainer;
    auto* findAction = new QAction(pane.renderedContainer);
    findAction->setShortcut(QKeySequence::Find);
    findAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    pane.renderedContainer->addAction(findAction);
    connect(findAction, &QAction::triggered, fb, [fb, fc]() {
        fc->setVisible(true);
        fb->setFocus();
        fb->selectAll();
    });

    // Escape to hide find bar
    auto* escAction = new QAction(fb);
    escAction->setShortcut(QKeySequence(Qt::Key_Escape));
    escAction->setShortcutContext(Qt::WidgetShortcut);
    fb->addAction(escAction);
    connect(escAction, &QAction::triggered, fb, [fc, sci]() {
        fc->setVisible(false);
        sci->setFocus();
    });

    // Search on text change and Enter
    connect(fb, &QLineEdit::textChanged, sci, [sci](const QString& text) {
        if (text.isEmpty()) return;
        sci->findFirst(text, false, false, false, true, true, 0, 0);
    });
    connect(fb, &QLineEdit::returnPressed, sci, [sci, fb]() {
        QString text = fb->text();
        if (text.isEmpty()) return;
        if (!sci->findNext())
            sci->findFirst(text, false, false, false, true, true, 0, 0);
    });
    connect(ccNextBtn, &QToolButton::clicked, sci, [sci, fb]() {
        if (!sci->findNext())
            sci->findFirst(fb->text(), false, false, false, true, true, 0, 0);
    });
    connect(ccPrevBtn, &QToolButton::clicked, sci, [sci, fb]() {
        QString text = fb->text();
        if (text.isEmpty()) return;
        int line, col;
        sci->getCursorPosition(&line, &col);
        sci->findFirst(text, false, false, false, true, false, line, col);
    });
    connect(ccCloseBtn, &QToolButton::clicked, sci, [fc, sci]() {
        fc->setVisible(false);
        sci->setFocus();
    });

    pane.tabWidget->addTab(pane.renderedContainer, "C/C++");     // index 1

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

// Event filter to manage border overlay + resize grip on floating dock widgets
class DockBorderFilter : public QObject {
public:
    BorderOverlay* border;
    ResizeGrip* grip;
    DockBorderFilter(BorderOverlay* b, ResizeGrip* g, QObject* parent)
        : QObject(parent), border(b), grip(g) {}
    bool eventFilter(QObject* obj, QEvent* ev) override {
        auto* dock = qobject_cast<QDockWidget*>(obj);
        if (!dock || !dock->isFloating()) return false;
        if (ev->type() == QEvent::Resize) {
            border->setGeometry(0, 0, dock->width(), dock->height());
            border->raise();
            grip->reposition();
            grip->raise();
        } else if (ev->type() == QEvent::WindowActivate) {
            border->color = ThemeManager::instance().current().borderFocused;
            border->update();
        } else if (ev->type() == QEvent::WindowDeactivate) {
            border->color = ThemeManager::instance().current().border;
            border->update();
        }
        return false;
    }
};

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

QDockWidget* MainWindow::createTab(RcxDocument* doc) {
    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setHandleWidth(1);
    auto* ctrl = new RcxController(doc, splitter);

    QString title = doc->filePath.isEmpty()
                    ? rootName(doc->tree) : QFileInfo(doc->filePath).fileName();
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(QStringLiteral("DocDock_%1").arg(quintptr(dock), 0, 16));
    dock->setFeatures(QDockWidget::DockWidgetClosable |
                      QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable);
    // Two title bar widgets: a hidden one (docked) and a draggable one (floating)
    auto* emptyTitleBar = new QWidget(dock);
    emptyTitleBar->setFixedHeight(0);

    auto* floatTitleBar = new QWidget(dock);
    {
        const auto& t = ThemeManager::instance().current();
        floatTitleBar->setFixedHeight(24);
        floatTitleBar->setAutoFillBackground(true);
        {
            QPalette tbPal = floatTitleBar->palette();
            tbPal.setColor(QPalette::Window, t.backgroundAlt);
            floatTitleBar->setPalette(tbPal);
        }
        auto* hl = new QHBoxLayout(floatTitleBar);
        hl->setContentsMargins(4, 2, 2, 2);
        hl->setSpacing(4);

        auto* grip = new DockGripWidget(floatTitleBar);
        grip->setObjectName("dockFloatGrip");
        hl->addWidget(grip);

        auto* lbl = new QLabel(title, floatTitleBar);
        lbl->setObjectName("dockFloatTitle");
        {
            QPalette lp = lbl->palette();
            lp.setColor(QPalette::WindowText, t.textDim);
            lbl->setPalette(lp);
        }
        {
            QSettings settings("Reclass", "Reclass");
            QFont f(settings.value("font", "JetBrains Mono").toString(), 12);
            f.setFixedPitch(true);
            lbl->setFont(f);
        }
        hl->addWidget(lbl);
        hl->addStretch();
        auto* closeBtn = new QToolButton(floatTitleBar);
        closeBtn->setObjectName("dockFloatClose");
        closeBtn->setText(QStringLiteral("\u2715"));
        closeBtn->setAutoRaise(true);
        closeBtn->setCursor(Qt::PointingHandCursor);
        closeBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: none; padding: 0px 4px 2px 4px; font-size: 12px; }"
            "QToolButton:hover { color: %2; }")
            .arg(t.textDim.name(), t.indHoverSpan.name()));
        connect(closeBtn, &QToolButton::clicked, dock, &QDockWidget::close);
        hl->addWidget(closeBtn);
    }
    floatTitleBar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(floatTitleBar, &QWidget::customContextMenuRequested,
            this, [this, dock, floatTitleBar](const QPoint& pos) {
        QMenu menu;
        menu.addAction("Dock", [dock]() { dock->setFloating(false); });
        menu.addSeparator();
        auto* alwaysFloat = menu.addAction("Always Floating");
        alwaysFloat->setCheckable(true);
        bool locked = !(dock->features() & QDockWidget::DockWidgetMovable);
        alwaysFloat->setChecked(locked);
        connect(alwaysFloat, &QAction::toggled, dock, [dock](bool checked) {
            auto features = dock->features();
            if (checked)
                features &= ~QDockWidget::DockWidgetMovable;
            else
                features |= QDockWidget::DockWidgetMovable;
            dock->setFeatures(features);
        });
        menu.addSeparator();
        menu.addAction("Close", [dock]() { dock->close(); });
        menu.exec(floatTitleBar->mapToGlobal(pos));
    });

    dock->setTitleBarWidget(emptyTitleBar);
    dock->setWidget(splitter);

    // Border overlay and resize grip for floating state
    auto* dockBorder = new BorderOverlay(dock);
    dockBorder->color = ThemeManager::instance().current().borderFocused;
    dockBorder->hide();

    auto* dockGrip = new ResizeGrip(dock);
    dockGrip->hide();

    // Swap title bar when floating/docking, show/hide border + grip
    connect(dock, &QDockWidget::topLevelChanged, this, [dock, emptyTitleBar, floatTitleBar, dockBorder, dockGrip](bool floating) {
        dock->setTitleBarWidget(floating ? floatTitleBar : emptyTitleBar);
        if (floating) {
            dockBorder->setGeometry(0, 0, dock->width(), dock->height());
            dockBorder->raise();
            dockBorder->show();
            dockGrip->reposition();
            dockGrip->raise();
            dockGrip->show();
        } else {
            dockBorder->hide();
            dockGrip->hide();
        }
    });
    dock->installEventFilter(new DockBorderFilter(dockBorder, dockGrip, dock));
    // Keep float title bar label in sync with dock title
    connect(dock, &QDockWidget::windowTitleChanged, floatTitleBar, [floatTitleBar](const QString& t) {
        if (auto* lbl = floatTitleBar->findChild<QLabel*>("dockFloatTitle"))
            lbl->setText(t);
    });

    // Tabify with existing doc docks, or add to top area
    if (!m_docDocks.isEmpty())
        tabifyDockWidget(m_docDocks.last(), dock);
    else
        addDockWidget(Qt::TopDockWidgetArea, dock);

    m_docDocks.append(dock);
    m_tabs[dock] = { doc, ctrl, splitter, {}, 0 };
    m_activeDocDock = dock;
    auto& tab = m_tabs[dock];

    // Create the initial split pane
    tab.panes.append(createSplitPane(tab));

    // Apply global compact columns setting to new tab
    ctrl->setCompactColumns(QSettings("Reclass", "Reclass").value("compactColumns", true).toBool());
    ctrl->setTreeLines(QSettings("Reclass", "Reclass").value("treeLines", false).toBool());
    ctrl->setBraceWrap(QSettings("Reclass", "Reclass").value("braceWrap", false).toBool());

    // Give every controller the shared document list for cross-tab type visibility
    ctrl->setProjectDocuments(&m_allDocs);
    rebuildAllDocs();

    // Track active tab via visibility
    connect(dock, &QDockWidget::visibilityChanged, this, [this, dock](bool visible) {
        if (visible) {
            m_activeDocDock = dock;
            updateWindowTitle();
            rebuildWorkspaceModel();
            // Sync view toggle buttons to this tab's active pane
            auto it = m_tabs.find(dock);
            if (it != m_tabs.end()) {
                auto& tab = *it;
                if (tab.activePaneIdx >= 0 && tab.activePaneIdx < tab.panes.size())
                    syncViewButtons(tab.panes[tab.activePaneIdx].viewMode);
            }
        }
        // Keep border overlay on top after dock rearrangements
        if (m_borderOverlay) m_borderOverlay->raise();
    });

    // Cleanup on close
    connect(dock, &QObject::destroyed, this, [this, dock]() {
        auto it = m_tabs.find(dock);
        if (it != m_tabs.end()) {
            RcxDocument* doc = it->doc;
            m_tabs.erase(it);
            // Only delete the doc if no other tab references it
            bool docStillUsed = false;
            for (auto jt = m_tabs.begin(); jt != m_tabs.end(); ++jt) {
                if (jt->doc == doc) { docStillUsed = true; break; }
            }
            if (!docStillUsed)
                doc->deleteLater();
        }
        m_docDocks.removeOne(dock);
        if (m_activeDocDock == dock)
            m_activeDocDock = m_docDocks.isEmpty() ? nullptr : m_docDocks.last();
        rebuildAllDocs();
        rebuildWorkspaceModel();
    });

    connect(ctrl, &RcxController::nodeSelected,
            this, [this, ctrl, dock](int nodeIdx) {
        if (nodeIdx >= 0 && nodeIdx < ctrl->document()->tree.nodes.size()) {
            auto& node = ctrl->document()->tree.nodes[nodeIdx];
            auto* ap = findActiveSplitPane();
            if (ap && ap->viewMode == VM_Rendered)
                setAppStatus(
                    QString("Rendered: %1 %2")
                        .arg(kindToString(node.kind))
                        .arg(node.name));
            else
                setAppStatus(
                    QString("%1 %2  offset: 0x%3  size: %4 bytes")
                        .arg(kindToString(node.kind))
                        .arg(node.name)
                        .arg(node.offset, 4, 16, QChar('0'))
                        .arg(node.byteSize()));
        }
        // Update all rendered panes on selection change
        auto it = m_tabs.find(dock);
        if (it != m_tabs.end())
            updateAllRenderedPanes(*it);
    });
    connect(ctrl, &RcxController::selectionChanged,
            this, [this](int count) {
        if (count > 1)
            setAppStatus(QString("%1 nodes selected").arg(count));
    });

    // Append Float/Close actions to any editor context menu
    connect(ctrl, &RcxController::contextMenuAboutToShow,
            this, [this, dock](QMenu* menu, int /*line*/) {
        menu->addSeparator();
        menu->addAction(dock->isFloating() ? "Dock" : "Float", [dock]() {
            dock->setFloating(!dock->isFloating());
        });
        menu->addAction("Close Tab", [dock]() { dock->close(); });
    });

    // Update rendered panes and workspace on document changes and undo/redo
    // Use QPointer to guard against dock being destroyed before deferred timer fires
    QPointer<QDockWidget> dockGuard = dock;
    connect(doc, &RcxDocument::documentChanged,
            this, [this, dockGuard]() {
        if (!dockGuard) return;
        auto it = m_tabs.find(dockGuard);
        if (it != m_tabs.end())
            QTimer::singleShot(0, this, [this, dockGuard]() {
                if (!dockGuard) return;
                auto it2 = m_tabs.find(dockGuard);
                if (it2 != m_tabs.end()) {
                    updateAllRenderedPanes(*it2);
                    if (it2->doc->filePath.isEmpty())
                        dockGuard->setWindowTitle(rootName(it2->doc->tree, it2->ctrl->viewRootId()));
                }
                rebuildWorkspaceModel();
                updateWindowTitle();
            });
    });
    connect(&doc->undoStack, &QUndoStack::indexChanged,
            this, [this, dockGuard](int) {
        if (!dockGuard) return;
        auto it = m_tabs.find(dockGuard);
        if (it != m_tabs.end())
            QTimer::singleShot(0, this, [this, dockGuard]() {
                if (!dockGuard) return;
                auto it2 = m_tabs.find(dockGuard);
                if (it2 != m_tabs.end()) {
                    updateAllRenderedPanes(*it2);
                    if (it2->doc->filePath.isEmpty())
                        dockGuard->setWindowTitle(rootName(it2->doc->tree, it2->ctrl->viewRootId()));
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

    dock->raise();
    dock->show();
    // Ensure the new dock's tab is activated in the tab bar.
    // Since we tabify with the last dock, the new tab is always appended last.
    for (auto* tabBar : findChildren<QTabBar*>()) {
        if (tabBar->parent() != this) continue;
        if (tabBar->count() > 0) {
            tabBar->setCurrentIndex(tabBar->count() - 1);
            break;
        }
    }

    // Install context menu + pin/close buttons on dock tab bars
    // (deferred — tab bar created after tabification)
    QTimer::singleShot(0, this, [this]() { setupDockTabBars(); });

    return dock;
}

// ── Setup dock tab bars ──
// Installs pin/close buttons, context menu, font, and style on all
// dock tab bars owned by this QMainWindow. Safe to call repeatedly —
// skips tabs that already have buttons and tab bars that already have
// a context menu.
void MainWindow::setupDockTabBars() {
    const auto& theme = ThemeManager::instance().current();
    for (auto* tabBar : findChildren<QTabBar*>()) {
        if (tabBar->parent() != this) continue;

        // No stylesheet — painting handled by MenuBarStyle
        tabBar->setStyleSheet(QString());
        tabBar->setElideMode(Qt::ElideNone);
        tabBar->setExpanding(false);
        // Set editor font so tab width sizing matches our label painting
        {
            QSettings s("Reclass", "Reclass");
            QFont tabFont(s.value("font", "JetBrains Mono").toString(), 10);
            tabFont.setFixedPitch(true);
            tabBar->setFont(tabFont);
        }
        QPalette tp = tabBar->palette();
        tp.setColor(QPalette::WindowText, theme.textDim);
        tp.setColor(QPalette::Text, theme.text);
        tp.setColor(QPalette::Window, theme.background);
        tp.setColor(QPalette::Mid, theme.hover);
        tp.setColor(QPalette::Dark, theme.border);
        tp.setColor(QPalette::Link, theme.indHoverSpan);
        tabBar->setPalette(tp);

        // Install tab buttons for any tab that doesn't have them yet
        for (int i = 0; i < tabBar->count(); ++i) {
            auto* existing = qobject_cast<DockTabButtons*>(
                tabBar->tabButton(i, QTabBar::RightSide));
            if (existing) continue;

            auto* btns = new DockTabButtons(tabBar);
            btns->applyTheme(theme.hover);

            // Find dock by matching tab title
            QString title = tabBar->tabText(i);
            QDockWidget* target = nullptr;
            for (auto* d : m_docDocks) {
                if (d->windowTitle() == title) { target = d; break; }
            }
            if (target) {
                connect(btns->closeBtn, &QToolButton::clicked,
                        target, &QDockWidget::close);
            }
            tabBar->setTabButton(i, QTabBar::RightSide, btns);
        }

        // Context menu (install only once)
        if (tabBar->contextMenuPolicy() == Qt::CustomContextMenu) continue;
        tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tabBar, &QTabBar::customContextMenuRequested,
                this, [this, tabBar](const QPoint& pos) {
            int idx = tabBar->tabAt(pos);
            if (idx < 0) return;

            // Find target dock
            QString tabTitle = tabBar->tabText(idx);
            QDockWidget* target = nullptr;
            for (auto* d : m_docDocks)
                if (d->windowTitle() == tabTitle) { target = d; break; }
            if (!target) return;

            auto tabIt = m_tabs.find(target);
            auto* btns = qobject_cast<DockTabButtons*>(
                tabBar->tabButton(idx, QTabBar::RightSide));
            bool isPinned = btns && btns->pinned;

            QMenu menu;

            // Close
            menu.addAction(makeIcon(":/vsicons/close.svg"), "Close",
                           QKeySequence(Qt::CTRL | Qt::Key_W),
                           [target]() { target->close(); });

            menu.addSeparator();

            // Close All Tabs
            menu.addAction(makeIcon(":/vsicons/close-all.svg"), "Close All Tabs",
                           [this]() { closeAllDocDocks(); });

            // Close All But This
            if (m_docDocks.size() > 1) {
                menu.addAction("Close All But This", [this, target]() {
                    auto docks = m_docDocks;
                    for (auto* d : docks)
                        if (d != target) d->close();
                });
            }

            // Close All But Pinned (only if any tab is pinned)
            bool anyPinned = false;
            for (int i = 0; i < tabBar->count(); ++i) {
                auto* b = qobject_cast<DockTabButtons*>(
                    tabBar->tabButton(i, QTabBar::RightSide));
                if (b && b->pinned) { anyPinned = true; break; }
            }
            if (anyPinned) {
                menu.addAction("Close All But Pinned", [this, tabBar]() {
                    QVector<QDockWidget*> toClose;
                    for (int i = 0; i < tabBar->count(); ++i) {
                        auto* b = qobject_cast<DockTabButtons*>(
                            tabBar->tabButton(i, QTabBar::RightSide));
                        if (b && b->pinned) continue;
                        QString title = tabBar->tabText(i);
                        for (auto* d : m_docDocks)
                            if (d->windowTitle() == title) { toClose.append(d); break; }
                    }
                    for (auto* d : toClose) d->close();
                });
            }

            menu.addSeparator();

            // Copy Full Path / Open Containing Folder (only if saved)
            if (tabIt != m_tabs.end() && !tabIt->doc->filePath.isEmpty()) {
                QString path = tabIt->doc->filePath;
                menu.addAction(makeIcon(":/vsicons/clippy.svg"), "Copy Full Path",
                               [path]() { QGuiApplication::clipboard()->setText(path); });
                menu.addAction(makeIcon(":/vsicons/folder-opened.svg"),
                               "Open Containing Folder", [path]() {
                    QDesktopServices::openUrl(
                        QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
                });
            }

            // Float / Dock
            menu.addAction(target->isFloating() ? "Dock" : "Float", [target]() {
                target->setFloating(!target->isFloating());
            });

            menu.addSeparator();

            // Pin / Unpin
            if (btns) {
                QIcon pinIcon = makeIcon(isPinned ? ":/vsicons/pinned.svg"
                                                  : ":/vsicons/pin.svg");
                menu.addAction(pinIcon, isPinned ? "Unpin Tab" : "Pin Tab",
                               [btns, isPinned]() { btns->setPinned(!isPinned); });
            }

            menu.addSeparator();

            // New Document Groups (only if >1 tab)
            if (tabBar->count() > 1) {
                menu.addAction(makeIcon(":/vsicons/split-horizontal.svg"),
                               "New Horizontal Document Group",
                               [this, target]() {
                    Qt::DockWidgetArea area = dockWidgetArea(target);
                    if (area == Qt::NoDockWidgetArea) area = Qt::TopDockWidgetArea;
                    removeDockWidget(target);
                    addDockWidget(area, target, Qt::Horizontal);
                    target->show();
                    QList<QDockWidget*> docks;
                    QList<int> sizes;
                    for (auto* d : m_docDocks) {
                        if (!d->isFloating() && d->isVisible() && dockWidgetArea(d) == area) {
                            docks.append(d);
                            sizes.append(width() / 2);
                        }
                    }
                    if (docks.size() >= 2)
                        resizeDocks(docks, sizes, Qt::Horizontal);
                    QTimer::singleShot(0, this, [this]() { setupDockTabBars(); });
                });
                menu.addAction(makeIcon(":/vsicons/split-vertical.svg"),
                               "New Vertical Document Group",
                               [this, target]() {
                    Qt::DockWidgetArea area = dockWidgetArea(target);
                    if (area == Qt::NoDockWidgetArea) area = Qt::TopDockWidgetArea;
                    removeDockWidget(target);
                    addDockWidget(area, target, Qt::Vertical);
                    target->show();
                    QList<QDockWidget*> docks;
                    QList<int> sizes;
                    for (auto* d : m_docDocks) {
                        if (!d->isFloating() && d->isVisible() && dockWidgetArea(d) == area) {
                            docks.append(d);
                            sizes.append(height() / 2);
                        }
                    }
                    if (docks.size() >= 2)
                        resizeDocks(docks, sizes, Qt::Vertical);
                    QTimer::singleShot(0, this, [this]() { setupDockTabBars(); });
                });
            }

            menu.exec(tabBar->mapToGlobal(pos));
        });
    }
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

    // Default project: add an example enum and a class with a union
    if (classKeyword.isEmpty()) {
        // ── Example enum: _POOL_TYPE ──
        {
            Node e;
            e.kind = NodeKind::Struct;
            e.name = QStringLiteral("_POOL_TYPE");
            e.structTypeName = QStringLiteral("_POOL_TYPE");
            e.classKeyword = QStringLiteral("enum");
            e.parentId = 0;
            e.collapsed = false;
            e.enumMembers = {
                {QStringLiteral("NonPagedPool"), 0},
                {QStringLiteral("PagedPool"), 1},
                {QStringLiteral("NonPagedPoolMustSucceed"), 2},
                {QStringLiteral("DontUseThisType"), 3},
                {QStringLiteral("NonPagedPoolCacheAligned"), 4},
                {QStringLiteral("PagedPoolCacheAligned"), 5},
            };
            tree.addNode(e);
        }

    }
}

MainWindow::~MainWindow() {
    /*
     * When MainWindow is destroyed:
     *
	 *	  1. ~MainWindow() runs (our code — plugin DLLs still loaded)
	 *	  2. MainWindow member variables are destroyed (m_pluginManager — unloads plugin DLLs)
	 *	  3. QObject::~QObject() runs — destroys child widgets (QMdiSubWindow → RcxController → ~RcxController())
     * 
     */

    // Disconnect all signals before members are torn down,
    // so the lambdas capturing 'this' never fire on a half-destroyed object.
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        disconnect(it.key(), &QObject::destroyed, this, nullptr);
        disconnect(&it->doc->undoStack, nullptr, this, nullptr);
        // Release providers now while plugin DLLs are still loaded;
        // if deferred to Qt child cleanup the DLL code may already be unloaded.
        it->doc->provider.reset();
        it->ctrl->resetProvider();
    }
    m_tabs.clear();
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

    // ── Example enum: _POOL_TYPE ──
    {
        Node e;
        e.kind = NodeKind::Struct;
        e.name = QStringLiteral("_POOL_TYPE");
        e.structTypeName = QStringLiteral("_POOL_TYPE");
        e.classKeyword = QStringLiteral("enum");
        e.parentId = 0;
        e.collapsed = false;
        e.enumMembers = {
            {QStringLiteral("NonPagedPool"), 0},
            {QStringLiteral("PagedPool"), 1},
            {QStringLiteral("NonPagedPoolMustSucceed"), 2},
            {QStringLiteral("DontUseThisType"), 3},
            {QStringLiteral("NonPagedPoolCacheAligned"), 4},
            {QStringLiteral("PagedPoolCacheAligned"), 5},
        };
        tree.addNode(e);
    }

}

void MainWindow::selfTest() {
#ifdef Q_OS_WIN
    // Tab 2: Editor demo with live process memory (created first)
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

    // Tab 1: Empty class for user work (created second, becomes active)
    auto* userTab = project_new(QStringLiteral("class"));
    userTab->raise();
    userTab->show();
#else
    project_new();
    auto* userTab = project_new(QStringLiteral("class"));
    userTab->raise();
    userTab->show();
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
        int idx = ctrl->document()->tree.indexOfId(
            id & ~(kFooterIdBit | kArrayElemBit | kArrayElemMask
                   | kMemberBit | kMemberSubMask));
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
        setAppStatus("MCP server stopped");
    } else {
        m_mcp->start();
        m_mcpAction->setText("Stop &MCP Server");
        setAppStatus("MCP server listening on pipe: ReclassMcpBridge");
    }
}

void MainWindow::applyTheme(const Theme& theme) {
    applyGlobalTheme(theme);

#ifdef __APPLE__
    applyMacTitleBarTheme(this, theme);
#endif

    // Dock separator is 1px via PM_DockWidgetSeparatorExtent in MenuBarStyle

    // Custom title bar
    if (m_titleBar)
        m_titleBar->applyTheme(theme);

    // Start page
    if (m_startPage)
        m_startPage->applyTheme(theme);

    // Update border overlay color
    updateBorderColor(isActiveWindow() ? theme.borderFocused : theme.border);

    // Style doc dock tab bars and remove dock borders.
    // QWidget default colors are required because having ANY stylesheet on QMainWindow
    // switches children from palette-based to CSS-based rendering.
    setStyleSheet(QStringLiteral(
        "QMainWindow::separator { width: 1px; height: 1px; background: transparent; }"
        "QDockWidget { border: none; }"
        "QDockWidget > QWidget { border: none; }"));

    for (auto* tabBar : findChildren<QTabBar*>()) {
        // Only style tab bars owned directly by this QMainWindow (dock tabs),
        // skip ones inside SplitPane QTabWidgets etc.
        if (tabBar->parent() == this) {
            // No stylesheet — painting handled by MenuBarStyle (CE_TabBarTabShape/Label)
            tabBar->setStyleSheet(QString());
            tabBar->setElideMode(Qt::ElideNone);
            tabBar->setExpanding(false);
            // Set editor font so tab width sizing matches our label painting
            {
                QSettings s("Reclass", "Reclass");
                QFont tabFont(s.value("font", "JetBrains Mono").toString(), 10);
                tabFont.setFixedPitch(true);
                tabBar->setFont(tabFont);
            }
            QPalette tp = tabBar->palette();
            tp.setColor(QPalette::WindowText, theme.textDim);
            tp.setColor(QPalette::Text, theme.text);
            tp.setColor(QPalette::Window, theme.background);
            tp.setColor(QPalette::Mid, theme.hover);
            tp.setColor(QPalette::Dark, theme.border);
            tp.setColor(QPalette::Link, theme.indHoverSpan);
            tabBar->setPalette(tp);
            // Update DockTabButtons theme
            for (int i = 0; i < tabBar->count(); ++i) {
                auto* btns = qobject_cast<DockTabButtons*>(
                    tabBar->tabButton(i, QTabBar::RightSide));
                if (btns) btns->applyTheme(theme.hover);
            }
        }
    }

    // Restyle per-pane view tab bars (Reclass / C++)
    {
        QString paneTabStyle = QStringLiteral(
            "QTabBar { border: none; }"
            "QTabBar::tab {"
            "  background: %1; color: %2; padding: 0px 16px; border: none; border-radius: 0px; height: 24px;"
            "}"
            "QTabBar::tab:selected { color: %3; background: %4;"
            "  border-top: 3px solid %6; padding-top: -3px; }"
            "QTabBar::tab:hover { color: %3; background: %5; }")
            .arg(theme.background.name(), theme.textMuted.name(), theme.text.name(),
                 theme.backgroundAlt.name(), theme.hover.name(), theme.indHoverSpan.name());
        for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
            for (auto& pane : it->panes) {
                if (pane.tabWidget)
                    pane.tabWidget->setStyleSheet(paneTabStyle);
            }
        }
    }

    // Status bar
    {
        QPalette sbPal = statusBar()->palette();
        sbPal.setColor(QPalette::Window, theme.background);
        sbPal.setColor(QPalette::WindowText, theme.textDim);
        statusBar()->setPalette(sbPal);
    }
    // Status bar chrome
    {
        auto* fsb = static_cast<FlatStatusBar*>(statusBar());
        fsb->setTopLineColor(theme.border);
        fsb->setDividerColor(theme.border);
    }
    // Resize grip (direct child of main window, not in status bar)
    if (auto* w = findChild<QWidget*>("resizeGrip"))
        static_cast<ResizeGrip*>(w)->setGripColor(theme.textFaint);

    // Workspace tree: colors from theme (selection + text)
    if (m_workspaceTree) {
        QPalette tp = m_workspaceTree->palette();
        tp.setColor(QPalette::Text, theme.textDim);
        tp.setColor(QPalette::Highlight, theme.hover);
        tp.setColor(QPalette::HighlightedText, theme.text);
        m_workspaceTree->setPalette(tp);
    }
    if (m_workspaceSearch) {
        m_workspaceSearch->setStyleSheet(QStringLiteral(
            "QLineEdit { background: %1; color: %2; border: none;"
            " border-bottom: 1px solid %3; padding: 4px 6px; }")
            .arg(theme.background.name(), theme.textDim.name(), theme.border.name()));
    }

    // Dock titlebar: restyle via palette + close button
    if (m_dockTitleLabel) {
        QPalette lp = m_dockTitleLabel->palette();
        lp.setColor(QPalette::WindowText, theme.textDim);
        m_dockTitleLabel->setPalette(lp);
    }
    if (auto* titleBar = m_workspaceDock ? m_workspaceDock->titleBarWidget() : nullptr) {
        QPalette tbPal = titleBar->palette();
        tbPal.setColor(QPalette::Window, theme.backgroundAlt);
        titleBar->setPalette(tbPal);
    }
    if (m_dockCloseBtn)
        m_dockCloseBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: none; padding: 0px 4px 2px 4px; font-size: 12px; }"
            "QToolButton:hover { color: %2; }")
            .arg(theme.textDim.name(), theme.indHoverSpan.name()));
    if (m_dockGrip)
        m_dockGrip->setGripColor(theme.textFaint);

    // Scanner dock
    if (m_scannerPanel)
        m_scannerPanel->applyTheme(theme);
    if (m_scanDockTitle) {
        QPalette lp = m_scanDockTitle->palette();
        lp.setColor(QPalette::WindowText, theme.textDim);
        m_scanDockTitle->setPalette(lp);
    }
    if (auto* titleBar = m_scannerDock ? m_scannerDock->titleBarWidget() : nullptr) {
        QPalette tbPal = titleBar->palette();
        tbPal.setColor(QPalette::Window, theme.backgroundAlt);
        titleBar->setPalette(tbPal);
    }
    if (m_scanDockCloseBtn)
        m_scanDockCloseBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: none; padding: 0px 4px 2px 4px; font-size: 12px; }"
            "QToolButton:hover { color: %2; }")
            .arg(theme.textDim.name(), theme.indHoverSpan.name()));
    if (m_scanDockGrip)
        m_scanDockGrip->setGripColor(theme.textFaint);

    // Doc dock floating title bars
    for (auto* dock : m_docDocks) {
        // The float title bar is stored alongside the empty one; find by object name
        for (auto* child : dock->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (auto* lbl = child->findChild<QLabel*>("dockFloatTitle")) {
                // Restyle the float title bar background
                QPalette tbPal = child->palette();
                tbPal.setColor(QPalette::Window, theme.backgroundAlt);
                child->setPalette(tbPal);
                // Label color
                QPalette lp = lbl->palette();
                lp.setColor(QPalette::WindowText, theme.textDim);
                lbl->setPalette(lp);
            }
            if (auto* closeBtn = child->findChild<QToolButton*>("dockFloatClose")) {
                closeBtn->setStyleSheet(QStringLiteral(
                    "QToolButton { color: %1; border: none; padding: 0px 4px 2px 4px; font-size: 12px; }"
                    "QToolButton:hover { color: %2; }")
                    .arg(theme.textDim.name(), theme.indHoverSpan.name()));
            }
            if (auto* gripW = child->findChild<QWidget*>("dockFloatGrip")) {
                if (auto* grip = dynamic_cast<DockGripWidget*>(gripW))
                    grip->setGripColor(theme.textFaint);
            }
        }
    }

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
    current.menuBarTitleCase = m_menuBarTitleCase;
    current.showIcon = m_titleBar
        ? QSettings("Reclass", "Reclass").value("showIcon", false).toBool()
        : false;
    current.autoStartMcp = QSettings("Reclass", "Reclass").value("autoStartMcp", true).toBool();
    current.refreshMs = QSettings("Reclass", "Reclass").value("refreshMs", 660).toInt();
    current.generatorAsserts = QSettings("Reclass", "Reclass").value("generatorAsserts", false).toBool();
    current.braceWrap = QSettings("Reclass", "Reclass").value("braceWrap", false).toBool();

    OptionsDialog dlg(current, this);
    if (dlg.exec() != QDialog::Accepted) return; // OptionsDialog doesn't apply anything. Only apply on OK

    auto r = dlg.result();

    if (r.themeIndex != current.themeIndex)
        tm.setCurrent(r.themeIndex);

    if (r.fontName != current.fontName)
        setEditorFont(r.fontName);

    if (r.menuBarTitleCase != current.menuBarTitleCase) {
        applyMenuBarTitleCase(r.menuBarTitleCase);
        QSettings("Reclass", "Reclass").setValue("menuBarTitleCase", r.menuBarTitleCase);
    }

    if (r.showIcon != current.showIcon) {
        if (m_titleBar)
            m_titleBar->setShowIcon(r.showIcon);
        QSettings("Reclass", "Reclass").setValue("showIcon", r.showIcon);
    }

    if (r.autoStartMcp != current.autoStartMcp)
        QSettings("Reclass", "Reclass").setValue("autoStartMcp", r.autoStartMcp);

    if (r.refreshMs != current.refreshMs) {
        QSettings("Reclass", "Reclass").setValue("refreshMs", r.refreshMs);
        for (auto& tab : m_tabs)
            tab.ctrl->setRefreshInterval(r.refreshMs);
    }

    if (r.generatorAsserts != current.generatorAsserts)
        QSettings("Reclass", "Reclass").setValue("generatorAsserts", r.generatorAsserts);

    if (r.braceWrap != current.braceWrap) {
        QSettings("Reclass", "Reclass").setValue("braceWrap", r.braceWrap);
        for (auto& tab : m_tabs)
            tab.ctrl->setBraceWrap(r.braceWrap);
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
    // Sync scanner panel font
    if (m_scannerPanel)
        m_scannerPanel->setEditorFont(f);
    if (m_scanDockTitle)
        m_scanDockTitle->setFont(f);
    // Sync doc dock float title fonts
    for (auto* dock : m_docDocks) {
        if (auto* lbl = dock->findChild<QLabel*>("dockFloatTitle"))
            lbl->setFont(f);
    }
    // Update dock tab bar font so tab sizing matches label painting
    {
        QFont tabFont(fontName, 10);
        tabFont.setFixedPitch(true);
        for (auto* tabBar : findChildren<QTabBar*>()) {
            if (tabBar->parent() == this) {
                tabBar->setFont(tabFont);
                tabBar->update();
            }
        }
    }
}

RcxController* MainWindow::activeController() const {
    if (m_activeDocDock && m_tabs.contains(m_activeDocDock))
        return m_tabs[m_activeDocDock].ctrl;
    return nullptr;
}

MainWindow::TabState* MainWindow::activeTab() {
    if (m_activeDocDock && m_tabs.contains(m_activeDocDock))
        return &m_tabs[m_activeDocDock];
    return nullptr;
}

MainWindow::TabState* MainWindow::tabByIndex(int index) {
    if (index < 0 || index >= m_docDocks.size()) return nullptr;
    auto* dock = m_docDocks[index];
    if (m_tabs.contains(dock))
        return &m_tabs[dock];
    return nullptr;
}

void MainWindow::updateWindowTitle() {
#ifdef __APPLE__
    setWindowTitle(QStringLiteral("Reclass"));
#else
    QString title;
    auto* activeDock = m_activeDocDock;
    if (activeDock && m_tabs.contains(activeDock)) {
        auto& tab = m_tabs[activeDock];
        QString name = tab.doc->filePath.isEmpty()
                       ? rootName(tab.doc->tree, tab.ctrl->viewRootId())
                       : QFileInfo(tab.doc->filePath).fileName();
        if (tab.doc->modified) name += " *";
        title = name + " - Reclass";
    } else {
        title = "Reclass";
    }
    setWindowTitle(title);
#endif
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

void MainWindow::syncViewButtons(ViewMode /*mode*/) {
    // View toggle is now per-pane via QTabWidget tab bar — nothing to sync globally
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
        selId &= ~(kFooterIdBit | kArrayElemBit | kArrayElemMask
                   | kMemberBit | kMemberSubMask);
        rootId = findRootStructForNode(tab.doc->tree, selId);
    }

    // Fall back to the controller's current view root (set by double-click / navigation)
    if (rootId == 0)
        rootId = findRootStructForNode(tab.doc->tree, tab.ctrl->viewRootId());

    // Last resort: first root-level struct in the project
    if (rootId == 0) {
        for (const auto& n : tab.doc->tree.nodes) {
            if (n.parentId == 0 && n.kind == rcx::NodeKind::Struct) {
                rootId = n.id;
                break;
            }
        }
    }

    // Generate text
    const QHash<NodeKind, QString>* aliases =
        tab.doc->typeAliases.isEmpty() ? nullptr : &tab.doc->typeAliases;
    bool asserts = QSettings("Reclass", "Reclass").value("generatorAsserts", false).toBool();
    QString text;
    if (rootId != 0)
        text = renderCpp(tab.doc->tree, rootId, aliases, asserts);
    else
        text = renderCppAll(tab.doc->tree, aliases, asserts);

    // Scroll restoration: save if same root, reset if different
    int restoreLine = 0;
    if (rootId != 0 && rootId == pane.lastRenderedRootId) {
        restoreLine = (int)pane.rendered->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
    }
    pane.lastRenderedRootId = rootId;

    // Set text
    pane.rendered->setText(text);

    // Set horizontal scroll width to match the longest line (ignoring trailing spaces)
    {
        int maxLen = 0;
        const QStringList lines = text.split(QChar('\n'));
        for (const auto& line : lines) {
            int len = (int)line.size();
            while (len > 0 && line[len - 1] == QChar(' ')) --len;
            maxLen = std::max(maxLen, len);
        }
        QFontMetrics fm(pane.rendered->font());
        int pixelWidth = fm.horizontalAdvance(QString(maxLen, QChar('0')));
        pane.rendered->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTH,
                                     (unsigned long)qMax(1, pixelWidth));
    }

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
    bool asserts = QSettings("Reclass", "Reclass").value("generatorAsserts", false).toBool();
    QString text = renderCppAll(tab->doc->tree, aliases, asserts);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Failed",
            "Could not write to: " + path);
        return;
    }
    file.write(text.toUtf8());
    setAppStatus("Exported to " + QFileInfo(path).fileName());
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

    setAppStatus(QStringLiteral("Exported %1 classes to %2")
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

    closeAllDocDocks();
    createTab(doc);
    rebuildWorkspaceModel();
    setAppStatus(QStringLiteral("Imported %1 classes from %2")
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

    closeAllDocDocks();
    createTab(doc);
    rebuildWorkspaceModel();
    if (!m_docDocks.isEmpty()) {
        splitDockWidget(m_workspaceDock, m_docDocks.first(), Qt::Horizontal);
        resizeDocks({m_workspaceDock}, {220}, Qt::Horizontal);
    }
    m_workspaceDock->show();
    setAppStatus(QStringLiteral("Imported %1 classes from source").arg(classCount));
}

// ── Import PDB ──

void MainWindow::importPdb() {
    rcx::PdbImportDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    QString pdbPath = dlg.pdbPath();
    QVector<uint32_t> indices = dlg.selectedTypeIndices();
    if (indices.isEmpty()) return;

    QProgressDialog progress("Importing types...", "Cancel", 0, indices.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);
    bool cancelled = false;

    QString error;
    NodeTree tree = rcx::importPdbSelected(pdbPath, indices, &error,
        [&](int current, int total) -> bool {
            progress.setMaximum(total);
            progress.setValue(current);
            QApplication::processEvents();
            if (progress.wasCanceled()) {
                cancelled = true;
                return false;
            }
            return true;
        });
    progress.close();

    if (tree.nodes.isEmpty()) {
        if (!cancelled)
            QMessageBox::warning(this, "Import Failed", error.isEmpty()
                ? QStringLiteral("No types imported") : error);
        return;
    }

    int classCount = 0;
    for (const auto& n : tree.nodes)
        if (n.parentId == 0 && n.kind == rcx::NodeKind::Struct) classCount++;

    auto* doc = new rcx::RcxDocument(this);
    doc->tree = std::move(tree);

    closeAllDocDocks();
    createTab(doc);
    rebuildWorkspaceModel();
    if (!m_docDocks.isEmpty()) {
        splitDockWidget(m_workspaceDock, m_docDocks.first(), Qt::Horizontal);
        resizeDocks({m_workspaceDock}, {220}, Qt::Horizontal);
    }
    m_workspaceDock->show();
    setAppStatus(QStringLiteral("Imported %1 classes from %2")
        .arg(classCount).arg(QFileInfo(pdbPath).fileName()));
}

// ── Type Aliases Dialog ──

void MainWindow::showTypeAliasesDialog() {
    auto* tab = activeTab();
    if (!tab) return;

    QDialog dlg(this);
    dlg.setWindowTitle("Type Aliases");
    dlg.resize(400, 380);

    auto* layout = new QVBoxLayout(&dlg);

    // Preset buttons (stdint + Windows only, no redundant Reset)
    auto* presetRow = new QHBoxLayout;
    auto* btnStdint  = new QPushButton("stdint (C99)", &dlg);
    auto* btnWindows = new QPushButton("Windows (basetsd.h)", &dlg);
    presetRow->addWidget(btnStdint);
    presetRow->addWidget(btnWindows);
    presetRow->addStretch();
    layout->addLayout(presetRow);

    auto* table = new QTableWidget(&dlg);
    table->setColumnCount(2);
    table->horizontalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);

    // Skip types that nobody aliases (Vec, Mat, Struct, Array)
    auto shouldSkip = [](NodeKind k) {
        return k == NodeKind::Vec2  || k == NodeKind::Vec3
            || k == NodeKind::Vec4  || k == NodeKind::Mat4x4
            || k == NodeKind::Struct || k == NodeKind::Array;
    };

    // Build filtered row→meta index mapping
    QVector<int> rowMap;
    int totalMeta = static_cast<int>(std::size(kKindMeta));
    for (int i = 0; i < totalMeta; i++)
        if (!shouldSkip(kKindMeta[i].kind)) rowMap.append(i);

    table->setRowCount(rowMap.size());
    for (int row = 0; row < rowMap.size(); row++) {
        const auto& meta = kKindMeta[rowMap[row]];
        auto* kindItem = new QTableWidgetItem(QString::fromLatin1(meta.name));
        kindItem->setFlags(kindItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 0, kindItem);

        QString alias = tab->doc->typeAliases.value(meta.kind);
        table->setItem(row, 1, new QTableWidgetItem(alias));
    }

    // stdint preset: actual typeName values from kKindMeta
    static QHash<NodeKind, QString> kStdintPreset;
    if (kStdintPreset.isEmpty()) {
        for (const auto& m : kKindMeta)
            kStdintPreset[m.kind] = QString::fromLatin1(m.typeName);
    }

    // Windows (basetsd.h) preset mapping
    static const QHash<NodeKind, QString> kWindowsPreset = {
        {NodeKind::Int8,      QStringLiteral("CHAR")},
        {NodeKind::Int16,     QStringLiteral("SHORT")},
        {NodeKind::Int32,     QStringLiteral("LONG")},
        {NodeKind::Int64,     QStringLiteral("LONGLONG")},
        {NodeKind::UInt8,     QStringLiteral("UCHAR")},
        {NodeKind::UInt16,    QStringLiteral("USHORT")},
        {NodeKind::UInt32,    QStringLiteral("ULONG")},
        {NodeKind::UInt64,    QStringLiteral("ULONGLONG")},
        {NodeKind::Float,     QStringLiteral("FLOAT")},
        {NodeKind::Double,    QStringLiteral("DOUBLE")},
        {NodeKind::Bool,      QStringLiteral("BOOLEAN")},
        {NodeKind::Pointer32, QStringLiteral("ULONG")},
        {NodeKind::Pointer64, QStringLiteral("ULONG_PTR")},
        {NodeKind::FuncPtr32, QStringLiteral("ULONG")},
        {NodeKind::FuncPtr64, QStringLiteral("ULONG_PTR")},
        {NodeKind::Hex8,      QStringLiteral("BYTE")},
        {NodeKind::Hex16,     QStringLiteral("WORD")},
        {NodeKind::Hex32,     QStringLiteral("DWORD")},
        {NodeKind::Hex64,     QStringLiteral("DWORD64")},
        {NodeKind::UTF8,      QStringLiteral("CHAR[]")},
        {NodeKind::UTF16,     QStringLiteral("WCHAR[]")},
    };

    auto applyPreset = [&](const QHash<NodeKind, QString>& preset) {
        for (int row = 0; row < rowMap.size(); row++)
            table->item(row, 1)->setText(preset.value(kKindMeta[rowMap[row]].kind));
    };

    connect(btnStdint,  &QPushButton::clicked, [&]() { applyPreset(kStdintPreset); });
    connect(btnWindows, &QPushButton::clicked, [&]() { applyPreset(kWindowsPreset); });

    layout->addWidget(table);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    // Collect new aliases
    QHash<NodeKind, QString> newAliases;
    for (int row = 0; row < rowMap.size(); row++) {
        QString val = table->item(row, 1)->text().trimmed();
        if (!val.isEmpty())
            newAliases[kKindMeta[rowMap[row]].kind] = val;
    }

    tab->doc->typeAliases = newAliases;
    tab->doc->modified = true;
    tab->ctrl->refresh();
    updateWindowTitle();
}

// ── Project Lifecycle API ──

QDockWidget* MainWindow::project_new(const QString& classKeyword) {
    auto* doc = new RcxDocument(this);

    QByteArray data(256, '\0');
    doc->loadData(data);
    doc->tree.baseAddress = 0x00400000;

    buildEmptyStruct(doc->tree, classKeyword);

    // Inherit source from current tab (if any)
    auto* currentCtrl = activeController();
    if (currentCtrl && currentCtrl->document()->provider
        && currentCtrl->document()->provider->isValid()) {
        doc->provider = currentCtrl->document()->provider;
    }

    auto* dock = createTab(doc);

    // Copy saved sources to new tab's controller
    if (currentCtrl && !currentCtrl->savedSources().isEmpty()) {
        auto& newTab = m_tabs[dock];
        newTab.ctrl->copySavedSources(currentCtrl->savedSources(),
                                       currentCtrl->activeSourceIndex());
    }

    rebuildWorkspaceModel();
    return dock;
}

QDockWidget* MainWindow::project_open(const QString& path) {
    QString filePath = path;
    if (filePath.isEmpty()) {
        filePath = QFileDialog::getOpenFileName(this,
            "Open Definition", {},
            "Reclass (*.rcx)"
            ";;All (*)");
        if (filePath.isEmpty()) return nullptr;
    }

    // Detect if this is an XML-based ReClass file by checking first bytes
    bool isXml = false;
    {
        QFile probe(filePath);
        if (probe.open(QIODevice::ReadOnly)) {
            QByteArray head = probe.read(64);
            isXml = head.trimmed().startsWith("<?xml") || head.trimmed().startsWith("<ReClass");
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
        closeAllDocDocks();
        auto* dock = createTab(doc);
        rebuildWorkspaceModel();
        if (!m_docDocks.isEmpty()) {
        splitDockWidget(m_workspaceDock, m_docDocks.first(), Qt::Horizontal);
        resizeDocks({m_workspaceDock}, {220}, Qt::Horizontal);
    }
    m_workspaceDock->show();
        int classCount = 0;
        for (const auto& n : doc->tree.nodes)
            if (n.parentId == 0 && n.kind == NodeKind::Struct) classCount++;
        setAppStatus(QStringLiteral("Imported %1 classes from %2")
            .arg(classCount).arg(QFileInfo(filePath).fileName()));
        addRecentFile(filePath);
        return dock;
    }

    auto* doc = new RcxDocument(this);
    if (!doc->load(filePath)) {
        QMessageBox::warning(this, "Error", "Failed to load: " + filePath);
        delete doc;
        return nullptr;
    }

    // Close all existing tabs so the project replaces the current state
    closeAllDocDocks();

    auto* dock = createTab(doc);
    rebuildWorkspaceModel();
    if (!m_docDocks.isEmpty()) {
        splitDockWidget(m_workspaceDock, m_docDocks.first(), Qt::Horizontal);
        resizeDocks({m_workspaceDock}, {220}, Qt::Horizontal);
    }
    m_workspaceDock->show();
    addRecentFile(filePath);
    return dock;
}

bool MainWindow::project_save(QDockWidget* dock, bool saveAs) {
    if (!dock) dock = m_activeDocDock;
    if (!dock || !m_tabs.contains(dock)) return false;
    auto& tab = m_tabs[dock];

    if (saveAs || tab.doc->filePath.isEmpty()) {
        QString path = QFileDialog::getSaveFileName(this,
            "Save Definition", {}, "Reclass (*.rcx);;JSON (*.json)");
        if (path.isEmpty()) return false;
        tab.doc->save(path);
        addRecentFile(path);
    } else {
        tab.doc->save(tab.doc->filePath);
        addRecentFile(tab.doc->filePath);
    }
    updateWindowTitle();
    return true;
}

void MainWindow::project_close(QDockWidget* dock) {
    if (!dock) dock = m_activeDocDock;
    if (!dock) return;
    dock->close();
    rebuildWorkspaceModel();
}

void MainWindow::closeAllDocDocks() {
    // Take a copy since closing modifies m_docDocks via destroyed signal
    auto docks = m_docDocks;
    for (auto* dock : docks) {
        dock->setAttribute(Qt::WA_DeleteOnClose);
        dock->close();
    }
}


// ── Workspace Dock ──

void MainWindow::createWorkspaceDock() {
    m_workspaceDock = new QDockWidget("Project", this);
    m_workspaceDock->setObjectName("WorkspaceDock");
    m_workspaceDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_workspaceDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);

    // Custom titlebar: label + ✕ close button (matches MDI tab style)
    {
        const auto& t = ThemeManager::instance().current();

        auto* titleBar = new QWidget(m_workspaceDock);
        titleBar->setFixedHeight(24);
        titleBar->setAutoFillBackground(true);
        {
            QPalette tbPal = titleBar->palette();
            tbPal.setColor(QPalette::Window, t.backgroundAlt);
            titleBar->setPalette(tbPal);
        }
        auto* layout = new QHBoxLayout(titleBar);
        layout->setContentsMargins(4, 2, 2, 2);
        layout->setSpacing(4);

        m_dockGrip = new DockGripWidget(titleBar);
        layout->addWidget(m_dockGrip);

        m_dockTitleLabel = new QLabel("Project", titleBar);
        {
            QPalette lp = m_dockTitleLabel->palette();
            lp.setColor(QPalette::WindowText, t.textDim);
            m_dockTitleLabel->setPalette(lp);
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

    // Container widget: search box + tree view
    auto* dockContainer = new QWidget(m_workspaceDock);
    auto* dockLayout = new QVBoxLayout(dockContainer);
    dockLayout->setContentsMargins(0, 0, 0, 0);
    dockLayout->setSpacing(0);

    m_workspaceSearch = new QLineEdit(dockContainer);
    m_workspaceSearch->setPlaceholderText(QStringLiteral("Search..."));
    m_workspaceSearch->setClearButtonEnabled(true);
    {
        const auto& t = ThemeManager::instance().current();
        m_workspaceSearch->setStyleSheet(QStringLiteral(
            "QLineEdit { background: %1; color: %2; border: none;"
            " border-bottom: 1px solid %3; padding: 4px 6px; }")
            .arg(t.background.name(), t.textDim.name(), t.border.name()));
    }
    dockLayout->addWidget(m_workspaceSearch);

    m_workspaceTree = new QTreeView(dockContainer);
    m_workspaceModel = new QStandardItemModel(this);
    m_workspaceModel->setHorizontalHeaderLabels({"Name"});

    m_workspaceProxy = new QSortFilterProxyModel(this);
    m_workspaceProxy->setSourceModel(m_workspaceModel);
    m_workspaceProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_workspaceProxy->setRecursiveFilteringEnabled(true);

    m_workspaceTree->setModel(m_workspaceProxy);
    m_workspaceTree->setHeaderHidden(true);
    m_workspaceTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_workspaceTree->setExpandsOnDoubleClick(false);
    m_workspaceTree->setMouseTracking(true);

    connect(m_workspaceSearch, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_workspaceProxy->setFilterFixedString(text);
        if (!text.isEmpty())
            m_workspaceTree->expandAll();
        else
            m_workspaceTree->expandToDepth(0);
    });

    // Override palette: selection + hover use theme colors (not default blue)
    {
        const auto& t = ThemeManager::instance().current();
        QPalette tp = m_workspaceTree->palette();
        tp.setColor(QPalette::Text, t.textDim);
        tp.setColor(QPalette::Highlight, t.hover);
        tp.setColor(QPalette::HighlightedText, t.text);
        m_workspaceTree->setPalette(tp);
    }

    dockLayout->addWidget(m_workspaceTree);

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
        auto* dock = static_cast<QDockWidget*>(subVar.value<void*>());
        if (!dock || !m_tabs.contains(dock)) return;

        auto& tab = m_tabs[dock];
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

    // Ctrl+F focuses the workspace search field
    {
        auto* findAction = new QAction(dockContainer);
        findAction->setShortcut(QKeySequence::Find);
        findAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        dockContainer->addAction(findAction);
        connect(findAction, &QAction::triggered, this, [this]() {
            m_workspaceSearch->setFocus();
            m_workspaceSearch->selectAll();
        });
    }

    m_workspaceTree->setMinimumWidth(0);
    m_workspaceSearch->setMinimumWidth(0);
    dockContainer->setMinimumWidth(0);
    m_workspaceDock->setWidget(dockContainer);
    addDockWidget(Qt::TopDockWidgetArea, m_workspaceDock);
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
        auto* ownerDock = static_cast<QDockWidget*>(subVar.value<void*>());
        if (!ownerDock || !m_tabs.contains(ownerDock)) return;

        RcxDocument* doc = m_tabs[ownerDock].doc;
        auto& tree = doc->tree;
        int ni = tree.indexOfId(structId);
        if (ni < 0) return;

        // For child members: navigate within the owner tab and scroll
        uint64_t parentId = tree.nodes[ni].parentId;
        if (parentId != 0) {
            ownerDock->raise();
            ownerDock->show();
            m_activeDocDock = ownerDock;
            auto& tab = m_tabs[ownerDock];
            int pi = tree.indexOfId(parentId);
            if (pi >= 0) tree.nodes[pi].collapsed = false;
            tab.ctrl->setViewRootId(parentId);
            tab.ctrl->scrollToNodeId(structId);
            QTimer::singleShot(0, this, [this, ownerDock]() {
                if (!m_tabs.contains(ownerDock)) return;
                auto& t = m_tabs[ownerDock];
                if (t.activePaneIdx >= 0 && t.activePaneIdx < t.panes.size()) {
                    auto& p = t.panes[t.activePaneIdx];
                    if (p.viewMode == VM_Rendered) updateRenderedView(t, p);
                }
            });
            return;
        }

        // Root struct/enum: check if any existing tab already views this struct
        for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
            if (it->doc == doc && it->ctrl->viewRootId() == structId) {
                it.key()->raise();
                it.key()->show();
                m_activeDocDock = it.key();
                return;
            }
        }

        // Open in a new tab sharing the same document
        tree.nodes[ni].collapsed = false;
        auto* newDock = createTab(doc);
        m_tabs[newDock].ctrl->setViewRootId(structId);
        m_tabs[newDock].ctrl->refresh();
        // Set tab title to struct name
        QString structName = tree.nodes[ni].structTypeName.isEmpty()
            ? tree.nodes[ni].name : tree.nodes[ni].structTypeName;
        if (!structName.isEmpty())
            newDock->setWindowTitle(structName);
        rebuildWorkspaceModel();
    });
}

// ── Scanner Dock ──

void MainWindow::createScannerDock() {
    m_scannerDock = new QDockWidget("Scanner", this);
    m_scannerDock->setObjectName("ScannerDock");
    m_scannerDock->setAllowedAreas(
        Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
    m_scannerDock->setFeatures(
        QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
        QDockWidget::DockWidgetFloatable);

    // Custom titlebar: label + close button (matches workspace dock)
    {
        const auto& t = ThemeManager::instance().current();

        auto* titleBar = new QWidget(m_scannerDock);
        titleBar->setFixedHeight(24);
        titleBar->setAutoFillBackground(true);
        {
            QPalette tbPal = titleBar->palette();
            tbPal.setColor(QPalette::Window, t.backgroundAlt);
            titleBar->setPalette(tbPal);
        }
        auto* layout = new QHBoxLayout(titleBar);
        layout->setContentsMargins(4, 2, 2, 2);
        layout->setSpacing(4);

        m_scanDockGrip = new DockGripWidget(titleBar);
        layout->addWidget(m_scanDockGrip);

        m_scanDockTitle = new QLabel("Scanner", titleBar);
        {
            QPalette lp = m_scanDockTitle->palette();
            lp.setColor(QPalette::WindowText, t.textDim);
            m_scanDockTitle->setPalette(lp);
        }
        layout->addWidget(m_scanDockTitle);

        layout->addStretch();

        m_scanDockCloseBtn = new QToolButton(titleBar);
        m_scanDockCloseBtn->setText(QStringLiteral("\u2715"));
        m_scanDockCloseBtn->setAutoRaise(true);
        m_scanDockCloseBtn->setCursor(Qt::PointingHandCursor);
        m_scanDockCloseBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: none; padding: 0px 4px 2px 4px; font-size: 12px; }"
            "QToolButton:hover { color: %2; }")
            .arg(t.textDim.name(), t.indHoverSpan.name()));
        connect(m_scanDockCloseBtn, &QToolButton::clicked, m_scannerDock, &QDockWidget::close);
        layout->addWidget(m_scanDockCloseBtn);

        m_scannerDock->setTitleBarWidget(titleBar);
    }

    m_scannerPanel = new ScannerPanel(m_scannerDock);
    m_scannerPanel->applyTheme(ThemeManager::instance().current());
    {
        QSettings settings("Reclass", "Reclass");
        QString fontName = settings.value("font", "JetBrains Mono").toString();
        QFont f(fontName, 12);
        f.setFixedPitch(true);
        m_scannerPanel->setEditorFont(f);
        m_scanDockTitle->setFont(f);
    }
    m_scannerDock->setWidget(m_scannerPanel);
    addDockWidget(Qt::BottomDockWidgetArea, m_scannerDock);
    m_scannerDock->hide();

    // Border overlay and resize grip for floating state
    {
        auto* border = new BorderOverlay(m_scannerDock);
        border->color = ThemeManager::instance().current().borderFocused;
        border->hide();
        auto* grip = new ResizeGrip(m_scannerDock);
        grip->hide();

        connect(m_scannerDock, &QDockWidget::topLevelChanged,
                this, [this, border, grip](bool floating) {
            if (floating) {
                border->setGeometry(0, 0, m_scannerDock->width(), m_scannerDock->height());
                border->raise();
                border->show();
                grip->reposition();
                grip->raise();
                grip->show();
            } else {
                border->hide();
                grip->hide();
            }
        });
        m_scannerDock->installEventFilter(new DockBorderFilter(border, grip, m_scannerDock));
    }

    // Wire provider getter: lazily captures the active tab's provider at scan time
    m_scannerPanel->setProviderGetter([this]() -> std::shared_ptr<rcx::Provider> {
        auto* ctrl = activeController();
        return ctrl ? ctrl->document()->provider : nullptr;
    });

    // Wire bounds getter: struct base + size for "Current Struct" filter
    m_scannerPanel->setBoundsGetter([this]() -> rcx::ScannerPanel::StructBounds {
        auto* ctrl = activeController();
        if (!ctrl) return {};
        auto& tree = ctrl->document()->tree;
        uint64_t base = tree.baseAddress;
        uint64_t viewRoot = ctrl->viewRootId();
        int span = 0;
        if (viewRoot != 0) {
            span = tree.structSpan(viewRoot);
        } else {
            // Compute extent from all top-level nodes
            for (int i = 0; i < tree.nodes.size(); i++) {
                const auto& n = tree.nodes[i];
                int64_t off = tree.computeOffset(i);
                int sz = (n.kind == rcx::NodeKind::Struct || n.kind == rcx::NodeKind::Array)
                    ? tree.structSpan(n.id) : n.byteSize();
                int64_t end = off + sz;
                if (end > span) span = static_cast<int>(end);
            }
        }
        return { base, static_cast<uint64_t>(span) };
    });

    // Wire "Go to Address" to rebase the active tab
    connect(m_scannerPanel, &ScannerPanel::goToAddress, this, [this](uint64_t addr) {
        auto* ctrl = activeController();
        if (!ctrl) return;
        ctrl->document()->tree.baseAddress = addr;
        ctrl->document()->tree.baseAddressFormula.clear();
        ctrl->resetChangeTracking();
        ctrl->refresh();
    });
}

void MainWindow::rebuildAllDocs() {
    m_allDocs.clear();
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        if (!m_allDocs.contains(it.value().doc))
            m_allDocs.append(it.value().doc);
    }
}

void MainWindow::rebuildWorkspaceModel() {
    QVector<rcx::TabInfo> tabs;
    QSet<RcxDocument*> seenDocs;
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        TabState& tab = it.value();
        if (seenDocs.contains(tab.doc)) continue;  // skip duplicate doc views
        seenDocs.insert(tab.doc);
        QString name = tab.doc->filePath.isEmpty()
            ? rootName(tab.doc->tree, tab.ctrl->viewRootId())
            : QFileInfo(tab.doc->filePath).fileName();
        tabs.append({ &tab.doc->tree, name, static_cast<void*>(it.key()) });
    }
    rcx::buildProjectExplorer(m_workspaceModel, tabs);
    m_workspaceTree->expandToDepth(0);
}

void MainWindow::addRecentFile(const QString& path) {
    if (path.isEmpty()) return;
    QString absPath = QFileInfo(path).absoluteFilePath();

    QSettings s("Reclass", "Reclass");
    QStringList recent = s.value("recentFiles").toStringList();
    recent.removeAll(absPath);
    recent.prepend(absPath);
    while (recent.size() > 10)
        recent.removeLast();
    s.setValue("recentFiles", recent);

    updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu() {
    if (!m_recentFilesMenu) return;
    m_recentFilesMenu->clear();

    QSettings s("Reclass", "Reclass");
    QStringList recent = s.value("recentFiles").toStringList();

    int added = 0;
    for (const QString& path : recent) {
        if (!QFile::exists(path)) continue;
        QString label = QStringLiteral("&%1  %2").arg(added + 1).arg(QFileInfo(path).fileName());
        m_recentFilesMenu->addAction(label, this, [this, path]() {
            project_open(path);
        })->setToolTip(path);
        if (++added >= 10) break;
    }
    if (added == 0) {
        auto* empty = m_recentFilesMenu->addAction(QStringLiteral("(empty)"));
        empty->setEnabled(false);
    }
}

void MainWindow::populateSourceMenu() {
    m_sourceMenu->clear();
    auto* ctrl = activeController();

    // Icon map for known provider identifiers
    static const QHash<QString, QString> s_providerIcons = {
        {QStringLiteral("processmemory"),          QStringLiteral(":/vsicons/server-process.svg")},
        {QStringLiteral("remoteprocessmemory"),    QStringLiteral(":/vsicons/remote.svg")},
        {QStringLiteral("windbgmemory"),           QStringLiteral(":/vsicons/debug.svg")},
        {QStringLiteral("reclass.netcompatlayer"), QStringLiteral(":/vsicons/plug.svg")},
    };

    auto addSourceAction = [this](const QString& text, const QIcon& icon, auto&& slot) {
        auto* act = m_sourceMenu->addAction(icon, text);
        act->setIconVisibleInMenu(true);
        connect(act, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return act;
    };

    addSourceAction(QStringLiteral("File"),
                    makeIcon(QStringLiteral(":/vsicons/file-binary.svg")),
                    [this]() {
        if (auto* c = activeController()) c->selectSource(QStringLiteral("File"));
    });

    const auto& providers = ProviderRegistry::instance().providers();
    for (const auto& prov : providers) {
        QString name = prov.name;
        auto it = s_providerIcons.constFind(prov.identifier);
        QIcon icon = makeIcon(it != s_providerIcons.constEnd() ? *it
                              : QStringLiteral(":/vsicons/extensions.svg"));

        QString label = prov.dllFileName.isEmpty()
            ? name
            : QStringLiteral("%1  (%2)").arg(name, prov.dllFileName);

        addSourceAction(label, icon, [this, name]() {
            if (auto* c = activeController()) c->selectSource(name);
        });
    }

    if (ctrl && !ctrl->savedSources().isEmpty()) {
        m_sourceMenu->addSeparator();
        for (int i = 0; i < ctrl->savedSources().size(); i++) {
            const auto& e = ctrl->savedSources()[i];
            auto* act = m_sourceMenu->addAction(
                QStringLiteral("%1 '%2'").arg(e.kind, e.displayName));
            act->setCheckable(true);
            act->setChecked(i == ctrl->activeSourceIndex());
            connect(act, &QAction::triggered, this, [this, i]() {
                if (auto* c = activeController()) c->switchSource(i);
            });
        }
        m_sourceMenu->addSeparator();
        auto* clearAct = addSourceAction(QStringLiteral("Clear All"),
                        makeIcon(QStringLiteral(":/vsicons/clear-all.svg")),
                        [this]() {
            if (auto* c = activeController()) c->clearSources();
        });
        Q_UNUSED(clearAct);
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
                setAppStatus("Plugin loaded successfully");
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
                setAppStatus("Plugin unloaded");
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
    if (event->type() == QEvent::WindowStateChange && m_titleBar)
        m_titleBar->updateMaximizeIcon();
    // Keep border overlay on top after any state change
    if (m_borderOverlay) {
        m_borderOverlay->setGeometry(rect());
        m_borderOverlay->raise();
    }
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

void MainWindow::showStartPage() {
    if (m_startPage) return;

    m_startPage = new StartPageWidget(this);
    m_startPage->applyTheme(ThemeManager::instance().current());

    // Size the popup to ~90% of the main window
    QSize sz(qBound(900, int(width() * 0.9), width() - 20),
             qBound(560, int(height() * 0.85), height() - 20));
    m_startPage->setFixedSize(sz);

    // Wire start page signals — each closes the dialog then performs action
    connect(m_startPage, &StartPageWidget::openProject, this, [this]() {
        dismissStartPage();
        openFile();
        if (m_tabs.isEmpty()) showStartPage();
    });
    connect(m_startPage, &StartPageWidget::newClass, this, [this]() {
        dismissStartPage();
        newClass();
    });
    connect(m_startPage, &StartPageWidget::importSource, this, [this]() {
        dismissStartPage();
        importFromSource();
        if (m_tabs.isEmpty()) showStartPage();
    });
    connect(m_startPage, &StartPageWidget::importXml, this, [this]() {
        dismissStartPage();
        importReclassXml();
        if (m_tabs.isEmpty()) showStartPage();
    });
    connect(m_startPage, &StartPageWidget::importPdb, this, [this]() {
        dismissStartPage();
        importPdb();
        if (m_tabs.isEmpty()) showStartPage();
    });
    connect(m_startPage, &StartPageWidget::continueClicked, this, [this]() {
        dismissStartPage();
        selfTest();
    });
    connect(m_startPage, &StartPageWidget::fileSelected, this, [this](const QString& path) {
        dismissStartPage();
        project_open(path);
    });
    connect(m_startPage, &QDialog::rejected, this, [this]() {
        dismissStartPage();
    });

    // Center over main window and show as application-modal
    m_startPage->move(geometry().center() - m_startPage->rect().center());
    m_startPage->open();
}

void MainWindow::dismissStartPage() {
    if (!m_startPage) return;
    m_startPage->close();
    m_startPage->deleteLater();
    m_startPage = nullptr;
}

} // namespace rcx

// ── Entry point ──

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#endif
#ifdef Q_OS_MACOS
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
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

    // Show VS2022-style start page instead of jumping straight to demo
    QMetaObject::invokeMethod(&window, "showStartPage", Qt::QueuedConnection);

    return app.exec();
}

