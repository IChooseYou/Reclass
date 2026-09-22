// Offscreen render harness for the TypeSelectorPopup. Builds a representative
// full type catalogue (every primitive + a couple user structs + a couple
// std-lib "Common Types"), shows the popup in its default SIMPLE view, and
// grabs it to a PNG — so the "common-by-default + Show all toggle" layout can
// be eyeballed deterministically. Runs on the default windows platform (the
// offscreen plugin isn't installed; the popup is a transient real window).
//
// Usage: typeselector_render <out.png> [mode] [light | <theme>]
//   mode   filter=<text> | expand | perf | selecthdr | toggleclasses | bottom
//          | ptr | arr | detail | select | hex | hexpin (see below)
//   theme  "light" picks the first light built-in; any other word is a
//          built-in theme's "name" or JSON basename (tw, vs, ...). Either
//          goes through setCurrent (the popup's delegate reads the manager)
//          and is restored on exit.
//
// Stdout: the popup's logical size, the dpr and the grab's device size, then
// the filter, list, viewport and scrollbar rects in popup coordinates, so a
// pixel scan can crop the frame, the field seam and the track exactly.
#include <QApplication>
#include <QFont>
#include <QListView>
#include <QLineEdit>
#include <QKeyEvent>
#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QSettings>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollBar>
#include <QLabel>
#include <QMouseEvent>
#include <QTextDocument>
#include <cstdio>
#include "typeselectorpopup.h"
#include "hextoolbarpopup.h"
#include "core.h"
#include "themes/thememanager.h"

