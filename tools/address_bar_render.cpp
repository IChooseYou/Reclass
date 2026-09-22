// Render harness for rcx::AddressBar (the strip above the command row).
// Builds the bar standalone — no controller, no editor, no NodeTree — feeds
// it a synthetic AddressBarState of 1, 3 and 6 crumbs, and grabs the three
// stacked at 240 / 300 / 480 / 760 / 1080 / 1920 logical px so the overflow
// rule, the seam, the chip's liveness dot and the tone ladder can be
// eyeballed (1080 = the user's ~1080-logical window at 125 %). Folding is
// WIDTH-driven, not a fixed threshold: the 6-crumb trail is ~600 px of
// labels, so its « shows at 1080 and 760 too, while the 3-crumb row first
// folds near 480. Under ~420 px `up` / `hist` go; under the ~395-px floor
// the six fit steps reach (process source + formula base) the narrow-pane
// steps turn — the chip drops its name, then root.chev, then the base
// shows its bare literal, then Forward, and at the 228-px floor
// (AddressBar::kNarrowFloorW) Back and the divider go — so the deepest
// crumb and `recent` stay inside the strip down to that floor (the 240-
// and 300-px sheets show it).
// Needs the "windows" platform (the offscreen plugin isn't installed) —
// run it on the hidden desktop via tools/run_tests_hidden.py's
// run_hidden(). Set QT_SCALE_FACTOR for HiDPI.
//
// Usage: address_bar_render <out-prefix> [themeJsonPath|themeName]
//   themeName matches the "name" field or the basename of a JSON in
//   <exe>/themes or <source>/src/themes/defaults (default: vs.json).
//
// Output: <out-prefix>_<w>.png per width — three bars (1, 3, 6 crumbs) top
// to bottom with a magenta gap between them (never black: black would read
// as an unpainted band) — and, on stdout, per bar the logical itemRect of
// every laid-out id plus the dpr, so a pixel scan of the PNG can crop a
// cell exactly (multiply by the dpr, add the row's device y offset).
//
// The six fit steps bottom out near 395 px with a process source and a
// formula base (nav 48 + divider 13 + chip 110 + root chevron 14 + base
// 102 + « 18 + deepest 68 + recent 22); the narrow-pane steps take that
// down to kNarrowFloorW = 228 (gutter 4 + chip icon 22 + chevron 14 + pad 4
// + bare base 72 + « 18 + deepest 72 + recent 16 + margin 6). Narrower
// still is best effort.
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QtMath>
#include <QPixmap>
#include <QStringConverter>
#include <QTextStream>

#include "core.h"                  // fmt::baseAddressHelpTitle / Body
#include "rcxtooltip.h"
#include "themes/theme.h"
#include "themes/thememanager.h"
#include "widgets/address_bar.h"
#include "widgets/dock_header.h"   // chromeFont

using namespace rcx;

static bool loadThemeFile(const QString& path, Theme& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    out = Theme::fromJson(doc.object());
    return out.background.isValid();
}

static QStringList themeDirs() {
    QStringList dirs;
    dirs << QCoreApplication::applicationDirPath() + QStringLiteral("/themes");
#ifdef RCX_SOURCE_DIR
    dirs << QStringLiteral(RCX_SOURCE_DIR) + QStringLiteral("/src/themes/defaults");
#endif
    return dirs;
}

static bool resolveTheme(const QString& arg, Theme& out) {
    if (arg.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) && QFileInfo::exists(arg))
        return loadThemeFile(arg, out);
    for (const QString& dir : themeDirs()) {
        QDir d(dir);
        if (!d.exists()) continue;
        for (const QString& name : d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            if (QFileInfo(name).completeBaseName().compare(arg, Qt::CaseInsensitive) == 0
                && loadThemeFile(d.filePath(name), out)) return true;
            Theme t;
            if (loadThemeFile(d.filePath(name), t) && t.name.compare(arg, Qt::CaseInsensitive) == 0) {
                out = t;
                return true;
            }
        }
    }
    return false;
}

