// Offscreen render harness for the SourceChooserPopup. Builds a representative
// source list (a "Connected" section with several saved sources — one active,
// one stale — plus an "Add Source" provider section and Clear All) and grabs
// the popup to a PNG so the per-row × delete button, the section labels, and
// the two-line card layout can be eyeballed deterministically.
//
// Runs on the default windows platform (the offscreen plugin isn't installed;
// the popup is a transient real window).
//
// Usage: sourcechooser_render <out.png> [many]
//   "many" → 12 saved sources, to confirm the popup scrolls (caps at ~520px)
//            rather than breaking when there are lots of bound sources.
#include <QApplication>
#include <QFont>
#include <QListView>
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

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const QString out  = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                     : QStringLiteral("sourcechooser_render.png");
    const QString mode = (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QString();

    SourceChooserPopup popup;
    popup.applyTheme(ThemeManager::instance().current());
    popup.setFont(QFont(QStringLiteral("Consolas"), 11));

    QVector<SourceEntry> entries;
    entries.append(header(QStringLiteral("Connected")));

    if (mode == QStringLiteral("many")) {
        // 12 saved sources — should scroll inside the capped popup height.
        for (int i = 0; i < 12; ++i)
            entries.append(saved(QStringLiteral("process_%1.exe").arg(i),
                                  QStringLiteral("processmemory"),
                                  QString::number(1000 + i),
                                  (i % 2) ? QStringLiteral("x86") : QStringLiteral("x64"),
                                  QStringLiteral("0x7FF6%1230000").arg(i, 0, 16),
                                  i, /*active*/ i == 0, /*stale*/ i == 3));
    } else {
        entries.append(saved(QStringLiteral("notepad.exe"),
                              QStringLiteral("processmemory"),
                              QStringLiteral("4242"), QStringLiteral("x64"),
                              QStringLiteral("0x7FF612340000"), 0,
                              /*active*/ true, /*stale*/ false));
        entries.append(saved(QStringLiteral("game.exe"),
                              QStringLiteral("processmemory"),
                              QStringLiteral("9001"), QStringLiteral("x86"),
                              QStringLiteral("0x00400000"), 1,
                              /*active*/ false, /*stale*/ true));   // exited
        SourceEntry f = saved(QStringLiteral("dump.bin"), QStringLiteral("File"),
                              QString(), QString(), QString(), 2, false, false);
        f.filePath = QStringLiteral("C:/captures/long/path/to/some/dump.bin");
        entries.append(f);
    }

    // "Add Source" provider section
    entries.append(header(QStringLiteral("Add Source")));
    {
        SourceEntry p;
        p.entryKind          = SourceEntry::ProviderAction;
        p.displayName        = QStringLiteral("Open File");
        p.providerIdentifier = QStringLiteral("File");
        p.kindLabel          = kindLabelFor(QStringLiteral("File"));
        p.dllFileName        = QStringLiteral("built-in");
        p.iconPath           = iconForProvider(QStringLiteral("File"));
        entries.append(p);
    }

    // Clear All
    {
        SourceEntry c;
        c.entryKind   = SourceEntry::ClearAction;
        c.displayName = QStringLiteral("Clear All");
        c.iconPath    = QStringLiteral(":/vsicons/clear-all.svg");
        c.enabled     = true;
        entries.append(c);
    }

    popup.setSources(entries);
    popup.popup(QPoint(120, 120));
    app.processEvents();
    app.processEvents();

    popup.grab().save(out);
    return 0;
}
