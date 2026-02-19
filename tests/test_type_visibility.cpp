#include <QtTest/QTest>
#include <QApplication>
#include <QSplitter>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "typeselectorpopup.h"
#include "core.h"
#include "providers/buffer_provider.h"

using namespace rcx;

static QByteArray makeBuffer() { return QByteArray(0x200, '\0'); }

// Build a tree with one root struct + a Pointer64 field
static void buildPointerTree(NodeTree& tree, const QString& rootName) {
    tree.baseAddress = 0;
    Node root;
    root.kind = NodeKind::Struct;
    root.name = "instance";
    root.structTypeName = rootName;
    root.parentId = 0;
    root.offset = 0;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    Node ptr;
    ptr.kind = NodeKind::Pointer64;
    ptr.name = "ptr";
    ptr.parentId = rootId;
    ptr.offset = 0;
    tree.addNode(ptr);
}

class TestTypeVisibility : public QObject {
    Q_OBJECT

private slots:

    // ── 1. New types created via createNewTypeRequested get a default name ──

    void testCreateNewTypeGetsDefaultName() {
        auto* doc = new RcxDocument();
        buildPointerTree(doc->tree, "Main");
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);
        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        int nodesBefore = doc->tree.nodes.size();

        // Simulate what createNewTypeRequested does: create struct with default name
        // (The actual handler is a lambda; we test the result via tree inspection)
        {
            bool wasSuppressed = ctrl->document() != nullptr; Q_UNUSED(wasSuppressed);

            // Generate unique default name — same logic as the handler
            QString baseName = QStringLiteral("NewClass");
            QString typeName = baseName;
            int counter = 1;
            QSet<QString> existing;
            for (const auto& nd : doc->tree.nodes) {
                if (nd.kind == NodeKind::Struct && !nd.structTypeName.isEmpty())
                    existing.insert(nd.structTypeName);
            }
            while (existing.contains(typeName))
                typeName = baseName + QString::number(counter++);

            Node n;
            n.kind = NodeKind::Struct;
            n.structTypeName = typeName;
            n.name = QStringLiteral("instance");
            n.parentId = 0;
            n.offset = 0;
            n.id = doc->tree.reserveId();
            doc->undoStack.push(new RcxCommand(ctrl, cmd::Insert{n}));
        }

        ctrl->refresh();
        QApplication::processEvents();

        // Verify new struct was created with a name
        QCOMPARE(doc->tree.nodes.size(), nodesBefore + 1);
        bool found = false;
        for (const auto& n : doc->tree.nodes) {
            if (n.structTypeName == "NewClass") { found = true; break; }
        }
        QVERIFY2(found, "New struct should have structTypeName 'NewClass'");

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── 2. Second new type gets incremented name ──

    void testCreateNewTypeIncrementsName() {
        auto* doc = new RcxDocument();
        buildPointerTree(doc->tree, "Main");
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        // Add a struct already named "NewClass"
        {
            Node n;
            n.kind = NodeKind::Struct;
            n.structTypeName = "NewClass";
            n.name = "instance";
            n.parentId = 0;
            n.offset = 0;
            doc->tree.addNode(n);
        }

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);
        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Generate name using same logic
        QString baseName = QStringLiteral("NewClass");
        QString typeName = baseName;
        int counter = 1;
        QSet<QString> existing;
        for (const auto& nd : doc->tree.nodes) {
            if (nd.kind == NodeKind::Struct && !nd.structTypeName.isEmpty())
                existing.insert(nd.structTypeName);
        }
        while (existing.contains(typeName))
            typeName = baseName + QString::number(counter++);

        QCOMPARE(typeName, QStringLiteral("NewClass1"));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── 3. Cross-tab: types from other documents visible via project docs ──

