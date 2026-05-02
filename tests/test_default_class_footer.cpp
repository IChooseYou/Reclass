// Reproduces the user's bug: opening multiple "New Class" tabs causes
// the footer of every tab AFTER the first to come out malformed
// (truncated bytes line + footer concatenated, root name "instanceN"
// appearing at the end). Test creates 3 default RcxDocuments + composes
// each, then asserts the textual output ends with the expected footer.

#include <QtTest/QTest>
#include <QApplication>
#include <QStringList>
#include <Qsci/qsciscintilla.h>
#include <cstdio>

#include "core.h"
#include "editor.h"

#define LOG(...) do { std::fprintf(stdout, __VA_ARGS__); std::fflush(stdout); } while (0)

using namespace rcx;

namespace {
    // Replicates MainWindow::buildEmptyStruct (src/main.cpp:3458) verbatim.
    static int s_classCounter = 0;

    static uint64_t buildDefaultClass(NodeTree& tree) {
        int idx = s_classCounter++;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = QStringLiteral("instance%1").arg(idx);
        root.structTypeName = QStringLiteral("UnnamedClass%1").arg(idx);
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;
        for (int i = 0; i < 16; i++) {
            Node n;
            n.kind = NodeKind::Hex64;
            n.name = QStringLiteral("field_%1").arg(i * 8, 2, 16, QChar('0'));
            n.parentId = rootId;
            n.offset = i * 8;
            tree.addNode(n);
        }
        tree.baseAddress = 0x400000;
        return rootId;
    }
}

class TestDefaultClassFooter : public QObject {
    Q_OBJECT
private slots:
    void footerStaysCleanAcrossMultipleNewTabs();
    void editorTextMatchesComposeAcrossSequentialApplies();
    void footerSurvivesSetCommandRowThenRefresh();
};

void TestDefaultClassFooter::footerStaysCleanAcrossMultipleNewTabs()
{
    s_classCounter = 0;
    NullProvider prov;
    QStringList outputs;

    for (int tabNo = 0; tabNo < 3; tabNo++) {
        NodeTree tree;
        buildDefaultClass(tree);
        ComposeResult r = compose(tree, prov);
        outputs.append(r.text);

        QStringList lines = r.text.split('\n');
        LOG("\n=== tab %d (UnnamedClass%d) — %d lines ===\n",
            tabNo, tabNo, lines.size());
        int from = qMax(0, lines.size() - 4);
        for (int i = from; i < lines.size(); ++i)
            LOG("  L%-3d: \"%s\"\n", i, lines[i].toUtf8().constData());
    }

    for (int tabNo = 0; tabNo < outputs.size(); tabNo++) {
        QStringList lines = outputs[tabNo].split('\n');
        QVERIFY2(lines.size() >= 2,
            qPrintable(QString("tab %1 has %2 lines (need >=2)")
                       .arg(tabNo).arg(lines.size())));

        QString last = lines.back();
        QString secondToLast = lines[lines.size() - 2];

        LOG("tab %d: 2nd-to-last=\"%s\"\n", tabNo, secondToLast.toUtf8().constData());
        LOG("tab %d: last      =\"%s\"\n", tabNo, last.toUtf8().constData());

        QVERIFY2(last.contains(QStringLiteral("};")),
            qPrintable(QString("tab %1 last line missing '};': \"%2\"")
                       .arg(tabNo).arg(last)));
        QVERIFY2(!last.contains(QStringLiteral("hex64")),
            qPrintable(QString("tab %1 last line has hex64+footer merged: \"%2\"")
                       .arg(tabNo).arg(last)));
        QVERIFY2(!secondToLast.contains(QStringLiteral("};")),
            qPrintable(QString("tab %1 bytes line merged with footer: \"%2\"")
                       .arg(tabNo).arg(secondToLast)));
        QString instanceTag = QStringLiteral("instance%1").arg(tabNo);
        QVERIFY2(!last.contains(instanceTag),
            qPrintable(QString("tab %1 footer leaked instance name: \"%2\"")
                       .arg(tabNo).arg(last)));
    }
}

