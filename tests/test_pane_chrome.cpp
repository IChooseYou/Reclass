// Chrome contracts that used to live as hand-written duplicates inside
// main.cpp: the pane-tab stylesheet (two disagreeing copies, creation vs
// applyTheme) and the status bar's location segment (a lambda bound to
// whichever tab last emitted nodeSelected). Both now live in pure headers —
// which is also the only way they can be tested at all, since no test target
// compiles main.cpp and MainWindow is therefore not constructible here.

#include <QtTest/QTest>
#include <QApplication>

#include "core.h"
#include "themes/theme.h"
#include "widgets/pane_tabs.h"
#include "widgets/selection_status.h"

using namespace rcx;

static Theme makeTheme() {
    Theme t;
    t.background   = QColor("#1e1e1e");
    t.backgroundAlt = QColor("#252526");
    t.text         = QColor("#d4d4d4");
    t.textDim      = QColor("#9d9d9d");
    t.textMuted    = QColor("#6e6e6e");
    t.border       = QColor("#3c3c3c");
    t.hover        = QColor("#242427");
    t.indHoverSpan = QColor("#a97cf0");
    return t;
}

// Root struct + one hex64 field at +0x10, spanning 0x80 bytes.
struct Built { uint64_t rootId; int fieldIdx; };
static Built buildClass(NodeTree& tree, const char* typeName) {
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = QString::fromLatin1(typeName);
    root.parentId = 0;
    root.offset = 0;
    root.collapsed = false;
    const uint64_t rootId = tree.nodes[tree.addNode(root)].id;

    Built b{ rootId, -1 };
    for (int off = 0; off < 0x80; off += 8) {
        Node f;
        f.kind = NodeKind::Hex64;
        f.name = QStringLiteral("field_%1").arg(off, 0, 16);
        f.parentId = rootId;
        f.offset = off;
        const int i = tree.addNode(f);
        if (off == 0x10) b.fieldIdx = i;
    }
    return b;
}

class TestPaneChrome : public QObject {
    Q_OBJECT
private slots:

    // ── Pane tabs ────────────────────────────────────────────────────────
    void testTabZeroIsStructureNotTheAppName() {
        // The brand belongs on the title bar; a VIEW is named after what it
        // shows. Tab 0 used to read "RC".
        QCOMPARE(QString::fromLatin1(kPaneTabs[0].name), QStringLiteral("Structure"));
        QCOMPARE(kPaneTabCount, 4);
    }

    void testEveryPaneTabHasItsOwnTooltip() {
        QStringList tips;
        for (int i = 0; i < kPaneTabCount; ++i) {
            const QString tip = QString::fromLatin1(kPaneTabs[i].tip);
            QVERIFY2(!tip.isEmpty(), kPaneTabs[i].name);
            QVERIFY2(!tips.contains(tip), kPaneTabs[i].name);
            tips << tip;
        }
    }

    void testPaneTabStyleIsAnUnderline() {
        const Theme t = makeTheme();
        const QString qss = paneTabStyle(t, QStringLiteral("JetBrains Mono"));
        QVERIFY(qss.contains(QStringLiteral("border-bottom: 2px solid ")
                             + t.indHoverSpan.name()));
        QVERIFY(qss.contains(QStringLiteral("margin-left: 8px")));   // kGutter
        QVERIFY(qss.contains(QStringLiteral("padding: 0px 8px")));
        QVERIFY(qss.contains(editorPaperColor(t).name()));           // paper, not chrome
    }

    void testPaneTabStyleHasNoBoxAndNoFill() {
        const Theme t = makeTheme();
        const QString qss = paneTabStyle(t, QStringLiteral("JetBrains Mono"));
        // The two literals this replaced drew side borders, a selected-tab
        // fill and a negative padding to make room for a TOP accent.
        QVERIFY(!qss.contains(QStringLiteral("border-left")));
        QVERIFY(!qss.contains(QStringLiteral("border-right")));
        QVERIFY(!qss.contains(QStringLiteral("border-top")));
        QVERIFY(!qss.contains(QStringLiteral("padding-top: -")));
        QVERIFY(!qss.contains(t.backgroundAlt.name()));
    }

    void testEveryPaneTabReservesTheUnderline() {
        const Theme t = makeTheme();
        const QString qss = paneTabStyle(t, QStringLiteral("JetBrains Mono"));
        // A border-bottom only on :selected ADDS to that tab's box, so the
        // selected tab stood 2 px taller than its siblings (and than the
        // corner strip, which is pinned to the same 26 px). Every tab has to
        // reserve the same underline row and only change its colour.
        QVERIFY(qss.contains(QStringLiteral("border-bottom: 2px solid transparent")));
    }

