#include "mainwindow.h"
#include <cstdio>
#include "tab_source_icon.h"
#include "profiler.h"
#include "profilerdialog.h"
#include "typeselectorpopup.h"
#include "providerregistry.h"
#include "providers/file_provider.h"
#include <QElapsedTimer>
#include <QInputDialog>
#include "generator.h"
#include "imports/import_reclass_xml.h"
#include "imports/import_source.h"
#include "imports/export_reclass_xml.h"
#include "imports/import_pdb.h"
#include "symbolstore.h"
#include "symbol_downloader.h"
#include "imports/pe_debug_info.h"
#include "mcp/mcp_bridge.h"
#include "gotoaddressdialog.h"
#include "commandpalette.h"
#include "rtti.h"
#include "rttibrowser.h"
#include "rcxtooltip.h"
#include "tooltip_bridge.h"
#include "paintutil.h"
#include <QApplication>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QSlider>
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
#include "ribbon.h"
#include "ribbon_actions.h"
#include "ribbon_icons.h"   // tintedSvgIcon: the Edit menu shares the ribbon's icons
#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QDialog>
#include <QProgressDialog>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexercpp.h>
#include "code_highlight.h"
#include <QProxyStyle>
#include <QDesktopServices>
#include <QClipboard>
#include <QGuiApplication>
#include <QScreen>
#include <QPixmap>
#include <QWindow>
#include <QMouseEvent>
#include <QScrollBar>
#include <QShortcut>
#include "themes/thememanager.h"
#include "svgicon.h"
#include "themes/themeeditor.h"
#include "optionsdialog.h"
#include "widgets/themed_messagebox.h"
#include "widgets/resize_edges.h"
#include "widgets/themed_inputdialog.h"
#include "widgets/themed_dialog.h"
#include "widgets/dialog_button.h"
#include "widgets/unified_symbol_panel.h"
#include "widgets/empty_overlay.h"
#include "widgets/pane_tabs.h"
#include "widgets/timeline_strip.h"
#include "timeline/capture_context.h"
#include "timeline/timeline_hub.h"
#include "timeline/tl_clock.h"
#include "timeline/tl_model.h"
#include "timeline/timeline_service.h"
#include "demo_simulation.h"
#include "widgets/font_choices.h"
#include "widgets/selection_status.h"
#include "widgets/dock_header.h"
#include "widgets/panel_search_field.h"
#include "widgets/section_header.h"
#include "names/name_registry.h"
#include "names/pdb_name_provider.h"
#include "names/pdb_type_provider.h"
#include "names/rtti_name_provider.h"
#include "names/bookmark_name_provider.h"
#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <cstdio>
#include <io.h>      // _get_osfhandle / _fileno — detect redirected std streams

// ── On-demand console (backs View ▸ Show Console) ──
// Reclass is a GUI (windows) subsystem app — see WIN32_EXECUTABLE in
// CMakeLists — so launching pops NO console and there is no startup flash;
// there is simply nothing to hide. stderr still reaches an inheriting
// terminal or the test harness's pipe (crash dumps, --screenshot/--profile,
// QT_FORCE_STDERR_LOGGING) because the CRT wires fd 1/2 to inherited handles
// regardless of subsystem. We only conjure a console when the user asks for
// one (the menu) via AllocConsole, and we remember that WE created it so we
// never tear down a console inherited from a parent shell.
static bool g_rcxConsoleAllocated = false;

// True if the CRT stream's fd is bound to a pipe or disk file — i.e. the user
// or a harness redirected it (`2> log`, or the --screenshot test pipe). We must
// NOT freopen such a stream onto CONOUT$: that would steal already-captured
// output (e.g. the --profile dump) into a throwaway console window.
static bool rcxStreamRedirected(FILE* stream) {
    intptr_t osf = _get_osfhandle(_fileno(stream));
    if (osf == -1 || osf == -2) return false;            // not associated
    DWORD ft = GetFileType(reinterpret_cast<HANDLE>(osf));
    return ft == FILE_TYPE_PIPE || ft == FILE_TYPE_DISK;
}

static void rcxSetConsoleVisible(bool visible) {
    if (visible) {
        if (!GetConsoleWindow() && AllocConsole()) {
            g_rcxConsoleAllocated = true;
            // Disable the console's [X] button: closing the console window
            // sends CTRL_CLOSE_EVENT, which by default TERMINATES the app
            // (losing unsaved work). Users dismiss it via View > Show Console.
            if (HWND con = GetConsoleWindow())
                if (HMENU sys = GetSystemMenu(con, FALSE))
                    DeleteMenu(sys, SC_CLOSE, MF_BYCOMMAND);
            // Point stdout/stderr at the new console so fprintf/qDebug land in
            // it — but ONLY for streams that weren't already redirected to a
            // pipe/file, so a `2> log` or the --screenshot capture pipe keeps
            // receiving output instead of having it stolen into the console.
            if (!rcxStreamRedirected(stdout)) { FILE* f = freopen("CONOUT$", "w", stdout); (void)f; }
            if (!rcxStreamRedirected(stderr)) { FILE* f = freopen("CONOUT$", "w", stderr); (void)f; }
        }
        if (HWND h = GetConsoleWindow())
            ShowWindow(h, SW_SHOW);
    } else if (g_rcxConsoleAllocated) {
        // Only close a console we created — never an inherited shell/pipe.
        FreeConsole();
        g_rcxConsoleAllocated = false;
    }
}

static void setDarkTitleBar(QWidget* widget) {
    // One-bit "please use the dark chrome" hint to DWM. Nothing more.
    // We deliberately don't call DWMWA_CAPTION_COLOR / DWMWA_TEXT_COLOR
    // — forcing explicit RGB into the Win32 caption fought with the
    // OS's own focus/accent painting and produced muddy results
    // ("gross orange" against our dark palette). Match what
    // QMessageBox gets: immersive dark mode hint, OS picks the paint.
    // Requires Windows 10 1809+ (build 17763).
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

    // Phase 1: always-safe output (no allocations, no complex APIs).
    // GUI-subsystem build: if launched console-less this stderr goes nowhere,
    // but the full minidump below is still written next to the exe — that's
    // the authoritative crash artifact. A terminal/Show-Console run sees this.
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
                   L"%sRC_crash_%04d%02d%02d_%02d%02d%02d.dmp",
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

// ── POSIX crash handler (Linux + macOS) ──
// Mirrors the Windows path above: on fatal signal, print cause + a backtrace
// to stderr and to $HOME/.reclass/crash_YYYYMMDD_HHMMSS.log, then re-raise
// with the default handler so the OS can still dump a core file / CrashReporter.
#if defined(__linux__) || defined(__APPLE__)
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

static volatile sig_atomic_t s_inPosixCrashHandler = 0;

static const char* posixSigName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    default:      return "unknown";
    }
}

static void posixCrashHandler(int sig, siginfo_t* info, void* /*uctx*/) {
    if (s_inPosixCrashHandler) {
        // Re-entrant crash in the handler itself — give up and default.
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    s_inPosixCrashHandler = 1;

    // Phase 1: always-safe output to stderr.
    fprintf(stderr, "\n=== UNHANDLED SIGNAL ===\n");
    fprintf(stderr, "Signal : %s (%d)\n", posixSigName(sig), sig);
    fprintf(stderr, "Addr   : %p\n", info ? info->si_addr : nullptr);
    fflush(stderr);

    // Phase 2: open the crash log file under $HOME/.reclass/.
    char logPath[1024] = {};
    const char* home = getenv("HOME");
    if (home && *home) {
        char dirPath[1024];
        snprintf(dirPath, sizeof(dirPath), "%s/.RC", home);
        mkdir(dirPath, 0700);  // ignore EEXIST
        time_t now = time(nullptr);
        struct tm tm{};
#if defined(__APPLE__) || defined(__linux__)
        localtime_r(&now, &tm);
#else
        tm = *localtime(&now);
#endif
        snprintf(logPath, sizeof(logPath),
                 "%s/crash_%04d%02d%02d_%02d%02d%02d.log",
                 dirPath,
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    FILE* logF = logPath[0] ? fopen(logPath, "w") : nullptr;
    if (logF) {
        fprintf(stderr, "Log    : %s\n", logPath);
        fprintf(logF, "=== RC crash ===\n");
        fprintf(logF, "Signal : %s (%d)\n", posixSigName(sig), sig);
        fprintf(logF, "Addr   : %p\n", info ? info->si_addr : nullptr);
        fflush(logF);
    }

    // Phase 3: backtrace via libc. backtrace(3) is async-signal-unsafe
    // strictly, but pragmatic for a best-effort crash log — we've already
    // lost correctness by the time we get here.
    void* frames[64];
    int n = backtrace(frames, 64);
    char** syms = backtrace_symbols(frames, n);
    fprintf(stderr, "\nStack trace (%d frames):\n", n);
    for (int i = 0; i < n; i++) {
        const char* s = syms ? syms[i] : "<no symbols>";
        fprintf(stderr, "  [%2d] %s\n", i, s);
        if (logF) fprintf(logF, "  [%2d] %s\n", i, s);
    }
    // syms is malloc'd; leaking is fine, we're about to terminate.

    fprintf(stderr, "=== END CRASH ===\n");
    fflush(stderr);
    if (logF) { fflush(logF); fclose(logF); }

    // Re-raise with the default handler so the OS still produces a core
    // dump / CrashReporter invocation.
    signal(sig, SIG_DFL);
    raise(sig);
}

static void installPosixCrashHandler() {
    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sa.sa_sigaction = posixCrashHandler;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGBUS, SIGILL})
        sigaction(sig, &sa, nullptr);
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

// Height of a document-dock tab (set in MenuBarStyle::sizeFromContents,
// CT_TabBarTab). The Project dock's rcx::DockHeader is a SEPARATE control sitting
// beside these tabs, but their bottom separator lines must line up to the
// pixel — so createWorkspaceDock derives the header height from this same
// constant (a QTabBar renders one extra pixel below the tab content, hence the
// +1 there). Single source of truth so the two never drift again.
// 31 = 37 shrunk ~15% (user-tuned); the Project header tracks it automatically.
static constexpr int kDocTabBarHeight = 31;
// Every dock header derives from the same number (rcx::DockHeader), so a
// drift here would silently un-align the Project header seam from the
// doc-tab seam beside it. Pinned at compile time rather than by comment.
static_assert(kDocTabBarHeight == rcx::kDockHeaderContentH,
              "dock headers and document tabs must share one height");

// The start-page "splash" is a centered card; the main window launches 33%
// LARGER than it in both width and height (window = 1.33 × splash). Both sizes
// derive from this one canonical splash size so the proportion is exact on the
// first show: window 1277×904, splash 960×680. The splash size is chosen to
// fully show the start page's content (title + search + 5 "Get started" cards
// ≈ 790×640). (Logical pixels — DPI-correct.)
static constexpr int    kSplashW          = 960;
static constexpr int    kSplashH          = 680;
static constexpr double kWindowOverSplash = 1.33;

// Device-pixel-exact edge fills — see src/paintutil.h for the clip-rounding
// rules (and tests/test_hairline_dpr.cpp for the phase-sweep regression pin).
using rcx::fillBottomDeviceRowOfRect;
using rcx::fillLeftDeviceColOfRect;

// The editor/Code container outline sits against the doc-tab strip, the dock
// edge and the splitter handle — all of which already carry border-weight
// colours. At full theme.border the outline competes with them and the panes
// read as a stack of parallel lines. Blend it toward the editor paper so it
// reads as a seam between surfaces rather than a fourth line. DERIVED, not a
// new token: it tracks the theme automatically.
static constexpr qreal kContainerBorderFade = 0.35;  // 0 = full border, 1 = gone
static QColor containerBorderColor(const rcx::Theme& t) {
    const QColor a = t.border;
    const QColor b = rcx::editorPaperColor(t);
    const qreal  k = kContainerBorderFade;
    return QColor::fromRgbF(a.redF()   + (b.redF()   - a.redF())   * k,
                            a.greenF() + (b.greenF() - a.greenF()) * k,
                            a.blueF()  + (b.blueF()  - a.blueF())  * k,
                            a.alphaF());
}
using rcx::fillRightDeviceColOfRect;

class MenuBarStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    bool eventFilter(QObject* obj, QEvent* ev) override {
#ifdef _WIN32
        if (ev->type() == QEvent::Show && qobject_cast<QMenu*>(obj)) {
            auto* w = static_cast<QWidget*>(obj);
            HWND hwnd = reinterpret_cast<HWND>(w->winId());
            ULONG_PTR cs = GetClassLongPtr(hwnd, GCL_STYLE);
            if (cs & CS_DROPSHADOW)
                SetClassLongPtr(hwnd, GCL_STYLE, cs & ~CS_DROPSHADOW);
            obj->removeEventFilter(this);  // one-shot
        }
#endif
        return QProxyStyle::eventFilter(obj, ev);
    }
    void polish(QWidget* w) override {
#ifdef _WIN32
        if (qobject_cast<QMenu*>(w)) {
            w->setWindowFlag(Qt::FramelessWindowHint, true);
            w->setAttribute(Qt::WA_TranslucentBackground);
            w->installEventFilter(this);  // deferred CS_DROPSHADOW removal on first show
        }
#endif
        QProxyStyle::polish(w);
    }
    using QProxyStyle::polish;
    QSize sizeFromContents(ContentsType type, const QStyleOption* opt,
                           const QSize& sz, const QWidget* w) const override {
        QSize s = QProxyStyle::sizeFromContents(type, opt, sz, w);
        if (type == CT_MenuBarItem)
            s.setHeight(s.height() + qRound(s.height() * 0.5));
        if (type == CT_MenuItem) {
            if (auto* mi = qstyleoption_cast<const QStyleOptionMenuItem*>(opt))
                if (mi->menuItemType == QStyleOptionMenuItem::Separator)
                    return QSize(s.width(), 7);
            s = QSize(s.width() + 24, s.height() + 4);
        }
        if (type == CT_ItemViewItem)
            s.setHeight(s.height() + 4);
        // Dock tab bar: fixed height + extra width for close button
        if (type == CT_TabBarTab) {
            if (auto* tabBar = qobject_cast<const QTabBar*>(w)) {
                if (tabBar->parent() && qobject_cast<const QMainWindow*>(tabBar->parent())) {
                    s.setHeight(kDocTabBarHeight);
                    // Sentinel "+" tab: compact icon-only width
                    if (auto* tab = qstyleoption_cast<const QStyleOptionTab*>(opt))
                        if (tab->text == QStringLiteral("\u200B"))
                            return QSize(32, kDocTabBarHeight);
                    // Reserve room for:
                    //   right: DockTabButtons (close x) ≈ 24px
                    //   left:  inline source icon painted in CE_TabBarTabLabel ≈ 20px
                    s.setWidth(s.width() + 24 + 20);
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
        if (metric == PM_MenuVMargin)
            return 3;
        if (metric == PM_DockWidgetSeparatorExtent)
            return 4;
        if (metric == PM_DockWidgetFrameWidth)
            return 0;
        if (metric == PM_DockWidgetTitleMargin)
            return 0;
        if (metric == PM_DockWidgetTitleBarButtonMargin)
            return 0;
        // The ribbon host toolbar is a pure layout slot -- no Fusion chrome.
        if (w && w->objectName() == QLatin1String("RibbonHost")
            && (metric == PM_ToolBarFrameWidth || metric == PM_ToolBarItemMargin
                || metric == PM_ToolBarItemSpacing || metric == PM_ToolBarHandleExtent
                || metric == PM_ToolBarSeparatorExtent || metric == PM_ToolBarExtensionExtent))
            return 0;
        return QProxyStyle::pixelMetric(metric, opt, w);
    }
    QRect subElementRect(SubElement sr, const QStyleOption* opt,
                         const QWidget* w) const override {
        // Fusion's default subElementRect for SE_TabBarTabLeftButton /
        // SE_TabBarTabRightButton shifts the button UP by a couple of
        // pixels when the tab is selected (legacy "selected tab sticks
        // out" effect). That made the close X visibly jump up the
        // moment a doc tab became active. We paint a flat tab shape
        // (no protrusion), so the shift is purely cosmetic noise —
        // re-center the button rect vertically inside the full tab.
        if (sr == SE_TabBarTabLeftButton || sr == SE_TabBarTabRightButton) {
            QRect r = QProxyStyle::subElementRect(sr, opt, w);
            if (auto* tab = qstyleoption_cast<const QStyleOptionTab*>(opt)) {
                // Center vertically inside the full tab rect, then
                // nudge down 2 px so the X visually aligns with the
                // tab label baseline (pure geometric center read as
                // "too high" — Fusion places labels slightly below
                // center, the X needs to match).
                int cy = tab->rect.top()
                         + (tab->rect.height() - r.height()) / 2 + 2;
                r.moveTop(cy);
            }
            return r;
        }
        return QProxyStyle::subElementRect(sr, opt, w);
    }
    void drawPrimitive(PrimitiveElement elem, const QStyleOption* opt,
                       QPainter* p, const QWidget* w) const override {
        // Opaque fill + 1px border at the true widget edge.
        // WA_TranslucentBackground (set in polish) makes this a layered window,
        // so DWM doesn't clip any edges.
        // Dock separator — 1px border line at rest, hover highlight + accent
        if (elem == PE_IndicatorDockWidgetResizeHandle) {
            QRect r = opt->rect;
            bool vertical = r.height() > r.width();
            bool hovered  = opt->state & State_MouseOver;
            QColor bg     = opt->palette.color(QPalette::Window); // theme.background
            QColor line   = opt->palette.color(QPalette::Dark);   // theme.border
            QColor hov    = opt->palette.color(QPalette::Mid);    // theme.hover
            // Accent budget: indHoverSpan marks current/selected/checked. A
            // separator the mouse happens to be over is neither, so its hover
            // bar is textDim — a "you can grab this" cue, not a state.
            QColor accent = rcx::ThemeManager::instance().current().textDim;

            // Top/bottom horizontal separators — keep invisible (prevents double
            // border lines near menu bar and status bar)
            if (!vertical && w && (r.y() < w->height() / 4 || r.y() > w->height() * 3 / 4)) {
                p->fillRect(r, bg);
                return;
            }

            if (hovered) {
                p->fillRect(r, hov);
                // 2px accent line centered
                if (vertical)
                    p->fillRect(r.x() + (r.width() - 2) / 2, r.y(), 2, r.height(), accent);
                else
                    p->fillRect(r.x(), r.y() + (r.height() - 2) / 2, r.width(), 2, accent);
            } else {
                p->fillRect(r, bg);
                // 1px border line centered
                if (vertical)
                    p->fillRect(r.center().x(), r.y(), 1, r.height(), line);
                else
                    p->fillRect(r.x(), r.center().y(), r.width(), 1, line);
            }
            return;
        }
        // Suppress dock widget frame (removes internal padding around content)
        if (elem == PE_FrameDockWidget) {
            return;  // no frame
        }
        if (elem == PE_FrameMenu) {
            QRect r = opt->rect;
            QColor bg = opt->palette.color(QPalette::Window);
            QColor bd = opt->palette.color(QPalette::Dark);
            p->fillRect(r, bg);
            // 1px border via fillRect strips (no pen ambiguity)
            p->fillRect(r.left(), r.top(), r.width(), 1, bd);          // top
            p->fillRect(r.left(), r.bottom(), r.width(), 1, bd);       // bottom
            p->fillRect(r.left(), r.top(), 1, r.height(), bd);         // left
            p->fillRect(r.right(), r.top(), 1, r.height(), bd);        // right
            return;
        }
        // Kill the status bar item frame and panel border
        if (elem == PE_FrameStatusBarItem || elem == PE_PanelStatusBar)
            return;
        // Kill Fusion's frame outlines in dock area (prevents double borders near tab bar)
        if (elem == PE_Frame) {
            if (w && w->inherits("QsciScintilla"))
                return;
            // Suppress frame for any widget inside a QDockWidget
            for (auto* pw = w ? w->parentWidget() : nullptr; pw; pw = pw->parentWidget()) {
                if (qobject_cast<const QDockWidget*>(pw))
                    return;
                if (qobject_cast<const QMainWindow*>(pw))
                    break;
            }
        }
        // Transparent menu bar background (no CSS needed)
        if (elem == PE_PanelMenuBar)
            return;
        // Suppress every PE_FrameTabBarBase paint. The dock-tab variant
        // used to draw a 1-px line at the tab bar's bottom edge as a
        // visual separator with the editor; the editor now paints its own
        // device-exact border (EditorContainer::paintEvent), so painting
        // another line here just produced a double-line artifact at the
        // editor's top edge. The pane-tab variant (Structure/Code/Debug at
        // the bottom) never wanted a base line either — its selected tab
        // gets the shared 2-px underline. Killing the whole primitive is the
        // cleanest fix.
        if (elem == PE_FrameTabBarBase)
            return;
        if (elem == PE_FrameTabWidget)
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
                // Destructive items paint their label in theme.markerPtr, and
                // KEEP it when hovered — a red that disappears the moment you
                // point at the row is a warning that vanishes exactly when it
                // matters. QStyleOptionMenuItem carries no QAction, so the
                // action is recovered from the menu by matching this item's
                // rect against the geometry the menu just laid out.
                bool destructive = false;
                if (const auto* menu = qobject_cast<const QMenu*>(w)) {
                    for (QAction* a : menu->actions()) {
                        if (menu->actionGeometry(a) != mi->rect) continue;
                        destructive = a->property("rcxDestructive").toBool();
                        break;
                    }
                }
                const QColor destructiveInk = mi->palette.color(QPalette::LinkVisited);

                // Hover highlight — flat fill (no Fusion border) then delegate
                // for text/icon/arrow with Selected cleared
                if ((mi->state & State_Selected)) {
                    p->fillRect(mi->rect, mi->palette.color(QPalette::Highlight));
                    QStyleOptionMenuItem patched = *mi;
                    patched.state &= ~State_Selected;
                    patched.palette.setColor(QPalette::Text,
                        destructive ? destructiveInk
                                    : mi->palette.color(QPalette::Link));  // theme.indHoverSpan
                    QProxyStyle::drawControl(element, &patched, p, w);
                    return;
                }
                if (destructive) {
                    QStyleOptionMenuItem patched = *mi;
                    // Fusion reads the label colour from Text for an enabled
                    // item and ButtonText in some paths; set both rather than
                    // depend on which branch it takes.
                    patched.palette.setColor(QPalette::Text,       destructiveInk);
                    patched.palette.setColor(QPalette::ButtonText, destructiveInk);
                    patched.palette.setColor(QPalette::WindowText, destructiveInk);
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
                    bool sentinel = (tab->text == QStringLiteral("\u200B"));
                    bool selected = tab->state & State_Selected;
                    bool hovered  = tab->state & State_MouseOver;
                    // Background
                    QColor bg = tab->palette.color(QPalette::Window);      // theme.background
                    if (hovered || (sentinel && selected))
                        bg = tab->palette.color(QPalette::Mid);            // theme.hover
                    // Fill the entire tab rect. We used to leave the
                    // bottom 1 px unfilled so PE_FrameTabBarBase could
                    // paint through, but that primitive is now killed
                    // (the editorContainer hand-paints its own device-exact
                    // border — see EditorContainer::paintEvent). The
                    // unfilled strip otherwise showed the chrome bg as
                    // a stray line just above the editor border — the
                    // "double line" at the top of the editor.
                    p->fillRect(tab->rect, bg);
                    // Side borders ONLY (no top/bottom): device-exact 1px in
                    // theme.border. Right edge on every tab → single
                    // separators between adjacent tabs; left edge only on the
                    // first tab closes the outer edge. Sentinel "+" stays
                    // border-free so the last real tab's right line is the
                    // lone divider before it.
                    if (!sentinel) {
                        const QColor side = tab->palette.color(QPalette::Dark); // theme.border
                        if (tab->position == QStyleOptionTab::Beginning
                            || tab->position == QStyleOptionTab::OnlyOneTab)
                            fillLeftDeviceColOfRect(*p, QRectF(tab->rect), side);
                        fillRightDeviceColOfRect(*p, QRectF(tab->rect), side);
                    }
                    // Active-tab underline — BOTTOM edge, exactly 2 DEVICE
                    // rows. This was a 2-logical-px fillRect on the TOP edge:
                    // 3 device rows at DPR 1.25, and it pointed the opposite
                    // way from the ribbon tab row directly above it. One tab
                    // grammar now — ribbon tabs, doc tabs and pane tabs all
                    // mark "active" with the same underline. (The 2 is
                    // kUnderlineRows; it lives in ribbon.cpp until the shared
                    // copy lands in paintutil.h.)
                    if (selected && !sentinel)
                        rcx::fillBottomDeviceRowsOfRect(
                            *p, QRectF(tab->rect), 2,
                            tab->palette.color(QPalette::Link));  // theme.indHoverSpan
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
                    // Sentinel "+" tab — draw add icon instead of text
                    QString tabText = (tabIdx >= 0) ? tabBar->tabText(tabIdx) : tab->text;
                    if (tabText == QStringLiteral("\u200B")) {
                        // + tab: dim at rest, plain text on hover.
                        // Subordinate visual weight relative to the per-tab
                        // \u00D7 close button \u2014 earlier rev used full
                        // WindowText so the + competed equally with the
                        // \u00D7. Disabled.WindowText = theme.textMuted in the
                        // app palette. Hover is Text, NOT Link: the accent is
                        // reserved for current/selected, and "+" is neither.
                        bool hov = tab->state & State_MouseOver;
                        QColor fg = hov
                            ? tab->palette.color(QPalette::Text)
                            : tab->palette.color(QPalette::Disabled,
                                                 QPalette::WindowText);
                        // Center in the content area: the accent no longer eats
                        // the top 2 px, only the 1px bottom border is reserved.
                        int cx = tab->rect.left() + tab->rect.width() / 2;
                        int cy = tab->rect.top() + (tab->rect.height() - 1) / 2;
                        p->fillRect(cx - 3, cy, 7, 1, fg);  // horizontal
                        p->fillRect(cx, cy - 3, 1, 7, fg);  // vertical
                        return;
                    }

                    int rightBtnW = 0;
                    if (tabIdx >= 0) {
                        if (auto* rb = tabBar->tabButton(tabIdx, QTabBar::RightSide))
                            // size(), NOT sizeHint(). The wrapper is frozen with
                            // setFixedSize so hiding the x cannot resize the tab,
                            // but sizeHint() still comes from the LAYOUT, which
                            // skips a hidden child and collapses to 0. Reading the
                            // hint gave every non-current tab 16 px more label room
                            // than the hovered one, so long titles re-elided (lost
                            // a couple of characters) the instant you pointed at
                            // them, and grew them back on leave.
                            rightBtnW = rb->size().width() + 4;
                    }

                    // Source-status icon — read directly from the dock's
                    // own properties (set in MainWindow::refreshDocTabSourceIcon).
                    // Properties are bound to the QDockWidget object, so
                    // they survive tab reorder, tabify/untabify, and any
                    // tabData wipes Qt does internally during dock moves.
                    bool selected = tab->state & State_Selected;
                    QColor fg = selected ? tab->palette.color(QPalette::Text)
                                         : tab->palette.color(QPalette::WindowText);

                    // Use editor font from settings — needed before icon
                    // sizing to match the dropdown's fm.height()+4 sizing
                    // and to center vertically the same way Qt::AlignVCenter
                    // centers the text below.
                    QSettings s("RC", "RC");
                    QFont f(s.value("font", "JetBrains Mono").toString(), 10);
                    f.setFixedPitch(true);
                    p->setFont(f);
                    QFontMetrics fm(f);

                    // Icon size = fm.height() so its vertical extent matches
                    // the text-block Qt::AlignVCenter centers. Matching the
                    // dropdown's ONE-line icon size (sourcechooserpopup.cpp:172
                    // — `iconSz = fm.height()` for single-line entries). The
                    // earlier +4 made the icon's top sit above the text's
                    // top — visually too high. With them at the same height
                    // and same vertical center, the box edges line up.
                    const int kIconSz  = fm.height();
                    const int kIconPad = rcx::kGutter;  // one left margin, every strip
                    const int kIconGap = 6;
                    int leftInset = kIconPad;
                    if (tabIdx >= 0) {
                        QString tabText2 = tabBar->tabText(tabIdx);
                        if (auto* mw = qobject_cast<QMainWindow*>(tabBar->parent())) {
                            for (auto* dw : mw->findChildren<QDockWidget*>()) {
                                if (dw->windowTitle() != tabText2) continue;
                                QString iconPath = dw->property("rcxSourceIcon").toString();
                                if (iconPath.isEmpty()) break;
                                bool live = dw->property("rcxSourceLive").toBool();
                                // Visible content area: only the 1px bottom
                                // base-line border is reserved. The selected
                                // accent used to steal the top 2 px, so labels
                                // and icons jumped down a pixel the moment you
                                // clicked a tab; the underline sits on the
                                // bottom edge now, so every tab is one box.
                                const int topInset    = 0;
                                const int bottomInset = 1;
                                int caTop = tab->rect.top() + topInset;
                                int caH   = tab->rect.height() - topInset - bottomInset;
                                int iy    = caTop + (caH - kIconSz) / 2 + 1;  // visual nudge
                                QRect iconRect(tab->rect.left() + kIconPad, iy,
                                               kIconSz, kIconSz);
                                // Selected/unselected contrast: tab palette
                                // already gives `fg` two distinct colors, but
                                // the SVG-tinted-via-SourceIn loses some of
                                // that contrast. Drop unselected icons to 70%
                                // opacity so the active tab's icon stands
                                // out clearly. Disconnected goes further to
                                // 35% (the !live case).
                                qreal stateOpacity = selected ? 1.0 : 0.70;
                                qreal prev = p->opacity();
                                p->setOpacity(prev * stateOpacity);
                                rcx::drawTabSourceIcon(p, iconRect, iconPath, live, fg);
                                p->setOpacity(prev);
                                leftInset = kIconPad + kIconSz + kIconGap;
                                break;
                            }
                        }
                    }
                    // Match the icon's content area (see topInset above):
                    // the same box whether or not the tab is selected.
                    const int textTopInset    = 0;
                    const int textBottomInset = 1;
                    QRect textRect = tab->rect.adjusted(leftInset, textTopInset,
                                                        -(rcx::kGutter + rightBtnW),
                                                        -textBottomInset);

                    QString text = tabText;
                    int maxW = textRect.width();
                    if (fm.horizontalAdvance(text) > maxW) {
                        text = fm.elidedText(text, Qt::ElideRight, maxW);
                    }

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
#include "sourcechooserpopup.h"
#include "dockoverlay.h"

static void applyGlobalTheme(const rcx::Theme& theme) {
    PROFILE_SCOPE("applyGlobalTheme");
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
    // LinkVisited is otherwise unused, so it carries the DESTRUCTIVE colour to
    // the styles: theme.markerPtr, the same red the dialogs use for a
    // destructive button (#800000 light, #f44747 dark). Routing it through the
    // palette keeps MenuBarStyle painting from theme tokens rather than
    // reaching into ThemeManager mid-paint.
    pal.setColor(QPalette::LinkVisited,     theme.markerPtr);

    // Disabled group: Fusion reads these for disabled menu items, buttons, etc.
    pal.setColor(QPalette::Disabled, QPalette::WindowText,      theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::Text,            theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText,      theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::Light,           theme.background);

    qApp->setPalette(pal);

    // Global scrollbar styling — track matches control bg, handle is solid.
    // Also a QToolTip rule so the dozens of QToolTip::showText calls + every
    // .setToolTip() across the app render in our dark palette instead of
    // Qt's default white-balloon-with-black-text Windows look.
    qApp->setStyleSheet(QStringLiteral(
        "QScrollBar:vertical { background: palette(window); width: 8px; margin: 0; border: none; }"
        "QScrollBar::handle:vertical { background: %1; min-height: 20px; border: none; }"
        "QScrollBar::handle:vertical:hover { background: %2; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
        "QScrollBar:horizontal { background: palette(window); height: 8px; margin: 0; border: none; }"
        "QScrollBar::handle:horizontal { background: %1; min-width: 20px; border: none; }"
        "QScrollBar::handle:horizontal:hover { background: %2; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }"
        "QToolTip { background: %3; color: %4; border: 1px solid %5; padding: 5px 8px;"
        "           font-family: '%6'; font-size: 10pt; }")
        .arg(theme.textFaint.name(), theme.textDim.name(),
             theme.backgroundAlt.name(), theme.text.name(), theme.border.name(),
             QSettings("RC", "RC").value("font", "JetBrains Mono").toString()));
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
        int w = width(), h = height();
        p.fillRect(0, 0, w, 1, color);          // top
        p.fillRect(0, h - 1, w, 1, color);      // bottom
        p.fillRect(0, 0, 1, h, color);           // left
        p.fillRect(w - 1, 0, 1, h, color);       // right
    }
};

class InspectionOverlay : public QWidget {
public:
    QRect  highlightRect;   // in parent (MainWindow) coordinates
    QString label;

    explicit InspectionOverlay(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }
    void paintEvent(QPaintEvent*) override {
        if (highlightRect.isNull()) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Red highlight rectangle (2px)
        p.setPen(QPen(QColor(255, 60, 60), 2));
        p.setBrush(QColor(255, 60, 60, 20));
        p.drawRect(highlightRect.adjusted(1, 1, -1, -1));

        // Floating label above the rect
        if (!label.isEmpty()) {
            QFont f = p.font();
            f.setPointSize(9);
            p.setFont(f);
            QFontMetrics fm(f);
            QRect lr = fm.boundingRect(label).adjusted(-6, -2, 6, 2);
            lr.moveBottomLeft(highlightRect.topLeft() + QPoint(0, -3));
            if (lr.top() < 0)
                lr.moveTopLeft(highlightRect.bottomLeft() + QPoint(0, 3));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 200));
            p.drawRoundedRect(lr, 3, 3);
            p.setPen(Qt::white);
            p.drawText(lr, Qt::AlignCenter, label);
        }
    }
};

// Diagonal-hatched "empty workspace" background. Used as the QMainWindow
// central widget; it is height-pinned to 0 (invisible) while any document
// tab is open and lifted to fill the empty band only when the last tab is
// closed (see MainWindow::updateEmptyWorkspaceVisibility). The 45° hatch +
// centered hint reads as "no document here", clearly distinct from a real
// editor surface so the user knows the workspace is intentionally empty.
class WorkspaceHatch : public QWidget {
public:
    explicit WorkspaceHatch(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_NoSystemBackground);  // paintEvent owns every pixel
    }
    void paintEvent(QPaintEvent*) override {
        const auto& t = rcx::ThemeManager::instance().current();
        QPainter p(this);
        // Base = editor "paper" surface so the band reads as workspace, not
        // chrome; the diagonal hatch on top marks it as a non-document area.
        // t.border at ~30% alpha over the paper surface: visible enough to
        // read as "intentionally not a document", subtle enough not to
        // distract. Tune the alpha (44=whisper … 110=loud) or `gap` to taste.
        p.fillRect(rect(), rcx::editorPaperColor(t));
        QColor stripe = t.border;
        stripe.setAlpha(78);
        p.setPen(QPen(stripe, 1));
        const int gap = 12;
        for (int x = -height(); x < width(); x += gap)
            p.drawLine(x, height(), x + height(), 0);  // 45° stripes
        p.setPen(t.textMuted);
        QFont f = font();
        f.setPointSize(13);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("No document open\n\n"
                                  "File ▸ New Class    (Ctrl+N)"));
    }
};

namespace rcx {

#ifdef __APPLE__
void applyMacTitleBarTheme(QWidget* window, const Theme& theme);
#endif

// Helper: extract text from a Scintilla line (duplicated from editor.cpp where it's file-local)
static QString sciGetLineText(QsciScintilla* sci, int line) {
    int len = (int)sci->SendScintilla(QsciScintillaBase::SCI_LINELENGTH, (unsigned long)line);
    if (len <= 0) return {};
    QByteArray buf(len + 1, '\0');
    sci->SendScintilla(QsciScintillaBase::SCI_GETLINE, (unsigned long)line, (void*)buf.data());
    QString text = QString::fromUtf8(buf.data(), len);
    while (text.endsWith('\n') || text.endsWith('\r'))
        text.chop(1);
    return text;
}

// MainWindow class declaration is in mainwindow.h

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    PROFILE_SCOPE("MainWindow::ctor");
    setWindowTitle("RC");
    // Initial size +30% over the legacy 1080×720 to give docks + editor
    // more breathing room on first launch.
    resize(1080, 720);

#ifndef __APPLE__
    // Frameless window with system menu (Alt+Space) and min/max/close support.
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowSystemMenuHint
                   | Qt::WindowMinMaxButtonsHint);

    // Custom title bar (replaces native menu bar area in QMainWindow)
    m_titleBar = new TitleBarWidget(this);
    // Object-name so DockOverlay::contentRect() can find it as a fallback
    // (the primary path is QMainWindow::menuWidget() since we install it
    // via setMenuWidget below, but naming it explicitly is cheap insurance
    // against the lookup ever failing).
    m_titleBar->setObjectName(QStringLiteral("TitleBarWidget"));
    m_titleBar->applyTheme(ThemeManager::instance().current());
    setMenuWidget(m_titleBar);
    m_menuBar = m_titleBar->menuBar();
#ifdef __linux__
    m_menuBar->setNativeMenuBar(false);
#endif
#else
    setWindowTitle(QStringLiteral("RC"));
    setUnifiedTitleAndToolBarOnMac(true);
    m_menuBar = menuBar();
    m_menuBar->setNativeMenuBar(true);
    applyMacTitleBarTheme(this, ThemeManager::instance().current());
#endif

#ifdef _WIN32
    // Explicitly disable DWM drop shadow on the frameless window
    {
        auto hwnd = reinterpret_cast<HWND>(winId());
        MARGINS margins = {0, 0, 0, 0};
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

    // Central placeholder — will be replaced by start page after
    // construction. Sizing is per-axis:
    //   * Height pinned to 0 (max height = 0). Top doc dock area sits
    //     directly above central; with central max=0 the top dock fills
    //     the full vertical strip between the menu bar and bottom dock /
    //     status bar. If central had any height it would steal it from
    //     the doc dock (editor would only fill the top portion of the
    //     window, leaving a dead band below the South pane tabs).
    //   * Width left unbounded (max width = default = QWIDGETSIZE_MAX).
    //     The right-edge separator on the workspace dock drags space
    //     between the left dock area and central. With horizontal max
    //     bounded (the prior setFixedSize(0,0) bug), central refused to
    //     grow → splitter snapped right back → drag "fought" the user.
    //     With horizontal unbounded, central absorbs the freed pixels
    //     and the splitter moves freely.
    // Size policy is Ignored,Ignored: Qt allocates whatever leftover space
    // is available on both axes, bounded only by min/max. While any doc tab
    // is open the maximumHeight stays pinned to 0 (top dock fills the strip);
    // when the last tab closes updateEmptyWorkspaceVisibility() lifts the cap
    // and the WorkspaceHatch fills the empty band (Ignored vertical lets it
    // grow — a Fixed policy would clamp to the 0 sizeHint and stay invisible).
    m_centralPlaceholder = new WorkspaceHatch(this);
    m_centralPlaceholder->setMinimumSize(0, 0);
    m_centralPlaceholder->setMaximumHeight(0);
    m_centralPlaceholder->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setCentralWidget(m_centralPlaceholder);
    setDockNestingEnabled(true);
    // Give left/right docks full height (corners belong to left/right, not top/bottom)
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    // Tab strip always renders at the TOP of its dock area, regardless of
    // which side of the window the area is on. Qt's default is `South` for
    // Left/Right/Bottom areas, which made the doc-tab strip appear at the
    // bottom of the right area after a Dock-Right drop — confusing UX.
    setTabPosition(Qt::TopDockWidgetArea,    QTabWidget::North);
    setTabPosition(Qt::LeftDockWidgetArea,   QTabWidget::North);
    setTabPosition(Qt::RightDockWidgetArea,  QTabWidget::North);
    setTabPosition(Qt::BottomDockWidgetArea, QTabWidget::North);

    { PROFILE_SCOPE("MainWindow::createWorkspaceDock"); createWorkspaceDock(); }
    { PROFILE_SCOPE("MainWindow::createScannerDock");   createScannerDock();   }
    // createSymbolsDock used to run here at ~360 ms (3 trees + tab widget +
    // models + many QSS applies). The dock is hidden by default — fired
    // from QTimer::singleShot(0,…) below after window.show() instead, and
    // re-entry-guarded inside the function so the View > Modules menu
    // action can also call it the moment a user beats the timer to it.
    { PROFILE_SCOPE("MainWindow::createBookmarksDock"); createBookmarksDock(); }

    { PROFILE_SCOPE("MainWindow::createMenus");         createMenus();         }
    if (m_titleBar) {
        PROFILE_SCOPE("TitleBarWidget::finalizeMenuBar");
        m_titleBar->finalizeMenuBar();
    }
    { PROFILE_SCOPE("MainWindow::createStatusBar");     createStatusBar();     }
    { PROFILE_SCOPE("MainWindow::createRibbon");        createRibbon();        }

    // Autosave timer — writes a shadow copy of every modified doc that has
    // a known filePath, so a crash mid-edit doesn't lose work. Untitled docs
    // are skipped here; a full implementation will need an app-config dir
    // and a restore prompt on startup.
    m_autosaveTimer = new QTimer(this);
    m_autosaveTimer->setInterval(60 * 1000);
    connect(m_autosaveTimer, &QTimer::timeout, this,
            &MainWindow::autosaveAllModifiedDocs);
    m_autosaveTimer->start();

    // Eliminate gap between central widget and status bar
    if (auto* ml = layout()) {
        ml->setSpacing(0);
        ml->setContentsMargins(0, 0, 0, 0);
    }
    // Separator line between central widget and status bar is killed in MenuBarStyle::drawControl

    // Restore menu bar title case setting (after menus are created)
    {
        QSettings s("RC", "RC");
        m_menuBarTitleCase = s.value("menuBarTitleCase", false).toBool();
        applyMenuBarTitleCase(m_menuBarTitleCase);
        if (m_titleBar && s.value("showIcon", false).toBool())
            m_titleBar->setShowIcon(true);
    }

    // MenuBarStyle is set as app style in main() — covers both QMenuBar and QMenu

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const Theme& t) {
        applyTheme(t);
        // A theme's `font` field is a DEFAULT it brings with it when you
        // switch TO it (tw.json / Light asks for IBM Plex Mono) — not a lock
        // it re-imposes forever. It used to live inside applyTheme(), which
        // setEditorFont()'s own tail calls back into (the pane-tab font has
        // to go through the stylesheet), so on any theme declaring a font
        // every View > Font pick was reverted one call later, settings and
        // all. That re-entry also ran a full theme apply over a half-built
        // window at startup, which is what crashed launching with a saved
        // font the theme disagreed with. This connection fires only on a
        // real switch — never on the initial apply below, never from
        // setEditorFont — so a pick made afterwards stands, restarts
        // included.
        if (!t.font.isEmpty() && rcx::RcxEditor::globalFontName() != t.font)
            setEditorFont(t.font);
    });

    // Apply theme once at startup (the signal only fires on change, not initial load)
    {
        PROFILE_SCOPE("MainWindow::applyTheme(initial)");
        applyTheme(ThemeManager::instance().current());
    }

    // Plugin loading was synchronous here (~40 ms of dlopen). Deferred
    // out of the ctor — see loadPluginsDeferred() below. The MCP bridge
    // also waits on plugins so it's started in the same deferred batch.
    m_mcp = new McpBridge(this, this);

    // Register built-in NameProviders. Order = priority for reverse-lookup
    // (PDB symbols > PDB types > RTTI > Bookmarks). Plugins can register
    // more later. Splitting PDB symbols vs PDB types into two providers
    // gives each a distinct filter chip + accent color in the Symbols
    // panel — addresses the "no visual difference between a PDB function
    // and a struct definition" feedback.
    {
        rcx::NameRegistry::instance().registerProvider(
            std::make_shared<rcx::PdbNameProvider>());
        rcx::NameRegistry::instance().registerProvider(
            std::make_shared<rcx::PdbTypeProvider>());
        rcx::NameRegistry::instance().registerProvider(
            std::shared_ptr<rcx::RttiNameProvider>(&rcx::RttiNameProvider::instance(),
                [](rcx::RttiNameProvider*){ /* singleton — don't delete */ }));
        auto bm = std::make_shared<rcx::BookmarkNameProvider>(
            [this]() -> rcx::RcxController* { return activeController(); });
        rcx::NameRegistry::instance().registerProvider(std::move(bm));
    }
    // Hook compose's RTTI walker into the registry so discoveries flow
    // into the unified Symbols panel and the expression parser. Wire the
    // unified lookup + change-nudge so controller.cpp can talk to the
    // registry without dragging it into headless test targets.
    rcx::g_rttiDiscoveryHook = [](const QString& name, uint64_t address,
                                   const QString& moduleName) {
        rcx::RttiNameProvider::instance().push(name, address, moduleName);
    };
    rcx::g_nameLookupHook = [](uint64_t addr, const rcx::Provider* active) {
        return rcx::NameRegistry::instance().nameFor(addr, active);
    };
    rcx::g_namesChangedHook = []() {
        rcx::NameRegistry::instance().emitChanged();
    };

    // Active doc tracking is handled per dock in createTab() via visibilityChanged

    // Install global event filter for Ctrl+Click UI inspection
    qApp->installEventFilter(this);

    // Dock overlay drag system
    setupDockOverlay();

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
        // Track the active DOC dock by focus — more reliable than tabbed-dock
        // visibilityChanged, which can skip firing on a tab switch. When focus
        // lands inside a different doc dock, refresh the status-bar source chip,
        // window title, and bookmarks so they track the focused tab.
        for (QWidget* w = now; w; w = w->parentWidget()) {
            auto* dk = qobject_cast<QDockWidget*>(w);
            if (dk && m_tabs.contains(dk)) {
                if (m_activeDocDock != dk)
                    setActiveDocDock(dk);
                break;
            }
        }
        auto* tab = activeTab();
        if (!tab) return;
        for (int i = 0; i < tab->panes.size(); ++i) {
            if (tab->panes[i].tabWidget && tab->panes[i].tabWidget->isAncestorOf(now)) {
                tab->activePaneIdx = i;
                return;
            }
        }
    });

    // Auto-size the window every launch instead of restoring a saved position.
    // A persisted geometry goes stale the moment the user changes resolution,
    // DPI, or monitor layout (the window can land off-screen or mis-sized);
    // computing fresh from the CURRENT primary screen avoids all of that.
    //
    // Position the edge/grip resize zones for the initial (default-resized) size.
    repositionResizeWidgets();
}

QIcon MainWindow::makeIcon(const QString& svgPath) {
    // Tinted to the live text colour. The :/vsicons Codicons bake a light-grey
    // #C5C5C5 ink chosen for dark chrome; rendering it verbatim (which this
    // used to do via a hand-rolled QSvgRenderer pass) left every menu-bar icon
    // near-invisible on the light theme, while the item text — which comes
    // from the palette — was correctly black.
    //
    // themedVsIcon caches on (path, tint, size, dpr) and falls back to a plain
    // QIcon when the file cannot be read, which also covers the "qsvgicon
    // plugin may not be deployed" case the hand-rolled path existed for.
    // Menu actions are built ONCE, so remember which path produced which icon
    // and re-tint them on a live theme switch (retintMenuIcons).
    const QColor ink = ThemeManager::instance().current().text;
    const QIcon ic = rcx::themedVsIcon(svgPath, ink, 16, devicePixelRatioF());
    m_iconPaths.insert(ic.cacheKey(), svgPath);
    return ic;
}

// Re-ink every makeIcon() icon after a live theme switch. Without this the
// menus keep the ink of whatever theme was active when the menu bar was built,
// which on a dark -> light switch is the invisible case above.
void MainWindow::retintMenuIcons(const rcx::Theme& theme) {
    if (m_iconPaths.isEmpty()) return;
    const qreal dpr = devicePixelRatioF();
    QHash<qint64, QString> rebuilt;
    for (QAction* a : findChildren<QAction*>()) {
        const auto it = m_iconPaths.constFind(a->icon().cacheKey());
        if (it == m_iconPaths.constEnd()) continue;
        const QIcon ic = rcx::themedVsIcon(*it, theme.text, 16, dpr);
        a->setIcon(ic);
        rebuilt.insert(ic.cacheKey(), *it);
    }
    // Keep the map keyed on what the actions now hold, so a second switch works.
    for (auto it = rebuilt.constBegin(); it != rebuilt.constEnd(); ++it)
        m_iconPaths.insert(it.key(), it.value());
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
    m_actNewClass = Qt5Qt6AddAction(file, "New &Class",  QKeySequence::New, QIcon(), this, &MainWindow::newClass);
    m_actNewStruct = Qt5Qt6AddAction(file, "New &Struct", QKeySequence(Qt::CTRL | Qt::Key_T), QIcon(), this, &MainWindow::newStruct);
    m_actNewEnum = Qt5Qt6AddAction(file, "New &Enum",   QKeySequence(Qt::CTRL | Qt::Key_E), QIcon(), this, &MainWindow::newEnum);
    m_actOpen = Qt5Qt6AddAction(file, "&Open...", QKeySequence::Open, makeIcon(":/vsicons/folder-opened.svg"), this, &MainWindow::openFile);
    m_recentFilesMenu = file->addMenu("Recent &Files");
    updateRecentFilesMenu();
    Qt5Qt6AddAction(file, "&Welcome Screen", QKeySequence::UnknownKey,
                    makeIcon(":/vsicons/home.svg"), this, &MainWindow::showStartPage);
    file->addSeparator();
    m_actSave = Qt5Qt6AddAction(file, "&Save", QKeySequence::Save, makeIcon(":/vsicons/save.svg"), this, &MainWindow::saveFile);
    Qt5Qt6AddAction(file, "Save &As...", QKeySequence::SaveAs, makeIcon(":/vsicons/save-as.svg"), this, &MainWindow::saveFileAs);
    file->addSeparator();
    auto* importMenu = file->addMenu("&Import");
    Qt5Qt6AddAction(importMenu, "From &Source...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::importFromSource);
    Qt5Qt6AddAction(importMenu, "ReClass XML / .NET (.&rcnet)...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::importReclassXml);
    Qt5Qt6AddAction(importMenu, "&PDB...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::importPdb);
    auto* exportMenu = file->addMenu("E&xport");
    m_exportMenu = exportMenu;
    Qt5Qt6AddAction(exportMenu, "&C++ Header...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::exportCpp);
    Qt5Qt6AddAction(exportMenu, "&Rust Structs...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::exportRust);
    Qt5Qt6AddAction(exportMenu, "#&define Offsets...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::exportDefines);
    Qt5Qt6AddAction(exportMenu, "C&# Structs...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::exportCSharp);
    Qt5Qt6AddAction(exportMenu, "&Python ctypes...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::exportPython);
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
    m_actClose = Qt5Qt6AddAction(file, "&Close Project", QKeySequence(Qt::CTRL | Qt::Key_W), QIcon(), this, &MainWindow::closeFile);
    file->addSeparator();
#ifdef _WIN32
    {
        // "Relaunch as Administrator" — hidden when already elevated
        bool elevated = false;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION elev{};
            DWORD sz = sizeof(elev);
            if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz))
                elevated = (elev.TokenIsElevated != 0);
            CloseHandle(token);
        }
        if (!elevated) {
            Qt5Qt6AddAction(file, "Relaunch as &Administrator",
                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A),
                makeIcon(":/vsicons/shield.svg"), this, [this]() {
                    wchar_t exePath[MAX_PATH];
                    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                    SHELLEXECUTEINFOW sei{};
                    sei.cbSize = sizeof(sei);
                    sei.lpVerb = L"runas";
                    sei.lpFile = exePath;
                    sei.nShow  = SW_SHOWNORMAL;
                    if (ShellExecuteExW(&sei))
                        QCoreApplication::quit();
                    // If UAC was cancelled, do nothing
                });
            file->addSeparator();
        }
    }
#endif
    m_sourceMenu = file->addMenu("&Data Source");
    connect(m_sourceMenu, &QMenu::aboutToShow, this, &MainWindow::populateSourceMenu);
    connect(m_sourceMenu, &QMenu::triggered, this, [this](QAction* act) {
        auto* c = activeController();
        if (!c) return;
        QString data = act->data().toString();
        if (data.isEmpty()) return;  // plugin actions handle themselves via lambda
        if (data == QStringLiteral("#clear"))
            c->clearSources();
        else if (data.startsWith(QStringLiteral("#saved:")))
            c->switchSource(data.mid(7).toInt());
        else
            c->selectSource(data);
    });
    file->addSeparator();
    Qt5Qt6AddAction(file, "E&xit", QKeySequence(Qt::Key_Close), makeIcon(":/vsicons/close.svg"), this, &QMainWindow::close);

    // Edit
    auto* edit = m_menuBar->addMenu("&Edit");
    // Undo / Redo = the return arrow and its mirror, the same pair the
    // title-strip quick-access buttons show. arrow-left / arrow-right here
    // meant one command had two icons depending on where you looked.
    {
        QAction* undoAct = Qt5Qt6AddAction(edit, "&Undo", QKeySequence::Undo, QIcon(), this, &MainWindow::undo);
        QAction* redoAct = Qt5Qt6AddAction(edit, "&Redo", QKeySequence::Redo, QIcon(), this, &MainWindow::redo);
        // The ink is theme-derived, so it has to follow the theme: a pair
        // tinted once at construction keeps near-white arrows on a light menu
        // after a live switch. (The title-strip twins re-tint in
        // TitleBarWidget::applyTheme.)
        auto retint = [this, undoAct, redoAct](const rcx::Theme& t) {
            const qreal dpr = devicePixelRatioF();
            undoAct->setIcon(rcx::themedVsIcon(QStringLiteral(":/vsicons/reply.svg"), t.text, 16, dpr));
            redoAct->setIcon(rcx::themedVsIcon(QStringLiteral(":/vsicons/reply.svg"), t.text, 16, dpr, true));
        };
        retint(ThemeManager::instance().current());
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, retint);
        // Same problem, every other menu icon: makeIcon() tints at build time,
        // so a live switch needs a re-ink pass over all of them.
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
                [this](const rcx::Theme& t) { retintMenuIcons(t); });
    }
    edit->addSeparator();
    Qt5Qt6AddAction(edit, "&Find Field...", QKeySequence::Find,
                    makeIcon(":/vsicons/search.svg"), this,
                    &MainWindow::showFindFieldDialog);
    edit->addSeparator();
    // Break the current selection (byte range, or selected nodes) off into a
    // new embedded class — the same op as the node menu's "Carve", but
    // reachable from the menu bar. Auto-detects bytes vs node selection.
    m_actBreakClass = Qt5Qt6AddAction(edit, "&Carve",
                    QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B),
                    makeIcon(":/vsicons/symbol-structure.svg"), this, [this]() {
        auto* c = activeController();
        if (!c) return;
        auto region = c->regionFromCurrentSelection(c->primaryEditor());
        if (!region) {
            setAppStatus(QStringLiteral("Carve: select bytes or fields first"));
            return;
        }
        c->extractByteSelectionToNewClass(region->first, region->second);
    });
    edit->addSeparator();
    Qt5Qt6AddAction(edit, "Add &Bookmark...", QKeySequence(Qt::CTRL | Qt::Key_B), QIcon(),
                    this, &MainWindow::promptAddBookmark);
    // Quick bookmark — captures the current address with an auto-generated
    // name (no dialog). Uses the formula if there is one (preserves rebases),
    // falls back to the literal hex address.
    Qt5Qt6AddAction(edit, "&Quick Bookmark Here",
                    QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_B), QIcon(), this, [this]() {
        auto* c = activeController();
        if (!c) return;
        QString formula = c->baseAddressFormula();
        if (formula.isEmpty())
            formula = QStringLiteral("0x") + QString::number(c->baseAddress(), 16).toUpper();
        // Find a free slot name: bookmark_NN
        int n = 1;
        QSet<QString> taken;
        for (const auto& bm : c->document()->tree.bookmarks) taken.insert(bm.name);
        QString name;
        do { name = QStringLiteral("bookmark_%1").arg(n++, 2, 10, QChar('0')); }
        while (taken.contains(name) && n < 1000);
        c->addBookmark(name, formula);
        refreshBookmarksDock();
        if (m_bookmarksDock) m_bookmarksDock->show();
        setAppStatus(QStringLiteral("Bookmarked: ") + name + QStringLiteral(" → ") + formula);
    });

    // View
    auto* view = m_menuBar->addMenu("&View");
#ifdef _WIN32
    // Top of View: a checkbox to summon a console window on demand. The app
    // is GUI-subsystem (no console by default) — see rcxSetConsoleVisible.
    {
        auto* actConsole = view->addAction(QStringLiteral("Show &Console"));
        m_actConsole = actConsole;
        actConsole->setCheckable(true);
        actConsole->setChecked(
            QSettings("RC", "RC").value("showConsole", false).toBool());
        connect(actConsole, &QAction::triggered, this, [](bool checked) {
            rcxSetConsoleVisible(checked);
            QSettings("RC", "RC").setValue("showConsole", checked);
        });
        view->addSeparator();
    }
#endif
    Qt5Qt6AddAction(view, "&Reset Windows", QKeySequence::UnknownKey, QIcon(), this, [this](bool) {
        // Safety button — force every dock back to its canonical area
        // regardless of how mangled the current layout is. Steps in
        // order so each operates on a clean preceding state:
        //   1. Restore the default corner config (Left/Right own all
        //      four corners, undoing any EdgeX-drop reassignment).
        //   2. Wipe every sentinel — reconcile will recreate exactly
        //      the ones we need at the end.
        //   3. addDockWidget every dock to its canonical area (this
        //      detaches each from any stale tab group it was caught in).
        //   4. Tabify all doc docks together in the top area.
        //   5. Reset visibility / sizes to a sensible default.
        //   6. reconcileDockTabBars() to restyle and re-pair sentinels.

        // (1) Default corners: Left/Right span the full window height,
        //     Top/Bottom only span between them. This is the original
        //     constructor setup.
        setCorner(Qt::TopLeftCorner,     Qt::LeftDockWidgetArea);
        setCorner(Qt::BottomLeftCorner,  Qt::LeftDockWidgetArea);
        setCorner(Qt::TopRightCorner,    Qt::RightDockWidgetArea);
        setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

        // (2) Nuke every existing sentinel synchronously — reconcile at
        //     the end will recreate fresh ones for any solo doc dock.
        for (auto* s : m_sentinelDocks) {
            if (s) { removeDockWidget(s); s->deleteLater(); }
        }
        m_sentinelDocks.clear();

        // (3) Force every dock back to its canonical area. addDockWidget
        //     on an already-placed dock removes it from its current area
        //     and re-adds — this is what breaks any "workspace tabified
        //     with a doc dock" mess.
        if (m_workspaceDock) {
            if (m_workspaceDock->isFloating()) m_workspaceDock->setFloating(false);
            addDockWidget(Qt::LeftDockWidgetArea, m_workspaceDock);
            m_workspaceDock->show();
        }
        if (m_bookmarksDock) {
            if (m_bookmarksDock->isFloating()) m_bookmarksDock->setFloating(false);
            addDockWidget(Qt::LeftDockWidgetArea, m_bookmarksDock);
            m_bookmarksDock->hide();  // hidden by default; View menu toggles
        }
        if (m_symbolsDock) {
            if (m_symbolsDock->isFloating()) m_symbolsDock->setFloating(false);
            addDockWidget(Qt::RightDockWidgetArea, m_symbolsDock);
            m_symbolsDock->hide();
        }
        if (m_scannerDock) {
            if (m_scannerDock->isFloating()) m_scannerDock->setFloating(false);
            addDockWidget(Qt::BottomDockWidgetArea, m_scannerDock);
            m_scannerDock->hide();
        }
        for (auto* dock : m_docDocks) {
            if (!dock) continue;
            if (dock->isFloating()) dock->setFloating(false);
            addDockWidget(Qt::TopDockWidgetArea, dock);
            dock->show();
        }

        // (4) Tabify all doc docks into one group at top.
        if (m_docDocks.size() > 1) {
            auto* first = m_docDocks.first();
            for (int i = 1; i < m_docDocks.size(); ++i)
                tabifyDockWidget(first, m_docDocks[i]);
            if (m_activeDocDock) m_activeDocDock->raise();
            else first->raise();
        }

        // (5) Reasonable sizes — workspace at its content-aware default,
        //     doc area gets the rest.
        if (m_workspaceDock) {
            int wsW = computeWorkspaceDockWidth();
            resizeDocks({m_workspaceDock}, {wsW}, Qt::Horizontal);
        }

        // (6) One synchronous pass to recreate sentinels for solo docs
        //     and restyle every tab bar.
        reconcileDockTabBars();
    });
    view->addSeparator();
    // ── View ▸ Font ──
    //
    // The list is DISCOVERED, never written down. It used to be three
    // hard-coded names, one of which — Consolas — is a Windows font: off
    // Windows that entry did not fail, it silently resolved to whatever the
    // matcher substituted, so the menu offered a face the user could neither
    // identify nor have asked for, and there was no way to reach any of the
    // good monospace faces that WERE installed. Now the two bundled families
    // are pinned (they live in resources.qrc, so they are offerable on any
    // machine with no fonts installed at all) and everything under them comes
    // from the font database. Nothing here names an operating system.
    //
    // Rebuilt on every open rather than in the constructor: enumerating
    // families is cheap but startup is where this app counts milliseconds,
    // and a font installed while RC is running then shows up without a
    // restart. See src/widgets/font_choices.h for the filtering (monospace
    // only — the editor lays every column out on one advance).
    auto* fontMenu = view->addMenu(makeIcon(":/vsicons/text-size.svg"), "&Font");
    fontMenu->setObjectName(QStringLiteral("rcxViewFontMenu"));   // --screenshot fontmenu
    connect(fontMenu, &QMenu::aboutToShow, this, [this, fontMenu] {
        fontMenu->clear();
        auto* group = new QActionGroup(fontMenu);   // dies with the rows it owns
        group->setExclusive(true);
        const QString current = rcx::RcxEditor::globalFontName();
        auto addFamily = [&](const QString& family) -> QAction* {
            QAction* a = fontMenu->addAction(family);
            a->setCheckable(true);
            a->setActionGroup(group);
            a->setChecked(family.compare(current, Qt::CaseInsensitive) == 0);
            // Each row wears its own face, so the menu IS the specimen sheet:
            // picking between 30-odd monospace names you have never seen
            // rendered is picking blind.
            QFont sample(family, 10);
            sample.setFixedPitch(true);
            a->setFont(sample);
            connect(a, &QAction::triggered, this, [this, family] { setEditorFont(family); });
            return a;
        };

        const QStringList bundled = rcx::bundledMonoFamilies();
        const QStringList system  = rcx::systemMonoFamilies();
        // The two that ship with RC, then a rule, then what this
        // machine has. Plain separators, not addSection: MenuBarStyle draws
        // a section as a bare line and swallows its label, so a heading here
        // would be an invisible promise.
        for (const QString& f : bundled) addFamily(f);
        // The face in force, when it is neither bundled nor installed — a
        // settings file carried from another machine, or a font uninstalled
        // since. Without a row the menu would show nothing checked and
        // quietly disagree with what is on screen; the suffix is on the row
        // because a heading would not survive the style.
        if (rcx::fontMenuNeedsInForceRow(current, bundled, system)) {
            fontMenu->addSeparator();
            addFamily(current)->setText(tr("%1  (not installed)").arg(current));
        }
        if (!system.isEmpty()) {
            fontMenu->addSeparator();
            for (const QString& f : system) addFamily(f);
        }
    });

    // The rest of the View menu reads its checkboxes from here. This used to
    // be declared by the font block above, which no longer needs it — the
    // font in force comes from RcxEditor::globalFontName(), not from disk.
    QSettings settings("RC", "RC");

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
        connect(&tm, &ThemeManager::themeChanged, act, [act, i](const Theme&) {
            act->setChecked(i == ThemeManager::instance().currentIndex());
        });
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
        QSettings("RC", "RC").setValue("compactColumns", checked);
        for (auto& tab : m_tabs)
            tab.ctrl->setCompactColumns(checked);
    });

    auto* actTreeLines = view->addAction("&Tree Lines");
    actTreeLines->setCheckable(true);
    actTreeLines->setChecked(settings.value("treeLines", true).toBool());
    connect(actTreeLines, &QAction::triggered, this, [this](bool checked) {
        QSettings("RC", "RC").setValue("treeLines", checked);
        for (auto& tab : m_tabs)
            tab.ctrl->setTreeLines(checked);
    });

    // Quiet lines between the type, name and value columns — its own option,
    // on or off regardless of the tree lines.
    auto* actTreeColumns = view->addAction("Tree C&olumns");
    actTreeColumns->setCheckable(true);
    actTreeColumns->setChecked(settings.value("treeColumns", true).toBool());
    connect(actTreeColumns, &QAction::triggered, this, [this](bool checked) {
        QSettings("RC", "RC").setValue("treeColumns", checked);
        for (auto& tab : m_tabs)
            tab.ctrl->setTreeColumns(checked);
    });

    m_actRelOfs = view->addAction("R&elative Offsets");
    m_actRelOfs->setCheckable(true);
    m_actRelOfs->setChecked(settings.value("relativeOffsets", true).toBool());
    connect(m_actRelOfs, &QAction::triggered, this, [this](bool checked) {
        QSettings("RC", "RC").setValue("relativeOffsets", checked);
        for (auto& tab : m_tabs)
            for (auto& pane : tab.panes)
                pane.editor->setRelativeOffsets(checked);
    });

    // ── Tail-chip toggles ──
    // Each chip kind gets its own line so users see exactly what they're
    // toggling. Labels describe the chip's marker so the menu doesn't
    // pretend to control something broader (e.g. "Comments" used to be
    // ambiguous about user comments vs. PDB symbol annotations).
    view->addSeparator();
    auto* hints = view->addMenu(QStringLiteral("Visual hints"));
    hints->setToolTipsVisible(true);   // QMenu tooltips are off by default

    auto* actComments = hints->addAction(QStringLiteral("Comment chips"));
    actComments->setToolTip(QStringLiteral("Show the  / note  marker in a field's value tail"));
    actComments->setCheckable(true);
    actComments->setChecked(settings.value("showComments", false).toBool());
    connect(actComments, &QAction::triggered, this, [this](bool checked) {
        QSettings("RC", "RC").setValue("showComments", checked);
        for (auto& tab : m_tabs)
            tab.ctrl->setShowComments(checked);
    });

    // Type-inference chip toggle removed — the inline "[ptr64]" /
    // "[int32_t×2]" annotations were retired (see compose.cpp). The
    // RcxController::setTypeHints API survives as a no-op so saved
    // settings / MCP clients don't break, but there's no UI exposure.

    // Auto-RTTI walks every non-null pointer/Hex64 candidate's vtable -> RTTI
    // chain (several cross-process reads each) on EVERY refresh — rttiForVtable's
    // cache is per-compose-pass, so it re-walks from scratch every tick. On a
    // module-heavy target (DayZ etc.) that's hundreds of reads per refresh and
    // makes the view barely usable. OFF by default; opt in here when you want
    // the {RTTI: ClassName} chips and can afford the per-refresh scan cost.
    auto* actRttiChips = hints->addAction(QStringLiteral("Auto-detect RTTI"));
    actRttiChips->setToolTip(QStringLiteral(
        "Show {RTTI: ClassName} chips by walking pointer vtables — scans every "
        "refresh, can lag on module-heavy targets"));
    actRttiChips->setCheckable(true);
    actRttiChips->setChecked(settings.value("showRttiChips", false).toBool());
    connect(actRttiChips, &QAction::triggered, this, [this](bool checked) {
        QSettings("RC", "RC").setValue("showRttiChips", checked);
        for (auto& tab : m_tabs)
            tab.ctrl->setShowRtti(checked);
    });

    view->addSeparator();

    auto* actHoverEffects = view->addAction("Ho&ver Effects");
    actHoverEffects->setCheckable(true);
    actHoverEffects->setChecked(settings.value("hoverEffects", true).toBool());
    connect(actHoverEffects, &QAction::triggered, this, [this](bool checked) {
        QSettings("RC", "RC").setValue("hoverEffects", checked);
        for (auto& tab : m_tabs)
            for (auto& pane : tab.panes)
                if (pane.editor) pane.editor->setHoverEffects(checked);
    });

    // Value popups — the hover/inline-edit "Previous Values" + preview host.
    // The popup's own "✕ Hide popups" footer link flips this off (see the
    // per-editor valuePopupsDisableRequested wiring); re-enable here.
    m_actValuePopups = view->addAction("Value &Popups");
    m_actValuePopups->setToolTip(QStringLiteral(
        "Hover/edit previews for changing values (previous-value history, "
        "hex dump, disasm, struct target)"));
    m_actValuePopups->setCheckable(true);
    // Off at every launch, and the choice is not remembered: these follow the
    // pointer, so having them on is something you ask for while you need it.
    m_actValuePopups->setChecked(false);
    connect(m_actValuePopups, &QAction::triggered, this, [this](bool checked) {
        for (auto& tab : m_tabs)
            for (auto& pane : tab.panes)
                if (pane.editor) pane.editor->setValuePopupsEnabled(checked);
    });

    // ── Ribbon (ReClassEx-style Home | Modify strip above the doc tabs) ──
    // ONE submenu, ONE persisted key. The old pair — a "Ribbon" checkbox that
    // wrote `showRibbon` and a "Ribbon Labels" submenu — left the collapsed
    // state (double-click) with no menu entry at all and two settings that
    // could disagree.
    view->addSeparator();
    {
        m_ribbonMenu = view->addMenu("&Ribbon");
        m_ribbonStateGroup = new QActionGroup(this);
        m_ribbonStateGroup->setExclusive(true);
        QSettings s("RC", "RC");
        const int state = rcx::ribbonStateFromSettings(s);
        struct { const char* text; const char* tip; int state; } states[] = {
            {"&Full",      "Tabs and buttons",              rcx::RibbonFull},
            {"&Collapsed", "Tabs only — clicking a tab expands the ribbon", rcx::RibbonCollapsed},
            {"&Hidden",    "No ribbon at all",              rcx::RibbonHidden},
        };
        for (const auto& st : states) {
            auto* a = m_ribbonMenu->addAction(QString::fromLatin1(st.text));
            a->setCheckable(true);
            a->setData(st.state);
            a->setToolTip(QString::fromLatin1(st.tip));
            a->setChecked(st.state == state);
            m_ribbonStateGroup->addAction(a);
        }
        connect(m_ribbonStateGroup, &QActionGroup::triggered, this,
                [this](QAction* a) { applyRibbonState(a->data().toInt()); });

        // Ctrl+F1 never puts the ribbon into Hidden — that state has no
        // on-screen chevron, so it stays a deliberate menu choice. Anything
        // that is not Full (Collapsed OR Hidden) therefore maps back to Full,
        // which also makes Ctrl+F1 the keyboard way out of Hidden.
        m_ribbonMenu->addSeparator();
        auto* toggle = m_ribbonMenu->addAction("&Toggle Ribbon");
        toggle->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F1));
        toggle->setToolTip(QStringLiteral("Collapse or expand the ribbon body"));
        connect(toggle, &QAction::triggered, this, [this]() {
            applyRibbonState(m_ribbonState == rcx::RibbonFull ? int(rcx::RibbonCollapsed)
                                                              : int(rcx::RibbonFull));
        });
        // No addAction(toggle) here: the menu bar lives inside the window's
        // menu widget, so a menu action's shortcut is already registered —
        // a second registration makes Qt report an ambiguous overload and
        // fire NEITHER (the same trap Bookmarks hit with Ctrl+Shift+B).

        m_ribbonMenu->addSeparator();
        m_ribbonLabelGroup = new QActionGroup(this);
        m_ribbonLabelGroup->setExclusive(true);
        const int savedMode =
            settings.value("ribbonLabels", int(RibbonBar::LabelMode::Auto)).toInt();
        struct { const char* text; RibbonBar::LabelMode mode; } modes[] = {
            {"&All labels", RibbonBar::LabelMode::All},
            {"A&uto",       RibbonBar::LabelMode::Auto},
            {"&Icons only", RibbonBar::LabelMode::IconsOnly},
        };
        for (const auto& m : modes) {
            auto* a = m_ribbonMenu->addAction(QString::fromLatin1(m.text));
            a->setCheckable(true);
            a->setData(int(m.mode));
            a->setChecked(int(m.mode) == savedMode);
            m_ribbonLabelGroup->addAction(a);
        }
        connect(m_ribbonLabelGroup, &QActionGroup::triggered, this, [this](QAction* a) {
            const int mode = a->data().toInt();
            QSettings("RC", "RC").setValue("ribbonLabels", mode);
            if (m_ribbon) m_ribbon->setLabelMode(RibbonBar::LabelMode(mode));
        });
    }

    // Minimap: narrow read-only Scintilla mirror on the right of each editor.
    // Off by default — adds visual noise on short structs, but useful on
    // 10k+ line composed views (kernel PTE dumps, generated SDKs).
    auto* actMinimap = view->addAction("&Minimap");
    actMinimap->setCheckable(true);
    actMinimap->setChecked(settings.value("minimap", false).toBool());
    connect(actMinimap, &QAction::triggered, this, [this](bool checked) {
        QSettings("RC", "RC").setValue("minimap", checked);
        for (auto& tab : m_tabs) {
            for (auto& pane : tab.panes) {
                if (!pane.minimap || !pane.editor) continue;
                pane.minimap->setVisible(checked);
                if (checked) {
                    // Force a full refresh so the just-revealed minimap
                    // receives the current text via documentApplied.
                    tab.ctrl->refresh();
                }
            }
        }
    });

    // ── Timeline ──
    // On by default: every class on a live source keeps its recent past, so
    // you can rewind something you only noticed after it happened. Switching
    // it off hides the strip, stops capture and frees what was kept.
    {
        auto* tlMenu = view->addMenu(QStringLiteral("&Timeline"));
        m_actTimeline = tlMenu->addAction(QStringLiteral("&Enable Timeline"));
        m_actTimeline->setCheckable(true);
        m_actTimeline->setChecked(settings.value("timeline/enabled", true).toBool());
        connect(m_actTimeline, &QAction::toggled, this, [this](bool on) { setTimelineEnabled(on); });
        // Showing the strip is not capturing: hiding it keeps every tab's
        // history, a recording, and the buttons beside the address.
        m_actTlStrip = tlMenu->addAction(QStringLiteral("&Show Timeline"));
        m_actTlStrip->setCheckable(true);
        m_actTlStrip->setChecked(settings.value("timeline/stripVisible", true).toBool());
        connect(m_actTlStrip, &QAction::toggled, this, [this](bool on) { setTimelineStripVisible(on); });
        tlMenu->addSeparator();
        auto addTimelineAction = [&](const QString& text, const QKeySequence& key,
                                     std::function<void(RcxController*)> fn) {
            QAction* a = tlMenu->addAction(text);
            if (!key.isEmpty()) a->setShortcut(key);
            connect(a, &QAction::triggered, this, [this, fn]() {
                if (auto* c = activeController()) { fn(c); syncTimelineActions(); }
            });
            return a;
        };
        // The same words as the buttons beside the address: Record / Stop
        // Recording, and Back to Live while looking back at a recording.
        // Values are live otherwise — there is nothing to pause.
        m_actTlRecord = addTimelineAction(QStringLiteral("&Record"), QKeySequence(Qt::Key_F9),
                                          [](RcxController* c) { c->timelineToggleRecording(); });
        m_actTlLive = addTimelineAction(QStringLiteral("&Back to Live"), QKeySequence(Qt::CTRL | Qt::Key_End),
                                        [](RcxController* c) { c->returnToLive(); });
        m_actTlPrev = addTimelineAction(QStringLiteral("Pre&vious Change"), QKeySequence(Qt::CTRL | Qt::Key_Comma),
                                        [](RcxController* c) { c->stepTimelineChange(-1); });
        m_actTlNext = addTimelineAction(QStringLiteral("&Next Change"), QKeySequence(Qt::CTRL | Qt::Key_Period),
                                        [](RcxController* c) { c->stepTimelineChange(+1); });
        tlMenu->addSeparator();
        m_actTlReset = tlMenu->addAction(QStringLiteral("&Clear Recording…"));
        connect(m_actTlReset, &QAction::triggered, this, [this]() {
            if (auto* c = activeController()) confirmTimelineClear(c);
        });
        connect(tlMenu, &QMenu::aboutToShow, this, &MainWindow::syncTimelineActions);
    }

    {
        auto* actRefresh = view->addAction("&Refresh");
        m_actRefresh = actRefresh;
        actRefresh->setShortcut(QKeySequence(Qt::Key_F5));
        connect(actRefresh, &QAction::triggered, this, [this]() {
            auto* ctrl = activeController();
            if (ctrl) {
                // Refresh means "show me now": leave the past first.
                if (ctrl->isViewingPast()) ctrl->returnToLive();
                ctrl->resetChangeTracking();
                ctrl->refresh();
            }
        });
    }
    {
        auto* actGoTo = view->addAction("&Goto Address...");
        m_actGoto = actGoTo;
        actGoTo->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
        connect(actGoTo, &QAction::triggered, this, &MainWindow::showGotoAddressDialog);
    }
    {
        // The address bar's navigation, reachable from the menu for
        // discoverability. The shortcuts are shown as hints only: the keys
        // are handled inside RcxEditor (they must not fire during an inline
        // edit), so binding them here as well would double-dispatch.
        m_actNavBack    = view->addAction("&Back	Alt+Left");
        m_actNavForward = view->addAction("For&ward	Alt+Right");
        m_actNavUp      = view->addAction("&Up One Level	Alt+Up");
        m_actNavPath    = view->addAction("Edit Pat&h	Alt+D");
        connect(m_actNavBack, &QAction::triggered, this, [this]() {
            if (auto* c = activeController()) c->goBack(activePaneEditor());
        });
        connect(m_actNavForward, &QAction::triggered, this, [this]() {
            if (auto* c = activeController()) c->goForward(activePaneEditor());
        });
        connect(m_actNavUp, &QAction::triggered, this, [this]() {
            if (auto* c = activeController()) c->goUp(activePaneEditor());
        });
        connect(m_actNavPath, &QAction::triggered, this, [this]() {
            if (auto* ed = activePaneEditor()) ed->beginAddressBarPathEdit();
        });
        // Enabled state is read from the active controller when the menu
        // opens; the bar's own cells track historyChanged live.
        connect(view, &QMenu::aboutToShow, this, [this]() {
            auto* c = activeController();
            m_actNavBack->setEnabled(c && c->canGoBack());
            m_actNavForward->setEnabled(c && c->canGoForward());
            m_actNavUp->setEnabled(c && c->canGoUp());
            m_actNavPath->setEnabled(activePaneEditor() != nullptr);
        });
    }
    {
        auto* actCmdPalette = view->addAction("Command &Palette...");
        actCmdPalette->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
        connect(actCmdPalette, &QAction::triggered, this,
                &MainWindow::showCommandPalette);
    }

    view->addSeparator();
    m_actSplit = Qt5Qt6AddAction(view, "Split View &Below",
                    QKeySequence(Qt::CTRL | Qt::Key_Backslash),
                    makeIcon(":/vsicons/split-vertical.svg"), this,
                    &MainWindow::splitView);
    Qt5Qt6AddAction(view, "&Unsplit View",
                    QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backslash),
                    QIcon(), this, &MainWindow::unsplitView);
    view->addSeparator();
    view->addAction(m_workspaceDock->toggleViewAction());
    {
        // Toggling never touches the dock's AREA or floating state — the
        // scanner opens wherever the user last left it (Bottom on a fresh
        // profile, sized by createScannerDock's first-show request). An older
        // version forced setFloating(true) in this handler, which undid any
        // redock the next time visibilityChanged round-tripped through the
        // action's checked state.
        auto* scanAct = new QAction(m_scannerDock->toggleViewAction()->icon(),
                                    QStringLiteral("Scanner"), this);
        scanAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
        scanAct->setCheckable(true);
        scanAct->setChecked(m_scannerDock->isVisible());
        connect(m_scannerDock, &QDockWidget::visibilityChanged, scanAct,
                &QAction::setChecked);
        connect(scanAct, &QAction::toggled, this, [this](bool on) {
            if (on) {
                // Lazy-build the panel inside the dock if the deferred
                // post-show timer hasn't run yet. Idempotent.
                ensureScannerPanel();
                m_scannerDock->show();
                m_scannerDock->raise();
                m_scannerDock->activateWindow();
                if (m_scannerPanel) m_scannerPanel->setFocus(Qt::OtherFocusReason);
            } else {
                m_scannerDock->hide();
            }
        });
        view->addAction(scanAct);
        m_actScanner = scanAct;
    }
    {
        // Symbols/Modules dock is built lazily (createSymbolsDock cost
        // ~360 ms in profiling, almost entirely tab + tree construction
        // for a panel that's hidden at startup). Use a free-standing
        // QAction that builds-on-toggle instead of binding to the
        // dock's toggleViewAction (which doesn't exist yet at this
        // point). The deferred build in main() typically gets there
        // first; if the user beats it to the punch this still works.
        auto* symAct = new QAction(QStringLiteral("Symbols"), this);
        symAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Y));
        symAct->setCheckable(true);
        connect(symAct, &QAction::toggled, this, [this, symAct](bool on) {
            createSymbolsDock();  // idempotent
            // Re-bind the action to the dock's visibility now that the
            // dock exists. Qt::UniqueConnection no-ops a second connect
            // with identical (sender, signal, receiver, slot), so the
            // first toggle wires it and subsequent toggles are
            // harmless. Keeps the action in sync when the dock is
            // closed via its own X button or layout restore.
            connect(m_symbolsDock, &QDockWidget::visibilityChanged,
                    symAct, &QAction::setChecked,
                    Qt::UniqueConnection);
            if (on) { m_symbolsDock->show(); m_symbolsDock->raise(); }
            else m_symbolsDock->hide();
        });
        view->addAction(symAct);
        m_actSymbols = symAct;
    }
    if (m_bookmarksDock) {
        auto* bmAct = m_bookmarksDock->toggleViewAction();
        m_actBookmarks = bmAct;
        // Ctrl+Shift+B belongs to Edit > Carve; the same sequence here
        // made Qt report an ambiguous shortcut and fire NEITHER.
        bmAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K));
        view->addAction(bmAct);
    }

    view->addSeparator();
    {
        m_actPresentationMode = view->addAction("&Presentation Mode");
        m_actPresentationMode->setCheckable(true);
        m_actPresentationMode->setChecked(false);
        m_actPresentationMode->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
        connect(m_actPresentationMode, &QAction::triggered, this, [this](bool checked) {
            m_presentationMode = checked;
            for (auto& tab : m_tabs)
                for (auto& pane : tab.panes)
                    if (pane.editor) pane.editor->setPresentationMode(checked);
            if (m_mcp) m_mcp->setSlowMode(checked);
            setAppStatus(checked ? "Presentation Mode ON" : "Presentation Mode OFF");
        });
    }

    // Tools
    auto* tools = m_menuBar->addMenu("&Tools");
    m_actRtti = Qt5Qt6AddAction(tools, "&RTTI Browser",
                    QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), QIcon(), this, [this]() {
        // Drill-down for the user-selected hex/pointer field. The compose
        // pipeline already auto-detects RTTI inline as a hint; this opens
        // the full hierarchy + vtable browser for the selected vtable.
        // No prompt — discoverability comes from the inline hints.
        auto* ctrl = activeController();
        if (!ctrl || !ctrl->provider()) {
            setAppStatus(QStringLiteral("No active provider"));
            return;
        }
        const auto& sel = ctrl->selectedIds();
        if (sel.size() != 1) {
            setAppStatus(QStringLiteral("Select a hex/pointer field first"));
            return;
        }
        uint64_t nid = baseNodeIdFromSelId(*sel.begin());
        int idx = ctrl->document()->tree.indexOfId(nid);
        if (idx < 0) return;
        const auto& n = ctrl->document()->tree.nodes[idx];
        const bool is64 = (n.kind == NodeKind::Hex64 || n.kind == NodeKind::Pointer64);
        const bool is32 = (n.kind == NodeKind::Hex32 || n.kind == NodeKind::Pointer32);
        if (!is64 && !is32) {
            setAppStatus(QStringLiteral("Selected field isn't a 4/8-byte word"));
            return;
        }
        int64_t off = ctrl->document()->tree.computeOffset(idx);
        if (off < 0) return;
        uint64_t addr = ctrl->baseAddress() + (uint64_t)off;
        uint64_t val = is64
            ? ctrl->provider()->readU64(addr)
            : (uint64_t)ctrl->provider()->readU32(addr);
        if (!val) {
            setAppStatus(QStringLiteral("Field is null"));
            return;
        }
        showRttiBrowser(val);
    });
    Qt5Qt6AddAction(tools, "&Type Aliases...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::showTypeAliasesDialog);
    Qt5Qt6AddAction(tools, "&Validate Project...",
                    QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V),
                    makeIcon(":/vsicons/warning.svg"), this,
                    &MainWindow::showValidateDialog);
    Qt5Qt6AddAction(tools, "&Performance Profiler...",
                    QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F),
                    QIcon(), this, &MainWindow::showProfilerDialog);
    tools->addSeparator();
    const auto mcpName = QSettings("RC", "RC").value("autoStartMcp", true).toBool() ? "Stop &MCP Server" : "Start &MCP Server";
    m_mcpAction = Qt5Qt6AddAction(tools, mcpName, QKeySequence::UnknownKey, QIcon(), this, &MainWindow::toggleMcp);
    tools->addSeparator();
    Qt5Qt6AddAction(tools, "&Options...", QKeySequence::UnknownKey, makeIcon(":/vsicons/settings-gear.svg"), this,
        static_cast<void(MainWindow::*)()>(&MainWindow::showOptionsDialog));

    // Plugins
    auto* plugins = m_menuBar->addMenu("&Plugins");
    Qt5Qt6AddAction(plugins, "&Manage Plugins...", QKeySequence::UnknownKey, QIcon(), this, &MainWindow::showPluginsDialog);

    // Help
    auto* help = m_menuBar->addMenu("&Help");
    Qt5Qt6AddAction(help, "&Keyboard Shortcuts...", QKeySequence(Qt::Key_F1),
                    makeIcon(":/vsicons/question.svg"), this,
                    &MainWindow::showShortcutsDialog);
    help->addSeparator();
    Qt5Qt6AddAction(help, "&About RC", QKeySequence::UnknownKey, makeIcon(":/vsicons/question.svg"), this, &MainWindow::about);
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

// ── Invisible resize zones along the frameless window's edges + corners ──
// The custom-chrome window is frameless (no native border), so without these
// only the bottom-right ResizeGrip could resize it — edges/other corners were
// dead. Each zone hands the drag to the window manager via
// QWindow::startSystemResize (supported on Windows, X11 and Wayland — i.e.
// every platform where we go frameless; macOS keeps its native frame, so these
// are not created there). The zones paint nothing (no paintEvent, no opaque
// background) so they're invisible; they exist purely to set the edge cursor
// and start a native resize, which preserves OS snap/aero behaviour.
class ResizeEdge : public QWidget {
    Q_OBJECT  // Qt 6.8 findChildren<ResizeEdge*> (repositionResizeWidgets) hard-
              // requires Q_OBJECT via a static_assert 6.5 lacked.
public:
    ResizeEdge(Qt::Edges edges, Qt::CursorShape cursor, QWidget* parent)
        : QWidget(parent), m_edges(edges) {
        setCursor(cursor);
        setAttribute(Qt::WA_NoSystemBackground);   // stay see-through
    }
    Qt::Edges edges() const { return m_edges; }
protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && window()->windowHandle()) {
            window()->windowHandle()->startSystemResize(m_edges);
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }
    // The top band sits over the title bar, and double-clicking the title bar
    // maximizes: the band keeps that, instead of starting a second resize.
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && m_edges.testFlag(Qt::TopEdge)) {
            QWidget* w = window();
            if (w->isMaximized()) w->showNormal();
            else w->showMaximized();
            e->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(e);
    }
private:
    Qt::Edges m_edges;
};

// Zone geometry (every edge and corner, none while maximized) lives in
// widgets/resize_edges.h so it can be tested without a window.

// ── Dock title-bar grip (VS2022-style dot pattern) ──
// Still used by the FLOATING document-dock title bar (createDocDock). The four
// side docks dropped it when they moved to rcx::DockHeader — there the header
// itself is the drag surface, so a separate 12-px dot column that existed only
// to own a SizeAllCursor was pure chrome noise.
class DockGripWidget : public QWidget {
public:
    explicit DockGripWidget(QWidget* parent) : QWidget(parent) {
        setFixedWidth(12);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setCursor(Qt::SizeAllCursor);
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
        // 2 columns x 4 rows, centered symmetrically
        const double offsets[] = {-1.5, -0.5, 0.5, 1.5};
        for (double off : offsets) {
            p.drawEllipse(QPointF(cx - s * 0.5, cy + off * s), r, r);
            p.drawEllipse(QPointF(cx + s * 0.5, cy + off * s), r, r);
        }
    }
private:
    QColor m_color;
};

// ── Custom-painted dock title bar ──
// Used as QDockWidget::setTitleBarWidget(). Paints its own background so Fusion
// can't insert frames or steal pixels. Qt handles drag/dock natively.
// SUPERSEDED for the four side docks by rcx::DockHeader (widgets/dock_header.h),
// which adds the seam, the title, the close button and the float chrome that
// each dock used to hand-roll differently. Kept as the plain painted base.
// Widget-rect wrappers over the device-exact edge fills defined above
// MenuBarStyle (see the comment there for the why).
static void fillBottomDeviceRow(QPainter& p, const QWidget* w, const QColor& c) {
    fillBottomDeviceRowOfRect(p, QRectF(w->rect()), c);
}
static void fillRightDeviceCol(QPainter& p, const QWidget* w, const QColor& c) {
    fillRightDeviceColOfRect(p, QRectF(w->rect()), c);
}

// Style the minimap to match the main editor at 4pt.
//
// ORDER MATTERS, and getting it wrong is why this used to render BLACK text on
// the dark paper. QsciScintilla::setColor() only writes style 0, and only when
// there is no lexer; QsciScintilla::setPaper() then issues its own
// SCI_STYLECLEARALL, which copies STYLE_DEFAULT over every style — including
// style 0. STYLE_DEFAULT's foreground was never assigned, so it defaulted to
// black and clobbered the colour setColor() had just set. Configuring a lexer
// up front sidesteps both no-lexer paths entirely.
//
// The lexer stays even though the minimap is deliberately NOT syntax-coloured:
// it is the thing that keeps STYLE_DEFAULT out of the picture. Mirroring the
// editor's full palette here was tried and reads as noise at 4pt — a 110px
// strip of confetti competing with the editor for attention. What the minimap
// is for is SHAPE (where the dense blocks are, where the blank runs are), so
// it renders in the theme's existing grey tiers: three tones, enough to keep
// structure legible, no hues. Reuses textDim/textMuted/textFaint rather than
// introducing minimap-specific colours, so it follows the theme for free.
static void styleMinimap(QsciScintilla* mm, const rcx::Theme& t) {
    if (!mm) return;
    const QColor paper = rcx::editorPaperColor(t);
    // Very small font so a ~100-line struct fits vertically at a glance.
    QFont mf(QStringLiteral("JetBrains Mono"), 4);
    mf.setFixedPitch(true);

    auto* lex = qobject_cast<QsciLexerCPP*>(mm->lexer());
    if (!lex) lex = new QsciLexerCPP(mm);

    lex->setDefaultFont(mf);
    lex->setDefaultPaper(paper);
    lex->setDefaultColor(t.textMuted);
    // Flatten every style to one of three greys first, so any lexer style not
    // named below (there are ~40) can never fall back to STYLE_DEFAULT black.
    for (int i = 0; i <= 127; i++) {
        lex->setPaper(paper, i);
        lex->setFont(mf, i);
        lex->setColor(t.textMuted, i);
    }
    // Structure reads slightly stronger; commentary recedes. That is the whole
    // vocabulary — no hues.
    lex->setColor(t.textDim,   QsciLexerCPP::Keyword);
    lex->setColor(t.textDim,   QsciLexerCPP::KeywordSet2);
    lex->setColor(t.textDim,   QsciLexerCPP::GlobalClass);
    lex->setColor(t.textFaint, QsciLexerCPP::Comment);
    lex->setColor(t.textFaint, QsciLexerCPP::CommentLine);
    lex->setColor(t.textFaint, QsciLexerCPP::CommentDoc);
    if (mm->lexer() != lex) mm->setLexer(lex);
    // Selection colours muted — the user can't actually select text here.
    mm->setSelectionForegroundColor(t.text);
    mm->setSelectionBackgroundColor(t.selected);
}

// 1-logical-px horizontal separator that always renders as exactly ONE
// device pixel row (its bottom one), unlike a stylesheet'd QFrame.
class HairlineSeparator : public QWidget {
    // The four side docks no longer instantiate one: rcx::DockHeader and
    // rcx::PanelSearchField each paint their own seam. Kept for other chrome.
    Q_OBJECT
    QColor m_color;
public:
    explicit HairlineSeparator(const QColor& c, QWidget* parent = nullptr)
        : QWidget(parent), m_color(c) { setFixedHeight(1); }
    void setColor(const QColor& c) { m_color = c; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        fillBottomDeviceRow(p, this, m_color);
    }
};

class DockTitleBar : public QWidget {
    Q_OBJECT
    int m_h;
    QColor m_bg;
    QColor m_borderRight;
public:
    explicit DockTitleBar(int height, const QColor& bg, QWidget* parent = nullptr)
        : QWidget(parent), m_h(height), m_bg(bg) {
        setMinimumHeight(m_h);
        setMaximumHeight(m_h);
    }
    void setBarHeight(int h) { m_h = h; setMinimumHeight(h); setMaximumHeight(h); updateGeometry(); }
    void setBackground(const QColor& c) { m_bg = c; update(); }
    void setBorderRight(const QColor& c) { m_borderRight = c; update(); }
    QSize sizeHint()        const override { return {QWidget::sizeHint().width(), m_h}; }
    QSize minimumSizeHint() const override { return {0, m_h}; }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), m_bg);
        if (m_borderRight.isValid())
            fillRightDeviceCol(p, this, m_borderRight);
    }
};

// Workspace dock content surface: custom-painted background + optional
// device-exact right edge line, replacing the old stylesheet background so
// the docked-only border doesn't have QSS and custom paint fighting.
class WorkspacePanel : public QWidget {
    Q_OBJECT
    QColor m_bg;
    QColor m_rightLine;
public:
    explicit WorkspacePanel(const QColor& bg, QWidget* parent = nullptr)
        : QWidget(parent), m_bg(bg) {}
    void setBackground(const QColor& c) { m_bg = c; update(); }
    void setRightLine(const QColor& c)  { m_rightLine = c; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), m_bg);
        if (m_rightLine.isValid())
            fillRightDeviceCol(p, this, m_rightLine);
    }
};

// ── Collapsed left-edge rail to re-open the hidden Workspace tree ──
// A thin chrome strip shown ONLY while the workspace dock is hidden, so there
// is always a discoverable handle to bring the "Project" tree back. Pure-Qt
// custom paint (no stylesheets, no platform APIs) — identical on every OS.
class WorkspaceRail : public QWidget {
    Q_OBJECT
public:
    explicit WorkspaceRail(QWidget* parent = nullptr) : QWidget(parent) {
        // 22 px: the same column the dock's own edge occupies, so expanding
        // and collapsing the Project dock doesn't shift the editor sideways.
        setFixedWidth(22);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Show Project"));
    }
signals:
    void clicked();
protected:
    void enterEvent(QEnterEvent*) override { m_hover = true;  update(); }
    void leaveEvent(QEvent*)      override { m_hover = false; update(); }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) emit clicked();
    }
    void paintEvent(QPaintEvent*) override {
        const auto& t = rcx::ThemeManager::instance().current();
        QPainter p(this);
        // Match the editor "paper" surface so the collapsed Project rail reads
        // as the SAME background as the editor (no jarring lighter chrome).
        // Hover is the shared t.hover fill, not railBg.lighter(135): the rail
        // is a rest-state handle and gets no accent of its own (accent budget).
        const QColor railBg = rcx::editorPaperColor(t);
        p.fillRect(rect(), railBg);
        if (m_hover) p.fillRect(rect(), t.hover);
        // No right edge line of our own — the editor container immediately to
        // the right paints its device-exact left border, and a rail-side line
        // next to it read as a double line.

        const QColor fg = m_hover ? t.text : t.textDim;
        const double cx = width() / 2.0;

        // Chevron ▸ near the top (right-pointing arrow → "expand"), painted as
        // device-aligned pixel art with AA off (was a 1.8-wide RoundCap pen,
        // i.e. a soft smear at fractional DPR).
        rcx::drawRailChevron(p, QPointF(cx, 18.0), fg, devicePixelRatioF());

        // Vertical "PROJECT" label, reading bottom-to-top, centered on the
        // rail so it doesn't float against the bottom edge. Size derived
        // through resolvedPointSize — a raw setPixelSize(13) both ignored the
        // user's chrome size and made the label the loudest thing on the strip.
        // The label is drawn FLAT and then turned, never drawn turned.
        // Windows hands back subpixel (LCD) coverage for text, which owes its
        // sharpness to the RGB stripes running ALONG the glyphs; rotate the
        // face 90° and those stripes cross the stems instead, leaving the
        // orange/blue fringing that reads as a pixelated label. A transparent
        // image carries no subpixel coverage, so rasterising there gives plain
        // greyscale edges, and a quarter turn of a bitmap is lossless.
        //
        // The chrome face, not the inherited app font: this label stands in
        // for the Project dock's title and has to match it.
        QFont f = rcx::chromeFont();
        f.setPointSize(qMax(7, rcx::resolvedPointSize(f) - 1));
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
        const QString label = QStringLiteral("PROJECT");
        const QFontMetrics fm(f);
        const qreal dpr = devicePixelRatioF();
        const QSize flat(fm.horizontalAdvance(label) + 2, fm.height() + 2);
        QImage img(QSize(qMax(1, int(std::lround(flat.width() * dpr))),
                         qMax(1, int(std::lround(flat.height() * dpr)))),
                   QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(dpr);
        img.fill(Qt::transparent);
        {
            QPainter tp(&img);
            tp.setRenderHint(QPainter::TextAntialiasing, true);
            tp.setFont(f);
            tp.setPen(fg);
            tp.drawText(QRectF(QPointF(0, 0), QSizeF(flat)), Qt::AlignCenter, label);
        }
        QTransform turn;
        turn.rotate(-90.0);
        QImage upright = img.transformed(turn);
        upright.setDevicePixelRatio(dpr);
        p.drawImage(QPointF((width() - upright.width() / dpr) / 2.0,
                            (height() - upright.height() / dpr) / 2.0),
                    upright);
    }
private:
    bool m_hover = false;
};

//TODO-DELETE(ViewTabButton) // ── Custom-painted view tab button (no CSS) ──
//class ViewTabButton : public QPushButton {
//public:
//    static constexpr int kAccentH = 3;   // accent line height in pixels
//    static constexpr int kPadLR  = 12;   // horizontal padding
//    static constexpr int kPadBot = 4;    // extra bottom padding
//
//    int baselineY = -1;  // set by FlatStatusBar for cross-widget text alignment
//
//    QColor colBg, colBgChecked, colBgHover, colBgPressed;
//    QColor colText, colTextMuted, colAccent, colBorder;
//
//    explicit ViewTabButton(const QString& text, QWidget* parent = nullptr)
//        : QPushButton(text, parent) {
//        setCheckable(true);
//        setFlat(true);
//        setCursor(Qt::PointingHandCursor);
//        setContentsMargins(0, 0, 0, 0);
//        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
//    }
//
//    QSize sizeHint() const override {
//        QFontMetrics fm(font());
//        int w = fm.horizontalAdvance(text()) + 2 * kPadLR;
//        int h = qRound((fm.height() + kAccentH + kPadBot) * 1.33);
//        return QSize(w, h);
//    }
//
//protected:
//    void paintEvent(QPaintEvent*) override {
//        QPainter p(this);
//        // Background
//        QColor bg = colBg;
//        if (isDown())          bg = colBgPressed;
//        else if (underMouse()) bg = colBgHover;
//        else if (isChecked())  bg = colBgChecked;
//        p.fillRect(rect(), bg);
//
//        // Top border (continuous with status bar hairline)
//        if (colBorder.isValid())
//            p.fillRect(0, 0, width(), 1, colBorder);
//
//        // Accent line at y=0 when checked (paints over border)
//        if (isChecked())
//            p.fillRect(0, 0, width(), kAccentH, colAccent);
//
//        // Text — use shared baseline if set, otherwise fall back to VCenter
//        p.setPen(isChecked() || underMouse() || isDown() ? colText : colTextMuted);
//        p.setFont(font());
//        if (baselineY >= 0) {
//            p.drawText(kPadLR, baselineY, text());
//        } else {
//            QRect textRect(kPadLR, kAccentH, width() - 2 * kPadLR, height() - kAccentH);
//            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text());
//        }
//    }
//
//    void enterEvent(QEnterEvent*) override { update(); }
//    void leaveEvent(QEvent*) override { update(); }
//};

// ── Shimmer label — gradient text sweep for MCP activity ──
class ShimmerLabel : public QWidget {
public:
    explicit ShimmerLabel(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_Hover, true);   // for the hover underline below
        m_timer.setInterval(30);
        connect(&m_timer, &QTimer::timeout, this, [this]() {
            m_phase += 0.012f;
            if (m_phase > 1.0f) m_phase -= 1.0f;
            update();
        });
    }

    void setText(const QString& t) { m_text = t; m_dimSuffix.clear(); update(); }
    void setText(const QString& t, const QString& dimSuffix) { m_text = t; m_dimSuffix = dimSuffix; update(); }
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
    QColor colBase;     // normal text
    QColor colDim;      // dimmed suffix text
    QColor colBright;   // highlight sweep
    QColor colSep;      // vertical separator between sections
    QColor colHover;    // main segment while hovered (the label is clickable)

    // Optional click handler — installed by createStatusBar to launch the
    // Goto Address dialog when the user clicks the location segment.
    // std::function instead of a Qt signal because the class is defined
    // inline in main.cpp (no Q_OBJECT, no MOC pass on inline-defined
    // classes) — the callback contract is the simplest plumbing that
    // doesn't require restructuring the file.
    std::function<void()> onClicked;

protected:
    void enterEvent(QEnterEvent*) override { m_hover = true;  update(); }
    void leaveEvent(QEvent*)      override { m_hover = false; update(); }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && onClicked) {
            onClicked();
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void paintEvent(QPaintEvent*) override {
        if (m_text.isEmpty()) return;
        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setFont(font());

        QRect r = contentsRect();

        if (!m_shimmer) {
            QColor c = colBase.isValid() ? colBase
                                         : palette().color(QPalette::WindowText);
            // The whole label is a click target (Goto Address), so on hover the
            // MAIN segment says so: brighter + underlined, the same link cue
            // the address bar uses. The dim detail and the separator stay put.
            QFont mainFont = font();
            if (m_hover) {
                if (colHover.isValid()) c = colHover;
                mainFont.setUnderline(true);
            }
            p.setPen(c);
            p.setFont(mainFont);
            if (m_dimSuffix.isEmpty()) {
                p.drawText(r, m_align, m_text);
            } else {
                QFontMetrics fm(font());
                int tw = fm.horizontalAdvance(m_text);
                p.drawText(r, m_align, m_text);
                p.setFont(font());

                // Vertical separator between main text and dim suffix
                int sepGap = fm.horizontalAdvance(' ');
                int sepX = r.left() + tw + sepGap;
                QColor sc = colSep.isValid() ? colSep : palette().color(QPalette::Dark);
                p.fillRect(sepX, r.top() + 4, 1, r.height() - 8, sc);

                QColor dc = colDim.isValid() ? colDim : c;
                p.setPen(dc);
                QRect sr = r;
                sr.setLeft(sepX + sepGap);
                p.drawText(sr, m_align, m_dimSuffix);
            }
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
    QString      m_dimSuffix;
    bool         m_shimmer = false;
    bool         m_hover   = false;
    float        m_phase   = 0.0f;
    Qt::Alignment m_align  = Qt::AlignLeft | Qt::AlignVCenter;
    QTimer       m_timer;
};

// ── Borderless status bar with manual child layout ──
// QStatusBarLayout hardcodes 2px margins that can't be overridden.
// We bypass it entirely: children are placed manually in resizeEvent,
// and addWidget() is NOT used. Instead, create children as direct
// children and call manualLayout() to position them.
// Right-anchored status-bar chip: a liveness dot (green=live / amber=stale /
// red=disconnected / grey=static file) plus the active data-source label
// "Process:notepad.exe". Combines the source-status badge and the "which
// source am I on" indicator into one painted widget (no stylesheet — matches
// the flat status bar). Hidden when there is no source.
class SourceStatusChip : public QWidget {
    Q_OBJECT
public:
    explicit SourceStatusChip(QWidget* parent = nullptr) : QWidget(parent) {
        setVisible(false);
    }
    void setState(RcxController::SourceStatus st, const QString& label) {
        if (st == m_st && label == m_label) return;
        m_st = st; m_label = label;
        const bool vis = (st != RcxController::SourceStatus::None) && !label.isEmpty();
        setVisible(vis);
        setToolTip(vis ? tooltipFor(st, label) : QString());
        updateGeometry();
        update();
        emit needsRelayout();
    }
    QSize sizeHint() const override {
        if (m_st == RcxController::SourceStatus::None || m_label.isEmpty())
            return QSize(0, 0);
        const QFontMetrics fm(font());
        return QSize(kPadL + kDot + kGap + fm.horizontalAdvance(m_label) + kPadR,
                     fm.height());
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
signals:
    void needsRelayout();
protected:
    void paintEvent(QPaintEvent*) override {
        if (m_st == RcxController::SourceStatus::None || m_label.isEmpty()) return;
        const auto& th = ThemeManager::instance().current();
        QColor dot, fg = th.textDim;
        switch (m_st) {
            case RcxController::SourceStatus::Live:  dot = th.indHintGreen; break;
            case RcxController::SourceStatus::Stale: dot = th.focusGlow;    break;
            case RcxController::SourceStatus::Disconnected:
                dot = th.markerError; fg = th.textMuted; break;
            default: dot = th.textMuted; break;   // Static
        }
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal cy = height() / 2.0;
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(QPointF(kPadL + kDot / 2.0, cy), kDot / 2.0, kDot / 2.0);
        p.setPen(fg);
        const QRect tr(kPadL + kDot + kGap, 0,
                       width() - (kPadL + kDot + kGap), height());
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, m_label);
    }
private:
    static QString tooltipFor(RcxController::SourceStatus st, const QString& label) {
        QString s;
        switch (st) {
            case RcxController::SourceStatus::Live:  s = QStringLiteral("Live — reading"); break;
            case RcxController::SourceStatus::Stale: s = QStringLiteral("Stale — read failing"); break;
            case RcxController::SourceStatus::Disconnected: s = QStringLiteral("Disconnected"); break;
            default: s = QStringLiteral("Static file"); break;
        }
        return label + QStringLiteral("  ·  ") + s;
    }
    static constexpr int kDot = 8, kGap = 6, kPadL = 8, kPadR = 10;
    RcxController::SourceStatus m_st = RcxController::SourceStatus::None;
    QString m_label;
};

class FlatStatusBar : public QStatusBar {
public:
    QWidget*       tabRow     = nullptr;   // set by createStatusBar
    ShimmerLabel*  label      = nullptr;   // set by createStatusBar
    QWidget*       sourceChip = nullptr;   // right-anchored source chip; set by createStatusBar
    QWidget*       grip       = nullptr;   // resize grip pinned to the far bottom-right corner
    void refreshLayout() { manualLayout(); }

    void setDividerColor(const QColor& c) { m_div = c; update(); }
    void setTopLineColor(const QColor& c) { m_top = c; update(); }

    explicit FlatStatusBar(QWidget* parent = nullptr) : QStatusBar(parent) {
        setSizeGripEnabled(false);
        setStyleSheet(QStringLiteral("QStatusBar { border: none; }"));
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

        // Top seam — through the shared device-exact helper. The old
        // hand-rolled `1.0 / dpr` rect picks a different row at a .25 phase
        // than every other chrome line in the window.
        if (m_top.isValid())
            rcx::fillTopDeviceRowOfRect(p, QRectF(rect()), m_top);

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
        const int gutter = rcx::kGutter;   // one left margin down the window
        // Resize grip pinned to the far bottom-right corner — laid out HERE (in
        // the status bar) rather than as a free MainWindow child, so QMainWindow's
        // layout can't reclaim it back to (0,0). Reserve its width on the right.
        int gripReserve = 0;
        if (grip) {
            const int gs = grip->height();
            grip->move(width() - gs - 2, qMax(0, h - gs - 2));
            grip->raise();
            gripReserve = gs + 4;
        }
        // Reserve right-side space for the source chip when it's visible, so
        // the status text never runs underneath it.
        int rightReserve = gripReserve;
        if (sourceChip && sourceChip->isVisible()) {
            const int cw = sourceChip->sizeHint().width();
            const int edge = rcx::kGutter;
            sourceChip->setGeometry(qMax(0, width() - cw - edge - gripReserve), 0, cw, h);
            sourceChip->raise();
            rightReserve = cw + edge + gripReserve + gutter;
        }
        if (tabRow) {
            const int tw = tabRow->sizeHint().width();
            tabRow->setGeometry(0, 0, tw, h);
            m_divX = tw;
            label->setGeometry(tw + 1 + gutter, 0,
                               qMax(0, width() - (tw + 1 + gutter) - rightReserve), h);
        } else {
            m_divX = -1;
            label->setGeometry(gutter, 0, qMax(0, width() - gutter - rightReserve), h);
        }
        label->setContentsMargins(0, 0, 0, 0);
        label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    }
};

// paintEmptyOverlay lives in widgets/empty_overlay.h (shared with the scanner's
// EmptyResultsTable).

// QTreeView with a centered placeholder when its model is empty (the Workspace
// "Project" types tree). Distinguishes "no types yet" from "no filter matches".
class EmptyHintTreeView : public QTreeView {
public:
    using QTreeView::QTreeView;
    QString placeholder, hint;
protected:
    void paintEvent(QPaintEvent* e) override {
        QTreeView::paintEvent(e);
        if (model() && model()->rowCount(rootIndex()) > 0) return;
        // This tree's model is always the WorkspaceProxyModel (set in
        // createWorkspaceDock); static_cast matches the existing call sites
        // (WorkspaceProxyModel has no Q_OBJECT, so qobject_cast can't refine it).
        bool filtered = false;
        if (auto* pm = static_cast<rcx::WorkspaceProxyModel*>(model()))
            filtered = pm->hasFilter();
        paintEmptyOverlay(viewport(), font(),
            filtered ? QStringLiteral("No types match the filter") : placeholder,
            filtered ? QString() : hint);
    }
};

// QListWidget with a centered placeholder when empty (the Bookmarks panel). Its
// filter hides rows rather than removing them, so "no visible rows" with a
// non-zero count means "no filter matches".
class EmptyHintListWidget : public QListWidget {
public:
    using QListWidget::QListWidget;
    QString placeholder, hint;
protected:
    void paintEvent(QPaintEvent* e) override {
        QListWidget::paintEvent(e);
        bool anyVisible = false;
        for (int i = 0; i < count(); ++i)
            if (!item(i)->isHidden()) { anyVisible = true; break; }
        if (anyVisible) return;
        const bool filtered = count() > 0;  // items exist but all hidden
        paintEmptyOverlay(viewport(), font(),
            filtered ? QStringLiteral("No bookmarks match the filter") : placeholder,
            filtered ? QString() : hint);
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
    // Click anywhere on the status text to open Goto Address — the
    // location segment (path / +0xNN / class size) is already showing
    // there, so click-to-jump is the natural affordance even without a
    // dedicated standalone button.
    m_statusLabel->setCursor(Qt::PointingHandCursor);
    m_statusLabel->onClicked = [this]() { showGotoAddressDialog(); };
    // The type-key legend used to be printed into the status text on every
    // selection. It never changed, so it was permanent clutter competing with
    // the part that does change — it lives here now, where it is one hover
    // away and can also explain what clicking the label does.
    m_statusLabel->setToolTip(QStringLiteral(
        "Type keys: P ptr · F float · S int · U uint  ·  ←/→ cycle same-size "
        "types  ·  click: go to address (Ctrl+G)"));

    // View toggle is now per-pane via QTabWidget tab bar (Structure / Code)
    sb->tabRow = nullptr;
    sb->label  = m_statusLabel;

    // Resize grip lives IN the status bar (child of sb) and is pinned to the
    // bottom-right by FlatStatusBar::manualLayout — robust against QMainWindow's
    // layout (a free MainWindow child kept getting reclaimed to 0,0).
    auto* grip = new ResizeGrip(sb);
    grip->setObjectName("resizeGrip");
    sb->grip = grip;

#ifndef __APPLE__
    // Resize zones on every edge and corner (the visible grip already covers
    // bottom-right). Positioned/raised in repositionResizeWidgets, hidden while
    // maximized. Frameless platforms only; macOS keeps its native frame, which
    // already resizes from every edge.
    struct { Qt::Edges e; Qt::CursorShape c; } kEdges[] = {
        { Qt::TopEdge,                     Qt::SizeVerCursor   },
        { Qt::BottomEdge,                  Qt::SizeVerCursor   },
        { Qt::LeftEdge,                    Qt::SizeHorCursor   },
        { Qt::RightEdge,                   Qt::SizeHorCursor   },
        { Qt::TopEdge | Qt::LeftEdge,      Qt::SizeFDiagCursor },
        { Qt::TopEdge | Qt::RightEdge,     Qt::SizeBDiagCursor },
        { Qt::BottomEdge | Qt::LeftEdge,   Qt::SizeBDiagCursor },
    };
    for (auto& z : kEdges)
        (new ResizeEdge(z.e, z.c, this))->raise();
#endif

    {
        const auto& t = ThemeManager::instance().current();
        QPalette sbPal = statusBar()->palette();
        sbPal.setColor(QPalette::Window, t.background);
        sbPal.setColor(QPalette::WindowText, t.textDim);
        statusBar()->setPalette(sbPal);
        statusBar()->setAutoFillBackground(true);

        // The status strip's top edge touches the document column, so it is a
        // seam (containerBorderColor), not a chrome-strip hairline (t.border).
        sb->setTopLineColor(rcx::containerBorderColor(t));
        sb->setDividerColor(t.border);

        m_statusLabel->colBase   = t.textDim;
        m_statusLabel->colDim    = t.textMuted;
        m_statusLabel->colBright = t.indHoverSpan;
        m_statusLabel->colSep    = t.border;
        m_statusLabel->colHover  = t.text;
    }

    // Sync status bar font to global editor font (10pt monospace)
    {
        QSettings s("RC", "RC");
        QFont f(s.value("font", "JetBrains Mono").toString(), 10);
        f.setFixedPitch(true);
        m_statusLabel->setFont(f);
        sb->setMinimumHeight(QFontMetrics(f).height() + 6);
    }

    // Progress widgets — child of status bar, positioned manually in
    // begin/end. Hidden by default; shown only during long operations.
    {
        const auto& t = ThemeManager::instance().current();
        QSettings s("RC", "RC");
        QFont f(s.value("font", "JetBrains Mono").toString(), 10);
        f.setFixedPitch(true);

        m_progressLabel = new QLabel(sb);
        m_progressLabel->setFont(f);
        m_progressLabel->setStyleSheet(QStringLiteral("color: %1;").arg(t.text.name()));
        m_progressLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        m_progressLabel->setVisible(false);

        m_progressBar = new QProgressBar(sb);
        m_progressBar->setFixedHeight(QFontMetrics(f).height() - 2);
        m_progressBar->setTextVisible(false);
        m_progressBar->setStyleSheet(QStringLiteral(
            "QProgressBar { background: %1; border: 1px solid %2; }"
            "QProgressBar::chunk { background: %3; }")
            .arg(t.background.name(), t.border.name(), t.indHoverSpan.name()));
        m_progressBar->setVisible(false);
    }

    // Source-status chip — right-anchored liveness dot + "[kind:name]" of the
    // active data source. Updated via RcxController::sourceStatusChanged.
    {
        // 10 pt, like the label beside it: at 9 pt the chip and the status text
        // were two sizes on one baseline in a strip 20 px tall.
        QSettings s("RC", "RC");
        QFont f(s.value("font", "JetBrains Mono").toString(), 10);
        f.setFixedPitch(true);
        m_sourceChip = new SourceStatusChip(sb);
        m_sourceChip->setFont(f);
        sb->sourceChip = m_sourceChip;
        connect(m_sourceChip, &SourceStatusChip::needsRelayout, sb,
                [sb]() { sb->refreshLayout(); });
    }
}

void MainWindow::beginProgress(const QString& label, int total) {
    if (!m_progressBar || !m_progressLabel) return;
    m_progressLabel->setText(label);
    m_progressBar->setMinimum(0);
    m_progressBar->setMaximum(total);  // 0 = indeterminate (Qt convention)
    m_progressBar->setValue(0);

    // Position: right-anchored on status bar, label to the left of bar
    int sbH = statusBar()->height();
    int barW = 160, lblW = 220, gap = 6, edge = 8;
    int sbW = statusBar()->width();
    int lblY = (sbH - m_progressLabel->sizeHint().height()) / 2;
    int barY = (sbH - m_progressBar->height()) / 2;
    int barX = sbW - barW - edge;
    int lblX = barX - gap - lblW;
    m_progressLabel->setGeometry(lblX, lblY, lblW, m_progressLabel->sizeHint().height());
    m_progressBar->setGeometry(barX, barY, barW, m_progressBar->height());
    m_progressLabel->show();
    m_progressBar->show();
    m_progressBar->raise();
    m_progressLabel->raise();
}

void MainWindow::updateProgress(int value, const QString& label) {
    if (!m_progressBar) return;
    if (!label.isEmpty() && m_progressLabel) m_progressLabel->setText(label);
    m_progressBar->setValue(value);
}

void MainWindow::endProgress() {
    if (m_progressBar) m_progressBar->setVisible(false);
    if (m_progressLabel) m_progressLabel->setVisible(false);
}

void MainWindow::setAppStatus(const QString& text) {
    m_appStatus = text;
    m_appStatusDim.clear();
    if (!m_mcpBusy) {
        m_statusLabel->setText(text);
        m_statusLabel->setShimmerActive(false);
    }
}

void MainWindow::setAppStatus(const QString& text, const QString& dimSuffix) {
    m_appStatus = text;
    m_appStatusDim = dimSuffix;
    if (!m_mcpBusy) {
        m_statusLabel->setText(text, dimSuffix);
        m_statusLabel->setShimmerActive(false);
    }
}

// Rebuild the status bar's location segment from a controller's CURRENT
// selection. Split out of the nodeSelected lambda so it can also be called
// when nothing was selected but the visible document changed: the segment used
// to keep whatever the last tab to fire nodeSelected had written, so it
// happily named UnnamedClass0 while UnnamedClass4 was on screen.
// nodeIdx < 0 means "resolve the selection yourself" (setActiveDocDock has no
// signal payload to hand on).
void MainWindow::refreshSelectionStatus(RcxController* ctrl, int nodeIdx) {
    if (!ctrl || !ctrl->document()) return;
    const auto& tree = ctrl->document()->tree;
    const auto sel = ctrl->selectedIds();
    const int selCount = sel.size();
    if (nodeIdx < 0 && selCount >= 1) {
        // Resolve the selection ourselves. Pick the LOWEST-OFFSET member, not
        // whatever the QSet hands out first: set order is not document order,
        // so picking arbitrarily would make the same selection describe itself
        // differently from one rebuild to the next. Resolving a multi-selection
        // too (rather than only a lone one) is what makes this function
        // idempotent — rebuilding never downgrades a live "hex64 x3" to the
        // class-root form.
        int best = -1;
        uint64_t bestOff = 0;
        for (const auto id : sel) {
            const int i = tree.indexOfId(rcx::baseNodeIdFromSelId(id));
            if (i < 0) continue;
            if (best < 0 || tree.nodes[i].offset < bestOff) {
                best = i;
                bestOff = tree.nodes[i].offset;
            }
        }
        nodeIdx = best;
    }

    // Gate on the node index ALONE. handleMarginClick emits nodeSelected
    // without touching m_selIds, so an address-margin click with nothing
    // selected is a real "this field" gesture that carries selCount == 0; a
    // selCount floor here silently downgraded it to the class-root form.
    const rcx::SelectionStatus st =
        (nodeIdx >= 0 && nodeIdx < tree.nodes.size())
            ? rcx::buildSelectionStatus(tree, nodeIdx, ctrl->viewRootId(), selCount)
            : rcx::buildRootStatus(tree, ctrl->viewRootId());

    auto* ap = findActiveSplitPane();
    if (ap && ap->viewMode == VM_Rendered)
        setAppStatus(QStringLiteral("Rendered: ") + st.main);
    else
        setAppStatus(st.main, st.dim);
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
            m_statusLabel->setText(m_appStatus, m_appStatusDim);
            m_statusLabel->setShimmerActive(false);
        });
    }
    m_mcpClearTimer->start(750);
}



// Minimap subclass — the read-only narrow QsciScintilla that mirrors
// the editor at 4pt font. Two custom behaviours layered on top of
// QsciScintilla:
//   - Click anywhere on the minimap → emit lineClicked(int) so the
//     parent pane can scroll the main editor to that line. Using the
//     base-class mousePressEvent drop-through caused Scintilla's own
//     selection logic to fire too; we swallow the event entirely.
//   - Translucent overlay child that paints a rectangle covering the
//     lines currently visible in the main editor. Position + height
//     update as the user scrolls or zooms the main editor.
class MinimapScintilla : public QsciScintilla {
    Q_OBJECT
public:
    explicit MinimapScintilla(QWidget* parent = nullptr) : QsciScintilla(parent) {}
signals:
    void lineClicked(int line);
protected:
    void mousePressEvent(QMouseEvent* e) override {
        // Map y → document line. lineAt() returns -1 below the last
        // line of text; clamp to [0, lines()-1] so a click in the
        // empty area below the text still scrolls to the bottom.
        int line = lineAt(e->pos());
        if (line < 0) line = lines() - 1;
        if (line < 0) line = 0;
        emit lineClicked(line);
        e->accept();  // do not let Scintilla's selection logic run
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        // Treat drag as continuous click — drag-scrub the main editor.
        if (e->buttons() & Qt::LeftButton) {
            int line = lineAt(e->pos());
            if (line < 0) line = lines() - 1;
            if (line < 0) line = 0;
            emit lineClicked(line);
            e->accept();
            return;
        }
        QsciScintilla::mouseMoveEvent(e);
    }
};

// Translucent rectangle drawn on top of the minimap to indicate which
// document lines are currently visible in the main editor. Repositioned
// from the parent (createSplitPane) on every viewport change.
class MinimapViewportIndicator : public QWidget {
public:
    explicit MinimapViewportIndicator(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        // A faint neutral wash, no outline: this is "where you are", not a
        // selection. It used to be a purple box with a bright edge, which made
        // the map the loudest thing on the right-hand side and spent the
        // accent on a passive indicator. Purple stays on the map's own hover.
        //
        // Read the tint LIVE, do not cache it at construction. That was
        // survivable while the tint was indHoverSpan (which reads on either
        // theme) but not now that it is theme.text, which INVERTS: a pane
        // built under the dark theme kept #DCDCDC, and #DCDCDC at alpha 24
        // over light paper is a ~1/255 delta, i.e. no indicator at all.
        QColor fill = ThemeManager::instance().current().text;
        fill.setAlpha(24);
        p.fillRect(rect(), fill);
    }
};

MainWindow::SplitPane MainWindow::createSplitPane(TabState& tab) {
    SplitPane pane;

    pane.tabWidget = new QTabWidget;
    pane.tabWidget->setTabPosition(QTabWidget::South);
    pane.tabWidget->tabBar()->setVisible(true);
    pane.tabWidget->setDocumentMode(true);  // kill QTabWidget frame border
    // Hide the unstyled `<` `>` scroll buttons that Qt adds when tabs
    // overflow. Overflowing tabs just clip — user can widen the pane to
    // see the rest. Matches the user's preference: no graphical
    // clutter, no Qt-default chrome poking through the theme.
    pane.tabWidget->setUsesScrollButtons(false);
    pane.tabWidget->setElideMode(Qt::ElideNone);

    // Flat tabs on the editor paper — one sheet, shared with applyTheme
    // (rcx::paneTabStyle). The box borders are gone: the selected tab is
    // marked only by the underline every tab family in the app uses.
    {
        const auto& t = ThemeManager::instance().current();
        QSettings s("RC", "RC");
        pane.tabWidget->setStyleSheet(rcx::paneTabStyle(
            t, s.value("font", "JetBrains Mono").toString()));
    }

    // Create editor via controller (parent = tabWidget for ownership)
    pane.editor = tab.ctrl->addSplitEditor(pane.tabWidget);
    {
        QSettings s("RC", "RC");
        pane.editor->setRelativeOffsets(s.value("relativeOffsets", true).toBool());
        pane.editor->setHoverEffects(s.value("hoverEffects", true).toBool());
        // Session state, not a stored one: a pane opened after the user turned
        // popups on gets them, but a fresh launch starts without them.
        pane.editor->setValuePopupsEnabled(m_actValuePopups && m_actValuePopups->isChecked());
    }
    pane.editor->setPresentationMode(m_presentationMode);
    // RTTI chip click is intentionally not wired — the chip is visual
    // only. Earlier attempts (auto-create class + retype pointer, or
    // open inline rename on the root) both proved more annoying than
    // useful; the chip already shows the resolved type-name as data.
    //
    // TypeHint overlay chip → commit the inferred kind to the field.
    // For uniform splits (int32×2), the controller's batch path
    // applies the kind across the whole hex run.
    // The address bar's places menu and base context menu offer "Go to
    // address..."; the dialog lives on the main window, so the editor only
    // asks for it.
    connect(pane.editor, &RcxEditor::gotoDialogRequested, this, &MainWindow::showGotoAddressDialog);

    connect(pane.editor, &RcxEditor::typeHintChipClicked, this,
            [this](int nodeIdx, QVector<NodeKind> kinds) {
        if (kinds.isEmpty()) return;
        if (auto* c = activeController())
            c->changeNodeKind(nodeIdx, kinds.first());
    });

    // The popup footer's "✕ Hide popups" link disables value popups for the
    // emitting editor + emits this. Mirror it to the View-menu checkbox, the
    // persisted setting, and every other editor so the choice is global.
    connect(pane.editor, &RcxEditor::valuePopupsDisableRequested, this, [this]() {
        if (m_actValuePopups) m_actValuePopups->setChecked(false);
        for (auto& tab : m_tabs)
            for (auto& p : tab.panes)
                if (p.editor) p.editor->setValuePopupsEnabled(false);
    });

    // Sync View menu checkbox when editor toggles offset mode (double-click / context menu)
    connect(pane.editor, &RcxEditor::relativeOffsetsChanged, this, [this](bool rel) {
        QSettings("RC", "RC").setValue("relativeOffsets", rel);
        if (m_actRelOfs) m_actRelOfs->setChecked(rel);
        // Propagate to all other editors so they stay in sync
        for (auto& tab : m_tabs)
            for (auto& p : tab.panes)
                if (p.editor && p.editor != sender())
                    p.editor->setRelativeOffsets(rel);
    });

    // Editor + minimap container. Main editor stretches; minimap is a narrow
    // read-only Scintilla pinned on the right. Off by default — enabled via
    // View menu Minimap toggle. Text sync is driven by the editor's
    // documentApplied signal (see RcxEditor::applyDocument).
    // Custom container that paints its own 1-DEVICE-pixel border.
    // Going through QSS `border: 1px solid X` rendered as a 2-pixel
    // sunken bevel because Qt treats QSS px as logical, then the
    // display's DPR (1.5 on this monitor) multiplied that to 2 device
    // pixels — visible as a "double line". Painting at 1 / DPR uses
    // QRectF subpixel coordinates so the result is exactly one device
    // pixel thick regardless of DPI scaling.
    class EditorContainer : public QWidget {
    public:
        explicit EditorContainer() : QWidget() {}
    protected:
        void paintEvent(QPaintEvent*) override {
            // ONLY the bottom edge: the seam between the document and the view
            // tabs under it. The outline around the whole pane belongs to
            // rcx::PaneBox now (widgets/pane_tabs.h) — this class used to draw
            // all four edges, which is why Structure and Code were boxed while
            // Debug was not and Both drew two boxes. Drawing them here again
            // would land on the pane box's own device rows and double.
            const QColor seam = property("borderColor").value<QColor>();
            if (!seam.isValid()) return;
            QPainter p(this);
            rcx::fillBottomDeviceRowOfRect(p, QRectF(rect()), seam);
        }
    };
    {
        auto* ec = new EditorContainer;
        ec->setObjectName(QStringLiteral("rcxEditorContainer"));
        ec->setProperty("borderColor",
                        containerBorderColor(ThemeManager::instance().current()));
        pane.editorContainer = ec;
    }
    auto* ecLayout = new QHBoxLayout(pane.editorContainer);
    ecLayout->setContentsMargins(1, 1, 1, 1);
    ecLayout->setSpacing(0);
    ecLayout->addWidget(pane.editor, /*stretch=*/1);

    auto* mm = new MinimapScintilla;
    pane.minimap = mm;
    mm->setReadOnly(true);
    mm->setWrapMode(QsciScintilla::WrapNone);
    mm->setCaretLineVisible(false);
    mm->setMarginWidth(0, 0);
    mm->setMarginWidth(1, 0);
    mm->setMarginWidth(2, 0);
    mm->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // hide both
    mm->setFixedWidth(110);
    mm->setCursor(Qt::PointingHandCursor);
    // Font, paper and syntax colours all come from styleMinimap() so the
    // construction path and the theme-change path can never drift.
    styleMinimap(mm, ThemeManager::instance().current());
    mm->setVisible(
        QSettings("RC", "RC").value("minimap", false).toBool());
    ecLayout->addWidget(mm);

    // Translucent rectangle overlay covering the lines currently
    // visible in the main editor. Auto-resizes when the editor is
    // scrolled or zoomed (zoom changes linesOnScreen → height changes).
    auto* vIndicator = new MinimapViewportIndicator(mm);
    vIndicator->hide();

    QsciScintilla* edSci = pane.editor->scintilla();

    auto syncIndicator = [vIndicator, mmw = mm, edSci]() {
        if (!mmw->isVisible() || !edSci) { vIndicator->hide(); return; }
        int total = mmw->lines();
        if (total <= 0) { vIndicator->hide(); return; }
        int firstLine = edSci->firstVisibleLine();
        int linesOnEd = qMax(1, (int)edSci->SendScintilla(
                                    QsciScintillaBase::SCI_LINESONSCREEN));
        // Nothing to indicate when the whole document is already on screen —
        // a rectangle covering the entire map is noise, not information.
        if (firstLine == 0 && linesOnEd >= total) { vIndicator->hide(); return; }
        // Convert from main-editor line indices to minimap pixel rows.
        // Each minimap line is mm->textHeight(0) tall.
        int mmLineH = mmw->textHeight(0);
        if (mmLineH <= 0) { vIndicator->hide(); return; }
        int y = qBound(0, firstLine * mmLineH, mmw->height());
        int h = qMax(mmLineH, linesOnEd * mmLineH);
        if (y + h > mmw->height()) h = mmw->height() - y;
        vIndicator->setGeometry(0, y, mmw->width(), h);
        vIndicator->show();
        vIndicator->raise();
    };

    connect(pane.editor, &RcxEditor::documentApplied, mm,
            [mm, syncIndicator](const QString& text) {
        if (!mm->isVisible()) return;
        mm->setReadOnly(false);
        mm->setText(text);
        mm->setReadOnly(true);
        syncIndicator();
    });
    // Fire on every viewport change in the main editor — scrolls,
    // zoom-in/zoom-out, font-size changes, layout updates.
    if (edSci) {
        connect(edSci, &QsciScintilla::SCN_UPDATEUI, mm,
                [syncIndicator](int) { syncIndicator(); });
    }

    // Click on minimap → scroll main editor to that line. Centres the
    // clicked line in the main editor's viewport for a "look here" feel.
    connect(mm, &MinimapScintilla::lineClicked, pane.editor,
            [edSci](int line) {
        if (!edSci) return;
        int linesOnEd = edSci->SendScintilla(
            QsciScintillaBase::SCI_LINESONSCREEN);
        int target = qMax(0, line - linesOnEd / 2);
        edSci->setFirstVisibleLine(target);
        edSci->ensureLineVisible(line);
    });

    // Host the editor in a thin container page so "Both" can REPARENT the
    // editorContainer into a side-by-side splitter and back without disturbing
    // the tab itself (the page stays; only its child moves).
    pane.reclassPage = new QWidget;
    auto* reclassPageLay = new QVBoxLayout(pane.reclassPage);
    reclassPageLay->setContentsMargins(0, 0, 0, 0);
    reclassPageLay->setSpacing(0);
    reclassPageLay->addWidget(pane.editorContainer);
    pane.tabWidget->addTab(pane.reclassPage, rcx::kPaneTabs[0].name);  // "Structure"

    // Create per-pane rendered C++ view with find bar. Same device-exact
    // outline as the hex editor (EditorContainer) so the Code view reads as a
    // peer pane, especially side-by-side in "Both".
    {
        auto* rc = new EditorContainer;
        rc->setObjectName(QStringLiteral("rcxCodeContainer"));
        rc->setProperty("borderColor",
                        containerBorderColor(ThemeManager::instance().current()));
        pane.renderedContainer = rc;
    }
    auto* rvLayout = new QVBoxLayout(pane.renderedContainer);
    rvLayout->setContentsMargins(1, 1, 1, 1);   // room for the 1px border
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
                        " padding: 2px 6px; font-size: 13px; }"
                        "QLineEdit:focus { border-color: %4; }")
            .arg(fbTheme.backgroundAlt.name(), fbTheme.text.name(),
                 fbTheme.border.name(), fbTheme.borderFocused.name()));
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

    // Host the rendered Code view in a page too (same reparent reason as Reclass).
    pane.codePage = new QWidget;
    auto* codePageLay = new QVBoxLayout(pane.codePage);
    codePageLay->setContentsMargins(0, 0, 0, 0);
    codePageLay->setSpacing(0);
    codePageLay->addWidget(pane.renderedContainer);
    pane.tabWidget->addTab(pane.codePage, rcx::kPaneTabs[1].name);      // "Code"

    // Create Debug view: plain-text Scintilla showing composed text with visible special chars
    pane.debugView = new QsciScintilla;
    setupDebugSci(pane.debugView);
    pane.tabWidget->addTab(pane.debugView, rcx::kPaneTabs[2].name);     // "Debug"

    // "Both" — index 3 — a real view that shows the hex editor (Structure, 67%)
    // and the generated Code (33%) side by side IN THIS pane. The single tab
    // bar + corner (zoom, format/scope/gear) drive both. On entering/leaving
    // the tab the editorContainer/renderedContainer are reparented between
    // their own pages and bothSplitter (see the currentChanged handler).
    pane.bothSplitter = new QSplitter(Qt::Horizontal);
    pane.bothSplitter->setHandleWidth(1);
    pane.bothSplitter->setChildrenCollapsible(false);
    pane.tabWidget->addTab(pane.bothSplitter, rcx::kPaneTabs[3].name);  // "Both"
    // Every tab says what it shows — the strip is the only place view mode is
    // switched, so it has to explain itself.
    for (int ti = 0; ti < rcx::kPaneTabCount && ti < pane.tabWidget->count(); ++ti)
        pane.tabWidget->setTabToolTip(ti, QString::fromLatin1(rcx::kPaneTabs[ti].tip));

    // Corner widget: format combo + gear icon
    {
        const auto& ct = ThemeManager::instance().current();
        QSettings cs("RC", "RC");
        QString ef = cs.value("font", "JetBrains Mono").toString();

        auto* cornerWidget = new QWidget;
        // Named so rcx::paneTabStyle can give it the strip's surface — see the
        // #rcxPaneCorner rule there.
        cornerWidget->setObjectName(QStringLiteral("rcxPaneCorner"));
        // Pin the corner strip to the SAME height as the tabs (26px, set in
        // rcx::paneTabStyle) so AlignVCenter on its children actually
        // centers them against the Structure/Code/Debug tab text —
        // otherwise QTabWidget gives the shorter corner widget its own height
        // and the row floats above the tab baseline.
        cornerWidget->setFixedHeight(26);
        auto* cornerLayout = new QHBoxLayout(cornerWidget);
        cornerLayout->setContentsMargins(0, 0, 4, 0);
        cornerLayout->setSpacing(2);

        pane.fmtCombo = new QComboBox;
        for (int fi = 0; fi < static_cast<int>(CodeFormat::_Count); ++fi)
            pane.fmtCombo->addItem(codeFormatName(static_cast<CodeFormat>(fi)));
        pane.fmtCombo->setCurrentIndex(cs.value("codeFormat", 0).toInt());
        pane.fmtCombo->setFixedHeight(22);
        pane.fmtCombo->setStyleSheet(QStringLiteral(
            "QComboBox { background: %1; color: %2; border: 1px solid %3;"
            " padding: 1px 6px; font-family: '%6'; font-size: 9pt; }"
            "QComboBox:focus { border-color: %7; }"
            "QComboBox::drop-down { border: none; width: 14px; }"
            "QComboBox::down-arrow { image: url(:/vsicons/chevron-down.svg);"
            " width: 10px; height: 10px; }"
            "QComboBox QAbstractItemView { background: %4; color: %2;"
            " selection-background-color: %5; border: 1px solid %3; }")
            .arg(ct.background.name(), ct.textMuted.name(), ct.border.name(),
                 ct.backgroundAlt.name(), ct.hover.name(), ef,
                 ct.borderFocused.name()));

        pane.fmtGear = new QToolButton;
        pane.fmtGear->setIcon(QIcon(":/vsicons/settings-gear.svg"));
        pane.fmtGear->setFixedSize(22, 22);
        pane.fmtGear->setToolTip("Generator Options");
        pane.fmtGear->setStyleSheet(QStringLiteral(
            "QToolButton { background: %1; color: %2; border: 1px solid %3; border-radius: 0px; }"
            "QToolButton:hover { background: %4; }")
            .arg(ct.background.name(), ct.textMuted.name(), ct.border.name(),
                 ct.hover.name()));

        pane.scopeCombo = new QComboBox;
        for (int si = 0; si < static_cast<int>(CodeScope::_Count); ++si)
            pane.scopeCombo->addItem(codeScopeName(static_cast<CodeScope>(si)));
        pane.scopeCombo->setCurrentIndex(cs.value("codeScope", 0).toInt());
        pane.scopeCombo->setFixedHeight(22);
        pane.scopeCombo->setStyleSheet(pane.fmtCombo->styleSheet());

        cornerLayout->addWidget(pane.fmtCombo);
        cornerLayout->addWidget(pane.scopeCombo);
        cornerLayout->addWidget(pane.fmtGear);

        // ── Zoom — a discoverable handle for the EXISTING Scintilla zoom
        // (the same thing Ctrl+wheel drives). Always visible, separated to the
        // right of the Code-only controls. A slider over the zoom level (point
        // delta) plus a "%" readout. Bidirectional: the slider drives zoomTo()
        // and SCN_ZOOM (Ctrl+wheel) drives the slider.
        cornerLayout->addSpacing(10);
        // Match the rendered tab text: the tabs use font-size 10pt + the editor
        // font family via the QTabBar stylesheet above (NOT tabBar()->font(),
        // which returns the smaller default widget font). Mirror that here so
        // "Zoom 100%" reads at the same size as Structure/Code/Debug.
        const QFont tabFont(ef, 10);
        auto* zoomCap = new QLabel(QStringLiteral("Zoom"), cornerWidget);
        zoomCap->setFont(tabFont);
        zoomCap->setStyleSheet(QStringLiteral(
            "color: %1; padding-right: 2px;").arg(ct.textMuted.name()));
        pane.zoomSlider = new QSlider(Qt::Horizontal);
        pane.zoomSlider->setRange(-8, 24);   // ~4pt..36pt over the 12pt base
        pane.zoomSlider->setFixedWidth(90);
        pane.zoomSlider->setFixedHeight(18);   // fit within the 26px tab strip
        // Nudge the slider groove ~3px lower than its row neighbours. A styled
        // QSlider centres the groove in its contents rect, so a 6px top inset
        // moves the centre down by half = 3px. Only the slider shifts.
        pane.zoomSlider->setContentsMargins(0, 6, 0, 0);
        pane.zoomSlider->setToolTip(QStringLiteral("Zoom (also Ctrl+scroll)"));
        // Square corners, and a 3x11 bar for the handle instead of a 9px
        // puck: the strip has no other rounded shape on it.
        pane.zoomSlider->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal { height: 3px; background: %1; border-radius: 0px; }"
            "QSlider::handle:horizontal { background: %2; width: 3px; height: 11px;"
            " margin: -4px 0; border-radius: 0px; }"
            "QSlider::handle:horizontal:hover { background: %3; }")
            .arg(ct.border.name(), ct.textMuted.name(), ct.text.name()));
        pane.zoomLabel = new QLabel(QStringLiteral("100%"), cornerWidget);
        pane.zoomLabel->setFont(tabFont);
        pane.zoomLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        pane.zoomLabel->setStyleSheet(QStringLiteral(
            "color: %1; padding-left: 4px;").arg(ct.textMuted.name()));
        pane.zoomLabel->setMinimumWidth(34);
        cornerLayout->addWidget(zoomCap, 0, Qt::AlignVCenter);
        cornerLayout->addWidget(pane.zoomSlider, 0, Qt::AlignVCenter);
        cornerLayout->addWidget(pane.zoomLabel, 0, Qt::AlignVCenter);

        // Code-only controls (format/scope/gear) hidden until the Code tab is
        // selected; the zoom control stays visible on every tab.
        pane.fmtCombo->setVisible(false);
        pane.scopeCombo->setVisible(false);
        pane.fmtGear->setVisible(false);
        pane.tabWidget->setCornerWidget(cornerWidget, Qt::BottomRightCorner);

        // Apply the saved zoom level to this pane's views + slider/readout,
        // before wiring change signals (so this doesn't bounce).
        {
            int savedLevel = cs.value("viewZoomLevel", 0).toInt();
            applyPaneZoom(pane, savedLevel);
        }

        auto refreshAllRendered = [this]() {
            for (auto& tab : m_tabs)
                for (auto& p : tab.panes)
                    if (p.viewMode == VM_Rendered || p.viewMode == VM_Both)
                        updateRenderedView(tab, p);
        };

        connect(pane.fmtCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, refreshAllRendered](int idx) {
            QSettings("RC", "RC").setValue("codeFormat", idx);
            refreshAllRendered();
            for (auto& tab : m_tabs)
                for (auto& p : tab.panes)
                    if (p.fmtCombo && p.fmtCombo->currentIndex() != idx)
                        p.fmtCombo->setCurrentIndex(idx);
        });
        connect(pane.scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, refreshAllRendered](int idx) {
            QSettings("RC", "RC").setValue("codeScope", idx);
            refreshAllRendered();
            for (auto& tab : m_tabs)
                for (auto& p : tab.panes)
                    if (p.scopeCombo && p.scopeCombo->currentIndex() != idx)
                        p.scopeCombo->setCurrentIndex(idx);
        });
        // Slider → drive Scintilla zoom on this pane's views.
        QTabWidget* zw = pane.tabWidget;
        connect(pane.zoomSlider, &QSlider::valueChanged, this, [this, zw](int level) {
            if (SplitPane* p = findPaneByTabWidget(zw)) applyPaneZoom(*p, level);
        });
        // Ctrl+wheel (SCN_ZOOM) on any view → mirror its level back to the
        // slider + the pane's other views. applyPaneZoom blocks the views'
        // signals while zooming, so this can't recurse.
        auto wireZoomSync = [this, zw](QsciScintilla* s) {
            if (!s) return;
            connect(s, &QsciScintillaBase::SCN_ZOOM, this, [this, zw, s]() {
                if (SplitPane* p = findPaneByTabWidget(zw))
                    applyPaneZoom(*p, (int)s->SendScintilla(QsciScintillaBase::SCI_GETZOOM));
            });
        };
        wireZoomSync(pane.editor ? pane.editor->scintilla() : nullptr);
        wireZoomSync(pane.rendered);
        wireZoomSync(pane.debugView);
        connect(pane.fmtGear, &QToolButton::clicked, this, [this]() {
            showOptionsDialog(2); // Generator page
        });
    }

    pane.tabWidget->setCurrentIndex(0);
    pane.viewMode = VM_Reclass;

    // Right-click on the Structure/Code/Debug tab bar → quick split/unsplit
    // and tab switching. The split offers go through the same splitView
    // path as Ctrl+\, so the splitter flips to vertical (south stack) and
    // a new pane appends below.
    {
        QTabBar* tabBar = pane.tabWidget->tabBar();
        tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
        QTabWidget* tw = pane.tabWidget;
        connect(tabBar, &QWidget::customContextMenuRequested, this,
                [this, tw, tabBar](const QPoint& pos) {
            QMenu menu;
            int tabIdx = tabBar->tabAt(pos);
            if (tabIdx >= 0 && tabIdx != tw->currentIndex()) {
                menu.addAction(QStringLiteral("Open '%1'").arg(tw->tabText(tabIdx)),
                    [tw, tabIdx]() { tw->setCurrentIndex(tabIdx); });
                menu.addSeparator();
            }
            menu.addAction(QStringLiteral("Split View &Below"),
                this, &MainWindow::splitView);
            if (auto* t = activeTab(); t && t->panes.size() > 1) {
                menu.addAction(QStringLiteral("&Unsplit View"),
                    this, &MainWindow::unsplitView);
            }
            menu.exec(tabBar->mapToGlobal(pos));
        });
    }

    // Add to splitter
    // The PANE draws the outline now, not the content: one box around the
    // editor AND its view tabs, the same in all four view modes (see
    // rcx::PaneBox in widgets/pane_tabs.h for why the content stopped).
    pane.paneBox = new rcx::PaneBox(pane.tabWidget);
    tab.splitter->addWidget(pane.paneBox);

    // Connect per-pane page switching (driven by the pane view tabs)
    QTabWidget* tw = pane.tabWidget;
    connect(tw, &QTabWidget::currentChanged, this, [this, tw](int index) {
        SplitPane* p = findPaneByTabWidget(tw);
        if (!p) return;

        // Reparent the shared editor / rendered views for the selected layout.
        // (addWidget reparents; for Both the two addWidget calls always leave
        // the order [editor, rendered] regardless of where they were before.)
        if (index == 0 && p->reclassPage && p->editorContainer) {
            p->reclassPage->layout()->addWidget(p->editorContainer);
        } else if (index == 1 && p->codePage && p->renderedContainer) {
            p->codePage->layout()->addWidget(p->renderedContainer);
        } else if (index == 3 && p->bothSplitter) {
            if (p->editorContainer)   p->bothSplitter->addWidget(p->editorContainer);
            if (p->renderedContainer) p->bothSplitter->addWidget(p->renderedContainer);
            p->bothSplitter->setSizes({ 67, 33 });  // Reclass 67% | Code 33%
        }

        // Format/scope/gear belong to the Code render — show for Code AND Both.
        const bool codeish = (index == 1 || index == 3);
        if (p->fmtCombo)   p->fmtCombo->setVisible(codeish);
        if (p->scopeCombo) p->scopeCombo->setVisible(codeish);
        if (p->fmtGear)    p->fmtGear->setVisible(codeish);

        if (index == 0)      p->viewMode = VM_Reclass;
        else if (index == 1) p->viewMode = VM_Rendered;
        else if (index == 3) p->viewMode = VM_Both;
        else                 p->viewMode = VM_Debug;

        // Refresh the views the new layout shows: Code render for Code/Both,
        // debug text for Debug. (The editor/Reclass side is always live.)
        if (index == 1 || index == 2 || index == 3) {
            for (auto& tab : m_tabs) {
                for (auto& pane : tab.panes) {
                    if (&pane == p) {
                        if (index == 2) updateDebugView(tab, pane);
                        else            updateRenderedView(tab, pane);  // Code or Both
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

QStringList MainWindow::collectDirtyDocLabels(const RcxDocument* doc) const {
    if (!doc) return {};
    if (!doc->filePath.isEmpty())
        return { QFileInfo(doc->filePath).fileName() };
    return rcx::rootClassNames(doc->tree);
}

QString MainWindow::tabTitle(const TabState& tab) const {
    // Source identity is carried by the left-side DockTabSourceIcon \u2014
    // no need to append the source's filename/process to the text,
    // which used to balloon to "UnnamedClass0 \u2014 long_filename.png".
    const QString name = rootName(tab.doc->tree, tab.ctrl->viewRootId());
    return tab.ctrl->isInstanceView()
        ? name + QStringLiteral(" @ 0x") + QString::number(tab.ctrl->baseAddress(), 16).toUpper()
        : name;
}

QDockWidget* MainWindow::openInstanceBeside(RcxController* source, uint64_t structId,
                                            uint64_t address, const QString& expression,
                                            std::shared_ptr<rcx::Provider> provider) {
    if (!source) return nullptr;
    QDockWidget* sourceDock = nullptr;
    for (auto it = m_tabs.constBegin(); it != m_tabs.constEnd(); ++it)
        if (it->ctrl == source) { sourceDock = it.key(); break; }
    if (!sourceDock) return nullptr;
    const auto targetProvider = provider ? provider : source->provider();
    const int ri = source->document()->tree.indexOfId(structId);
    if (!targetProvider || !targetProvider->isReadable(address, 1) || ri < 0
        || source->document()->tree.nodes[ri].parentId != 0
        || source->document()->tree.nodes[ri].kind != NodeKind::Struct
        || (targetProvider->isLive()
            && targetProvider->pointerSize() != source->document()->tree.pointerSize)) {
        setAppStatus(QStringLiteral("Cannot open this address with the current class and source"));
        return nullptr;
    }
    auto* dock = createTab(source->document());
    auto* ctrl = m_tabs[dock].ctrl;
    if (!ctrl->configureInstance(*source, structId, address, expression, targetProvider)) {
        dock->close();
        return nullptr;
    }
    dock->setWindowTitle(tabTitle(m_tabs[dock]));
    removeDockWidget(dock);
    if (sourceDock->isFloating()) {
        addDockWidget(Qt::TopDockWidgetArea, dock);
        dock->setFloating(true);
        const QRect available = sourceDock->screen()->availableGeometry();
        const QSize size = sourceDock->size().boundedTo(available.size());
        const int x = qBound(available.left(), sourceDock->frameGeometry().right() + 8,
                             available.right() - size.width() + 1);
        const int y = qBound(available.top(), sourceDock->y(), available.bottom() - size.height() + 1);
        dock->setGeometry(QRect(QPoint(x, y), size));
    } else {
        // Use the same placement as New Horizontal Document Group. Splitting
        // a tabbed dock (including our sentinel tabs) only adds another tab.
        auto area = dockWidgetArea(sourceDock);
        if (area == Qt::NoDockWidgetArea) area = Qt::TopDockWidgetArea;
        addDockWidget(area, dock, Qt::Horizontal);
        sourceDock->show();
        sourceDock->raise();
        dock->show();
        resizeDocks({sourceDock, dock}, {width() / 2, width() / 2}, Qt::Horizontal);
    }
    dock->show();
    dock->raise();
    reconcileDockTabBars();
    setActiveDocDock(dock);
    return dock;
}

// Create a sentinel dock — invisible tab that keeps Qt's tab bar on-screen
// when only 1 real dock remains in a group.
QDockWidget* MainWindow::createSentinelDock() {
    auto* sentinel = new QDockWidget(this);
    sentinel->setObjectName(QStringLiteral("_sentinel_%1").arg(quintptr(sentinel), 0, 16));
    sentinel->setFeatures(QDockWidget::NoDockWidgetFeatures);
    sentinel->setWidget(new QWidget(sentinel));
    auto* stb = new QWidget(sentinel);
    stb->setFixedHeight(0);
    sentinel->setTitleBarWidget(stb);
    sentinel->setWindowTitle(QStringLiteral("\u200B"));
    m_sentinelDocks.append(sentinel);
    return sentinel;
}

QDockWidget* MainWindow::createTab(RcxDocument* doc) {
    PROFILE_SCOPE("MainWindow::createTab");
    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setHandleWidth(1);
    auto* ctrl = new RcxController(doc, splitter);

    QString title = rootName(doc->tree);
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(QStringLiteral("DocDock_%1").arg(quintptr(dock), 0, 16));
    dock->setFeatures(QDockWidget::DockWidgetClosable |
                      QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable);
    dock->setAttribute(Qt::WA_DeleteOnClose);
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
            QSettings settings("RC", "RC");
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
    // The tab body: every pane, then the class timeline along the bottom.
    // At the bottom, showing the strip only trims the panes from below, so
    // the first visible line never moves; and it spans the TAB, because a
    // strip inside one pane would silently rewind its split sibling too.
    auto* tabBody = new QWidget;
    tabBody->setObjectName(QStringLiteral("rcxTabBody"));
    auto* tabBodyLayout = new QVBoxLayout(tabBody);
    tabBodyLayout->setContentsMargins(0, 0, 0, 0);
    tabBodyLayout->setSpacing(0);
    tabBodyLayout->addWidget(splitter, 1);
    auto* timelineStrip = new rcx::TimelineStrip(tabBody);
    tabBodyLayout->addWidget(timelineStrip);
    dock->setWidget(tabBody);

    // Border overlay and resize grip for floating state
    auto* dockBorder = new BorderOverlay(dock);
    dockBorder->color = ThemeManager::instance().current().borderFocused;
    dockBorder->hide();

    auto* dockGrip = new ResizeGrip(dock);
    dockGrip->hide();

    // Swap title bar when floating/docking, show/hide border + grip
    connect(dock, &QDockWidget::topLevelChanged, this, [this, dock, emptyTitleBar, floatTitleBar, dockBorder, dockGrip](bool floating) {
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
            // Re-docking creates a new tab bar — reinstall pin/close buttons
            reconcileDockTabBars();
        }
    });
    dock->installEventFilter(new DockBorderFilter(dockBorder, dockGrip, dock));
    // Keep float title bar label in sync with dock title
    connect(dock, &QDockWidget::windowTitleChanged, floatTitleBar, [floatTitleBar](const QString& t) {
        if (auto* lbl = floatTitleBar->findChild<QLabel*>("dockFloatTitle"))
            lbl->setText(t);
    });

    // Tabify with the user's CURRENTLY ACTIVE doc dock — that's the one
    // they're looking at when they click "+", regardless of how many
    // separate tab groups exist. Falling back to m_docDocks.last() (the
    // old behaviour) would put the new tab wherever the most recently
    // *added* dock lives, which after a float→redock cycle is never
    // where the user expected. If neither is set, the dock starts a new
    // group at top.
    QDockWidget* tabifyTarget = nullptr;
    if (m_activeDocDock && m_activeDocDock != dock
        && !m_activeDocDock->isFloating() && m_activeDocDock->isVisible())
        tabifyTarget = m_activeDocDock;
    else if (!m_docDocks.isEmpty())
        tabifyTarget = m_docDocks.last();
    if (tabifyTarget) {
        tabifyDockWidget(tabifyTarget, dock);
    } else {
        addDockWidget(Qt::TopDockWidgetArea, dock);
        // Sentinel pairing + tab-strip styling is handled by the
        // synchronous reconcile call further below.
    }

    m_docDocks.append(dock);
    m_tabs[dock] = { doc, ctrl, splitter, {}, 0 };
    m_activeDocDock = dock;
    auto& tab = m_tabs[dock];

    // Create the initial split pane
    tab.panes.append(createSplitPane(tab));
    tab.timeline = timelineStrip;
    bindTimelineStrip(tab);

    // Apply global compact columns setting to new tab
    ctrl->setCompactColumns(QSettings("RC", "RC").value("compactColumns", true).toBool());
    ctrl->setTreeLines(QSettings("RC", "RC").value("treeLines", true).toBool());
    ctrl->setTreeColumns(QSettings("RC", "RC").value("treeColumns", true).toBool());
    ctrl->setBraceWrap(QSettings("RC", "RC").value("braceWrap", false).toBool());
    ctrl->setTypeHints(QSettings("RC", "RC").value("typeHints", false).toBool());
    ctrl->setShowComments(QSettings("RC", "RC").value("showComments", false).toBool());
    ctrl->setShowRtti(QSettings("RC", "RC").value("showRttiChips", false).toBool());

    // Give every controller the shared document list for cross-tab type visibility
    ctrl->setProjectDocuments(&m_allDocs);
    rebuildAllDocs();

    // Track active tab via visibility
    connect(dock, &QDockWidget::visibilityChanged, this, [this, dock](bool visible) {
        if (visible) {
            m_activeDocDock = dock;
            updateWindowTitle();
            refreshBookmarksDock();
            updateSourceChip();
        }
        // Keep border overlay on top after dock rearrangements
        if (m_borderOverlay) m_borderOverlay->raise();
    });
    // Refresh bookmarks on document changes (e.g. add/remove)
    connect(doc, &RcxDocument::documentChanged, this, [this, dock]() {
        if (m_activeDocDock == dock) refreshBookmarksDock();
        refreshDocTabSourceIcon(dock);
    });
    // Live-update the tab's source icon when the provider's isValid()
    // flips (process exit / reattach / file vanish). Fires from the
    // controller's existing 660ms refresh tick — no new timers.
    connect(ctrl, &RcxController::sourceLivenessChanged, this, [this, dock](bool) {
        refreshDocTabSourceIcon(dock);
        if (m_activeDocDock == dock) { updateScannerTitle(); updateSourceChip(); }
    });
    // Tri-state source status → status-bar badge/chip (transition-only).
    connect(ctrl, &RcxController::sourceStatusChanged, this,
            [this, dock](RcxController::SourceStatus) {
        if (m_activeDocDock == dock) updateSourceChip();
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
        if (m_activeDocDock == dock) {
            m_activeDocDock = m_docDocks.isEmpty() ? nullptr : m_docDocks.last();
            if (m_activeDocDock) {
                m_activeDocDock->raise();
                m_activeDocDock->show();
            }
        }
        rebuildAllDocs();
        rebuildWorkspaceModel();
        updateWindowTitle();
        if (m_tabs.isEmpty() && !m_closingAll) {
            // Zero tabs is a valid state. Purge the now-orphaned sentinels
            // (which kept solo-dock tab strips alive) and reveal the empty-
            // workspace hatch instead of force-opening a fresh class.
            qDeleteAll(m_sentinelDocks);
            m_sentinelDocks.clear();
            updateEmptyWorkspaceVisibility();
        } else if (!m_closingAll) {
            // Closing a tabified doc dock was the ONE layout event that ran
            // no tab-bar maintenance: Qt re-syncs / pools the surviving
            // dock-area tab bar (a dissolving group's bar is stripped to zero
            // tabs and recycled, deleting its close buttons; a re-inserted
            // tab comes back buttonless), and nothing reinstalled them.
            // Reconcile so every surviving tab keeps a working close button
            // and orphaned sentinels are purged. Deferred one tick so Qt has
            // finished settling the tab bar before we walk it.
            QTimer::singleShot(0, this, [this]() {
                if (!m_closingAll) reconcileDockTabBars();
            });
        }
    });

    connect(ctrl, &RcxController::nodeSelected,
            this, [this, ctrl, dock](int nodeIdx) {
        refreshSelectionStatus(ctrl, nodeIdx);
        // Update all rendered/debug panes on selection change
        auto it = m_tabs.find(dock);
        if (it != m_tabs.end()) {
            updateAllRenderedPanes(*it);
            updateAllDebugPanes(*it);
        }
    });
    connect(ctrl, &RcxController::selectionChanged,
            this, [this](int count) {
        if (count > 1)
            setAppStatus(QString("%1 nodes selected").arg(count));
    });
    connect(ctrl, &RcxController::statusHint,
            this, [this](const QString& text) { setAppStatus(text); });

    // Append Float/Close actions to any editor context menu
    connect(ctrl, &RcxController::contextMenuAboutToShow,
            this, [this, ctrl, dock](QMenu* menu, int /*line*/) {
        // "Copy as C Struct" in the menu (generator linked in main app, not tests)
        menu->addAction(makeIcon(":/vsicons/code.svg"), "Copy as C Struct", [this, ctrl]() {
            uint64_t rootId = ctrl->viewRootId();
            if (!rootId) {
                for (const auto& n : ctrl->document()->tree.nodes)
                    if (n.parentId == 0 && n.kind == rcx::NodeKind::Struct) { rootId = n.id; break; }
            }
            if (rootId) {
                const auto* aliases = ctrl->document()->typeAliases.isEmpty()
                    ? nullptr : &ctrl->document()->typeAliases;
                QApplication::clipboard()->setText(
                    rcx::renderCpp(ctrl->document()->tree, rootId, aliases));
                setAppStatus(QStringLiteral("Copied C struct to clipboard"));
            }
        });
    });

    connect(ctrl, &RcxController::requestOpenInstanceBeside, this,
            [this, ctrl](uint64_t id, uint64_t address, const QString& expression) {
        openInstanceBeside(ctrl, id, address, expression);
    });

    // Ctrl+Click navigation: open a struct in a new tab sharing the same
    // document. Mirrors the workspace-tree "Open in New Tab" action.
    connect(ctrl, &RcxController::requestOpenStructInNewTab,
            this, [this, ctrl](uint64_t structId) {
        RcxDocument* doc = ctrl->document();
        int ni = doc->tree.indexOfId(structId);
        if (ni < 0) return;
        // Reuse an existing tab if one already views this struct
        for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
            if (it->doc == doc && it->ctrl->viewRootId() == structId) {
                it.key()->raise();
                it.key()->show();
                setActiveDocDock(it.key());
                return;
            }
        }
        doc->tree.nodes[ni].collapsed = false;
        auto* newDock = createTab(doc);
        m_tabs[newDock].ctrl->setViewRootId(structId);
        m_tabs[newDock].ctrl->refresh();
        const Node& n = doc->tree.nodes[ni];
        QString name = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
        if (!name.isEmpty()) newDock->setWindowTitle(name);
        rebuildWorkspaceModel();
    });

    // Open a new tab with a plugin-provided provider (e.g. kernel physical memory)
    connect(ctrl, &RcxController::requestOpenProviderTab,
            this, [this](const QString& pluginId, const QString& target,
                         const QString& title) {
        auto* newDoc = new RcxDocument(this);
        QByteArray data(4096, '\0');
        newDoc->loadData(data);
        newDoc->tree.baseAddress = 0;

        auto* newDock = createTab(newDoc);
        auto it = m_tabs.find(newDock);
        if (it != m_tabs.end()) {
            it->ctrl->attachViaPlugin(pluginId, target);
            // Try to load PageTables.rcx template for physical kernel tabs
            QString examplesPath = QCoreApplication::applicationDirPath()
                + QStringLiteral("/examples/PageTables.rcx");
            if (QFile::exists(examplesPath))
                newDoc->load(examplesPath);
            // Set base address from provider (template has baseAddress=0,
            // but we want to start at the target physical address)
            if (newDoc->provider)
                newDoc->tree.baseAddress = newDoc->provider->base();
        }
        newDock->setWindowTitle(title);
        rebuildWorkspaceModelNow();
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
                    updateAllDebugPanes(*it2);
                    dockGuard->setWindowTitle(tabTitle(*it2));
                }
                rebuildWorkspaceModel();
                rebuildSymbols();
                updateWindowTitle();
            });
    });
    // Notify MCP clients of tree changes
    connect(doc, &RcxDocument::documentChanged, this, [this]() {
        if (m_mcp) m_mcp->notifyTreeChanged();
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
                    // Only regenerate rendered/debug panes if any are actually visible.
                    // This undo-stack path is the ONLY rendered refresh for edits +
                    // undo/redo (applyCommand→refresh() emits no documentChanged), so
                    // it MUST include VM_Both or the Code half of "Both" goes stale on
                    // every edit/undo/redo.
                    bool hasRendered = false, hasDebug = false;
                    for (const auto& pane : it2->panes) {
                        if ((pane.viewMode == VM_Rendered || pane.viewMode == VM_Both)
                            && pane.rendered && pane.rendered->isVisible())
                            hasRendered = true;
                        if (pane.viewMode == VM_Debug && pane.debugView && pane.debugView->isVisible())
                            hasDebug = true;
                    }
                    if (hasRendered) updateAllRenderedPanes(*it2);
                    if (hasDebug) updateAllDebugPanes(*it2);
                    dockGuard->setWindowTitle(tabTitle(*it2));
                }
                updateWindowTitle();
                rebuildWorkspaceModel();
            });
    });

    // Auto-focus rule: prefer the save-file's `initialClass` tag (a class
    // name to auto-open on load); fall back to the first root struct when
    // the tag is missing or names something we can't find. Match against
    // both structTypeName (canonical) and Node::name so legacy projects
    // that only set the instance name still work.
    {
        const QString& wanted = doc->tree.initialClass;
        uint64_t targetId = 0;
        if (!wanted.isEmpty()) {
            for (const auto& n : doc->tree.nodes) {
                if (n.parentId != 0 || n.kind != NodeKind::Struct) continue;
                if (n.structTypeName == wanted || n.name == wanted) {
                    targetId = n.id;
                    break;
                }
            }
        }
        if (targetId == 0) {
            // Fallback: first root struct
            for (const auto& n : doc->tree.nodes) {
                if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                    targetId = n.id;
                    break;
                }
            }
        }
        if (targetId != 0)
            ctrl->setViewRootId(targetId);
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
    reconcileDockTabBars();

    // A document is open again — re-pin the central widget to 0 height so the
    // doc-dock area reclaims the full strip and the empty-workspace hatch hides.
    updateEmptyWorkspaceVisibility();

    return dock;
}

// ── Setup dock tab bars ──
// ── Dock overlay drag system ──

void MainWindow::setupDockOverlay() {
    m_dockOverlay = new DockOverlay(this);
    m_dockDragDetector = new DockDragDetector(this, this);

    connect(m_dockDragDetector, &DockDragDetector::dragStarted,
            this, &MainWindow::onDockDragStarted);
    connect(m_dockOverlay, &DockOverlay::dropRequested,
            this, [this](QDockWidget* source, QDockWidget* target, DropZone zone) {
        onDockDropRequested(source, target, static_cast<int>(zone));
    });
    connect(m_dockOverlay, &DockOverlay::dragCancelled, this, [this](QDockWidget* dock) {
        if (!dock) return;
        // Restore to original position
        if (m_dragOrigPeer) {
            dock->setFloating(false);
            tabifyDockWidget(m_dragOrigPeer, dock);
        } else if (m_dragOrigArea != Qt::NoDockWidgetArea) {
            dock->setFloating(false);
            addDockWidget(m_dragOrigArea, dock);
        } else {
            dock->setFloating(false);
        }
        dock->show();
        dock->raise();
        reconcileDockTabBars();
    });
}

void MainWindow::onDockDragStarted(QDockWidget* dock, QPoint globalPos) {
    if (!dock || !m_dockOverlay) return;

    QString title = dock->windowTitle();
    const auto& theme = ThemeManager::instance().current();
    m_dockOverlay->setAccentColor(theme.indHoverSpan);

    // Remember where the dock was for cancel restoration
    m_dragOrigArea = dockWidgetArea(dock);
    m_dragOrigPeer = nullptr;
    auto tabified = tabifiedDockWidgets(dock);
    for (auto* peer : tabified) {
        if (peer != dock && !peer->objectName().startsWith(QStringLiteral("_sentinel_"))) {
            m_dragOrigPeer = static_cast<QDockWidget*>(peer);
            break;
        }
    }

    // Detach the dock from its tab group and make it float (hidden)
    dock->setFloating(true);
    dock->hide();

    m_dockOverlay->beginDrag(dock, title);

    // Position the overlay cursor at the initial drag point
    QPoint localPos = m_dockOverlay->mapFromGlobal(globalPos);
    QMouseEvent fakeMove(QEvent::MouseMove, localPos,
                         globalPos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_dockOverlay, &fakeMove);
}

void MainWindow::onDockDropRequested(QDockWidget* source, QDockWidget* target, int zoneInt) {
    auto zone = static_cast<DropZone>(zoneInt);
    if (!source) return;

    // Map edge zones to QMainWindow dock areas
    auto edgeToArea = [](DropZone z) -> Qt::DockWidgetArea {
        switch (z) {
        case DropZone::EdgeLeft:   return Qt::LeftDockWidgetArea;
        case DropZone::EdgeRight:  return Qt::RightDockWidgetArea;
        case DropZone::EdgeTop:    return Qt::TopDockWidgetArea;
        case DropZone::EdgeBottom: return Qt::BottomDockWidgetArea;
        default: return Qt::NoDockWidgetArea;
        }
    };

    source->show();

    // Resolve a fallback target when the cursor was over dead space —
    // pick the active doc dock, else the first non-floating doc dock.
    // Used by Center to prevent the "stayed floating" bug when the user
    // drops in the central editor area.
    auto fallbackTarget = [this, source]() -> QDockWidget* {
        if (m_activeDocDock && m_activeDocDock != source
            && !m_activeDocDock->isFloating())
            return m_activeDocDock;
        for (auto* d : m_docDocks) {
            if (d != source && !d->isFloating()) return d;
        }
        return nullptr;
    };

    if (zone == DropZone::Float) {
        // Leave floating at cursor position
        source->setFloating(true);
        source->move(QCursor::pos() - QPoint(50, 10));
        source->show();
    } else if (zone == DropZone::Center) {
        // Tabify only with a doc-dock target (objectName "DocDock_*").
        // Sidebars (workspace/scanner/symbols/bookmarks) are not valid
        // tabify targets — tabifying with one would hide the sidebar
        // behind the new dock. If the resolved target isn't a doc dock,
        // fall through to the active doc dock; if no doc dock exists,
        // dock the source at the right window edge so it never silently
        // stays floating.
        QDockWidget* tabTarget = nullptr;
        if (target && m_docDocks.contains(target))
            tabTarget = target;
        if (!tabTarget) tabTarget = fallbackTarget();
        if (tabTarget) {
            source->setFloating(false);
            tabifyDockWidget(tabTarget, source);
            source->show();
            source->raise();
        } else {
            source->setFloating(false);
            addDockWidget(Qt::RightDockWidgetArea, source);
            source->show();
        }
    } else if (zone >= DropZone::EdgeLeft && zone <= DropZone::EdgeBottom) {
        Qt::DockWidgetArea area = edgeToArea(zone);
        // Reassign corners so the chosen edge actually takes its full
        // extent (matching the preview rect). Qt's QMainWindow gives a
        // corner to exactly one of its two adjacent areas — without
        // this, BottomDockWidgetArea would be squeezed between Left/
        // Right (which currently own the bottom corners), and "Dock
        // Bottom" would just visually be a regular dock somewhere in
        // the middle. Most-recent-edge-drop wins.
        switch (area) {
        case Qt::LeftDockWidgetArea:
            setCorner(Qt::TopLeftCorner,    Qt::LeftDockWidgetArea);
            setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
            break;
        case Qt::RightDockWidgetArea:
            setCorner(Qt::TopRightCorner,    Qt::RightDockWidgetArea);
            setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
            break;
        case Qt::TopDockWidgetArea:
            setCorner(Qt::TopLeftCorner,  Qt::TopDockWidgetArea);
            setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
            break;
        case Qt::BottomDockWidgetArea:
            setCorner(Qt::BottomLeftCorner,  Qt::BottomDockWidgetArea);
            setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
            break;
        default: break;
        }
        source->setFloating(false);
        addDockWidget(area, source);
        source->show();
        // Match the preview rect (1/4 of the window) so the dock doesn't
        // balloon to its sizeHint after a drop.
        bool horiz = (area == Qt::LeftDockWidgetArea
                   || area == Qt::RightDockWidgetArea);
        int defSz = horiz ? width() / 4 : height() / 4;
        if (source == m_workspaceDock)
            defSz = computeWorkspaceDockWidth();
        int sz = loadDockSize(source, defSz);
        resizeDocks({source}, {sz}, horiz ? Qt::Horizontal : Qt::Vertical);
    }

    // Synchronous reconcile — collapses the previous family of deferred
    // 0-timers into one inline call that runs before this drop handler
    // returns. Idempotent + re-entry-guarded so cascading layout signals
    // can't blow it up.
    reconcileDockTabBars();
}

void MainWindow::reconcileDockTabBars() {
    // Re-entry guard. Cascading signals (tabifyDockWidget triggers
    // layoutChanged etc.) could call back in here; we want one pass.
    if (m_reconciling) return;
    m_reconciling = true;

    // Phase A — purge orphan sentinels. A sentinel is "orphan" iff it is
    // not tabified with any visible non-floating doc-dock. Sentinels
    // still validly partnered with a doc dock survive this pass
    // untouched (no nuke, no flicker).
    for (int i = m_sentinelDocks.size() - 1; i >= 0; --i) {
        QDockWidget* s = m_sentinelDocks[i];
        if (!s) { m_sentinelDocks.removeAt(i); continue; }
        bool keep = false;
        for (auto* td : tabifiedDockWidgets(s)) {
            auto* qd = static_cast<QDockWidget*>(td);
            if (m_docDocks.contains(qd) && qd->isVisible() && !qd->isFloating()) {
                keep = true; break;
            }
        }
        if (!keep) {
            m_sentinelDocks.removeAt(i);
            removeDockWidget(s);
            s->deleteLater();
        }
    }

    // Phase B — every visible non-floating doc dock that is not already
    // in a tab group with another doc OR a sentinel gets a fresh
    // sentinel tabified with it. Solo docks otherwise have no tab bar
    // (Qt only renders QTabBar when a dock area has 2+ docks).
    for (auto* dock : m_docDocks) {
        if (!dock || dock->isFloating() || !dock->isVisible()) continue;
        bool hasPartner = false;
        for (auto* td : tabifiedDockWidgets(dock)) {
            auto* qd = static_cast<QDockWidget*>(td);
            if (qd == dock) continue;
            if (m_docDocks.contains(qd) || m_sentinelDocks.contains(qd)) {
                hasPartner = true; break;
            }
        }
        if (!hasPartner) {
            auto* s = createSentinelDock();
            tabifyDockWidget(dock, s);
            dock->raise();
        }
    }

    // Phase B' — make sure the active tab in every doc-dock group is the
    // doc dock, never the sentinel. After a tab is dragged out of a
    // group and lands alone, Qt sometimes picks the just-added sentinel
    // as the current tab — the user sees a blank "+" tab as active.
    // Explicitly setCurrentIndex on the tab bar to the doc-dock's tab.
    for (auto* tabBar : findChildren<QTabBar*>()) {
        if (tabBar->parent() != this) continue;
        // Find the first tab whose text matches a known doc dock's title.
        // If the current tab is already a doc dock, leave it alone.
        int curIdx = tabBar->currentIndex();
        QString curText = (curIdx >= 0) ? tabBar->tabText(curIdx) : QString();
        bool curIsDoc = false;
        for (auto* d : m_docDocks)
            if (d->windowTitle() == curText) { curIsDoc = true; break; }
        if (curIsDoc) continue;
        // Current tab is the sentinel (or unknown). Find any doc-dock
        // tab in this bar and switch to it.
        for (int i = 0; i < tabBar->count(); ++i) {
            QString t = tabBar->tabText(i);
            for (auto* d : m_docDocks) {
                if (d->windowTitle() == t) {
                    tabBar->setCurrentIndex(i);
                    d->raise();
                    goto next_bar;
                }
            }
        }
        next_bar: ;
    }

    // Phase C — restyle every QTabBar that QMainWindow owns. Reinstall
    // close buttons / fonts / palette on tab bars that lack them.
    setupDockTabBars();

    m_reconciling = false;
}

// A source icon on every tab says nothing when every tab has the SAME source
// — the status chip already owns "which source am I on". Paint one only where
// a tab DIFFERS from the active tab: a single-source session shows none, and
// the odd tab out is impossible to miss. The raw identity stays on the dock
// (rcxSourceIconRaw/Raw); rcxSourceIcon is the DERIVED, painted property that
// CE_TabBarTabLabel reads.
static void applyDocTabSourceIconVisibility(const QVector<QDockWidget*>& docks,
                                            QDockWidget* active) {
    const QString aIcon = active ? active->property("rcxSourceIconRaw").toString()
                                 : QString();
    const bool aLive = active && active->property("rcxSourceLiveRaw").toBool();
    for (auto* d : docks) {
        if (!d) continue;
        const QString icon = d->property("rcxSourceIconRaw").toString();
        const bool    live = d->property("rcxSourceLiveRaw").toBool();
        const bool differs = (icon != aIcon) || (live != aLive);
        d->setProperty("rcxSourceIcon", differs ? icon : QString());
        d->setProperty("rcxSourceLive", live);
    }
}

// ── Doc-tab close button: × only where it can be clicked ──
// A row of five tabs used to be a row of five ×. Closing is a per-tab action,
// so the button belongs on the tab you are on or the tab you are pointing at.
// Only the inner button is hidden: QTabBar sizes a tab from its right widget,
// so hiding the wrapper would make tabs twitch narrower under the cursor.
static void updateDocTabCloseButtons(QTabBar* bar) {
    if (!bar) return;
    const QPoint pos = bar->mapFromGlobal(QCursor::pos());
    const int hovered = bar->rect().contains(pos) ? bar->tabAt(pos) : -1;
    for (int i = 0; i < bar->count(); ++i) {
        auto* btns = qobject_cast<DockTabButtons*>(
            bar->tabButton(i, QTabBar::RightSide));
        if (!btns || !btns->closeBtn) continue;
        btns->closeBtn->setVisible(i == bar->currentIndex() || i == hovered);
    }
}

// Hover tracking for the above — a local concern, kept out of the (already
// long) MainWindow::eventFilter. One watcher per tab bar, owned by it.
class DocTabHoverWatcher : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        switch (e->type()) {
            case QEvent::HoverEnter:
            case QEvent::HoverMove:
            case QEvent::HoverLeave:
            case QEvent::Leave:
                updateDocTabCloseButtons(qobject_cast<QTabBar*>(o));
                break;
            default: break;
        }
        return QObject::eventFilter(o, e);
    }
};

void MainWindow::refreshDocTabSourceIcon(QDockWidget* docDock) {
    if (!docDock) return;
    auto it = m_tabs.find(docDock);
    if (it == m_tabs.end()) return;
    auto* doc = it->doc;
    auto* ctrl = it->ctrl;
    if (!doc || !ctrl) return;

    QString iconPath;
    QString tip;
    bool live = false;
    int idx = ctrl->activeSourceIndex();
    const auto& saved = ctrl->savedSources();
    if (idx >= 0 && idx < saved.size()) {
        const auto& ss = saved[idx];
        iconPath = iconForProvider(ss.kind);
        live = (ctrl->provider() && ctrl->provider()->isValid());
        tip = QStringLiteral("Source: %1 (%2)")
                .arg(ss.displayName.isEmpty() ? ss.kind : ss.displayName)
                .arg(live ? QStringLiteral("connected") : QStringLiteral("disconnected"));
    } else {
        iconPath = QStringLiteral(":/vsicons/plug.svg");
        live = false;
        tip = QStringLiteral("No source — click to connect");
    }

    // Store on the dock object itself — survives any tab-reorder /
    // tabify event because QObject properties are bound to the dock,
    // not to a tab index. CE_TabBarTabLabel looks these up by tabText.
    docDock->setProperty("rcxSourceIconRaw", iconPath);
    docDock->setProperty("rcxSourceLiveRaw", live);
    // The painted properties are derived from the whole set of doc tabs.
    applyDocTabSourceIconVisibility(m_docDocks, m_activeDocDock);
    for (auto* tabBar : findChildren<QTabBar*>()) {
        if (tabBar->parent() != this) continue;
        for (int i = 0; i < tabBar->count(); ++i) {
            if (tabBar->tabText(i) == docDock->windowTitle())
                tabBar->setTabToolTip(i, tip);
        }
        tabBar->update();
    }
}

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
        tabBar->setAttribute(Qt::WA_Hover, true);
        tabBar->setElideMode(Qt::ElideNone);
        tabBar->setExpanding(false);
        tabBar->setUsesScrollButtons(true);
        // Kill QTabBar's internal 2-pixel "base" bevel (palette.dark +
        // palette.midlight strips at the tab bar's bottom). It bypasses
        // our MenuBarStyle::PE_FrameTabBarBase suppression because the
        // base bevel is computed inside QTabBar itself, not delegated
        // to PE_FrameTabBarBase. Pixel scan of the production capture
        // showed exactly two consecutive pixels (#959492 + #AAA8A4) at
        // the doc tab bar's bottom edge, immediately above the editor
        // — that's this internal bevel.
        tabBar->setDrawBase(false);
        // Set editor font so tab width sizing matches our label painting
        {
            QSettings s("RC", "RC");
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

        // Style scroll arrows (appear when tabs overflow)
        for (auto* btn : tabBar->findChildren<QToolButton*>()) {
            if (btn->arrowType() == Qt::LeftArrow) {
                btn->setArrowType(Qt::NoArrow);
                btn->setIcon(QIcon(QStringLiteral(":/vsicons/chevron-left.svg")));
                btn->setIconSize(QSize(14, 14));
            } else if (btn->arrowType() == Qt::RightArrow) {
                btn->setArrowType(Qt::NoArrow);
                btn->setIcon(QIcon(QStringLiteral(":/vsicons/chevron-right.svg")));
                btn->setIconSize(QSize(14, 14));
            } else continue;
            btn->setStyleSheet(QStringLiteral(
                "QToolButton { background: %1; border: 1px solid %2; padding: 2px; }"
                "QToolButton:hover { background: %3; }")
                .arg(theme.background.name(), theme.border.name(), theme.hover.name()));
        }

        // Sentinel "+" tab: ensure it's always the last tab
        static const QString sentinelTitle = QStringLiteral("\u200B");
        for (int i = 0; i < tabBar->count(); ++i) {
            if (tabBar->tabText(i) == sentinelTitle && i != tabBar->count() - 1) {
                tabBar->moveTab(i, tabBar->count() - 1);
                break;
            }
        }
        // The "+" is a command like any other, so it gets a command's tooltip
        // — and a tooltip has to name what THIS button does. It calls
        // project_new() bare (no classKeyword, forceFreshDoc = false), which
        // appends a STRUCT to the CURRENT project; Ctrl+N is newClass(), a
        // brand-new project with its own buffer. Advertising that shortcut
        // here promised a different command than the click performs.
        for (int i = 0; i < tabBar->count(); ++i)
            if (tabBar->tabText(i) == sentinelTitle)
                tabBar->setTabToolTip(i, QStringLiteral("New struct in this project"));

        // Helper: find any dock widget by title (doc tabs + sidebar docks)
        auto findDockByTitle = [this](const QString& title) -> QDockWidget* {
            for (auto* d : m_docDocks)
                if (d->windowTitle() == title) return d;
            for (auto* d : {m_workspaceDock, m_scannerDock, m_symbolsDock})
                if (d && d->windowTitle() == title) return d;
            return nullptr;
        };

        // Install tab buttons for any tab that doesn't have them yet
        for (int i = 0; i < tabBar->count(); ++i) {
            if (tabBar->tabText(i) == sentinelTitle)
                continue;
            auto* existing = qobject_cast<DockTabButtons*>(
                tabBar->tabButton(i, QTabBar::RightSide));
            QDockWidget* target = findDockByTitle(tabBar->tabText(i));
            if (!existing) {
                auto* btns = new DockTabButtons(tabBar);
                // Use theme.selected (not theme.hover) for the X hover.
                // CE_TabBarTabShape already paints the tab background with
                // theme.hover when the mouse is over the tab — so a same-
                // color hover on the X is invisible. theme.selected gives
                // a distinctly contrasting shade that paints visibly on
                // top of the already-hovered tab.
                btns->applyTheme(theme.text, theme.selected);
                // Freeze the wrapper's size while it still contains a visible
                // button, so hiding the × (below) never resizes the tab.
                btns->setFixedSize(btns->sizeHint());
                // Resolve the dock at CLICK time by the button's CURRENT tab
                // text — never bind the dock pointer at install time. Doc
                // docks are destroyed on close (WA_DeleteOnClose) and Qt
                // re-syncs / pools the dock-area tab bar underneath us: an
                // install-time binding either goes dead (receiver destroyed →
                // Qt auto-disconnect) or ends up under a shifted tab, which is
                // why the X stopped working after closing a sibling tab while
                // middle-click (which resolves at click time) kept working.
                // This mirrors the middle-click path exactly.
                connect(btns->closeBtn, &QToolButton::clicked, this,
                        [this, btns, findDockByTitle]() {
                    // Derive the tab bar from the button itself, not a captured
                    // pointer: Qt pools/destroys dock-area tab bars, so a
                    // captured QTabBar* could dangle. The button only emits
                    // clicked while alive, and a live tab button lives inside
                    // its current (live) tab bar — walk up to find it (robust
                    // to whatever intermediate widget Qt reparents through).
                    QTabBar* bar = nullptr;
                    for (QWidget* w = btns->parentWidget(); w; w = w->parentWidget())
                        if ((bar = qobject_cast<QTabBar*>(w))) break;
                    if (!bar) return;
                    for (int j = 0; j < bar->count(); ++j) {
                        if (bar->tabButton(j, QTabBar::RightSide) == btns) {
                            if (QDockWidget* d = findDockByTitle(bar->tabText(j)))
                                d->close();
                            return;
                        }
                    }
                });
                tabBar->setTabButton(i, QTabBar::RightSide, btns);
            }

            // Drop any stale LeftSide widget from earlier implementations —
            // the icon is now painted inline in CE_TabBarTabLabel, no
            // widget needed. A leftover widget would visually fight
            // with our inline draw.
            if (tabBar->tabButton(i, QTabBar::LeftSide))
                tabBar->setTabButton(i, QTabBar::LeftSide, nullptr);

            // Doc-dock tab: ensure source-icon properties are populated
            // on the dock. CE_TabBarTabLabel reads them on every paint.
            if (target && m_docDocks.contains(target))
                refreshDocTabSourceIcon(target);
        }
        // Newly installed buttons start hidden unless their tab is current.
        updateDocTabCloseButtons(tabBar);

        // Middle-click close + context menu + drag detection (install only once)
        if (tabBar->contextMenuPolicy() == Qt::CustomContextMenu) continue;
        tabBar->installEventFilter(this);
        // × follows the current/hovered tab; the source icons follow whichever
        // tab is current (m_activeDocDock may not have caught up yet when
        // currentChanged fires, so resolve the reference from the bar itself).
        tabBar->installEventFilter(new DocTabHoverWatcher(tabBar));
        connect(tabBar, &QTabBar::currentChanged, this, [this, tabBar](int idx) {
            updateDocTabCloseButtons(tabBar);
            QDockWidget* cur = nullptr;
            const QString title = (idx >= 0) ? tabBar->tabText(idx) : QString();
            for (auto* d : m_docDocks)
                if (d->windowTitle() == title) { cur = d; break; }
            // ...and so does every other piece of chrome that names the active
            // document. This is THE tab-click path: a tabified dock switch
            // reaches m_activeDocDock through QDockWidget::visibilityChanged,
            // which assigns the member DIRECTLY, and the focusChanged fallback
            // is then suppressed by its own (m_activeDocDock != dk) guard — so
            // without this the status segment kept the previous tab's text.
            // setActiveDocDock re-derives the icons too; the else covers the
            // sentinel / non-doc tab, where there is no active doc to name.
            if (cur && m_tabs.contains(cur))   setActiveDocDock(cur);
            else if (cur)                      applyDocTabSourceIconVisibility(m_docDocks, cur);
            // cur == nullptr means this is a SIDEBAR tab bar (or the sentinel):
            // there is no new active document, so leave the doc chrome alone
            // rather than re-deriving the icons against "no active tab".
            tabBar->update();
        });
        if (m_dockDragDetector)
            tabBar->installEventFilter(m_dockDragDetector);
        tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tabBar, &QTabBar::customContextMenuRequested,
                this, [this, tabBar](const QPoint& pos) {
            int idx = tabBar->tabAt(pos);
            if (idx < 0) return;
            // No context menu on sentinel "+" tab
            QString tabTitle = tabBar->tabText(idx);
            if (tabTitle == QStringLiteral("\u200B")) return;
            QDockWidget* target = nullptr;
            for (auto* d : m_docDocks)
                if (d->windowTitle() == tabTitle) { target = d; break; }
            if (!target) {
                for (auto* d : {m_workspaceDock, m_scannerDock, m_symbolsDock})
                    if (d && d->windowTitle() == tabTitle) { target = d; break; }
            }
            if (!target) return;

            bool isDocDock = m_docDocks.contains(target);

            QMenu menu;

            // Close
            menu.addAction(makeIcon(":/vsicons/close.svg"), "Close",
                           [target]() { target->close(); });

            // Doc-only actions
            if (isDocDock) {
                auto tabIt = m_tabs.find(target);

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
            }

            menu.addSeparator();

            // Float / Dock
            menu.addAction(target->isFloating() ? "Dock" : "Float", [target]() {
                target->setFloating(!target->isFloating());
            });

            // Show / Hide the Project workspace (on document tabs). Label is
            // rebuilt with the menu on every right-click, so it always reflects
            // the workspace's current visibility.
            if (isDocDock && m_workspaceDock) {
                menu.addSeparator();
                const bool wsVisible = m_workspaceDock->isVisible();
                menu.addAction(makeIcon(":/vsicons/layout-sidebar-left.svg"),
                               wsVisible ? "Hide Workspace" : "Show Workspace",
                               [this, wsVisible]() {
                    applyLayoutPreset(wsVisible ? Layout_NoWorkspace
                                                : Layout_Workspace);
                });
            }

            // New Document Groups (doc tabs only, >1 visible tab)
            if (isDocDock) {
                menu.addSeparator();

                int visibleTabs = 0;
                for (int i = 0; i < tabBar->count(); ++i)
                    if (tabBar->isTabVisible(i)) ++visibleTabs;
                if (visibleTabs > 1) {
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
                        // Sentinel pairing + tab-strip styling is handled
                        // by the synchronous reconcile pass — no defer.
                        reconcileDockTabBars();
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
                        reconcileDockTabBars();
                    });
                }
            }

            menu.exec(tabBar->mapToGlobal(pos));
        });
    }

    // Re-raise border overlay — new tab bars created during dock rearrangement
    // can end up above the overlay in z-order
    if (m_borderOverlay) {
        m_borderOverlay->setGeometry(rect());
        m_borderOverlay->raise();
    }

    // Whatever just happened to the tab strip (a tab created, closed, moved,
    // docked or undocked), the status segment has to describe the document
    // that is on screen NOW. QTabBar::currentChanged covers a plain tab click,
    // but not the cases where the bar itself is rebuilt or destroyed: creating
    // the first tab left the segment EMPTY, and closing the active one left it
    // naming the class that had just gone away. refreshSelectionStatus is a
    // pure rebuild from the controller's current selection, so calling it on
    // every reconcile is idempotent — it never invents or loses a selection.
    if (m_activeDocDock) {
        if (auto it = m_tabs.find(m_activeDocDock); it != m_tabs.end() && it->ctrl)
            refreshSelectionStatus(it->ctrl, -1);
    }
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    // ── F12: app-only screenshot to <exe-dir>/issue.png ──
    // Captures only the Reclass main window (including any child
    // overlays painted on top, e.g. DockOverlay during a drag) — NOT
    // the whole screen. Triggered from the qApp event filter so it
    // works even when DockOverlay has the keyboard focus.
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_F12 && !ke->isAutoRepeat()) {
            QScreen* screen = windowHandle() ? windowHandle()->screen()
                                             : QGuiApplication::primaryScreen();
            // grab the SCREEN rect that this window occupies (rather
            // than the window only) so any visible top-level popups
            // overlapping the editor — hover preview, autocomplete,
            // tooltips — get captured along with the main UI. The
            // previous grabWindow(winId(), …) call missed Qt::ToolTip
            // child popups entirely because they are independent
            // top-level windows, not painted into winId().
            QPixmap pix;
            if (screen) {
                QPoint tl = mapToGlobal(QPoint(0, 0));
                pix = screen->grabWindow(0, tl.x(), tl.y(),
                                         width(), height());
            } else {
                pix = grab();  // fallback: widget-tree paint
            }
            QString path = QCoreApplication::applicationDirPath()
                         + QStringLiteral("/issue.png");
            if (!pix.isNull() && pix.save(path, "PNG")) {
                setAppStatus(QStringLiteral("Screenshot → ") + path);
                qDebug() << "[Screenshot] saved" << path
                         << pix.width() << "x" << pix.height();
            } else {
                setAppStatus(QStringLiteral("Screenshot FAILED: ") + path);
            }
            return true;
        }
    }

    // ── Ctrl+Click: UI inspection ──
    if (event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton
            && (me->modifiers() & Qt::ControlModifier)
            && (me->modifiers() & Qt::ShiftModifier)) {
            QPoint globalPos = me->globalPosition().toPoint();
            QWidget* hit = qApp->widgetAt(globalPos);
            if (hit) {
                auto result = inspectAt(hit, hit->mapFromGlobal(globalPos));
                if (result.selected && result.region == m_inspectedRegion.region
                    && m_inspectedRegion.selected) {
                    clearInspection();
                } else {
                    m_inspectedRegion = result;
                    // Show overlay
                    if (!m_inspectionOverlay)
                        m_inspectionOverlay = new InspectionOverlay(this);
                    auto* overlay = static_cast<InspectionOverlay*>(m_inspectionOverlay);
                    overlay->highlightRect = QRect(
                        mapFromGlobal(result.globalRect.topLeft()),
                        result.globalRect.size());
                    overlay->label = result.region;
                    overlay->setGeometry(rect());
                    overlay->raise();
                    overlay->show();
                    overlay->update();
                    // Status bar
                    QString dimPart;
                    for (const auto& v : result.themeColors) {
                        auto co = v.toObject();
                        dimPart += QStringLiteral("  %1=%2")
                            .arg(co["key"].toString(), co["value"].toString());
                    }
                    setAppStatus(result.region, dimPart);
                }
            }
            return true; // consume
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (auto* tabBar = qobject_cast<QTabBar*>(obj)) {
            int idx = tabBar->tabAt(me->pos());
            if (idx >= 0 && tabBar->tabText(idx) == QStringLiteral("\u200B")) {
                // Sentinel "+" tab: left-click opens new struct, ignore others
                if (me->button() == Qt::LeftButton) {
                    auto* dock = project_new();
                    if (dock) {
                        dock->raise();
                        dock->show();
                        reconcileDockTabBars();
                    }
                    return true;
                }
                return true;  // swallow middle-click etc.
            }
            if (me->button() == Qt::MiddleButton && idx >= 0) {
                QString title = tabBar->tabText(idx);
                for (auto* d : m_docDocks) {
                    if (d->windowTitle() == title) { d->close(); return true; }
                }
                for (auto* d : {m_workspaceDock, m_scannerDock, m_symbolsDock}) {
                    if (d && d->windowTitle() == title) { d->close(); return true; }
                }
                return true;
            }
        }
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        // Persist the new dock size on drag-release. Otherwise the user
        // resizes the workspace dock once, restarts the app, and Qt
        // re-spawns it at the unbounded default again — which is
        // exactly the "separator drag doesn't stick" complaint.
        for (QDockWidget* d : {m_workspaceDock, m_bookmarksDock,
                                m_symbolsDock, m_scannerDock}) {
            if (!d || obj != d) continue;
            saveDockSize(d);
            break;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// Build a minimal empty struct for new documents
static int s_classCounter = 0;

static void buildEmptyStruct(NodeTree& tree, const QString& classKeyword = QString()) {
    // ── Enum: bare node with empty enumMembers, no hex children ──
    if (classKeyword == QStringLiteral("enum")) {
        int idx = s_classCounter++;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = QStringLiteral("UnnamedEnum%1").arg(idx);
        root.structTypeName = root.name;
        root.classKeyword = classKeyword;
        root.parentId = 0;
        root.offset = 0;
        root.enumMembers = {
            {QStringLiteral("Member0"), 0},
            {QStringLiteral("Member1"), 1},
            {QStringLiteral("Member2"), 2},
            {QStringLiteral("Member3"), 3},
            {QStringLiteral("Member4"), 4},
        };
        tree.addNode(root);
        return;
    }

    int idx = s_classCounter++;
    Node root;
    root.kind = NodeKind::Struct;
    root.name = QStringLiteral("instance%1").arg(idx);
    root.structTypeName = QStringLiteral("UnnamedClass%1").arg(idx);
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

MainWindow::~MainWindow() {
    /*
     * When MainWindow is destroyed:
     *
	 *	  1. ~MainWindow() runs (our code — plugin DLLs still loaded)
	 *	  2. MainWindow member variables are destroyed (m_pluginManager — unloads plugin DLLs)
	 *	  3. QObject::~QObject() runs — destroys child widgets (QMdiSubWindow → RcxController → ~RcxController())
     * 
     */

    // Scan results and undoable writes also retain their originating provider.
    delete m_scannerPanel;
    m_scannerPanel = nullptr;
    QSet<RcxDocument*> clearedDocs;
    for (const auto& tab : m_tabs) {
        if (clearedDocs.contains(tab.doc)) continue;
        const QSignalBlocker blocker(&tab.doc->undoStack);
        tab.doc->undoStack.clear();
        clearedDocs.insert(tab.doc);
    }

    // Disconnect all signals before members are torn down,
    // so the lambdas capturing 'this' never fire on a half-destroyed object.
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        disconnect(it.key(), &QObject::destroyed, this, nullptr);
        disconnect(&it->doc->undoStack, nullptr, this, nullptr);
        // Release providers now while plugin DLLs are still loaded;
        // if deferred to Qt child cleanup the DLL code may already be unloaded.
        it->ctrl->resetProvider();
        it->doc->provider.reset();
    }
    m_tabs.clear();
}

void MainWindow::newClass() {
    project_new(QStringLiteral("class"), /*forceFreshDoc=*/true);
}

void MainWindow::newStruct() {
    project_new(QString(),                /*forceFreshDoc=*/true);
}

void MainWindow::newEnum() {
    project_new(QStringLiteral("enum"),   /*forceFreshDoc=*/true);
}

// Returns the RcxEditor root struct id so the caller can pin viewRootId.
static uint64_t buildEditorDemo(NodeTree& tree, uintptr_t editorAddr) {
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
    root.collapsed = false;
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
    // QObject layout (Qt 6 / Itanium ABI on MinGW):
    //   +0  vtable pointer → QObject's vtable (effectively QWidget's via inheritance)
    //   +8  QObjectData* d_ptr — Qt's pimpl, holds parent/children/objectName
    // QWidget extends QObject and adds its own d_ptr-style internals; the
    // exact size of QWidget's instance data depends on the Qt build, so we
    // mark the QWidget body as raw bytes and let the user explore.
    // Past the QWidget base, RcxEditor's own members start. The exact offsets
    // depend on Qt build flags + compiler padding, so we don't hardcode them
    // beyond +0 / +8 — the user sees them as hex64 and can rename / retype
    // them live (which is the whole point of a Reclass tutorial).
    auto addPtr = [&](uint64_t parent, int off, const QString& name,
                      uint64_t refId = 0,
                      const QString& comment = {}) {
        Node n;
        n.kind = NodeKind::Pointer64;
        n.name = name;
        n.parentId = parent;
        n.offset = off;
        n.refId = refId;
        n.comment = comment;
        tree.addNode(n);
    };
    auto addHex = [&](uint64_t parent, int off, const QString& name) {
        Node n;
        n.kind = NodeKind::Hex64;
        n.name = name;
        n.parentId = parent;
        n.offset = off;
        tree.addNode(n);
    };

    // ── Tutorial: example enum so the Enum chip lights up on a member ──
    // A small RGBA-style enum used by an int field below. Demonstrates
    // the Enum chip ((MEMBER)) on a UInt32 whose refId points at this
    // top-level enum struct.
    Node demoEnum;
    demoEnum.kind = NodeKind::Struct;
    demoEnum.classKeyword = QStringLiteral("enum");
    demoEnum.name = QStringLiteral("EditorColor");
    demoEnum.structTypeName = QStringLiteral("EditorColor");
    demoEnum.enumMembers = {
        {QStringLiteral("BLACK"), 0},
        {QStringLiteral("RED"),   1},
        {QStringLiteral("GREEN"), 2},
        {QStringLiteral("BLUE"),  3},
    };
    int dei = tree.addNode(demoEnum);
    uint64_t demoEnumId = tree.nodes[dei].id;

    // __vptr — the merged RTTI+Symbol chip is auto-emitted by compose
    // (the live demo's whole point), so no user comment is needed here.
    // Earlier the demo seeded a teaching comment on every demo field,
    // but that turned into noise once chips landed: real Reclass docs
    // never have a tutorial-text comment on every field, and seeing
    // one made the demo look like "what is this shit" (user verbatim).
    addPtr(rootId, 0, QStringLiteral("__vptr"), vtId);
    addPtr(rootId, 8, QStringLiteral("d_ptr"));
    // Enum chip demo: a UInt32 field referencing the enum above. The
    // (MEMBER) chip fires automatically when the live memory at this
    // offset matches an enum value — no comment needed.
    {
        Node n;
        n.kind = NodeKind::UInt32;
        n.name = QStringLiteral("colorMode");
        n.parentId = rootId;
        n.offset = 0x10;
        n.refId = demoEnumId;
        tree.addNode(n);
    }
    // The rest of the object: raw memory visible as Hex64 fields with
    // no auto-generated names. Tutorial-style "qwidget_internal_18 /
    // rcxeditor_member_050 / field_00c4" labels were dropped because
    // they LOOKED like reverse-engineering progress already made by us
    // on the user's behalf — when the actual point of the tutorial is
    // for the user to discover, name, and type these fields themselves.
    // Blank names also let the eye go straight to the values + chips.
    // Loop starts at 0x18 — colorMode UInt32 (above) already occupies 0x10.
    for (int off = 0x18; off < 0x200; off += 8)
        addHex(rootId, off, QString());
    return rootId;
}

void MainWindow::selfTest() {
#ifdef Q_OS_WIN
    // Tutorial flow: open *the editor demo* as the active tab so the user
    // lands on a meaningful RcxEditor* layout instead of a blank class.
    // The demo points at the real RcxEditor object in this process — the
    // user is literally inspecting the editor that's drawing the inspection.
    project_new();
    auto* editorCtrl = activeController();
    if (!editorCtrl || editorCtrl->editors().isEmpty()) return;
    auto* editor = editorCtrl->editors().first();
    auto* editorDoc = editorCtrl->document();

    // Lay out the RcxEditor struct skeleton at the live object address.
    // Capture the root id so we can pin viewRootId — the controller's
    // pre-existing viewRootId points at the now-deleted UnnamedClass that
    // project_new() built before we wiped the tree.
    uint64_t rcxEditorId = buildEditorDemo(editorDoc->tree,
        reinterpret_cast<uintptr_t>(editor));

    // Tag the document with the auto-open class. On project load this is
    // what `createTab` looks up to set viewRootId — so even if the user
    // saves the demo as a .rcx and reopens it later, RcxEditor auto-opens.
    // The demo additionally pins baseAddress (below) so the user lands on
    // the live editor object instead of an arbitrary VA.
    editorDoc->tree.initialClass = QStringLiteral("RcxEditor");

    // Attach to self via processmemory plugin — provider supplies live bytes
    // so the vtable + d_ptr fields show real values. Guard against the
    // plugin being missing at runtime (user deleted the DLL, custom build
    // without it, etc.) — without the check, attachViaPlugin pops a
    // "Provider Unavailable" dialog mid-tutorial which derails the flow.
    // Silent skip is fine; the address bar's chip just reads "Select
    // source" and the tutorial still demonstrates the layout, just with
    // empty bytes instead of live ones.
    if (ProviderRegistry::instance().findProvider(QStringLiteral("processmemory"))) {
        DWORD pid = GetCurrentProcessId();
        QString target = QString("%1:RC.exe").arg(pid);
        editorCtrl->attachViaPlugin(QStringLiteral("processmemory"), target);
    }

    // Live self-attach is a foot-gun: writing to e.g. __vptr would stomp
    // this RcxEditor instance's vtable and Qt's next virtual dispatch
    // crashes the editor (Reclass+0x… → call qword ptr [rax+30h] with
    // rax=1). The tutorial is for INSPECTION — disable writes entirely
    // through the controller. The provider can still report writable;
    // setNodeValue + cmd::WriteBytes refuse to commit while the override
    // is on, so undo/redo also can't replay an old write later.
    editorCtrl->setReadOnlyOverride(true);

    // Tutorial: turn every chip kind on so the user sees the full set in
    // the demo (Comment / TypeHint / Rtti / Enum). These respect each
    // tab's controller, so the user's persisted View toggles for *other*
    // tabs aren't affected — only the demo flips them on for visibility.
    editorCtrl->setShowComments(true);
    editorCtrl->setShowRtti(true);
    editorCtrl->setShowEnumChips(true);
    // setTypeHints intentionally not called — TypeHint chips were
    // retired (see compose.cpp).

    // The plugin attach can rewrite baseAddress to its own default — pin
    // it back at the actual editor object so the demo lands where intended.
    editorDoc->tree.baseAddress = reinterpret_cast<uint64_t>(editor);
    editorDoc->tree.baseAddressFormula.clear();
    // Point the controller's view at the freshly-built RcxEditor root,
    // not the stale UnnamedClass id from before buildEditorDemo wiped
    // the tree. Without this the command row reads "struct Untitled"
    // (the empty-tree placeholder) and compose may render the wrong root.
    editorCtrl->setViewRootId(rcxEditorId);

    // Find the editor demo's dock (the most recently added doc dock that
    // owns this controller) and rename its window title to something
    // self-explanatory in the dock tab bar.
    QDockWidget* editorDock = nullptr;
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        if (it->ctrl == editorCtrl) { editorDock = it.key(); break; }
    }
    if (editorDock)
        editorDock->setWindowTitle(QStringLiteral("RcxEditor* (live)"));

    // Optional: a second tab with an empty class for the user to noodle on.
    // Created BEFORE we raise the editor tab so the editor demo wins focus.
    auto* userTab = project_new(QStringLiteral("class"));
    if (userTab)
        userTab->setWindowTitle(QStringLiteral("Scratch"));

    // Raise the editor demo last so it becomes the active tab on Continue.
    if (editorDock) {
        editorDock->raise();
        editorDock->show();
        m_activeDocDock = editorDock;
        editorCtrl->refresh();
    }
#else
    // Non-Windows: no live process self-attach available. Just open a
    // scratch class so the user has something to play with.
    auto* userTab = project_new(QStringLiteral("class"));
    if (userTab) userTab->raise();
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
    ctrl->deleteSelection();   // same path as the Delete key and the ribbon
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
    // Split south (Structure on top, Code below). The splitter is created
    // Qt::Horizontal in createTab so the workspace dock layout has a
    // natural left-right metaphor; for in-tab pane splits the user
    // wants stacked vertically so they can read full-width rows of
    // both views at once.
    if (tab->splitter)
        tab->splitter->setOrientation(Qt::Vertical);
    tab->panes.append(createSplitPane(*tab));
    // The new pane's editor must feed the ribbon's inline-edit tracking too.
    if (m_ribbonActions) m_ribbonActions->setActiveController(tab->ctrl);
}

void MainWindow::unsplitView() {
    auto* tab = activeTab();
    if (!tab || tab->panes.size() <= 1) return;
    auto pane = tab->panes.takeLast();
    tab->ctrl->removeSplitEditor(pane.editor);
    pane.tabWidget->deleteLater();
    tab->activePaneIdx = qBound(0, tab->activePaneIdx, tab->panes.size() - 1);
    if (m_ribbonActions) m_ribbonActions->setActiveController(tab->ctrl);
}

void MainWindow::previewBothSplit() {
    auto* tab = activeTab();
    if (!tab || tab->panes.isEmpty()) return;
    tab->panes.first().tabWidget->setCurrentIndex(3);  // select the "Both" view
}

// --screenshot code: exercise the SAME path the ribbon's Code button uses, so
// the capture proves the real command rather than a parallel shortcut.
void MainWindow::previewCodeView() {
    showCodeView(CodeScope::FullSdk);
}

void MainWindow::previewLargeFile() {
    const QString path = qEnvironmentVariable("RCX_LARGE_FILE");
    if (path.isEmpty()) { fprintf(stderr, "LARGE_FILE_RESULT FAIL no fixture\n"); return; }
    QElapsedTimer timer;
    timer.start();
    auto* dock = project_open(path);
    if (!dock) { fprintf(stderr, "LARGE_FILE_RESULT FAIL open\n"); return; }
    const qint64 openMs = timer.elapsed();
    auto* ctrl = m_tabs[dock].ctrl;
    auto file = std::dynamic_pointer_cast<FileProvider>(ctrl->provider());
    if (!file || file->byteSize() < 256) { fprintf(stderr, "LARGE_FILE_RESULT FAIL provider\n"); return; }
    QFile reference(path);
    bool ok = reference.open(QIODevice::ReadOnly) && ctrl->document()->filePath.isEmpty();
    const uint64_t size = file->byteSize();
    timer.restart();
    const QVector<uint64_t> offsets = {0, qMin(size - 256, (uint64_t(1) << 31) - 8),
        qMin(size - 256, (uint64_t(1) << 32) - 8), size / 2, size - 256};
    for (uint64_t offset : offsets) {
        ok = reference.seek(qint64(offset)) && ok;
        ok = file->readBytes(offset, 256) == reference.read(256) && ok;
    }
    const qint64 sampleMs = timer.elapsed();
    resize(1280, 720);
    uint64_t root = ctrl->viewRootId();
    if (!root && !ctrl->document()->tree.nodes.isEmpty()) root = ctrl->document()->tree.nodes.first().id;
    auto* otherDock = openInstanceBeside(ctrl, root, size - 128);
    ok = otherDock && m_tabs[otherDock].ctrl->baseAddress() == size - 128 && ok;
    fprintf(stderr, "LARGE_FILE_RESULT %s bytes=%llu open_ms=%lld sample_ms=%lld cache_bytes=%d\n",
        ok ? "PASS" : "FAIL", static_cast<unsigned long long>(size),
        static_cast<long long>(openMs), static_cast<long long>(sampleMs), file->cachedBytes());
    fflush(stderr);
}

void MainWindow::previewInstances() {
    auto* source = activeController();
    if (!source) return;
    auto* doc = source->document();
    doc->undoStack.clear();
    doc->tree = NodeTree();
    doc->tree.baseAddress = 0x20;
    Node root;
    root.kind = NodeKind::Struct;
    root.name = root.structTypeName = QStringLiteral("Player");
    root.classKeyword = QStringLiteral("class");
    root.collapsed = false;
    const uint64_t rootId = doc->tree.nodes[doc->tree.addNode(root)].id;
    auto field = [&](int offset, NodeKind kind, const QString& name) {
        Node n;
        n.kind = kind; n.offset = offset; n.parentId = rootId; n.name = name;
        if (kind == NodeKind::Pointer64) n.refId = rootId;
        doc->tree.addNode(n);
    };
    field(0, NodeKind::Float, QStringLiteral("health"));
    field(4, NodeKind::UInt32, QStringLiteral("team"));
    field(8, NodeKind::Vec3, QStringLiteral("position"));
    field(20, NodeKind::Pointer64, QStringLiteral("next"));
    QByteArray bytes(512, '\0');
    const float firstHealth = 100, secondHealth = 62.5f;
    const uint32_t firstTeam = 1, secondTeam = 2;
    const float firstPosition[3] = {10, 20, 30}, secondPosition[3] = {14, 25, 30};
    const uint64_t next = 0x80;
    memcpy(bytes.data() + 0x20, &firstHealth, 4);
    memcpy(bytes.data() + 0x24, &firstTeam, 4);
    memcpy(bytes.data() + 0x28, firstPosition, 12);
    memcpy(bytes.data() + 0x34, &next, 8);
    memcpy(bytes.data() + 0x80, &secondHealth, 4);
    memcpy(bytes.data() + 0x84, &secondTeam, 4);
    memcpy(bytes.data() + 0x88, secondPosition, 12);
    doc->provider = std::make_shared<BufferProvider>(bytes, QStringLiteral("instances.bin"));
    source->copySavedSources({}, -1);
    source->setReadOnlyOverride(false);
    source->setViewRootId(rootId);
    source->refresh();
    const int width = qEnvironmentVariableIntValue("RCX_INSTANCE_SHOT_WIDTH");
    resize(width > 0 ? width : 1280, 720);
    for (int line = 0; line < source->lastResult().meta.size(); ++line) {
        if (source->lastResult().meta[line].nodeIdx == 4) {
            source->openPointerBeside(source->primaryEditor(), line);
            break;
        }
    }
    QPointer<RcxController> left = source;
    QPointer<RcxController> right = activeController();
    QTimer::singleShot(500, this, [this, left, right] {
        if (!left || !right || left == right) { fprintf(stderr, "INSTANCES_RESULT FAIL no second view\n"); return; }
        QDockWidget* leftDock = nullptr;
        QDockWidget* rightDock = nullptr;
        for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
            if (it->ctrl == left) leftDock = it.key();
            if (it->ctrl == right) rightDock = it.key();
        }
        const bool ok = leftDock && rightDock && leftDock->isVisible() && rightDock->isVisible()
            && leftDock->geometry().right() <= rightDock->geometry().left()
            && left->document() == right->document() && left->baseAddress() == 0x20
            && right->baseAddress() == 0x80
            && left->lastResult().text.contains(QStringLiteral("100.00f"))
            && right->lastResult().text.contains(QStringLiteral("62.500"));
        fprintf(stderr, "INSTANCES_RESULT %s\n", ok ? "PASS" : "FAIL");
        fflush(stderr);
    });
}

// --screenshot symbols: open the Symbols dock at the narrow width the header
// chips / sort row have to survive (the user's dock is ~270 px).
// --screenshot fontmenu: pop View ▸ Font where the capture can see it.
// The rows are built in the menu's aboutToShow handler from whatever the
// font database reports, so this is the only way to check what the list
// actually looks like on a given machine — the families differ per platform
// by design, and each row wears its own face.
// --screenshot nodemenu: pop the editor's right-click menu where the capture
// can see it. showContextMenu() exec()s, so the caller arms a timer that
// photographs the live popup and closes it. This is the only way to check
// the DESTRUCTIVE red on a menu label: it is painted by MenuBarStyle, which
// is installed on the application, so no render harness can show it.
void MainWindow::previewNodeMenu() {
    auto* tab = activeTab();
    if (!tab || tab->panes.isEmpty() || !tab->ctrl) return;
    rcx::RcxEditor* ed = tab->panes.first().editor;
    if (!ed) return;
    tab->ctrl->refresh();
    const auto& meta = tab->ctrl->lastResult().meta;
    for (int i = 0; i < meta.size(); i++) {
        if (meta[i].nodeIdx <= 0 || meta[i].lineKind != rcx::LineKind::Field) continue;
        tab->ctrl->showContextMenu(ed, i, meta[i].nodeIdx, 0,
                                   mapToGlobal(QPoint(160, 160)));
        return;
    }
}

// --screenshot <png> timeline: the class timeline, end to end, in the real
// app. project_new()'s class is self-attached — the processmemory provider
// reading a buffer in our own process — so it is a genuinely LIVE source:
// writing into that buffer on a timer sends real changes through the real
// refresh loop into the timeline. Saves <png>_live.png, then scrubs to the
// middle of what was captured and saves <png> showing the past.
void MainWindow::previewTimeline(const QString& ssPath) {
    auto* tab = activeTab();
    if (!tab || !tab->doc || !tab->doc->m_ownedBuffer || !tab->ctrl) {
        fprintf(stderr, "timeline: no self-attached class\n");
        QApplication::quit();
        return;
    }
    uint8_t* buf = tab->doc->m_ownedBuffer.get();
    const size_t size = tab->doc->m_ownedBufferSize;
    auto* mutate = new QTimer(this);
    auto ticks = std::make_shared<int>(0);
    connect(mutate, &QTimer::timeout, this, [buf, size, ticks]() {
        const int t = ++*ticks;
        const uint32_t health = 100 - uint32_t(t % 60);
        std::memcpy(buf, &health, 4);
        if (t % 3 == 0) {
            const uint32_t ammo = uint32_t(t / 3);
            std::memcpy(buf + 4, &ammo, 4);
        }
        if (t % 17 == 0)
            for (size_t i = 16; i < 48 && i < size; ++i) buf[i] = uint8_t(t * 31 + int(i));
    });
    mutate->start(60);

    QPointer<RcxController> c(tab->ctrl);
    // Nothing is captured until Record.
    tab->ctrl->timelineToggleRecording();
    QTimer::singleShot(4000, this, [this, mutate, c, ssPath]() {
        mutate->stop();
        QString live = ssPath;
        live.insert(live.lastIndexOf(QLatin1Char('.')), QStringLiteral("_live"));
        grab().save(live);
        if (!c || !c->timelineContext() || c->timelineContext()->model().isEmpty()) {
            fprintf(stderr, "timeline: nothing captured\n");
            QApplication::quit();
            return;
        }
        const tl::TimelineModel& m = c->timelineContext()->model();
        const int64_t first = m.timeOf(m.firstRecord());
        const int64_t last = m.timeOf(m.lastRecord());
        c->viewTimelineAt((first + last) / 2);
        fprintf(stderr, "timeline: %d records over %lld ms; viewing record %u (past=%d)\n",
                m.size(), (long long)(last - first), (unsigned)c->pastRecord(), int(c->isViewingPast()));
        QTimer::singleShot(1200, this, [this, c, ssPath]() {
            grab().save(ssPath);
            fprintf(stderr, "timeline: past=%d -> %s\n", int(c && c->isViewingPast()), qPrintable(ssPath));
            QApplication::quit();
        });
    });
}

void MainWindow::previewSimulationTimeline(const QString& ssPath) {
    project_newSimulation();
    auto* tab = activeTab();
    if (!tab || !tab->ctrl) {
        fprintf(stderr, "simtimeline: no simulation tab\n");
        QApplication::quit();
        return;
    }
    QPointer<RcxController> c(tab->ctrl);
    auto suffixed = [ssPath](const char* tag) {
        QString p = ssPath;
        p.insert(p.lastIndexOf(QLatin1Char('.')), QLatin1String(tag));
        return p;
    };
    // Record after the first compose, for about seven seconds of bouncing.
    QTimer::singleShot(600, this, [c]() { if (c) c->timelineToggleRecording(); });
    QTimer::singleShot(7600, this, [this, c, ssPath, suffixed]() {
        grab().save(ssPath);                                   // recording
        if (!c) { QApplication::quit(); return; }
        c->timelineToggleRecording();                          // Stop
        QTimer::singleShot(900, this, [this, c, suffixed]() {
            grab().save(suffixed("_stopped"));
            if (!c || !c->timelineContext() || c->timelineContext()->model().isEmpty()) {
                fprintf(stderr, "simtimeline: nothing recorded\n");
                QApplication::quit();
                return;
            }
            const tl::TimelineModel& m = c->timelineContext()->model();
            const int64_t first = m.timeOf(m.firstRecord());
            const int64_t last = m.timeOf(m.lastRecord());
            c->viewTimelineAt(first + (last - first) * 2 / 5);
            QTimer::singleShot(900, this, [this, c, suffixed]() {
                grab().save(suffixed("_past"));
                fprintf(stderr, "simtimeline: past=%d\n", int(c && c->isViewingPast()));
                QApplication::quit();
            });
        });
    });
}

void MainWindow::previewFontMenu() {
    auto* m = findChild<QMenu*>(QStringLiteral("rcxViewFontMenu"));
    if (!m) return;
    // popup(), not exec(): exec() blocks and the capture never runs.
    m->popup(mapToGlobal(QPoint(120, 90)));
}

void MainWindow::previewSymbolsDock() {
    createSymbolsDock();
    if (!m_symbolsDock) return;
    m_symbolsDock->show();
    m_symbolsDock->raise();
    resizeDocks({m_symbolsDock}, {270}, Qt::Horizontal);
}

void MainWindow::previewCloseViaX() {
    // The --screenshot handler already opened one tab; add two more so there
    // are three (UnnamedClass0/1/2). Then close two of them via the REAL
    // close-X — the path the bug report is about ("close one, the X stops
    // working on the rest"). Verified: both closes land, leaving one tab, so
    // closing a sibling does not break the survivors' X buttons.
    project_new();
    project_new();

    auto clickActiveX = [this]() {
        QDockWidget* active = m_activeDocDock;
        if (!active && !m_docDocks.isEmpty()) active = m_docDocks.last();
        if (!active) return;
        for (auto* tabBar : findChildren<QTabBar*>()) {
            if (tabBar->parent() != this) continue;
            for (int i = 0; i < tabBar->count(); ++i) {
                if (tabBar->tabText(i) == active->windowTitle()) {
                    if (auto* b = qobject_cast<DockTabButtons*>(
                            tabBar->tabButton(i, QTabBar::RightSide)))
                        b->closeBtn->click();
                    return;
                }
            }
        }
    };

    const int startCount = m_docDocks.size();   // 3 (handler's project_new + 2 here)
    clickActiveX();   // close the active (3rd) tab
    // Let the close + deferred reconcile settle, then click a survivor's X.
    QTimer::singleShot(500, this, [this, clickActiveX, startCount]() {
        clickActiveX();   // close a survivor — the click-time-resolution path
        // After the second close + its deferred reconcile settle, emit a
        // machine-checkable result for the `closetest` harness: two X-closes
        // from three tabs must leave exactly one. A stale install-time binding
        // (the original bug) would no-op the survivor's X and leave two.
        QTimer::singleShot(400, this, [this, startCount]() {
            const int remaining = m_docDocks.size();
            std::fprintf(stderr, "CLOSEX_RESULT started=%d remaining=%d expected=%d %s\n",
                         startCount, remaining, startCount - 2,
                         remaining == startCount - 2 ? "PASS" : "FAIL");
            std::fflush(stderr);
        });
    });
}

void MainWindow::undo() {
    auto* tab = activeTab();
    if (tab) tab->doc->undoStack.undo();
}

void MainWindow::redo() {
    auto* tab = activeTab();
    if (tab) tab->doc->undoStack.redo();
}

void MainWindow::showGotoAddressDialog() {
    auto* ctrl = activeController();
    if (!ctrl) {
        setAppStatus(QStringLiteral("Open a project first"));
        return;
    }

    // Build the same callback set the controller uses for navigateToFormula —
    // module resolution, pointer reads, identifier lookup, and kernel paging
    // when the provider supports it.
    AddressParserCallbacks cbs;
    auto* doc = ctrl->document();
    if (ctrl->provider()) {
        auto* prov = ctrl->provider().get();
        cbs.resolveModule = [prov](const QString& name, bool* ok) -> uint64_t {
            uint64_t base = prov->symbolToAddress(name);
            *ok = (base != 0);
            return base;
        };
        int ptrSz = doc->tree.pointerSize;
        cbs.readPointer = [prov, ptrSz](uint64_t addr, bool* ok) -> uint64_t {
            uint64_t val = 0;
            *ok = prov->read(addr, &val, ptrSz);
            return val;
        };
        cbs.resolveIdentifier = [prov](const QString& name, bool* ok) -> uint64_t {
            return SymbolStore::instance().resolve(name, prov, ok);
        };
        if (prov->hasKernelPaging()) {
            cbs.vtop = [prov](uint32_t pid, uint64_t va, bool* ok) -> uint64_t {
                Q_UNUSED(pid);
                auto r = prov->translateAddress(va);
                *ok = r.valid;
                return r.physical;
            };
            cbs.cr3 = [prov](uint32_t pid, bool* ok) -> uint64_t {
                Q_UNUSED(pid);
                uint64_t cr3 = prov->getCr3();
                *ok = (cr3 != 0);
                return cr3;
            };
            cbs.physRead = [prov](uint64_t physAddr, bool* ok) -> uint64_t {
                auto entries = prov->readPageTable(physAddr, 0, 1);
                *ok = !entries.isEmpty();
                return entries.isEmpty() ? 0 : entries[0];
            };
        }
    }

    GotoAddressDialog dlg(cbs, doc->tree.pointerSize, this);
    if (dlg.exec() != QDialog::Accepted) return;

    QString err;
    if (!ctrl->navigateToFormula(dlg.formula(), &err)) {
        ThemedMessageBox::warn(this,
            QStringLiteral("Address Not Resolved"),
            QStringLiteral("Couldn't evaluate \"%1\". %2")
                .arg(dlg.formula(),
                     err.isEmpty() ? QStringLiteral("The expression isn't valid.")
                                   : err));
        return;
    }
    setAppStatus(QStringLiteral("Jumped to 0x%1")
        .arg(dlg.resolvedAddress(), 0, 16));
}

void MainWindow::showCommandPalette() {
    CommandPalette dlg(m_menuBar, this);
    dlg.exec();
}

void MainWindow::showRttiBrowser(uint64_t vtableAddr) {
    auto* ctrl = activeController();
    if (!ctrl || !ctrl->provider()) {
        setAppStatus(QStringLiteral("No active provider for RTTI"));
        return;
    }
    // Compose runs RTTI through the snapshot provider — its page-cache
    // already holds the vtable + type_info + name pages that produced the
    // chip's hint text. The live provider may fail isReadable on the same
    // pages a moment later (process unmapped them, page protection, etc.),
    // so try the snapshot first; only fall back to live if the snapshot is
    // gone. This is what makes the click-through actually match what the
    // chip claims.
    RttiInfo info;
    if (auto* snap = ctrl->snapshotProv()) {
        info = walkRtti(*snap, vtableAddr);
    }
    if (!info.ok) {
        info = walkRtti(*ctrl->provider(), vtableAddr);
    }
    if (!info.ok) {
        ThemedMessageBox::info(this,
            QStringLiteral("No RTTI Here"),
            info.error.isEmpty()
                ? QStringLiteral("No RTTI structures found at 0x%1.")
                      .arg(vtableAddr, 0, 16)
                : info.error);
        return;
    }
    RttiBrowserDialog dlg(info, ctrl->provider().get(), this);
    dlg.exec();
}

void MainWindow::about() {
    ThemedDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("About RC"));
    dlg.setFixedSize(420, 420);
    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(10);

    const auto& t = ThemeManager::instance().current();
    auto* buildLabel = new QLabel(
        QStringLiteral("<span style='color:%1;font-size:11px;'>"
                       "Build&ensp;" __DATE__ "&ensp;" __TIME__ "</span>")
            .arg(t.textDim.name()));
    buildLabel->setAlignment(Qt::AlignCenter);
    lay->addWidget(buildLabel);

    // Acknowledgments — third-party open source projects Reclass ships
    // or links against. Linked names go to upstream; license names sit
    // next to each entry so users can verify compatibility at a glance.
    auto* ackTitle = new QLabel(
        QStringLiteral("<span style='color:%1;font-size:11px;font-weight:600;'>"
                       "Open Source Acknowledgments</span>")
            .arg(t.text.name()));
    ackTitle->setAlignment(Qt::AlignCenter);
    lay->addWidget(ackTitle);

    auto* ack = new QLabel;
    ack->setTextFormat(Qt::RichText);
    ack->setOpenExternalLinks(true);
    ack->setWordWrap(true);
    ack->setText(QStringLiteral(
        "<div style='color:%1;font-size:11px;line-height:140%%;'>"
        "<p>RC uses the following open-source software. Many thanks "
        "to their authors and maintainers.</p>"
        "<ul style='margin-left:14px;padding-left:0;'>"
        "<li><a href='https://www.qt.io/' style='color:%2;'>Qt</a> "
        "— LGPL v3 (dynamically linked)</li>"
        "<li><a href='https://www.riverbankcomputing.com/software/qscintilla/'"
        " style='color:%2;'>QScintilla</a> — GPL v3 / commercial</li>"
        "<li><a href='https://github.com/MicrosoftDocs/cpp-docs' "
        "style='color:%2;'>raw_pdb</a> — BSD 2-Clause "
        "(Microsoft PDB parser fork)</li>"
        "<li><a href='https://github.com/aengelke/fadec' style='color:%2;'>"
        "fadec</a> — BSD 2-Clause (x86 decoder)</li>"
        "<li><a href='https://github.com/JetBrains/JetBrainsMono' "
        "style='color:%2;'>JetBrains Mono</a> — SIL Open Font License 1.1</li>"
        "<li><a href='https://github.com/IBM/plex' style='color:%2;'>"
        "IBM Plex Mono</a> — SIL Open Font License 1.1</li>"
        "</ul>"
        "<p style='color:%3;font-size:10px;'>Full license texts are "
        "redistributed alongside the binaries in the project's "
        "<code>third_party/</code> and <code>src/fonts/</code> "
        "directories.</p>"
        "</div>")
        .arg(t.text.name(), t.borderFocused.name(), t.textDim.name()));
    lay->addWidget(ack);

    lay->addStretch();

    auto* ghBtn = new DialogButton(QStringLiteral("Open GitHub"),
        DialogButton::Primary, &dlg);
    connect(ghBtn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/IChooseYou/RC"));
    });
    lay->addWidget(ghBtn, 0, Qt::AlignCenter);
    dlg.exec();
}

void MainWindow::showShortcutsDialog() {
    // Discoverable home for the editor's keyboard shortcuts. Static content
    // grouped by category. Two-column layout (Key, Description) with
    // section header rows that span both columns.
    const auto& t = ThemeManager::instance().current();

    // ThemedDialog gives us the standard palette + theme-change signal
    // wiring for free, instead of the ad-hoc QPalette setup this dialog
    // used to do. Same approach as the unsaved-changes box and the
    // Type Aliases dialog.
    rcx::ThemedDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Keyboard Shortcuts"));
    dlg.resize(560, 520);

    QSettings settings("RC", "RC");
    QFont monoFont(settings.value("font", "JetBrains Mono").toString(), 10);
    monoFont.setFixedPitch(true);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(8);

    // Filter box — type to narrow the list to matching key OR description.
    // Section headers stay visible whenever any of their rows survive the
    // filter, so the result reads as "the same dialog with fewer rows" not
    // an unrelated list view.
    auto* filterEdit = new QLineEdit(&dlg);
    filterEdit->setPlaceholderText(QStringLiteral("Filter shortcuts…"));
    filterEdit->setClearButtonEnabled(true);
    filterEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3; padding: 4px 6px; }"
        "QLineEdit:focus { border: 1px solid %4; }")
        .arg(t.background.name(), t.text.name(), t.border.name(),
             t.borderFocused.name()));
    lay->addWidget(filterEdit);

    auto* table = new QTableWidget(&dlg);
    table->setColumnCount(2);
    table->horizontalHeader()->setVisible(false);
    table->verticalHeader()->setVisible(false);
    table->setShowGrid(false);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setFocusPolicy(Qt::NoFocus);
    table->setAlternatingRowColors(false);
    table->setFont(monoFont);
    table->setStyleSheet(QStringLiteral(
        "QTableWidget { background: %1; color: %2; border: 1px solid %3; }"
        "QTableWidget::item { padding: 4px 8px; }")
        .arg(t.background.name(), t.text.name(), t.border.name()));

    struct Row { QString key; QString desc; bool header = false; };
    const QVector<Row> rows = {
        {QStringLiteral("Navigation"),         {}, true},
        {QStringLiteral("↑ / ↓"),     QStringLiteral("Previous / next field (↓ at end adds a new field)")},
        {QStringLiteral("PgUp / PgDn"),         QStringLiteral("Jump by visible lines")},
        {QStringLiteral("Home / End"),          QStringLiteral("Line start / line end")},
        {QStringLiteral("Ctrl+F"),              QStringLiteral("Find field by name across open docs")},
        {QStringLiteral("F12"),                 QStringLiteral("Go to definition")},
        {QStringLiteral("Ctrl+Click"),          QStringLiteral("Open type in new tab")},

        {QStringLiteral("Editing"),            {}, true},
        {QStringLiteral("Enter"),               QStringLiteral("Edit value at caret")},
        {QStringLiteral("Tab"),                 QStringLiteral("Cycle edit targets (name → value → comment → type)")},
        {QStringLiteral("F2"),                  QStringLiteral("Rename")},
        {QStringLiteral("Space"),               QStringLiteral("Cycle hex size")},
        {QStringLiteral(";"),                   QStringLiteral("Edit comment")},
        {QStringLiteral("Ctrl+Shift+↑/↓"), QStringLiteral("Reorder field")},
        {QStringLiteral("Ctrl+D"),              QStringLiteral("Duplicate node")},
        {QStringLiteral("Delete"),              QStringLiteral("Delete selected node(s)")},
        {QStringLiteral("Insert / Shift+Ins"),  QStringLiteral("Insert hex64 / hex32 above")},

        {QStringLiteral("Type changes"),       {}, true},
        {QStringLiteral("1 – 5"),          QStringLiteral("Hex8 / Hex16 / Hex32 / Hex64 / Hex128")},
        {QStringLiteral("P"),                   QStringLiteral("Pointer")},
        {QStringLiteral("F"),                   QStringLiteral("Float / Double (size-aware)")},
        {QStringLiteral("S"),                   QStringLiteral("Signed int (size-aware)")},
        {QStringLiteral("U"),                   QStringLiteral("Unsigned int (size-aware)")},
        {QStringLiteral("← / →"),     QStringLiteral("Cycle through same-size type variants")},
        {QStringLiteral("T"),                   QStringLiteral("Open type picker")},

        {QStringLiteral("Byte selection"),     {}, true},
        {QStringLiteral("Drag on hex bytes"),    QStringLiteral("Select a range of bytes (8 px threshold upgrades from row-drag)")},
        {QStringLiteral("Shift+Click on hex byte"), QStringLiteral("Extend existing byte selection to clicked byte")},
        {QStringLiteral("Ctrl+A on hex row"),    QStringLiteral("Select all hex bytes in document")},
        {QStringLiteral("Shift+← / →"),       QStringLiteral("Extend selection by one byte (clamps at doc end)")},
        {QStringLiteral("Shift+↓ / ↑"),         QStringLiteral("Snap selection to next / previous hex row boundary")},
        {QStringLiteral("Shift+End / Home"),     QStringLiteral("Extend to last hex byte / collapse to anchor")},
        {QStringLiteral("Ctrl+C"),               QStringLiteral("Copy as hex string (e.g. \"DE AD BE EF\")")},
        {QStringLiteral("Ctrl+Shift+C"),         QStringLiteral("Copy selection range (\"0xLO..0xHI (N bytes)\")")},
        {QStringLiteral("Right-click"),          QStringLiteral("Copy as C array / Python bytes / save as file / paste / zero-fill / edit")},
        {QStringLiteral("Ctrl+V"),               QStringLiteral("Paste hex (parses \"DE AD\", \"0xDEAD\", \"{0xDE,0xAD}\")")},
        {QStringLiteral("Enter"),                QStringLiteral("Edit selected bytes in hex-overwrite mode")},
        {QStringLiteral("Delete / Backspace"),   QStringLiteral("Zero-fill selection")},
        {QStringLiteral("Esc"),                  QStringLiteral("Clear byte selection (first Esc), then node selection")},

        {QStringLiteral("Timeline"),           {}, true},
        {QStringLiteral("F9"),                  QStringLiteral("Record / stop recording this class")},
        {QStringLiteral("Ctrl+, / Ctrl+."),     QStringLiteral("Previous / next recorded change")},
        {QStringLiteral("Ctrl+End"),            QStringLiteral("Back to live (leave the recording)")},
        {QStringLiteral("Drag on the timeline"), QStringLiteral("Look back through the recording (Shift: don't snap to changes)")},
        {QStringLiteral("Wheel / Shift+Wheel"), QStringLiteral("Zoom / pan the timeline")},

        {QStringLiteral("Bookmarks & Window"), {}, true},
        {QStringLiteral("Ctrl+B"),              QStringLiteral("Add bookmark...")},
        {QStringLiteral("Ctrl+Alt+B"),          QStringLiteral("Quick bookmark here")},
        {QStringLiteral("Ctrl+\\"),             QStringLiteral("Split view below (RC / Code side-by-side)")},
        {QStringLiteral("Ctrl+Shift+\\"),       QStringLiteral("Unsplit view")},
        {QStringLiteral("Ctrl+Shift+[ / ]"),    QStringLiteral("Collapse all / expand all")},
        {QStringLiteral("F5"),                  QStringLiteral("Refresh")},
        {QStringLiteral("Ctrl+G"),              QStringLiteral("Go to offset...")},
        {QStringLiteral("Ctrl+K"),              QStringLiteral("Command palette")},
        {QStringLiteral("Ctrl+Shift+V"),        QStringLiteral("Validate project (find sibling overlaps)")},
        {QStringLiteral("Ctrl+Shift+P"),        QStringLiteral("Presentation mode")},
        {QStringLiteral("F1"),                  QStringLiteral("This dialog")},
    };

    table->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); i++) {
        const auto& r = rows[i];
        if (r.header) {
            auto* item = new QTableWidgetItem(r.key);
            QFont hf = monoFont;
            hf.setBold(true);
            item->setFont(hf);
            item->setForeground(QBrush(t.indHoverSpan));
            item->setBackground(QBrush(t.backgroundAlt));
            table->setItem(i, 0, item);
            table->setSpan(i, 0, 1, 2);
            table->setRowHeight(i, 28);
        } else {
            auto* keyItem = new QTableWidgetItem(r.key);
            keyItem->setForeground(QBrush(t.text));
            keyItem->setFont(monoFont);
            auto* descItem = new QTableWidgetItem(r.desc);
            descItem->setForeground(QBrush(t.textDim));
            table->setItem(i, 0, keyItem);
            table->setItem(i, 1, descItem);
            table->setRowHeight(i, 22);
        }
    }
    table->setColumnWidth(0, 200);
    table->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(table);

    // Live filter — hide any non-header row that doesn't substring-match
    // (case-insensitive) the query in either column. Headers stay hidden
    // until at least one body row beneath them is visible.
    auto applyFilter = [table, &rows](const QString& q) {
        QString needle = q.trimmed().toLower();
        if (needle.isEmpty()) {
            for (int i = 0; i < rows.size(); ++i) table->setRowHidden(i, false);
            return;
        }
        QVector<bool> visible(rows.size(), false);
        for (int i = 0; i < rows.size(); ++i) {
            if (rows[i].header) continue;
            bool m = rows[i].key.toLower().contains(needle)
                  || rows[i].desc.toLower().contains(needle);
            visible[i] = m;
        }
        // Walk through and reveal headers whose section has any visible
        // body row. Section ends at the next header or the table end.
        int curHeader = -1;
        bool sectionHasMatch = false;
        for (int i = 0; i < rows.size(); ++i) {
            if (rows[i].header) {
                if (curHeader >= 0) visible[curHeader] = sectionHasMatch;
                curHeader = i;
                sectionHasMatch = false;
            } else if (visible[i]) {
                sectionHasMatch = true;
            }
        }
        if (curHeader >= 0) visible[curHeader] = sectionHasMatch;
        for (int i = 0; i < rows.size(); ++i)
            table->setRowHidden(i, !visible[i]);
    };
    QObject::connect(filterEdit, &QLineEdit::textChanged, &dlg, applyFilter);

    auto* closeBtn = new rcx::DialogButton(QStringLiteral("Close"),
        rcx::DialogButton::Primary, &dlg);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    lay->addWidget(closeBtn, 0, Qt::AlignRight);

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
        setAppStatus("MCP server listening on pipe: RCMcpBridge");
    }
}

void MainWindow::applyTheme(const Theme& theme) {
    applyGlobalTheme(theme);

    // Empty-workspace hatch repaints itself from the new theme colours.
    if (m_centralPlaceholder) m_centralPlaceholder->update();

    // (A theme's `font` field is applied by the themeChanged connection in
    // the constructor, not here: here it would also fire on the initial
    // apply and on the re-apply setEditorFont ends with, which is what made
    // View > Font self-undoing.)

#ifdef __APPLE__
    applyMacTitleBarTheme(this, theme);
#endif

    // Dock separator is 1px via PM_DockWidgetSeparatorExtent in MenuBarStyle

    // Start page
    if (m_startPage)
        m_startPage->applyTheme(theme);

    // Update border overlay color
    updateBorderColor(isActiveWindow() ? theme.borderFocused : theme.border);

    // Propagate theme to the drag-overlay so its drop-zone chrome isn't
    // stuck with hard-coded dark-theme colours.
    if (m_dockOverlay) {
        m_dockOverlay->setTheme(theme);
        m_dockOverlay->setAccentColor(theme.indHoverSpan);
    }

    // Style doc dock tab bars and remove dock borders.
    // QWidget default colors are required because having ANY stylesheet on QMainWindow
    // switches children from palette-based to CSS-based rendering.
    setStyleSheet(QStringLiteral(
        "QMainWindow::separator { width: 4px; height: 4px; background: %1; }"
        "QMainWindow::separator:hover { background: %2; }"
        "QDockWidget { border: none; margin: 0px; padding: 0px; }"
        "QDockWidget > QWidget { border: none; margin: 0px; padding: 0px; }"
        // The ribbon host is a bare layout slot: no Fusion toolbar chrome.
        "QToolBar#RibbonHost { border: none; margin: 0px; padding: 0px; spacing: 0px; background: transparent; }")
        .arg(theme.background.name(), theme.hover.name()));

    // Custom title bar — applied AFTER setStyleSheet() because the MainWindow
    // stylesheet re-resolves descendant palettes and would reset the QMenuBar palette.
    if (m_titleBar)
        m_titleBar->applyTheme(theme);
    if (m_ribbon)
        m_ribbon->applyTheme(theme);

    for (auto* tabBar : findChildren<QTabBar*>()) {
        // Only style tab bars owned directly by this QMainWindow (dock tabs),
        // skip ones inside SplitPane QTabWidgets etc.
        if (tabBar->parent() == this) {
            // No stylesheet — painting handled by MenuBarStyle (CE_TabBarTabShape/Label)
            tabBar->setStyleSheet(QString());
            tabBar->setAttribute(Qt::WA_Hover, true);
            tabBar->setElideMode(Qt::ElideNone);
            tabBar->setExpanding(false);
            // Set editor font so tab width sizing matches our label painting
            {
                QSettings s("RC", "RC");
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
                if (btns) btns->applyTheme(theme.text, theme.selected);
            }
            // Update scroll arrow styling
            for (auto* btn : tabBar->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
                if (btn->icon().isNull()) continue;  // skip non-arrow buttons
                btn->setStyleSheet(QStringLiteral(
                    "QToolButton { background: %1; border: 1px solid %2; padding: 2px; }"
                    "QToolButton:hover { background: %3; }")
                    .arg(theme.background.name(), theme.border.name(), theme.hover.name()));
            }
        }
    }

    // Restyle per-pane view tab bars (Structure / Code / Debug / Both).
    // The literal that used to live here disagreed with the creation-time one
    // — chrome background instead of paper, a filled selected tab and a 3px
    // TOP accent — so the strip changed grammar on every theme switch. Both
    // sites call rcx::paneTabStyle now.
    {
        QString editorFont = QSettings("RC", "RC").value("font", "JetBrains Mono").toString();
        const QString tabSheet = rcx::paneTabStyle(theme, editorFont);
        QString comboStyle = QStringLiteral(
            "QComboBox { background: %1; color: %2; border: 1px solid %3;"
            " padding: 1px 6px; font-family: '%6'; font-size: 9pt; }"
            "QComboBox:focus { border-color: %7; }"
            "QComboBox::drop-down { border: none; width: 14px; }"
            "QComboBox::down-arrow { image: url(:/vsicons/chevron-down.svg);"
            " width: 10px; height: 10px; }"
            "QComboBox QAbstractItemView { background: %4; color: %2;"
            " selection-background-color: %5; border: 1px solid %3; }")
            .arg(theme.background.name(), theme.textMuted.name(), theme.border.name(),
                 theme.backgroundAlt.name(), theme.hover.name(), editorFont,
                 theme.borderFocused.name());
        QString gearStyle = QStringLiteral(
            "QToolButton { background: %1; color: %2; border: 1px solid %3; border-radius: 0px; }"
            "QToolButton:hover { background: %4; }")
            .arg(theme.background.name(), theme.textMuted.name(), theme.border.name(),
                 theme.hover.name());
        for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
            for (auto& pane : it->panes) {
                if (pane.tabWidget)
                    pane.tabWidget->setStyleSheet(tabSheet);
                if (pane.fmtCombo)
                    pane.fmtCombo->setStyleSheet(comboStyle);
                if (pane.scopeCombo)
                    pane.scopeCombo->setStyleSheet(comboStyle);
                if (pane.fmtGear)
                    pane.fmtGear->setStyleSheet(gearStyle);
                // Refresh the dynamic property the custom
                // EditorContainer::paintEvent reads, then trigger a repaint.
                // Do NOT install a QSS border here — Qt would draw it as a
                // Fusion bevel (2-pixel light/dark double line) on top of the
                // custom paint. Both the hex editor and the Code view use it.
                for (QWidget* ec : { pane.editorContainer, pane.renderedContainer }) {
                    if (ec) {
                        ec->setProperty("borderColor", containerBorderColor(theme));
                        ec->update();
                    }
                }
                // The minimap was absent from this path entirely, so its
                // paper/text stayed on the theme that was active when the
                // pane was built until the tab was reopened.
                if (pane.minimap) {
                    styleMinimap(pane.minimap, theme);
                    pane.minimap->update();
                }
            }
        }
    }

    // Status bar
    {
        QPalette sbPal = statusBar()->palette();
        sbPal.setColor(QPalette::Window, theme.background);
        sbPal.setColor(QPalette::WindowText, theme.textDim);
        statusBar()->setPalette(sbPal);
        m_statusLabel->colBase   = theme.textDim;
        m_statusLabel->colDim    = theme.textMuted;
        m_statusLabel->colBright = theme.indHoverSpan;
        m_statusLabel->colSep    = theme.border;
        m_statusLabel->colHover  = theme.text;
    }
    // Status bar chrome
    {
        auto* fsb = static_cast<FlatStatusBar*>(statusBar());
        fsb->setTopLineColor(rcx::containerBorderColor(theme));
        fsb->setDividerColor(theme.border);
    }
    // Resize grip (direct child of main window, not in status bar)
    if (auto* w = findChild<QWidget*>("resizeGrip"))
        static_cast<ResizeGrip*>(w)->setGripColor(theme.textFaint);

    // Workspace tree: delegate colors, palette, stylesheet
    if (m_workspaceDelegate)
        m_workspaceDelegate->setThemeColors(theme);
    // Match the editor body's "paper" colour (a touch darker than chrome) so
    // the Project workspace reads as the SAME surface as the editor, not a
    // jarringly lighter panel. Single source of truth: rcx::editorPaperColor.
    const QColor wsPaper = rcx::editorPaperColor(theme);
    if (auto* cont = m_workspaceDock
            ? m_workspaceDock->findChild<WorkspacePanel*>("workspaceContainer") : nullptr)
        cont->setBackground(wsPaper);
    if (m_workspaceTree) {
        QPalette tp = m_workspaceTree->palette();
        tp.setColor(QPalette::Text, theme.textDim);
        tp.setColor(QPalette::Highlight, theme.selected);
        tp.setColor(QPalette::HighlightedText, theme.text);
        tp.setColor(QPalette::Base, wsPaper);   // item-row bg → editor paper
        m_workspaceTree->setPalette(tp);
        m_workspaceTree->setStyleSheet(QStringLiteral(
            "QTreeView { background: %1; border: none; padding-left: 4px; }"
            "QTreeView::branch:has-children:closed { image: url(:/vsicons/chevron-right.svg); }"
            "QTreeView::branch:has-children:open { image: url(:/vsicons/chevron-down.svg); }"
            "QTreeView::branch { width: 12px; }"
            "QAbstractScrollArea::corner { background: %1; border: none; }"
            "QHeaderView { background: %1; border: none; }"
            "QHeaderView::section { background: %1; border: none; }")
            .arg(wsPaper.name()));
        m_workspaceTree->viewport()->update();
    }
    if (m_workspaceSearch) m_workspaceSearch->applyTheme(theme);

    // Bookmarks panel shares the editor-paper surface (same as the workspace).
    themeBookmarksContent();

    // Dock headers: one shared control, so one call each. (This block used to
    // be ~55 lines of four divergent hand-rolled title bars — including two
    // U+2715 text-glyph close buttons that turned indHoverSpan on hover, i.e.
    // purple spent on an ordinary close affordance.)
    for (auto* h : {m_workspaceHeader, m_scanDockHeader, m_symDockHeader, m_bmDockHeader})
        if (h) h->applyTheme(theme);
    updateWorkspaceDockEdge();  // re-arm the docked-only right hairline in the new theme

    // Scanner dock
    if (m_scannerPanel)
        m_scannerPanel->applyTheme(theme);

    // Symbols dock
    if (m_unifiedSymbols)
        m_unifiedSymbols->applyTheme(theme);

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

    // Rendered Code views: re-theme the per-language lexer, paper, margins.
    for (auto& tab : m_tabs) {
        for (auto& pane : tab.panes) {
            auto* sci = pane.rendered;
            if (!sci) continue;
            // Re-theme whatever lexer the current output format uses. The
            // format the user last viewed governs which lexer is attached;
            // re-apply for it so a theme switch recolors all languages.
            const CodeFormat fmt = static_cast<CodeFormat>(
                QSettings("RC", "RC").value("codeFormat", 0).toInt());
            applyCodeLexer(sci, fmt, theme, sci->font());
            sci->setPaper(rcx::editorPaperColor(theme));
            sci->setColor(theme.text);
            sci->setCaretForegroundColor(theme.text);
            sci->setCaretLineBackgroundColor(theme.hover);
            sci->setSelectionBackgroundColor(theme.selection);
            sci->setSelectionForegroundColor(theme.text);
            sci->setMarginsBackgroundColor(rcx::editorPaperColor(theme));
            sci->setMarginsForegroundColor(theme.textDim);

            // Debug view — update paper/caret colors and restyle
            if (pane.debugView) {
                const QColor dbg = rcx::editorPaperColor(theme);
                pane.debugView->setPaper(dbg);
                pane.debugView->setColor(theme.text);
                pane.debugView->setCaretForegroundColor(theme.text);
                pane.debugView->setCaretLineBackgroundColor(theme.hover);
                pane.debugView->setSelectionBackgroundColor(theme.selection);
                pane.debugView->setSelectionForegroundColor(theme.text);
                pane.debugView->setMarginsBackgroundColor(dbg);
                pane.debugView->setMarginsForegroundColor(theme.textDim);
                applyDebugStyles(pane.debugView);
                if (pane.viewMode == VM_Debug)
                    updateDebugView(tab, pane);
            }
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

void MainWindow::showOptionsDialog() { showOptionsDialog(-1); }

void MainWindow::loadPluginsDeferred() {
    PROFILE_SCOPE("MainWindow::loadPluginsDeferred");
    if (m_pluginsLoaded) return;
    m_pluginsLoaded = true;
    {
        PROFILE_SCOPE("PluginManager::LoadPlugins");
        m_pluginManager.LoadPlugins();
    }
    // Surface ABI-rejected provider plugins (otherwise the reason is stderr-only).
    if (int n = m_pluginManager.rejectedPlugins().size())
        setAppStatus(QStringLiteral(
            "%1 plugin(s) skipped (incompatible) — see Plugins ▸ Manage Plugins").arg(n));
    if (m_mcp && QSettings("RC", "RC").value("autoStartMcp", true).toBool())
        m_mcp->start();
}

void MainWindow::showProfilerDialog() {
    // Lightweight RAII timer aggregator. Dialog auto-enables profiling
    // on first open so the user gets samples immediately.
    static QPointer<rcx::ProfilerDialog> s_dlg;
    if (!s_dlg) s_dlg = new rcx::ProfilerDialog(this);
    rcx::Profiler::instance().setEnabled(true);
    s_dlg->show();
    s_dlg->raise();
    s_dlg->activateWindow();
}

void MainWindow::showOptionsDialog(int initialPage) {
    auto& tm = ThemeManager::instance();
    OptionsResult current;
    current.themeIndex = tm.currentIndex();
    current.fontName = QSettings("RC", "RC").value("font", "JetBrains Mono").toString();
    current.menuBarTitleCase = m_menuBarTitleCase;
    current.showIcon = m_titleBar
        ? QSettings("RC", "RC").value("showIcon", false).toBool()
        : false;
    current.autoStartMcp = QSettings("RC", "RC").value("autoStartMcp", true).toBool();
    current.refreshMs = QSettings("RC", "RC").value("refreshMs", rcx::kDefaultRefreshMs).toInt();
    current.generatorAsserts = QSettings("RC", "RC").value("generatorAsserts", false).toBool();
    current.braceWrap = QSettings("RC", "RC").value("braceWrap", false).toBool();
    current.floatDecimals = rcx::fmt::floatDecimals();

    OptionsDialog dlg(current, this);
    if (initialPage >= 0)
        dlg.selectPage(initialPage);
    if (dlg.exec() != QDialog::Accepted) return;

    auto r = dlg.result();

    if (r.themeIndex != current.themeIndex)
        tm.setCurrent(r.themeIndex);

    if (r.fontName != current.fontName)
        setEditorFont(r.fontName);

    if (r.menuBarTitleCase != current.menuBarTitleCase) {
        applyMenuBarTitleCase(r.menuBarTitleCase);
        QSettings("RC", "RC").setValue("menuBarTitleCase", r.menuBarTitleCase);
    }

    if (r.showIcon != current.showIcon) {
        if (m_titleBar)
            m_titleBar->setShowIcon(r.showIcon);
        QSettings("RC", "RC").setValue("showIcon", r.showIcon);
    }

    if (r.autoStartMcp != current.autoStartMcp)
        QSettings("RC", "RC").setValue("autoStartMcp", r.autoStartMcp);

    if (r.refreshMs != current.refreshMs) {
        QSettings("RC", "RC").setValue("refreshMs", r.refreshMs);
        for (auto& tab : m_tabs)
            tab.ctrl->setRefreshInterval(r.refreshMs);
    }

    if (r.generatorAsserts != current.generatorAsserts)
        QSettings("RC", "RC").setValue("generatorAsserts", r.generatorAsserts);

    if (r.braceWrap != current.braceWrap) {
        QSettings("RC", "RC").setValue("braceWrap", r.braceWrap);
        for (auto& tab : m_tabs)
            tab.ctrl->setBraceWrap(r.braceWrap);
    }

    if (r.floatDecimals != current.floatDecimals) {
        QSettings("RC", "RC").setValue("floatDecimals", r.floatDecimals);
        rcx::fmt::setFloatDecimals(r.floatDecimals);
        for (auto& tab : m_tabs)
            if (tab.ctrl) tab.ctrl->refresh();
    }
}

void MainWindow::setEditorFont(const QString& fontName) {
    QSettings settings("RC", "RC");
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
            // Update debug view font
            if (pane.debugView) {
                pane.debugView->setFont(f);
                pane.debugView->setMarginsFont(f);
            }
        }
    }
    // Sync workspace tree, title, search, and status bar font (10pt monospace)
    {
        QFont wf(fontName, 10);
        wf.setFixedPitch(true);
        if (m_workspaceTree)
            m_workspaceTree->setFont(wf);
        // Both shared panel filter boxes; PanelSearchField re-reads the same
        // face from QSettings on applyTheme, this just keeps them in step now.
        if (m_workspaceSearch)
            m_workspaceSearch->setFont(wf);
        if (m_bookmarksFilter)
            m_bookmarksFilter->setFont(wf);
        if (m_statusLabel) {
            m_statusLabel->setFont(wf);
            auto* fsb = static_cast<FlatStatusBar*>(statusBar());
            fsb->setMinimumHeight(QFontMetrics(wf).height() + 6);
        }
    }
    // Sync scanner panel font
    if (m_scannerPanel)
        m_scannerPanel->setEditorFont(f);
    // Dock TITLES are chrome, not document text — they follow the face but
    // never the 12-pt editor size (the scanner title used to, which is why it
    // sat two sizes above every other dock title).
    for (auto* h : {m_workspaceHeader, m_scanDockHeader, m_symDockHeader, m_bmDockHeader})
        if (h) h->applyTheme(ThemeManager::instance().current());
    if (m_unifiedSymbols)
        m_unifiedSymbols->applyTheme(ThemeManager::instance().current());
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
        // Pane tab bars (Structure / Code) — re-apply stylesheet with new font
        // (stylesheet overrides setFont, so font must be in the CSS)
        applyTheme(ThemeManager::instance().current());
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

void MainWindow::updateEmptyWorkspaceVisibility() {
    // Central widget is pinned to 0 height so the doc-dock area fills the
    // window; with no document tab open we lift that cap so the WorkspaceHatch
    // fills the empty band and signals "no document open".
    if (m_centralPlaceholder)
        m_centralPlaceholder->setMaximumHeight(
            m_tabs.isEmpty() ? QWIDGETSIZE_MAX : 0);
}

void MainWindow::updateWindowTitle() {
    syncRibbonController();
    syncTimelineActions();
#ifdef __APPLE__
    setWindowTitle(QStringLiteral("RC"));
#else
    QString title;
    QDockWidget* activeDock = m_activeDocDock;
    if (activeDock && m_tabs.contains(activeDock)) {
        auto& tab = m_tabs[activeDock];
        QString name = rootName(tab.doc->tree, tab.ctrl->viewRootId());
        if (tab.doc->modified) name += " *";
        title = name + " - RC";
    } else {
        title = "RC";
    }
    setWindowTitle(title);
#endif
    // Keep the scanner's title bar in sync with the editor's active tab so
    // the user can see which source the next scan will run against.
    updateScannerTitle();
    updateSourceChip();
}

// ── Ribbon ──────────────────────────────────────────────────────────────
// ReClassEx-style Home | Modify strip. The RibbonBar is one custom-painted
// widget; the QToolBar host exists only to claim the QMainWindow layout slot
// between the title/menu bar and the doc tabs (MenuBarStyle + the QSS rule in
// applyTheme strip its Fusion chrome). Every button is a QAction: the Modify
// tab + Undo/Redo come from RibbonActions (which resolves the active
// controller at trigger time and tracks its signals for enabled-state), the
// Home tab reuses the menu actions captured in createMenus().
void MainWindow::createRibbon() {
    QSettings s("RC", "RC");

    m_ribbon = new RibbonBar(this);
    m_ribbon->setLabelMode(RibbonBar::LabelMode(
        s.value("ribbonLabels", int(RibbonBar::LabelMode::Auto)).toInt()));
    // Home is the first-run tab (it is what a new user needs); a persisted
    // choice still wins.
    m_ribbon->setCurrentTab(s.value("ribbonTab", QStringLiteral("home")).toString());
    connect(m_ribbon, &RibbonBar::currentTabChanged, this, [](const QString& id) {
        QSettings("RC", "RC").setValue("ribbonTab", id);
    });
    // The chevron and the tab double-click go through the SAME helper as the
    // menu, so all three write the one `ribbonState` key.
    connect(m_ribbon, &RibbonBar::minimizedChanged, this, [this](bool on) {
        if (m_ribbonState == rcx::RibbonHidden) return;
        applyRibbonState(on ? int(rcx::RibbonCollapsed) : int(rcx::RibbonFull));
    });

    m_ribbonHost = new QToolBar(QStringLiteral("Ribbon"), this);
    m_ribbonHost->setObjectName(QStringLiteral("RibbonHost"));
    m_ribbonHost->setMovable(false);
    m_ribbonHost->setFloatable(false);
    m_ribbonHost->setAllowedAreas(Qt::TopToolBarArea);
    m_ribbonHost->setFocusPolicy(Qt::NoFocus);
    m_ribbonHost->addWidget(m_ribbon);
    addToolBar(Qt::TopToolBarArea, m_ribbonHost);
    // Right-click anywhere on the strip = the View > Ribbon submenu. The host
    // used to swallow the click entirely (PreventContextMenu) to keep Qt's
    // stock toolbar menu away; routing it to our own submenu is what the user
    // expects from a ribbon.
    for (QWidget* w : {static_cast<QWidget*>(m_ribbonHost), static_cast<QWidget*>(m_ribbon)}) {
        w->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(w, &QWidget::customContextMenuRequested, this, [this, w](const QPoint& pos) {
            if (m_ribbonMenu) m_ribbonMenu->popup(w->mapToGlobal(pos));
        });
    }
    // Qt's default createPopupMenu() would list the host's own toggle action
    // (bypassing View > Ribbon and the persisted state) - keep it out.
    m_ribbonHost->toggleViewAction()->setVisible(false);
    applyRibbonState(rcx::ribbonStateFromSettings(s), /*persist=*/false);
#ifdef __APPLE__
    setUnifiedTitleAndToolBarOnMac(false);   // would swallow the ribbon into the native title bar
#endif

    m_ribbonActions = new RibbonActions(
        [this]() -> RcxController* { return activeController(); },
        [this]() -> RcxEditor* { return activePaneEditor(); },
        this);
    connect(m_ribbonActions, &RibbonActions::statusHint, this,
            [this](const QString& text) { setAppStatus(text); });
    for (const QString& id : m_ribbonActions->ids())
        m_ribbon->setAction(id, m_ribbonActions->action(id));

    // Home tab: same QAction objects as the menus. A missing action (e.g. the
    // console toggle off Windows) hides its button.
    auto bind = [this](const char* id, QAction* act) {
        if (act) m_ribbon->setAction(QLatin1String(id), act);
        else if (QAction* own = m_ribbon->action(QLatin1String(id))) own->setVisible(false);
    };
    bind("home.class.newclass",   m_actNewClass);
    bind("home.class.newstruct",  m_actNewStruct);
    bind("home.class.newenum",    m_actNewEnum);
    bind("home.file.open",        m_actOpen);
    bind("home.file.save",        m_actSave);
    bind("home.file.close",       m_actClose);
    bind("home.source.refresh",   m_actRefresh);
    bind("home.source.goto",      m_actGoto);
    bind("home.panels.split",     m_actSplit);
    bind("home.panels.scanner",   m_actScanner);
    bind("home.panels.symbols",   m_actSymbols);
    bind("home.panels.bookmarks", m_actBookmarks);
    bind("home.panels.console",   m_actConsole);

    // Drop-downs anchored under their ▾ buttons: Source -> Data Source menu,
    // Export -> Export menu.
    auto popupUnder = [this](const char* id, QMenu* menu) {
        if (!menu) return;
        const QRect r = m_ribbon->itemRect(QLatin1String(id));
        const QPoint at = r.isNull() ? QCursor::pos()
                                     : m_ribbon->mapToGlobal(r.bottomLeft() + QPoint(0, 1));
        menu->popup(at);
    };
    // Source opens the same chooser the address bar's chip opens (one source
    // picker, not a menu here and a popup there); the plain Data Source menu
    // is the fallback when no document pane exists to host the popup.
    if (QAction* a = m_ribbon->action(QStringLiteral("home.source.attach")))
        connect(a, &QAction::triggered, this, [this, popupUnder]() {
            auto* ctrl = activeController();
            auto* ed = activePaneEditor();
            if (!ctrl || !ed) { popupUnder("home.source.attach", m_sourceMenu); return; }
            const QRect r = m_ribbon->itemRect(QStringLiteral("home.source.attach"));
            const QPoint at = r.isNull() ? QCursor::pos()
                                         : m_ribbon->mapToGlobal(r.bottomLeft() + QPoint(0, 1));
            ctrl->openSourceChooser(ed, at);
        });
    // "Code" does NOT open a save dialog (File ▸ Export still does that). It
    // switches the active pane to its Code view and picks the scope, which is
    // what people actually reach for: read the generated struct, for this class
    // or for the whole project.
    if (QAction* a = m_ribbon->action(QStringLiteral("home.file.code"))) {
        if (!m_codeViewMenu) {
            m_codeViewMenu = new QMenu(this);
            // Labels come from codeScopeName so the menu and the pane's scope
            // combo can never drift apart.
            for (CodeScope sc : {CodeScope::WithChildren, CodeScope::FullSdk}) {
                m_codeViewMenu->addAction(QString::fromLatin1(codeScopeName(sc)), this,
                                          [this, sc]() { showCodeView(sc); });
            }
        }
        connect(a, &QAction::triggered, this,
                [this, popupUnder]() { popupUnder("home.file.code", m_codeViewMenu); });
    }

    // RTTI moved to Modify > Selection (it acts on the selected pointer, not
    // on the project). RibbonActions owns the id so it gets an enabled
    // predicate; the op stays on the Tools menu action.
    connect(m_ribbonActions, &RibbonActions::rttiRequested, this,
            [this]() { if (m_actRtti) m_actRtti->trigger(); });

    // Undo / Redo live in the title strip now — same actions, no ribbon panel.
    if (m_titleBar) {
        m_titleBar->setQuickActions(m_ribbonActions->action(QStringLiteral("edit.undo")),
                                    m_ribbonActions->action(QStringLiteral("edit.redo")));
        connect(m_titleBar, &TitleBarWidget::darkThemeRequested, this, [this](bool dark) {
            auto& tm = ThemeManager::instance();
            tm.setDarkMode(dark);
            m_titleBar->setDarkTheme(tm.current().background.lightnessF() < 0.5);
        });
    }

    syncRibbonController();
}

// ── Class timeline ──

// How often a tab's strip is handed fresh state. Records land every capture
// tick (~200 ms), so a once-a-second push delivered five of them at once and
// the graph lurched; while a recording runs it is fed at capture rate instead.
// Idle (stopped, static, or looking back) nothing new arrives, so the slow
// beat is enough to keep durations honest.
static constexpr int kTimelinePushLiveMs = 125;
static constexpr int kTimelinePushIdleMs = 1000;

// What a tab's timeline strip shows, read from its controller.
static rcx::TimelineStripState timelineStripStateFor(rcx::RcxController* ctrl) {
    using namespace rcx;
    TimelineStripState s;
    if (!ctrl) return s;
    s.nowMs = tl::CaptureClock::nowMs();
    s.epochAtZeroMs = tl::CaptureClock::toEpochMs(0);
    s.utcOffsetSeconds = QDateTime::currentDateTime().offsetFromUtc();
    s.readsFailing = ctrl->sourceStatus() == RcxController::SourceStatus::Stale;
    tl::CaptureContext* ctx = ctrl->timelineContext();
    tl::TimelineHub* hub = ctrl->timelineHub();
    if (!ctx || !hub) {
        s.capture = timeline::Capture::Static;
        return s;
    }
    const uint64_t cls = ctrl->timelineClassId();
    const tl::ClassTrack& track = hub->track(cls);
    switch (track.mode) {
    case tl::CaptureMode::Rolling:   s.capture = timeline::Capture::Rolling;   break;
    case tl::CaptureMode::Recording: s.capture = timeline::Capture::Recording; break;
    case tl::CaptureMode::Paused:    s.capture = timeline::Capture::Paused;    break;
    }
    const tl::TimelineModel& m = ctx->model();
    // This class's own recording — not whatever else the source's history
    // holds (another class, another tab), and nothing from before a Clear.
    s.hasData = hub->firstVisibleRecord(cls) != tl::kNoRecord;
    // Not recording, the timeline does not move: its "now" stays where this
    // class's recording stopped, however much other tabs go on recording.
    if (track.mode != tl::CaptureMode::Recording && s.hasData) {
        const int64_t end = (!track.pauses.isEmpty() && track.pauses.last().isOpen())
            ? track.pauses.last().startMs : m.timeOf(m.lastRecord());
        s.nowMs = qMin(s.nowMs, end);
    }
    s.dataGeneration = m.generation() * 1000003u + ctrl->timelineClassChangesVersion();
    for (const tl::FieldSpan& f : ctrl->timelineSelectionScope())
        s.selectionKey = s.selectionKey * 1000003u + f.id + 1;
    const tl::RecordId first = hub->firstVisibleRecord(cls);
    s.retainedBeginMs = (first != tl::kNoRecord) ? m.timeOf(first) : s.nowMs;
    if (track.floorMs != INT64_MIN) s.retainedBeginMs = qMax(s.retainedBeginMs, track.floorMs);
    s.recordBeginMs = track.recordingSinceMs;
    s.past = ctrl->isViewingPast();
    s.viewedMs = ctrl->pastTimeMs();
    const tl::RetentionPolicy policy = hub->retention();
    s.rollingWindowMs = policy.windowMs;
    s.byteBudget = policy.budgetBytes;
    s.bytesUsed = m.stats().totalBytes() + m.stats().diskBytes;
    for (const tl::Gap& g : m.gaps()) s.gaps.append({g.startMs, g.endMs});
    for (const tl::Gap& g : track.pauses) s.gaps.append({g.startMs, g.endMs});
    for (const tl::TimelineEvent& e : hub->events()) {
        if (e.classId != 0 && e.classId != cls) continue;
        s.markers.append({e.timeMs, TimelineMarkerKind(int(e.kind)), e.label});
    }
    return s;
}

void MainWindow::bindTimelineStrip(TabState& tab) {
    TimelineStrip* strip = tab.timeline;
    RcxController* ctrl = tab.ctrl;
    if (!strip || !ctrl) return;
    QPointer<RcxController> c(ctrl);

    TimelineStrip::Callbacks cb;
    cb.onClear        = [this, c]() { if (c) confirmTimelineClear(c); };
    cb.onReturnToLive = [c]() { if (c) c->returnToLive(); };
    cb.onHide         = [this]() { setTimelineStripVisible(false); };
    cb.onScrub        = [c](int64_t t, bool) { if (c) c->viewTimelineAt(t); };
    cb.onScrubBegin   = [c]() { if (c) c->beginTimelineScrub(); };
    cb.onScrubCancel  = [c]() { if (c) c->cancelTimelineScrub(); };
    cb.onStep         = [c](int dir) { if (c) c->stepTimelineChange(dir); };
    // The graph counts FIELDS of this class that changed (the byte counts of
    // the whole source stand in until the view has composed once).
    cb.columns = [c](int64_t t0, int64_t step, int n, QVector<TimelineColumn>& out) {
        out.resize(n);
        if (!c || !c->timelineContext()) return;
        const bool fields = c->timelineCountsFields();
        const tl::TimelineModel& m = c->timelineContext()->model();
        const tl::ClassChangeSeries& s = c->timelineClassChanges();
        for (int i = 0; i < n; ++i) {
            const int64_t a = t0 + step * i;
            const auto agg = fields ? s.aggregate(a, a + step) : m.aggregate(a, a + step);
            out[i].maxChanged = agg.max;
            out[i].sumChanged = agg.sum;
            out[i].records = agg.count;
        }
    };
    cb.changeTicks = [c](int64_t t0, int64_t t1) {
        QVector<int64_t> ticks;
        if (!c || !c->timelineContext()) return ticks;
        if (c->timelineCountsFields()) return c->timelineClassChanges().times(t0, t1);
        const tl::TimelineModel& m = c->timelineContext()->model();
        const auto range = m.indexRange(t0, t1);
        for (int i = range.first; i < range.second && ticks.size() < 256; ++i) {
            const tl::RecordId r = m.recordAtIndex(i);
            if (m.changedBytesOf(r) > 0) ticks.append(m.timeOf(r));
        }
        return ticks;
    };
    // The selection lane, per graph column: did the selected rows change?
    cb.selection = [c](int64_t t0, int64_t step, int n, QVector<char>& out) {
        out.fill(0, std::max(n, 0));
        if (!c || !c->timelineContext()) return;
        const QVector<tl::FieldSpan> scope = c->timelineSelectionScope();
        if (scope.isEmpty()) return;
        c->timelineClassChanges().matchColumns(t0, step, n, &scope, out);
    };
    // Beads, per graph column: the most fields one record started changing
    // (a bounce among steady motion).
    cb.events = [c](int64_t t0, int64_t step, int n, QVector<uint32_t>& out) {
        out.fill(0, std::max(n, 0));
        if (!c || !c->timelineContext() || !c->timelineCountsFields()) return;
        const tl::ClassChangeSeries& series = c->timelineClassChanges();
        for (int i = 0; i < n; ++i)
            out[i] = series.onsetMax(t0 + step * i, t0 + step * (i + 1));
    };
    // The records themselves, when there are few enough to draw one by one:
    // the graph places each at its own moment, so it flows with the clock
    // rather than stepping with the bin grid.
    cb.points = [c](int64_t t0, int64_t t1, int cap, QVector<TimelinePoint>& out) {
        out.clear();
        if (!c || !c->timelineContext()) return true;   // nothing to draw, completely
        if (c->timelineCountsFields()) {
            const QVector<tl::FieldSpan> scope = c->timelineSelectionScope();
            QVector<tl::ClassChangeSeries::Point> pts;
            const bool complete = c->timelineClassChanges().points(
                t0, t1, scope.isEmpty() ? nullptr : &scope, cap, pts);
            if (!complete) return false;
            out.reserve(pts.size());
            for (const auto& p : pts)
                out.append(TimelinePoint{p.tMs, p.count, p.onset, p.matched});
            return true;
        }
        const tl::TimelineModel& m = c->timelineContext()->model();
        const auto range = m.indexRange(t0, t1);
        if (range.second - range.first > cap) return false;
        out.reserve(range.second - range.first);
        for (int i = range.first; i < range.second; ++i) {
            const tl::RecordId r = m.recordAtIndex(i);
            out.append(TimelinePoint{m.timeOf(r), m.changedBytesOf(r), 0, false});
        }
        return true;
    };
    cb.describe = [c](int64_t t) -> QString { return c ? c->describeTimelineAt(t) : QString(); };
    strip->setCallbacks(cb);

    const bool enabled = QSettings("RC", "RC").value("timeline/enabled", true).toBool();
    strip->setVisible(enabled && QSettings("RC", "RC").value("timeline/stripVisible", true).toBool());
    ctrl->setTimelineEnabled(enabled);

    QPointer<TimelineStrip> sp(strip);
    auto push = [sp, c]() {
        if (sp && c && sp->isVisible()) sp->setState(timelineStripStateFor(c));
    };
    connect(ctrl, &RcxController::timelineChanged, strip, push);
    connect(ctrl, &RcxController::sourceStatusChanged, strip, push);
    connect(ctrl, &RcxController::selectionChanged, strip, push);
    connect(ctrl, &RcxController::timelineChanged, this, [this]() { syncTimelineActions(); });
    connect(ctrl, &RcxController::timelineRevealRequested, strip, [this, sp, c](int64_t a, int64_t b) {
        if (!sp || !c) return;
        if (m_actTimeline && !m_actTimeline->isChecked()) setTimelineEnabled(true);
        if (!sp->isVisible()) setTimelineStripVisible(true);
        sp->setState(timelineStripStateFor(c));
        sp->showRange(a, b);
    });
    // "Now" advances even when nothing changes: the graph moves, durations
    // tick. The beat follows what the strip is showing — capture rate while
    // this class is recording, lazy otherwise.
    auto* clock = new QTimer(strip);
    clock->setInterval(kTimelinePushIdleMs);
    connect(clock, &QTimer::timeout, strip, [sp, push, clock]() {
        push();
        const bool live = sp && sp->isVisible()
                       && sp->state().capture == timeline::Capture::Recording;
        const int want = live ? kTimelinePushLiveMs : kTimelinePushIdleMs;
        if (clock->interval() != want) clock->setInterval(want);
    });
    clock->start();
    push();
}

void MainWindow::setTimelineEnabled(bool on) {
    QSettings("RC", "RC").setValue("timeline/enabled", on);
    if (m_actTimeline && m_actTimeline->isChecked() != on) {
        QSignalBlocker block(m_actTimeline);
        m_actTimeline->setChecked(on);
    }
    for (auto& tab : m_tabs) {
        if (tab.ctrl) tab.ctrl->setTimelineEnabled(on);
        if (tab.timeline) {
            const bool show = on && (!m_actTlStrip || m_actTlStrip->isChecked());
            tab.timeline->setVisible(show);
            if (show && tab.ctrl) tab.timeline->setState(timelineStripStateFor(tab.ctrl));
        }
    }
    syncTimelineActions();
}

// View ▸ Timeline ▸ Show Timeline, and the strip menu's Hide Timeline: only
// the strip. Capture, history and the buttons beside the address stay.
void MainWindow::setTimelineStripVisible(bool on) {
    QSettings("RC", "RC").setValue("timeline/stripVisible", on);
    if (m_actTlStrip && m_actTlStrip->isChecked() != on) {
        QSignalBlocker block(m_actTlStrip);
        m_actTlStrip->setChecked(on);
    }
    const bool enabled = m_actTimeline && m_actTimeline->isChecked();
    for (auto& tab : m_tabs) {
        if (!tab.timeline) continue;
        tab.timeline->setVisible(on && enabled);
        if (on && enabled && tab.ctrl) tab.timeline->setState(timelineStripStateFor(tab.ctrl));
    }
}

void MainWindow::syncTimelineActions() {
    RcxController* ctrl = activeController();
    const bool on = m_actTimeline && m_actTimeline->isChecked();
    tl::CaptureContext* ctx = (on && ctrl) ? ctrl->timelineContext() : nullptr;
    const bool capturing = ctx != nullptr;
    const tl::CaptureMode mode = (capturing && ctrl->timelineHub())
        ? ctrl->timelineHub()->mode(ctrl->timelineClassId()) : tl::CaptureMode::Rolling;
    const bool hasData = capturing && ctrl->timelineHub()
                      && ctrl->timelineHub()->firstVisibleRecord(ctrl->timelineClassId()) != tl::kNoRecord;
    const bool past = capturing && ctrl->isViewingPast();
    if (m_actTlRecord) {
        m_actTlRecord->setEnabled(capturing);
        m_actTlRecord->setText(mode == tl::CaptureMode::Recording ? QStringLiteral("&Stop Recording")
                                                                  : QStringLiteral("&Record"));
    }
    if (m_actTlLive)  m_actTlLive->setEnabled(past);
    // The strip can only be shown while the timeline is on.
    if (m_actTlStrip) m_actTlStrip->setEnabled(on);
    // "Previous Change of 'health'": stepping follows the selection.
    QString of = capturing ? ctrl->timelineScopeLabel() : QString();
    of.replace(QLatin1Char('&'), QStringLiteral("&&"));
    if (!of.isEmpty()) of.prepend(QStringLiteral(" of "));
    if (m_actTlPrev) {
        m_actTlPrev->setEnabled(hasData);
        m_actTlPrev->setText(QStringLiteral("Pre&vious Change") + of);
    }
    if (m_actTlNext) {
        m_actTlNext->setEnabled(past);
        m_actTlNext->setText(QStringLiteral("&Next Change") + of);
    }
    if (m_actTlReset) m_actTlReset->setEnabled(hasData);
}

void MainWindow::confirmTimelineClear(RcxController* ctrl) {
    if (!ctrl || !ctrl->timelineHub()) return;
    // Nothing recorded for this class: nothing to clear.
    const bool hasRecording =
        ctrl->timelineHub()->firstVisibleRecord(ctrl->timelineClassId()) != tl::kNoRecord;
    if (!hasRecording) return;
    if (ctrl->document()) {
        QString className;
        const int idx = ctrl->document()->tree.indexOfId(ctrl->timelineClassId());
        if (idx >= 0) {
            const Node& n = ctrl->document()->tree.nodes[idx];
            className = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
        }
        const QString msg = QStringLiteral("Discard the recording of “%1”? This can't be undone.")
                                .arg(className);
        if (!ThemedMessageBox::confirm(this, QStringLiteral("Clear Recording"), msg,
                                       QStringLiteral("Clear"), QStringLiteral("Cancel"),
                                       /*destructive=*/true))
            return;
    }
    ctrl->timelineReset();
    syncTimelineActions();
}

// The one place ribbon visibility / collapse is decided: menu radios, the
// Ctrl+F1 toggle, the strip's chevron and the tab double-click all land here,
// so the persisted `ribbonState` can never disagree with what is on screen.
void MainWindow::applyRibbonState(int state, bool persist) {
    if (state < rcx::RibbonFull || state > rcx::RibbonHidden) state = rcx::RibbonFull;
    m_ribbonState = state;
    if (m_ribbon) {
        QSignalBlocker block(m_ribbon);   // minimizedChanged would re-enter
        m_ribbon->setMinimized(state != rcx::RibbonFull);
    }
    if (m_ribbonHost) m_ribbonHost->setVisible(state != rcx::RibbonHidden);
    if (m_ribbonStateGroup)
        for (QAction* a : m_ribbonStateGroup->actions())
            a->setChecked(a->data().toInt() == state);
    if (persist) QSettings("RC", "RC").setValue("ribbonState", state);
}

// Point the ribbon's enabled-state tracking at the active tab's controller.
// Reached from every tab-switch path through updateWindowTitle().
void MainWindow::syncRibbonController() {
    if (!m_ribbonActions) return;
    RcxController* c = activeController();
    if (m_ribbonActions->activeController() != c)
        m_ribbonActions->setActiveController(c);
}

void MainWindow::setActiveDocDock(QDockWidget* dock) {
    m_activeDocDock = dock;
    updateWindowTitle();      // chains updateScannerTitle() + updateSourceChip()
    // ...and so does the status segment: without this it kept the last
    // selection ANY tab made and went stale the moment you switched tabs.
    if (auto it = m_tabs.find(dock); it != m_tabs.end() && it->ctrl)
        refreshSelectionStatus(it->ctrl, -1);
    // The painted source icons are DERIVED from "differs from the active tab",
    // so they go stale the moment the active tab changes — including on the
    // paths that never touch a tab bar (focus into a side-by-side doc dock,
    // workspace navigation), which used to leave icons describing a tab the
    // user had already left.
    applyDocTabSourceIconVisibility(m_docDocks, dock);
    refreshBookmarksDock();
}

void MainWindow::updateScannerTitle() {
    if (!m_scanDockTitle) return;

    QString sourceName, sourceKind;
    auto provider = m_scannerPanel ? m_scannerPanel->resultProvider() : nullptr;
    if (!provider && activeController()) provider = activeController()->provider();
    if (provider) {
        sourceName = provider->name();
        sourceKind = provider->kind();
    }

    QString text;
    if (sourceName.isEmpty()) {
        // No provider attached on the active tab — be explicit about it
        // rather than leaving a stale "Scanner (notepad.exe)" lying
        // around from a previously-active tab.
        text = QStringLiteral("Scanner — no source on active tab");
    } else if (sourceKind.isEmpty()) {
        text = QStringLiteral("Scanner — %1").arg(sourceName);
    } else {
        text = QStringLiteral("Scanner — %1 (%2)")
            .arg(sourceName, sourceKind);
    }
    m_scanDockTitle->setText(text);

    // Window title (the OS chrome around the floating dock) gets the same
    // text so taskbar / Alt-Tab also identify the source.
    if (m_scannerDock) m_scannerDock->setWindowTitle(text);

    m_scanDockTitle->setToolTip(sourceName);
    if (m_scannerDock) m_scannerDock->setToolTip(QString());
}

void MainWindow::updateSourceChip() {
    if (!m_sourceChip) return;
    RcxController::SourceStatus st = RcxController::SourceStatus::None;
    QString label;
    if (m_activeDocDock && m_tabs.contains(m_activeDocDock)) {
        auto& tab = m_tabs[m_activeDocDock];
        if (tab.ctrl) st = tab.ctrl->sourceStatus();
        if (tab.doc && tab.ctrl->provider()) {
            const QString name = tab.ctrl->provider()->name();
            const QString kind = tab.ctrl->provider()->kind();
            if (!name.isEmpty())
                label = kind.isEmpty() ? name
                                       : QStringLiteral("%1:%2").arg(kind, name);
        }
    }
    if (label.isEmpty()) {
        m_sourceChip->setState(RcxController::SourceStatus::None, QString());
        return;
    }
    // A named source always shows; a None status (provider name present but no
    // active saved-source index) falls back to a neutral "Static" dot.
    if (st == RcxController::SourceStatus::None)
        st = RcxController::SourceStatus::Static;
    m_sourceChip->setState(st, label);
}

// ── Rendered view setup ──

void MainWindow::setupRenderedSci(QsciScintilla* sci) {
    QSettings settings("RC", "RC");
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

    // Line number margin — same background as editor, muted text, padded right
    sci->setMarginType(0, QsciScintilla::NumberMargin);
    sci->setMarginWidth(0, "0000000 ");  // room for large line counts + right padding
    const auto& theme = ThemeManager::instance().current();
    sci->setMarginsBackgroundColor(rcx::editorPaperColor(theme));
    sci->setMarginsForegroundColor(theme.textDim);
    sci->setMarginsFont(f);
    // Left padding between margin numbers and code content
    sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINLEFT, 0, (long)6);

    // Hide other margins
    sci->setMarginWidth(1, 0);
    sci->setMarginWidth(2, 0);

    // Attach + theme the per-language code lexer. setLexer (inside
    // applyCodeLexer) must run BEFORE the caret/selection/paper colors below,
    // because it resets them. The lexer is swapped per output format by
    // updateRenderedView; here we attach the one for the saved format so the
    // view is correctly coloured before the first render.
    const CodeFormat initialFmt = static_cast<CodeFormat>(
        QSettings("RC", "RC").value("codeFormat", 0).toInt());
    applyCodeLexer(sci, initialFmt, theme, f);
    const QColor editorBg = rcx::editorPaperColor(theme);
    sci->setBraceMatching(QsciScintilla::NoBraceMatch);

    // Colors applied AFTER setLexer() — the lexer resets these on attach
    sci->setPaper(editorBg);
    sci->setColor(theme.text);
    sci->setCaretForegroundColor(theme.text);
    sci->setCaretLineVisible(true);
    sci->setCaretLineBackgroundColor(theme.hover);
    sci->setSelectionBackgroundColor(theme.selection);
    sci->setSelectionForegroundColor(theme.text);
}

void MainWindow::setupDebugSci(QsciScintilla* sci) {
    QSettings settings("RC", "RC");
    QString fontName = settings.value("font", "JetBrains Mono").toString();
    QFont f(fontName, 12);
    f.setFixedPitch(true);

    sci->setFont(f);
    sci->setReadOnly(true);
    sci->setWrapMode(QsciScintilla::WrapNone);
    sci->setTabWidth(4);
    sci->setIndentationsUseTabs(false);
    sci->SendScintilla(QsciScintillaBase::SCI_SETEXTRAASCENT, (long)2);
    sci->SendScintilla(QsciScintillaBase::SCI_SETEXTRADESCENT, (long)2);

    // Line number margin
    sci->setMarginType(0, QsciScintilla::NumberMargin);
    sci->setMarginWidth(0, "0000000 ");
    const auto& theme = ThemeManager::instance().current();
    sci->setMarginsBackgroundColor(rcx::editorPaperColor(theme));
    sci->setMarginsForegroundColor(theme.textDim);
    sci->setMarginsFont(f);
    sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINLEFT, 0, (long)6);
    sci->setMarginWidth(1, 0);
    sci->setMarginWidth(2, 0);

    // Container lexer — we style manually per-character in updateDebugView
    sci->SendScintilla(QsciScintillaBase::SCI_SETLEXER, 2 /*SCLEX_CONTAINER*/);

    const QColor editorBg = rcx::editorPaperColor(theme);
    sci->setPaper(editorBg);
    sci->setColor(theme.text);
    sci->setCaretForegroundColor(theme.text);
    sci->setCaretLineVisible(true);
    sci->setCaretLineBackgroundColor(theme.hover);
    sci->setSelectionBackgroundColor(theme.selection);
    sci->setSelectionForegroundColor(theme.text);

    applyDebugStyles(sci);
}

void MainWindow::applyDebugStyles(QsciScintilla* sci) {
    const auto& theme = ThemeManager::instance().current();
    const QColor editorBg = rcx::editorPaperColor(theme);

    QSettings settings("RC", "RC");
    QString fontName = settings.value("font", "JetBrains Mono").toString();
    QFont f(fontName, 12);
    f.setFixedPitch(true);

    // Style 0: default text
    // Style 1: offset (before |) — dim
    // Style 2: pipe separator — border/accent
    // Style 3: bracket markers [>] [v] etc — accent
    // Style 4: middle dots (visible spaces) — faint
    // Style 5: meta comment ## — comment color
    // Style 6: meta keys (L=, nKind=, depth=) — dim
    // Style 7: meta values (numbers, kind names) — number/type color
    // Style 8: flags (static, cont, member, fold) — keyword color
    struct StyleDef { int id; QColor fg; };
    StyleDef styles[] = {
        {0, theme.text},
        {1, theme.textDim},
        {2, theme.border.lighter(150)},
        {3, theme.syntaxPreproc},
        {4, theme.textFaint},
        {5, theme.syntaxComment},
        {6, theme.textDim},
        {7, theme.syntaxNumber},
        {8, theme.syntaxKeyword},
    };
    for (auto& s : styles) {
        sci->SendScintilla(QsciScintillaBase::SCI_STYLESETFORE, (long)s.id,
            (long)(s.fg.red() | (s.fg.green() << 8) | (s.fg.blue() << 16)));
        sci->SendScintilla(QsciScintillaBase::SCI_STYLESETBACK, (long)s.id,
            (long)(editorBg.red() | (editorBg.green() << 8) | (editorBg.blue() << 16)));
        sci->SendScintilla(QsciScintillaBase::SCI_STYLESETFONT, (long)s.id,
            fontName.toUtf8().constData());
        sci->SendScintilla(QsciScintillaBase::SCI_STYLESETSIZE, (long)s.id, (long)12);
    }
}

// ── View mode / generator switching ──

void MainWindow::setViewMode(ViewMode mode) {
    auto* pane = findActiveSplitPane();
    if (!pane) return;
    pane->viewMode = mode;
    int idx = (mode == VM_Both) ? 3 : (mode == VM_Debug) ? 2
            : (mode == VM_Rendered) ? 1 : 0;
    pane->tabWidget->setCurrentIndex(idx);
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

void MainWindow::applyPaneZoom(SplitPane& p, int level) {
    level = qBound(-8, level, 24);
    QSettings("RC", "RC").setValue("viewZoomLevel", level);
    // Drive Scintilla's own zoom (the same SCI_SETZOOM that Ctrl+wheel uses) on
    // all three views so the pane stays consistent across tab switches. Block
    // each view's signals while zooming so the SCN_ZOOM sync can't bounce back.
    auto setZoom = [level](QsciScintilla* s) {
        if (!s) return;
        QSignalBlocker block(s);
        s->zoomTo(level);
    };
    setZoom(p.editor ? p.editor->scintilla() : nullptr);
    setZoom(p.rendered);
    setZoom(p.debugView);

    if (p.zoomSlider && p.zoomSlider->value() != level) {
        QSignalBlocker block(p.zoomSlider);
        p.zoomSlider->setValue(level);
    }
    if (p.zoomLabel) {
        // Approximate % off the 12pt base for a familiar readout.
        int pct = qRound((12.0 + level) / 12.0 * 100.0);
        p.zoomLabel->setText(QStringLiteral("%1%").arg(pct));
    }
}

// ── Update the rendered view for a single pane ──

// Show the generated code for the active tab in its active pane, at `scope`.
// Drives the pane's own scope combo rather than writing the setting directly,
// so the combo, the QSettings key and every other open pane stay in step (the
// combo's currentIndexChanged owns all three).
void MainWindow::showCodeView(CodeScope scope) {
    if (!m_activeDocDock || !m_tabs.contains(m_activeDocDock)) return;
    TabState& tab = m_tabs[m_activeDocDock];
    const int idx = qBound(0, tab.activePaneIdx, int(tab.panes.size()) - 1);
    if (idx < 0 || idx >= tab.panes.size()) return;
    SplitPane& pane = tab.panes[idx];
    if (pane.scopeCombo) pane.scopeCombo->setCurrentIndex(static_cast<int>(scope));
    // Tab 1 is Code; the tab bar's currentChanged sets viewMode and renders.
    if (pane.tabWidget && pane.tabWidget->currentIndex() != 1)
        pane.tabWidget->setCurrentIndex(1);
    else
        updateRenderedView(tab, pane);
}

void MainWindow::updateRenderedView(TabState& tab, SplitPane& pane) {
    if (pane.viewMode != VM_Rendered && pane.viewMode != VM_Both) return;
    if (!pane.rendered) return;

    // Determine which struct to render based on selection
    uint64_t rootId = 0;
    QSet<uint64_t> selIds = tab.ctrl->selectedIds();
    if (selIds.size() >= 1) {
        uint64_t selId = baseNodeIdFromSelId(*selIds.begin());
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

    // Generate text — cached on (tree.generation, rootId, format, scope, asserts).
    // Refresh ticks that don't touch tree shape (provider tick, value updates,
    // selection-only) reuse the prior render. On 200-class projects this drops
    // ~800ms generator passes to <1ms.
    const QHash<NodeKind, QString>* aliases =
        tab.doc->typeAliases.isEmpty() ? nullptr : &tab.doc->typeAliases;
    bool asserts = QSettings("RC", "RC").value("generatorAsserts", false).toBool();
    CodeFormat fmt = static_cast<CodeFormat>(
        QSettings("RC", "RC").value("codeFormat", 0).toInt());
    CodeScope scope = static_cast<CodeScope>(
        QSettings("RC", "RC").value("codeScope", 0).toInt());
    quint64 treeGen = tab.doc->tree.generation();
    bool cacheHit = (!pane.lastRenderedText.isEmpty()
                     && pane.lastRenderedTreeGen == treeGen
                     && pane.lastRenderedRootId == rootId
                     && pane.lastRenderedFmt == static_cast<int>(fmt)
                     && pane.lastRenderedScope == static_cast<int>(scope)
                     && pane.lastRenderedAsserts == asserts);
    QString text;
    if (cacheHit) {
        text = pane.lastRenderedText;
    } else {
        if (scope == CodeScope::FullSdk) {
            text = renderCodeAll(fmt, tab.doc->tree, aliases, asserts);
        } else if (rootId != 0) {
            if (scope == CodeScope::WithChildren)
                text = renderCodeTree(fmt, tab.doc->tree, rootId, aliases, asserts);
            else
                text = renderCode(fmt, tab.doc->tree, rootId, aliases, asserts);
        } else {
            text = renderCodeAll(fmt, tab.doc->tree, aliases, asserts);
        }
        pane.lastRenderedText     = text;
        pane.lastRenderedTreeGen  = treeGen;
        pane.lastRenderedFmt      = static_cast<int>(fmt);
        pane.lastRenderedScope    = static_cast<int>(scope);
        pane.lastRenderedAsserts  = asserts;
    }

    // Scroll restoration: save if same root, reset if different
    int restoreLine = 0;
    if (rootId != 0 && rootId == pane.lastRenderedRootId) {
        restoreLine = (int)pane.rendered->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
    }
    pane.lastRenderedRootId = rootId;

    // Attach the per-language lexer. Only needed when the generated text was
    // actually re-rendered (format/structure change) or no lexer is attached
    // yet — on a value-only refresh tick the cached text keeps its existing
    // lexer styling. Theme switches re-theme the lexer separately in
    // applyTheme(), so calling it every tick here was ~270 redundant Scintilla
    // style mutations per refresh while the Code pane is visible.
    if (!cacheHit || !pane.rendered->lexer())
        applyCodeLexer(pane.rendered, fmt, ThemeManager::instance().current(),
                       pane.rendered->font());

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

    // Update margin width for line count (min 5 digits + padding)
    int lineCount = pane.rendered->lines();
    int digits = qMax(5, QString::number(lineCount).size() + 2);
    QString marginStr = QString(digits, '0') + QStringLiteral("  ");
    pane.rendered->setMarginWidth(0, marginStr);

    // Restore scroll
    if (restoreLine > 0) {
        pane.rendered->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                                     (unsigned long)restoreLine);
    }
}

void MainWindow::updateAllRenderedPanes(TabState& tab) {
    for (auto& pane : tab.panes) {
        if (pane.viewMode == VM_Rendered || pane.viewMode == VM_Both)
            updateRenderedView(tab, pane);
    }
}

// ── Debug view ──

QString MainWindow::generateDebugText(RcxEditor* editor) const {
    auto* sci = editor->scintilla();
    int lineCount = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETLINECOUNT);
    QStringList output;
    output.reserve(lineCount);

    // LineKind names for annotation
    static const char* lineKindNames[] = {
        "CmdRow", "Blank", "Header", "Field", "Cont", "Footer", "ArrSep"
    };

    for (int i = 0; i < lineCount; i++) {
        // Margin text (offset)
        QString margin;
        const LineMeta* lm = editor->metaForLine(i);
        if (lm) margin = lm->offsetText;

        QString lineText = sciGetLineText(sci, i);

        // Replace special Unicode chars with visible names, spaces with middle dot
        QString annotated;
        annotated.reserve(lineText.size() * 2);
        for (QChar ch : lineText) {
            switch (ch.unicode()) {
            case 0x25B8: annotated += QStringLiteral("[>]"); break;   // ▸ command-row chevron
            case 0x2026: annotated += QStringLiteral("[..]"); break;  // … ellipsis
            case 0x2192: annotated += QStringLiteral("[->]"); break;  // → arrow
            case 0x00B7: annotated += QStringLiteral("[.]"); break;   // · middle dot (margin)
            case ' ':    annotated += QChar(0x00B7); break;           // space → visible dot
            default:     annotated += ch; break;
            }
        }

        // Per-line metadata annotation
        QString meta;
        if (lm) {
            int lk = (int)lm->lineKind;
            const char* kindName = (lk >= 0 && lk <= 6) ? lineKindNames[lk] : "?";
            const LineChip* dbgComment  = findChip(*lm, ChipKind::Comment);
            const LineChip* dbgTypeHint = findChip(*lm, ChipKind::TypeHint);
            meta = QStringLiteral("  ## L=%1 %2 nKind=%3 depth=%4 cmtStart=%5 tW=%6 nW=%7")
                .arg(i)
                .arg(QString::fromLatin1(kindName))
                .arg(QString::fromLatin1(kindToString(lm->nodeKind)))
                .arg(lm->depth)
                .arg(dbgComment ? dbgComment->startCol : -1)
                .arg(lm->effectiveTypeW)
                .arg(lm->effectiveNameW);
            if (lm->isContinuation) meta += QStringLiteral(" cont");
            if (lm->isMemberLine) meta += QStringLiteral(" member");
            if (lm->isArrayElement) meta += QStringLiteral(" arrElem");
            if (lm->foldHead) meta += lm->foldCollapsed ? QStringLiteral(" fold+") : QStringLiteral(" fold-");
            if (dbgTypeHint) meta += QStringLiteral(" hint@%1").arg(dbgTypeHint->startCol);
            // The drawn tree lines of this row, as text (they are not in the buffer).
            const QString tree = editor->treePrefixForLine(i);
            if (!tree.trimmed().isEmpty()) meta += QStringLiteral(" tree=[%1]").arg(tree);
            const TreeColumns& tc = editor->treeColumnGuides();
            if (i < tc.lineCount && tc.endCol(i) > tc.firstCol(i)) {
                QStringList cells;
                for (int k = tc.firstCol(i); k < tc.endCol(i); ++k) cells << QString::number(tc.cols[k]);
                meta += QStringLiteral(" cols=[%1]").arg(cells.join(QLatin1Char(',')));
            }
        }

        output.append(margin + QStringLiteral("|") + annotated + meta);
    }
    return output.join('\n');
}

void MainWindow::updateDebugView(TabState& tab, SplitPane& pane) {
    if (pane.viewMode != VM_Debug) return;
    if (!pane.debugView || !pane.editor) return;

    QString text = generateDebugText(pane.editor);
    pane.debugView->setReadOnly(false);
    pane.debugView->setText(text);

    // Apply syntax highlighting
    styleDebugText(pane.debugView, text);

    pane.debugView->setReadOnly(true);

    // Adjust margin width for line count
    int lineCount = pane.debugView->lines();
    int digits = qMax(5, QString::number(lineCount).size() + 2);
    pane.debugView->setMarginWidth(0, QString(digits, '0') + QStringLiteral("  "));
}

void MainWindow::styleDebugText(QsciScintilla* sci, const QString& text) {
    // Style IDs (matching applyDebugStyles):
    // 0=text, 1=offset, 2=pipe, 3=bracket markers, 4=middle dots,
    // 5=meta comment, 6=meta keys, 7=meta values, 8=flags/keywords
    QByteArray utf8 = text.toUtf8();
    int len = utf8.size();
    if (len == 0) return;

    QByteArray styles(len, '\0');  // style per byte

    // State machine per line
    int i = 0;
    while (i < len) {
        // Find end of line
        int lineEnd = utf8.indexOf('\n', i);
        if (lineEnd < 0) lineEnd = len;

        // Phase 1: offset region (before first '|')
        int pipePos = -1;
        for (int j = i; j < lineEnd; j++) {
            if (utf8[j] == '|') { pipePos = j; break; }
        }

        if (pipePos >= 0) {
            // Offset text
            for (int j = i; j < pipePos; j++)
                styles[j] = 1;
            // Pipe
            styles[pipePos] = 2;

            // Phase 2: content after pipe, before ##
            int metaPos = -1;
            // Search for "  ## " which marks metadata
            for (int j = pipePos + 1; j < lineEnd - 4; j++) {
                if (utf8[j] == ' ' && utf8[j+1] == ' ' && utf8[j+2] == '#' && utf8[j+3] == '#') {
                    metaPos = j;
                    break;
                }
            }
            int contentEnd = (metaPos >= 0) ? metaPos : lineEnd;

            // Style content region
            for (int j = pipePos + 1; j < contentEnd; j++) {
                unsigned char ch = (unsigned char)utf8[j];
                if (ch == 0xC2 && j + 1 < contentEnd && (unsigned char)utf8[j+1] == 0xB7) {
                    // UTF-8 for U+00B7 middle dot (visible space)
                    styles[j] = 4;
                    styles[j+1] = 4;
                    j++; // skip second byte
                } else if (ch == '[') {
                    // Bracket marker: find closing ]
                    int closeB = -1;
                    for (int k = j + 1; k < qMin(j + 5, contentEnd); k++) {
                        if (utf8[k] == ']') { closeB = k; break; }
                    }
                    if (closeB >= 0) {
                        for (int k = j; k <= closeB; k++)
                            styles[k] = 3;
                        j = closeB;
                    }
                }
                // else: stays 0 (default text)
            }

            // Phase 3: metadata region (after ##)
            if (metaPos >= 0) {
                // "  ## " prefix
                for (int j = metaPos; j < qMin(metaPos + 4, lineEnd); j++)
                    styles[j] = 5;

                // Parse key=value pairs and keywords in meta region
                int mStart = metaPos + 4;
                int j = mStart;
                while (j < lineEnd) {
                    // Skip spaces
                    if (utf8[j] == ' ') { styles[j] = 5; j++; continue; }

                    // Check for key=value: scan for '='
                    int eqPos = -1;
                    int tokEnd = j;
                    while (tokEnd < lineEnd && utf8[tokEnd] != ' ')
                        tokEnd++;
                    for (int k = j; k < tokEnd; k++) {
                        if (utf8[k] == '=') { eqPos = k; break; }
                    }

                    if (eqPos >= 0) {
                        // key part (before =)
                        for (int k = j; k <= eqPos; k++)
                            styles[k] = 6;
                        // value part (after =)
                        for (int k = eqPos + 1; k < tokEnd; k++)
                            styles[k] = 7;
                    } else {
                        // Standalone token — check if it's a LineKind or flag
                        QByteArray tok = utf8.mid(j, tokEnd - j);
                        if (tok == "CmdRow" || tok == "Blank" || tok == "Header"
                            || tok == "Field" || tok == "Cont" || tok == "Footer"
                            || tok == "ArrSep") {
                            // LineKind — use type color
                            for (int k = j; k < tokEnd; k++)
                                styles[k] = 7;
                        } else if (tok == "static" || tok == "cont" || tok == "member"
                                   || tok == "arrElem" || tok == "fold+" || tok == "fold-") {
                            // Flags — keyword color
                            for (int k = j; k < tokEnd; k++)
                                styles[k] = 8;
                        } else {
                            // Unknown meta token
                            for (int k = j; k < tokEnd; k++)
                                styles[k] = 5;
                        }
                    }
                    j = tokEnd;
                }
            }
        }
        // else: no pipe found, leave as default style 0

        i = lineEnd + 1; // skip \n
    }

    // Apply styles to Scintilla
    sci->SendScintilla(QsciScintillaBase::SCI_STARTSTYLING, (long)0, (long)0x1f);
    sci->SendScintilla(QsciScintillaBase::SCI_SETSTYLINGEX, (long)len, styles.constData());
}

void MainWindow::updateAllDebugPanes(TabState& tab) {
    for (auto& pane : tab.panes) {
        if (pane.viewMode == VM_Debug)
            updateDebugView(tab, pane);
    }
}

// ── Export C++ header to file ──

void MainWindow::exportToFile(CodeFormat fmt) {
    auto* tab = activeTab();
    if (!tab) return;

    QString title = QStringLiteral("Export %1").arg(codeFormatName(fmt));
    QString path = QFileDialog::getSaveFileName(this, title, {},
        QString::fromLatin1(codeFormatFileFilter(fmt)));
    if (path.isEmpty()) return;

    const QHash<NodeKind, QString>* aliases =
        tab->doc->typeAliases.isEmpty() ? nullptr : &tab->doc->typeAliases;
    bool asserts = QSettings("RC", "RC").value("generatorAsserts", false).toBool();
    QString text = renderCodeAll(fmt, tab->doc->tree, aliases, asserts);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        ThemedMessageBox::warn(this,
            QStringLiteral("Export Failed"),
            QStringLiteral("Couldn't write to %1.").arg(path));
        return;
    }
    file.write(text.toUtf8());
    setAppStatus("Exported to " + QFileInfo(path).fileName());
}

void MainWindow::exportCpp()     { exportToFile(CodeFormat::CppHeader); }
void MainWindow::exportRust()    { exportToFile(CodeFormat::RustStruct); }
void MainWindow::exportDefines() { exportToFile(CodeFormat::DefineOffsets); }
void MainWindow::exportCSharp()  { exportToFile(CodeFormat::CSharpStruct); }
void MainWindow::exportPython()  { exportToFile(CodeFormat::PythonCtypes); }

// ── Export ReClass XML ──

void MainWindow::exportReclassXmlAction() {
    auto* tab = activeTab();
    if (!tab) return;

    QString path = QFileDialog::getSaveFileName(this,
        "Export ReClass XML", {}, "ReClass XML (*.RC);;All Files (*)");
    if (path.isEmpty()) return;

    QString error;
    if (!rcx::exportReclassXml(tab->doc->tree, path, &error)) {
        ThemedMessageBox::warn(this,
            QStringLiteral("Export Failed"),
            error.isEmpty() ? QStringLiteral("Couldn't export the project.")
                            : error);
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
        "Import ReClass / ReClass.NET", {},
        "ReClass formats (*.RC *.MemeCls *.xml *.rcnet);;"
        "ReClass.NET (*.rcnet);;"
        "ReClass XML (*.RC *.MemeCls *.xml);;"
        "All Files (*)");
    if (filePath.isEmpty()) return;

    QString error;
    NodeTree tree = rcx::importReclassXml(filePath, &error);
    if (tree.nodes.isEmpty()) {
        ThemedMessageBox::warn(this,
            QStringLiteral("Import Failed"),
            error.isEmpty()
                ? QStringLiteral("The file doesn't contain any class data.")
                : error);
        return;
    }

    // Count root structs for status message
    int classCount = 0;
    for (const auto& n : tree.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) classCount++;

    auto* doc = new RcxDocument(this);
    doc->tree = std::move(tree);

    { ClosingGuard guard(m_closingAll);
      closeAllDocDocks();
      createTab(doc);
    }
    rebuildWorkspaceModel();
    setAppStatus(QStringLiteral("Imported %1 classes from %2")
        .arg(classCount).arg(QFileInfo(filePath).fileName()));
}

// ── Import from Source ──

void MainWindow::importFromSource() {
    ThemedDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Import from Source"));
    dlg.resize(700, 600);

    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    auto* sci = new QsciScintilla(&dlg);
    setupRenderedSci(sci);
    sci->setReadOnly(false);
    sci->setMarginWidth(0, "00000");
    layout->addWidget(sci);

    auto* cancelBtn = new DialogButton(QStringLiteral("Cancel"),
        DialogButton::Secondary, &dlg);
    auto* importBtn = new DialogButton(QStringLiteral("Import"),
        DialogButton::Primary, &dlg);
    connect(importBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    importBtn->setDefault(true);
    layout->addLayout(ThemedDialog::makeButtonRow({ cancelBtn, importBtn }));

    if (dlg.exec() != QDialog::Accepted) return;

    QString source = sci->text();
    if (source.trimmed().isEmpty()) return;

    QString error;
    NodeTree tree = rcx::importFromSource(source, &error);
    if (tree.nodes.isEmpty()) {
        ThemedMessageBox::warn(this,
            QStringLiteral("Import Failed"),
            error.isEmpty()
                ? QStringLiteral("No struct definitions were found in the pasted source.")
                : error);
        return;
    }

    int classCount = 0;
    for (const auto& n : tree.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) classCount++;

    auto* doc = new RcxDocument(this);
    doc->tree = std::move(tree);

    { ClosingGuard guard(m_closingAll);
      closeAllDocDocks();
      createTab(doc);
    }
    rebuildWorkspaceModel();
    setAppStatus(QStringLiteral("Imported %1 classes from source").arg(classCount));
}

// ── Import PDB ──
// Opens a file dialog, loads symbols + types into the Symbols dock,
// and switches to the Types tab for the user to select and import.

void MainWindow::importPdb() {
    QString pdbPath = QFileDialog::getOpenFileName(this,
        "Select PDB File", {},
        "PDB Files (*.pdb);;All Files (*)");
    if (pdbPath.isEmpty()) return;

    int symCount = loadPdbAndCacheTypes(pdbPath);
    rebuildSymbols();
    if (m_symbolsDock) m_symbolsDock->show();

    // Count the types that were just loaded so the status reflects whether
    // this PDB was stripped of TPI data (typical for MS user-mode public
    // PDBs like advapi32) or carries real type definitions (ntdll, ntos,
    // private builds).
    int typeCount = 0;
    QString baseName = QFileInfo(pdbPath).completeBaseName();
    QString canonical = rcx::SymbolStore::instance().resolveAlias(baseName);
    if (const auto* set = rcx::SymbolStore::instance().moduleData(canonical))
        typeCount = set->types.size();

    QString fname = QFileInfo(pdbPath).fileName();
    if (typeCount > 0) {
        setAppStatus(QStringLiteral("Loaded %1 symbols + %2 types from %3 — "
                                    "right-click a type to import")
            .arg(symCount).arg(typeCount).arg(fname));
    } else {
        setAppStatus(QStringLiteral("Loaded %1 symbols from %2 "
                                    "(public PDB — no type info)")
            .arg(symCount).arg(fname));
    }
}

// ── Type Aliases Dialog ──

void MainWindow::showTypeAliasesDialog() {
    auto* tab = activeTab();
    if (!tab) return;

    ThemedDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Type Aliases"));
    dlg.resize(420, 400);

    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    // Preset buttons (stdint + Windows only, no redundant Reset)
    auto* presetRow = new QHBoxLayout;
    auto* btnStdint  = new DialogButton(QStringLiteral("stdint (C99)"),
        DialogButton::Secondary, &dlg);
    auto* btnWindows = new DialogButton(QStringLiteral("Windows (basetsd.h)"),
        DialogButton::Secondary, &dlg);
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

    // Skip types that nobody aliases (Vec, Mat, Struct, Array, Asm)
    auto shouldSkip = [](NodeKind k) {
        return k == NodeKind::Vec2  || k == NodeKind::Vec3
            || k == NodeKind::Vec4  || k == NodeKind::Mat4x4
            || k == NodeKind::Struct || k == NodeKind::Array
            || isCodeKind(k);   // "asm" is the disassembler's word, not a typedef
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

    auto* cancelBtn = new DialogButton(QStringLiteral("Cancel"),
        DialogButton::Secondary, &dlg);
    auto* okBtn = new DialogButton(QStringLiteral("Save aliases"),
        DialogButton::Primary, &dlg);
    connect(okBtn,     &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    okBtn->setDefault(true);
    layout->addLayout(ThemedDialog::makeButtonRow({ cancelBtn, okBtn }));

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

QDockWidget* MainWindow::project_new(const QString& classKeyword,
                                      bool forceFreshDoc) {
    // If an active document exists AND the caller didn't force fresh,
    // add the new struct to it and open in a new tab sharing the same
    // document (so all structs live in one project). File → New Class
    // and friends pass forceFreshDoc=true so each menu invocation
    // creates its own project with its own buffer — the user expects
    // a fresh sandbox, not a tab that silently shares the previous
    // project's base address and provider.
    auto* existingCtrl = activeController();
    // The live example's buffer belongs to its simulation: a new struct laid
    // over it would show (and take writes into) bytes rewritten 30 times a
    // second. It gets a document of its own instead.
    if (existingCtrl && !forceFreshDoc
        && !existingCtrl->document()->property("rcxDemoSimulation").toBool()) {
        auto* doc = existingCtrl->document();
        buildEmptyStruct(doc->tree, classKeyword);

        // Find the struct we just added (last root struct in tree)
        uint64_t newId = 0;
        for (int i = doc->tree.nodes.size() - 1; i >= 0; --i) {
            if (doc->tree.nodes[i].parentId == 0 && doc->tree.nodes[i].kind == NodeKind::Struct) {
                newId = doc->tree.nodes[i].id;
                break;
            }
        }

        // Open in a new tab sharing the same document
        auto* dock = createTab(doc);
        if (newId != 0) {
            m_tabs[dock].ctrl->setViewRootId(newId);
            m_tabs[dock].ctrl->refresh();
            QString name = rootName(doc->tree, newId);
            dock->setWindowTitle(name);
        }
        // Copy saved sources from the original controller
        if (!existingCtrl->savedSources().isEmpty()) {
            m_tabs[dock].ctrl->copySavedSources(existingCtrl->savedSources(),
                                                  existingCtrl->activeSourceIndex());
        }
        rebuildWorkspaceModelNow();
        return dock;
    }

    // No active document — create a fresh one
    auto* doc = new RcxDocument(this);

#ifdef Q_OS_WIN
    // Self-attach: allocate a 64 KB RW buffer in our own process and
    // point baseAddress at it. The processmemory provider then reads
    // and writes against our own PID via RPM/WPM — every byte the user
    // sees is guaranteed writable. No source picker, no 0x00400000
    // placeholder, no "page invalid" landing experience for beginners.
    constexpr size_t kBufSize = 64 * 1024;
    doc->m_ownedBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[kBufSize]());
    doc->m_ownedBufferSize = kBufSize;
    // (Zero-init is handled by the value-init `new uint8_t[N]()` above.)
    doc->tree.baseAddress = reinterpret_cast<uint64_t>(doc->m_ownedBuffer.get());

    buildEmptyStruct(doc->tree, classKeyword);

    auto* dock = createTab(doc);

    // Attach via processmemory if available — mirror selfTest's guard
    // so a missing plugin DLL doesn't pop a "Provider Unavailable"
    // dialog on every New Class.
    if (ProviderRegistry::instance().findProvider(QStringLiteral("processmemory"))) {
        auto& tab = m_tabs[dock];
        DWORD pid = GetCurrentProcessId();
        QString target = QString("%1:RC.exe").arg(pid);
        // registerAsSavedSource=true so the source-picker dropdown
        // surfaces "RC.exe" as an entry. Without this the user
        // would see an active source label but an empty dropdown,
        // breaking discoverability when they want to switch back.
        tab.ctrl->attachViaPlugin(QStringLiteral("processmemory"), target,
                                  /*registerAsSavedSource=*/true);
        // Re-pin baseAddress — attachViaPlugin re-evaluates any saved
        // formula and may overwrite it. The buffer's address is what
        // we actually want to view. Same dance selfTest does (main.cpp
        // around line 4280).
        doc->tree.baseAddress = reinterpret_cast<uint64_t>(doc->m_ownedBuffer.get());
        doc->tree.baseAddressFormula.clear();
        tab.ctrl->refresh();
    }
    // NOTE: do NOT call setReadOnlyOverride here. The buffer is ours
    // to write into — that's the entire point of this branch. The
    // override that selfTest sets is for the live RcxEditor object
    // where stomping bytes can crash Qt's virtual dispatch; this
    // scenario is completely different.
#else
    // Non-Windows: processmemory plugin is Windows-only today. Fall
    // back to the legacy 256-byte zero buffer at a fake base. Once
    // Linux/macOS get a processmemory equivalent, this branch can
    // adopt the same self-attach behaviour.
    QByteArray data(256, '\0');
    doc->loadData(data);
    doc->tree.baseAddress = 0x00400000;

    buildEmptyStruct(doc->tree, classKeyword);

    auto* dock = createTab(doc);
#endif

    // Workspace stays hidden until the user opens it via View menu.
    // The dock's visibilityChanged handler in createWorkspaceDock
    // triggers rebuildWorkspaceModel() the first time it's shown, so
    // we don't pay the tree-walk + QStandardItem construction cost
    // until the user actually wants to see it. (placeSidebarDock used
    // to fire here and force-show the dock on every "New Class".)

    rebuildWorkspaceModelNow();
    return dock;
}

// The class a first launch opens on: a bouncing-ball simulation running in a
// buffer inside RC, self-attached like New Class, so the view, its
// change highlights and the timeline have something alive to show from the
// first second. Values typed into it feed back into the simulation.
QDockWidget* MainWindow::project_newSimulation() {
#ifdef Q_OS_WIN
    if (!ProviderRegistry::instance().findProvider(QStringLiteral("processmemory")))
        return project_new(QStringLiteral("class"), /*forceFreshDoc=*/true);

    auto* doc = new RcxDocument(this);
    doc->setProperty("rcxDemoSimulation", true);   // project_new never stacks a struct on it
    constexpr size_t kBufSize = 64 * 1024;
    doc->m_ownedBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[kBufSize]());
    doc->m_ownedBufferSize = kBufSize;
    const uint64_t base = reinterpret_cast<uint64_t>(doc->m_ownedBuffer.get());
    doc->tree.baseAddress = base;
    const demo::BuiltDemo built = demo::buildBouncingBall(doc->tree);
    doc->tree.initialClass = QStringLiteral("BouncingBall");
    auto sim = std::make_shared<demo::BouncingBallSim>(doc->m_ownedBuffer.get(), doc->m_ownedBufferSize);
    sim->reset();   // real values before the first read

    auto* dock = createTab(doc);
    auto& tab = m_tabs[dock];
    const DWORD pid = GetCurrentProcessId();
    tab.ctrl->attachViaPlugin(QStringLiteral("processmemory"),
                              QString("%1:RC.exe").arg(pid), /*registerAsSavedSource=*/true);
    // attachViaPlugin may re-evaluate a formula over the base: pin it back.
    doc->tree.baseAddress = base;
    doc->tree.baseAddressFormula.clear();
    tab.ctrl->setViewRootId(built.ballClassId);
    tab.ctrl->refresh();
    dock->setWindowTitle(QStringLiteral("BouncingBall"));

    // 30 steps a second on the UI thread. The timer is the document's child
    // and the buffer the document's member, so both go when the tab closes.
    auto* timer = new QTimer(doc);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, doc, [sim]() { sim->step(); });
    timer->start(int(std::lround(demo::BouncingBallSim::kStepSeconds * 1000.0)));

    rebuildWorkspaceModelNow();
    return dock;
#else
    return project_new(QStringLiteral("class"), /*forceFreshDoc=*/true);
#endif
}

QDockWidget* MainWindow::project_open(const QString& path) {
    PROFILE_SCOPE("MainWindow::project_open");
    QString filePath = path;
    if (filePath.isEmpty()) {
        filePath = QFileDialog::getOpenFileName(this,
            "Open File", {},
            "Supported Files (*.rcx *.json *.rcnet *.reclass *.xml *.bin *.dmp *.dump)"
            ";;RC (*.rcx);;Binary Files (*.bin *.dmp *.dump);;All (*)");
        if (filePath.isEmpty()) return nullptr;
    }

    QFile probe(filePath);
    if (!probe.open(QIODevice::ReadOnly)) {
        ThemedMessageBox::warn(this, QStringLiteral("Open Failed"), probe.errorString());
        return nullptr;
    }
    const QByteArray head = probe.read(4096).trimmed();
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    const bool binary = QStringList{"bin", "dmp", "dump"}.contains(suffix);
    const bool definition = !binary && (QStringList{"rcx", "json", "rcnet", "reclass", "xml"}.contains(suffix)
        || head.startsWith('{') || head.startsWith("\xEF\xBB\xBF{")
        || head.startsWith("<?xml") || head.startsWith("<ReClass"));
    probe.close();
    if (!definition) {
        auto* doc = new RcxDocument(this);
        if (!doc->loadData(filePath)) {
            delete doc;
            ThemedMessageBox::warn(this, QStringLiteral("Open Failed"),
                                   QStringLiteral("Couldn't open %1.").arg(filePath));
            return nullptr;
        }
        buildEmptyStruct(doc->tree);
        auto* dock = createTab(doc);
        SavedSourceEntry entry;
        entry.kind = QStringLiteral("File");
        entry.filePath = QFileInfo(filePath).absoluteFilePath();
        entry.displayName = QFileInfo(filePath).fileName();
        m_tabs[dock].ctrl->copySavedSources({entry}, 0);
        rebuildWorkspaceModel();
        addRecentFile(filePath);
        setAppStatus(QStringLiteral("Opened %1 (%2 bytes)")
            .arg(entry.displayName, QLocale().toString(QFileInfo(filePath).size())));
        return dock;
    }

    // Recovery: if a fresher .autosave shadow exists next to the target,
    // ask whether to restore. We delete the .autosave file in either case
    // — if the user declines, the next normal save sequence will reclaim
    // the slot anyway, and keeping a stale shadow would re-trigger the
    // prompt forever.
    //
    // restoredFromAutosave preserves the original target path so a
    // subsequent Ctrl+S writes back to foo.rcx, NOT foo.rcx.autosave.
    // Without this fixup `doc->filePath` would carry the .autosave
    // path forward and Save would never restore the user's real file.
    QString originalPath;
    {
        QString shadow = filePath + QStringLiteral(".autosave");
        QFileInfo origInfo(filePath);
        QFileInfo shadowInfo(shadow);
        if (shadowInfo.exists() && shadowInfo.isFile()
            && shadowInfo.lastModified() > origInfo.lastModified()) {
            // Show the full absolute path so the user can verify
            // exactly which file (and which shadow) the prompt refers
            // to before agreeing — important when the same basename
            // exists in multiple places.
            bool restore = ThemedMessageBox::confirm(this,
                QStringLiteral("Restore Autosave?"),
                QStringLiteral(
                    "A more recent autosave exists for this file.\n\n"
                    "Original:\n  %1\n  modified %2\n\n"
                    "Autosave:\n  %3\n  modified %4\n\n"
                    "Open the autosave instead?")
                    .arg(origInfo.absoluteFilePath())
                    .arg(QLocale().toString(origInfo.lastModified(), QLocale::ShortFormat))
                    .arg(shadowInfo.absoluteFilePath())
                    .arg(QLocale().toString(shadowInfo.lastModified(), QLocale::ShortFormat)),
                QStringLiteral("Restore"),
                QStringLiteral("Open original"));
            if (restore) {
                originalPath = filePath;
                filePath = shadow;
            } else {
                QFile::remove(shadow);
            }
        }
    }

    // Detect if this is an XML-based ReClass file by checking first bytes.
    // Also recognise the ReClass.NET .rcnet ZIP container (PK\x03\x04
    // magic) — importReclassXml handles the unzip itself.
    bool isXml = false;
    {
        QFile probe(filePath);
        if (probe.open(QIODevice::ReadOnly)) {
            QByteArray head = probe.read(64);
            QByteArray trimmed = head.trimmed();
            isXml = trimmed.startsWith("<?xml")
                 || trimmed.startsWith("<ReClass")
                 || head.startsWith("PK\x03\x04");
        }
    }

    if (isXml) {
        QString error;
        NodeTree tree = rcx::importReclassXml(filePath, &error);
        if (tree.nodes.isEmpty()) {
            ThemedMessageBox::warn(this,
                QStringLiteral("Import Failed"),
                error.isEmpty()
                    ? QStringLiteral("The file doesn't contain any class data.")
                    : error);
            return nullptr;
        }
        auto* doc = new RcxDocument(this);
        doc->tree = std::move(tree);
        QDockWidget* dock;
        { ClosingGuard guard(m_closingAll);
          closeAllDocDocks();
          dock = createTab(doc);
        }
        rebuildWorkspaceModel();
        int classCount = 0;
        for (const auto& n : doc->tree.nodes)
            if (n.parentId == 0 && n.kind == NodeKind::Struct) classCount++;
        setAppStatus(QStringLiteral("Imported %1 classes from %2")
            .arg(classCount).arg(QFileInfo(filePath).fileName()));
        addRecentFile(filePath);
        return dock;
    }

    auto* doc = new RcxDocument(this);

    // Show progress for large files
    beginProgress(QStringLiteral("Loading ") + QFileInfo(filePath).fileName(), 0);
    setAppStatus(QStringLiteral("Loading %1...").arg(QFileInfo(filePath).fileName()));
    QApplication::processEvents();

    if (!doc->load(filePath)) {
        ThemedMessageBox::warn(this,
            QStringLiteral("Open Failed"),
            QStringLiteral("Couldn't load %1.").arg(filePath));
        setAppStatus({});
        endProgress();
        delete doc;
        return nullptr;
    }

    // If we loaded from a .autosave shadow, point doc->filePath back at
    // the real file so a subsequent Save writes to foo.rcx, not the
    // shadow. Mark the doc modified so the user gets the usual unsaved-
    // changes prompt on close — the autosave content was an in-flight
    // edit, not a committed state.
    if (!originalPath.isEmpty()) {
        doc->filePath = originalPath;
        doc->modified = true;
        filePath = originalPath;  // local var so addRecentFile + status use the real path
    }

    int nodeCount = doc->tree.nodes.size();
    if (nodeCount > 5000) {
        updateProgress(0, QStringLiteral("Composing %1 nodes...").arg(nodeCount));
        setAppStatus(QStringLiteral("Composing %1 nodes...").arg(nodeCount));
    }
    QApplication::processEvents();

    // Close all existing tabs so the project replaces the current state
    QDockWidget* dock;
    { ClosingGuard guard(m_closingAll);
      closeAllDocDocks();
      dock = createTab(doc);
    }
    rebuildWorkspaceModel();
    addRecentFile(filePath);

    int classCount = 0;
    for (const auto& n : doc->tree.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) classCount++;
    QString loadedMsg = QStringLiteral("Loaded %1 (%2 classes, %3 nodes)")
        .arg(QFileInfo(filePath).fileName()).arg(classCount).arg(nodeCount);
    if (doc->m_loadOverlapCount > 0) {
        // Tools → Validate Project (Ctrl+Shift+V) opens the dialog that
        // enumerates each pair — the load path only owns the summary.
        setAppStatus(loadedMsg,
            QStringLiteral(" — %1 sibling overlap%2 (Ctrl+Shift+V to review)")
                .arg(doc->m_loadOverlapCount)
                .arg(doc->m_loadOverlapCount == 1 ? "" : "s"));
    } else {
        setAppStatus(loadedMsg);
    }
    endProgress();

    return dock;
}

bool MainWindow::project_save(QDockWidget* dock, bool saveAs) {
    if (!dock) dock = m_activeDocDock;
    if (!dock || !m_tabs.contains(dock)) return false;
    auto& tab = m_tabs[dock];

    QString savedPath;
    const NodeTree* view = tab.ctrl->isInstanceView() ? &tab.ctrl->viewTree() : nullptr;
    if (saveAs || tab.doc->filePath.isEmpty()) {
        QString path = QFileDialog::getSaveFileName(this,
            "Save Definition", {}, "RC (*.rcx);;JSON (*.json)");
        if (path.isEmpty()) return false;
        if (!tab.doc->save(path, view)) return false;
        addRecentFile(path);
        savedPath = path;
    } else {
        if (!tab.doc->save(tab.doc->filePath, view)) return false;
        addRecentFile(tab.doc->filePath);
        savedPath = tab.doc->filePath;
    }
    // Drop the autosave shadow on a real save — the user's explicit save
    // is the authoritative version, no need to keep the recovery copy.
    if (!savedPath.isEmpty())
        QFile::remove(savedPath + QStringLiteral(".autosave"));
    updateWindowTitle();
    rebuildWorkspaceModel();
    return true;
}

void MainWindow::project_close(QDockWidget* dock) {
    if (!dock) dock = m_activeDocDock;
    if (!dock) return;
    dock->close();
}

void MainWindow::closeAllDocDocks() {
    // Take a copy since closing modifies m_docDocks via destroyed signal
    auto docks = m_docDocks;
    for (auto* dock : docks)
        dock->close();
}

QVector<MainWindow::ReferenceHit>
MainWindow::findReferences(const QString& targetTypeName,
                            uint64_t targetStructId) const {
    QVector<ReferenceHit> hits;
    QSet<RcxDocument*> scannedDocs;
    for (auto it = m_tabs.constBegin(); it != m_tabs.constEnd(); ++it) {
        RcxDocument* doc = it.value().doc;
        // Dedup by document — multiple tabs can share a doc, and we only
        // want to walk each unique tree once.
        if (scannedDocs.contains(doc)) continue;
        scannedDocs.insert(doc);

        const auto& tree = doc->tree;
        for (const Node& n : tree.nodes) {
            const bool idMatch   = (targetStructId != 0 && n.refId == targetStructId);
            const bool nameMatch = !targetTypeName.isEmpty()
                                   && n.structTypeName == targetTypeName;
            if (!idMatch && !nameMatch) continue;

            // Walk up to the root-level struct for a human-readable owner.
            QString ownerType;
            uint64_t cur = n.parentId;
            QSet<uint64_t> visited;
            while (cur != 0 && !visited.contains(cur)) {
                visited.insert(cur);
                int pi = tree.indexOfId(cur);
                if (pi < 0) break;
                const Node& p = tree.nodes[pi];
                if (p.parentId == 0) {
                    ownerType = p.structTypeName.isEmpty() ? p.name : p.structTypeName;
                    break;
                }
                cur = p.parentId;
            }

            ReferenceHit h;
            h.ownerDock   = it.key();
            h.nodeId      = n.id;
            h.ownerType   = ownerType;
            h.fieldName   = n.name;
            h.fieldOffset = n.offset;
            hits.append(h);
        }
    }
    return hits;
}

void MainWindow::showValidateDialog() {
    // Walks every open document's tree and reports sibling overlaps —
    // the most common bug a user introduces by editing offsets manually.
    // (NodeTree::validate's orphans/cycles/duplicates auto-repair on
    // load, so they don't need a dialog.) Pulls overlaps from EVERY
    // open doc, not just the active tab, so a multi-doc project gets
    // one consolidated review surface.
    struct OverlapRow {
        QDockWidget* dock;
        QString docTitle;
        QString parentName;
        QString aName;
        QString bName;
        int     aOffset;
        int     aSize;
        int     bOffset;
        int     bSize;
        uint64_t aId;
    };
    QVector<OverlapRow> rows;
    QSet<RcxDocument*> seen;
    for (auto it = m_tabs.constBegin(); it != m_tabs.constEnd(); ++it) {
        RcxDocument* doc = it.value().doc;
        if (seen.contains(doc)) continue;
        seen.insert(doc);
        const auto& tree = doc->tree;
        auto pairs = tree.findOverlaps();
        if (pairs.isEmpty()) continue;
        auto fieldSize = [&](const Node& n) {
            return (n.kind == NodeKind::Struct || n.kind == NodeKind::Array)
                ? tree.structSpan(n.id) : n.byteSize();
        };
        for (const auto& p : pairs) {
            int ai = tree.indexOfId(p.aId);
            int bi = tree.indexOfId(p.bId);
            int pi = tree.indexOfId(p.parentId);
            if (ai < 0 || bi < 0) continue;
            OverlapRow row;
            row.dock = it.key();
            row.docTitle = it.key() ? it.key()->windowTitle() : QStringLiteral("?");
            if (pi >= 0) {
                const Node& pn = tree.nodes[pi];
                row.parentName = pn.structTypeName.isEmpty() ? pn.name : pn.structTypeName;
            }
            const Node& a = tree.nodes[ai];
            const Node& b = tree.nodes[bi];
            row.aName = a.name;  row.aOffset = a.offset;  row.aSize = fieldSize(a);
            row.bName = b.name;  row.bOffset = b.offset;  row.bSize = fieldSize(b);
            row.aId = p.aId;
            rows.append(row);
        }
    }

    const auto& t = ThemeManager::instance().current();
    rcx::ThemedDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Validate Project"));
    dlg.resize(620, 420);
    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* header = new QLabel(rows.isEmpty()
        ? QStringLiteral("No sibling overlaps detected.")
        : QStringLiteral("%1 sibling overlap%2 across %3 document%4")
              .arg(rows.size()).arg(rows.size() == 1 ? "" : "s")
              .arg(seen.size()).arg(seen.size() == 1 ? "" : "s"), &dlg);
    header->setStyleSheet(QStringLiteral("color: %1;")
        .arg(rows.isEmpty() ? t.indHintGreen.name() : t.textDim.name()));
    layout->addWidget(header);

    auto* list = new QListWidget(&dlg);
    list->setAlternatingRowColors(false);
    QSettings settings("RC", "RC");
    QFont monoFont(settings.value("font", "JetBrains Mono").toString(), 10);
    monoFont.setFixedPitch(true);
    list->setFont(monoFont);
    list->setStyleSheet(QStringLiteral(
        "QListWidget { background: %1; color: %2; border: 1px solid %3; }"
        "QListWidget::item { padding: 4px 8px; }"
        "QListWidget::item:hover { background: %4; }"
        "QListWidget::item:selected { background: %5; color: %6; }")
        .arg(t.background.name(), t.text.name(), t.border.name(),
             t.hover.name(), t.selected.name(), t.text.name()));
    for (const auto& r : rows) {
        QString text = QStringLiteral("%1 · %2  ↔  %3 @ +0x%4 (size %5)  vs  "
                                       "%6 @ +0x%7 (size %8)")
            .arg(r.docTitle)
            .arg(r.parentName.isEmpty() ? QStringLiteral("(root)") : r.parentName)
            .arg(r.aName)
            .arg(r.aOffset, 0, 16)
            .arg(r.aSize)
            .arg(r.bName)
            .arg(r.bOffset, 0, 16)
            .arg(r.bSize);
        auto* item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(
            reinterpret_cast<quintptr>(r.dock)));
        item->setData(Qt::UserRole + 1, QString::number(r.aId));
        list->addItem(item);
    }
    layout->addWidget(list, 1);

    // Double-click → raise dock, scroll to the first offending field.
    connect(list, &QListWidget::itemActivated, this,
            [this, &dlg](QListWidgetItem* item) {
        if (!item) return;
        auto* dock = reinterpret_cast<QDockWidget*>(
            item->data(Qt::UserRole).value<quintptr>());
        uint64_t nodeId = item->data(Qt::UserRole + 1).toString().toULongLong();
        if (!dock || !m_tabs.contains(dock)) return;
        dock->raise();
        dock->show();
        m_activeDocDock = dock;
        m_tabs[dock].ctrl->scrollToNodeId(nodeId);
        // Re-assert after dlg.accept(): closing the dialog can return focus to
        // the old dock and let focusChanged revert m_activeDocDock.
        QPointer<QDockWidget> dockRef = dock;
        QTimer::singleShot(0, this, [this, dockRef]() {
            if (dockRef && m_tabs.contains(dockRef)) setActiveDocDock(dockRef);
        });
        dlg.accept();
    });

    auto* closeBtn = new rcx::DialogButton(QStringLiteral("Close"),
        rcx::DialogButton::Primary, &dlg);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    auto* btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);
    closeBtn->setDefault(true);
    dlg.exec();
}

void MainWindow::showFindFieldDialog() {
    // Quick navigation: type a substring, see matching fields from every open
    // doc, Enter / double-click jumps to the field. Cheap O(N·M) scan because
    // even 100 docs × 10k fields is 1M strings — fast enough to refilter on
    // every keystroke without async work.
    struct Hit {
        QDockWidget* dock;
        QString      docTitle;
        QString      path;         // dot-path including container ancestry
        QString      typeName;
        int          offset;
        uint64_t     nodeId;
    };
    QVector<Hit> all;
    QSet<RcxDocument*> seen;
    for (auto it = m_tabs.constBegin(); it != m_tabs.constEnd(); ++it) {
        RcxDocument* doc = it.value().doc;
        if (seen.contains(doc)) continue;
        seen.insert(doc);
        const auto& tree = doc->tree;
        for (int i = 0; i < tree.nodes.size(); ++i) {
            const Node& n = tree.nodes[i];
            // Skip the synthetic root structs themselves — they show up
            // as their named children's path prefix anyway.
            if (n.parentId == 0) continue;
            // Anonymous fields are useless to navigate to via name search.
            if (n.name.isEmpty()) continue;
            Hit h;
            h.dock     = it.key();
            h.docTitle = it.key() ? it.key()->windowTitle() : QStringLiteral("?");
            h.path     = tree.fieldPath(n.id);
            h.typeName = doc->resolveTypeName(n.kind);
            h.offset   = n.offset;
            h.nodeId   = n.id;
            all.append(h);
        }
    }

    const auto& t = ThemeManager::instance().current();
    rcx::ThemedDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Find Field"));
    dlg.resize(620, 480);
    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // Last query — sticky across Find Field invocations within the session.
    // Reopening Ctrl+F right after closing pre-fills the previous filter
    // with selectAll so a single keystroke either continues the search or
    // overwrites cleanly. Function-local static is intentional: this state
    // is scoped to this dialog and shouldn't outlive a process restart.
    static QString s_lastQuery;

    auto* search = new QLineEdit(&dlg);
    search->setPlaceholderText(QStringLiteral("Type to filter %1 field%2…")
        .arg(all.size()).arg(all.size() == 1 ? "" : "s"));
    search->setClearButtonEnabled(true);
    if (!s_lastQuery.isEmpty()) {
        search->setText(s_lastQuery);
        search->selectAll();
    }
    search->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3; padding: 6px 8px; }"
        "QLineEdit:focus { border: 1px solid %4; }")
        .arg(t.background.name(), t.text.name(), t.border.name(),
             t.borderFocused.name()));
    layout->addWidget(search);

    QSettings settings("RC", "RC");
    QFont monoFont(settings.value("font", "JetBrains Mono").toString(), 10);
    monoFont.setFixedPitch(true);

    auto* list = new QListWidget(&dlg);
    list->setAlternatingRowColors(false);
    list->setFont(monoFont);
    list->setStyleSheet(QStringLiteral(
        "QListWidget { background: %1; color: %2; border: 1px solid %3; }"
        "QListWidget::item { padding: 4px 8px; }"
        "QListWidget::item:hover { background: %4; }"
        "QListWidget::item:selected { background: %5; color: %6; }")
        .arg(t.background.name(), t.text.name(), t.border.name(),
             t.hover.name(), t.selected.name(), t.text.name()));
    layout->addWidget(list, 1);

    auto refill = [list, &all, &t](const QString& q) {
        list->clear();
        QString needle = q.trimmed().toLower();
        int shown = 0;
        for (const auto& h : all) {
            if (!needle.isEmpty() && !h.path.toLower().contains(needle)) continue;
            QString text = QStringLiteral("%1  %2  +0x%3  [%4]")
                .arg(h.path, -38, QChar(' '))
                .arg(h.typeName, -14, QChar(' '))
                .arg(h.offset, 0, 16)
                .arg(h.docTitle);
            auto* item = new QListWidgetItem(text);
            item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(
                reinterpret_cast<quintptr>(h.dock)));
            item->setData(Qt::UserRole + 1, QString::number(h.nodeId));
            list->addItem(item);
            if (++shown >= 500) break;  // Cap to avoid pathological repaint cost.
        }
        if (list->count() > 0) list->setCurrentRow(0);
    };
    refill(search->text());

    QObject::connect(search, &QLineEdit::textChanged, &dlg, refill);

    // Enter in the search box activates the current row. Down/Up are handled
    // by the dialog-level shortcuts below so the user can drive the whole
    // dialog from the keyboard without ever leaving the line edit.
    QObject::connect(search, &QLineEdit::returnPressed, &dlg, [list, &dlg]() {
        QListWidgetItem* item = list->currentItem();
        if (item) emit list->itemActivated(item);
        else dlg.reject();
    });
    auto moveSel = [list](int delta) {
        int r = list->currentRow() + delta;
        if (r < 0) r = 0;
        if (r >= list->count()) r = list->count() - 1;
        if (r >= 0) list->setCurrentRow(r);
    };
    auto* sDown = new QShortcut(QKeySequence(Qt::Key_Down), &dlg);
    QObject::connect(sDown, &QShortcut::activated, &dlg,
                     [moveSel]() { moveSel(+1); });
    auto* sUp = new QShortcut(QKeySequence(Qt::Key_Up), &dlg);
    QObject::connect(sUp, &QShortcut::activated, &dlg,
                     [moveSel]() { moveSel(-1); });

    auto activate = [this, &dlg](QListWidgetItem* item) {
        if (!item) return;
        auto* dock = reinterpret_cast<QDockWidget*>(
            item->data(Qt::UserRole).value<quintptr>());
        uint64_t nodeId = item->data(Qt::UserRole + 1).toString().toULongLong();
        if (!dock || !m_tabs.contains(dock)) return;
        dock->raise();
        dock->show();
        m_activeDocDock = dock;
        m_tabs[dock].ctrl->scrollToNodeId(nodeId);
        QPointer<QDockWidget> dockRef = dock;
        QTimer::singleShot(0, this, [this, dockRef]() {
            if (dockRef && m_tabs.contains(dockRef)) setActiveDocDock(dockRef);
        });
        dlg.accept();
    };
    connect(list, &QListWidget::itemActivated, this, activate);

    search->setFocus();
    dlg.exec();
    s_lastQuery = search->text();
}

void MainWindow::showFindReferences(const QString& targetTypeName,
                                     uint64_t targetStructId) {
    auto hits = findReferences(targetTypeName, targetStructId);

    // ThemedDialog + DialogButton — was a raw QDialog + QDialogButtonBox
    // which inherited Qt's default (light) palette and OS-themed buttons,
    // out of style with every other dialog in the app.
    const auto& t = ThemeManager::instance().current();
    rcx::ThemedDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("References to %1")
                       .arg(targetTypeName.isEmpty()
                            ? QStringLiteral("(unnamed)") : targetTypeName));
    dlg.resize(560, 400);
    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* header = new QLabel(QStringLiteral("%1 reference%2")
                               .arg(hits.size())
                               .arg(hits.size() == 1 ? "" : "s"), &dlg);
    header->setStyleSheet(QStringLiteral("color: %1;").arg(t.textDim.name()));
    layout->addWidget(header);

    auto* list = new QListWidget(&dlg);
    list->setAlternatingRowColors(false);  // theme palette handles row contrast
    list->setStyleSheet(QStringLiteral(
        "QListWidget { background: %1; color: %2; border: 1px solid %3; }"
        "QListWidget::item { padding: 3px 6px; }"
        "QListWidget::item:hover { background: %4; }"
        "QListWidget::item:selected { background: %5; color: %6; }")
        .arg(t.background.name(), t.text.name(), t.border.name(),
             t.hover.name(), t.selected.name(), t.text.name()));
    for (const auto& h : hits) {
        QString text = QStringLiteral("%1 · %2.%3  (+0x%4)")
            .arg(h.ownerDock ? h.ownerDock->windowTitle() : QStringLiteral("?"),
                 h.ownerType.isEmpty() ? QStringLiteral("?") : h.ownerType,
                 h.fieldName)
            .arg(h.fieldOffset, 0, 16);
        auto* item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(
            reinterpret_cast<quintptr>(h.ownerDock)));
        item->setData(Qt::UserRole + 1, QString::number(h.nodeId));
        list->addItem(item);
    }
    layout->addWidget(list, 1);

    // Double-click → raise that dock, scroll to the node.
    connect(list, &QListWidget::itemActivated, this,
            [this, &dlg](QListWidgetItem* item) {
        if (!item) return;
        auto* dock = reinterpret_cast<QDockWidget*>(
            item->data(Qt::UserRole).value<quintptr>());
        uint64_t nodeId = item->data(Qt::UserRole + 1).toString().toULongLong();
        if (!dock || !m_tabs.contains(dock)) return;
        dock->raise();
        dock->show();
        m_activeDocDock = dock;
        m_tabs[dock].ctrl->scrollToNodeId(nodeId);
        QPointer<QDockWidget> dockRef = dock;
        QTimer::singleShot(0, this, [this, dockRef]() {
            if (dockRef && m_tabs.contains(dockRef)) setActiveDocDock(dockRef);
        });
        dlg.accept();
    });

    auto* closeBtn = new rcx::DialogButton(QStringLiteral("Close"),
        rcx::DialogButton::Primary, &dlg);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    auto* btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);
    closeBtn->setDefault(true);

    dlg.exec();
}

void MainWindow::applyLayoutPreset(int preset) {
    // Two-mode toggle: workspace on (Layout_Workspace) or off.
    // Other docks (symbols, scanner) keep whatever state the user set via
    // their own toggles — we don't touch them here.
    if (!m_workspaceDock) return;
    const bool showWorkspace = (preset == Layout_Workspace);
    m_workspaceClosed = !showWorkspace;   // drives the collapsed-rail handle
    m_workspaceDock->setVisible(showWorkspace);
    // setVisible() on a dock that is ALREADY in that state (Project deselected
    // inside a tab group is already hidden) emits nothing, so the rail would
    // keep the stale visibility the flag just invalidated. Reconcile directly.
    if (m_workspaceRailDock)
        m_workspaceRailDock->setVisible(
            rcx::railVisibleFor(m_workspaceDock->isVisible(), m_workspaceClosed));

    // Re-install tab bar buttons — a newly revealed tab bar needs them.
    reconcileDockTabBars();

    setAppStatus(showWorkspace ? QStringLiteral("Workspace shown")
                               : QStringLiteral("Workspace hidden"));
}


// ── Workspace Dock ──

// Right-edge hairline on the Project dock — shown ONLY while it sits docked
// in the main window (a floating dock has its own window chrome instead).
// Spans the header (rcx::DockHeader) and the content panel (WorkspacePanel);
// both paint a device-exact 1px column so it stays a single crisp line at
// fractional display scaling. The content layout frees its right-most
// logical px while the line is on so the search box / tree don't cover it.
void MainWindow::updateWorkspaceDockEdge() {
    if (!m_workspaceDock) return;
    const bool docked = !m_workspaceDock->isFloating();
    const auto& t = ThemeManager::instance().current();
    // containerBorderColor, not t.border: this seam touches the document
    // column, and the editor container beside it paints the same tone.
    const QColor edge = docked ? rcx::containerBorderColor(t) : QColor();
    if (m_workspaceHeader) m_workspaceHeader->setBorderRight(edge);
    if (auto* cont = m_workspaceDock->findChild<WorkspacePanel*>("workspaceContainer")) {
        cont->setRightLine(edge);
        if (auto* lay = cont->layout()) {
            QMargins m = lay->contentsMargins();
            m.setRight(docked ? 1 : 0);
            lay->setContentsMargins(m);
        }
    }
}

// One dock header for Project, Scanner, Symbols and Bookmarks: builds the
// shared rcx::DockHeader, installs it as the title bar, and wires the floating
// chrome (BorderOverlay + ResizeGrip + DockBorderFilter) that the scanner and
// symbols docks each carried their own copy of. Every dock therefore shows the
// same height, ground, title tone, close button AND float outline. The caller
// connects the close button, because "closed" means something slightly
// different per dock (the Project dock also arms the collapsed-rail flag).
rcx::DockHeader* MainWindow::makeDockHeader(QDockWidget* dock, const QString& title) {
    auto* header = new rcx::DockHeader(title, dock);
    dock->setTitleBarWidget(header);

    auto* border = new BorderOverlay(dock);
    border->color = ThemeManager::instance().current().borderFocused;
    border->hide();
    auto* grip = new ResizeGrip(dock);
    grip->hide();
    connect(dock, &QDockWidget::topLevelChanged, this,
            [dock, border, grip](bool floating) {
        if (floating) {
            border->setGeometry(0, 0, dock->width(), dock->height());
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
    dock->installEventFilter(new DockBorderFilter(border, grip, dock));
    return header;
}

void MainWindow::createWorkspaceDock() {
    m_workspaceDock = new QDockWidget("Project", this);
    m_workspaceDock->setObjectName("WorkspaceDock");
    m_workspaceDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_workspaceDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    // Shared dock header (rcx::DockHeader) — identical height, ground, title
    // tone and close button on all four docks, and Qt still handles
    // drag/dock natively because the header never consumes mouse events.
    const auto& t = ThemeManager::instance().current();
    m_workspaceHeader = makeDockHeader(m_workspaceDock, QStringLiteral("Project"));
    m_dockTitleLabel  = m_workspaceHeader->titleLabel();
    connect(m_workspaceHeader->closeButton(), &QToolButton::clicked, this, [this] {
        m_workspaceClosed = true;          // an explicit user close → show the rail
        m_workspaceDock->close();
    });

    // ...and every OTHER route to the same place. A middle-click on the Project
    // tab, the tab's own x button and the tab context menu's Close all call
    // QDockWidget::close() directly, none of them knowing about the rail flag —
    // so each one used to hide the dock and leave no handle to get it back.
    // They all raise a QEvent::Close on the dock first, which is the one thing
    // they have in common. (Hiding via View > Project or a layout preset is a
    // Hide, not a Close, so those two still set the flag themselves.)
    {
        class WorkspaceCloseWatcher : public QObject {
        public:
            WorkspaceCloseWatcher(bool* flag, QObject* parent)
                : QObject(parent), m_flag(flag) {}
        protected:
            bool eventFilter(QObject* o, QEvent* e) override {
                if (e->type() == QEvent::Close) *m_flag = true;
                return QObject::eventFilter(o, e);
            }
            bool* m_flag;
        };
        m_workspaceDock->installEventFilter(
            new WorkspaceCloseWatcher(&m_workspaceClosed, m_workspaceDock));
    }

    // Content container: search + tree. Custom-painted (editor-paper bg +
    // optional device-exact right edge) instead of QSS so the docked-only
    // right border isn't a stylesheet/custom-paint fight.
    auto* dockContainer = new WorkspacePanel(rcx::editorPaperColor(t), m_workspaceDock);
    dockContainer->setObjectName(QStringLiteral("workspaceContainer"));
    auto* dockLayout = new QVBoxLayout(dockContainer);
    dockLayout->setContentsMargins(0, 0, 0, 0);
    dockLayout->setSpacing(0);

    // Shared panel filter box — paper ground, no box at rest, ONE device-exact
    // hairline underneath (which turns borderFocused on focus). The two
    // HairlineSeparator widgets that used to frame a bordered QLineEdit are
    // gone: the header owns the line above, the field owns the line below.
    m_workspaceSearch = new rcx::PanelSearchField(
        QStringLiteral(":/vsicons/filter.svg"),
        QStringLiteral("Filter types…"), dockContainer);
    dockLayout->addWidget(m_workspaceSearch);

    auto* wsTree = new EmptyHintTreeView(dockContainer);
    wsTree->placeholder = QStringLiteral("No types yet");
    wsTree->hint        = QStringLiteral("File ▸ New Class, or import a PDB");
    m_workspaceTree = wsTree;
    m_workspaceModel = new QStandardItemModel(this);
    m_workspaceModel->setHorizontalHeaderLabels({"Name"});

    // Inline rename commit. Right-click → Rename sets EditRole to the plain
    // name, flips m_wsRenaming, and opens the inline editor; on commit the
    // item's EditRole changes and we push a ChangeStructTypeName undo command.
    // Gated by m_wsRenaming so programmatic model edits don't fire it.
    connect(m_workspaceModel, &QStandardItemModel::itemChanged, this,
            [this](QStandardItem* item) {
        if (!m_wsRenaming || !item) return;
        m_wsRenaming = false;
        auto idVar = item->data(Qt::UserRole + 1);
        uint64_t sid = idVar.isValid() ? idVar.toULongLong() : 0;
        auto subVar = item->data(Qt::UserRole);
        auto* dk = subVar.isValid()
            ? static_cast<QDockWidget*>(subVar.value<void*>()) : nullptr;
        if (sid == 0 || !dk || !m_tabs.contains(dk)) { rebuildWorkspaceModel(); return; }
        auto& tab = m_tabs[dk];
        int ni = tab.doc->tree.indexOfId(sid);
        if (ni < 0) { rebuildWorkspaceModel(); return; }
        QString oldName = tab.doc->tree.nodes[ni].structTypeName.isEmpty()
            ? tab.doc->tree.nodes[ni].name : tab.doc->tree.nodes[ni].structTypeName;
        QString newName = item->data(Qt::EditRole).toString().trimmed();
        if (newName.isEmpty() || newName == oldName) { rebuildWorkspaceModel(); return; }
        tab.doc->undoStack.push(new rcx::RcxCommand(tab.ctrl,
            rcx::cmd::ChangeStructTypeName{sid, oldName, newName}));
        tab.ctrl->refresh();
        if (dk->windowTitle() == oldName) dk->setWindowTitle(newName);
        rebuildWorkspaceModel();
    });

    m_workspaceProxy = new rcx::WorkspaceProxyModel(this);
    m_workspaceProxy->setSourceModel(m_workspaceModel);
    m_workspaceProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_workspaceProxy->setRecursiveFilteringEnabled(true);

    m_workspaceTree->setModel(m_workspaceProxy);
    m_workspaceTree->setHeaderHidden(true);
    m_workspaceTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_workspaceTree->setExpandsOnDoubleClick(false);
    m_workspaceTree->setMouseTracking(true);
    m_workspaceTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    {
        QSettings s("RC", "RC");
        QFont f(s.value("font", "JetBrains Mono").toString(), 10);
        f.setFixedPitch(true);
        m_workspaceTree->setFont(f);
    }

    m_workspaceSearchTimer = new QTimer(this);
    m_workspaceSearchTimer->setSingleShot(true);
    m_workspaceSearchTimer->setInterval(150);
    connect(m_workspaceSearchTimer, &QTimer::timeout, this, [this]() {
        QString text = m_workspaceSearch->text();
        static_cast<rcx::WorkspaceProxyModel*>(m_workspaceProxy)->setHasFilter(!text.isEmpty());
        m_workspaceProxy->setFilterFixedString(text);
        if (!text.isEmpty())
            m_workspaceTree->expandAll();
        else
            m_workspaceTree->collapseAll();
    });
    connect(m_workspaceSearch, &QLineEdit::textChanged, this, [this]() {
        m_workspaceSearchTimer->start();
    });

    // Custom delegate for rich text rendering (name bright, metadata dim)
    {
        const auto& t = ThemeManager::instance().current();
        m_workspaceDelegate = new rcx::WorkspaceDelegate(m_workspaceTree);
        m_workspaceDelegate->setThemeColors(t);
        m_workspaceTree->setItemDelegate(m_workspaceDelegate);

        // Construction defaults must match applyTheme()'s runtime values
        // (QPalette::Base + the editor-"paper" surface) so the tree is the
        // editor's dark paper from the FIRST paint — not the lighter chrome
        // background, which read as a "light" panel until a theme reapply.
        const QColor wsPaper0 = rcx::editorPaperColor(t);
        QPalette tp = m_workspaceTree->palette();
        tp.setColor(QPalette::Text, t.textDim);
        tp.setColor(QPalette::Highlight, t.selected);
        tp.setColor(QPalette::HighlightedText, t.text);
        tp.setColor(QPalette::Base, wsPaper0);
        m_workspaceTree->setPalette(tp);

        m_workspaceTree->setStyleSheet(QStringLiteral(
            "QTreeView { background: %1; border: none; padding-left: 4px; }"
            "QTreeView::branch:has-children:closed { image: url(:/vsicons/chevron-right.svg); }"
            "QTreeView::branch:has-children:open { image: url(:/vsicons/chevron-down.svg); }"
            "QTreeView::branch { width: 12px; }"
            "QAbstractScrollArea::corner { background: %1; border: none; }"
            "QHeaderView { background: %1; border: none; }"
            "QHeaderView::section { background: %1; border: none; }")
            .arg(wsPaper0.name()));
    }

    m_workspaceTree->setIndentation(12);
    dockLayout->addWidget(m_workspaceTree);

    m_workspaceTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_workspaceTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QModelIndex clickedIndex = m_workspaceTree->indexAt(pos);

        // Right-click on empty area → New Class / New Struct / New Enum
        if (!clickedIndex.isValid()) {
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

        // Skip section headers (non-interactive)
        if (!clickedIndex.data(rcx::RoleSectionHeader).toString().isEmpty()) return;

        // If right-clicked item is not in current selection, select only it
        auto* sel = m_workspaceTree->selectionModel();
        if (!sel->isSelected(clickedIndex))
            sel->select(clickedIndex,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

        // Gather all selected ROOT items (children are not independently actionable)
        struct SelItem {
            uint64_t structId;
            QDockWidget* dock;
            int nodeIdx;
            QString keyword;
            QString typeName;
        };
        QVector<SelItem> items;

        for (const auto& idx : sel->selectedIndexes()) {
            if (idx.parent().isValid()) continue;  // skip children
            auto idVar = idx.data(Qt::UserRole + 1);
            uint64_t sid = idVar.isValid() ? idVar.toULongLong() : 0;
            if (sid == 0) continue;
            auto subVar = idx.data(Qt::UserRole);
            if (!subVar.isValid()) continue;
            auto* dk = static_cast<QDockWidget*>(subVar.value<void*>());
            if (!dk || !m_tabs.contains(dk)) continue;
            int ni = m_tabs[dk].doc->tree.indexOfId(sid);
            if (ni < 0) continue;
            const auto& nd = m_tabs[dk].doc->tree.nodes[ni];
            QString tn = nd.structTypeName.isEmpty() ? nd.name : nd.structTypeName;
            if (tn.isEmpty()) tn = QStringLiteral("(unnamed)");
            items.push_back(SelItem{sid, dk, ni, nd.resolvedClassKeyword(), tn});
        }
        if (items.isEmpty()) return;

        QMenu menu;

        // Navigation actions (single selection only)
        QAction* actOpenCurrent = nullptr;
        QAction* actOpenNew = nullptr;
        QAction* actDuplicate = nullptr;
        QAction* actRename = nullptr;
        if (items.size() == 1) {
            actOpenCurrent = menu.addAction("Open in Current Tab");
            actOpenNew     = menu.addAction("Open in New Tab");
            actDuplicate   = menu.addAction("Duplicate");
            actRename      = menu.addAction("Rename");
            menu.addSeparator();
        }

        // Convert: only for single selection, class↔struct (not enum)
        QAction* actConvert = nullptr;
        if (items.size() == 1) {
            if (items[0].keyword == QStringLiteral("class"))
                actConvert = menu.addAction("Convert to Struct");
            else if (items[0].keyword == QStringLiteral("struct"))
                actConvert = menu.addAction("Convert to Class");
        }

        // Find References — single-selection only. Scans every open doc for
        // Node.refId == target or Node.structTypeName == target.typeName and
        // opens a results dialog. Inverse of the "rename struct → fields
        // auto-update" flow; useful when untangling a cross-class refactor.
        // Inked for the theme, like every other menu icon: a raw :/vsicons SVG
        // keeps VS Code's #C5C5C5 and washes out beside black item text on a
        // light theme. Rebuilt per right-click, so no retint pass is needed.
        const qreal mdpr = devicePixelRatioF();
        auto menuIcon = [mdpr](const QString& path) {
            return rcx::themedVsIcon(path, ThemeManager::instance().current().text, 16, mdpr);
        };
        // Delete is the one row here that destroys work, so it wears the
        // destructive red — theme.markerPtr, the token the dialogs already use
        // for a destructive button. The icon is tinted here; the LABEL is
        // painted by MenuBarStyle off the rcxDestructive property, because a
        // QStyleOptionMenuItem carries no QAction to ask.
        auto destructiveIcon = [mdpr](const QString& path) {
            return rcx::themedVsIcon(path, ThemeManager::instance().current().markerPtr, 16, mdpr);
        };

        QAction* actFindRefs = nullptr;
        if (items.size() == 1) {
            actFindRefs = menu.addAction(menuIcon(":/vsicons/search.svg"),
                                          QStringLiteral("Find References"));
        }

        // Pin/Unpin
        bool allPinned = true;
        for (const auto& item : items)
            if (!m_pinnedIds.contains(item.structId)) { allPinned = false; break; }
        auto* actPin = menu.addAction(
            menuIcon(QStringLiteral(":/vsicons/pin.svg")),
            allPinned ? QStringLiteral("Unpin") : QStringLiteral("Pin"));

        menu.addSeparator();

        // Delete: works for single or multi
        QString delLabel = items.size() == 1
            ? QStringLiteral("Delete")
            : QStringLiteral("Delete %1 items").arg(items.size());
        auto* actDelete = menu.addAction(destructiveIcon(":/vsicons/remove.svg"), delLabel);
        actDelete->setProperty("rcxDestructive", true);

        QAction* chosen = menu.exec(m_workspaceTree->viewport()->mapToGlobal(pos));

        if (chosen == actDelete) {
            // Collect reference info across all selected items
            QStringList refDetails;
            QStringList typeNames;
            for (const auto& item : items) {
                typeNames << item.typeName;
                if (!m_tabs.contains(item.dock)) continue;
                for (const auto& n : m_tabs[item.dock].doc->tree.nodes) {
                    if (n.refId == item.structId) {
                        QString ownerName;
                        uint64_t pid = n.parentId;
                        while (pid != 0) {
                            int pi = m_tabs[item.dock].doc->tree.indexOfId(pid);
                            if (pi < 0) break;
                            if (m_tabs[item.dock].doc->tree.nodes[pi].parentId == 0) {
                                const auto& pn = m_tabs[item.dock].doc->tree.nodes[pi];
                                ownerName = pn.structTypeName.isEmpty()
                                    ? pn.name : pn.structTypeName;
                                break;
                            }
                            pid = m_tabs[item.dock].doc->tree.nodes[pi].parentId;
                        }
                        QString fieldDesc = ownerName.isEmpty()
                            ? n.name
                            : QStringLiteral("%1::%2").arg(ownerName, n.name);
                        refDetails << QStringLiteral("  \u2022 %1 (%2)")
                            .arg(fieldDesc, kindToString(n.kind));
                    }
                }
            }

            QString msg;
            if (items.size() == 1) {
                msg = refDetails.isEmpty()
                    ? QStringLiteral("Delete '%1'?").arg(typeNames[0])
                    : QStringLiteral("Delete '%1'?\n\n"
                        "The following %2 field(s) reference this type "
                        "and will become untyped (void):\n\n%3")
                        .arg(typeNames[0])
                        .arg(refDetails.size())
                        .arg(refDetails.join('\n'));
            } else {
                msg = QStringLiteral("Delete %1 types?\n\n%2")
                    .arg(items.size())
                    .arg(typeNames.join(QStringLiteral(", ")));
                if (!refDetails.isEmpty())
                    msg += QStringLiteral("\n\n%1 field(s) reference these types "
                        "and will become untyped (void):\n\n%2")
                        .arg(refDetails.size())
                        .arg(refDetails.join('\n'));
            }

            const QString deleteLabel = items.size() == 1
                ? QStringLiteral("Delete type")
                : QStringLiteral("Delete %1 types").arg(items.size());
            if (!ThemedMessageBox::confirm(this,
                    QStringLiteral("Delete Type"),
                    msg, deleteLabel,
                    QStringLiteral("Cancel"),
                    /*destructive=*/true)) return;

            // Group deletes by controller for single undo macro per document
            QHash<RcxController*, QVector<uint64_t>> byCtrl;
            for (const auto& item : items) {
                if (!m_tabs.contains(item.dock)) continue;
                byCtrl[m_tabs[item.dock].ctrl].append(item.structId);
            }
            for (auto it = byCtrl.begin(); it != byCtrl.end(); ++it) {
                auto* ctrl = it.key();
                const auto& ids = it.value();
                if (ids.size() == 1) {
                    ctrl->deleteRootStruct(ids[0]);
                } else {
                    // Wrap multiple deletes in a single undo macro
                    ctrl->document()->undoStack.beginMacro(
                        QStringLiteral("Delete %1 types").arg(ids.size()));
                    for (uint64_t sid : ids)
                        ctrl->deleteRootStruct(sid);
                    ctrl->document()->undoStack.endMacro();
                }
            }
            rebuildWorkspaceModel();

        } else if (chosen && chosen == actOpenCurrent && items.size() == 1) {
            // Open in current (active) tab — set viewRootId on active editor
            const auto& item = items[0];
            if (!m_tabs.contains(item.dock)) return;
            RcxDocument* doc = m_tabs[item.dock].doc;
            int ni = doc->tree.indexOfId(item.structId);
            if (ni < 0) return;
            // Use the active tab if it shares the same document, else use owner
            QDockWidget* targetDock = item.dock;
            if (m_activeDocDock && m_tabs.contains(m_activeDocDock)
                && m_tabs[m_activeDocDock].doc == doc)
                targetDock = m_activeDocDock;

            auto& tab = m_tabs[targetDock];
            tab.ctrl->setCollapsed(item.structId, false);
            tab.ctrl->setViewRootId(item.structId);
            tab.ctrl->refresh();
            targetDock->raise();
            targetDock->show();
            setActiveDocDock(targetDock);
            QString structName = doc->tree.nodes[ni].structTypeName.isEmpty()
                ? doc->tree.nodes[ni].name
                : doc->tree.nodes[ni].structTypeName;
            if (!structName.isEmpty())
                targetDock->setWindowTitle(tabTitle(tab));
            rebuildWorkspaceModel();

        } else if (chosen && chosen == actOpenNew && items.size() == 1) {
            // Open in a brand new tab (sharing the same document)
            const auto& item = items[0];
            if (!m_tabs.contains(item.dock)) return;
            RcxDocument* doc = m_tabs[item.dock].doc;
            int ni = doc->tree.indexOfId(item.structId);
            if (ni < 0) return;
            doc->tree.nodes[ni].collapsed = false;
            auto* newDock = createTab(doc);
            m_tabs[newDock].ctrl->setViewRootId(item.structId);
            m_tabs[newDock].ctrl->refresh();
            QString structName = doc->tree.nodes[ni].structTypeName.isEmpty()
                ? doc->tree.nodes[ni].name
                : doc->tree.nodes[ni].structTypeName;
            if (!structName.isEmpty())
                newDock->setWindowTitle(structName);
            rebuildWorkspaceModel();

        } else if (chosen && chosen == actRename && items.size() == 1) {
            // Inline rename. The display is rich ("Name — N"), so set EditRole
            // to the plain name first (this emits itemChanged while m_wsRenaming
            // is still false → ignored), then arm the guard and open the inline
            // editor. The model's itemChanged handler commits on Enter.
            const auto& item = items[0];
            QModelIndex srcIdx = m_workspaceProxy->mapToSource(clickedIndex);
            if (auto* it = m_workspaceModel->itemFromIndex(srcIdx)) {
                it->setData(item.typeName, Qt::EditRole);
                m_wsRenaming = true;
                m_workspaceTree->edit(clickedIndex);
            }

        } else if (chosen && chosen == actDuplicate && items.size() == 1) {
            // Duplicate: deep-copy the struct as a new root with a unique name
            const auto& item = items[0];
            if (!m_tabs.contains(item.dock)) return;
            auto& tab = m_tabs[item.dock];
            auto& tree = tab.doc->tree;

            // Generate unique name
            QString baseName = item.typeName + QStringLiteral("_copy");
            QString newName = baseName;
            int counter = 1;
            QSet<QString> existing;
            for (const auto& n : tree.nodes)
                if (n.kind == rcx::NodeKind::Struct && !n.structTypeName.isEmpty())
                    existing.insert(n.structTypeName);
            while (existing.contains(newName))
                newName = baseName + QString::number(counter++);

            tab.ctrl->setSuppressRefresh(true);
            tab.doc->undoStack.beginMacro(QStringLiteral("Duplicate ") + item.typeName);

            // Clone root node (re-lookup by ID since menu.exec() may have invalidated index)
            int ni = tree.indexOfId(item.structId);
            if (ni < 0) return;
            rcx::Node root = tree.nodes[ni];
            root.id = tree.reserveId();
            root.structTypeName = newName;
            root.name = newName;
            root.parentId = 0;
            tab.doc->undoStack.push(new rcx::RcxCommand(tab.ctrl,
                rcx::cmd::Insert{root}));

            // Clone children (re-lookup after insert since indices may shift)
            QVector<int> children = tree.childrenOf(item.structId);
            for (int ci : children) {
                rcx::Node child = tree.nodes[ci];
                child.id = tree.reserveId();
                child.parentId = root.id;
                child.refId = 0;  // don't copy pointer refs
                tab.doc->undoStack.push(new rcx::RcxCommand(tab.ctrl,
                    rcx::cmd::Insert{child}));
            }

            tab.doc->undoStack.endMacro();
            tab.ctrl->setSuppressRefresh(false);
            tab.ctrl->refresh();
            rebuildWorkspaceModel();

        } else if (chosen && chosen == actConvert && items.size() == 1) {
            const auto& item = items[0];
            if (!m_tabs.contains(item.dock)) return;
            auto& tab = m_tabs[item.dock];
            int ni = tab.doc->tree.indexOfId(item.structId);
            if (ni < 0) return;
            QString newKw = item.keyword == QStringLiteral("class")
                ? QStringLiteral("struct") : QStringLiteral("class");
            tab.doc->undoStack.push(new rcx::RcxCommand(tab.ctrl,
                rcx::cmd::ChangeClassKeyword{item.structId, item.keyword, newKw}));
            // Sync all dock titles that share this document
            for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it)
                if (it->doc == tab.doc)
                    it.key()->setWindowTitle(tabTitle(*it));
            rebuildWorkspaceModel();

        } else if (chosen && chosen == actFindRefs && items.size() == 1) {
            showFindReferences(items[0].typeName, items[0].structId);

        } else if (chosen && chosen == actPin) {
            for (const auto& item : items) {
                if (allPinned)
                    m_pinnedIds.remove(item.structId);
                else
                    m_pinnedIds.insert(item.structId);
            }
            // Full rebuild to reorder pinned items to top
            m_workspaceModel->removeRows(0, m_workspaceModel->rowCount());
            rebuildWorkspaceModelNow();
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
    // Floor on dock width — keeps the dock from being crushed below readable
    // size when another dock shares the area. computeWorkspaceDockWidth()
    // returns 180 as its lower bound; matching that here so the user can
    // collapse to that width but no narrower.
    m_workspaceDock->setMinimumWidth(180);
    // Initial placement is LeftDockWidgetArea (not Top) so the first
    // time the user opens the workspace via View menu it appears in
    // the expected sidebar position. We used to put it in Top and rely
    // on placeSidebarDock to move it on the first project open — but
    // that's also what was force-showing the dock on load, which the
    // user explicitly didn't want.
    addDockWidget(Qt::LeftDockWidgetArea, m_workspaceDock);
    m_workspaceDock->hide();
    // Watch resize events to drive the live divider-size tooltip.
    m_workspaceDock->installEventFilter(this);
    // Rebuild the workspace model the moment it becomes visible. We
    // skip rebuilds while hidden (see rebuildWorkspaceModelNow) so the
    // first show after a load has a stale empty model — this fixes
    // that without doing the work eagerly.
    //
    // Also re-cap the dock width on every show. The View → Workspace
    // toggle hits setVisible() directly (bypassing placeSidebarDock's
    // sizing path), and Qt's QMainWindow layout opens a previously
    // hidden left dock at the natural-split width — often half the
    // window. Worse, that wide size becomes the layout's new effective
    // minimum, so the separator can't be dragged smaller without a
    // float/redock cycle (which is what users were doing to recover).
    // Defer the resize one tick so it runs after Qt's own layout pass
    // settles — calling resizeDocks() inline at this point gets
    // silently overridden by the show's size-hint resolution.
    // Collapsed left-edge rail — a discoverable handle to re-open the
    // workspace when it's hidden (it's hidden by default). A thin fixed-width
    // dock in the LEFT area so it RESERVES its own column: the editor and its
    // north/south tab bars sit to its right instead of being overlaid. No
    // title bar, not movable/closable.
    {
        auto* rail = new WorkspaceRail(this);   // fixed 22 px, set in the class
        connect(rail, &WorkspaceRail::clicked, this,
                [this]() { applyLayoutPreset(Layout_Workspace); });
        m_workspaceRailDock = new QDockWidget(this);
        m_workspaceRailDock->setObjectName(QStringLiteral("WorkspaceRailDock"));
        m_workspaceRailDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
        m_workspaceRailDock->setAllowedAreas(Qt::LeftDockWidgetArea);
        auto* emptyTitle = new QWidget(m_workspaceRailDock);
        emptyTitle->setFixedHeight(0);                 // no title-bar chrome
        m_workspaceRailDock->setTitleBarWidget(emptyTitle);
        m_workspaceRailDock->setWidget(rail);
        addDockWidget(Qt::LeftDockWidgetArea, m_workspaceRailDock);
        // Hidden-by-default at launch IS the closed state, so the rail shows
        // on first run.
        m_workspaceRailDock->setVisible(
            rcx::railVisibleFor(m_workspaceDock->isVisible(), m_workspaceClosed));
    }
    // View ▸ Project toggles visibility without ever raising a Close event,
    // so the flag has to be maintained here too.
    connect(m_workspaceDock->toggleViewAction(), &QAction::triggered, this,
            [this](bool checked) { m_workspaceClosed = !checked; });
    connect(m_workspaceDock, &QDockWidget::visibilityChanged, this,
            [this](bool visible) {
        if (visible) { m_workspaceClosed = false; rebuildWorkspaceModel(); }
        // The rail is the handle back to a dock the user CLOSED. QDockWidget
        // also emits visibilityChanged(false) when the dock is merely
        // DESELECTED inside a tab group, and keying the rail off that alone
        // made it pop into the left column on every Bookmarks tab click.
        if (m_workspaceRailDock)
            m_workspaceRailDock->setVisible(
                rcx::railVisibleFor(visible, m_workspaceClosed));
    });

    // Right hairline only while docked — drop it the moment the dock floats.
    connect(m_workspaceDock, &QDockWidget::topLevelChanged, this,
            [this](bool) { updateWorkspaceDockEdge(); });
    updateWorkspaceDockEdge();

    connect(m_workspaceTree, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!index.data(rcx::RoleSectionHeader).toString().isEmpty()) return;

        auto structIdVar = index.data(Qt::UserRole + 1);
        uint64_t structId = structIdVar.isValid() ? structIdVar.toULongLong() : 0;

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
            setActiveDocDock(ownerDock);
            auto& tab = m_tabs[ownerDock];
            int pi = tree.indexOfId(parentId);
            if (pi >= 0) tab.ctrl->setCollapsed(parentId, false);
            tab.ctrl->setViewRootId(parentId);
            tab.ctrl->scrollToNodeId(structId);
            // setActiveDocDock refreshed the status segment BEFORE the drill-down
            // above changed the view root, so it described the pre-drill class.
            // Re-derive it now that the controller is on the target member.
            refreshSelectionStatus(tab.ctrl, -1);
            QPointer<QDockWidget> dockRef = ownerDock;
            QTimer::singleShot(0, this, [this, dockRef]() {
                if (!dockRef || !m_tabs.contains(dockRef)) return;
                auto& t = m_tabs[dockRef];
                if (t.activePaneIdx >= 0 && t.activePaneIdx < t.panes.size()) {
                    auto& p = t.panes[t.activePaneIdx];
                    if (p.viewMode == VM_Rendered || p.viewMode == VM_Both)
                        updateRenderedView(t, p);
                }
            });
            return;
        }

        // Root struct/enum: check if any existing tab already views this struct
        for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
            if (it->doc == doc && it->ctrl->viewRootId() == structId) {
                it.key()->raise();
                it.key()->show();
                setActiveDocDock(it.key());
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

    // Single-click: peek (raise existing tab / scroll to member) — no new tab creation
    connect(m_workspaceTree, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        if (!index.data(rcx::RoleSectionHeader).toString().isEmpty()) return;

        // Modifier held → user is multi-selecting, don't navigate
        if (QApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
            return;

        auto structIdVar = index.data(Qt::UserRole + 1);
        uint64_t structId = structIdVar.isValid() ? structIdVar.toULongLong() : 0;
        if (structId == 0) return;

        auto subVar = index.data(Qt::UserRole);
        if (!subVar.isValid()) return;
        auto* ownerDock = static_cast<QDockWidget*>(subVar.value<void*>());
        if (!ownerDock || !m_tabs.contains(ownerDock)) return;

        RcxDocument* doc = m_tabs[ownerDock].doc;
        auto& tree = doc->tree;
        int ni = tree.indexOfId(structId);
        if (ni < 0) return;

        uint64_t parentId = tree.nodes[ni].parentId;
        if (parentId != 0) {
            // Child member: navigate within owner tab, scroll to member
            ownerDock->raise();
            ownerDock->show();
            setActiveDocDock(ownerDock);
            auto& tab = m_tabs[ownerDock];
            int pi = tree.indexOfId(parentId);
            if (pi >= 0) tab.ctrl->setCollapsed(parentId, false);
            tab.ctrl->setViewRootId(parentId);
            tab.ctrl->scrollToNodeId(structId);
            // setActiveDocDock refreshed the status segment BEFORE the drill-down
            // above changed the view root, so it described the pre-drill class.
            // Re-derive it now that the controller is on the target member.
            refreshSelectionStatus(tab.ctrl, -1);
            QPointer<QDockWidget> dockRef = ownerDock;
            QTimer::singleShot(0, this, [this, dockRef]() {
                if (!dockRef || !m_tabs.contains(dockRef)) return;
                auto& t = m_tabs[dockRef];
                if (t.activePaneIdx >= 0 && t.activePaneIdx < t.panes.size()) {
                    auto& p = t.panes[t.activePaneIdx];
                    if (p.viewMode == VM_Rendered || p.viewMode == VM_Both)
                        updateRenderedView(t, p);
                }
            });
        } else {
            // Root item: raise existing tab if one views this struct (peek only)
            for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
                if (it->doc == doc && it->ctrl->viewRootId() == structId) {
                    it.key()->raise();
                    it.key()->show();
                    setActiveDocDock(it.key());
                    return;
                }
            }
        }
    });
}

// ── Scanner Dock ──

void MainWindow::createScannerDock() {
    m_scannerDock = new QDockWidget("Scanner", this);
    m_scannerDock->setObjectName("ScannerDock");
    // Allow Top in addition to Bottom and Left. The previous restriction
    // assumed the scanner only ever made sense under the code or beside
    // the workspace, but with corner-reassignment on EdgeX drops the
    // scanner can take the full top strip too — same as it now can the
    // full bottom strip — and that's a valid layout.
    m_scannerDock->setAllowedAreas(Qt::BottomDockWidgetArea
                                 | Qt::LeftDockWidgetArea
                                 | Qt::TopDockWidgetArea);
    m_scannerDock->setFeatures(
        QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
        QDockWidget::DockWidgetFloatable);

    // Shared dock header — same height/ground/title tone as Project, Symbols
    // and Bookmarks. Qt's native drag-to-redock still runs because the header
    // never consumes the mouse events (the old hand-rolled DockTitleBar proved
    // the mechanism; this one just does it in one place).
    m_scanDockHeader = makeDockHeader(m_scannerDock, QStringLiteral("Scanner"));
    m_scanDockTitle  = m_scanDockHeader->titleLabel();
    connect(m_scanDockHeader->closeButton(), &QToolButton::clicked,
            m_scannerDock, &QDockWidget::close);

    // Placeholder widget so the dock has *something* to show until the
    // real ScannerPanel is built lazily by ensureScannerPanel(). The
    // placeholder is replaced via setWidget() — Qt deletes the prior
    // widget under the dock automatically.
    m_scannerDock->setWidget(new QWidget(m_scannerDock));
    // Floor on dock height — scanner now has 3 input rows (mode/value, filters,
    // workflow buttons), the breadcrumb, the result filter, the truncation
    // banner, and a results table. 140 px crushed everything; 320 px gives
    // the table real room and the chrome stops competing with results.
    m_scannerDock->setMinimumHeight(320);
    addDockWidget(Qt::BottomDockWidgetArea, m_scannerDock);
    m_scannerDock->hide();
    m_scannerDock->installEventFilter(this);

    // Default size on initial show. resizeDocks() issued HERE is silently
    // dropped — the dock is still hidden and still holds the empty placeholder,
    // so QMainWindow re-resolves its height from the real panel's size hint on
    // the first show. Re-issue it once, on the first real show, after the panel
    // has landed and the layout pass has settled. A size the user dragged and
    // saved now wins over the default, which on this path it never did before
    // (only a drag-PLACED dock ever restored its saved size), and a floating
    // scanner is left alone.
    //
    // MEASURED CAVEAT: this is a floor-clamped REQUEST, not a size. With SCAN
    // FOR expanded ScannerPanel::minimumSizeHint() is 408 px, so the dock
    // cannot go below 408 + kDockHeaderHeight = 440 and the 360 asked for here
    // is clamped to that. An actual 360 needs the panel's own minimum to come
    // down (its results table) — panel internals, not dock chrome. The request
    // stays so it takes effect the moment that happens.
    connect(m_scannerDock, &QDockWidget::visibilityChanged, this,
            [this](bool visible) {
        if (!visible || m_scannerSizedOnce) return;
        m_scannerSizedOnce = true;
        // Swap the placeholder for the real panel FIRST: setWidget() re-runs
        // the dock layout off the new widget's size hint, so a resize issued
        // before it is undone the moment the panel lands.
        ensureScannerPanel();
        QPointer<QDockWidget> dockPtr(m_scannerDock);
        const int sz = loadDockSize(m_scannerDock, kScannerDockDefaultH);
        QTimer::singleShot(0, this, [this, dockPtr, sz]() {
            if (!dockPtr || dockPtr->isFloating() || !dockPtr->isVisible()) return;
            resizeDocks({dockPtr}, {sz}, Qt::Vertical);
        });
    });

    // Floating chrome (BorderOverlay + ResizeGrip) is wired by makeDockHeader,
    // which every dock shares. The scanner used to force setFloating(true) and
    // centre itself at 720x700 in the ctor, which overrode the Bottom
    // placement + the size request above on EVERY first show; floating is one
    // header drag away, and a saved layout that WAS floating still restores.
}

// Build the heavy ScannerPanel widget on demand. Pulled out of
// createScannerDock because it accounts for ~195 ms of MainWindow ctor
// time on a fresh launch (FilterChips, scan buttons, type/condition
// combos, sortable results table — a lot of widget construction). The
// dock itself is built synchronously so menu wiring (`m_scannerDock`
// references in createMenus) still finds a non-null target; the panel
// inside it lazy-builds the first time someone asks for it.
//
// Idempotent: subsequent calls are no-ops. Triggers:
//   * QTimer::singleShot(0, …) after window.show() in main()
//   * View > Scanner toggle (in case the user beats the timer)
//   * Anything in MainWindow that touches m_scannerPanel directly
void MainWindow::ensureScannerPanel() {
    if (m_scannerPanel || !m_scannerDock) return;
    PROFILE_SCOPE("MainWindow::ensureScannerPanel");
    m_scannerPanel = new ScannerPanel(m_scannerDock);
    m_scannerPanel->applyTheme(ThemeManager::instance().current());
    {
        QSettings settings("RC", "RC");
        QString fontName = settings.value("font", "JetBrains Mono").toString();
        QFont f(fontName, 12);
        f.setFixedPitch(true);
        m_scannerPanel->setEditorFont(f);
    }
    m_scannerDock->setWidget(m_scannerPanel);  // replaces placeholder

    // Wire provider getter: lazily captures the active tab's provider at scan time
    m_scannerPanel->setProviderGetter([this]() -> std::shared_ptr<rcx::Provider> {
        auto* ctrl = activeController();
        return ctrl ? ctrl->provider() : nullptr;
    });
    connect(m_scannerPanel, &ScannerPanel::resultSourceChanged,
            this, &MainWindow::updateScannerTitle);

    // Wire bounds getter: struct base + size for "Current Struct" filter
    m_scannerPanel->setBoundsGetter([this]() -> rcx::ScannerPanel::StructBounds {
        auto* ctrl = activeController();
        if (!ctrl) return {};
        auto& tree = ctrl->document()->tree;
        uint64_t base = ctrl->baseAddress();
        uint64_t viewRoot = ctrl->viewRootId();
        int span = 0;
        if (viewRoot != 0) {
            span = tree.structSpan(viewRoot);
        } else {
            // Compute extent from all top-level nodes
            for (int i = 0; i < tree.nodes.size(); i++) {
                const auto& n = tree.nodes[i];
                int64_t off = tree.computeOffset(i);
                if (off < 0) continue;
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
        if (m_scannerPanel->resultProvider() && m_scannerPanel->resultProvider() != ctrl->provider()) {
            setAppStatus(QStringLiteral("Scan source differs from the active view"));
            return;
        }
        // Undoable, recent-listed and tracking-cooled like every other rebase.
        ctrl->navigateToAddress(addr);
    });
    connect(m_scannerPanel, &ScannerPanel::openBesideRequested, this, [this](uint64_t addr) {
        auto* ctrl = activeController();
        if (!ctrl) return;
        uint64_t root = ctrl->viewRootId();
        if (!root) {
            for (const auto& node : ctrl->document()->tree.nodes)
                if (node.parentId == 0 && node.kind == NodeKind::Struct) { root = node.id; break; }
        }
        openInstanceBeside(ctrl, root, addr, {}, m_scannerPanel->resultProvider());
    });
}

void MainWindow::createSymbolsDock() {
    if (m_symbolsDock) return;  // idempotent: deferred + menu may both call
    PROFILE_SCOPE("MainWindow::createSymbolsDock");
    m_symbolsDock = new QDockWidget("Symbols", this);
    m_symbolsDock->setObjectName("SymbolsDock");
    m_symbolsDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_symbolsDock->setFeatures(
        QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
        QDockWidget::DockWidgetFloatable);

    const auto& t = ThemeManager::instance().current();

    // Shared dock header. The old one was a bare 24-px QWidget whose
    // background came from an autoFillBackground palette hack that applyTheme
    // re-set but never repainted, so the Symbols header colour effectively
    // never applied. "Download All" is icon-only in the header right slot;
    // the label lives in its tooltip.
    m_symDockHeader = makeDockHeader(m_symbolsDock, QStringLiteral("Symbols"));
    m_symDockTitle  = m_symDockHeader->titleLabel();
    m_symDownloadBtn = rcx::makeHeaderToolButton(
        QStringLiteral(":/vsicons/cloud-download.svg"),
        QStringLiteral("Download all symbols"), m_symDockHeader);
    connect(m_symDownloadBtn, &QToolButton::clicked,
            this, &MainWindow::downloadSymbolsForProcess);
    m_symDockHeader->addRightWidget(m_symDownloadBtn);
    m_symDockHeader->applyTheme(t);     // tint the freshly added right-slot icon
    connect(m_symDockHeader->closeButton(), &QToolButton::clicked,
            m_symbolsDock, &QDockWidget::close);

    // Container hosting the unified Symbols panel (no tab widget — one list).
    auto* container = new QWidget(m_symbolsDock);
    auto* containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);

    m_unifiedSymbols = new rcx::UnifiedSymbolPanel(container);
    m_unifiedSymbols->setActiveProviderFn([this]() -> const rcx::Provider* {
        auto* ctrl = activeController();
        return (ctrl && ctrl->document()) ? ctrl->provider().get() : nullptr;
    });
    containerLayout->addWidget(m_unifiedSymbols, 1);

    connect(m_unifiedSymbols, &rcx::UnifiedSymbolPanel::navigateRequested, this,
            [this](const rcx::NamedAddress& e) {
        if (e.address == 0) return;
        auto* ctrl = activeController();
        if (!ctrl) return;
        if (!ctrl->navigateToAddress(e.address, e.name)) return;
        setAppStatus(QStringLiteral("Navigated to %1 (0x%2)")
            .arg(e.name).arg(e.address, 0, 16));
    });

    connect(m_unifiedSymbols, &rcx::UnifiedSymbolPanel::importTypeRequested, this,
            [this](const rcx::NamedAddress& e) {
        if (e.typeIndex == 0 || e.meta.isEmpty()) return;
        importTypeFromPdbUI(e.meta, e.typeIndex, e.name);
    });

    connect(m_unifiedSymbols, &rcx::UnifiedSymbolPanel::importSelectedRequested, this,
            [this](const QVector<rcx::NamedAddress>& sel) {
        bulkImportTypesUI(sel);
    });

    connect(m_unifiedSymbols, &rcx::UnifiedSymbolPanel::removeRequested, this,
            [this](const rcx::NamedAddress& e) {
        for (const auto& p : rcx::NameRegistry::instance().providers()) {
            if (p->id() == e.source && p->supportsRemove()) {
                p->remove(e.name);
                if (auto* ctrl = activeController()) ctrl->refresh();
                rcx::NameRegistry::instance().emitChanged();
                setAppStatus(QStringLiteral("Removed %1").arg(e.name));
                return;
            }
        }
    });

    // The symbol panel's empty-area context menu emits "Load PDB..." /
    // "Add bookmark..." but they were never connected — wire them to the
    // existing handlers so the menu items actually work.
    connect(m_unifiedSymbols, &rcx::UnifiedSymbolPanel::loadPdbRequested,
            this, &MainWindow::importPdb);
    connect(m_unifiedSymbols, &rcx::UnifiedSymbolPanel::addBookmarkRequested,
            this, &MainWindow::promptAddBookmark);

    m_symbolsDock->setWidget(container);
    // Symbols dock is taller and needs room for module list + symbol tree.
    // 260 = the panel's narrow-chip breakpoint, so the default width always
    // yields one chip row + the count instead of a two-column chip grid.
    m_symbolsDock->setMinimumWidth(260);
    addDockWidget(Qt::RightDockWidgetArea, m_symbolsDock);
    m_symbolsDock->hide();
    m_symbolsDock->installEventFilter(this);

    // Floating chrome (BorderOverlay + ResizeGrip + DockBorderFilter) is wired
    // by makeDockHeader — one copy for all four docks.
}

int MainWindow::loadPdbAndCacheTypes(const QString& pdbPath) {
    QString symErr;
    auto result = rcx::extractPdbSymbols(pdbPath, &symErr);
    if (result.symbols.isEmpty()) return 0;

    QVector<QPair<QString, uint32_t>> pairs;
    QHash<QString, uint32_t> typeIndices;
    pairs.reserve(result.symbols.size());
    for (const auto& s : result.symbols) {
        pairs.emplaceBack(s.name, s.rva);
        if (s.typeIndex != 0)
            typeIndices.insert(s.name, s.typeIndex);
    }

    int count = rcx::SymbolStore::instance().addModule(
        result.moduleName, pdbPath, pairs);
    if (!typeIndices.isEmpty())
        rcx::SymbolStore::instance().addModuleTypeIndices(
            result.moduleName, typeIndices);

    // Standalone TPI types — populates the unified Symbols panel with the
    // struct/enum definitions even when no symbol names them. Right-click
    // a type row → "Import type" pulls it into the active document.
    QString typeErr;
    auto types = rcx::enumeratePdbTypes(pdbPath, &typeErr);
    if (!types.isEmpty()) {
        std::sort(types.begin(), types.end(), [](const auto& a, const auto& b) {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
        rcx::SymbolStore::instance().addModuleTypes(result.moduleName, types);
    }

    rcx::NameRegistry::instance().emitChanged();
    return count;
}

// ── Bookmarks dock ──

void MainWindow::createBookmarksDock() {
    // Shell only — kept cheap so createMenus()' toggleViewAction() and Reset
    // Windows work immediately. The CONTENT is built lazily off the critical
    // path (populateBookmarksDock): its first setWidget() was a ~300 ms hit —
    // the first stylesheet'd widgets in the app (the DialogButtons) get their
    // QSS style resolved during that polish — and the dock is hidden by
    // default, so paying it before first paint was pure waste.
    m_bookmarksDock = new QDockWidget("Bookmarks", this);
    m_bookmarksDock->setObjectName("BookmarksDock");
    m_bookmarksDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    // Minimum width stops Qt's splitter from crushing the dock below the
    // point where content is readable when other docks share the area.
    m_bookmarksDock->setMinimumWidth(180);
    // Shared dock header. Bookmarks was the one dock still on Fusion's stock
    // title bar, so tabifying it with Project visibly changed the header
    // height (and font) on every tab switch.
    m_bmDockHeader = makeDockHeader(m_bookmarksDock, QStringLiteral("Bookmarks"));
    connect(m_bmDockHeader->closeButton(), &QToolButton::clicked,
            m_bookmarksDock, &QDockWidget::close);
    addDockWidget(Qt::LeftDockWidgetArea, m_bookmarksDock);
    m_bookmarksDock->hide();  // hidden by default; toggle via View menu
    m_bookmarksDock->installEventFilter(this);

    // When the dock becomes visible via any path (View menu toggle, drag
    // re-dock, MCP, etc.) tabify with the workspace if it's already open in
    // the same area. Prevents the "open bookmarks → workspace gets squished
    // by Qt's splitter" pathology. m_placingSidebar guards against signal
    // recursion (placeSidebarDock calls show() which re-fires this signal).
    connect(m_bookmarksDock, &QDockWidget::visibilityChanged, this,
            [this](bool visible) {
        if (!visible || m_placingSidebar) return;
        // Revealed before the deferred populate ran? Build it now so the user
        // never sees an empty dock (the populate guard makes this idempotent).
        if (!m_bookmarksList) populateBookmarksDock();
        placeSidebarDock(m_bookmarksDock, Qt::LeftDockWidgetArea, 240);
    });
}

void MainWindow::populateBookmarksDock() {
    if (m_bookmarksList) return;  // already built (deferred tick or first show)
    PROFILE_SCOPE("MainWindow::populateBookmarksDock");

    auto* container = new QWidget(m_bookmarksDock);
    container->setObjectName(QStringLiteral("bookmarksContainer"));
    auto* layout = new QVBoxLayout(container);
    // Flush like the Project dock: the panel's own hairlines delineate the
    // rows, so an inset frame of empty paper around them is just noise.
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_bookmarksFilter = new rcx::PanelSearchField(
        QStringLiteral(":/vsicons/filter.svg"),
        QStringLiteral("Filter bookmarks\u2026"), container);
    layout->addWidget(m_bookmarksFilter);

    auto* bmList = new EmptyHintListWidget(container);
    bmList->setFont(rcx::chromeFont());
    bmList->placeholder = QStringLiteral("No bookmarks");
    bmList->hint        = QStringLiteral("Right-click a field ▸ Bookmark this address…");
    m_bookmarksList = bmList;
    m_bookmarksList->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_bookmarksList, 1);

    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(rcx::kGutter, 6, rcx::kGutter, 6);
    auto* addBtn = new rcx::DialogButton(QStringLiteral("Add"),
        rcx::DialogButton::Primary, container);
    auto* removeBtn = new rcx::DialogButton(QStringLiteral("Remove"),
        rcx::DialogButton::Secondary, container);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(removeBtn);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    m_bookmarksDock->setWidget(container);

    connect(addBtn, &QPushButton::clicked, this, &MainWindow::promptAddBookmark);
    connect(removeBtn, &QPushButton::clicked, this, [this]() {
        int row = m_bookmarksList->currentRow();
        auto* c = activeController();
        if (c && row >= 0) { c->removeBookmark(row); refreshBookmarksDock(); }
    });
    connect(m_bookmarksList, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) {
        if (item) navigateBookmark(m_bookmarksList->row(item));
    });
    connect(m_bookmarksFilter, &QLineEdit::textChanged, this,
            [this](const QString& text) {
        for (int i = 0; i < m_bookmarksList->count(); i++) {
            auto* it = m_bookmarksList->item(i);
            it->setHidden(!text.isEmpty()
                          && !it->text().contains(text, Qt::CaseInsensitive));
        }
    });
    connect(m_bookmarksList, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        int row = m_bookmarksList->indexAt(pos).row();
        if (row < 0) return;
        QMenu menu;
        menu.addAction("&Navigate", this, [this, row]() { navigateBookmark(row); });
        menu.addAction("&Remove", this, [this, row]() {
            auto* c = activeController();
            if (c) { c->removeBookmark(row); refreshBookmarksDock(); }
        });
        menu.exec(m_bookmarksList->viewport()->mapToGlobal(pos));
    });

    // Fill with the active document's bookmarks — refreshBookmarksDock has
    // been a no-op until now (it early-returns while m_bookmarksList is null).
    refreshBookmarksDock();
    // Lazily-built content missed the startup applyTheme — colour it now (and
    // applyTheme re-runs this on theme change).
    themeBookmarksContent();
}

// Match the Bookmarks panel to the editor "paper" surface, same as the
// workspace — it's a side panel that sits beside the editor (and tabifies with
// the workspace), so the lighter chrome background reads as jarring. No-op
// until the lazily-built content exists.
void MainWindow::themeBookmarksContent() {
    if (!m_bookmarksList) return;
    const rcx::Theme& theme = rcx::ThemeManager::instance().current();
    const QColor paper = rcx::editorPaperColor(theme);
    if (auto* cont = m_bookmarksDock
            ? m_bookmarksDock->findChild<QWidget*>("bookmarksContainer") : nullptr)
        cont->setStyleSheet(QStringLiteral(
            "QWidget#bookmarksContainer { background: %1; }").arg(paper.name()));
    QPalette lp = m_bookmarksList->palette();
    lp.setColor(QPalette::Base, paper);                 // item rows → editor paper
    m_bookmarksList->setPalette(lp);
    m_bookmarksList->setFont(rcx::chromeFont());
    m_bookmarksList->setStyleSheet(QStringLiteral(
        "QListWidget { background: %1; border: none; }").arg(paper.name()));
    if (m_bookmarksFilter) m_bookmarksFilter->applyTheme(theme);
    if (m_bmDockHeader)    m_bmDockHeader->applyTheme(theme);
}

void MainWindow::refreshBookmarksDock() {
    if (!m_bookmarksList) return;
    m_bookmarksList->clear();
    auto* c = activeController();
    if (!c) return;
    const auto& bms = c->document()->tree.bookmarks;
    for (const auto& b : bms) {
        QString label = b.name + QStringLiteral("  ") + b.addressFormula;
        m_bookmarksList->addItem(label);
    }
    QString filter = m_bookmarksFilter ? m_bookmarksFilter->text() : QString();
    if (!filter.isEmpty()) {
        for (int i = 0; i < m_bookmarksList->count(); i++) {
            auto* it = m_bookmarksList->item(i);
            it->setHidden(!it->text().contains(filter, Qt::CaseInsensitive));
        }
    }
}

void MainWindow::promptAddBookmark() {
    auto* c = activeController();
    if (!c) return;
    QString defaultFormula = c->addressExpression(c->baseAddress(), c->baseAddressFormula());
    ThemedDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Add Bookmark"));
    dlg.setMinimumWidth(420);
    auto* form = new QVBoxLayout(&dlg);
    form->setContentsMargins(14, 12, 14, 12);
    form->setSpacing(8);
    auto* nameEdit = new QLineEdit(&dlg);
    nameEdit->setPlaceholderText(QStringLiteral("Bookmark name"));
    auto* formulaEdit = new QLineEdit(defaultFormula, &dlg);
    formulaEdit->setPlaceholderText(QStringLiteral("Address formula (e.g. <game.exe>+0x12340)"));
    form->addWidget(new QLabel(QStringLiteral("Name:"), &dlg));
    form->addWidget(nameEdit);
    form->addWidget(new QLabel(QStringLiteral("Address:"), &dlg));
    form->addWidget(formulaEdit);
    auto* cancelBtn = new DialogButton(QStringLiteral("Cancel"),
        DialogButton::Secondary, &dlg);
    auto* addBtn = new DialogButton(QStringLiteral("Add bookmark"),
        DialogButton::Primary, &dlg);
    QObject::connect(addBtn,    &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    addBtn->setDefault(true);
    form->addLayout(ThemedDialog::makeButtonRow({ cancelBtn, addBtn }));
    nameEdit->setFocus();
    if (dlg.exec() != QDialog::Accepted) return;
    if (nameEdit->text().trimmed().isEmpty()) return;
    c->addBookmark(nameEdit->text(), formulaEdit->text());
    refreshBookmarksDock();
    if (m_bookmarksDock) m_bookmarksDock->show();
}

void MainWindow::navigateBookmark(int idx) {
    auto* c = activeController();
    if (!c) return;
    const auto& bms = c->document()->tree.bookmarks;
    if (idx < 0 || idx >= bms.size()) return;
    QString err;
    if (!c->navigateToFormula(bms[idx].addressFormula, &err)) {
        ThemedMessageBox::warn(this,
            QStringLiteral("Bookmark Not Resolved"),
            QStringLiteral("Couldn't evaluate \"%1\". %2")
                .arg(bms[idx].addressFormula,
                     err.isEmpty() ? QStringLiteral("The expression isn't valid.")
                                   : err));
    }
}

void MainWindow::rebuildSymbols() {
    // The unified panel listens for NameRegistry::providersChanged and
    // refreshes itself, so the canonical way to nudge it is via the
    // registry signal. Direct rebuild() is kept for callers that just want
    // to force a re-pull regardless of provider state changes.
    if (m_unifiedSymbols) m_unifiedSymbols->rebuild();
    rcx::NameRegistry::instance().emitChanged();
}

void MainWindow::importTypeFromPdbUI(const QString& pdbPath,
                                     uint32_t typeIndex,
                                     const QString& displayName) {
    if (typeIndex == 0 || pdbPath.isEmpty()) return;

    auto* tab = activeTab();
    if (!tab) {
        project_new();
        tab = activeTab();
        if (!tab) return;
    }

    QString importedTypeName;
    QString err;
    rcx::NodeTree importedTree = rcx::importTypeForSymbol(
        pdbPath, typeIndex, &importedTypeName, &err);
    if (importedTree.nodes.isEmpty()) {
        setAppStatus(err.isEmpty()
            ? QStringLiteral("Failed to import type %1").arg(displayName)
            : err);
        return;
    }

    auto& tree = tab->doc->tree;
    tab->ctrl->setSuppressRefresh(true);
    tab->doc->undoStack.beginMacro(
        QStringLiteral("Import type %1").arg(displayName));
    QHash<uint64_t, uint64_t> idMap;
    for (const auto& node : importedTree.nodes) idMap[node.id] = tree.reserveId();
    for (const auto& node : importedTree.nodes) {
        rcx::Node copy = node;
        copy.id = idMap.value(node.id, node.id);
        copy.parentId = idMap.value(node.parentId, node.parentId);
        if (copy.refId != 0)
            copy.refId = idMap.value(node.refId, node.refId);
        tab->doc->undoStack.push(new rcx::RcxCommand(tab->ctrl, rcx::cmd::Insert{copy}));
    }
    tab->doc->undoStack.endMacro();
    tab->ctrl->setSuppressRefresh(false);
    tab->ctrl->refresh();
    rebuildWorkspaceModel();
    setAppStatus(QStringLiteral("Imported %1 (%2 nodes)")
        .arg(importedTypeName).arg(importedTree.nodes.size()));
}

void MainWindow::bulkImportTypesUI(const QVector<rcx::NamedAddress>& entries) {
    // Group by pdbPath (stored in NamedAddress::meta by PdbNameProvider).
    QHash<QString, QVector<uint32_t>> byPdb;
    for (const auto& e : entries) {
        if (e.typeIndex == 0 || e.meta.isEmpty()) continue;
        byPdb[e.meta].append(e.typeIndex);
    }
    if (byPdb.isEmpty()) return;

    auto* tab = activeTab();
    if (!tab) {
        project_new();
        tab = activeTab();
        if (!tab) return;
    }

    int totalImported = 0;
    for (auto it = byPdb.constBegin(); it != byPdb.constEnd(); ++it) {
        QString err;
        rcx::NodeTree importedTree = rcx::importPdbSelected(it.key(), it.value(), &err);
        if (importedTree.nodes.isEmpty()) continue;

        auto& tree = tab->doc->tree;
        tab->ctrl->setSuppressRefresh(true);
        tab->doc->undoStack.beginMacro(QStringLiteral("Import PDB types"));
        QHash<uint64_t, uint64_t> idMap;
        for (const auto& node : importedTree.nodes) idMap[node.id] = tree.reserveId();
        for (const auto& node : importedTree.nodes) {
            rcx::Node copy = node;
            copy.id = idMap.value(node.id, node.id);
            copy.parentId = idMap.value(node.parentId, node.parentId);
            if (copy.refId != 0)
                copy.refId = idMap.value(node.refId, node.refId);
            tab->doc->undoStack.push(new rcx::RcxCommand(tab->ctrl, rcx::cmd::Insert{copy}));
        }
        tab->doc->undoStack.endMacro();
        tab->ctrl->setSuppressRefresh(false);

        int rootStructs = 0;
        for (const auto& n : importedTree.nodes)
            if (n.parentId == 0 && n.kind == rcx::NodeKind::Struct) rootStructs++;
        totalImported += rootStructs;
    }
    tab->ctrl->refresh();
    rebuildWorkspaceModel();
    setAppStatus(QStringLiteral("Imported %1 types into current project").arg(totalImported));
}

void MainWindow::downloadSymbolsForProcess() {
    auto* ctrl = activeController();
    if (!ctrl || !ctrl->provider()) {
        setAppStatus(QStringLiteral("No process attached"));
        return;
    }
    auto prov = ctrl->provider();
    auto modules = prov->enumerateModules();
    if (modules.isEmpty()) {
        setAppStatus(QStringLiteral("No modules found in target process"));
        return;
    }

    // Create downloader on first use
    if (!m_symDownloader) {
        m_symDownloader = new rcx::SymbolDownloader(this);
        connect(m_symDownloader, &rcx::SymbolDownloader::progress,
                this, [this](const QString& mod, int received, int total) {
            if (total > 0)
                setAppStatus(QStringLiteral("Downloading %1... %2/%3 KB")
                    .arg(mod).arg(received/1024).arg(total/1024));
            else
                setAppStatus(QStringLiteral("Downloading %1... %2 KB")
                    .arg(mod).arg(received/1024));
        });
        connect(m_symDownloader, &rcx::SymbolDownloader::finished,
                this, [this](const QString& mod, const QString& localPath,
                             bool success, const QString& error) {
            if (!success) {
                qDebug() << "[SymbolDownloader]" << mod << "failed:" << error;
                return;
            }
            // Extract symbols and add to store
            QString symErr;
            auto result = rcx::extractPdbSymbols(localPath, &symErr);
            if (!result.symbols.isEmpty()) {
                QVector<QPair<QString, uint32_t>> pairs;
                pairs.reserve(result.symbols.size());
                for (const auto& s : result.symbols)
                    pairs.emplaceBack(s.name, s.rva);
                int count = rcx::SymbolStore::instance().addModule(
                    result.moduleName, localPath, pairs);
                setAppStatus(QStringLiteral("Loaded %1 symbols for %2")
                    .arg(count).arg(mod));
            }
            rebuildSymbols();
            if (auto* c = activeController())
                c->refresh();
        });
    }

    // Build download queue: skip modules already loaded
    struct PendingModule {
        QString name;
        QString fullPath;
        uint64_t base;
        rcx::PdbDebugInfo debugInfo;
    };
    QVector<PendingModule> pending;

    setAppStatus(QStringLiteral("Scanning %1 modules for debug info...").arg(modules.size()));
    QApplication::processEvents();

    auto& store = rcx::SymbolStore::instance();
    for (const auto& mod : modules) {
        // Strip extension for canonical name check
        QString canonical = store.resolveAlias(mod.name);
        if (store.moduleData(canonical))
            continue; // already loaded

        // Extract PDB debug info from PE header in memory
        auto info = rcx::extractPdbDebugInfo(*prov, mod.base);
        if (!info.valid)
            continue;

        // Check local first (same directory as module)
        QString localPdb = rcx::SymbolDownloader::findLocal(mod.fullPath, info.pdbName);
        if (!localPdb.isEmpty()) {
            // Load directly
            QString symErr;
            auto result = rcx::extractPdbSymbols(localPdb, &symErr);
            if (!result.symbols.isEmpty()) {
                QVector<QPair<QString, uint32_t>> pairs;
                pairs.reserve(result.symbols.size());
                for (const auto& s : result.symbols)
                    pairs.emplaceBack(s.name, s.rva);
                int count = store.addModule(result.moduleName, localPdb, pairs);
                setAppStatus(QStringLiteral("Loaded %1 symbols for %2 (local)")
                    .arg(count).arg(mod.name));
                QApplication::processEvents();
            }
            continue;
        }

        // Check cache
        rcx::SymbolDownloader::DownloadRequest req;
        req.moduleName = mod.name;
        req.pdbName = info.pdbName;
        req.guidString = info.guidString;
        req.age = info.age;

        QString cached = m_symDownloader->findCached(req);
        if (!cached.isEmpty()) {
            QString symErr;
            auto result = rcx::extractPdbSymbols(cached, &symErr);
            if (!result.symbols.isEmpty()) {
                QVector<QPair<QString, uint32_t>> pairs;
                pairs.reserve(result.symbols.size());
                for (const auto& s : result.symbols)
                    pairs.emplaceBack(s.name, s.rva);
                int count = store.addModule(result.moduleName, cached, pairs);
                setAppStatus(QStringLiteral("Loaded %1 symbols for %2 (cached)")
                    .arg(count).arg(mod.name));
                QApplication::processEvents();
            }
            continue;
        }

        pending.push_back(PendingModule{mod.name, mod.fullPath, mod.base, info});
    }

    rebuildSymbols();

    if (pending.isEmpty()) {
        setAppStatus(QStringLiteral("All available symbols loaded"));
        if (auto* c = activeController())
            c->refresh();
        return;
    }

    // Download pending modules sequentially
    auto queue = std::make_shared<QVector<PendingModule>>(std::move(pending));
    auto idx = std::make_shared<int>(0);
    auto conn = std::make_shared<QMetaObject::Connection>();

    auto processNext = [this, queue, idx, conn]() {
        if (*idx >= queue->size()) {
            setAppStatus(QStringLiteral("Symbol download complete (%1 modules)")
                .arg(queue->size()));
            disconnect(*conn);
            return;
        }
        const auto& mod = (*queue)[*idx];
        (*idx)++;

        rcx::SymbolDownloader::DownloadRequest req;
        req.moduleName = mod.name;
        req.pdbName = mod.debugInfo.pdbName;
        req.guidString = mod.debugInfo.guidString;
        req.age = mod.debugInfo.age;
        m_symDownloader->download(req);
    };

    // Chain downloads: when one finishes, start the next
    *conn = connect(m_symDownloader, &rcx::SymbolDownloader::finished,
            this, [this, processNext](const QString&, const QString&, bool, const QString&) {
        QTimer::singleShot(0, this, processNext);
    });

    setAppStatus(QStringLiteral("Downloading symbols for %1 modules...").arg(queue->size()));
    processNext();
}

void MainWindow::rebuildAllDocs() {
    m_allDocs.clear();
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        if (!m_allDocs.contains(it.value().doc))
            m_allDocs.append(it.value().doc);
    }
}

void MainWindow::rebuildWorkspaceModel() {
    // Debounce: coalesce rapid calls into a single rebuild
    if (!m_workspaceRebuildTimer) {
        m_workspaceRebuildTimer = new QTimer(this);
        m_workspaceRebuildTimer->setSingleShot(true);
        m_workspaceRebuildTimer->setInterval(50);
        connect(m_workspaceRebuildTimer, &QTimer::timeout,
                this, &MainWindow::rebuildWorkspaceModelNow);
    }
    m_workspaceRebuildTimer->start();
}

void MainWindow::rebuildWorkspaceModelNow() {
    // Skip entirely when the dock is hidden — the rebuild walks every
    // tab and emits a QStandardItem per struct, which on big projects
    // costs hundreds of ms. Pointless when nobody can see the result.
    // The dock's visibilityChanged handler in createWorkspaceDock
    // re-triggers this when the user actually opens the workspace, so
    // the model is fresh the first time it's painted. We DON'T touch
    // m_workspaceGen here, so the next call after the dock becomes
    // visible sees the gen drift and runs the real rebuild.
    if (m_workspaceDock && !m_workspaceDock->isVisible())
        return;

    // Generation gate — hash (tab list, struct id+name+keyword+pinned status)
    // and skip the rebuild when the result would be identical. Catches the
    // common "documentChanged fired but only baseAddress / values changed"
    // path so we don't blow away expansion + scroll state on every refresh.
    quint64 gen = 0;
    auto mix = [&](quint64 v) {
        gen ^= v + 0x9E3779B97F4A7C15ULL + (gen << 6) + (gen >> 2);
    };
    QSet<RcxDocument*> seen;
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        mix(reinterpret_cast<quintptr>(it.key()));
        mix(it->ctrl->viewRootId());
        if (seen.contains(it->doc)) continue;
        seen.insert(it->doc);
        for (const auto& n : it->doc->tree.nodes) {
            if (n.parentId != 0 || n.kind != NodeKind::Struct) continue;
            mix(n.id);
            mix(qHash(n.structTypeName));
            mix(qHash(n.name));
            mix(qHash(n.classKeyword));
            mix(m_pinnedIds.contains(n.id) ? 1 : 0);
        }
    }
    if (gen == m_workspaceGen) return;
    m_workspaceGen = gen;

    // Save scroll position before model clear (which resets it)
    int savedScroll = 0;
    if (m_workspaceTree && m_workspaceTree->verticalScrollBar())
        savedScroll = m_workspaceTree->verticalScrollBar()->value();

    // Capture expansion state + current selection keyed by node id. The
    // subsequent model->clear() destroys all QModelIndex objects, so we must
    // translate "which row is expanded / selected" into something stable
    // (node id) and re-apply after the rebuild. Without this, any refresh
    // collapses every expanded type and loses the user's selection — which
    // is visibly jumpy on large projects.
    QSet<uint64_t> expandedIds;
    uint64_t selectedId = 0;
    if (m_workspaceTree && m_workspaceProxy) {
        for (int i = 0; i < m_workspaceModel->rowCount(); ++i) {
            auto* item = m_workspaceModel->item(i);
            if (!item) continue;
            uint64_t id = item->data(Qt::UserRole + 1).toULongLong();
            if (id == 0) continue;
            QModelIndex src = m_workspaceModel->indexFromItem(item);
            QModelIndex proxy = m_workspaceProxy->mapFromSource(src);
            if (m_workspaceTree->isExpanded(proxy))
                expandedIds.insert(id);
        }
        QModelIndexList sel = m_workspaceTree->selectionModel()->selectedIndexes();
        if (!sel.isEmpty()) {
            QModelIndex src = m_workspaceProxy->mapToSource(sel.first());
            auto* item = m_workspaceModel->itemFromIndex(src);
            if (item) selectedId = item->data(Qt::UserRole + 1).toULongLong();
        }
    }

    QVector<rcx::TabInfo> tabs;
    QSet<RcxDocument*> seenDocs;
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        TabState& tab = it.value();
        if (seenDocs.contains(tab.doc)) continue;
        seenDocs.insert(tab.doc);
        QString name = rootName(tab.doc->tree, tab.ctrl->viewRootId());
        tabs.push_back(rcx::TabInfo{ &tab.doc->tree, name, static_cast<void*>(it.key()) });
    }
    rcx::syncProjectExplorer(m_workspaceModel, tabs, m_pinnedIds);

    // Mark items that are currently viewed in a tab + pinned/dirty state
    QSet<uint64_t> viewedIds;
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it)
        viewedIds.insert(it->ctrl->viewRootId());
    for (int i = 0; i < m_workspaceModel->rowCount(); ++i) {
        auto* item = m_workspaceModel->item(i);
        if (!item) continue;
        if (!item->data(rcx::RoleSectionHeader).toString().isEmpty()) continue;
        uint64_t id = item->data(Qt::UserRole + 1).toULongLong();
        item->setData(viewedIds.contains(id), Qt::UserRole + 3);
        item->setData(m_pinnedIds.contains(id), Qt::UserRole + 4);
    }

    if (m_dockTitleLabel) {
        bool anyDirty = false;
        for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it)
            if (it->doc->modified) { anyDirty = true; break; }
        // Just "Project" (+ dirty dot) \u2014 no struct/enum tallies; the tree is
        // rarely tall enough for the counts to be worth the header noise.
        QString title;
        if (anyDirty)
            title = QStringLiteral("\u2022 ");
        title += QStringLiteral("Project");
        m_dockTitleLabel->setText(title);
    }

    // Restore expansion + selection state captured before model->clear().
    if (m_workspaceTree && m_workspaceProxy) {
        for (int i = 0; i < m_workspaceModel->rowCount(); ++i) {
            auto* item = m_workspaceModel->item(i);
            if (!item) continue;
            uint64_t id = item->data(Qt::UserRole + 1).toULongLong();
            if (id == 0) continue;
            QModelIndex src = m_workspaceModel->indexFromItem(item);
            QModelIndex proxy = m_workspaceProxy->mapFromSource(src);
            if (expandedIds.contains(id))
                m_workspaceTree->setExpanded(proxy, true);
            if (selectedId != 0 && id == selectedId)
                m_workspaceTree->selectionModel()->setCurrentIndex(
                    proxy, QItemSelectionModel::ClearAndSelect);
        }
    }

    // Restore scroll position after rebuild
    if (m_workspaceTree && m_workspaceTree->verticalScrollBar())
        m_workspaceTree->verticalScrollBar()->setValue(savedScroll);
}

int MainWindow::computeWorkspaceDockWidth() const {
    // Measure longest type name across all open documents
    int maxChars = 12;  // minimum reasonable
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        const auto& tree = it->doc->tree;
        for (const auto& n : tree.nodes) {
            if (n.parentId != 0 || n.kind != rcx::NodeKind::Struct) continue;
            QString name = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
            if (name.size() > maxChars) maxChars = name.size();
        }
    }
    // Compute pixel width: badge(fontH) + gap(4) + name + gap + count pill(~30) + padding(24)
    QSettings s("RC", "RC");
    QFont f(s.value("font", "JetBrains Mono").toString(), 10);
    f.setFixedPitch(true);
    QFontMetrics fm(f);
    int nameW = fm.horizontalAdvance(QString(maxChars, QChar('W')));
    int total = fm.height() + 4 + nameW + 30 + 24;
    return qBound(180, total, 420);
}

// ── Sidebar dock placement helpers ──

// Single entry point for positioning a sidebar dock. Core rule: if another
// sidebar dock is already visible and docked in the target area, tabify with
// it instead of subdividing. Mirrors Visual Studio's behaviour where tool
// windows in the same region tabify by default. Eliminates the "open
// bookmarks → squish workspace" class of bug — extends to any future panel
// without new code.
void MainWindow::placeSidebarDock(QDockWidget* dock, Qt::DockWidgetArea area,
                                   int preferredSize) {
    if (!dock || m_placingSidebar) return;
    m_placingSidebar = true;

    // Find a visible, docked peer already living in the target area.
    QDockWidget* peer = nullptr;
    for (QDockWidget* other : {m_workspaceDock, m_bookmarksDock,
                                m_symbolsDock, m_scannerDock}) {
        if (!other || other == dock) continue;
        if (!other->isVisible() || other->isFloating()) continue;
        if (dockWidgetArea(other) != area) continue;
        peer = other;
        break;
    }

    const bool horizontal = (area == Qt::LeftDockWidgetArea
                              || area == Qt::RightDockWidgetArea);
    if (peer) {
        tabifyDockWidget(peer, dock);
    } else {
        // Previously: when the requested area was horizontal AND any
        // doc dock existed, we called splitDockWidget(dock,
        // m_docDocks.first(), Qt::Horizontal). The intent was to force
        // the sidebar into a full-height left/right strip, but
        // splitDockWidget uses the doc dock as a splitting anchor and
        // bisects its main-window-level area. When the doc dock
        // already contained a split editor (multiple SplitPanes), the
        // user saw three columns: bookmarks + the editor's two panes.
        // A plain addDockWidget puts the new dock in the requested
        // area without touching the doc dock — the editor's internal
        // panes stay where they were.
        addDockWidget(area, dock);
        // All sidebars (including workspace) honour the saved size.
        // First-launch fallback for workspace uses computeWorkspaceDockWidth
        // (content-aware fit, qBound(180, ..., 420)) instead of the old
        // hardcoded 235-px cap.
        int def = preferredSize;
        if (dock == m_workspaceDock && def <= 0)
            def = computeWorkspaceDockWidth();
        if (def > 0) {
            int sz = loadDockSize(dock, def);
            // Workspace should never start at the 50/50 split Qt picks
            // when the central widget has zero size. Cap to 35% of the
            // window width — user can drag it wider, the new size
            // persists, but the dock never opens at half-window.
            if (dock == m_workspaceDock && horizontal) {
                int cap = qMax(280, width() * 35 / 100);
                if (sz > cap) sz = cap;
            }
            // Defer resize until AFTER show() + Qt's layout pass settles.
            // resizeDocks called pre-show is silently overridden by Qt's
            // size-hint resolution, which is what was producing 50/50.
            QPointer<QDockWidget> dockPtr(dock);
            QTimer::singleShot(0, this, [this, dockPtr, sz, horizontal]() {
                if (!dockPtr) return;
                resizeDocks({dockPtr}, {sz},
                            horizontal ? Qt::Horizontal : Qt::Vertical);
            });
        }
    }
    dock->show();
    dock->raise();
    // Tab bar styling needs to pick up the new tab (tabifyDockWidget path) or
    // the new dock layout (split path).
    reconcileDockTabBars();
    m_placingSidebar = false;
}

void MainWindow::saveDockSize(QDockWidget* dock) {
    if (!dock || dock->isFloating()) return;
    Qt::DockWidgetArea a = dockWidgetArea(dock);
    if (a == Qt::NoDockWidgetArea) return;
    const bool horizontal = (a == Qt::LeftDockWidgetArea
                              || a == Qt::RightDockWidgetArea);
    int size = horizontal ? dock->width() : dock->height();
    if (size <= 0) return;
    QSettings s("RC", "RC");
    s.setValue(QStringLiteral("ui/dock.%1.size").arg(dock->objectName()), size);
}

int MainWindow::loadDockSize(QDockWidget* dock, int fallback) const {
    if (!dock) return fallback;
    QSettings s("RC", "RC");
    return s.value(QStringLiteral("ui/dock.%1.size").arg(dock->objectName()),
                   fallback).toInt();
}

void MainWindow::autosaveAllModifiedDocs() {
    // Walk every open doc once (multiple tabs can share a doc — dedup) and
    // shadow-save any that have unsaved changes AND a known filePath. We
    // do NOT clear doc->modified — this is a recovery snapshot, not a real
    // save. A real saveFile / saveFileAs removes the .autosave file.
    QSet<RcxDocument*> seen;
    int wrote = 0;
    for (auto it = m_tabs.constBegin(); it != m_tabs.constEnd(); ++it) {
        RcxDocument* doc = it.value().doc;
        if (seen.contains(doc)) continue;
        seen.insert(doc);
        if (!doc->modified) continue;
        if (doc->filePath.isEmpty()) continue;
        if (doc->save(doc->filePath + QStringLiteral(".autosave")))
            ++wrote;
    }
    if (wrote > 0)
        qInfo().nospace() << "[autosave] wrote " << wrote << " shadow copy(ies)";
}

void MainWindow::addRecentFile(const QString& path) {
    if (path.isEmpty()) return;
    QString absPath = QFileInfo(path).absoluteFilePath();

    QSettings s("RC", "RC");
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

    QSettings s("RC", "RC");
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
    } else {
        m_recentFilesMenu->addSeparator();
        m_recentFilesMenu->addAction(QStringLiteral("&Clear Recent"), this, [this]() {
            QSettings s("RC", "RC");
            s.remove("recentFiles");
            updateRecentFilesMenu();
        });
    }
}

void MainWindow::populateSourceMenu() {
    m_sourceMenu->clear();
    auto* ctrl = activeController();

    // Build saved sources for the shared menu builder
    QVector<SavedSourceDisplay> saved;
    if (ctrl) {
        const auto& ss = ctrl->savedSources();
        for (int i = 0; i < ss.size(); i++) {
            SavedSourceDisplay d;
            d.text = QStringLiteral("%1 '%2'").arg(ss[i].kind, ss[i].displayName);
            d.active = (i == ctrl->activeSourceIndex());
            saved.append(d);
        }
    }

    ProviderRegistry::populateSourceMenu(m_sourceMenu, saved);
}

void MainWindow::showPluginsDialog() {
    ThemedDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Plugins"));
    dialog.resize(620, 420);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

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

        // Skipped/incompatible provider plugins (ABI guard) — show the reason so
        // the user knows to rebuild rather than wondering where the plugin went.
        const auto& rejected = m_pluginManager.rejectedPlugins();
        if (!rejected.isEmpty()) {
            const auto& th = ThemeManager::instance().current();
            auto* head = new QListWidgetItem(
                QStringLiteral("⚠  %1 plugin(s) skipped:").arg(rejected.size()));
            head->setFlags(Qt::NoItemFlags);
            head->setForeground(th.markerError);
            list->addItem(head);
            for (const auto& r : rejected) {
                auto* item = new QListWidgetItem(
                    QStringLiteral("   %1\n     %2")
                        .arg(QFileInfo(r.path).fileName(), r.reason));
                item->setFlags(Qt::NoItemFlags);
                item->setForeground(th.textMuted);
                list->addItem(item);
            }
        }
    };

    refreshList();

    // Button row
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setContentsMargins(0, 6, 0, 0);
    btnLayout->setSpacing(8);

    auto* btnLoad = new DialogButton(QStringLiteral("Load plugin..."),
        DialogButton::Primary, &dialog);
    connect(btnLoad, &QPushButton::clicked, [&, refreshList]() {
        QString path = QFileDialog::getOpenFileName(&dialog, "Load Plugin",
                                                    QCoreApplication::applicationDirPath() + "/Plugins",
                                                    "Plugins (*.dll *.so *.dylib);;All Files (*)");

        if (!path.isEmpty()) {
            if (m_pluginManager.LoadPluginFromPath(path)) {
                refreshList();
                setAppStatus("Plugin loaded successfully");
            } else {
                ThemedMessageBox::warn(&dialog,
                    QStringLiteral("Plugin Load Failed"),
                    QStringLiteral("Couldn't load the selected plugin. "
                                   "Check the console for details."));
            }
        }
    });

    auto* btnUnload = new DialogButton(QStringLiteral("Unload selected"),
        DialogButton::Destructive, &dialog);
    connect(btnUnload, &QPushButton::clicked, [&, list, refreshList]() {
        auto* item = list->currentItem();
        if (!item) {
            ThemedMessageBox::info(&dialog,
                QStringLiteral("No Plugin Selected"),
                QStringLiteral("Pick a plugin in the list first, then click Unload Selected."));
            return;
        }

        QString pluginName = item->data(Qt::UserRole).toString();
        if (pluginName.isEmpty()) return;

        if (!ThemedMessageBox::confirm(&dialog,
                QStringLiteral("Unload Plugin"),
                QStringLiteral("Unload \"%1\"? Anything currently using it "
                               "will lose its provider.").arg(pluginName),
                QStringLiteral("Unload"),
                QStringLiteral("Cancel"),
                /*destructive=*/true)) return;

        if (m_pluginManager.UnloadPlugin(pluginName)) {
            refreshList();
            setAppStatus("Plugin unloaded");
        } else {
            ThemedMessageBox::warn(&dialog,
                QStringLiteral("Plugin Unload Failed"),
                QStringLiteral("Couldn't unload \"%1\".").arg(pluginName));
        }
    });

    auto* btnClose = new DialogButton(QStringLiteral("Close"),
        DialogButton::Secondary, &dialog);
    connect(btnClose, &QPushButton::clicked, &dialog, &QDialog::accept);

    btnLayout->addWidget(btnLoad);
    btnLayout->addWidget(btnUnload);
    btnLayout->addStretch();
    btnLayout->addWidget(btnClose);

    layout->addLayout(btnLayout);

    dialog.exec();
}

bool MainWindow::event(QEvent* e) {
    // (LayoutRequest→repositionResizeWidgets removed — suspected startup
    // layout churn. resizeEvent still repositions the zones.)
    return QMainWindow::event(e);
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange) {
        const auto& t = ThemeManager::instance().current();
        updateBorderColor(isActiveWindow() ? t.borderFocused : t.border);
        if (!isActiveWindow()) {
            for (auto& tab : m_tabs)
                for (auto& pane : tab.panes)
                    if (pane.editor) pane.editor->dismissAllPopups();
        }
    }
    if (event->type() == QEvent::WindowStateChange && m_titleBar)
        m_titleBar->updateMaximizeIcon();
    // Maximizing or restoring hides or shows the resize zones; the resize
    // and state-change events come in no guaranteed order.
    if (event->type() == QEvent::WindowStateChange)
        repositionResizeWidgets();
    // Drive the controller's adaptive refresh rate from window state:
    // focus loss widens the interval, minimize pauses the timer.
    // Folded into the same event handler so the wiring lives next to
    // the activation/border-color logic above.
    if (event->type() == QEvent::ActivationChange ||
        event->type() == QEvent::WindowStateChange) {
        bool focused = isActiveWindow();
        bool visible = !isMinimized();
        for (auto& tab : m_tabs)
            if (tab.ctrl) tab.ctrl->setWindowState(focused, visible);
    }
    // Keep border overlay on top after any state change
    if (m_borderOverlay) {
        m_borderOverlay->setGeometry(rect());
        m_borderOverlay->raise();
    }
}

void MainWindow::repositionResizeWidgets() {
    // The resize grip now lives in the status bar (FlatStatusBar pins it).
    // Here we only keep the frameless edge/corner zones glued to the border —
    // and out of the way while maximized, when the title bar owns the top edge.
    const bool enabled = rcx::resizeZonesEnabled(windowState());
    for (auto* re : findChildren<ResizeEdge*>()) {
        re->setVisible(enabled);
        if (!enabled) continue;
        re->setGeometry(rcx::resizeEdgeRect(re->edges(), width(), height(),
                                            menuWidget() ? menuWidget()->height() : 0));
        re->raise();
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (m_borderOverlay) {
        m_borderOverlay->setGeometry(rect());
        m_borderOverlay->raise();
    }
    repositionResizeWidgets();
}

void MainWindow::updateBorderColor(const QColor& color) {
    static_cast<BorderOverlay*>(m_borderOverlay)->color = color;
    m_borderOverlay->update();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Window geometry is intentionally NOT persisted — every launch auto-sizes
    // to 40% of the primary screen, centered (see the ctor). Saving it caused
    // stale off-screen/mis-sized restores after a resolution/DPI/monitor change.

    // Collect unique dirty docs (multiple tabs CAN share a document —
    // project_new() reuses the active doc and adds a new root struct, so
    // UnnamedClass0 + UnnamedClass1 typically live in the *same* doc).
    // For each dirty doc we collect ALL its root struct names — the
    // earlier rev took only the active tab's view-root, which silently
    // hid the other classes in a multi-class document.
    QSet<RcxDocument*> dirtyDocs;
    QStringList dirtyNames;
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        if (!it->doc->modified || dirtyDocs.contains(it->doc)) continue;
        dirtyDocs.insert(it->doc);
        // No cross-doc dedup of names — two unsaved projects with the
        // default class name "UnnamedClass0" SHOULD show two entries so
        // the user can see exactly how many are dirty. The
        // dirtyDocs.contains(it->doc) guard above already handles the
        // multi-tab-shares-one-doc case, which was the original dedup's
        // real purpose.
        dirtyNames.append(collectDirtyDocLabels(it->doc));
    }
    if (dirtyDocs.isEmpty()) { event->accept(); return; }

    // Three wordings, picked by what's actually dirty:
    //   • one doc, one class      → "<class> has unsaved changes."
    //   • one doc, many classes   → "Unsaved changes:" + list of class names
    //   • many docs               → "<count> projects have unsaved changes:"
    //                               + list of doc labels
    // In every case the headline matches the visible content — no more
    // "1 project has unsaved changes" + only the first class name.
    QString text;
    QString detail;
    if (dirtyDocs.size() == 1 && dirtyNames.size() == 1) {
        text = QStringLiteral("%1 has unsaved changes.")
                   .arg(dirtyNames.first());
    } else if (dirtyDocs.size() == 1) {
        text = QStringLiteral("Unsaved changes in %1 classes:")
                   .arg(dirtyNames.size());
        detail = dirtyNames.join(QStringLiteral("\n"));
    } else {
        text = QStringLiteral("%1 projects have unsaved changes:")
                   .arg(dirtyDocs.size());
        detail = dirtyNames.join(QStringLiteral("\n"));
    }

    auto choice = ThemedMessageBox::unsavedChanges(this,
        QStringLiteral("Unsaved Changes"),
        text,
        detail);

    if (choice == ThemedMessageBox::UnsavedChoice::Cancel) {
        event->ignore();
        return;
    }
    if (choice == ThemedMessageBox::UnsavedChoice::Save) {
        // Save each unique dirty doc via the tab that owns it.
        QSet<RcxDocument*> saved;
        for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
            if (!it->doc->modified || saved.contains(it->doc)) continue;
            saved.insert(it->doc);
            if (!project_save(it.key(), false)) {
                event->ignore();
                return;
            }
        }
    }
    // Discard → fall through and accept
    event->accept();
}

// ════════════════════════════════════════════════════════════════════
// UI Inspection (Ctrl+Shift+Click)
// ════════════════════════════════════════════════════════════════════

void MainWindow::clearInspection() {
    m_inspectedRegion = InspectionResult{};
    if (m_inspectionOverlay) {
        m_inspectionOverlay->hide();
        static_cast<InspectionOverlay*>(m_inspectionOverlay)->highlightRect = QRect();
    }
    setAppStatus("");
}

// Helper: build theme color entries for a set of field keys
static QJsonArray themeColorsForKeys(const QStringList& keys) {
    const auto& theme = ThemeManager::instance().current();
    QJsonArray arr;
    for (const QString& key : keys) {
        for (int i = 0; i < kThemeFieldCount; i++) {
            if (key == QLatin1String(kThemeFields[i].key)) {
                QJsonObject entry;
                entry["key"] = key;
                entry["value"] = (theme.*kThemeFields[i].ptr).name();
                entry["label"] = QString::fromLatin1(kThemeFields[i].label);
                entry["group"] = QString::fromLatin1(kThemeFields[i].group);
                arr.append(entry);
                break;
            }
        }
    }
    return arr;
}

// Region → theme field keys mapping
static QStringList themeKeysForRegion(const QString& region) {
    static const QHash<QString, QStringList> map = {
        {"editor.typeColumn",  {"syntaxKeyword", "syntaxType", "text", "textDim"}},
        {"editor.nameColumn",  {"text"}},
        {"editor.valueColumn", {"text", "syntaxNumber", "indHeatCold", "indHeatWarm", "indHeatHot"}},
        {"editor.hexBytes",    {"text"}},
        {"editor.foldArrow",   {"textFaint"}},
        {"editor.margin",      {"textFaint", "background"}},
        {"editor.asciiPreview", {"textMuted"}},
        {"editor.commandRow",  {"indCmdPill", "textFaint", "indHoverSpan", "syntaxType"}},
        {"editor.footer",      {"textFaint", "textDim", "border", "indCmdPill"}},
        {"editor.header",      {"background", "text", "textFaint"}},
        {"editor.treeLines",   {"textMuted"}},
        {"editor.background",  {"background"}},
        {"workspace.tree",     {"background", "text", "textDim", "hover", "selected"}},
        {"dockTabBar",         {"background", "text", "textDim", "hover", "border", "indHoverSpan"}},
        {"statusBar",          {"background", "textDim", "textMuted", "border"}},
        {"menuBar",            {"background", "text", "hover", "border"}},
        {"ribbon",             {"background", "text", "textDim", "hover", "selected", "border",
                                "indHoverSpan", "markerPtr", "syntaxKeyword", "indHintGreen",
                                "syntaxString", "syntaxType"}},
        {"scanner",            {"background", "text", "textDim", "border"}},
        {"symbols.tree",       {"background", "text", "textDim", "hover", "selected"}},
        {"mainWindow.border",  {"border", "borderFocused"}},
    };
    return map.value(region);
}

MainWindow::InspectionResult MainWindow::inspectAt(QWidget* widget, QPoint localPos) {
    InspectionResult r;
    r.selected = true;
    r.widgetName = QString::fromLatin1(widget->metaObject()->className());
    r.globalRect = QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size());

    // Generic properties for any widget
    r.properties["className"] = r.widgetName;
    r.properties["objectName"] = widget->objectName();
    r.properties["width"] = widget->width();
    r.properties["height"] = widget->height();

    // ── RcxEditor (Scintilla viewport) ──
    // Walk up from the hit widget to find QsciScintilla → RcxEditor
    RcxEditor* editor = nullptr;
    {
        QsciScintilla* sci = qobject_cast<QsciScintilla*>(widget);
        if (!sci) sci = qobject_cast<QsciScintilla*>(widget->parent());
        if (sci) {
            for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
                for (const auto& pane : it->panes) {
                    if (pane.editor && pane.editor->scintilla() == sci) {
                        editor = pane.editor;
                        break;
                    }
                }
                if (editor) break;
            }
        }
    }
    if (editor) {
        auto* sci = editor->scintilla();
        QWidget* vp = sci->viewport();
        QPoint vpPos = vp->mapFromGlobal(widget->mapToGlobal(localPos));

        // Scintilla coordinate helpers
        auto posFromCol = [sci](int ln, int c) -> long {
            return sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                                      (unsigned long)ln, (long)c);
        };
        auto pixelX = [sci](long bytePos) -> int {
            return (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION,
                                            0UL, bytePos);
        };
        auto pixelY = [sci](long bytePos) -> int {
            return (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION,
                                            0UL, bytePos);
        };
        int lineH = (int)sci->SendScintilla(QsciScintillaBase::SCI_TEXTHEIGHT, 0UL);

        // Helper: build a global rect from a column span on a given line
        auto spanRect = [&](int ln, const ColumnSpan& s) -> QRect {
            if (!s.valid) return {};
            int x1 = pixelX(posFromCol(ln, s.start));
            int x2 = pixelX(posFromCol(ln, s.end));
            int y = pixelY(posFromCol(ln, 0));
            return QRect(vp->mapToGlobal(QPoint(x1, y)), QSize(qMax(x2 - x1, 4), lineH));
        };
        // Helper: full-width line rect
        auto lineRect = [&](int ln) -> QRect {
            int y = pixelY(posFromCol(ln, 0));
            return QRect(vp->mapToGlobal(QPoint(0, y)), QSize(vp->width(), lineH));
        };

        // Check if in margin area (left of viewport)
        int margin0W = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINWIDTHN, 0UL, 0L);
        int margin1W = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINWIDTHN, 1UL, 0L);
        int totalMarginW = margin0W + margin1W;
        if (vpPos.x() < 0) {
            // Click was in the margin (to the left of the viewport)
            r.region = QStringLiteral("editor.margin");
            r.description = QStringLiteral("Offset margin — hex addresses/relative offsets");
            r.globalRect = QRect(sci->mapToGlobal(QPoint(0, 0)),
                                 QSize(totalMarginW, vp->height()));
            r.properties["marginWidth"] = margin0W;
            r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
            r.properties["fontSize"] = sci->font().pointSize();
            r.properties["fontFamily"] = sci->font().family();
            return r;
        }

        // Resolve line/col via Scintilla API
        long pos = sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMPOINTCLOSE,
                                       (unsigned long)vpPos.x(), (long)vpPos.y());
        int line = -1, col = -1;
        if (pos >= 0) {
            line = (int)sci->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, (unsigned long)pos);
            col = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, (unsigned long)pos);
        } else {
            // Fallback: compute line from Y coordinate
            int firstVisible = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
            if (lineH > 0) line = firstVisible + vpPos.y() / lineH;
        }

        const LineMeta* lm = (line >= 0) ? editor->metaForLine(line) : nullptr;
        if (lm) {
            int typeW = lm->effectiveTypeW;
            int nameW = lm->effectiveNameW;
            int lineLen = (int)sci->SendScintilla(
                QsciScintillaBase::SCI_GETLINEENDPOSITION, (unsigned long)line)
                - (int)sci->SendScintilla(
                    QsciScintillaBase::SCI_POSITIONFROMLINE, (unsigned long)line);
            ColumnSpan ts = RcxEditor::typeSpan(*lm, typeW);
            ColumnSpan ns = RcxEditor::nameSpan(*lm, typeW, nameW);
            ColumnSpan vs = RcxEditor::valueSpan(*lm, lineLen, typeW, nameW);

            // Identify region + compute tight rect
            if (lm->lineKind == LineKind::CommandRow) {
                r.region = QStringLiteral("editor.commandRow");
                r.description = QStringLiteral("Command row — chevron, root type, class name");
                r.globalRect = lineRect(line);
            } else if (lm->lineKind == LineKind::Footer) {
                r.region = QStringLiteral("editor.footer");
                r.description = QStringLiteral("Struct footer — append/trim buttons");
                r.globalRect = lineRect(line);
            } else if (lm->lineKind == LineKind::Header) {
                r.region = QStringLiteral("editor.header");
                r.description = QStringLiteral("Struct/array header line");
                r.globalRect = lineRect(line);
            } else if (const ColumnSpan fs = foldSlotFor(*lm);
                       fs.valid && col >= fs.start && col < fs.end) {
                r.region = QStringLiteral("editor.foldArrow");
                r.description = QStringLiteral("Fold box (drawn on the tree) — click to expand/collapse");
                r.globalRect = spanRect(line, fs);
            } else if (col >= 0 && col < kFoldCol + lm->depth * kTreeIndent && lm->depth > 0) {
                r.region = QStringLiteral("editor.treeLines");
                r.description = QStringLiteral("Tree lines (drawn over the indent)");
                r.globalRect = spanRect(line, {kFoldCol, kFoldCol + lm->depth * kTreeIndent, true});
            } else if (ts.valid && col >= ts.start && col < ts.end) {
                r.region = QStringLiteral("editor.typeColumn");
                r.description = QStringLiteral("Type column — field type names (int32_t, float, void*, etc.)");
                r.globalRect = spanRect(line, ts);
            } else if (ns.valid && col >= ns.start && col < ns.end) {
                if (isHexPreview(lm->nodeKind)) {
                    r.region = QStringLiteral("editor.asciiPreview");
                    r.description = QStringLiteral("ASCII preview — printable character display for hex nodes");
                    r.globalRect = spanRect(line, ns);
                } else {
                    r.region = QStringLiteral("editor.nameColumn");
                    r.description = QStringLiteral("Name column — field names");
                    r.globalRect = spanRect(line, ns);
                }
            } else if (vs.valid && col >= vs.start && col < vs.end) {
                if (isHexPreview(lm->nodeKind)) {
                    r.region = QStringLiteral("editor.hexBytes");
                    r.description = QStringLiteral("Hex bytes — raw byte values (00 FF A3 ...)");
                    r.globalRect = spanRect(line, vs);
                } else {
                    r.region = QStringLiteral("editor.valueColumn");
                    r.description = QStringLiteral("Value column — live memory values");
                    r.globalRect = spanRect(line, vs);
                    if (lm->heatLevel > 0)
                        r.properties["heatLevel"] = lm->heatLevel;
                }
            } else {
                r.region = QStringLiteral("editor.background");
                r.description = QStringLiteral("Editor background — empty area");
                r.globalRect = lineRect(line);
            }

            // Extra context from LineMeta
            r.properties["line"] = line;
            r.properties["lineKind"] = (int)lm->lineKind;
            if (lm->nodeId != 0)
                r.properties["nodeId"] = QString::number(lm->nodeId);
            if (lm->nodeKind != NodeKind::Int32)
                r.properties["nodeKind"] = QString::fromLatin1(kindToString(lm->nodeKind));
        } else {
            r.region = QStringLiteral("editor.background");
            r.description = QStringLiteral("Editor background — below content");
            r.globalRect = QRect(vp->mapToGlobal(QPoint(0, 0)), vp->size());
        }

        // Editor-specific properties
        r.properties["fontSize"] = sci->font().pointSize();
        r.properties["fontFamily"] = sci->font().family();
        r.properties["lineHeight"] = lineH;
        r.properties["extraAscent"] = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETEXTRAASCENT);
        r.properties["extraDescent"] = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETEXTRADESCENT);

        r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
        return r;
    }

    // ── Workspace dock ──
    if (m_workspaceDock && m_workspaceDock->isAncestorOf(widget)) {
        if (m_workspaceTree && (widget == m_workspaceTree || m_workspaceTree->isAncestorOf(widget))) {
            r.region = QStringLiteral("workspace.tree");
            r.description = QStringLiteral("Project tree — struct/type list");
        } else {
            r.region = QStringLiteral("workspace.tree");
            r.description = QStringLiteral("Workspace dock");
        }
        r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
        return r;
    }

    // ── Scanner dock ──
    if (m_scannerDock && m_scannerDock->isAncestorOf(widget)) {
        r.region = QStringLiteral("scanner");
        r.description = QStringLiteral("Scanner panel — value/pattern search");
        r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
        return r;
    }

    // ── Symbols dock ──
    if (m_symbolsDock && m_symbolsDock->isAncestorOf(widget)) {
        r.region = QStringLiteral("symbols.tree");
        r.description = QStringLiteral("Symbols/modules panel");
        r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
        return r;
    }

    // ── Status bar ──
    if (statusBar() && statusBar()->isAncestorOf(widget)) {
        r.region = QStringLiteral("statusBar");
        r.description = QStringLiteral("Status bar — app status, MCP activity");
        r.properties["height"] = statusBar()->height();
        r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
        return r;
    }

    // ── Dock tab bar ──
    if (auto* tabBar = qobject_cast<QTabBar*>(widget)) {
        if (tabBar->parent() == this) {
            r.region = QStringLiteral("dockTabBar");
            r.description = QStringLiteral("Document tab bar — switch between open types");
            r.properties["tabCount"] = tabBar->count();
            r.properties["tabHeight"] = tabBar->height();
            r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
            return r;
        }
    }
    // Tab bar child widgets (close buttons etc.)
    for (QWidget* p = widget->parentWidget(); p; p = p->parentWidget()) {
        if (auto* tabBar = qobject_cast<QTabBar*>(p)) {
            if (tabBar->parent() == this) {
                r.region = QStringLiteral("dockTabBar");
                r.description = QStringLiteral("Document tab bar");
                r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
                return r;
            }
        }
    }

    // ── Menu bar ──
    if (m_menuBar && (widget == m_menuBar || m_menuBar->isAncestorOf(widget))) {
        r.region = QStringLiteral("menuBar");
        r.description = QStringLiteral("Menu bar");
        r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
        return r;
    }
    if (qobject_cast<QMenu*>(widget)) {
        r.region = QStringLiteral("menuBar");
        r.description = QStringLiteral("Menu popup");
        r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
        return r;
    }

    // ── Title bar ──
    // ── Ribbon ──
    if (m_ribbon && (widget == m_ribbon || m_ribbon->isAncestorOf(widget))) {
        r.region = QStringLiteral("ribbon");
        const QString item = m_ribbon->itemIdAt(
            m_ribbon->mapFromGlobal(widget->mapToGlobal(localPos)));
        r.description = item.isEmpty()
            ? QStringLiteral("Ribbon (%1 tab)").arg(m_ribbon->currentTab())
            : QStringLiteral("Ribbon button: %1").arg(item);
        r.properties[QStringLiteral("tab")] = m_ribbon->currentTab();
        r.properties[QStringLiteral("labelMode")] = int(m_ribbon->labelMode());
        r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
        return r;
    }

    if (m_titleBar && m_titleBar->isAncestorOf(widget)) {
        r.region = QStringLiteral("menuBar");
        r.description = QStringLiteral("Title bar / menu bar");
        r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
        return r;
    }

    // ── Main window border ──
    if (widget == m_borderOverlay) {
        r.region = QStringLiteral("mainWindow.border");
        r.description = QStringLiteral("Window border");
        r.themeColors = themeColorsForKeys(themeKeysForRegion(r.region));
        return r;
    }

    // ── Fallback ──
    r.region = r.widgetName;
    r.description = QStringLiteral("Unknown widget: ") + r.widgetName;
    // Return all theme colors as fallback
    r.themeColors = themeColorsForKeys({"background", "text", "textDim", "hover", "border"});
    return r;
}

void MainWindow::showStartPage() {
    if (m_startPage) return;

    // Preload a new class behind the splash so dismissing without a card
    // choice lands on something rather than a blank window. Only fires
    // when no tabs exist (the app-startup path); mid-session File →
    // Welcome leaves existing tabs untouched. The other cards (Open,
    // Import*, recent files, Tutorial) auto-replace the preloaded tab
    // when their action succeeds (closeAllDocDocks runs inside
    // project_open / the import_* paths).
    // The preload is the live bouncing-ball example: a first launch should
    // land on something that moves, not on sixteen zero hex rows.
    const bool preloadedNewClass = m_tabs.isEmpty();
    if (preloadedNewClass)
        project_newSimulation();

    m_startPage = new StartPageWidget(this);
    m_startPage->applyTheme(ThemeManager::instance().current());

    // Size the popup to ~90% of the main window
    QSize sz(qBound(900, int(width() * 0.9), width() - 20),
             qBound(560, int(height() * 0.85), height() - 20));
    m_startPage->setFixedSize(sz);

    // Explicit "New Class" card: if the preload already provided a fresh
    // class (startup path), just dismiss and the user lands on it. If no
    // preload happened (mid-session Welcome over existing tabs), create
    // one so the click actually produces a class.
    // "New Class" at startup lands on the example that is already open —
    // no blank class stacked beside it. Mid-session Welcome (nothing was
    // preloaded) still creates one.
    connect(m_startPage, &StartPageWidget::newClass, this, [this, preloadedNewClass]() {
        dismissStartPage();
        if (!preloadedNewClass) newClass();
    });
    // Esc or outside-click — dismiss only. If the session has no tabs at
    // dismiss time, open the example so they don't land on a blank window.
    connect(m_startPage, &StartPageWidget::dismissed, this, [this]() {
        dismissStartPage();
        if (m_tabs.isEmpty())
            project_newSimulation();
    });
    connect(m_startPage, &StartPageWidget::openProject, this, [this]() {
        dismissStartPage();
        openFile();  // cancel → preloaded stays; success → replaced
    });
    connect(m_startPage, &StartPageWidget::importSource, this, [this]() {
        dismissStartPage();
        importFromSource();
    });
    connect(m_startPage, &StartPageWidget::importXml, this, [this]() {
        dismissStartPage();
        importReclassXml();
    });
    connect(m_startPage, &StartPageWidget::importPdb, this, [this]() {
        dismissStartPage();
        importPdb();
    });
    connect(m_startPage, &StartPageWidget::continueClicked, this, [this]() {
        dismissStartPage();
        // selfTest creates its own demo tabs; drop the preloaded class first
        // so we don't stack an unrelated scratch class on top of the demo.
        { ClosingGuard guard(m_closingAll); closeAllDocDocks(); }
        selfTest();
    });
    connect(m_startPage, &StartPageWidget::fileSelected, this, [this](const QString& path) {
        dismissStartPage();
        project_open(path);
    });
    connect(m_startPage, &QDialog::rejected, this, [this]() {
        dismissStartPage();
    });

    // Center over main window. Shown non-modally so clicks on the main
    // window reach StartPageWidget's qApp-level event filter — QDialog::open()
    // makes it window-modal, which filters outside-clicks away before our
    // filter can see them. The filter in startpage.h now catches any click
    // outside the splash rect and triggers newClass().
    m_startPage->move(geometry().center() - m_startPage->rect().center());
    m_startPage->show();
    m_startPage->raise();
    m_startPage->activateWindow();
}

void MainWindow::dismissStartPage() {
    if (!m_startPage) return;
    auto* sp = m_startPage;
    m_startPage = nullptr;  // null first — close() may re-enter via rejected signal
    sp->close();
    sp->deleteLater();
}

} // namespace rcx

// ── Entry point ──

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#endif
#if defined(__linux__) || defined(__APPLE__)
    installPosixCrashHandler();
#endif
#ifdef Q_OS_MACOS
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
#endif
#ifdef _WIN32
    // GUI-subsystem build: no console at launch (no flash). If the user left
    // the console toggled on (View ▸ Show Console persists the choice),
    // summon one now. QSettings with explicit org/app works pre-QApplication.
    if (QSettings("RC", "RC").value("showConsole", false).toBool())
        rcxSetConsoleVisible(true);
#endif

    DarkApp app(argc, argv);
    app.setApplicationName("RC");
    app.setOrganizationName("RC");
    app.setStyle(new MenuBarStyle("Fusion")); // Fusion + generous menu sizing

    // Replace Qt's default tooltips with RcxTooltip everywhere. See class
    // comment for the focus/flicker bug this fixes.
    qApp->installEventFilter(new rcx::GlobalTooltipBridge(&app));

    // ── Profiler bootstrap ──
    // Enable PROFILE_SCOPE recording before anything else runs so init paths
    // (theme apply, font registration, popup preload, MainWindow ctor) all
    // get measured. The dialog auto-pops below after the window shows, so
    // the user sees results immediately without navigating to View > Profiler.
    //
    // Opt-in via `--profile` only — default startup is clean (no
    // dialog, no overhead). View > Profiler still works for ad-hoc
    // measurement, and Profiler::instance().setEnabled(true) inside
    // showProfilerDialog enables sample collection from that point
    // forward. The early-enable + auto-pop is kept here strictly for
    // measuring init-path costs that happen before the user can click.
    // --screenshot also disables it so captures stay clean.
    const bool hasProfile    = app.arguments().contains(QStringLiteral("--profile"));
    const bool hasScreenshot = app.arguments().contains(QStringLiteral("--screenshot"));
    const bool autoProfile   = hasProfile && !hasScreenshot;  // GUI dialog auto-pop
    // `--profile --screenshot <path>`: headless mode — record the init path,
    // then dump scope timings to stderr at the screenshot-exit (below) instead
    // of popping the dialog. Lets the startup profile be captured by a pipe.
    const bool profileDump   = hasProfile &&  hasScreenshot;
    if (autoProfile || profileDump)
        rcx::Profiler::instance().setEnabled(true);

    // Load embedded fonts
    if (QFontDatabase::addApplicationFont(":/fonts/JetBrainsMono.ttf") == -1)
        qWarning("Failed to load embedded JetBrains Mono font");
    if (QFontDatabase::addApplicationFont(":/fonts/IBMPlexMono.ttf") == -1)
        qWarning("Failed to load embedded IBM Plex Mono font");
    // Departure Mono backs the ribbon's type glyphs (H64 / PTR / WSTR …).
    // rcx::pixelLabelFont() loads it lazily on first use so tests and render
    // harnesses get it too; this is just the eager path with a warning, so a
    // missing resource is noisy rather than a silent fallback to Courier.
    if (QFontDatabase::addApplicationFont(":/fonts/DepartureMono.otf") == -1)
        qWarning("Failed to load embedded Departure Mono font");
    // Apply saved font preference before creating any editors
    {
        QSettings settings("RC", "RC");
        QString savedFont = settings.value("font", "JetBrains Mono").toString();
        rcx::RcxEditor::setGlobalFontName(savedFont);
        rcx::fmt::setFloatDecimals(
            settings.value("floatDecimals", rcx::fmt::kDefaultFloatDecimals).toInt());
        // One chrome font for the whole app: the menu bar, menu popups, every
        // QLabel/QToolButton and the rail inherit mono 10 pt (the address bar
        // sets chromeFont() itself)
        // instead of each re-deriving it (or silently falling back to the
        // system UI font at a different size). The per-widget 10-pt setFont
        // calls scattered through MainWindow become no-ops.
        QFont base(savedFont, 10);
        base.setFixedPitch(true);
        app.setFont(base);
    }

    // Global theme — first call is intentionally a no-op-on-empty-app
    // (no widgets yet to propagate to). The expensive call lives in
    // MainWindow::applyTheme(), which fires after dock + menu + status
    // bar are built. Removed the cheap pre-MainWindow call: 1 ms saved
    // and one fewer redundant qApp->setPalette() pass.
    //
    // TypeSelectorPopup::preload() used to run here and cost ~90 ms of
    // Qt DLL/style cold-start. Deferred to a QTimer::singleShot(0, …)
    // below so window exposure / first paint don't wait on it. The
    // first popup open still pays the cost, but the user sees the
    // window itself ~90 ms sooner.

    rcx::MainWindow window;
    window.setWindowIcon(QIcon(":/icons/class.png"));

    window.show();
#ifdef __linux__
    window.showMaximized();
#endif

    // ── Deferred startup work ──
    // These chunks were running synchronously before window.show() and
    // adding ~130 ms to perceived startup. Both are safe to defer:
    //   * TypeSelectorPopup::preload — burns Qt's one-time DLL/style
    //     cold-start (~90 ms). The popup isn't shown until the user
    //     clicks a type cell, so the cost shifts off the critical path.
    //   * PluginManager::LoadPlugins — dlopens every plugin in Plugins/
    //     (~40 ms). Process attach is the first place a plugin matters,
    //     and that's interactive — a single tick after first paint is
    //     plenty. MCP bridge auto-start uses plugin info, so kick that
    //     too in the same callback if it was scheduled.
    QTimer::singleShot(0, &window, []() {
        rcx::TypeSelectorPopup::preload();
    });
    QTimer::singleShot(0, &window, [&window]() {
        window.loadPluginsDeferred();
    });
    // Build the ScannerPanel widget after first paint so its ~195 ms
    // of FilterChip + scan button + results table construction doesn't
    // gate the user seeing the window. The dock shell already exists
    // (built by createScannerDock during MainWindow::ctor) so menu
    // wiring + layout are already correct; we're just lazy-filling
    // the dock's content. View > Scanner triggers the same
    // build immediately if the user beats this timer to the punch.
    QTimer::singleShot(0, &window, [&window]() {
        window.ensureScannerPanel();
    });
    // Same lazy-fill pattern for the Bookmarks dock — its first setWidget()
    // polish was ~300 ms on the critical path (first stylesheet'd widgets get
    // styled there) for a dock that's hidden by default. Shell already built
    // in the ctor; visibilityChanged populates it if the user opens it first.
    QTimer::singleShot(0, &window, [&window]() {
        window.populateBookmarksDock();
    });
    // Symbols/Modules dock is also lazy. Same idea: hidden at startup,
    // ~360 ms of QTreeView/QTabWidget/QStandardItemModel construction
    // moved off the path-to-first-paint. The View > Modules action
    // also calls this if the user beats the timer.
    QTimer::singleShot(0, &window, [&window]() {
        window.createSymbolsDock();
    });

    // Pop the profiler dialog right after the main window is up. Deferred
    // through the event loop so window exposure / first compose lands in
    // the profile snapshot before the dialog reads it. Same gate as the
    // setEnabled call above. invokeMethod (rather than a direct call)
    // because showProfilerDialog is a private slot — Qt's meta system
    // bypasses access control for slot invocation, no need to leak the
    // method into the public API just for one auto-launch hook.
    if (autoProfile) {
        QTimer::singleShot(0, &window, [&window]() {
            QMetaObject::invokeMethod(&window, "showProfilerDialog",
                                      Qt::QueuedConnection);
        });
    }

    // --screenshot <path> [scanner|workspace|both|closetest|code|splash|
    //                      symbols|home|modify|collapsed]: open the default
    // project (optionally also show the scanner dock, expand the Project
    // workspace dock, lay out the "Both" Reclass|Code split, or force a
    // ribbon tab / the collapsed ribbon), grab the window, save, exit.
    {
        QStringList args = app.arguments();
        int ssIdx = args.indexOf("--screenshot");
        if (ssIdx >= 0 && ssIdx + 1 < args.size()) {
            QString ssPath = args[ssIdx + 1];
            bool showScanner = (ssIdx + 2 < args.size()
                                 && args[ssIdx + 2] == "scanner");
            bool showWorkspace = (ssIdx + 2 < args.size()
                                 && args[ssIdx + 2] == "workspace");
            bool showBoth = (ssIdx + 2 < args.size()
                                 && args[ssIdx + 2] == "both");
            bool closeTest = (ssIdx + 2 < args.size()
                                 && args[ssIdx + 2] == "closetest");
            bool showCode = (ssIdx + 2 < args.size()
                                 && args[ssIdx + 2] == "code");
            bool showSplash = (ssIdx + 2 < args.size()
                                 && args[ssIdx + 2] == "splash");
            bool showSymbols = (ssIdx + 2 < args.size()
                                 && args[ssIdx + 2] == "symbols");
            bool showFontMenu = (ssIdx + 2 < args.size()
                                 && args[ssIdx + 2] == "fontmenu");
            bool showNodeMenu = (ssIdx + 2 < args.size()
                                 && args[ssIdx + 2] == "nodemenu");
            bool showInstances = (ssIdx + 2 < args.size()
                                   && args[ssIdx + 2] == "instances");
            bool showLargeFile = (ssIdx + 2 < args.size()
                                   && args[ssIdx + 2] == "largefile");
            bool showTimeline = (ssIdx + 2 < args.size()
                                  && args[ssIdx + 2] == "timeline");
            // The example a first launch opens on, after it has run a while.
            bool showSim = (ssIdx + 2 < args.size()
                             && (args[ssIdx + 2] == "sim" || args[ssIdx + 2] == "simtimeline"));
            // Ribbon captures: force a tab / the collapsed state for the shot.
            // The pre-existing settings are restored before quitting so a
            // screenshot run never rewrites the user's ribbon preferences.
            const QString ribbonMode = (ssIdx + 2 < args.size()) ? args[ssIdx + 2] : QString();
            const bool ribbonShot = (ribbonMode == "home" || ribbonMode == "modify"
                                     || ribbonMode == "collapsed");
            QVariant savedTab, savedState, savedScope;
            {
                QSettings rs("RC", "RC");
                if (ribbonShot) {
                    savedTab = rs.value("ribbonTab");
                    savedState = rs.value("ribbonState");
                }
                // previewCodeView drives the REAL command, which persists the
                // scope like a user click would — so a capture run would
                // silently rewrite the user's setting. Save it here, restore
                // below, same as the ribbon keys.
                if (showCode) savedScope = rs.value("codeScope");
            }
            QMetaObject::invokeMethod(&window, [&window, ssPath, showScanner, showWorkspace, showBoth, closeTest, showCode, showSplash, showSymbols, showFontMenu, showNodeMenu, showInstances, showLargeFile, showTimeline, showSim, ribbonMode, ribbonShot, savedTab, savedState, savedScope]() {
                if (showSplash) {
                    // Capture the start page itself — skip project_new so it
                    // shows the no-tabs landing.
                    QMetaObject::invokeMethod(&window, "showStartPage", Qt::QueuedConnection);
                } else {
                    if (showSim && ribbonMode == QLatin1String("simtimeline")) {
                        // Takes its own screenshots on its own schedule.
                        window.previewSimulationTimeline(ssPath);
                        return;
                    }
                    if (showSim) {
                        window.project_newSimulation();
                        QTimer::singleShot(3500, &window, [&window, ssPath]() {
                            window.grab().save(ssPath);
                            QApplication::quit();
                        });
                        return;
                    }
                    if (showLargeFile) window.previewLargeFile();
                    else window.project_new();
                    if (showInstances) window.previewInstances();
                    if (showTimeline) {
                        // Takes its own screenshots on its own schedule.
                        window.previewTimeline(ssPath);
                        return;
                    }
                    if (showScanner && window.scannerDock())
                        window.scannerDock()->show();
                    if (showWorkspace)
                        window.applyLayoutPreset(rcx::Layout_Workspace);
                    if (showBoth)
                        window.previewBothSplit();
                    if (showCode)
                        window.previewCodeView();
                    if (closeTest)
                        window.previewCloseViaX();
                    if (showSymbols)
                        window.previewSymbolsDock();
                    if (showFontMenu)
                        window.previewFontMenu();
                    if (showNodeMenu) {
                        // exec() blocks, so the photograph is taken from a
                        // timer and the popup closed to let it return. Zero
                        // delay, not a wait: with no pointer on the capture
                        // desktop the popup dismisses itself within a frame,
                        // so the grab has to happen in exec()'s first turn.
                        QTimer::singleShot(0, &window, [&window, ssPath]() {
                            if (auto* pop = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                                pop->grab().save(ssPath);
                                fprintf(stderr, "nodemenu %d items -> %s\n",
                                        (int)pop->actions().size(), qPrintable(ssPath));
                                pop->close();
                            } else {
                                fprintf(stderr, "nodemenu: no popup\n");
                            }
                            QApplication::quit();
                        });
                        window.previewNodeMenu();
                    }
                    if (ribbonShot && window.ribbon()) {
                        if (ribbonMode == "collapsed") window.ribbon()->setMinimized(true);
                        else window.ribbon()->setCurrentTab(ribbonMode);
                    }
                }
                // Defer the grab so the dock layout settles + the panel
                // paints its initial state before we capture.
                QTimer::singleShot(1500, &window, [&window, ssPath, showSplash, showFontMenu, ribbonShot, savedTab, savedState, showCode, savedScope]() {
                    QPixmap px;
                    if (showSplash) {
                        if (auto* sp = window.findChild<rcx::StartPageWidget*>())
                            px = sp->grab();   // the splash is a top-level dialog
                        else
                            px = window.grab();
                    } else if (showFontMenu) {
                        // A QMenu is its own top-level window, so window.grab()
                        // never contains it (the same reason tooltip_render
                        // exists). Grab the popup itself.
                        auto* m = window.findChild<QMenu*>(QStringLiteral("rcxViewFontMenu"));
                        px = m ? m->grab() : window.grab();
                    } else {
                        px = window.grab();
                    }
                    px.save(ssPath);
                    // RCX_RIBBON_DEBUG=1: dump the live ribbon geometry to stderr
                    // (chrome layout is only observable in the real window).
                    if (qEnvironmentVariableIsSet("RCX_RIBBON_DEBUG")) {
                        if (auto* rb = window.ribbon()) {
                            const QRect hr = rb->parentWidget() ? rb->parentWidget()->geometry() : QRect();
                            fprintf(stderr,
                                    "ribbon: win=%dx%d host=%d,%d %dx%d ribbon=%d,%d %dx%d natural=%d hint=%dx%d tab=%s overflow=%s font=%s/%d dpr=%.2f\n",
                                    window.width(), window.height(),
                                    hr.x(), hr.y(), hr.width(), hr.height(),
                                    rb->x(), rb->y(), rb->width(), rb->height(),
                                    rb->naturalWidth(), rb->sizeHint().width(), rb->sizeHint().height(),
                                    qPrintable(rb->currentTab()),
                                    qPrintable(rb->overflowedPanelIds().join(',')),
                                    qPrintable(rb->font().family()), rb->font().pointSize(),
                                    rb->devicePixelRatioF());
                        }
                    }
                    // Put the user's ribbon preferences back: a capture run
                    // must not leave the app on the tab it photographed.
                    if (ribbonShot || showCode) {
                        QSettings rs("RC", "RC");
                        if (ribbonShot) {
                            if (savedTab.isValid()) rs.setValue("ribbonTab", savedTab);
                            else rs.remove("ribbonTab");
                            if (savedState.isValid()) rs.setValue("ribbonState", savedState);
                            else rs.remove("ribbonState");
                        }
                        if (showCode) {
                            if (savedScope.isValid()) rs.setValue("codeScope", savedScope);
                            else rs.remove("codeScope");
                        }
                    }
                    // `--profile --screenshot`: emit the init-path timings we
                    // recorded (no-op if profiling wasn't enabled).
                    if (rcx::Profiler::instance().isEnabled())
                        rcx::Profiler::instance().dumpToStderr();
                    qApp->quit();
                });
            }, Qt::QueuedConnection);
            return app.exec();
        }
    }

    // Show VS2022-style start page instead of jumping straight to demo
    QMetaObject::invokeMethod(&window, "showStartPage", Qt::QueuedConnection);

    // Recordings spill to a per-session directory; once startup has settled,
    // sweep the ones crashed sessions left behind.
    QTimer::singleShot(5000, &window, [] { rcx::tl::TimelineService::instance().removeStaleSpillLater(); });

    const int exitCode = app.exec();
    // Queued capture work gets a bounded moment; nothing is kept past exit.
    rcx::tl::TimelineService::instance().shutdown(1500);
    return exitCode;
}

#include "main.moc"
