#include <QtTest/QTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QStandardItemModel>
#include "core.h"
#include "generator.h"
#include "controller.h"
#include "workspace_model.h"

using namespace rcx;

class TestNewFeatures : public QObject {
    Q_OBJECT

private:
    NodeTree makeSimpleTree() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Player";
        root.structTypeName = "Player";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f1;
        f1.kind = NodeKind::Int32;
        f1.name = "health";
        f1.parentId = rootId;
        f1.offset = 0;
        tree.addNode(f1);

        Node f2;
        f2.kind = NodeKind::Float;
        f2.name = "speed";
        f2.parentId = rootId;
        f2.offset = 4;
        tree.addNode(f2);

        return tree;
    }

    NodeTree makeTwoRootTree() {
        NodeTree tree;
        tree.baseAddress = 0;

        // Root struct A
        Node a;
        a.kind = NodeKind::Struct;
        a.name = "Alpha";
        a.structTypeName = "Alpha";
        a.parentId = 0;
        a.offset = 0;
        int ai = tree.addNode(a);
        uint64_t aId = tree.nodes[ai].id;

        Node af;
        af.kind = NodeKind::UInt32;
        af.name = "flagsA";
        af.parentId = aId;
        af.offset = 0;
        tree.addNode(af);

        // Root struct B
        Node b;
        b.kind = NodeKind::Struct;
        b.name = "Bravo";
        b.structTypeName = "Bravo";
        b.parentId = 0;
        b.offset = 0x100;
        int bi = tree.addNode(b);
        uint64_t bId = tree.nodes[bi].id;

        Node bf;
        bf.kind = NodeKind::UInt64;
        bf.name = "flagsB";
        bf.parentId = bId;
        bf.offset = 0;
        tree.addNode(bf);

        return tree;
    }

    NodeTree makeRichTree() {
        NodeTree tree;
        tree.baseAddress = 0x00400000;

        // ── Pet (root struct) ──
        Node pet;
        pet.kind = NodeKind::Struct;
        pet.name = "Pet";
        pet.structTypeName = "Pet";
        pet.parentId = 0;
        pet.offset = 0;
        int pi = tree.addNode(pet);
        uint64_t petId = tree.nodes[pi].id;

        { Node n; n.kind = NodeKind::Hex64;     n.name = "hex_00"; n.parentId = petId; n.offset = 0;  tree.addNode(n); }
        { Node n; n.kind = NodeKind::UTF8;      n.name = "name";   n.parentId = petId; n.offset = 8;  n.strLen = 16; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;     n.name = "hex_18"; n.parentId = petId; n.offset = 24; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex32;     n.name = "hex_20"; n.parentId = petId; n.offset = 32; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex32;     n.name = "hex_24"; n.parentId = petId; n.offset = 36; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Pointer64; n.name = "owner";  n.parentId = petId; n.offset = 40; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;     n.name = "hex_30"; n.parentId = petId; n.offset = 48; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;     n.name = "hex_38"; n.parentId = petId; n.offset = 56; tree.addNode(n); }

        // ── Cat (root struct, "inherits" Pet via nested struct) ──
        Node cat;
        cat.kind = NodeKind::Struct;
        cat.name = "Cat";
        cat.structTypeName = "Cat";
        cat.parentId = 0;
        cat.offset = 0;
        int ci = tree.addNode(cat);
        uint64_t catId = tree.nodes[ci].id;

        // base = embedded Pet (nested struct child at offset 0)
        Node base;
        base.kind = NodeKind::Struct;
        base.name = "base";
        base.structTypeName = "Pet";
        base.parentId = catId;
        base.offset = 0;
        int bi = tree.addNode(base);
        uint64_t baseId = tree.nodes[bi].id;

        // Children inside the nested Pet base
        { Node n; n.kind = NodeKind::Hex64;     n.name = "hex_00"; n.parentId = baseId; n.offset = 0;  tree.addNode(n); }
        { Node n; n.kind = NodeKind::UTF8;      n.name = "name";   n.parentId = baseId; n.offset = 8;  n.strLen = 16; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;     n.name = "hex_18"; n.parentId = baseId; n.offset = 24; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Pointer64; n.name = "owner";  n.parentId = baseId; n.offset = 32; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;     n.name = "hex_28"; n.parentId = baseId; n.offset = 40; tree.addNode(n); }

        // Cat's own fields after base
        { Node n; n.kind = NodeKind::Hex64; n.name = "hex_30";     n.parentId = catId; n.offset = 48; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64; n.name = "hex_38";     n.parentId = catId; n.offset = 56; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Float; n.name = "whiskerLen"; n.parentId = catId; n.offset = 64; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex32; n.name = "hex_44";     n.parentId = catId; n.offset = 68; tree.addNode(n); }
        { Node n; n.kind = NodeKind::UInt8; n.name = "lives";      n.parentId = catId; n.offset = 72; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex8;  n.name = "hex_49";     n.parentId = catId; n.offset = 73; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex16; n.name = "hex_4A";     n.parentId = catId; n.offset = 74; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex32; n.name = "hex_4C";     n.parentId = catId; n.offset = 76; tree.addNode(n); }

        // ── Ball (independent root struct) ──
        Node ball;
        ball.kind = NodeKind::Struct;
        ball.name = "Ball";
        ball.structTypeName = "Ball";
        ball.parentId = 0;
        ball.offset = 0;
        int bli = tree.addNode(ball);
        uint64_t ballId = tree.nodes[bli].id;

        { Node n; n.kind = NodeKind::Hex64;  n.name = "hex_00";   n.parentId = ballId; n.offset = 0;  tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;  n.name = "hex_08";   n.parentId = ballId; n.offset = 8;  tree.addNode(n); }
        { Node n; n.kind = NodeKind::Float;  n.name = "speed";    n.parentId = ballId; n.offset = 16; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex32;  n.name = "hex_14";   n.parentId = ballId; n.offset = 20; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;  n.name = "hex_18";   n.parentId = ballId; n.offset = 24; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Vec4;   n.name = "position"; n.parentId = ballId; n.offset = 32; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;  n.name = "hex_30";   n.parentId = ballId; n.offset = 48; tree.addNode(n); }
        { Node n; n.kind = NodeKind::UInt32; n.name = "color";    n.parentId = ballId; n.offset = 56; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex32;  n.name = "hex_3C";   n.parentId = ballId; n.offset = 60; tree.addNode(n); }
        { Node n; n.kind = NodeKind::Hex64;  n.name = "hex_40";   n.parentId = ballId; n.offset = 64; tree.addNode(n); }

        return tree;
    }

