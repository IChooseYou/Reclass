#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QSplitter>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <QToolButton>
#include <QLineEdit>
#include <QListView>
#include <QStringListModel>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "typeselectorpopup.h"
#include "themes/thememanager.h"
#include "core.h"

Q_DECLARE_METATYPE(rcx::TypeEntry)

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
        qRegisterMetaType<TypeEntry>("TypeEntry");
    }

    // ── Chevron span detection ──

    void testChevronSpanDetected() {
        QString text = QStringLiteral("[\u25B8] source\u25BE \u00B7 0x1000 \u00B7 struct\u25BE Alpha {");
        ColumnSpan span = commandRowChevronSpan(text);
        QVERIFY(span.valid);
        QCOMPARE(span.start, 0);
        QCOMPARE(span.end, 4);  // includes trailing space for easier clicking
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

    // ── Benchmark: warmUp() + cached reuse vs cold new/delete ──

    void benchmarkPopupOpen() {
        auto makeComposite = [](uint64_t id, const QString& name, const QString& kw) {
            TypeEntry e;
            e.entryKind = TypeEntry::Composite;
            e.structId = id;
            e.displayName = name;
            e.classKeyword = kw;
            return e;
        };
        QVector<TypeEntry> types;
        types.append(makeComposite(1, "Alpha", "struct"));
        types.append(makeComposite(2, "Bravo", "struct"));
        types.append(makeComposite(3, "Charlie", "struct"));
        types.append(makeComposite(4, "Delta", "class"));

        TypeEntry cur1 = makeComposite(1, "Alpha", "struct");
        TypeEntry cur2 = makeComposite(2, "Bravo", "struct");

        QFont font("Consolas", 12);
        font.setFixedPitch(true);

        auto ms = [](qint64 ns) { return QString::number(ns / 1000000.0, 'f', 2); };

        // --- Measure cold path: new popup, first show ever ---
        {
            QElapsedTimer total;
            total.start();
            auto* popup = new TypeSelectorPopup();
            popup->setFont(font);
            popup->setTypes(types, &cur1);
            popup->popup(QPoint(100, 100));
            QApplication::processEvents();
            qint64 tCold = total.nsecsElapsed();
            popup->hide();
            QApplication::processEvents();

            qDebug() << "";
            qDebug().noquote() << QString("=== COLD (new popup, no warmUp) ===");
            qDebug().noquote() << QString("  Total: %1 ms").arg(ms(tCold));

            // --- Measure cached reuse of same instance ---
            {
                QElapsedTimer t2;
                t2.start();
                popup->setTypes(types, &cur2);
                popup->popup(QPoint(100, 100));
                QApplication::processEvents();
                qint64 tReuse = t2.nsecsElapsed();
                popup->hide();
                QApplication::processEvents();

                qDebug() << "";
                qDebug().noquote() << QString("=== WARM (reuse same popup) ===");
                qDebug().noquote() << QString("  Total: %1 ms").arg(ms(tReuse));
            }

            delete popup;
        }

        // --- Measure warmUp() approach ---
        {
            QElapsedTimer tWarmup;
            tWarmup.start();
            auto* popup2 = new TypeSelectorPopup();
            popup2->warmUp();
            qint64 tWarmMs = tWarmup.nsecsElapsed();

            qDebug() << "";
            qDebug().noquote() << QString("=== warmUp() cost (constructor + hidden show/hide) ===");
            qDebug().noquote() << QString("  Total: %1 ms").arg(ms(tWarmMs));

            // First user-visible show after warmUp
            QElapsedTimer t3;
            t3.start();
            popup2->setFont(font);
            popup2->setTypes(types, &cur1);
            popup2->popup(QPoint(100, 100));
            QApplication::processEvents();
            qint64 tFirst = t3.nsecsElapsed();
            popup2->hide();
            QApplication::processEvents();

            qDebug() << "";
            qDebug().noquote() << QString("=== FIRST visible show after warmUp() ===");
            qDebug().noquote() << QString("  Total: %1 ms").arg(ms(tFirst));

            // Second show (fully warm)
            QElapsedTimer t4;
            t4.start();
            popup2->setTypes(types, &cur2);
            popup2->popup(QPoint(100, 100));
            QApplication::processEvents();
            qint64 tSecond = t4.nsecsElapsed();
            popup2->hide();
            QApplication::processEvents();

            qDebug() << "";
            qDebug().noquote() << QString("=== SECOND visible show after warmUp() ===");
            qDebug().noquote() << QString("  Total: %1 ms").arg(ms(tSecond));

            delete popup2;
        }
    }

    // ── Popup data model ──

    void testPopupListsRootStructs() {
        NodeTree tree;
        buildTwoRootTree(tree);

        QVector<TypeEntry> types;
        for (const auto& n : tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                TypeEntry e;
                e.entryKind = TypeEntry::Composite;
                e.structId = n.id;
                e.displayName = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
                e.classKeyword = n.resolvedClassKeyword();
                types.append(e);
            }
        }

        QCOMPARE(types.size(), 2);
        QCOMPARE(types[0].displayName, QString("Alpha"));
        QCOMPARE(types[1].displayName, QString("Bravo"));
    }

    // ── Popup signals ──

    void testPopupSignals() {
        TypeSelectorPopup popup;

        TypeEntry eA;
        eA.entryKind = TypeEntry::Composite;
        eA.structId = 1;
        eA.displayName = "A";
        eA.classKeyword = "struct";
        TypeEntry eB;
        eB.entryKind = TypeEntry::Composite;
        eB.structId = 2;
        eB.displayName = "B";
        eB.classKeyword = "struct";
        QVector<TypeEntry> types;
        types.append(eA);
        types.append(eB);
        popup.setTypes(types, &eA);

        QSignalSpy typeSpy(&popup, &TypeSelectorPopup::typeSelected);
        QSignalSpy createSpy(&popup, &TypeSelectorPopup::createNewTypeRequested);

        emit popup.typeSelected(eB, QStringLiteral("B"));
        QCOMPARE(typeSpy.count(), 1);
        // Verify the entry came through — check the fullText (second arg)
        QCOMPARE(typeSpy.at(0).at(1).toString(), QStringLiteral("B"));

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

    // ── parseTypeSpec tests ──

    void testParseTypeSpecPlain() {
        TypeSpec spec = parseTypeSpec("int32_t");
        QCOMPARE(spec.baseName, QString("int32_t"));
        QVERIFY(!spec.isPointer);
        QCOMPARE(spec.arrayCount, 0);
    }

    void testParseTypeSpecArray() {
        TypeSpec spec = parseTypeSpec("int32_t[10]");
        QCOMPARE(spec.baseName, QString("int32_t"));
        QVERIFY(!spec.isPointer);
        QCOMPARE(spec.arrayCount, 10);
    }

    void testParseTypeSpecPointer() {
        TypeSpec spec = parseTypeSpec("Ball*");
        QCOMPARE(spec.baseName, QString("Ball"));
        QVERIFY(spec.isPointer);
        QCOMPARE(spec.arrayCount, 0);
    }

    void testParseTypeSpecDoublePointer() {
        TypeSpec spec = parseTypeSpec("Ball**");
        QCOMPARE(spec.baseName, QString("Ball"));
        QVERIFY(spec.isPointer);
    }

    void testParseTypeSpecEmpty() {
        TypeSpec spec = parseTypeSpec("");
        QVERIFY(spec.baseName.isEmpty());
        QVERIFY(!spec.isPointer);
        QCOMPARE(spec.arrayCount, 0);
    }

    void testParseTypeSpecWhitespace() {
        TypeSpec spec = parseTypeSpec("  Ball *  ");
        // trimmed → "Ball *", ends with '*'
        QCOMPARE(spec.baseName, QString("Ball"));
        QVERIFY(spec.isPointer);
    }

    void testParseTypeSpecArrayZero() {
        // [0] parses baseName but arrayCount stays 0 (invalid count)
        TypeSpec spec = parseTypeSpec("int32_t[0]");
        QCOMPARE(spec.baseName, QString("int32_t"));
        QCOMPARE(spec.arrayCount, 0);
    }

    // ── FieldType popup: selecting a composite (struct) type changes node kind + structTypeName + collapsed ──

    void testFieldTypeCompositeChangesNodeToStruct() {
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Find the "x" field (Int32) inside Alpha struct, and Bravo struct id
        int xIdx = -1;
        uint64_t bravoId = 0;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            const auto& n = doc->tree.nodes[i];
            if (n.name == "x" && n.kind == NodeKind::Int32) xIdx = i;
            if (n.name == "Bravo" && n.kind == NodeKind::Struct) bravoId = n.id;
        }
        QVERIFY(xIdx >= 0);
        QVERIFY(bravoId != 0);

        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);
        QVERIFY(!doc->tree.nodes[xIdx].collapsed);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Simulate the plain-struct path of applyTypePopupResult:
        // beginMacro → changeNodeKind(Struct) → ChangeStructTypeName → ChangePointerRef → endMacro
        doc->undoStack.beginMacro(QStringLiteral("Change to composite type"));
        ctrl->changeNodeKind(xIdx, NodeKind::Struct);

        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);

        int bravoIdx = doc->tree.indexOfId(bravoId);
        QVERIFY(bravoIdx >= 0);
        QString targetName = doc->tree.nodes[bravoIdx].structTypeName;

        doc->undoStack.push(new RcxCommand(ctrl,
            cmd::ChangeStructTypeName{xNodeId, doc->tree.nodes[xIdx].structTypeName, targetName}));

        // Set refId so compose can expand referenced struct children (auto-collapses)
        doc->undoStack.push(new RcxCommand(ctrl,
            cmd::ChangePointerRef{xNodeId, 0, bravoId}));

        doc->undoStack.endMacro();
        QApplication::processEvents();

        // Verify: Struct with correct name, refId, AND collapsed
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Struct);
        QCOMPARE(doc->tree.nodes[xIdx].structTypeName, QString("Bravo"));
        QCOMPARE(doc->tree.nodes[xIdx].refId, bravoId);
        QVERIFY(doc->tree.nodes[xIdx].collapsed);

        // Single undo reverses the entire macro
        doc->undoStack.undo();
        QApplication::processEvents();
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);
        QCOMPARE(doc->tree.nodes[xIdx].refId, uint64_t(0));
        QVERIFY(doc->tree.nodes[xIdx].structTypeName.isEmpty());

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── FieldType popup: selecting a composite with * modifier creates Pointer64 + refId ──

    void testFieldTypeCompositeWithPointerModifier() {
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Find the "x" field (Int32) and Bravo struct
        int xIdx = -1;
        uint64_t bravoId = 0;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            const auto& n = doc->tree.nodes[i];
            if (n.name == "x" && n.kind == NodeKind::Int32) xIdx = i;
            if (n.name == "Bravo" && n.kind == NodeKind::Struct) bravoId = n.id;
        }
        QVERIFY(xIdx >= 0);
        QVERIFY(bravoId != 0);

        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Simulate the pointer path of applyTypePopupResult:
        // beginMacro → changeNodeKind(Pointer64) → ChangePointerRef → endMacro
        doc->undoStack.beginMacro(QStringLiteral("Change to composite type"));
        ctrl->changeNodeKind(xIdx, NodeKind::Pointer64);

        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Pointer64);

        doc->undoStack.push(new RcxCommand(ctrl,
            cmd::ChangePointerRef{xNodeId, 0, bravoId}));
        doc->undoStack.endMacro();
        QApplication::processEvents();

        // Verify: Pointer64 with refId pointing to Bravo, auto-collapsed
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[xIdx].refId, bravoId);
        QVERIFY(doc->tree.nodes[xIdx].collapsed);

        // Single undo reverses the entire macro
        doc->undoStack.undo();
        QApplication::processEvents();
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);
        QCOMPARE(doc->tree.nodes[xIdx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── FieldType popup: selecting a primitive type still works ──

    void testFieldTypePrimitiveStillWorks() {
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Find the "x" field (Int32)
        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);

        // Change to Float via changeNodeKind (same path as primitive TypeEntry)
        ctrl->changeNodeKind(xIdx, NodeKind::Float);
        QApplication::processEvents();
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Float);

        // Undo
        doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── Section headers in filtered list ──

    void testSectionHeadersPresent() {
        TypeSelectorPopup popup;

        // Build entries with both primitives and composites
        QVector<TypeEntry> types;
        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = "int32_t";
        types.append(prim);

        TypeEntry comp;
        comp.entryKind = TypeEntry::Composite;
        comp.structId = 42;
        comp.displayName = "MyStruct";
        comp.classKeyword = "struct";
        types.append(comp);

        popup.setTypes(types);
        // After setTypes, the internal filtered list should have section headers
        // We can verify this indirectly by checking the model row count
        // (should be > 2 due to section headers)
        auto* listView = popup.findChild<QListView*>();
        QVERIFY(listView);
        QVERIFY(listView->model()->rowCount() > 2);
    }
};

QTEST_MAIN(TestTypeSelector)
#include "test_type_selector.moc"
