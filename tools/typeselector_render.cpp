// Offscreen render harness for the TypeSelectorPopup. Builds a representative
// full type catalogue (every primitive + a couple user structs + a couple
// std-lib "Common Types"), shows the popup in its default SIMPLE view, and
// grabs it to a PNG — so the "common-by-default + Show all toggle" layout can
// be eyeballed deterministically. Runs on the default windows platform (the
// offscreen plugin isn't installed; the popup is a transient real window).
//
// Usage: typeselector_render <out.png>
#include <QApplication>
#include <QFont>
#include <QListView>
#include <QLineEdit>
#include <QKeyEvent>
#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QSettings>
#include <cstdio>
#include "typeselectorpopup.h"
#include "core.h"
#include "themes/thememanager.h"

using namespace rcx;

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // ThemeManager::setCurrent() PERSISTS the chosen theme to the shared
    // QSettings("Reclass","Reclass") that the real app reads at startup — so a
    // harness run that switches theme would flip the user's app theme. Capture
    // the original and restore it on exit so the harness never mutates it.
    const QString kOrigTheme =
        QSettings(QStringLiteral("Reclass"), QStringLiteral("Reclass"))
            .value(QStringLiteral("theme")).toString();
    struct ThemeRestorer {
        QString name;
        ~ThemeRestorer() {
            if (!name.isEmpty())
                QSettings(QStringLiteral("Reclass"), QStringLiteral("Reclass"))
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

    const QString out = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                   : QStringLiteral("typeselector_render.png");
    popup.grab().save(out);
    return 0;
}
