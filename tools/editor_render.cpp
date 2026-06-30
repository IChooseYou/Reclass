// Offscreen render harness for the RcxEditor. Builds a controller + editor
// over a synthetic BufferProvider, applies the current theme, pins a
// (possibly multi-row) hex byte selection, and grabs the editor to a PNG —
// works under `-platform offscreen` with no display.
//
// Doubles as a programmatic check of the byte→row sync: it prints the byte
// range and the controller's resulting selectedIds() count, so the
// "byte selection selects every covered row" behaviour can be asserted
// without eyeballing the image (covered rows == grey M_SELECTED rows).
//
// Usage: editor_render <out.png> [loByte] [hiByte]
//   loByte/hiByte are buffer offsets; default [4, 16) spans rows 1..3.
#include <QApplication>
#include <QSplitter>
#include <QFont>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>
#include <cstdio>
#include "controller.h"
#include "editor.h"
#include "widgets/breadcrumb_bar.h"
#include "core.h"
#include "providers/buffer_provider.h"
#include "themes/thememanager.h"

using namespace rcx;

// A struct with 8× Hex32 fields covering 32 bytes — enough rows that a
// multi-row byte selection visibly highlights several grey rows.
static NodeTree buildTree() {
    NodeTree tree;
    tree.baseAddress = 0;  // BufferProvider treats addr as buffer offset
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "T";
    root.name = "t";
    root.parentId = 0;
    root.collapsed = false;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;
    for (int i = 0; i < 8; ++i) {
        Node n;
        n.kind = NodeKind::Hex32;
        n.name = QStringLiteral("h%1").arg(i);
        n.parentId = rootId;
        n.offset = i * 4;
        tree.addNode(n);
    }
    return tree;
}

static QByteArray buildBuffer() {
    QByteArray data(64, '\0');
    for (int i = 0; i < data.size(); ++i)
        data[i] = (char)(i + 0x10);  // byte at offset N == N + 0x10
    return data;
}