// A crumb in the production dotted shape ("Class.field" ancestors, a bare
// "Class" deepest); the address is what the per-crumb tooltip prints.
static Crumb crumb(const QString& label, uint64_t index, uint64_t address) {
    Crumb c;
    c.label     = label;
    c.rootId    = index;
    c.classId   = 100 + index;
    c.pointerId = index ? 200 + index : 0;
    c.address   = address;
    c.keyword   = QStringLiteral("class");
    return c;
}

// A live process source over a formula base (so the base segment carries
// its "→ 0x…" resolved-address suffix, the first thing the overflow rule
// takes away) and a trail of `depth` crumbs. Back is on, Forward off, Up
// follows the depth — the cells the app lights at that place.
static AddressBarState stateWithDepth(int depth) {
    AddressBarState s;
    s.sourceName   = QStringLiteral("RC.exe");
    s.sourceKindId = QStringLiteral("processmemory");
    s.liveness     = liveness::Live;
    s.baseAddress  = 0x7FF6DEAD0000ULL;
    s.baseFormula  = QStringLiteral("<RC.exe>+0x1234");
    s.resolvedBase = 0x7FF6DEAD1234ULL;
    s.viewRootId   = 100;
    s.canBack      = true;
    s.canForward   = false;
    s.canUp        = depth > 1;
    static const char* const kTrail[] = {
        "RcxEditor.vptr", "QWidgetPrivate.parent", "QWidget.d_ptr",
        "QObjectPrivate.parent", "QObject.d_ptr", "QObjectData",
    };
    static const char* const kDeepest[] = {
        "RcxEditor", "QWidgetPrivate", "QWidget", "QObjectPrivate", "QObject", "QObjectData",
    };
    const int n = qBound(1, depth, 6);
    for (int i = 0; i < n; ++i) {
        const bool deepest = (i == n - 1);
        s.crumbs << crumb(QString::fromLatin1(deepest ? kDeepest[i] : kTrail[i]), i,
                          s.resolvedBase + 0x100ULL * i);
    }
    s.trailPath = QStringLiteral("RcxEditor");
    for (int i = 0; i + 1 < n; ++i) {
        const QString label = QString::fromLatin1(kTrail[i]);
        s.trailPath += QLatin1Char('.') + label.mid(label.indexOf(QLatin1Char('.')) + 1);
    }
    return s;
}

// Every id the bar can lay out, in strip order; the ones itemRect() answers
// for are the ones laid out at this width.
static QStringList allIds(int crumbCount) {
    QStringList ids{ QStringLiteral("back"), QStringLiteral("fwd"), QStringLiteral("hist"),
                     QStringLiteral("up"), QStringLiteral("src"), QStringLiteral("src.chev"),
                     QStringLiteral("root.chev"), QStringLiteral("base"),
                     QStringLiteral("overflow") };
    for (int i = 0; i < crumbCount; ++i)
        ids << QStringLiteral("crumb:%1").arg(i) << QStringLiteral("chev:%1").arg(i);
    ids << QStringLiteral("space") << QStringLiteral("recent");
    return ids;
}

