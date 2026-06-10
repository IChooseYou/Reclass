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