// Tutorial-like tree for the breadcrumb "drill" mode: a root class RcxEditor
// with a drillable __vptr (Pointer64, refId → a QWidgetVTable struct of named
// FuncPtr64 slots) plus a non-drillable d_ptr and some hex fields.
static NodeTree buildDrillTree() {
    NodeTree tree;
    tree.baseAddress = 0;
    Node root; root.kind = NodeKind::Struct;
    root.structTypeName = "RcxEditor"; root.classKeyword = "class";
    root.name = "editor"; root.parentId = 0; root.collapsed = false;
    uint64_t rootId = tree.nodes[tree.addNode(root)].id;

    Node vt; vt.kind = NodeKind::Struct; vt.structTypeName = "QWidgetVTable";
    vt.parentId = 0;
    uint64_t vtId = tree.nodes[tree.addNode(vt)].id;
    static const char* names[] = { "deleting_dtor", "metaObject", "qt_metacast",
                                   "event", "eventFilter", "sizeHint" };
    for (int i = 0; i < 6; ++i) {
        Node fn; fn.kind = NodeKind::FuncPtr64; fn.name = QString::fromLatin1(names[i]);
        fn.parentId = vtId; fn.offset = i * 8;
        tree.addNode(fn);
    }
    Node vptr; vptr.kind = NodeKind::Pointer64; vptr.name = "__vptr";
    vptr.parentId = rootId; vptr.offset = 0; vptr.refId = vtId; vptr.collapsed = true;
    tree.addNode(vptr);
    Node dptr; dptr.kind = NodeKind::Pointer64; dptr.name = "d_ptr";
    dptr.parentId = rootId; dptr.offset = 8;
    tree.addNode(dptr);
    for (int i = 0; i < 4; ++i) {
        Node h; h.kind = NodeKind::Hex64; h.name = QStringLiteral("field_%1").arg(i);
        h.parentId = rootId; h.offset = 16 + i * 8;
        tree.addNode(h);
    }
    return tree;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const QString out = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                   : QStringLiteral("editor_render.png");
    const uint64_t lo = (argc > 2) ? QString::fromLocal8Bit(argv[2]).toULongLong() : 4;
    const uint64_t hi = (argc > 3) ? QString::fromLocal8Bit(argv[3]).toULongLong() : 16;

    // "zoom" mode: editor_render <out.png> zoom <level> — apply the Scintilla
    // zoom level (point delta, same as Ctrl+wheel) after the editor is built.
    auto* doc = new RcxDocument();
    doc->tree = buildTree();
    doc->provider = std::make_shared<BufferProvider>(buildBuffer(), "editor_render");

    auto* splitter = new QSplitter();
    auto* ctrl = new RcxController(doc, nullptr);
    auto* editor = ctrl->addSplitEditor(splitter);
    editor->applyTheme(ThemeManager::instance().current());
    ctrl->setEditorFont(QStringLiteral("Consolas"));
    splitter->resize(900, 380);
    splitter->show();
    app.processEvents();
    ctrl->refresh();
    app.processEvents();

    const QString mode = (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QString();

    if (mode == QStringLiteral("zoom")) {
        // Apply the Scintilla zoom level (same path Ctrl+wheel uses) and grab.
        editor->scintilla()->zoomTo(QString::fromLocal8Bit(argv[3]).toInt());
        app.processEvents();
        editor->grab().save(out);
        return 0;
    }

    if (mode == QStringLiteral("drill")) {
        // Breadcrumb proof: swap in the tutorial-like tree, view RcxEditor,
        // expand __vptr inline, then CLICK it (the click-driven breadcrumb adds
        // it). The grab shows the always-visible breadcrumb grown to
        // "RcxEditor › __vptr › QWidgetVTable", the vtable expanded inline, and
        // the fnptr rows (single address). Prints the focus path for asserting.
        doc->tree = buildDrillTree();
        uint64_t rootId = 0, vptrId = 0;
        for (const auto& n : doc->tree.nodes) {
            if (n.structTypeName == QStringLiteral("RcxEditor")) rootId = n.id;
            if (n.name == QStringLiteral("__vptr")) vptrId = n.id;
        }
        ctrl->setViewRootId(rootId);
        doc->tree.nodes[doc->tree.indexOfId(vptrId)].collapsed = false;  // fold-expand
        ctrl->refresh();
        app.processEvents();
        int vptrLine = -1;
        for (int i = 0; ; ++i) {
            const LineMeta* lm = editor->metaForLine(i);
            if (!lm) break;
            if (lm->nodeId == vptrId && lm->lineKind != LineKind::Footer) { vptrLine = i; break; }
        }
        ctrl->handleNodeClick(editor, vptrLine, vptrId, Qt::NoModifier);
        app.processEvents();
        QStringList fp;
        for (uint64_t id : ctrl->focusPath()) fp << QString::number(id);
        std::printf("drill: focusPath=[%s] viewRoot=%llu\n",
                    qPrintable(fp.join(',')), (unsigned long long)ctrl->viewRootId());
        std::fflush(stdout);
        editor->grab().save(out);
        // Also grab just the breadcrumb bar, scaled 4×, to inspect crumb text
        // rendering up close (vertical centering / first-crumb cleanliness).
        if (auto* bar = editor->breadcrumbBar()) {
            QPixmap bp = bar->grab();
            bp.scaled(bp.width() * 4, bp.height() * 4, Qt::IgnoreAspectRatio,
                      Qt::SmoothTransformation).save(QStringLiteral("bc_bar_4x.png"));
        }
        return 0;
    }

    auto lineForId = [&](uint64_t id) -> int {
        for (int i = 0; ; ++i) {
            const LineMeta* lm = editor->metaForLine(i);
            if (!lm) return -1;
            if (lm->nodeId == id && lm->lineKind == LineKind::Field) return i;
        }
    };
    auto highlightedRow = [&]() -> int {
        for (int ln = 0; ln < editor->scintilla()->lines(); ++ln) {
            int m = (int)editor->scintilla()->SendScintilla(
                QsciScintillaBase::SCI_MARKERGET, (unsigned long)ln);
            if (m & (1 << M_SELECTED)) return ln;
        }
        return -1;
    };

    if (mode == QStringLiteral("delete")) {
        // Reproduce the reported bug: select two node rows (h2, h3), park the
        // caret on one (as a click would), delete them, and see whether any
        // row stays highlighted / the focus lands on the shifted-up node.
        uint64_t id2 = doc->tree.nodes[3].id;  // h2 (idx0=root, idx1=h0…)
        uint64_t id3 = doc->tree.nodes[4].id;  // h3
        ctrl->handleNodeClick(editor, lineForId(id2), id2, Qt::NoModifier);
        ctrl->handleNodeClick(editor, lineForId(id3), id3, Qt::ControlModifier);
        editor->scintilla()->setCursorPosition(lineForId(id3), 4);
        app.processEvents();
        std::printf("before delete: selectedIds=%d highlightedRow=%d\n",
                    ctrl->selectedIds().size(), highlightedRow());
        QMetaObject::invokeMethod(editor, "deleteSelectedRequested");
        app.processEvents();
        int caretLine, caretCol;
        editor->scintilla()->getCursorPosition(&caretLine, &caretCol);
        const LineMeta* caretLm = editor->metaForLine(caretLine);
        std::printf("after delete: selectedIds=%d highlightedRow=%d caretLine=%d caretNode=%s\n",
                    ctrl->selectedIds().size(), highlightedRow(), caretLine,
                    caretLm ? qPrintable(caretLm->offsetText) : "(none)");
        std::fflush(stdout);
        editor->grab().save(out);
        return 0;
    }

    const bool ok = editor->setByteSelection(lo, hi);
    app.processEvents();

    const QSet<uint64_t> sel = ctrl->selectedIds();
    std::printf("byteSelection [%llu, %llu) accepted=%d -> selectedIds=%d\n",
                (unsigned long long)lo, (unsigned long long)hi,
                ok ? 1 : 0, sel.size());
    std::fflush(stdout);

    editor->grab().save(out);
    return 0;
}