    void testFirstPaneTabSpendsTheGutterExactlyOnce() {
        const Theme t = makeTheme();
        const QString qss = paneTabStyle(t, QStringLiteral("JetBrains Mono"));
        // margin-left 8 (kGutter) ON TOP OF the shared padding put the first
        // label 16 px in — twice the gutter every other strip uses.
        QVERIFY(qss.contains(QStringLiteral(
            "QTabBar::tab:first { margin-left: 8px; padding-left: 0px; }")));
    }

    void testWholePaneTabStripIsOneSurface() {
        const Theme t = makeTheme();
        const QString qss = paneTabStyle(t, QStringLiteral("JetBrains Mono"));
        const QString paper = editorPaperColor(t).name();
        // Tabs on paper with the bar (and the zoom/format corner) left on
        // theme.background drew a faint box around the tab group — the exact
        // "fill at rest" cue the underline grammar removes from the tabs.
        for (const QString& sel : { QStringLiteral("QTabBar { border: none; background: %1; }"),
                                    QStringLiteral("QTabWidget { background: %1; }"),
                                    QStringLiteral("QWidget#rcxPaneCorner { background: %1; }") })
            QVERIFY2(qss.contains(sel.arg(paper)), qPrintable(sel));
    }

    // ── Status segment ──────────────────────────────────────────────────
    void testSelectionStatusNamesFieldAndOffset() {
        NodeTree tree;
        const Built b = buildClass(tree, "UnnamedClass0");
        const SelectionStatus s = buildSelectionStatus(tree, b.fieldIdx, b.rootId, 1);
        QCOMPARE(s.main, QStringLiteral("UnnamedClass0.field_10"));
        QVERIFY(s.dim.contains(QStringLiteral("+0x10")));
        QVERIFY(s.dim.contains(QStringLiteral("0x80 (128)")));
    }

    void testStatusDropsTheKeyLegendAndTheRepeatedClassName() {
        NodeTree tree;
        const Built b = buildClass(tree, "UnnamedClass0");
        const SelectionStatus s = buildSelectionStatus(tree, b.fieldIdx, b.rootId, 1);
        // The legend never changed, so it was permanent clutter (tooltip now).
        QVERIFY(!s.dim.contains(QStringLiteral("P=ptr")));
        // The class name is already the head of `main` — the size clause used
        // to repeat it a second time in the same strip.
        QVERIFY(!s.dim.contains(QStringLiteral("UnnamedClass0")));
    }

    // The linkable half of "select a node in tab A, activate tab B, the label
    // names B": the segment is a pure function of the document it is given, so
    // rebuilding it against B's tree can never keep A's text.
    void testStatusFollowsTheDocumentItIsGiven() {
        NodeTree a, bTree;
        const Built ba = buildClass(a, "UnnamedClass0");
        buildClass(bTree, "UnnamedClass4");

        const SelectionStatus inA = buildSelectionStatus(a, ba.fieldIdx, ba.rootId, 1);
        QVERIFY(inA.main.startsWith(QStringLiteral("UnnamedClass0")));

        // Tab B has no single selection → the root form, naming B.
        const SelectionStatus inB = buildRootStatus(bTree, 0);
        QCOMPARE(inB.main, QStringLiteral("UnnamedClass4"));
        QVERIFY(!inB.main.contains(QStringLiteral("UnnamedClass0")));
        QVERIFY(!inB.dim.contains(QStringLiteral("UnnamedClass0")));
        QVERIFY(inB.dim.contains(QStringLiteral("0x80 (128)")));
    }

    // An address-margin click emits nodeSelected WITHOUT touching the byte /
    // row selection, so a real "this field" gesture arrives with a selection
    // count of zero. The node form has to survive that: gating the caller on
    // selCount >= 1 silently downgraded the click to the class-root form.
    void testNodeFormSurvivesAnEmptySelection() {
        NodeTree tree;
        const Built b = buildClass(tree, "UnnamedClass0");
        const SelectionStatus s = buildSelectionStatus(tree, b.fieldIdx, b.rootId,
                                                       /*selCount=*/0);
        QCOMPARE(s.main, QStringLiteral("UnnamedClass0.field_10"));
        QVERIFY(s.dim.contains(QStringLiteral("+0x10")));
        QVERIFY(!s.main.contains(QStringLiteral("×")));   // no plural form
    }

    void testRootStatusWithoutAnyClassIsEmpty() {
        NodeTree empty;
        const SelectionStatus s = buildRootStatus(empty, 0);
        QVERIFY(s.main.isEmpty());
        QVERIFY(s.dim.isEmpty());
    }
};

QTEST_MAIN(TestPaneChrome)
#include "test_pane_chrome.moc"
