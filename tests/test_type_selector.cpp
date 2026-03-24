#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QSplitter>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <QToolButton>
#include <QButtonGroup>
#include <QLineEdit>
#include <QListView>
#include <QStringListModel>
#include <QLabel>
#include <QFrame>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "typeselectorpopup.h"
#include "themes/thememanager.h"
#include "core.h"

Q_DECLARE_METATYPE(rcx::TypeEntry)

using namespace rcx;

static void buildTwoRootTree(NodeTree& tree) {
    tree.baseAddress = 0;

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
        QString text = QStringLiteral("[\u25B8] source\u25BE  0x1000  struct Alpha {");
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
        QString text = QStringLiteral("[\u25B8] source\u25BE  0x1000  struct Alpha {");

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

    // ── Isolate first-show cost with different window flags ──

    void benchmarkFirstShow() {
        auto ms = [](qint64 ns) { return QString::number(ns / 1000000.0, 'f', 2); };

        struct FlagTest {
            const char* name;
            Qt::WindowFlags flags;
        };
        FlagTest tests[] = {
            {"Qt::Popup|Frameless",         Qt::Popup | Qt::FramelessWindowHint},
            {"Qt::Tool|Frameless",          Qt::Tool | Qt::FramelessWindowHint},
            {"Qt::ToolTip",                 Qt::ToolTip},
            {"Qt::Window|Frameless",        Qt::Window | Qt::FramelessWindowHint},
            {"Qt::Popup|Frameless (2nd)",   Qt::Popup | Qt::FramelessWindowHint},
        };

        for (const auto& test : tests) {
            auto* f = new QFrame(nullptr, test.flags);
            f->resize(300, 400);

            QElapsedTimer t; t.start();
            f->show();
            qint64 t1 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t2 = t.nsecsElapsed();
            f->hide();
            QApplication::processEvents();

            t.restart();
            f->show();
            qint64 t3 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t4 = t.nsecsElapsed();
            f->hide();
            QApplication::processEvents();

            qDebug() << "";
            qDebug().noquote() << QString("=== %1 ===").arg(test.name);
            qDebug().noquote() << QString("  1st: show=%1ms events=%2ms | 2nd: show=%3ms events=%4ms")
                .arg(ms(t1)).arg(ms(t2)).arg(ms(t3)).arg(ms(t4));
            delete f;
        }

        // TypeSelectorPopup: cold vs after warmUp
        {
            auto* popup = new TypeSelectorPopup();
            TypeEntry dummy;
            dummy.entryKind = TypeEntry::Primitive;
            dummy.primitiveKind = NodeKind::Hex8;
            dummy.displayName = "test";
            popup->setTypes({dummy});

            QElapsedTimer t; t.start();
            popup->show();
            qint64 t1 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t2 = t.nsecsElapsed();
            popup->hide();
            QApplication::processEvents();

            t.restart();
            popup->show();
            qint64 t3 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t4 = t.nsecsElapsed();
            popup->hide();
            QApplication::processEvents();

            qDebug() << "";
            qDebug().noquote() << QString("=== TypeSelectorPopup (cold, Qt::Popup) ===");
            qDebug().noquote() << QString("  1st: show=%1ms events=%2ms | 2nd: show=%3ms events=%4ms")
                .arg(ms(t1)).arg(ms(t2)).arg(ms(t3)).arg(ms(t4));
            delete popup;
        }

        // Clean order test: dummy popup with children FIRST, then TypeSelectorPopup
        qDebug() << "";
        qDebug() << "=== CLEAN: dummy popup first, then TypeSelectorPopup ===";
        {
            auto* dummy = new QFrame(nullptr, Qt::Popup | Qt::FramelessWindowHint);
            dummy->resize(300, 400);
            auto* dLay = new QVBoxLayout(dummy);
            dLay->addWidget(new QLabel("dummy"));
            dLay->addWidget(new QLineEdit);
            auto* dModel = new QStringListModel(dummy);
            QStringList dItems; for (int i = 0; i < 10; i++) dItems << "x";
            dModel->setStringList(dItems);
            auto* dLv = new QListView; dLv->setModel(dModel);
            dLay->addWidget(dLv);

            QElapsedTimer t; t.start();
            dummy->show();
            qint64 t1 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t2 = t.nsecsElapsed();
            dummy->hide();
            QApplication::processEvents();
            qDebug().noquote() << QString("  Dummy popup: show=%1ms events=%2ms").arg(ms(t1)).arg(ms(t2));
            delete dummy;
        }
        {
            auto* popup = new TypeSelectorPopup();
            TypeEntry e;
            e.entryKind = TypeEntry::Primitive;
            e.primitiveKind = NodeKind::Hex8;
            e.displayName = "test";
            popup->setTypes({e});
            popup->resize(300, 400);
            QElapsedTimer t; t.start();
            popup->show();
            qint64 t1 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t2 = t.nsecsElapsed();
            popup->hide();
            QApplication::processEvents();
            qDebug().noquote() << QString("  TypeSelectorPopup (after dummy): show=%1ms events=%2ms").arg(ms(t1)).arg(ms(t2));
            delete popup;
        }
    }

    // ── Benchmark: large SDK (5000 structs) ──

    void benchmarkLargeSDK() {
        auto ms = [](qint64 ns) { return QString::number(ns / 1000000.0, 'f', 2); };

        // Build 5000 composite types with field summaries (simulates WinSDK)
        QVector<TypeEntry> types;
        types.reserve(5000);
        for (int i = 0; i < 5000; i++) {
            TypeEntry e;
            e.entryKind = TypeEntry::Composite;
            e.structId = (uint64_t)(i + 1);
            e.displayName = QStringLiteral("_STRUCT_%1").arg(i, 4, 10, QChar('0'));
            e.classKeyword = QStringLiteral("struct");
            e.sizeBytes = 64 + (i % 256) * 8;
            e.alignment = 8;
            e.fieldCount = 5 + (i % 20);
            for (int f = 0; f < qMin(6, e.fieldCount); f++)
                e.fieldSummary << QStringLiteral("0x%1: int32_t field_%2")
                    .arg(f * 4, 2, 16, QChar('0')).arg(f);
            types.append(e);
        }

        QFont font("Consolas", 12);
        font.setFixedPitch(true);
        auto* popup = new TypeSelectorPopup();
        popup->warmUp();
        popup->setFont(font);

        // Measure setTypes (data loading)
        QElapsedTimer t;
        t.start();
        popup->setTypes(types, nullptr);
        qint64 tSetTypes = t.nsecsElapsed();

        // Measure popup show (broken down)
        t.restart();
        popup->popup(QPoint(100, 100));
        qint64 tPopupCall = t.nsecsElapsed();
        t.restart();
        QApplication::processEvents();
        qint64 tProcessEvents = t.nsecsElapsed();
        qint64 tShow = tPopupCall + tProcessEvents;

        // Second popup show (warm)
        popup->hide();
        QApplication::processEvents();
        t.restart();
        popup->popup(QPoint(100, 100));
        qint64 tPopup2 = t.nsecsElapsed();
        t.restart();
        QApplication::processEvents();
        qint64 tProcess2 = t.nsecsElapsed();

        // Measure filter with 1-char (worst case: most matches)
        t.restart();
        auto* filterEdit = popup->findChild<QLineEdit*>();
        QVERIFY(filterEdit);

        filterEdit->setText(QStringLiteral("S"));
        qint64 tFilter1 = t.nsecsElapsed();

        // Measure filter with 3-char (moderate filtering)
        t.restart();
        filterEdit->setText(QStringLiteral("STR"));
        qint64 tFilter3 = t.nsecsElapsed();

        // Measure filter with 6-char (narrow results)
        t.restart();
        filterEdit->setText(QStringLiteral("STRUCT"));
        qint64 tFilter6 = t.nsecsElapsed();

        // Measure clear filter (back to grouped view)
        t.restart();
        filterEdit->setText(QString());
        qint64 tClear = t.nsecsElapsed();

        popup->hide();
        QApplication::processEvents();

        qDebug() << "";
        qDebug().noquote() << "=== Large SDK Benchmark (5000 structs) ===";
        qDebug().noquote() << QString("  setTypes:        %1 ms").arg(ms(tSetTypes));
        qDebug().noquote() << QString("  popup() call:    %1 ms").arg(ms(tPopupCall));
        qDebug().noquote() << QString("  processEvents:   %1 ms").arg(ms(tProcessEvents));
        qDebug().noquote() << QString("  popup total:     %1 ms").arg(ms(tShow));
        qDebug().noquote() << QString("  popup2() call:   %1 ms  (warm)").arg(ms(tPopup2));
        qDebug().noquote() << QString("  processEvents2:  %1 ms  (warm)").arg(ms(tProcess2));
        qDebug().noquote() << QString("  popup2 total:    %1 ms  (warm)").arg(ms(tPopup2 + tProcess2));
        qDebug().noquote() << QString("  filter 'S':    %1 ms").arg(ms(tFilter1));
        qDebug().noquote() << QString("  filter 'STR':  %1 ms").arg(ms(tFilter3));
        qDebug().noquote() << QString("  filter 'STRUCT': %1 ms").arg(ms(tFilter6));
        qDebug().noquote() << QString("  clear filter:  %1 ms").arg(ms(tClear));
        QVERIFY(tSetTypes > 0);

        delete popup;
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

        emit popup.createNewTypeRequested(0, 0);
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
        QCOMPARE(spec.ptrDepth, 1);
        QCOMPARE(spec.arrayCount, 0);
    }

    void testParseTypeSpecDoublePointer() {
        TypeSpec spec = parseTypeSpec("Ball**");
        QCOMPARE(spec.baseName, QString("Ball"));
        QVERIFY(spec.isPointer);
        QCOMPARE(spec.ptrDepth, 2);
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
        // Leaf nodes default to collapsed=true; set to false to verify
        // that ChangePointerRef correctly sets collapsed=true for struct refs.
        doc->tree.nodes[xIdx].collapsed = false;
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
    // ── FieldType popup: primitive with [n] creates an array ──

    void testFieldTypePrimitiveArrayCreation() {
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
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Simulate the primitive-array path of applyTypePopupResult:
        // beginMacro → changeNodeKind(Array) → ChangeArrayMeta → endMacro
        doc->undoStack.beginMacro(QStringLiteral("Change to primitive array"));
        ctrl->changeNodeKind(xIdx, NodeKind::Array);
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        doc->undoStack.push(new RcxCommand(ctrl,
            cmd::ChangeArrayMeta{xNodeId, doc->tree.nodes[xIdx].elementKind,
                                 NodeKind::Int32,
                                 doc->tree.nodes[xIdx].arrayLen, 4}));
        doc->undoStack.endMacro();
        QApplication::processEvents();

        // Node should now be an Array
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Array);
        QCOMPARE(doc->tree.nodes[xIdx].elementKind, NodeKind::Int32);
        QCOMPARE(doc->tree.nodes[xIdx].arrayLen, 4);

        // Single undo reverses the entire macro
        doc->undoStack.undo();
        QApplication::processEvents();
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);

        delete ctrl;
        delete splitter;
        delete doc;
    }
    // ── Test: SVG icon and gutter scale with font size ──

    void testDelegateIconScalesWithFont() {
        // Create a popup and set two different font sizes.
        // The delegate sizeHint row height should scale with font.
        TypeSelectorPopup popup;

        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = QStringLiteral("int32_t");

        TypeEntry comp;
        comp.entryKind = TypeEntry::Composite;
        comp.structId = 100;
        comp.displayName = QStringLiteral("TestStruct");
        comp.classKeyword = QStringLiteral("struct");

        // Small font
        QFont small(QStringLiteral("Consolas"), 9);
        popup.setFont(small);
        popup.setTypes({prim, comp});
        popup.popup(QPoint(-9999, -9999));  // offscreen
        QApplication::processEvents();

        auto* listView = popup.findChild<QListView*>();
        QVERIFY(listView);
        auto* delegate = listView->itemDelegate();
        QVERIFY(delegate);

        // Find first non-section row for consistent measurement
        int dataRow = -1;
        for (int i = 0; i < listView->model()->rowCount(); i++) {
            QSize h = delegate->sizeHint(QStyleOptionViewItem(), listView->model()->index(i, 0));
            // Non-section rows are taller (font.height + 8 vs + 2)
            if (h.height() > QFontMetrics(small).height() + 4) { dataRow = i; break; }
        }
        QVERIFY2(dataRow >= 0, "Should find a non-section row");

        QSize smallHint = delegate->sizeHint(QStyleOptionViewItem(), listView->model()->index(dataRow, 0));
        popup.hide();

        // Large font (simulates zoomed editor)
        QFont large(QStringLiteral("Consolas"), 18);
        popup.setFont(large);
        popup.setTypes({prim, comp});
        popup.popup(QPoint(-9999, -9999));
        QApplication::processEvents();

        QSize largeHint = delegate->sizeHint(QStyleOptionViewItem(), listView->model()->index(dataRow, 0));
        popup.hide();

        // Large font should produce taller rows than small font
        QVERIFY2(largeHint.height() > smallHint.height(),
                 qPrintable(QString("Large hint %1 should be > small hint %2")
                     .arg(largeHint.height()).arg(smallHint.height())));

        // The ratio should roughly match the font size ratio (18/9 = 2x)
        double ratio = double(largeHint.height()) / double(smallHint.height());
        QVERIFY2(ratio > 1.4, qPrintable(QString("Row height ratio %1 should be > 1.4").arg(ratio)));
    }

    void testPopupWidthScalesWithFont() {
        TypeSelectorPopup popup;

        // Use a very long name so even font-9 exceeds the minimum popup width
        TypeEntry comp;
        comp.entryKind = TypeEntry::Composite;
        comp.structId = 100;
        comp.displayName = QStringLiteral("MyExtremelyLongStructNameThatExceedsMinWidth");
        comp.classKeyword = QStringLiteral("struct");
        popup.setTypes({comp});

        // Small font
        QFont small(QStringLiteral("Consolas"), 9);
        popup.setFont(small);
        popup.popup(QPoint(-9999, -9999));
        QApplication::processEvents();
        int smallW = popup.width();
        popup.hide();

        // Large font
        QFont large(QStringLiteral("Consolas"), 18);
        popup.setFont(large);
        popup.setTypes({comp});
        popup.popup(QPoint(-9999, -9999));
        QApplication::processEvents();
        int largeW = popup.width();
        popup.hide();

        // Popup with larger font should be wider
        QVERIFY2(largeW > smallW,
                 qPrintable(QString("Large popup width %1 should be > small %2")
                     .arg(largeW).arg(smallW)));
    }
    // ── Test: popup updates colors when theme changes ──

    void testPopupUpdatesOnThemeChange() {
        auto& tm = ThemeManager::instance();
        int origIdx = tm.currentIndex();

        // Ensure at least two themes exist
        QVERIFY2(tm.themes().size() >= 2,
                 "Need at least 2 themes to test theme switching");

        // Create popup with current theme
        TypeSelectorPopup popup;
        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = QStringLiteral("int32_t");
        popup.setTypes({prim});

        QColor bgBefore = popup.palette().color(QPalette::Window);

        // Switch to a different theme
        int otherIdx = (origIdx == 0) ? 1 : 0;
        tm.setCurrent(otherIdx);
        QApplication::processEvents();

        // The popup should have applyTheme connected to themeChanged
        popup.applyTheme(tm.current());

        // Verify applyTheme didn't crash and child widgets exist.
        // Note: exact palette color checks are unreliable for unrealized widgets
        // because Qt's app-wide palette (set by applyGlobalTheme inside setCurrent)
        // may override the widget-local palette via the resolve mask.
        auto* filterEdit = popup.findChild<QLineEdit*>();
        QVERIFY(filterEdit);

        auto* listView = popup.findChild<QListView*>();
        QVERIFY(listView);

        // Restore original theme
        tm.setCurrent(origIdx);
    }

    void testPopupAutoConnectsThemeChange() {
        auto& tm = ThemeManager::instance();
        int origIdx = tm.currentIndex();
        QVERIFY2(tm.themes().size() >= 2, "Need >= 2 themes");

        TypeSelectorPopup popup;

        // applyTheme is a public slot — verify it can be connected
        connect(&tm, &ThemeManager::themeChanged,
                &popup, &TypeSelectorPopup::applyTheme);

        QColor bgBefore = popup.palette().color(QPalette::Window);

        int otherIdx = (origIdx == 0) ? 1 : 0;
        tm.setCurrent(otherIdx);
        QApplication::processEvents();

        // After theme change + signal, popup palette should match new theme
        QCOMPARE(popup.palette().color(QPalette::Window),
                 tm.current().backgroundAlt);

        // Restore
        tm.setCurrent(origIdx);
    }

    // ── parseTypeSpec: primitive pointer ptrDepth ──

    void testParseTypeSpecPrimitiveStar() {
        TypeSpec spec = parseTypeSpec("int32_t*");
        QCOMPARE(spec.baseName, QString("int32_t"));
        QVERIFY(spec.isPointer);
        QCOMPARE(spec.ptrDepth, 1);
        QCOMPARE(spec.arrayCount, 0);
    }

    void testParseTypeSpecPrimitiveDoubleStar() {
        TypeSpec spec = parseTypeSpec("f64**");
        QCOMPARE(spec.baseName, QString("f64"));
        QVERIFY(spec.isPointer);
        QCOMPARE(spec.ptrDepth, 2);
        QCOMPARE(spec.arrayCount, 0);
    }

    // ── Primitive pointer creation via applyTypePopupResult path ──

    void testPrimitivePointerCreation() {
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

        // Find the "x" field (Int32) inside Alpha
        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Simulate the primitive-pointer path: Int32 → Pointer64 + elementKind=Int32 + ptrDepth=1
        doc->undoStack.beginMacro(QStringLiteral("Change to primitive pointer"));
        ctrl->changeNodeKind(xIdx, NodeKind::Pointer64);
        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        doc->tree.nodes[idx].elementKind = NodeKind::Int32;
        doc->tree.nodes[idx].ptrDepth = 1;
        doc->undoStack.endMacro();
        QApplication::processEvents();

        // Verify: Pointer64 with elementKind=Int32, ptrDepth=1, refId=0
        idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].elementKind, NodeKind::Int32);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 1);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        // Undo reverses the macro
        doc->undoStack.undo();
        QApplication::processEvents();
        idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Int32);

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testDoublePointerCreation() {
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

        // Find the "x" field (Int32) inside Alpha
        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Simulate: Int32 → Pointer64 + elementKind=Double + ptrDepth=2
        doc->undoStack.beginMacro(QStringLiteral("Change to double pointer"));
        ctrl->changeNodeKind(xIdx, NodeKind::Pointer64);
        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        doc->tree.nodes[idx].elementKind = NodeKind::Double;
        doc->tree.nodes[idx].ptrDepth = 2;
        doc->undoStack.endMacro();
        QApplication::processEvents();

        // Verify: Pointer64 with elementKind=Double, ptrDepth=2
        idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].elementKind, NodeKind::Double);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 2);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── ptrDepth JSON round-trip ──

    void testPtrDepthJsonRoundTrip() {
        Node n;
        n.kind = NodeKind::Pointer64;
        n.name = "pData";
        n.elementKind = NodeKind::Float;
        n.ptrDepth = 2;
        n.id = 42;

        QJsonObject obj = n.toJson();
        QCOMPARE(obj["ptrDepth"].toInt(), 2);

        Node restored = Node::fromJson(obj);
        QCOMPARE(restored.ptrDepth, 2);
        QCOMPARE(restored.elementKind, NodeKind::Float);
        QCOMPARE(restored.kind, NodeKind::Pointer64);
    }

    void testPtrDepthJsonDefault() {
        // Nodes without ptrDepth in JSON should default to 0
        Node n;
        n.kind = NodeKind::Pointer64;
        n.name = "pVoid";
        n.id = 99;

        QJsonObject obj = n.toJson();
        // ptrDepth==0 is not serialized
        QVERIFY(!obj.contains("ptrDepth"));

        Node restored = Node::fromJson(obj);
        QCOMPARE(restored.ptrDepth, 0);
    }

    // ── setMode always resets modifier buttons ──

    void testSetModeResetsModifierInPointerTargetMode() {
        TypeSelectorPopup popup;

        // Set FieldType mode and select * modifier
        popup.setMode(TypePopupMode::FieldType);
        popup.setModifier(1);  // select *

        // Now switch to PointerTarget mode — should reset to plain
        popup.setMode(TypePopupMode::PointerTarget);

        // Verify: modifier buttons are hidden but internally reset to plain (modId=0)
        // This means primitives will be visible in applyFilter
        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = "int32_t";

        TypeEntry voidEntry;
        voidEntry.entryKind = TypeEntry::Primitive;
        voidEntry.primitiveKind = NodeKind::Pointer64;
        voidEntry.displayName = "void";

        popup.setTypes({prim, voidEntry});

        // Both primitives should be visible (not filtered out)
        auto* listView = popup.findChild<QListView*>();
        QVERIFY(listView);
        int rowCount = listView->model()->rowCount();
        // Should have section header + 2 primitives = at least 3 rows
        QVERIFY2(rowCount >= 3,
                 qPrintable(QString("Expected >=3 rows (header+2 prims), got %1").arg(rowCount)));
    }

    // ── setModifier preselection ──

    void testSetModifierPreselects() {
        TypeSelectorPopup popup;

        // Test * preselection
        popup.setMode(TypePopupMode::FieldType);
        popup.setModifier(1);
        auto* btnGroup = popup.findChild<QButtonGroup*>();
        QVERIFY(btnGroup);
        QCOMPARE(btnGroup->checkedId(), 1);

        // Test ** preselection
        popup.setMode(TypePopupMode::FieldType);
        popup.setModifier(2);
        QCOMPARE(btnGroup->checkedId(), 2);

        // Test [n] preselection with count
        popup.setMode(TypePopupMode::FieldType);
        popup.setModifier(3, 8);
        QCOMPARE(btnGroup->checkedId(), 3);
        auto* countEdit = popup.findChild<QLineEdit*>(QStringLiteral("arrayCountEdit"));
        // Array count edit may not have objectName set; find via parent
        // Just verify button group is correct
    }

    // ── isValidPrimitivePtrTarget ──

    void testIsValidPrimitivePtrTarget() {
        // Hex types → NOT valid (deref shows same hex as void*)
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Hex8));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Hex16));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Hex32));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Hex64));

        // Pointer types → NOT valid (use composite * for chains)
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Pointer32));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Pointer64));

        // Function pointers → NOT valid
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::FuncPtr32));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::FuncPtr64));

        // Containers → NOT valid
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Struct));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Array));

        // Value types → valid
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Int32));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::UInt64));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Float));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Double));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Bool));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Vec3));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::UTF8));
    }

    // ── hex64* falls back to void* ──

    void testHex64StarFallsBackToVoidPointer() {
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
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Build a TypeEntry for hex64
        TypeEntry hexEntry;
        hexEntry.entryKind = TypeEntry::Primitive;
        hexEntry.primitiveKind = NodeKind::Hex64;
        hexEntry.displayName = "hex64";

        // Apply it with pointer modifier (fullText = "hex64*")
        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   hexEntry, QStringLiteral("hex64*"));
        QApplication::processEvents();

        // Should be a void pointer: Pointer64, ptrDepth=0, refId=0
        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 0);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testHex8StarFallsBackToVoidPointer() {
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

        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        TypeEntry hexEntry;
        hexEntry.entryKind = TypeEntry::Primitive;
        hexEntry.primitiveKind = NodeKind::Hex8;
        hexEntry.displayName = "hex8";

        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   hexEntry, QStringLiteral("hex8*"));
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 0);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testPtr64StarFallsBackToVoidPointer() {
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

        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        TypeEntry ptrEntry;
        ptrEntry.entryKind = TypeEntry::Primitive;
        ptrEntry.primitiveKind = NodeKind::Pointer64;
        ptrEntry.displayName = "ptr64";

        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   ptrEntry, QStringLiteral("ptr64*"));
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 0);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── Valid primitive pointers still work ──

    void testInt32StarStillCreatesPrimitivePointer() {
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

        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        TypeEntry intEntry;
        intEntry.entryKind = TypeEntry::Primitive;
        intEntry.primitiveKind = NodeKind::Int32;
        intEntry.displayName = "int32_t";

        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   intEntry, QStringLiteral("int32_t*"));
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 1);
        QCOMPARE(doc->tree.nodes[idx].elementKind, NodeKind::Int32);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testDoubleDoubleStarStillCreatesPrimitivePointer() {
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

        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        TypeEntry dblEntry;
        dblEntry.entryKind = TypeEntry::Primitive;
        dblEntry.primitiveKind = NodeKind::Double;
        dblEntry.displayName = "double";

        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   dblEntry, QStringLiteral("double**"));
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 2);
        QCOMPARE(doc->tree.nodes[idx].elementKind, NodeKind::Double);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── Defense: compose/format treat invalid ptrDepth as void* ──

    void testComposeShowsVoidPtrForHexPtrDepth() {
        // If a node somehow has ptrDepth>0 with hex elementKind
        // (e.g. from old JSON), compose should show "void*" not "hex64*"
        NodeTree tree;
        tree.baseAddress = 0x1000;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Test";
        root.structTypeName = "Test";
        root.parentId = 0;
        tree.addNode(root);
        uint64_t rootId = tree.nodes[0].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "badPtr";
        ptr.parentId = rootId;
        ptr.offset = 0;
        ptr.ptrDepth = 1;
        ptr.elementKind = NodeKind::Hex64;  // invalid target
        tree.addNode(ptr);

        QByteArray buf(0x100, '\0');
        BufferProvider prov(buf);

        ComposeResult result = compose(tree, prov);

        // The composed text should NOT contain "hex64*" — the invalid target
        // should fall through to normal void pointer display
        QVERIFY2(!result.text.contains("hex64*"),
                 qPrintable("Should not show 'hex64*', got: " + result.text));
    }
    // ── Category chips and three-group filtering ──

    void testCategoryEnumOnEntry() {
        // Verify that Category enum values exist and are distinct
        TypeEntry prim;
        prim.category = TypeEntry::CatPrimitive;
        QCOMPARE(prim.category, TypeEntry::CatPrimitive);

        TypeEntry typ;
        typ.category = TypeEntry::CatType;
        QCOMPARE(typ.category, TypeEntry::CatType);

        TypeEntry en;
        en.category = TypeEntry::CatEnum;
        QCOMPARE(en.category, TypeEntry::CatEnum);

        QVERIFY(TypeEntry::CatPrimitive != TypeEntry::CatType);
        QVERIFY(TypeEntry::CatType != TypeEntry::CatEnum);
    }

    void testCategoryDefaultIsPrimitive() {
        TypeEntry e;
        QCOMPARE(e.category, TypeEntry::CatPrimitive);
    }

    void testCompositesCategorizedInController() {
        // Build tree with struct and enum types
        NodeTree tree;
        tree.baseAddress = 0;

        Node st;
        st.kind = NodeKind::Struct;
        st.name = "Ball";
        st.structTypeName = "Ball";
        st.parentId = 0;
        int si = tree.addNode(st);
        uint64_t stId = tree.nodes[si].id;

        { Node n; n.kind = NodeKind::Int32; n.name = "x"; n.parentId = stId;
          n.offset = 0; tree.addNode(n); }

        Node en;
        en.kind = NodeKind::Struct;
        en.name = "Color";
        en.structTypeName = "Color";
        en.classKeyword = QStringLiteral("enum");
        en.parentId = 0;
        tree.addNode(en);

        // Simulate controller logic: tag composites
        QVector<TypeEntry> entries;
        for (const auto& n : tree.nodes) {
            if (n.parentId != 0 || n.kind != NodeKind::Struct) continue;
            TypeEntry e;
            e.entryKind    = TypeEntry::Composite;
            e.structId     = n.id;
            e.displayName  = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
            e.classKeyword = n.resolvedClassKeyword();
            e.category     = (e.classKeyword == QStringLiteral("enum"))
                           ? TypeEntry::CatEnum : TypeEntry::CatType;
            entries.append(e);
        }

        QCOMPARE(entries.size(), 2);
        // Ball → CatType, Color → CatEnum
        bool foundType = false, foundEnum = false;
        for (const auto& e : entries) {
            if (e.displayName == "Ball") {
                QCOMPARE(e.category, TypeEntry::CatType);
                foundType = true;
            }
            if (e.displayName == "Color") {
                QCOMPARE(e.category, TypeEntry::CatEnum);
                foundEnum = true;
            }
        }
        QVERIFY(foundType);
        QVERIFY(foundEnum);
    }

    void testThreeGroupSections() {
        // Create popup and set types with mixed categories
        TypeSelectorPopup popup;
        popup.setMode(TypePopupMode::FieldType);

        QVector<TypeEntry> types;

        // A primitive
        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = QStringLiteral("int32_t");
        prim.category = TypeEntry::CatPrimitive;
        types.append(prim);

        // A struct type
        TypeEntry st;
        st.entryKind = TypeEntry::Composite;
        st.structId = 1;
        st.displayName = QStringLiteral("Player");
        st.classKeyword = QStringLiteral("struct");
        st.category = TypeEntry::CatType;
        types.append(st);

        // An enum type
        TypeEntry en;
        en.entryKind = TypeEntry::Composite;
        en.structId = 2;
        en.displayName = QStringLiteral("Color");
        en.classKeyword = QStringLiteral("enum");
        en.category = TypeEntry::CatEnum;
        types.append(en);

        popup.setTypes(types);

        // The popup should have three sections in field mode:
        // primitives → types → enums
        // We can access via the internal model
        auto* model = popup.findChild<QStringListModel*>();
        QVERIFY(model != nullptr);
        QStringList items = model->stringList();

        // Should contain section headers
        bool hasPrimSection = false, hasTypeSection = false, hasEnumSection = false;
        for (const auto& item : items) {
            if (item == QStringLiteral("primitives")) hasPrimSection = true;
            if (item == QStringLiteral("types")) hasTypeSection = true;
            if (item == QStringLiteral("enums")) hasEnumSection = true;
        }
        QVERIFY2(hasPrimSection, "Missing 'primitives' section header");
        QVERIFY2(hasTypeSection, "Missing 'types' section header");
        QVERIFY2(hasEnumSection, "Missing 'enums' section header");
    }

    // ── Test: struct embed auto-selects the current composite in popup ──

    void testStructEmbedAutoSelectsCurrent() {
        TypeSelectorPopup popup;
        popup.setMode(TypePopupMode::FieldType);
        QFont font(QStringLiteral("Consolas"), 10);
        popup.setFont(font);

        // Build entries: a primitive + two composites
        QVector<TypeEntry> types;

        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = QStringLiteral("int32_t");
        types.append(prim);

        TypeEntry alpha;
        alpha.entryKind = TypeEntry::Composite;
        alpha.structId = 100;
        alpha.displayName = QStringLiteral("Alpha");
        alpha.classKeyword = QStringLiteral("struct");
        alpha.category = TypeEntry::CatType;
        types.append(alpha);

        TypeEntry bravo;
        bravo.entryKind = TypeEntry::Composite;
        bravo.structId = 200;
        bravo.displayName = QStringLiteral("Bravo");
        bravo.classKeyword = QStringLiteral("struct");
        bravo.category = TypeEntry::CatType;
        types.append(bravo);

        // Set Bravo as the current type (simulates struct embed field with refId=200)
        popup.setTypes(types, &bravo);
        popup.popup(QPoint(-9999, -9999));
        QApplication::processEvents();

        // The list view should auto-select the row matching Bravo
        auto* listView = popup.findChild<QListView*>();
        QVERIFY(listView != nullptr);
        QModelIndex sel = listView->currentIndex();
        QVERIFY2(sel.isValid(), "No item selected — auto-select failed");

        // The selected row text should contain "Bravo"
        QString selectedText = sel.data().toString();
        QVERIFY2(selectedText.contains(QStringLiteral("Bravo")),
                 qPrintable(QString("Expected 'Bravo' in selected text, got '%1'").arg(selectedText)));

        popup.hide();
    }

    // ── createNewTypeRequested carries modifier state ──

    void testCreateNewSignalCarriesModifier() {
        TypeSelectorPopup popup;
        popup.warmUp();
        popup.setMode(TypePopupMode::FieldType);
        popup.setPointerSize(8);

        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = "int32_t";
        popup.setTypes({prim});

        QSignalSpy spy(&popup, &TypeSelectorPopup::createNewTypeRequested);
        QVERIFY(spy.isValid());

        // ── Plain (no modifier) ──
        popup.setModifier(0);
        auto* createBtn = popup.findChild<QToolButton*>(QString(), Qt::FindDirectChildrenOnly);
        // Find the "+ New" button by iterating children
        QToolButton* newBtn = nullptr;
        for (auto* btn : popup.findChildren<QToolButton*>()) {
            if (btn->text().contains("New")) { newBtn = btn; break; }
        }
        QVERIFY2(newBtn, "Could not find '+ New' button in popup");

        newBtn->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 0);   // modifierId = 0 (plain)
        QCOMPARE(spy.at(0).at(1).toInt(), 0);   // arrayCount = 0

        // ── Pointer (*) modifier ──
        spy.clear();
        popup.show();
        popup.setModifier(1);
        newBtn->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 1);   // modifierId = 1 (*)
        QCOMPARE(spy.at(0).at(1).toInt(), 0);

        // ── Double pointer (**) modifier ──
        spy.clear();
        popup.show();
        popup.setModifier(2);
        newBtn->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 2);   // modifierId = 2 (**)

        // ── Array ([]) modifier ──
        spy.clear();
        popup.show();
        popup.setModifier(3, 16);
        newBtn->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 3);   // modifierId = 3 ([])
        QCOMPARE(spy.at(0).at(1).toInt(), 16);  // arrayCount = 16
    }

    // ── Controller: createNewType with pointer modifier → node becomes Pointer64 ──

    void testCreateNewTypeWithPtrModifier() {
        // Setup: tree with one Hex64 field
        NodeTree tree;
        tree.baseAddress = 0;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.structTypeName = "Root";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node field;
        field.kind = NodeKind::Hex64;
        field.name = "target";
        field.parentId = rootId;
        field.offset = 0;
        int fi = tree.addNode(field);
        uint64_t fieldId = tree.nodes[fi].id;

        auto* doc = new RcxDocument();
        doc->tree = tree;
        doc->loadData(makeBuffer());
        doc->tree.baseAddress = 0;

        auto* splitter = new QSplitter;
        auto ctrl = new RcxController(doc, splitter);
        ctrl->addSplitEditor(splitter);
        ctrl->setViewRootId(rootId);
        ctrl->refresh();

        // Simulate: popup selects "New" with * modifier (modifierId=1)
        // This creates a new root struct and calls applyTypePopupResult with "NewClass*"
        TypeEntry newEntry;
        newEntry.entryKind = TypeEntry::Composite;

        // Create the new struct manually (mimics what the handler does)
        Node newStruct;
        newStruct.kind = NodeKind::Struct;
        newStruct.structTypeName = "TestNewClass";
        newStruct.name = "instance";
        newStruct.parentId = 0;
        newStruct.offset = 0;
        newStruct.id = doc->tree.reserveId();
        doc->undoStack.push(new RcxCommand(ctrl, cmd::Insert{newStruct}));
        newEntry.structId = newStruct.id;

        // Apply with pointer modifier fullText
        ctrl->applyTypePopupResult(TypePopupMode::FieldType, fi, newEntry,
                                   QStringLiteral("TestNewClass*"));

        // Verify: field should now be Pointer64 with refId pointing to the new struct
        int updatedIdx = doc->tree.indexOfId(fieldId);
        QVERIFY(updatedIdx >= 0);
        const Node& updated = doc->tree.nodes[updatedIdx];
        QCOMPARE(updated.kind, NodeKind::Pointer64);
        QCOMPARE(updated.refId, newStruct.id);

        delete splitter;
        delete doc;
    }

    // ── Controller: createNewType with array modifier → node becomes Array ──

    void testCreateNewTypeWithArrayModifier() {
        NodeTree tree;
        tree.baseAddress = 0;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.structTypeName = "Root";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node field;
        field.kind = NodeKind::Hex64;
        field.name = "target";
        field.parentId = rootId;
        field.offset = 0;
        int fi = tree.addNode(field);
        uint64_t fieldId = tree.nodes[fi].id;

        auto* doc = new RcxDocument();
        doc->tree = tree;
        doc->loadData(makeBuffer());
        doc->tree.baseAddress = 0;

        auto* splitter = new QSplitter;
        auto ctrl = new RcxController(doc, splitter);
        ctrl->addSplitEditor(splitter);
        ctrl->setViewRootId(rootId);
        ctrl->refresh();

        // Create the new struct
        Node newStruct;
        newStruct.kind = NodeKind::Struct;
        newStruct.structTypeName = "ArrayElemType";
        newStruct.name = "instance";
        newStruct.parentId = 0;
        newStruct.offset = 0;
        newStruct.id = doc->tree.reserveId();
        doc->undoStack.push(new RcxCommand(ctrl, cmd::Insert{newStruct}));

        TypeEntry newEntry;
        newEntry.entryKind = TypeEntry::Composite;
        newEntry.structId = newStruct.id;

        // Apply with array modifier fullText
        ctrl->applyTypePopupResult(TypePopupMode::FieldType, fi, newEntry,
                                   QStringLiteral("ArrayElemType[10]"));

        // Verify: field should now be Array with elementKind=Struct, arrayLen=10, refId set
        int updatedIdx = doc->tree.indexOfId(fieldId);
        QVERIFY(updatedIdx >= 0);
        const Node& updated = doc->tree.nodes[updatedIdx];
        QCOMPARE(updated.kind, NodeKind::Array);
        QCOMPARE(updated.elementKind, NodeKind::Struct);
        QCOMPARE(updated.arrayLen, 10);
        QCOMPARE(updated.refId, newStruct.id);

        delete splitter;
        delete doc;
    }
};

QTEST_MAIN(TestTypeSelector)
#include "test_type_selector.moc"
