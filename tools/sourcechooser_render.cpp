// Offscreen render harness for the SourceChooserPopup. Builds the list the
// controller builds for a live session — a "Connected" section with the
// attached process (PID + arch badges, "active"), the "Add Source" section
// with the built-in File provider and every plugin provider the app ships,
// and Clear All — and grabs the popup to a PNG per theme so the frame, the
// surfaces, the section labels and the two-line cards can be pixel-scanned
// deterministically instead of eyeballed in a live screenshot.
//
// Runs on the default windows platform (the offscreen plugin isn't installed;
// the popup is a transient real window) — drive it on the hidden desktop via
// tools/run_tests_hidden.py's run_hidden(), which is at 125 %, so the grabs
// come back at dpr 1.25 like the user's window.
//
// Usage: sourcechooser_render <out-prefix> [theme ...] [many]
//   theme  a built-in theme's "name" or JSON basename (tw, vs, ...); one PNG
//          per theme at <out-prefix>_<theme>.png. Default: tw vs.
//   many   12 saved sources instead of one, to confirm the list scrolls
//          inside the capped popup height rather than breaking.
//
// The theme is handed to applyTheme() straight from ThemeManager's list —
// never through setCurrent(), which PERSISTS the choice to the QSettings the
// real app reads at startup. The popup paints everything (chrome, frame,
// field seam, rows) from the Theme it was given, so a harness run neither
// needs nor touches the user's app theme.
//
// Stdout, per theme: the popup's logical size, the dpr and the grab's device
// size, then every child's logical rect in popup coordinates (title, Esc,
// filter, list viewport, the vertical scrollbar with its range, footer) and
// each list row's rect, so a pixel scan can crop any part exactly (multiply
// by the dpr).
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QScrollBar>
#include <QSettings>
#include <QTextStream>
#include <QToolButton>
#include "sourcechooserpopup.h"
#include "themes/thememanager.h"

using namespace rcx;

static SourceEntry header(const QString& name) {
    SourceEntry h;
    h.entryKind   = SourceEntry::SectionHeader;
    h.displayName = name;
    h.enabled     = false;
    return h;
}

static SourceEntry saved(const QString& name, const QString& providerId,
                         const QString& pid, const QString& arch,
                         const QString& baseAddr, int savedIndex,
                         bool active, bool stale) {
    SourceEntry e;
    e.entryKind          = SourceEntry::SavedSource;
    e.displayName        = name;
    e.providerIdentifier = providerId;
    e.kindLabel          = kindLabelFor(providerId);
    e.iconPath           = iconForProvider(providerId);
    e.pid                = pid;
    e.arch               = arch;
    e.baseAddress        = baseAddr;
    e.savedIndex         = savedIndex;
    e.isActive           = active;
    e.isStale            = stale;
    return e;
}

// A provider row exactly as RcxController::showSourcePopup builds one from
// the ProviderRegistry: the display name, the identifier that picks the icon
// and kind label, and the DLL the plugin came from.
static SourceEntry provider(const QString& name, const QString& identifier,
                            const QString& dll) {
    SourceEntry p;
    p.entryKind          = SourceEntry::ProviderAction;
    p.displayName        = name;
    p.providerIdentifier = identifier;
    p.kindLabel          = kindLabelFor(identifier);
    p.dllFileName        = dll;
    p.iconPath           = iconForProvider(identifier);
    return p;
}