// Drive applyDocument across multiple compose results — proves the
// editor's diff/patch path doesn't corrupt the footer when it runs
// against compose outputs of two different "default" classes in
// sequence (which is exactly what happens on tab 2 / tab 3 in the
// live app: the same editor instance gets a new compose result).
void TestDefaultClassFooter::editorTextMatchesComposeAcrossSequentialApplies()
{
    s_classCounter = 0;
    NullProvider prov;

    auto* editor = new RcxEditor(nullptr);
    editor->resize(800, 600);

    for (int tabNo = 0; tabNo < 3; tabNo++) {
        NodeTree tree;
        buildDefaultClass(tree);
        ComposeResult r = compose(tree, prov);
        editor->applyDocument(r);
        QApplication::processEvents();

        QString sciText = editor->scintilla()->text();
        QString composeText = r.text;

        // Strip trailing newlines from both for fair comparison —
        // Scintilla may or may not append a final EOL.
        while (sciText.endsWith('\n')) sciText.chop(1);
        while (composeText.endsWith('\n')) composeText.chop(1);

        QStringList sciLines = sciText.split('\n');
        QStringList composeLines = composeText.split('\n');

        LOG("\n=== editor after applyDocument tab %d ===\n", tabNo);
        LOG("  sci lines: %d, compose lines: %d\n",
            sciLines.size(), composeLines.size());
        if (sciLines.size() >= 2)
            LOG("  sci last2:    \"%s\" | \"%s\"\n",
                sciLines[sciLines.size()-2].toUtf8().constData(),
                sciLines.last().toUtf8().constData());
        if (composeLines.size() >= 2)
            LOG("  compose last2: \"%s\" | \"%s\"\n",
                composeLines[composeLines.size()-2].toUtf8().constData(),
                composeLines.last().toUtf8().constData());

        QCOMPARE(sciLines.size(), composeLines.size());
        for (int i = 0; i < sciLines.size(); ++i) {
            if (sciLines[i] != composeLines[i]) {
                LOG("MISMATCH at line %d (tab %d):\n"
                    "  sci    : \"%s\"\n"
                    "  compose: \"%s\"\n",
                    i, tabNo,
                    sciLines[i].toUtf8().constData(),
                    composeLines[i].toUtf8().constData());
            }
            QCOMPARE(sciLines[i], composeLines[i]);
        }
    }

    delete editor;
}

// Reproduces the exact live-app sequence that breaks tabs 2/3:
//   1. applyDocument(compose result with placeholder line 0)
//   2. setCommandRowText(real command-row text)  ← line 0 differs
//   3. applyDocument(compose result with placeholder line 0)  ← refresh
// If setCommandRowText doesn't sync m_prevText, the diff in step 3
// sees a fake prefix and SCI_REPLACETARGET corrupts the document.
void TestDefaultClassFooter::footerSurvivesSetCommandRowThenRefresh()
{
    s_classCounter = 0;
    NullProvider prov;

    auto* editor = new RcxEditor(nullptr);
    editor->resize(800, 600);

    NodeTree tree;
    buildDefaultClass(tree);
    ComposeResult r = compose(tree, prov);

    // Step 1: applyDocument
    editor->applyDocument(r);
    QApplication::processEvents();

    // Step 2: rewrite line 0 with text different from compose's
    // placeholder — same as controller's updateCommandRow does.
    editor->setCommandRowText("[>] source>  0xDEADBEEF  struct UnnamedClass0 {");
    QApplication::processEvents();

    // Step 3: another applyDocument with the SAME compose result —
    // mimics a refresh tick. Triggers the diff/patch path.
    editor->applyDocument(r);
    QApplication::processEvents();

    // Verify last two lines are still pristine — bytes line,
    // then footer.
    QString sciText = editor->scintilla()->text();
    while (sciText.endsWith('\n')) sciText.chop(1);
    QStringList lines = sciText.split('\n');

    LOG("\n=== after apply→setCmdRow→apply cycle ===\n");
    for (int i = qMax(0, lines.size() - 4); i < lines.size(); ++i)
        LOG("  L%-3d: \"%s\"\n", i, lines[i].toUtf8().constData());

    QString last = lines.back();
    QString secondToLast = lines[lines.size() - 2];

    QVERIFY2(last.startsWith("};"),
        qPrintable(QString("footer corrupted: \"%1\"").arg(last)));
    QVERIFY2(!last.contains("hex64"),
        qPrintable(QString("bytes merged into footer: \"%1\"").arg(last)));
    QVERIFY2(secondToLast.contains("hex64"),
        qPrintable(QString("bytes line missing: \"%1\"").arg(secondToLast)));
    QVERIFY2(!secondToLast.contains("};"),
        qPrintable(QString("};' leaked into bytes line: \"%1\"").arg(secondToLast)));

    delete editor;
}

QTEST_MAIN(TestDefaultClassFooter)
#include "test_default_class_footer.moc"
