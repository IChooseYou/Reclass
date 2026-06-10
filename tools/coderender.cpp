// Offscreen render harness for the "Code" tab's syntax highlighting. Builds a
// small struct, renders it in the chosen CodeFormat, attaches the themed
// per-language lexer (applyCodeLexer), and grabs the result to a PNG so the
// coloring (types/keywords/numbers/preproc/strings/comments) can be eyeballed
// per language. Runs on the default windows platform.
//
// Usage: coderender <out.png> [fmt]
//   fmt: 0=C++ 1=Rust 2=#define 3=C# 4=Python  (CodeFormat enum order)
#include <QApplication>
#include <QFont>
#include <Qsci/qsciscintilla.h>
#include "code_highlight.h"
#include "generator.h"
#include "core.h"
#include "themes/thememanager.h"

using namespace rcx;

static NodeTree buildTree() {
    NodeTree tree;
    tree.baseAddress = 0;
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "Player";
    root.name = "player";
    root.collapsed = false;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;
    auto add = [&](NodeKind k, const char* nm, int off) {
        Node n; n.kind = k; n.name = nm; n.parentId = rootId; n.offset = off;
        tree.addNode(n);
    };
    add(NodeKind::Hex64,     "_pad0",  0);
    add(NodeKind::Int32,     "health", 8);
    add(NodeKind::Float,     "posX",   12);
    add(NodeKind::UInt64,    "flags",  16);
    add(NodeKind::Pointer64, "next",   24);
    add(NodeKind::Bool,      "alive",  32);
    return tree;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const QString out = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                   : QStringLiteral("coderender.png");
    int fmtI = (argc > 2) ? QString::fromLocal8Bit(argv[2]).toInt() : 0;
    CodeFormat fmt = static_cast<CodeFormat>(fmtI);

    NodeTree tree = buildTree();
    uint64_t rootId = tree.nodes[0].id;
    QString code = renderCodeTree(fmt, tree, rootId, nullptr, false);

    const Theme& th = ThemeManager::instance().current();
    QFont f(QStringLiteral("Consolas"), 11);
    f.setFixedPitch(true);

    QsciScintilla sci;
    applyCodeLexer(&sci, fmt, th, f);
    sci.setPaper(th.background.darker(115));
    sci.setColor(th.text);
    sci.setMarginsBackgroundColor(th.background.darker(115));
    sci.setMarginsForegroundColor(th.textDim);
    sci.setText(code);
    sci.resize(720, 380);
    sci.show();
    app.processEvents();
    app.processEvents();

    sci.grab().save(out);
    return 0;
}
