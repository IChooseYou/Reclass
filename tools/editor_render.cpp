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
//   editor_render <out.png> tones   — visual proof of the document tone
//   ladder: a fresh 0x80 class of Hex64 rows over a realistic mix of zero
//   and non-zero bytes, at the real 1080-logical window width, with one row
//   selected and the pointer parked on another. Writes a second PNG
//   (<out>_pill.png) with the pointer on a footer pill, and prints the
//   resolved tone tokens so the image can be checked against numbers.
#include <QApplication>
#include <QSplitter>
#include <QFont>
#include <QMouseEvent>
#include <QFileInfo>
#include <QPainter>
#include <QTimer>
#include <QMenu>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>
#include <cstdio>
#include "controller.h"
#include "editor.h"
#include "widgets/address_bar.h"
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

// A struct with a code window in the middle of it: hex64 before, asm[24],
// hex64 after. Proves the whole point of the kind in one picture — real
// instructions inline, and the field after them still at its declared offset.
static NodeTree buildAsmTree() {
    NodeTree tree;
    tree.baseAddress = 0;
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "Hooked";
    root.name = "hooked";
    root.parentId = 0;
    root.collapsed = false;
    uint64_t rootId = tree.nodes[tree.addNode(root)].id;

    Node before; before.kind = NodeKind::Hex64; before.name = "vtable";
    before.parentId = rootId; before.offset = 0;
    tree.addNode(before);

    Node code; code.kind = NodeKind::Asm; code.name = "trampoline";
    code.parentId = rootId; code.offset = 8; code.arrayLen = 24;
    code.collapsed = false;
    tree.addNode(code);

    Node after; after.kind = NodeKind::Hex64; after.name = "flags";
    after.parentId = rootId; after.offset = 8 + 24;
    tree.addNode(after);
    return tree;
}

// Real x86-64 at offset 8: a hook prologue that jumps away, then a small
// function epilogue, then an int3 pad.
static QByteArray buildAsmBuffer() {
    QByteArray data(64, '\0');
    static const unsigned char code[] = {
        0x55,                                // push rbp
        0x48, 0x89, 0xE5,                    // mov rbp, rsp
        0x48, 0x8B, 0x01,                    // mov rax, [rcx]
        0xE8, 0x10, 0x00, 0x00, 0x00,        // call +0x10
        0x48, 0x83, 0xC4, 0x20,              // add rsp, 0x20
        0x5D,                                // pop rbp
        0xC3,                                // ret
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,  // int3 padding
    };
    memcpy(data.data() + 8, code, sizeof(code));
    return data;
}

static QByteArray buildBuffer() {
    QByteArray data(64, '\0');
    for (int i = 0; i < data.size(); ++i)
        data[i] = (char)(i + 0x10);  // byte at offset N == N + 0x10
    return data;
}

// Tutorial-like tree for the address-bar "drill" mode: a root class RcxEditor
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

// 16 x Hex64 = 0x80 — the shape a "New Class" lands in, and the one the tone
// ladder has to make readable: every row is type column + ASCII preview +
// eight bytes, with nothing but the byte values to carry the data.
static NodeTree buildTonesTree() {
    NodeTree tree;
    tree.baseAddress = 0;  // BufferProvider treats an address as a buffer offset
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "UnnamedClass0";
    root.classKeyword = "class";
    root.name = "obj";
    root.parentId = 0;
    root.collapsed = false;
    uint64_t rootId = tree.nodes[tree.addNode(root)].id;
    for (int i = 0; i < 16; ++i) {
        Node n;
        n.kind = NodeKind::Hex64;
        n.parentId = rootId;
        n.offset = i * 8;
        tree.addNode(n);
    }
    return tree;
}