using namespace rcx;

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // ThemeManager::setCurrent() PERSISTS the chosen theme to the shared
    // QSettings("RC","RC") that the real app reads at startup — so a
    // harness run that switches theme would flip the user's app theme. Capture
    // the original and restore it on exit so the harness never mutates it.
    const QString kOrigTheme =
        QSettings(QStringLiteral("RC"), QStringLiteral("RC"))
            .value(QStringLiteral("theme")).toString();
    struct ThemeRestorer {
        QString name;
        ~ThemeRestorer() {
            if (!name.isEmpty())
                QSettings(QStringLiteral("RC"), QStringLiteral("RC"))
                    .setValue(QStringLiteral("theme"), name);
        }
    } themeRestorer{kOrigTheme};

    // argv[3] == "light": switch to the first light-background built-in theme
    // so the chooser's new chrome (green ＋ Create row, segment borders) can be
    // checked for contrast/visibility on a light palette, not just dark.
    const QString themeArg = (argc > 3) ? QString::fromLocal8Bit(argv[3]) : QString();
    if (themeArg == QStringLiteral("light")) {
        const auto themes = ThemeManager::instance().themes();
        for (int i = 0; i < themes.size(); ++i) {
            if (themes[i].background.lightnessF() > 0.6) {
                ThemeManager::instance().setCurrent(i);
                break;
            }
        }
    } else if (!themeArg.isEmpty()) {
        // A built-in by "name", or by the JSON basename next to the exe
        // ("tw" is "Light", "vs" is "VS2022 Dark") — sourcechooser_render's
        // lookup, so the two harnesses take the same theme words.
        const auto themes = ThemeManager::instance().themes();
        int idx = -1;
        for (int i = 0; i < themes.size() && idx < 0; ++i)
            if (themes[i].name.compare(themeArg, Qt::CaseInsensitive) == 0) idx = i;
        if (idx < 0) {
            QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/themes/")
                    + themeArg.toLower() + QStringLiteral(".json"));
            if (f.open(QIODevice::ReadOnly)) {
                const QString name = QJsonDocument::fromJson(f.readAll()).object()
                                         .value(QStringLiteral("name")).toString();
                for (int i = 0; i < themes.size() && idx < 0; ++i)
                    if (!name.isEmpty() && themes[i].name == name) idx = i;
            }
        }
        if (idx < 0) { std::fprintf(stderr, "theme not found: %s\n", qPrintable(themeArg)); return 2; }
        ThemeManager::instance().setCurrent(idx);
    }

    TypeSelectorPopup popup;
    popup.applyTheme(ThemeManager::instance().current());
    popup.setFont(QFont(QStringLiteral("Consolas"), 10));
    popup.setMode(TypePopupMode::FieldType);

    // Full catalogue, mirroring RcxController::showTypePopup: every primitive
    // (except the Struct/Array containers), plus a couple user structs and a
    // couple std-lib "Common Types" (kindGroup="Common"). Simple mode should
    // render only the common subset + the structs + a "Show all types" row.
    QVector<TypeEntry> types;
    for (const auto& m : kKindMeta) {
        if (m.kind == NodeKind::Struct || m.kind == NodeKind::Array) continue;
        TypeEntry e;
        e.entryKind     = TypeEntry::Primitive;
        e.primitiveKind = m.kind;
        e.displayName   = QString::fromLatin1(m.typeName);
        e.sizeBytes     = m.size;
        e.alignment     = m.align;
        types.append(e);
    }
    auto composite = [&](const QString& name, const QString& group) {
        TypeEntry e;
        e.entryKind    = TypeEntry::Composite;
        e.structId     = (group == QStringLiteral("Common")) ? 0 : 100 + types.size();
        e.displayName  = name;
        e.classKeyword = QStringLiteral("struct");
        e.kindGroup    = group;
        e.sizeBytes    = 32;
        types.append(e);
    };
    composite(QStringLiteral("PlayerEntity"), QStringLiteral("Ctr"));  // user struct
    composite(QStringLiteral("CameraState"),  QStringLiteral("Ctr"));  // user struct
    // Stress the preview banner / action row with a very long name.
    composite(QStringLiteral("PlayerEntityControllerStateMachineComponent"),
              QStringLiteral("Ctr"));

    // Large-SDK stress: any arg containing "bigsdk" adds 1000 synthetic
    // classes, so the "Your classes" cap + "type to filter" hint and the
    // still-visible common primitives can be verified.
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]).contains(QStringLiteral("bigsdk"))) {
            for (int k = 0; k < 1000; ++k)
                composite(QStringLiteral("SdkClass_%1").arg(k, 4, 10, QChar('0')),
                          QStringLiteral("Ctr"));
            break;
        }
    }
    composite(QStringLiteral("UNICODE_STRING"), QStringLiteral("Common"));  // std-lib
    composite(QStringLiteral("std::vector"),    QStringLiteral("Common"));  // std-lib

    // Seed a couple of recent picks so the "Recent" section (non-collapsible,
    // built in the same SortGroup branch as the collapsible groups) renders.
    popup.setRecentTypes({QStringLiteral("PlayerEntity"), QStringLiteral("int32_t")});

    popup.setTypes(types, nullptr);   // default = simple view
    popup.popup(QPoint(120, 120));
    app.processEvents();
    app.processEvents();

    const QString arg2 = (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QString();

    // "filter=<text>": type a string into the filter box so the fuzzy view
    // (and, for a fresh identifier, the "＋ Create class" action row) renders.
    if (arg2.startsWith(QStringLiteral("filter="))) {
        if (auto* fe = popup.findChild<QLineEdit*>()) {
            fe->setText(arg2.mid(7));
            app.processEvents();
        }
    }

    // "expand": force-expand every collapsible section via the test hook, so
    // the fully-expanded layout can be grabbed.
    if (arg2 == QStringLiteral("expand")) {
        popup.setShowAllTypesForTest(true);
        app.processEvents();
    }

    // "perf": time per-keystroke filtering on the (bigsdk) catalogue and print
    // the average to stderr — confirms the fuzzy path stays snappy on a large
    // SDK. Pass alongside "bigsdk" (e.g. `perf bigsdk`).
    if (arg2 == QStringLiteral("perf")) {
        auto* fe = popup.findChild<QLineEdit*>();
        if (fe) {
            const char* probes[] = {"p","pl","pla","play","s","sd","sdk","sdkc",
                                    "int","ptr","floa","vec"};
            // Warm up once (first filter allocates).
            fe->setText(QStringLiteral("warm")); fe->clear();
            QElapsedTimer t; t.start();
            int n = 0;
            for (int rep = 0; rep < 20; ++rep)
                for (const char* p : probes) {
                    fe->setText(QString::fromLatin1(p));
                    ++n;
                }
            const double avgMs = (t.nsecsElapsed() / 1e6) / n;
            std::fprintf(stderr,
                "[perf] %d types, %d filter ops, avg %.3f ms/op\n",
                (int)popup.filteredTypes().size(), n, avgMs);
            fe->clear();
        }
    }

    // "selecthdr": set the current index onto the first collapsible section
    // header (no toggle) so its selection highlight can be inspected.
    if (arg2 == QStringLiteral("selecthdr")) {
        auto* lv = popup.findChild<QListView*>();
        if (lv && lv->model()) {
            const auto& ft = popup.filteredTypes();
            for (int i = 0; i < ft.size(); ++i)
                if (ft[i].entryKind == TypeEntry::Section && ft[i].sectionCollapsible) {
                    lv->setCurrentIndex(lv->model()->index(i, 0));
                    break;
                }
            app.processEvents();
        }
    }

    // "toggleclasses": drive the REAL click→acceptIndex path on the
    // "Your classes" collapsed header (Return on its row) to prove the
    // section actually expands in place, then scroll so it's visible.
    if (arg2 == QStringLiteral("toggleclasses")) {
        auto* lv = popup.findChild<QListView*>();
        if (lv && lv->model()) {
            const auto& ft = popup.filteredTypes();
            int row = -1;
            for (int i = 0; i < ft.size(); ++i)
                if (ft[i].entryKind == TypeEntry::Section
                    && ft[i].displayName.startsWith(QStringLiteral("Your classes"))) {
                    row = i; break;
                }
            if (row >= 0) {
                lv->setCurrentIndex(lv->model()->index(row, 0));
                QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(lv, &press);
                app.processEvents();
                // Re-find the (now-expanded) header and scroll it into view.
                const auto& ft2 = popup.filteredTypes();
                for (int i = 0; i < ft2.size(); ++i)
                    if (ft2[i].entryKind == TypeEntry::Section
                        && ft2[i].displayName.startsWith(QStringLiteral("Your classes"))) {
                        lv->scrollTo(lv->model()->index(i, 0),
                                     QAbstractItemView::PositionAtTop);
                        break;
                    }
                app.processEvents();
            }
        }
    }

    // "bottom"/"expand": scroll the list to the bottom so the user structs +
    // the toggle row (or, when expanded, the trailing groups) are visible.
    if (arg2 == QStringLiteral("bottom") || arg2 == QStringLiteral("expand")) {
        if (auto* lv = popup.findChild<QListView*>()) {
            lv->scrollToBottom();
            app.processEvents();
        }
    }

    // "ptr"/"arr": force the full (modifier-visible) view, select a concrete
    // type, then set the Pointer (reveals the ×2 toggle) or Array (reveals the
    // count edit) modifier — so the modifier row's expanded states can be
    // eyeballed for clipping / overlap, not just the resting 3-segment row.
    if (arg2 == QStringLiteral("ptr") || arg2 == QStringLiteral("arr")) {
        popup.setShowAllTypesForTest(true);   // full chrome incl. modifier row
        if (auto* lv = popup.findChild<QListView*>()) {
            // Select the first real (non-section) row so the preview is live.
            const auto& ft = popup.filteredTypes();
            for (int i = 0; i < ft.size(); ++i) {
                if (ft[i].entryKind != TypeEntry::Section && ft[i].enabled
                    && !ft[i].isExpandToggle && !ft[i].isCreateNew) {
                    lv->setCurrentIndex(lv->model()->index(i, 0));
                    break;
                }
            }
        }
        popup.setModifier(arg2 == QStringLiteral("ptr") ? 2 /*= ** , shows ×2*/
                                                        : 3, 16);
        app.processEvents();
    }

    // "select": the current index onto the long-named struct so the action
    // row's banner has to elide. "detail": the same, plus the detail pane
    // (its toolbar toggle is commented out; the test hook shows it).
    if (arg2 == QStringLiteral("select") || arg2 == QStringLiteral("detail")) {
        if (auto* lv = popup.findChild<QListView*>()) {
            const auto& ft = popup.filteredTypes();
            for (int i = 0; i < ft.size(); ++i)
                if (ft[i].displayName.startsWith(QStringLiteral("PlayerEntityController"))) {
                    lv->setCurrentIndex(lv->model()->index(i, 0));
                    break;
                }
        }
        if (arg2 == QStringLiteral("detail")) popup.setShowDetailForTest(true);
        app.processEvents();
        app.processEvents();
    }

    const QString out = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                   : QStringLiteral("typeselector_render.png");

    // "hex" / "hexpin": the hex toolbar instead of the chooser — unpinned
    // with the keyboard ring on the first size button, or pinned (through
    // its own pin hit rect) with a pointer and a float suggestion. This
    // target links resources.qrc, so the pin glyph is the real one (the
    // test target's is blank). Stdout: the hit rects, so a scan can crop
    // every outline and the ring.
    if (arg2 == QStringLiteral("hex") || arg2 == QStringLiteral("hexpin")) {
        popup.hide();
        HexToolbarPopup hex;
        hex.setFont(QFont(QStringLiteral("Consolas"), 10));
        HexPopupContext ctx;
        ctx.currentKind = NodeKind::Hex64;
        ctx.data = QByteArray::fromHex("4142434445464748");
        ctx.hasPtr = true;
        ctx.ptrSymbol = QStringLiteral("g_World");
        ctx.hasFloat = true;
        ctx.floatVal = 1.5f;
        hex.setContext(ctx);
        hex.popup(QPoint(120, 120));
        app.processEvents();
        hex.grab();   // paints once: builds the hit rects
        if (arg2 == QStringLiteral("hexpin")) {
            const QVector<QRect> hits = hex.hitRectsForTest();
            if (hits.size() > 5) {
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(hits[5].center()),
                                  hex.mapToGlobal(hits[5].center()),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(&hex, &press);
            }
        } else {
            QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
            QApplication::sendEvent(&hex, &right);
        }
        app.processEvents();
        const QPixmap g = hex.grab();
        g.save(out);
        std::printf("theme: %s\n", qPrintable(ThemeManager::instance().current().name));
        std::printf("%s  popup=%dx%d  dpr=%g  device=%dx%d  pinned=%d focused=%d\n", qPrintable(out),
                    hex.width(), hex.height(), g.devicePixelRatio(), g.width(), g.height(),
                    hex.isPinned() ? 1 : 0, hex.focusedHitForTest());
        const QVector<QRect> hits = hex.hitRectsForTest();
        for (int i = 0; i < hits.size(); ++i)
            std::printf("  hit %d x=%d y=%d w=%d h=%d\n", i, hits[i].x(), hits[i].y(), hits[i].width(), hits[i].height());
        return 0;
    }

    const QPixmap grab = popup.grab();
    grab.save(out);

    auto rectStr = [](const QRect& r) {
        return QStringLiteral("x=%1 y=%2 w=%3 h=%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
    };
    auto inPopup = [&popup](const QWidget* w) { return QRect(w->mapTo(&popup, QPoint(0, 0)), w->size()); };
    std::printf("theme: %s\n", qPrintable(ThemeManager::instance().current().name));
    std::printf("%s  popup=%dx%d  dpr=%g  device=%dx%d\n", qPrintable(out), popup.width(), popup.height(),
                grab.devicePixelRatio(), grab.width(), grab.height());
    if (auto* fe = popup.findChild<QLineEdit*>())
        std::printf("  filter %s\n", qPrintable(rectStr(inPopup(fe))));
    for (QLabel* l : popup.findChildren<QLabel*>()) {
        if (l->accessibleName() != QStringLiteral("Type preview")) continue;
        QTextDocument doc;
        doc.setHtml(l->text());
        std::printf("  banner \"%s\" %s\n", qPrintable(doc.toPlainText()), qPrintable(rectStr(inPopup(l))));
    }
    if (auto* lv = popup.findChild<QListView*>()) {
        std::printf("  list %s\n", qPrintable(rectStr(inPopup(lv))));
        std::printf("  viewport %s\n", qPrintable(rectStr(inPopup(lv->viewport()))));
        QScrollBar* sb = lv->verticalScrollBar();
        std::printf("  vscroll visible=%d range=%d..%d %s\n", sb->isVisible() ? 1 : 0,
                    sb->minimum(), sb->maximum(), qPrintable(rectStr(inPopup(sb))));
    }
    return 0;
}
