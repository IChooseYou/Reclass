#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QSplitter>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "typeselectorpopup.h"
#include "core.h"

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
Q_DECLARE_METATYPE(uint64_t)
#endif

using namespace rcx;

static void buildTwoRootTree(NodeTree& tree) {
    tree.baseAddress = 0x1000;

    Node a;
    a.kind = NodeKind::Struct;
    a.name = "Alpha";
    a.structTypeName = "Alpha";
    a.parentId = 0;
    a.offset = 0;
    int ai = tree.addNode(a);
    uint64_t aId = tree.nodes[ai].id;

    { Node n; n.kind = NodeKind::Int32; n.name = "x"; n.parentId = aId; n.offset = 0; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Int32; n.name = "y"; n.parentId = aId; n.offset = 4; tree.addNode(n); }

    Node b;
    b.kind = NodeKind::Struct;
    b.name = "Bravo";
    b.structTypeName = "Bravo";
    b.parentId = 0;
    b.offset = 0x100;
    int bi = tree.addNode(b);
    uint64_t bId = tree.nodes[bi].id;

    { Node n; n.kind = NodeKind::Float; n.name = "speed"; n.parentId = bId; n.offset = 0; tree.addNode(n); }
}

static QByteArray makeBuffer() {
    return QByteArray(0x200, '\0');
}

class TestTypeSelector : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        qRegisterMetaType<uint64_t>("uint64_t");
#endif
    }

    // ── Chevron span detection ──

    void testChevronSpanDetected() {
        QString text = QStringLiteral("[\u25B8] source\u25BE \u00B7 0x1000 \u00B7 struct\u25BE Alpha {");
        ColumnSpan span = commandRowChevronSpan(text);
        QVERIFY(span.valid);
        QCOMPARE(span.start, 0);
        QCOMPARE(span.end, 3);
    }

    void testChevronSpanRejects() {
        QVERIFY(!commandRowChevronSpan(QStringLiteral("Hi")).valid);
        QVERIFY(!commandRowChevronSpan(QStringLiteral("\u25B8 source")).valid);
        // Old down-triangle glyph must not match
        QVERIFY(!commandRowChevronSpan(QStringLiteral("[\u25BE] source")).valid);
    }

    // ── Existing spans unbroken by chevron prefix ──

    void testSpansWithPrefix() {
        QString text = QStringLiteral("[\u25B8] source\u25BE \u00B7 0x1000 \u00B7 struct\u25BE Alpha {");

        ColumnSpan src = commandRowSrcSpan(text);
        QVERIFY(src.valid);
        QVERIFY(text.mid(src.start, src.end - src.start).contains("source"));

        ColumnSpan addr = commandRowAddrSpan(text);
        QVERIFY(addr.valid);
        QVERIFY(text.mid(addr.start, addr.end - addr.start).contains("0x1000"));

        ColumnSpan rootName = commandRowRootNameSpan(text);
        QVERIFY(rootName.valid);
        QCOMPARE(text.mid(rootName.start, rootName.end - rootName.start).trimmed(), QString("Alpha"));
    }

    // ── Popup data model ──

    void testPopupListsRootStructs() {
        NodeTree tree;
        buildTwoRootTree(tree);

        QVector<TypeEntry> types;
        for (const auto& n : tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                types.append({n.id, n.structTypeName.isEmpty() ? n.name : n.structTypeName,
                              n.resolvedClassKeyword()});
            }
        }

        QCOMPARE(types.size(), 2);
        QCOMPARE(types[0].displayName, QString("Alpha"));
        QCOMPARE(types[1].displayName, QString("Bravo"));
    }

    // ── Popup signals ──

    void testPopupSignals() {
        TypeSelectorPopup popup;
        popup.setTypes({{1, "A", "struct"}, {2, "B", "struct"}}, 1);

        QSignalSpy typeSpy(&popup, &TypeSelectorPopup::typeSelected);
        QSignalSpy createSpy(&popup, &TypeSelectorPopup::createNewTypeRequested);

        emit popup.typeSelected(2);
        QCOMPARE(typeSpy.count(), 1);
        QCOMPARE(typeSpy.at(0).at(0).toULongLong(), (uint64_t)2);

        emit popup.createNewTypeRequested();
        QCOMPARE(createSpy.count(), 1);
    }

    // ── Full GUI integration ──
    // Single test method to avoid QScintilla reinit issues.

    void testViewSwitchingAndCreateType() {
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        auto* editor = ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));

        // Initial refresh so compose populates meta + editor text
        ctrl->refresh();
        QApplication::processEvents();

        auto* sci = editor->scintilla();

        // -- Command row starts with [U+25B8] --
        {
            const LineMeta* meta = editor->metaForLine(0);
            QVERIFY(meta);
            QCOMPARE(meta->lineKind, LineKind::CommandRow);

            QString line0 = sci->text(0);
            if (line0.endsWith('\n')) line0.chop(1);
            QVERIFY2(line0.startsWith(QStringLiteral("[\u25B8]")),
                     qPrintable("Expected chevron prefix, got: " + line0.left(10)));
        }

        // -- Find root IDs --
        uint64_t alphaId = 0, bravoId = 0;
        for (const auto& n : doc->tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                if (n.name == "Alpha") alphaId = n.id;
                if (n.name == "Bravo") bravoId = n.id;
            }
        }
        QVERIFY(alphaId != 0);
        QVERIFY(bravoId != 0);
        QCOMPARE(ctrl->viewRootId(), (uint64_t)0);

        // -- Switch to Bravo: command row + fields update --
        ctrl->setViewRootId(bravoId);
        QApplication::processEvents();

        QCOMPARE(ctrl->viewRootId(), bravoId);
        QVERIFY2(sci->text(0).contains("Bravo"),
                 qPrintable("Expected 'Bravo' in command row, got: " + sci->text(0)));
        QVERIFY2(sci->text().contains("speed"),
                 "View should show Bravo's 'speed' field");

        // -- Switch to Alpha --
        ctrl->setViewRootId(alphaId);
        QApplication::processEvents();

        QCOMPARE(ctrl->viewRootId(), alphaId);
        QVERIFY2(sci->text(0).contains("Alpha"),
                 qPrintable("Expected 'Alpha' in command row, got: " + sci->text(0)));

        // -- Create new type (no name) --
        int nodesBefore = doc->tree.nodes.size();

        Node newNode;
        newNode.kind = NodeKind::Struct;
        newNode.name = QString();
        newNode.parentId = 0;
        newNode.offset = 0;
        newNode.id = doc->tree.reserveId();
        uint64_t newId = newNode.id;

        doc->undoStack.push(new RcxCommand(ctrl, cmd::Insert{newNode}));
        ctrl->setViewRootId(newId);
        QApplication::processEvents();

        // Verify new struct
        int idx = doc->tree.indexOfId(newId);
        QVERIFY(idx >= 0);
        QVERIFY(doc->tree.nodes[idx].name.isEmpty());
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Struct);
        QCOMPARE(doc->tree.nodes[idx].parentId, (uint64_t)0);
        QCOMPARE(ctrl->viewRootId(), newId);

        // Command row shows "NoName" for empty-named struct
        QVERIFY2(sci->text(0).contains("NoName"),
                 qPrintable("Expected 'NoName' in command row, got: " + sci->text(0)));

        // -- Undo removes the new struct --
        doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(doc->tree.nodes.size(), nodesBefore);

        // Cleanup
        delete ctrl;
        delete splitter;
        delete doc;
    }
};

QTEST_MAIN(TestTypeSelector)
#include "test_type_selector.moc"