// A realistic mix: pointers and small ints leave long runs of 00 next to a
// few live bytes, which is exactly the contrast the ladder has to render.
static QByteArray buildTonesBuffer() {
    QByteArray data(0x80, '\0');
    auto w64 = [&](int off, quint64 v) { memcpy(data.data() + off, &v, 8); };
    w64(0x00, 0x00007FF6DE1234A0ULL);  // vtable-ish pointer
    w64(0x08, 0x0000000000000001ULL);  // small int -> seven 00s
    w64(0x10, 0x000001A4C3E20F90ULL);
    w64(0x18, 0x0000000000000000ULL);  // all zero
    w64(0x20, 0x3F80000042C80000ULL);  // two floats
    w64(0x28, 0x00007FFE3B8B53C0ULL);
    w64(0x30, 0x0000000000000064ULL);
    w64(0x38, 0xFFFFFFFFFFFFFFFFULL);  // all live
    w64(0x40, 0x000001A4C3D40000ULL);
    w64(0x50, 0x0000000000000010ULL);
    w64(0x60, 0x00007FFE3B720000ULL);
    w64(0x70, 0x0000000000000002ULL);
    // Printable ASCII, deliberately including 0x22 (") and 0x27 ('), plus a
    // "//" pair. The ASCII preview column is REAL memory, so these put the
    // C++ lexer into string / comment state on their row; without the
    // explicit byte tone the whole byte grid on those rows renders at
    // whatever the lexer chose (an unset UnclosedString = pure black).
    // A tones buffer of pointers and zeros can never show that.
    for (int i = 0; i < 8; ++i) data[0x48 + i] = (char)(0x20 + i);   // ` !"#$%&'`
    for (int i = 0; i < 8; ++i) data[0x58 + i] = (char)(0x28 + i);   // `()*+,-./`
    for (int i = 0; i < 8; ++i) data[0x68 + i] = (char)(0x30 + i);   // digits
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

    if (mode == QStringLiteral("menu")) {
        // The node context menu, grabbed as its own top-level popup.
        // showContextMenu() exec()s, so a zero-delay timer photographs
        // the live popup and closes it. Menu icons are the point: a raw
        // :/vsicons SVG keeps VS Code's #C5C5C5 and washes out on a
        // light theme, so this sheet has to be read in BOTH polarities.
        ctrl->refresh();
        app.processEvents();
        int line = -1, nodeIdx = -1;
        const auto& meta = ctrl->lastResult().meta;
        for (int i = 0; i < meta.size(); i++)
            if (meta[i].nodeIdx > 0 && meta[i].lineKind == LineKind::Field) {
                line = i; nodeIdx = meta[i].nodeIdx; break;
            }
        QTimer::singleShot(0, [&]() {
            if (auto* pop = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                pop->grab().save(out);
                fprintf(stdout, "menu %d items -> %s\n",
                        (int)pop->actions().size(), qPrintable(out));
                pop->close();
            } else {
                fprintf(stdout, "no popup captured\n");
            }
        });
        ctrl->showContextMenu(editor, line, nodeIdx, 0, QPoint(200, 200));
        return 0;
    }

    if (mode == QStringLiteral("asm")) {
        doc->tree = buildAsmTree();
        doc->provider = std::make_shared<BufferProvider>(buildAsmBuffer(), "asm.bin");
        splitter->resize(1080, 420);
        ctrl->refresh();
        app.processEvents();
        app.processEvents();
        editor->grab().save(out);
        fprintf(stdout, "asm sheet -> %s\n", qPrintable(out));
        return 0;
    }

    if (mode == QStringLiteral("tones")) {
        // Real window width: the user's app is ~1080 logical px, and the tone
        // ladder is only honest at the width the columns were laid out for.
        doc->tree = buildTonesTree();
        doc->provider = std::make_shared<BufferProvider>(buildTonesBuffer(),
                                                         "tones.bin");
        splitter->resize(1080, 560);
        ctrl->refresh();
        app.processEvents();

        const Theme& th = ThemeManager::instance().current();
        std::printf("tones: paper=%s text=%s textDim=%s textMuted=%s "
                    "textFaint=%s hover=%s selected=%s border=%s\n",
                    qPrintable(editorPaperColor(th).name()),
                    qPrintable(th.text.name()), qPrintable(th.textDim.name()),
                    qPrintable(th.textMuted.name()), qPrintable(th.textFaint.name()),
                    qPrintable(th.hover.name()), qPrintable(th.selected.name()),
                    qPrintable(th.border.name()));

        // Pin one row selected (the persistent state) and park the pointer on
        // another (the transient one) so the two bands appear side by side.
        int firstField = -1, footerLine = -1;
        for (int i = 0; ; ++i) {
            const LineMeta* lm = editor->metaForLine(i);
            if (!lm) break;
            if (firstField < 0 && lm->lineKind == LineKind::Field) firstField = i;
            if (lm->lineKind == LineKind::Footer) footerLine = i;
        }
        if (firstField >= 0) {
            const LineMeta* lm = editor->metaForLine(firstField + 2);
            if (lm) ctrl->handleNodeClick(editor, firstField + 2, lm->nodeId,
                                          Qt::NoModifier);
        }
        app.processEvents();

        auto moveTo = [&](int line, int col) {
            long pos = editor->scintilla()->SendScintilla(
                QsciScintillaBase::SCI_FINDCOLUMN, (unsigned long)line, (long)col);
            int x = (int)editor->scintilla()->SendScintilla(
                QsciScintillaBase::SCI_POINTXFROMPOSITION, 0, pos);
            int y = (int)editor->scintilla()->SendScintilla(
                QsciScintillaBase::SCI_POINTYFROMPOSITION, 0, pos);
            QPoint p(x + 2, y + 4);
            QMouseEvent move(QEvent::MouseMove, QPointF(p), QPointF(p),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(editor->scintilla()->viewport(), &move);
            app.processEvents();
        };

        if (firstField >= 0) moveTo(firstField + 5, 6);
        editor->grab().save(out);
        std::printf("tones: wrote %s (selected line %d, hover line %d)\n",
                    qPrintable(out), firstField + 2, firstField + 5);

        // Second frame: pointer on the "+10h" footer pill, for the
        // outline-at-rest / fill-on-hover check.
        if (footerLine >= 0) {
            const QString ft = editor->scintilla()->text(footerLine);
            const int at = ft.indexOf(QStringLiteral("+10h"));
            if (at > 0) {
                moveTo(footerLine, at + 1);
                QFileInfo fi(out);
                const QString pillOut = fi.path() + QStringLiteral("/")
                                      + fi.completeBaseName()
                                      + QStringLiteral("_pill.png");
                editor->grab().save(pillOut);
                std::printf("tones: wrote %s (footer line %d, +10h at col %d)\n",
                            qPrintable(pillOut), footerLine, at);
            }
        }
        std::fflush(stdout);
        return 0;
    }

    if (mode == QStringLiteral("drill")) {
        // Address-bar proof: swap in the tutorial-like tree, view RcxEditor,
        // expand __vptr inline, then CLICK it (the click-driven trail adds
        // it). The grab shows the always-visible bar — source chip, base,
        // and the trail grown to the dotted production shape
        // "RcxEditor.__vptr › QWidgetVTable" (ancestor = class.field,
        // deepest = bare class) — the vtable expanded inline, and the fnptr
        // rows (single address). Prints the crumbs with their addresses,
        // the focus path and the view root for asserting.
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
        QStringList cr;
        for (const Crumb& c : editor->addressBar()->state().crumbs)
            cr << QStringLiteral("%1@0x%2").arg(c.label, QString::number(c.address, 16).toUpper());
        std::printf("drill: crumbs=[%s] focusPath=[%s] viewRoot=%llu\n",
                    qPrintable(cr.join(',')), qPrintable(fp.join(',')),
                    (unsigned long long)ctrl->viewRootId());
        std::fflush(stdout);
        editor->grab().save(out);
        // Also grab just the address bar, scaled 4×, to inspect the crumb
        // text up close (vertical centering / first-ink at kGutter).
        if (auto* bar = editor->addressBar()) {
            QPixmap bp = bar->grab();
            bp.scaled(bp.width() * 4, bp.height() * 4, Qt::IgnoreAspectRatio,
                      Qt::SmoothTransformation).save(QStringLiteral("bc_bar_4x.png"));
        }
        // And the source chip + base segment in every liveness the controller
        // can report, over a formula base, one bar per row at 760 px, 4×
        // (bc_bar_states_4x.png): live, stale, disconnected, static, no
        // source, then live over a lone crumb (room for the base's resolved-
        // address suffix). The harness provider is a static buffer, so the
        // real bar above never shows a dot — this strip is the only place to
        // eyeball it without a live process.
        {
            AddressBarState s = editor->addressBar()->state();
            s.sourceName   = QStringLiteral("REECLASS.exe");
            s.sourceKindId = QStringLiteral("processmemory");
            s.baseFormula  = QStringLiteral("<REECLASS.exe>+0x1234+[0x10]*2");
            s.resolvedBase = 0x7FF6DEAD1234ULL;
            const int levels[] = { liveness::Live, liveness::Stale, liveness::Disconnected,
                                   liveness::Static, liveness::None };
            const int w = 760;
            QVector<QPixmap> rows;
            for (int lv : levels) {
                AddressBar bar;
                bar.applyTheme(ThemeManager::instance().current());
                AddressBarState st = s;
                st.liveness = lv;
                if (lv == liveness::None) { st.sourceName.clear(); st.sourceKindId.clear(); }
                bar.setState(st);
                bar.resize(w, AddressBar::kAddressBarHeight);
                rows << bar.grab();
            }
            {
                AddressBar bar;
                bar.applyTheme(ThemeManager::instance().current());
                AddressBarState st = s;
                st.liveness = liveness::Live;
                Crumb lone = st.crumbs.first();
                lone.label = QStringLiteral("RcxEditor");
                st.crumbs = { lone };
                bar.setState(st);
                bar.resize(w, AddressBar::kAddressBarHeight);
                rows << bar.grab();
            }
            const int rowH = rows.first().height();
            QPixmap strip(rows.first().width(), rowH * rows.size());
            strip.setDevicePixelRatio(rows.first().devicePixelRatio());
            strip.fill(Qt::black);
            {
                QPainter sp(&strip);
                for (int i = 0; i < rows.size(); ++i)
                    sp.drawPixmap(0, i * AddressBar::kAddressBarHeight, rows[i]);
            }
            strip.scaled(strip.width() * 4, strip.height() * 4, Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation).save(QStringLiteral("bc_bar_states_4x.png"));
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
