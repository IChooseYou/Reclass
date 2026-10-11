// Render the compact source chooser on the hidden desktop.
// Usage: sourcechooser_render <prefix> [vs tw] [many|empty|overflow|search|large|cached]
// Images use the same chrome font and source metadata as the real chooser.
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
#include "widgets/dock_header.h"

using namespace rcx;

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
    e.providerTarget     = pid + QChar(':') + name;
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
    entries.append(provider(QStringLiteral("Open File"), QStringLiteral("File"),
                            QStringLiteral("built-in")));
    entries.append(provider(QStringLiteral("Kernel Memory"), QStringLiteral("kernelmemory"),
                            QStringLiteral("libKernelMemoryPlugin.dll")));
    entries.append(provider(QStringLiteral("Process Memory"), QStringLiteral("processmemory"),
                            QStringLiteral("libProcessMemoryPlugin.dll")));
    entries.append(provider(QStringLiteral("ReClass.NET Compat Layer"),
                            QStringLiteral("reclass.netcompatlayer"),
                            QStringLiteral("libRcNetCompatPlugin.dll")));
    entries.append(provider(QStringLiteral("Remote Process Memory"),
                            QStringLiteral("remoteprocessmemory"),
                            QStringLiteral("libRemoteProcessMemoryPlugin.dll")));
    entries.append(provider(QStringLiteral("WinDbg Memory"), QStringLiteral("windbgmemory"),
                            QStringLiteral("libWinDbgMemoryPlugin.dll")));
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
    bool many = false, empty = false, overflow = false, search = false, large = false, cached = false;
    for (int i = 2; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("many")) many = true;
        else if (a == QStringLiteral("empty")) empty = true;
        else if (a == QStringLiteral("overflow")) overflow = true;
        else if (a == QStringLiteral("search")) search = true;
        else if (a == QStringLiteral("large")) large = true;
        else if (a == QStringLiteral("cached")) cached = true;
        else themeArgs << a;
    }
    if (themeArgs.isEmpty()) themeArgs << QStringLiteral("tw") << QStringLiteral("vs");

    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/JetBrainsMono.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexMono.ttf"));
    QFont font = chromeFont();
    if (large) font.setPointSize(18);

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
        // RcxController warms an empty popup before its first real use.
        if (cached) popup.warmUp();
        popup.applyTheme(theme);
        popup.setFont(font);
        auto entries = buildEntries(many);
        if (cached)
            entries.append(provider(QStringLiteral("Example provider"), QStringLiteral("example"),
                                    QStringLiteral("example.dll")));
        if (empty) {
            for (int i = entries.size() - 1; i >= 0; --i)
                if (entries[i].entryKind == SourceEntry::SavedSource) entries.removeAt(i);
        }
        if (overflow)
            for (int i = 0; i < 8; ++i)
                entries.append(provider(QStringLiteral("Transport %1").arg(i),
                                        QStringLiteral("transport%1").arg(i), QStringLiteral("transport.dll")));
        popup.setSources(entries);
        popup.popup(QPoint(120, 120));
        if (search) popup.findChild<QLineEdit*>()->setText(QStringLiteral("process"));
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
        for (auto* button : popup.findChildren<QToolButton*>())
            if (!button->property("providerId").toString().isEmpty())
                out << "  provider " << button->text() << " " << rectStr(inPopup(button, &popup)) << "\n";
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
