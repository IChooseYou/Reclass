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
#include "typeselectorpopup.h"
#include "core.h"
#include "themes/thememanager.h"

using namespace rcx;

int main(int argc, char** argv) {
    QApplication app(argc, argv);

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
    composite(QStringLiteral("UNICODE_STRING"), QStringLiteral("Common"));  // std-lib
    composite(QStringLiteral("std::vector"),    QStringLiteral("Common"));  // std-lib

    popup.setTypes(types, nullptr);   // default = simple view
    popup.popup(QPoint(120, 120));
    app.processEvents();
    app.processEvents();

    const QString arg2 = (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QString();

    // "expand": activate the "Show all types" toggle row end-to-end (the
    // same Return→acceptCurrent→acceptIndex path the UI uses) and grab the
    // resulting full view — confirms the toggle actually flips m_showAllTypes
    // and re-filters, not just that the simple view renders.
    if (arg2 == QStringLiteral("expand")) {
        auto* lv = popup.findChild<QListView*>();
        if (lv && lv->model()) {
            int toggleRow = -1;
            const auto& ft = popup.filteredTypes();
            for (int i = 0; i < ft.size(); ++i)
                if (ft[i].isExpandToggle) { toggleRow = i; break; }
            if (toggleRow >= 0) {
                lv->setCurrentIndex(lv->model()->index(toggleRow, 0));
                QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(lv, &press);
                if (auto* fe = popup.findChild<QLineEdit*>())
                    QApplication::sendEvent(fe, &press);  // event filter lives on both
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

    const QString out = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                   : QStringLiteral("typeselector_render.png");
    popup.grab().save(out);
    return 0;
}