static QVector<SourceEntry> buildEntries(bool many) {
    QVector<SourceEntry> entries;
    entries.append(header(QStringLiteral("Connected")));
    if (many) {
        for (int i = 0; i < 12; ++i)
            entries.append(saved(QStringLiteral("process_%1.exe").arg(i),
                                  QStringLiteral("processmemory"),
                                  QString::number(1000 + i),
                                  (i % 2) ? QStringLiteral("x86") : QStringLiteral("x64"),
                                  QStringLiteral("0x7FF6%1230000").arg(i, 0, 16),
                                  i, /*active*/ i == 0, /*stale*/ i == 3));
    } else {
        // The live session in build/issue.png: the app attached to itself.
        entries.append(saved(QStringLiteral("RC.exe"),
                              QStringLiteral("processmemory"),
                              QStringLiteral("34856"), QStringLiteral("x64"),
                              QStringLiteral("0x2665EACD160"), 0,
                              /*active*/ true, /*stale*/ false));
    }
    entries.append(header(QStringLiteral("Add Source")));
    entries.append(provider(QStringLiteral("Open File"), QStringLiteral("File"),
                            QStringLiteral("built-in")));
    entries.append(provider(QStringLiteral("Kernel Memory"), QStringLiteral("kernelmemory"),
                            QStringLiteral("libKernelMemoryPlugin.dll")));
    entries.append(provider(QStringLiteral("Process Memory"), QStringLiteral("processmemory"),
                            QStringLiteral("libProcessMemoryPlugin.dll")));
    entries.append(provider(QStringLiteral("ReClass.NET Compat Layer"),
                            QStringLiteral("RC.netcompatlayer"),
                            QStringLiteral("libRcNetCompatPlugin.dll")));
    entries.append(provider(QStringLiteral("Remote Process Memory"),
                            QStringLiteral("remoteprocessmemory"),
                            QStringLiteral("libRemoteProcessMemoryPlugin.dll")));
    entries.append(provider(QStringLiteral("WinDbg Memory"), QStringLiteral("windbgmemory"),
                            QStringLiteral("libWinDbgMemoryPlugin.dll")));
    {
        SourceEntry c;
        c.entryKind   = SourceEntry::ClearAction;
        c.displayName = QStringLiteral("Clear All");
        c.iconPath    = QStringLiteral(":/vsicons/clear-all.svg");
        c.enabled     = true;
        entries.append(c);
    }
    return entries;
}

static int themeIndexFor(const QString& arg) {
    const auto themes = ThemeManager::instance().themes();
    for (int i = 0; i < themes.size(); ++i)
        if (themes[i].name.compare(arg, Qt::CaseInsensitive) == 0) return i;
    // A JSON basename ("tw" is "Light", "vs" is "VS2022 Dark"): the built-in
    // files next to the exe carry the "name" the manager indexes by.
    QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/themes"));
    QFile f(dir.filePath(arg.toLower() + QStringLiteral(".json")));
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QString name = QJsonDocument::fromJson(f.readAll()).object()
                             .value(QStringLiteral("name")).toString();
    for (int i = 0; i < themes.size(); ++i)
        if (!name.isEmpty() && themes[i].name == name) return i;
    return -1;
}