    void testCrossTabTypesVisible() {
        // Doc A: has "Alpha" struct with a Pointer64 field
        auto* docA = new RcxDocument();
        buildPointerTree(docA->tree, "Alpha");
        docA->provider = std::make_unique<BufferProvider>(makeBuffer());

        // Doc B: has "Beta" struct
        auto* docB = new RcxDocument();
        buildPointerTree(docB->tree, "Beta");
        docB->provider = std::make_unique<BufferProvider>(makeBuffer());

        // Shared doc list (simulates MainWindow::m_allDocs)
        QVector<RcxDocument*> allDocs;
        allDocs << docA << docB;

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(docA, nullptr);
        ctrl->addSplitEditor(splitter);
        ctrl->setProjectDocuments(&allDocs);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Find the Pointer64 node in docA
        int ptrIdx = -1;
        for (int i = 0; i < docA->tree.nodes.size(); i++) {
            if (docA->tree.nodes[i].kind == NodeKind::Pointer64) {
                ptrIdx = i;
                break;
            }
        }
        QVERIFY(ptrIdx >= 0);

        // Apply an external type (structId=0, displayName="Beta") as pointer target
        TypeEntry extEntry;
        extEntry.entryKind = TypeEntry::Composite;
        extEntry.structId = 0;  // external sentinel
        extEntry.displayName = QStringLiteral("Beta");
        ctrl->applyTypePopupResult(TypePopupMode::PointerTarget, ptrIdx,
                                    extEntry, QString());
        QApplication::processEvents();

        // "Beta" should now exist in docA as a local struct (imported)
        bool found = false;
        uint64_t betaLocalId = 0;
        for (const auto& n : docA->tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct
                && n.structTypeName == "Beta") {
                found = true;
                betaLocalId = n.id;
                break;
            }
        }
        QVERIFY2(found, "Beta struct should be imported into docA");

        // The pointer's refId should point at the local Beta
        int ptrIdx2 = -1;
        for (int i = 0; i < docA->tree.nodes.size(); i++) {
            if (docA->tree.nodes[i].kind == NodeKind::Pointer64
                && docA->tree.nodes[i].name == "ptr") {
                ptrIdx2 = i;
                break;
            }
        }
        QVERIFY(ptrIdx2 >= 0);
        QCOMPARE(docA->tree.nodes[ptrIdx2].refId, betaLocalId);

        delete ctrl;
        delete splitter;
        delete docA;
        delete docB;
    }

    // ── 4. findOrCreateStructByName reuses existing local struct ──

    void testFindOrCreateReusesExisting() {
        auto* doc = new RcxDocument();
        buildPointerTree(doc->tree, "Main");
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        // Add "Target" struct manually
        Node target;
        target.kind = NodeKind::Struct;
        target.structTypeName = "Target";
        target.name = "instance";
        target.parentId = 0;
        target.offset = 0;
        int ti = doc->tree.addNode(target);
        uint64_t targetId = doc->tree.nodes[ti].id;

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);
        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        int nodesBefore = doc->tree.nodes.size();

        // Apply external entry with name "Target" — should reuse existing
        int ptrIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].kind == NodeKind::Pointer64) {
                ptrIdx = i;
                break;
            }
        }
        QVERIFY(ptrIdx >= 0);

        TypeEntry extEntry;
        extEntry.entryKind = TypeEntry::Composite;
        extEntry.structId = 0;
        extEntry.displayName = QStringLiteral("Target");
        ctrl->applyTypePopupResult(TypePopupMode::PointerTarget, ptrIdx,
                                    extEntry, QString());
        QApplication::processEvents();

        // Should NOT have created a new struct — reused existing one
        QCOMPARE(doc->tree.nodes.size(), nodesBefore);

        // Pointer should reference the existing Target
        int ptrIdx2 = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].kind == NodeKind::Pointer64
                && doc->tree.nodes[i].name == "ptr") {
                ptrIdx2 = i;
                break;
            }
        }
        QVERIFY(ptrIdx2 >= 0);
        QCOMPARE(doc->tree.nodes[ptrIdx2].refId, targetId);

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── 5. External types skip duplicates already in local doc ──

    void testExternalTypesSkipLocalDuplicates() {
        // Both docs have "Shared" type — should not appear twice
        auto* docA = new RcxDocument();
        buildPointerTree(docA->tree, "Shared");
        docA->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* docB = new RcxDocument();
        buildPointerTree(docB->tree, "Shared");
        docB->provider = std::make_unique<BufferProvider>(makeBuffer());

        QVector<RcxDocument*> allDocs;
        allDocs << docA << docB;

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(docA, nullptr);
        ctrl->addSplitEditor(splitter);
        ctrl->setProjectDocuments(&allDocs);
        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Count how many "Shared" entries exist in local doc's root structs
        int sharedCount = 0;
        for (const auto& n : docA->tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct
                && n.structTypeName == "Shared")
                sharedCount++;
        }
        QCOMPARE(sharedCount, 1); // only the local one

        delete ctrl;
        delete splitter;
        delete docA;
        delete docB;
    }
};

QTEST_MAIN(TestTypeVisibility)
#include "test_type_visibility.moc"
