// Regression tests for the Project-explorer workspace model (workspace_model.h),
// specifically the empty-state row count that drives the EmptyHintTreeView
// "No types yet" overlay in the workspace dock.
#include "workspace_model.h"
#include <QtTest/QtTest>
#include <QStandardItemModel>

using namespace rcx;

class TestWorkspace : public QObject {
    Q_OBJECT
private slots:
    // An empty project (no tabs) must yield ZERO rows so the tree's empty-state
    // overlay can fire. Regression: buildProjectExplorer used to append an
    // UNCONDITIONAL "ALL TYPES" section header, leaving the model permanently at
    // rowCount >= 1 — the overlay's `rowCount > 0 -> return` guard then never
    // painted the placeholder on a genuinely empty project.
    void testEmptyProjectHasNoRows() {
        QStandardItemModel model;
        buildProjectExplorer(&model, {}, {});
        QCOMPARE(model.rowCount(), 0);
    }

    // A tab whose tree holds no top-level Struct types is empty for the explorer
    // too (only Struct nodes are listed), so still zero rows.
    void testNonStructTabHasNoRows() {
        NodeTree tree;
        Node n; n.kind = NodeKind::Hex64; n.parentId = 0; tree.addNode(n);
        QVector<TabInfo> tabs{ TabInfo{ &tree, QStringLiteral("T"), nullptr } };
        QStandardItemModel model;
        buildProjectExplorer(&model, tabs, {});
        QCOMPARE(model.rowCount(), 0);
    }

    // One struct type → "ALL TYPES" header row + 1 type row (header still emits
    // when there's content under it).
    void testStructTabHasHeaderAndRow() {
        NodeTree tree;
        Node s; s.kind = NodeKind::Struct;
        s.structTypeName = QStringLiteral("MyType"); s.parentId = 0;
        tree.addNode(s);
        QVector<TabInfo> tabs{ TabInfo{ &tree, QStringLiteral("T"), nullptr } };
        QStandardItemModel model;
        buildProjectExplorer(&model, tabs, {});
        QCOMPARE(model.rowCount(), 2);   // ALL TYPES header + the type
        QVERIFY(!model.item(0)->data(RoleSectionHeader).toString().isEmpty());
        QVERIFY(model.item(1)->data(RoleSectionHeader).toString().isEmpty());
    }
};

QTEST_MAIN(TestWorkspace)
#include "test_workspace.moc"