static QString rectStr(const QRect& r) {
    return QStringLiteral("x=%1 y=%2 w=%3 h=%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

static QRect inPopup(const QWidget* w, const QWidget* popup) {
    return QRect(w->mapTo(popup, QPoint(0, 0)), w->size());
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTextStream out(stdout);

    const QString prefix = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                      : QStringLiteral("sourcechooser_render");
    QStringList themeArgs;
    bool many = false;
    for (int i = 2; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("many")) many = true;
        else themeArgs << a;
    }
    if (themeArgs.isEmpty()) themeArgs << QStringLiteral("tw") << QStringLiteral("vs");

    // The controller's font: the app's editor face (QSettings "font",
    // JetBrains Mono by default) at 12 pt, one point smaller for the popup.
    // The app registers its bundled faces at startup (main.cpp); without
    // them the saved name falls back to Courier New, whose shorter line
    // height hides the sizing the user sees (the bundled faces' taller
    // cards push the default list past the popup's height cap).
    for (const char* face : {":/fonts/JetBrainsMono.ttf", ":/fonts/IBMPlexMono.ttf"}) {
        const int id = QFontDatabase::addApplicationFont(QString::fromLatin1(face));
        out << "bundled " << face << " -> "
            << (id >= 0 ? QFontDatabase::applicationFontFamilies(id).join(", ") : QStringLiteral("FAILED"))
            << "\n";
    }
    const QString fontSetting = QSettings(QStringLiteral("RC"), QStringLiteral("RC"))
                                    .value(QStringLiteral("font"), QStringLiteral("JetBrains Mono")).toString();
    QFont font(fontSetting, 11);
    font.setFixedPitch(true);
    out << "font setting: \"" << fontSetting << "\"\n";

    // The app's global scrollbar rule (MainWindow::applyGlobalTheme): the
    // popup's list would inherit it in the app, so the harness wears it too
    // — the popup's local override (applyTheme) has to win over exactly this
    // rule, or the scroll case shows the 8-px solid textFaint bar the user saw.
    auto applyScrollbarQss = [&app](const Theme& t) {
        app.setStyleSheet(QStringLiteral(
            "QScrollBar:vertical { background: palette(window); width: 8px; margin: 0; border: none; }"
            "QScrollBar::handle:vertical { background: %1; min-height: 20px; border: none; }"
            "QScrollBar::handle:vertical:hover { background: %2; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }")
            .arg(t.textFaint.name(), t.textDim.name()));
    };

    int failures = 0;
    for (const QString& themeArg : themeArgs) {
        const int ti = themeIndexFor(themeArg);
        if (ti < 0) { out << "theme not found: " << themeArg << "\n"; ++failures; continue; }
        const Theme theme = ThemeManager::instance().themes()[ti];
        applyScrollbarQss(theme);

        SourceChooserPopup popup;
        popup.applyTheme(theme);
        popup.setFont(font);
        popup.setSources(buildEntries(many));
        popup.popup(QPoint(120, 120));
        app.processEvents();
        app.processEvents();

        const QPixmap grab = popup.grab();
        const QString path = prefix + QLatin1Char('_') + themeArg.toLower() + QStringLiteral(".png");
        if (!grab.save(path)) { out << "save failed: " << path << "\n"; ++failures; }

        out << "theme: " << popup.theme().name
            << "  font: " << QFontInfo(font).family() << " " << QFontInfo(font).pointSize() << "pt\n";
        out << path << "  popup=" << popup.width() << "x" << popup.height()
            << "  dpr=" << grab.devicePixelRatio()
            << "  device=" << grab.width() << "x" << grab.height() << "\n";
        const auto labels = popup.findChildren<QLabel*>();
        for (QLabel* l : labels)
            out << "  label \"" << l->text().left(24) << "\" " << rectStr(inPopup(l, &popup)) << "\n";
        if (auto* esc = popup.findChild<QToolButton*>())
            out << "  esc " << rectStr(inPopup(esc, &popup)) << "\n";
        if (auto* edit = popup.findChild<QLineEdit*>())
            out << "  filter " << rectStr(inPopup(edit, &popup)) << "\n";
        if (auto* view = popup.findChild<QListView*>()) {
            out << "  list " << rectStr(inPopup(view, &popup)) << "\n";
            out << "  viewport " << rectStr(inPopup(view->viewport(), &popup)) << "\n";
            QScrollBar* sb = view->verticalScrollBar();
            out << "  vscroll visible=" << (sb->isVisible() ? 1 : 0)
                << " range=" << sb->minimum() << ".." << sb->maximum()
                << " page=" << sb->pageStep() << " " << rectStr(inPopup(sb, &popup)) << "\n";
            const QPoint vpOrigin = view->viewport()->mapTo(&popup, QPoint(0, 0));
            for (int r = 0; r < view->model()->rowCount(); ++r) {
                const QRect vr = view->visualRect(view->model()->index(r, 0)).translated(vpOrigin);
                out << "  row " << r << " \"" << view->model()->index(r, 0).data().toString()
                    << "\" " << rectStr(vr) << "\n";
            }
        }
        popup.hide();
    }
    return failures;
}
