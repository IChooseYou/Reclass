// Unit tests for RcxController::attachRttiClassToPointer.
//
// This is the controller-side handler for the new RTTI overlay chip
// click flow: user clicks the "{class}" chip on a pointer field →
// MainWindow forwards to controller → controller creates a fresh
// root struct named after the demangled type and wires the clicked
// node's refId to it. Always creates new (with _N suffix on name
// collision) per user spec, so multiple clicks across fields each
// get their own editable class definition.

#include <QtTest/QTest>
#include <QByteArray>
#include "core.h"
#include "controller.h"
#include "providers/buffer_provider.h"

using namespace rcx;

namespace {

// Build a doc with a single Hex64 field at offset 0 of a root struct,
// ready for attachRttiClassToPointer to chew on.
struct Fixture {
    RcxDocument*   doc;
    RcxController* ctrl;
    uint64_t       rootId;
    uint64_t       hexId;
};

Fixture buildFixture(QObject* parent) {
    Fixture f;
    f.doc = new RcxDocument(parent);
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = QStringLiteral("Container");
    root.name = QStringLiteral("inst");
    int ri = f.doc->tree.addNode(root);
    f.rootId = f.doc->tree.nodes[ri].id;

    Node hex;
    hex.kind = NodeKind::Hex64;
    hex.name = QStringLiteral("vtbl_slot");
    hex.parentId = f.rootId;
    hex.offset = 0;
    int hi = f.doc->tree.addNode(hex);
    f.hexId = f.doc->tree.nodes[hi].id;

    QByteArray data(64, '\0');
    f.doc->provider = std::make_shared<BufferProvider>(
        std::move(data), QStringLiteral("synthetic"));

    f.ctrl = new RcxController(f.doc, /*parent*/ nullptr);
    return f;
}

} // anon

class TestOverlayClassCreate : public QObject {
    Q_OBJECT
private slots:

    // ── Click on RTTI chip creates a fresh class named "Foo" and
    //    points the Hex64 field at it (converting it to Pointer64). ──
    void createsAndWiresClass() {
        Fixture f = buildFixture(this);
        uint64_t structId = f.ctrl->attachRttiClassToPointer(
            f.hexId, QStringLiteral("Foo"));
        QVERIFY2(structId != 0, "should return a valid struct id");

        // Tree now has a root struct named "Foo".
        bool foundFoo = false;
        for (const auto& n : f.doc->tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct
                && n.structTypeName == QStringLiteral("Foo")) {
                foundFoo = true;
                QCOMPARE(n.id, structId);
                break;
            }
        }
        QVERIFY2(foundFoo, "tree should contain root struct named Foo");

        // The original node was converted to Pointer64 with refId == Foo.
        int hi = f.doc->tree.indexOfId(f.hexId);
        QVERIFY(hi >= 0);
        const Node& nptr = f.doc->tree.nodes[hi];
        QCOMPARE(nptr.kind, NodeKind::Pointer64);
        QCOMPARE(nptr.refId, structId);
    }

    // ── Second click with the SAME demangled name creates a SECOND
    //    distinct class "Foo_2", not a reused "Foo". This matches the
    //    user's preference: each RTTI chip click yields an independent
    //    editable class so layouts don't accidentally share state
    //    across unrelated fields. ──
    void secondClickWithSameNameSuffixes() {
        Fixture f = buildFixture(this);

        // Add a second Hex64 field for the second click target.
        Node hex2;
        hex2.kind = NodeKind::Hex64;
        hex2.name = QStringLiteral("vtbl_slot2");
        hex2.parentId = f.rootId;
        hex2.offset = 8;
        int hi2 = f.doc->tree.addNode(hex2);
        uint64_t hexId2 = f.doc->tree.nodes[hi2].id;

        uint64_t id1 = f.ctrl->attachRttiClassToPointer(
            f.hexId, QStringLiteral("Foo"));
        uint64_t id2 = f.ctrl->attachRttiClassToPointer(
            hexId2, QStringLiteral("Foo"));
        QVERIFY(id1 != 0 && id2 != 0);
        QVERIFY2(id1 != id2, "second class must be a separate struct");

        // Names: Foo + Foo_2.
        QSet<QString> names;
        for (const auto& n : f.doc->tree.nodes) {
            if (n.parentId != 0 || n.kind != NodeKind::Struct) continue;
            if (!n.structTypeName.isEmpty())
                names.insert(n.structTypeName);
        }
        QVERIFY2(names.contains(QStringLiteral("Foo")),
                 "first click should have created Foo");
        QVERIFY2(names.contains(QStringLiteral("Foo_2")),
                 "second click with same base should have created Foo_2");
    }

    // ── Empty base name (defensive — should never happen in practice
    //    since RTTI walking only emits chips with non-empty names)
    //    falls back to the legacy "NewClass" naming. ──
    void emptyBaseNameFallsBackToNewClass() {
        Fixture f = buildFixture(this);
        uint64_t structId = f.ctrl->attachRttiClassToPointer(
            f.hexId, QString());
        QVERIFY(structId != 0);
        int idx = f.doc->tree.indexOfId(structId);
        QVERIFY(idx >= 0);
        QCOMPARE(f.doc->tree.nodes[idx].structTypeName,
                 QStringLiteral("NewClass"));
    }

    // ── The new struct comes pre-populated with hex children so the
    //    user has something to start typing into immediately (matches
    //    convertToTypedPointer's UX). ──
    void newStructHasDefaultFields() {
        Fixture f = buildFixture(this);
        uint64_t structId = f.ctrl->attachRttiClassToPointer(
            f.hexId, QStringLiteral("MyClass"));
        QVERIFY(structId != 0);

        // Count children of the new struct.
        int childCount = 0;
        for (const auto& n : f.doc->tree.nodes) {
            if (n.parentId == structId) ++childCount;
        }
        QVERIFY2(childCount > 0,
                 "new class should have default hex children for the user to start with");
    }

    // ── The whole operation is one undo macro — undo restores the
    //    original Hex64 field and removes the new class entirely. ──
    void operationIsAtomicallyUndoable() {
        Fixture f = buildFixture(this);
        QString preName;
        for (const auto& n : f.doc->tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct
                && n.id != f.rootId) preName = n.structTypeName;
        }
        QVERIFY2(preName.isEmpty(), "no extra root struct should exist pre-click");

        f.ctrl->attachRttiClassToPointer(f.hexId, QStringLiteral("Bar"));
        // Confirm the side effects landed.
        int hi = f.doc->tree.indexOfId(f.hexId);
        QCOMPARE(f.doc->tree.nodes[hi].kind, NodeKind::Pointer64);

        // One undo should reverse the entire macro.
        f.doc->undoStack.undo();

        hi = f.doc->tree.indexOfId(f.hexId);
        QVERIFY(hi >= 0);
        QCOMPARE(f.doc->tree.nodes[hi].kind, NodeKind::Hex64);
        QCOMPARE(f.doc->tree.nodes[hi].refId, uint64_t(0));

        // The "Bar" root struct should be gone.
        bool foundBar = false;
        for (const auto& n : f.doc->tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct
                && n.structTypeName == QStringLiteral("Bar"))
                { foundBar = true; break; }
        }
        QVERIFY2(!foundBar, "undo should have removed the new class entirely");
    }
};

QTEST_MAIN(TestOverlayClassCreate)
#include "test_overlay_classcreate.moc"