// ── `edit` mode: the inline overlay, and the help beside it ──
//
// The base edit is where the width regression lived: the field opened at a
// flat 180 px whatever it held, so a 13-character address sat in a box with
// ~78 px of dead air before the "→ 0x…" preview. These sheets are the proof
// it fits now — row 0 at rest, row 1 with the base edit open, row 2 with the
// path edit open — and <prefix>_help.png is the grammar block the field
// shows while you type, rendered in the same chrome face so the second
// column can be checked for alignment.
static int renderEditSheets(QApplication& app, QTextStream& out,
                            const QString& prefix, bool haveTheme, const Theme& theme) {
    for (int w : {480, 760, 1080}) {
        QVector<QPixmap> rows;
        QStringList reports;
        // rest | base edit on a bare literal (the user's case in
        // build/issue.png) | base edit on a long formula | path edit |
        // the deepest crumb renaming its class.
        for (int mode = 0; mode < 5; ++mode) {
            AddressBar bar;
            if (haveTheme) bar.applyTheme(theme);
            {   // an evaluator, so the "= 0x…" preview is exercised
                AddressBar::Callbacks cb;
                cb.evaluate = [](const QString& t) {
                    return t.startsWith(QLatin1Char('<')) ? QStringLiteral("0x7FF6DEAD1234")
                                                          : QString();
                };
                bar.setCallbacks(std::move(cb));
            }
            AddressBarState st = stateWithDepth(3);
            if (mode == 1) {
                st.baseFormula.clear();
                st.baseAddress = st.resolvedBase = 0x279B3D07010ULL;
            }
            bar.setState(st);
            bar.show();
            bar.resize(w, AddressBar::kAddressBarHeight);
            app.processEvents();
            // The cell the overlay is about to replace, measured BEFORE it
            // opens: an edit that resizes anything shows up here as a
            // difference, and "cell == edit" is the whole contract.
            const QRect cellBefore = bar.itemRect(mode == 4 ? bar.deepestCrumbId()
                                                            : QStringLiteral("base"));
            if (mode == 1 || mode == 2) bar.beginBaseEdit();
            if (mode == 3) bar.beginPathEdit();
            if (mode == 4) bar.beginClassNameEdit();
            app.processEvents();
            app.processEvents();
            rows << bar.grab();
            QString rep;
            QTextStream rs(&rep);
            const QRect e = bar.editRect(), c = bar.editCoveredRect();
            const QRect recent = bar.itemRect(QStringLiteral("recent"));
            rs << "  mode=" << (mode == 0 ? "rest" : mode == 1 ? "base-literal"
                                                      : mode == 2 ? "base-formula"
                                                      : mode == 3 ? "path" : "class-rename")
               << "  text=\"" << bar.editText() << "\"";
            if (!e.isNull()) {
                rs << "  cell=" << cellBefore.x() << "," << cellBefore.width()
                   << "  edit=" << e.x() << "," << e.width()
                   << "  dx=" << (e.x() - cellBefore.x())
                   << "  dw=" << (e.width() - cellBefore.width())
                   << "  covered=" << c.x() << "," << c.width()
                   << "  preview=\"" << bar.editPreviewText() << "\""
                   << "  after=" << (c.right() - e.right())
                   << "  recent.left=" << recent.left()
                   << "  help=" << (bar.editHelpVisible() ? "up" : "DOWN");
            }
            reports << rep;
            bar.hide();
        }
        const qreal dpr = rows.first().devicePixelRatio();
        const int gapDev = qRound(3 * dpr);
        int sheetH = gapDev * (rows.size() - 1);
        for (const QPixmap& r : rows) sheetH += r.height();
        QImage sheet(rows.first().width(), sheetH, QImage::Format_ARGB32);
        sheet.fill(QColor(255, 0, 255));
        QVector<int> rowTopDev;
        {
            QPainter sp(&sheet);
            int y = 0;
            for (const QPixmap& r : rows) {
                QImage ri = r.toImage();
                ri.setDevicePixelRatio(1.0);
                sp.drawImage(0, y, ri);
                rowTopDev << y;
                y += ri.height() + gapDev;
            }
        }
        const QString path = QStringLiteral("%1_edit_%2.png").arg(prefix).arg(w);
        sheet.save(path);
        out << path << "  width=" << w << "  dpr=" << dpr << "\n";
        for (int i = 0; i < reports.size(); ++i)
            out << "  row " << i << " yDev=" << rowTopDev[i] << reports[i] << "\n";
    }

    // ── The contract, in pixels ──
    //
    // Clicking into a segment must change the ring and the selection and
    // NOTHING ELSE. Rects proving dx == 0 and dw == 0 are half the story; this
    // is the other half — the same bar grabbed at rest and mid-edit, differing
    // columns counted. Every differing column must fall inside the segment
    // being edited (its selection band and caret); a single one outside it is
    // the line having been shoved.
    for (int scope = 0; scope < 2; ++scope) {
        AddressBar bar;
        if (haveTheme) bar.applyTheme(theme);
        AddressBarState st = stateWithDepth(3);
        st.baseFormula.clear();
        st.baseAddress = st.resolvedBase = 0x279B3D07010ULL;
        bar.setState(st);
        bar.show();
        bar.resize(1080, AddressBar::kAddressBarHeight);
        app.processEvents();
        app.processEvents();
        const QRect cell = bar.itemRect(scope == 0 ? QStringLiteral("base")
                                                   : bar.deepestCrumbId());
        const QImage rest = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        if (scope == 0) bar.beginBaseEdit(); else bar.beginClassNameEdit();
        app.processEvents();
        app.processEvents();
        const QImage edit = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        const qreal dpr = bar.devicePixelRatioF();
        const int cellL = qFloor(cell.left() * dpr), cellR = qCeil((cell.right() + 1) * dpr);
        int outside = 0, firstOutside = -1, inside = 0;
        for (int x = 0; x < qMin(rest.width(), edit.width()); ++x) {
            bool diff = false;
            // The bottom two device rows are the focus seam — the ring IS
            // what clicking in is supposed to draw. Everything above it is
            // content, and content must not move.
            const int h = qMin(rest.height(), edit.height()) - qCeil(2 * dpr);
            for (int y = 0; y < h && !diff; ++y)
                diff = rest.pixel(x, y) != edit.pixel(x, y);
            if (!diff) continue;
            if (x >= cellL && x < cellR) { ++inside; continue; }
            ++outside;
            if (firstOutside < 0) firstOutside = x;
        }
        out << "  open-" << (scope == 0 ? "base " : "class")
            << "  cellDev=[" << cellL << "," << cellR << ")"
            << "  changed inside=" << inside << "  OUTSIDE=" << outside;
        if (outside) out << "  firstOutsideCol=" << firstOutside;
        out << (outside ? "   <-- the line moved\n"
                          : "   ok: nothing outside the segment moved\n");
        bar.hide();
    }

    // The help block on its own. Columns line up only in a fixed-pitch face,
    // which is what chromeFont() is; this sheet is where that is checked.
    RcxTooltip tip;
    const Theme& t = haveTheme ? theme : ThemeManager::instance().current();
    tip.setTheme(t.backgroundAlt, t.border, t.text, t.text, t.border);
    tip.populate(fmt::baseAddressHelpTitle(), fmt::baseAddressHelpBody(), chromeFont());
    tip.showAt(QPoint(400, 400));
    app.processEvents();
    app.processEvents();
    const QString hp = QStringLiteral("%1_help.png").arg(prefix);
    tip.grab().save(hp);
    out << hp << "  size=" << tip.width() << "x" << tip.height()
        << "  font=" << chromeFont().family() << "  fixedPitch="
        << (QFontMetrics(chromeFont()).horizontalAdvance(QStringLiteral("iiii"))
            == QFontMetrics(chromeFont()).horizontalAdvance(QStringLiteral("WWWW")) ? "yes" : "NO")
        << "\n";
    tip.dismiss();
    out.flush();
    return 0;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/JetBrainsMono.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexMono.ttf"));

    QTextStream out(stdout);
    // The report quotes « and …; the locale codec turned both into '?'
    // once stdout was a file (the driver redirects it on the hidden desktop).
    out.setEncoding(QStringConverter::Utf8);
    if (argc < 2) {
        out << "usage: address_bar_render <outPrefix> [themeName|path] [edit]\n";
        out.flush();
        return 2;
    }
    const QString prefix = QString::fromLocal8Bit(argv[1]);

    Theme theme;
    bool haveTheme = false;
    if (argc > 2) {
        haveTheme = resolveTheme(QString::fromLocal8Bit(argv[2]), theme);
        if (!haveTheme) out << "theme not found: " << argv[2] << " — using the default\n";
    }
    if (!haveTheme) haveTheme = resolveTheme(QStringLiteral("vs"), theme);

    // `edit`: the inline overlay sheets instead of the at-rest ones.
    if (argc > 3 && QString::fromLocal8Bit(argv[3]) == QLatin1String("edit"))
        return renderEditSheets(app, out, prefix, haveTheme, theme);

    const int depths[] = { 1, 3, 6 };
    const int gap = 3;   // magenta rows between the stacked bars

    AddressBar probe;
    if (haveTheme) probe.applyTheme(theme);
    out << "theme: " << probe.theme().name << "  dpr: " << probe.devicePixelRatioF()
        << "  height: " << AddressBar::kAddressBarHeight
        << "  font: " << probe.font().family() << " " << probe.font().pointSizeF() << "pt\n";

    for (int w : {240, 300, 480, 760, 1080, 1920}) {
        QVector<QPixmap> rows;
        QStringList reports;
        for (int depth : depths) {
            AddressBar bar;
            if (haveTheme) bar.applyTheme(theme);
            bar.setState(stateWithDepth(depth));
            bar.show();
            bar.resize(w, AddressBar::kAddressBarHeight);
            app.processEvents();
            app.processEvents();
            rows << bar.grab();
            QString rep;
            QTextStream rs(&rep);
            rs << "  crumbs=" << depth << "  segments=[" << bar.segments().join(QLatin1String(" | "))
               << "]  overflowMenu=[" << bar.overflowMenuLabels().join(QLatin1String(" | "))
               << "]  src=\"" << bar.sourceDisplayText() << "\"  base=\"" << bar.baseDisplayText()
               << "\"\n    rects:";
            for (const QString& id : allIds(depth)) {
                const QRect r = bar.itemRect(id);
                if (!r.isNull())
                    rs << " " << id << "=" << r.x() << "," << r.y() << "," << r.width() << "," << r.height();
            }
            const QRect dot = bar.livenessDotRect();
            if (!dot.isNull())
                rs << " dot=" << dot.x() << "," << dot.y() << "," << dot.width() << "," << dot.height();
            reports << rep;
            bar.hide();
        }
        // Stacked in DEVICE pixels: each grab is copied whole at an integer
        // device row, so every bar keeps its own seam row and a logical
        // offset rounded through the dpr can never crop the last one.
        const qreal dpr = rows.first().devicePixelRatio();
        const int gapDev = qRound(gap * dpr);
        int sheetH = gapDev * (rows.size() - 1);
        for (const QPixmap& r : rows) sheetH += r.height();
        QImage sheet(rows.first().width(), sheetH, QImage::Format_ARGB32);
        sheet.fill(QColor(255, 0, 255));
        QVector<int> rowTopDev;
        {
            QPainter sp(&sheet);
            int y = 0;
            for (const QPixmap& r : rows) {
                QImage ri = r.toImage();
                ri.setDevicePixelRatio(1.0);
                sp.drawImage(0, y, ri);
                rowTopDev << y;
                y += ri.height() + gapDev;
            }
        }
        const QString path = QStringLiteral("%1_%2.png").arg(prefix).arg(w);
        sheet.save(path);
        out << path << "  width=" << w << "  dpr=" << dpr << "  rowH=" << AddressBar::kAddressBarHeight
            << "  rowHdev=" << rows.first().height() << "  gapDev=" << gapDev << "\n";
        for (int i = 0; i < reports.size(); ++i)
            out << "  row " << i << " yDev=" << rowTopDev[i] << reports[i] << "\n";
    }
    out.flush();
    return 0;
}