private slots:

    // ═══════════════════════════════════════════════════
    // Feature 1: Type Aliases
    // ═══════════════════════════════════════════════════

    void testResolveTypeName_noAlias() {
        RcxDocument doc;
        // No aliases set — should return default type name
        QString name = doc.resolveTypeName(NodeKind::Int32);
        QCOMPARE(name, QString("int32_t"));

        name = doc.resolveTypeName(NodeKind::Float);
        QCOMPARE(name, QString("float"));

        name = doc.resolveTypeName(NodeKind::Hex64);
        QCOMPARE(name, QString("hex64"));
    }

    void testResolveTypeName_withAlias() {
        RcxDocument doc;
        doc.typeAliases[NodeKind::Int32] = "DWORD";
        doc.typeAliases[NodeKind::Float] = "FLOAT";

        QCOMPARE(doc.resolveTypeName(NodeKind::Int32), QString("DWORD"));
        QCOMPARE(doc.resolveTypeName(NodeKind::Float), QString("FLOAT"));
        // Non-aliased types still return default
        QCOMPARE(doc.resolveTypeName(NodeKind::UInt64), QString("uint64_t"));
    }

    void testResolveTypeName_emptyAlias() {
        RcxDocument doc;
        doc.typeAliases[NodeKind::Int32] = "";  // empty alias should be ignored
        QCOMPARE(doc.resolveTypeName(NodeKind::Int32), QString("int32_t"));
    }

    void testTypeAliases_saveLoad() {
        // Save a document with type aliases, reload, verify aliases persist
        QTemporaryFile tmpFile;
        tmpFile.setAutoRemove(true);
        QVERIFY(tmpFile.open());
        QString path = tmpFile.fileName();
        tmpFile.close();

        // Create document with aliases and save
        {
            RcxDocument doc;
            doc.tree = makeSimpleTree();
            doc.typeAliases[NodeKind::Int32] = "DWORD";
            doc.typeAliases[NodeKind::Float] = "FLOAT";
            QVERIFY(doc.save(path));
        }

        // Reload and check aliases
        {
            RcxDocument doc;
            QVERIFY(doc.load(path));
            QCOMPARE(doc.typeAliases.size(), 2);
            QCOMPARE(doc.typeAliases.value(NodeKind::Int32), QString("DWORD"));
            QCOMPARE(doc.typeAliases.value(NodeKind::Float), QString("FLOAT"));
        }
    }

    void testTypeAliases_saveLoadEmpty() {
        // Save without aliases, reload, verify no aliases
        QTemporaryFile tmpFile;
        tmpFile.setAutoRemove(true);
        QVERIFY(tmpFile.open());
        QString path = tmpFile.fileName();
        tmpFile.close();

        {
            RcxDocument doc;
            doc.tree = makeSimpleTree();
            QVERIFY(doc.save(path));
        }

        {
            RcxDocument doc;
            QVERIFY(doc.load(path));
            QVERIFY(doc.typeAliases.isEmpty());
        }
    }

    void testTypeAliases_jsonFormat() {
        // Verify the JSON format of saved aliases
        QTemporaryFile tmpFile;
        tmpFile.setAutoRemove(true);
        QVERIFY(tmpFile.open());
        QString path = tmpFile.fileName();
        tmpFile.close();

        RcxDocument doc;
        doc.tree = makeSimpleTree();
        doc.typeAliases[NodeKind::UInt32] = "UINT";
        QVERIFY(doc.save(path));

        // Read raw JSON
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonDocument jdoc = QJsonDocument::fromJson(file.readAll());
        QJsonObject root = jdoc.object();

        QVERIFY(root.contains("typeAliases"));
        QJsonObject aliases = root["typeAliases"].toObject();
        QCOMPARE(aliases["UInt32"].toString(), QString("UINT"));
    }

    void testGenerator_typeAliases() {
        // Generator should use aliases for field types
        auto tree = makeSimpleTree();
        uint64_t rootId = tree.nodes[0].id;

        QHash<NodeKind, QString> aliases;
        aliases[NodeKind::Int32] = "LONG";
        aliases[NodeKind::Float] = "FLOAT";

        QString result = renderCpp(tree, rootId, &aliases);

        QVERIFY(result.contains("LONG health;"));
        QVERIFY(result.contains("FLOAT speed;"));
        // struct keyword itself should not be aliased
        QVERIFY(result.contains("struct Player {"));
    }

    void testGenerator_typeAliases_null() {
        // With nullptr aliases, should behave like before
        auto tree = makeSimpleTree();
        uint64_t rootId = tree.nodes[0].id;

        QString result = renderCpp(tree, rootId, nullptr);
        QVERIFY(result.contains("int32_t health;"));
        QVERIFY(result.contains("float speed;"));
    }

    void testGenerator_typeAliases_array() {
        // Array element type should use alias
        NodeTree tree;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "ArrTest";
        root.structTypeName = "ArrTest";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "data";
        arr.parentId = rootId;
        arr.offset = 0;
        arr.arrayLen = 16;
        arr.elementKind = NodeKind::UInt32;
        tree.addNode(arr);

        QHash<NodeKind, QString> aliases;
        aliases[NodeKind::UInt32] = "DWORD";

        QString result = renderCpp(tree, rootId, &aliases);
        QVERIFY(result.contains("DWORD data[16];"));
    }

    void testGenerator_renderCppAll_typeAliases() {
        auto tree = makeTwoRootTree();

        QHash<NodeKind, QString> aliases;
        aliases[NodeKind::UInt32] = "DWORD";
        aliases[NodeKind::UInt64] = "QWORD";

        QString result = renderCppAll(tree, &aliases);
        QVERIFY(result.contains("DWORD flagsA;"));
        QVERIFY(result.contains("QWORD flagsB;"));
    }

    // ═══════════════════════════════════════════════════
    // Feature 3: Per-Window View Root Class
    // ═══════════════════════════════════════════════════

    void testCompose_viewRootId_zero() {
        // viewRootId=0 should show all roots (same as default)
        auto tree = makeTwoRootTree();

        NullProvider prov;
        ComposeResult result = compose(tree, prov, 0);

        // Should have content from both structs
        QStringList lines = result.text.split('\n');
        bool foundFlagsA = false, foundFlagsB = false;
        for (const QString& l : lines) {
            if (l.contains("flagsA")) foundFlagsA = true;
            if (l.contains("flagsB")) foundFlagsB = true;
        }
        QVERIFY2(foundFlagsA, "viewRootId=0 should include Alpha struct");
        QVERIFY2(foundFlagsB, "viewRootId=0 should include Bravo struct");
    }

    void testCompose_viewRootId_filter() {
        // viewRootId set to Alpha's id should only show Alpha's fields
        auto tree = makeTwoRootTree();
        uint64_t alphaId = tree.nodes[0].id;

        NullProvider prov;
        ComposeResult result = compose(tree, prov, alphaId);

        QStringList lines = result.text.split('\n');
        bool foundFlagsA = false, foundFlagsB = false;
        for (const QString& l : lines) {
            if (l.contains("flagsA")) foundFlagsA = true;
            if (l.contains("flagsB")) foundFlagsB = true;
        }
        QVERIFY2(foundFlagsA, "viewRootId=Alpha should include Alpha's fields");
        QVERIFY2(!foundFlagsB, "viewRootId=Alpha should NOT include Bravo's fields");
    }

    void testCompose_viewRootId_otherRoot() {
        // viewRootId set to Bravo's id should only show Bravo's fields
        auto tree = makeTwoRootTree();
        uint64_t bravoId = tree.nodes[2].id;  // Bravo is the 3rd node (index 2)

        NullProvider prov;
        ComposeResult result = compose(tree, prov, bravoId);

        QStringList lines = result.text.split('\n');
        bool foundFlagsA = false, foundFlagsB = false;
        for (const QString& l : lines) {
            if (l.contains("flagsA")) foundFlagsA = true;
            if (l.contains("flagsB")) foundFlagsB = true;
        }
        QVERIFY2(!foundFlagsA, "viewRootId=Bravo should NOT include Alpha's fields");
        QVERIFY2(foundFlagsB, "viewRootId=Bravo should include Bravo's fields");
    }

    void testCompose_viewRootId_invalid() {
        // viewRootId pointing to non-existent node: should show nothing (only command rows)
        auto tree = makeTwoRootTree();

        NullProvider prov;
        ComposeResult result = compose(tree, prov, 99999);

        // Only command row
        QCOMPARE(result.meta.size(), 1);
        QCOMPARE(result.meta[0].lineKind, LineKind::CommandRow);
    }

    void testCompose_viewRootId_singleRoot() {
        // Single root tree with viewRootId set to that root — should work normally
        auto tree = makeSimpleTree();
        uint64_t rootId = tree.nodes[0].id;

        NullProvider prov;
        ComposeResult full = compose(tree, prov, 0);
        ComposeResult filtered = compose(tree, prov, rootId);

        // Both should have same number of lines (only one root anyway)
        QCOMPARE(full.meta.size(), filtered.meta.size());
    }

    void testDocument_compose_viewRootId() {
        // Test RcxDocument::compose passes viewRootId through
        RcxDocument doc;
        doc.tree = makeTwoRootTree();
        uint64_t alphaId = doc.tree.nodes[0].id;

        ComposeResult fullResult = doc.compose(0);
        ComposeResult filtered = doc.compose(alphaId);

        // Filtered should have fewer lines than full
        QVERIFY(filtered.meta.size() < fullResult.meta.size());

        // Filtered should have Alpha's fields
        bool foundFlagsA = false;
        for (const QString& l : filtered.text.split('\n')) {
            if (l.contains("flagsA")) foundFlagsA = true;
        }
        QVERIFY(foundFlagsA);
    }

    // ═══════════════════════════════════════════════════
    // Feature 2: Project Lifecycle API (document-level)
    // ═══════════════════════════════════════════════════

    void testDocument_saveLoadPreservesData() {
        // Verify save/load round-trip preserves tree + aliases + baseAddress
        QTemporaryFile tmpFile;
        tmpFile.setAutoRemove(true);
        QVERIFY(tmpFile.open());
        QString path = tmpFile.fileName();
        tmpFile.close();

        {
            RcxDocument doc;
            doc.tree = makeTwoRootTree();
            doc.tree.baseAddress = 0xDEADBEEF;
            doc.typeAliases[NodeKind::Int32] = "INT";
            QVERIFY(doc.save(path));
        }

        {
            RcxDocument doc;
            QVERIFY(doc.load(path));
            QCOMPARE(doc.tree.baseAddress, (uint64_t)0xDEADBEEF);
            QCOMPARE(doc.tree.nodes.size(), 4);  // 2 roots + 2 fields
            QCOMPARE(doc.typeAliases.value(NodeKind::Int32), QString("INT"));
            QCOMPARE(doc.filePath, path);
            QVERIFY(!doc.modified);
        }
    }

    void testDocument_saveCreatesFile() {
        QTemporaryFile tmpFile;
        tmpFile.setAutoRemove(true);
        QVERIFY(tmpFile.open());
        QString path = tmpFile.fileName();
        tmpFile.close();

        RcxDocument doc;
        doc.tree = makeSimpleTree();
        QVERIFY(doc.save(path));
        QCOMPARE(doc.filePath, path);
        QVERIFY(!doc.modified);

        // Verify file exists and is valid JSON
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonDocument jdoc = QJsonDocument::fromJson(file.readAll());
        QVERIFY(!jdoc.isNull());
        QVERIFY(jdoc.object().contains("nodes"));
    }

    void testDocument_loadInvalidPath() {
        RcxDocument doc;
        QVERIFY(!doc.load("/nonexistent/path/file.rcx"));
    }

    // ═══════════════════════════════════════════════════
    // Integration: Type aliases + compose + generator
    // ═══════════════════════════════════════════════════

    // ═══════════════════════════════════════════════════
    // Feature 4: Workspace Model
    // ═══════════════════════════════════════════════════

    void testWorkspace_simpleTree() {
        auto tree = makeSimpleTree();
        QStandardItemModel model;
        QVector<TabInfo> tabs = {{ &tree, "TestProject.rcx", nullptr }};
        buildProjectExplorer(&model, tabs);

        // Single "Project" root
        QCOMPARE(model.rowCount(), 1);
        QStandardItem* project = model.item(0);
        QCOMPARE(project->text(), QString("Project"));

        // 1 type directly under Project: Player (no member fields)
        QCOMPARE(project->rowCount(), 1);
        QVERIFY(project->child(0)->text().contains("Player"));
        QVERIFY(project->child(0)->text().contains("struct"));
        QCOMPARE(project->child(0)->rowCount(), 0);
    }

    void testWorkspace_twoRootTree() {
        auto tree = makeTwoRootTree();
        QStandardItemModel model;
        QVector<TabInfo> tabs = {{ &tree, "TwoRoot.rcx", nullptr }};
        buildProjectExplorer(&model, tabs);

        QCOMPARE(model.rowCount(), 1);
        QStandardItem* project = model.item(0);

        // 2 types sorted alphabetically: Alpha, Bravo (no field children)
        QCOMPARE(project->rowCount(), 2);
        QVERIFY(project->child(0)->text().contains("Alpha"));
        QVERIFY(project->child(1)->text().contains("Bravo"));
        QCOMPARE(project->child(0)->rowCount(), 0);
        QCOMPARE(project->child(1)->rowCount(), 0);
    }

    void testWorkspace_richTree_rootCount() {
        auto tree = makeRichTree();
        QStandardItemModel model;
        QVector<TabInfo> tabs = {{ &tree, "Rich.rcx", nullptr }};
        buildProjectExplorer(&model, tabs);

        QStandardItem* project = model.item(0);
        QCOMPARE(project->rowCount(), 3);  // Ball, Cat, Pet (sorted)
    }

    void testWorkspace_richTree_sorted() {
        auto tree = makeRichTree();
        QStandardItemModel model;
        QVector<TabInfo> tabs = {{ &tree, "Rich.rcx", nullptr }};
        buildProjectExplorer(&model, tabs);

        QStandardItem* project = model.item(0);
        // Sorted alphabetically: Ball, Cat, Pet
        QVERIFY(project->child(0)->text().contains("Ball"));
        QVERIFY(project->child(1)->text().contains("Cat"));
        QVERIFY(project->child(2)->text().contains("Pet"));
        // No member fields under type nodes
        QCOMPARE(project->child(0)->rowCount(), 0);
        QCOMPARE(project->child(1)->rowCount(), 0);
        QCOMPARE(project->child(2)->rowCount(), 0);
    }

    void testWorkspace_emptyTree() {
        NodeTree tree;
        QStandardItemModel model;
        QVector<TabInfo> tabs = {{ &tree, "Empty.rcx", nullptr }};
        buildProjectExplorer(&model, tabs);

        // Still has the "Project" root, just no children
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.item(0)->text(), QString("Project"));
        QCOMPARE(model.item(0)->rowCount(), 0);
    }

    void testWorkspace_structIdRole() {
        auto tree = makeSimpleTree();
        QStandardItemModel model;
        QVector<TabInfo> tabs = {{ &tree, "Test.rcx", nullptr }};
        buildProjectExplorer(&model, tabs);

        QStandardItem* project = model.item(0);
        // Project root has kGroupSentinel
        QCOMPARE(project->data(Qt::UserRole + 1).toULongLong(), kGroupSentinel);

        // Player type item should have structId
        QStandardItem* player = project->child(0);
        QVERIFY(player->data(Qt::UserRole + 1).isValid());
        QVERIFY(player->data(Qt::UserRole + 1).toULongLong() > 0);
        QVERIFY(player->data(Qt::UserRole + 1).toULongLong() != kGroupSentinel);
    }

    // ═══════════════════════════════════════════════════
    // Feature: Double-click navigation (viewRootId + scroll)
    // ═══════════════════════════════════════════════════

    void testDoubleClick_switchToCollapsedClass() {
        // Simulates: Ball is collapsed (hidden). Double-click Ball in workspace
        // → uncollapse, set viewRootId, compose shows only Ball with children.
        RcxDocument doc;
        doc.tree = makeRichTree();

        // Collapse Ball (3rd root struct)
        uint64_t ballId = 0;
        for (auto& node : doc.tree.nodes) {
            if (node.parentId == 0 && node.kind == NodeKind::Struct
                && node.structTypeName == "Ball") {
                node.collapsed = true;
                ballId = node.id;
                break;
            }
        }
        QVERIFY(ballId != 0);

        // Compose with viewRootId=0 should skip collapsed Ball
        {
            NullProvider prov;
            ComposeResult result = compose(doc.tree, prov, 0);
            bool foundSpeed = false;
            for (const auto& lm : result.meta) {
                int ni = lm.nodeIdx;
                if (ni >= 0 && ni < doc.tree.nodes.size()
                    && doc.tree.nodes[ni].name == "speed")
                    foundSpeed = true;
            }
            QVERIFY2(!foundSpeed, "Collapsed Ball's children should not appear with viewRootId=0");
        }

        // Simulate double-click: uncollapse Ball + set viewRootId
        int bi = doc.tree.indexOfId(ballId);
        QVERIFY(bi >= 0);
        doc.tree.nodes[bi].collapsed = false;

        // Compose with viewRootId=Ball should show Ball and its children
        {
            NullProvider prov;
            ComposeResult result = compose(doc.tree, prov, ballId);
            bool foundSpeed = false, foundPosition = false, foundColor = false;
            for (const auto& lm : result.meta) {
                int ni = lm.nodeIdx;
                if (ni < 0 || ni >= doc.tree.nodes.size()) continue;
                const QString& name = doc.tree.nodes[ni].name;
                if (name == "speed")    foundSpeed = true;
                if (name == "position") foundPosition = true;
                if (name == "color")    foundColor = true;
            }
            QVERIFY2(foundSpeed, "Ball's speed field should appear");
            QVERIFY2(foundPosition, "Ball's position field should appear");
            QVERIFY2(foundColor, "Ball's color field should appear");
        }

        // Pet/Cat fields should NOT be in the Ball-filtered result
        {
            NullProvider prov;
            ComposeResult result = compose(doc.tree, prov, ballId);
            bool foundPetField = false;
            for (const auto& lm : result.meta) {
                int ni = lm.nodeIdx;
                if (ni < 0 || ni >= doc.tree.nodes.size()) continue;
                if (doc.tree.nodes[ni].name == "owner") foundPetField = true;
            }
            QVERIFY2(!foundPetField, "Pet's owner should not appear when viewing Ball");
        }
    }

    void testDoubleClick_fieldNavigatesToParentRoot() {
        // Simulates: double-click a field inside Ball → walk up to Ball root,
        // set viewRootId to Ball, and the field should be in the compose output.
        RcxDocument doc;
        doc.tree = makeRichTree();

        // Find Ball's "speed" child
        uint64_t ballId = 0, speedId = 0;
        for (auto& node : doc.tree.nodes) {
            if (node.parentId == 0 && node.structTypeName == "Ball")
                ballId = node.id;
        }
        QVERIFY(ballId != 0);
        for (auto& node : doc.tree.nodes) {
            if (node.parentId == ballId && node.name == "speed")
                speedId = node.id;
        }
        QVERIFY(speedId != 0);

        // Walk up from speed to find root struct (simulating handler logic)
        uint64_t rootId = 0;
        uint64_t cur = speedId;
        while (cur != 0) {
            int idx = doc.tree.indexOfId(cur);
            if (idx < 0) break;
            if (doc.tree.nodes[idx].parentId == 0) { rootId = cur; break; }
            cur = doc.tree.nodes[idx].parentId;
        }
        QCOMPARE(rootId, ballId);

        // Compose with viewRootId=Ball should contain speed
        NullProvider prov;
        ComposeResult result = compose(doc.tree, prov, ballId);
        bool foundSpeed = false;
        for (const auto& lm : result.meta) {
            if (lm.nodeId == speedId) { foundSpeed = true; break; }
        }
        QVERIFY2(foundSpeed, "speed field should be in compose output when viewing its root");
    }

    void testDoubleClick_projectRootShowsAll() {
        // Double-click project root clears viewRootId → all non-collapsed roots shown
        RcxDocument doc;
        doc.tree = makeRichTree();

        // Collapse Ball
        for (auto& node : doc.tree.nodes) {
            if (node.parentId == 0 && node.structTypeName == "Ball")
                node.collapsed = true;
        }

        // viewRootId=0 → Pet and Cat visible, Ball hidden
        NullProvider prov;
        ComposeResult result = compose(doc.tree, prov, 0);
        bool foundOwner = false, foundWhiskerLen = false, foundSpeed = false;
        for (const auto& lm : result.meta) {
            int ni = lm.nodeIdx;
            if (ni < 0 || ni >= doc.tree.nodes.size()) continue;
            const QString& name = doc.tree.nodes[ni].name;
            if (name == "owner")      foundOwner = true;
            if (name == "whiskerLen") foundWhiskerLen = true;
            if (name == "speed")      foundSpeed = true;
        }
        QVERIFY2(foundOwner, "Pet's owner should appear with viewRootId=0");
        QVERIFY2(foundWhiskerLen, "Cat's whiskerLen should appear with viewRootId=0");
        QVERIFY2(!foundSpeed, "Collapsed Ball's speed should not appear with viewRootId=0");
    }

    // ═══════════════════════════════════════════════════
    // Integration: Type aliases + compose + generator
    // ═══════════════════════════════════════════════════

    void testAliasesPreservedThroughSaveReloadCompose() {
        // Full workflow: set aliases, save, reload, compose + generate
        QTemporaryFile tmpFile;
        tmpFile.setAutoRemove(true);
        QVERIFY(tmpFile.open());
        QString path = tmpFile.fileName();
        tmpFile.close();

        auto tree = makeSimpleTree();

        // Save with aliases
        {
            RcxDocument doc;
            doc.tree = tree;
            doc.typeAliases[NodeKind::Int32] = "my_int32";
            doc.typeAliases[NodeKind::Float] = "my_float";
            QVERIFY(doc.save(path));
        }

        // Reload and verify compose + generate work
        {
            RcxDocument doc;
            QVERIFY(doc.load(path));

            // Compose should succeed
            ComposeResult result = doc.compose();
            QVERIFY(result.meta.size() > 0);

            // Generator should use aliases
            uint64_t rootId = doc.tree.nodes[0].id;
            const QHash<NodeKind, QString>* aliases =
                doc.typeAliases.isEmpty() ? nullptr : &doc.typeAliases;
            QString cpp = renderCpp(doc.tree, rootId, aliases);
            QVERIFY(cpp.contains("my_int32 health;"));
            QVERIFY(cpp.contains("my_float speed;"));
        }
    }
    void testVec4SingleLineValue() {
        NodeTree tree;
        tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Obj";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node v;
        v.kind = NodeKind::Vec4;
        v.name = "position";
        v.parentId = rootId;
        v.offset = 0;
        tree.addNode(v);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // CommandRow + 1 Vec4 line + footer = 3
        QCOMPARE(result.meta.size(), 3);

        // The Vec4 line (index 1) is a single field line, not continuation
        QCOMPARE(result.meta[1].lineKind, LineKind::Field);
        QCOMPARE(result.meta[1].nodeKind, NodeKind::Vec4);
        QVERIFY(!result.meta[1].isContinuation);

        // Copy text (equivalent to editor's "Copy All as Text")
        QString text = result.text;
        // NullProvider reads 0 for all floats, values are "0.f, 0.f, 0.f, 0.f"
        QVERIFY(text.contains("0.f, 0.f, 0.f, 0.f"));
        // Confirm type, name, and values all on the same line
        QStringList lines = text.split('\n');
        QVERIFY(lines[1].contains("vec4"));
        QVERIFY(lines[1].contains("position"));
        QVERIFY(lines[1].contains("0.f, 0.f, 0.f, 0.f"));
    }
};

QTEST_MAIN(TestNewFeatures)
#include "test_new_features.moc"
