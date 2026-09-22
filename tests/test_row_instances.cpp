// One instance of a row (row_instances.h), without a window: a node composed
// in several places is several instances; a value's extra rows belong to its
// first row; an anchor finds its instance again (by index, then address and
// depth); byte coverage marks every covered instance; and for a node shown
// once the rows are exactly the ones the old every-row rule marked.

#include <QtTest/QTest>

#include "core.h"
#include "row_instances.h"
#include "providers/buffer_provider.h"
#include "tree_fixture.h"

#include <algorithm>

using namespace rcx;

namespace {

ComposeResult composeRich(const treefix::Rich& fx, bool brace = false) {
    BufferProvider prov(fx.data);
    return compose(fx.tree, prov, fx.rootId, false, /*treeLines=*/true, brace);
}

// The editor's node id → rows index.
QHash<uint64_t, QVector<int>> lineIndex(const QVector<LineMeta>& meta) {
    QHash<uint64_t, QVector<int>> idx;
    for (int i = 0; i < meta.size(); ++i)
        if (meta[i].nodeId != 0) idx[meta[i].nodeId].append(i);
    return idx;
}

// The old rule: every row whose own selection id is this one.
QVector<int> everyRow(const QVector<LineMeta>& meta, const QVector<int>& nodeLines, uint64_t selId) {
    QVector<int> out;
    for (int ln : nodeLines)
        if (!isSyntheticLine(meta[ln]) && selIdForLine(meta[ln]) == selId) out.append(ln);
    return out;
}

uint64_t idOf(const treefix::Rich& fx, const QString& name) {
    for (const Node& n : fx.tree.nodes)
        if (n.name == name) return n.id;
    return 0;
}

} // namespace

class TestRowInstances : public QObject {
    Q_OBJECT

private slots:
    // (The rich fixture's `self` pointer shows Rich again inside itself, so its
    // own fields are each shown twice.)
    void aMatrixIsOneInstanceOfFourRows() {
        const auto fx = treefix::richTree();
        const auto cr = composeRich(fx);
        const auto idx = lineIndex(cr.meta);
        const uint64_t view = idOf(fx, QStringLiteral("view"));
        const QVector<int> starts = instanceStarts(cr.meta, idx.value(view), view);
        QVERIFY2(starts.size() >= 2, "the fixture no longer shows the matrix twice");
        for (int first : starts) {
            const QVector<int> block{first, first + 1, first + 2, first + 3};
            for (int ln : block) {
                QCOMPARE(instanceStartLine(cr.meta, ln), first);
                QCOMPARE(instanceBlock(cr.meta, ln), block);
            }
            // Clicked on its third row: the whole matrix, here only.
            const SelectionAnchor a = anchorForLine(cr.meta, first + 2);
            QCOMPARE(selectionLines(cr.meta, idx.value(view), view, &a), block);
        }
        const int first = starts[0];
        QCOMPARE(selectionLines(cr.meta, idx.value(view), view),
                 (QVector<int>{first, first + 1, first + 2, first + 3}));
    }

    void aClassShownInSeveralPlacesHasOneInstancePerPlace() {
        const auto fx = treefix::richTree();
        const auto cr = composeRich(fx);
        const auto& meta = cr.meta;
        const auto idx = lineIndex(meta);
        const uint64_t x = idOf(fx, QStringLiteral("x"));   // Vec::x — in points[0..1], pos, target
        const QVector<int>& rows = idx.value(x);
        const QVector<int> starts = instanceStarts(meta, rows, x);
        QVERIFY2(starts.size() >= 3, qPrintable(QStringLiteral("Vec::x shown %1 times").arg(starts.size())));
        QCOMPARE(everyRow(meta, rows, x), starts);   // one row each: the old rule lit them all

        // Nothing to go on: the first place only.
        QCOMPARE(selectionLines(meta, rows, x), QVector<int>{starts[0]});

        // Anchored where it was clicked: that place only.
        for (int k = 0; k < starts.size(); ++k) {
            const SelectionAnchor a = anchorForLine(meta, starts[k]);
            QCOMPARE(a.ordinal, k);
            QCOMPARE(a.addr, meta[starts[k]].offsetAddr);
            QCOMPARE(a.depth, meta[starts[k]].depth);
            QCOMPARE(selectionLines(meta, rows, x, &a), QVector<int>{starts[k]});
        }

        // The address moved (a pointer changed): the index still finds it.
        SelectionAnchor moved = anchorForLine(meta, starts.last());
        moved.addr += 0x1000;
        QCOMPARE(selectionLines(meta, rows, x, &moved), QVector<int>{starts.last()});

        // An instance appeared above (the index shifted): address and depth find it.
        int other = -1;
        for (int k = 1; k < starts.size() && other < 0; ++k)
            if (meta[starts[k]].offsetAddr != meta[starts[0]].offsetAddr
                || meta[starts[k]].depth != meta[starts[0]].depth)
                other = k;
        QVERIFY(other > 0);
        SelectionAnchor shifted = anchorForLine(meta, starts[other]);
        shifted.ordinal = 0;
        QCOMPARE(selectionLines(meta, rows, x, &shifted), QVector<int>{starts[other]});

        // Neither matches: the first.
        SelectionAnchor gone{0xDEAD0000, 99, 99};
        QCOMPARE(selectionLines(meta, rows, x, &gone), QVector<int>{starts[0]});
    }

    void anEnumFieldsMembersAreNotItsInstances() {
        const auto fx = treefix::richTree();
        const auto cr = composeRich(fx);
        const auto& meta = cr.meta;
        const auto idx = lineIndex(meta);
        const uint64_t mode = idOf(fx, QStringLiteral("mode"));
        const QVector<int>& rows = idx.value(mode);
        QVERIFY(rows.size() > 2);   // the field, its members, its `}`
        const QVector<int> starts = instanceStarts(meta, rows, mode);
        QVERIFY(!starts.isEmpty());
        for (int s : starts) {
            QVERIFY(!meta[s].isMemberLine && meta[s].lineKind == LineKind::Field);
            QCOMPARE(instanceBlock(meta, s), QVector<int>{s});   // the field alone, never its members
        }
        int members = 0;
        for (int ln : rows) {
            if (!meta[ln].isMemberLine) continue;
            ++members;
            const uint64_t sel = selIdForLine(meta[ln]);
            const QVector<int> memberStarts = instanceStarts(meta, rows, sel);
            QVERIFY(memberStarts.contains(ln));
            QCOMPARE(memberStarts.size(), starts.size());   // one per place the field is shown
            const SelectionAnchor a = anchorForLine(meta, ln);
            QCOMPARE(selectionLines(meta, rows, sel, &a), QVector<int>{ln});
        }
        QCOMPARE(members, 3 * int(starts.size()));
    }

    void footersAndArrayElementsAreTheirOwnInstances() {
        const auto fx = treefix::richTree();
        for (bool brace : {false, true}) {
            const auto cr = composeRich(fx, brace);
            const auto& meta = cr.meta;
            const auto idx = lineIndex(meta);
            const uint64_t inner = idOf(fx, QStringLiteral("inner"));
            const QVector<int>& rows = idx.value(inner);
            const QVector<int> heads = instanceStarts(meta, rows, inner);
            const QVector<int> feet = instanceStarts(meta, rows, inner | kFooterIdBit);
            QVERIFY(!heads.isEmpty());
            QVERIFY(feet.size() >= heads.size());
            for (int h : heads) {
                QVERIFY(meta[h].lineKind == LineKind::Header);
                const SelectionAnchor a = anchorForLine(meta, h);
                QCOMPARE(selectionLines(meta, rows, inner, &a), QVector<int>{h});   // never its footer
            }
            for (int f : feet) {
                QVERIFY(meta[f].lineKind == LineKind::Footer);
                // A footer clicked is the footer marked, even with a `{` row above.
                const SelectionAnchor a = anchorForLine(meta, f);
                QCOMPARE(selectionLines(meta, rows, inner | kFooterIdBit, &a), QVector<int>{f});
            }

            const uint64_t scores = idOf(fx, QStringLiteral("scores"));
            int elements = 0;
            for (int ln : idx.value(scores)) {
                if (!meta[ln].isArrayElement) continue;
                ++elements;
                const SelectionAnchor a = anchorForLine(meta, ln);
                QCOMPARE(selectionLines(meta, idx.value(scores), selIdForLine(meta[ln]), &a), QVector<int>{ln});
            }
            QVERIFY(elements >= 3 && elements % 3 == 0);
        }
    }

    void aByteSelectionMarksEveryCoveredInstance() {
        const auto fx = treefix::richTree();
        const auto cr = composeRich(fx);
        const auto idx = lineIndex(cr.meta);
        const uint64_t x = idOf(fx, QStringLiteral("x"));
        const QVector<int> starts = instanceStarts(cr.meta, idx.value(x), x);
        QVERIFY(starts.size() >= 3);
        const QVector<int> covered{starts[1], starts[2]};
        QCOMPARE(selectionLines(cr.meta, idx.value(x), x, nullptr, &covered), covered);
        const QVector<int> none;
        QVERIFY(selectionLines(cr.meta, idx.value(x), x, nullptr, &none).isEmpty());
    }

    // The instances split exactly the rows the old every-row rule marked: every
    // one of those rows is in exactly one instance, and nothing else is. So a
    // node shown once marks what it always did.
    void theInstancesSplitTheRowsTheOldRuleMarked() {
        const auto fx = treefix::richTree();
        for (bool brace : {false, true}) {
            const auto cr = composeRich(fx, brace);
            const auto& meta = cr.meta;
            const auto idx = lineIndex(meta);
            int checked = 0, shared = 0;
            QSet<uint64_t> seen;
            for (const LineMeta& lm : meta) {
                if (isSyntheticLine(lm) || lm.nodeId == 0) continue;
                const uint64_t sel = selIdForLine(lm);
                if (seen.contains(sel)) continue;
                seen.insert(sel);
                const QVector<int>& rows = idx.value(lm.nodeId);
                const QVector<int> starts = instanceStarts(meta, rows, sel);
                QVector<int> all;
                for (int s : starts) all += instanceBlock(meta, s);
                std::sort(all.begin(), all.end());
                QVERIFY2(std::adjacent_find(all.begin(), all.end()) == all.end(),
                         qPrintable(QStringLiteral("selection %1: a row in two instances").arg(sel, 0, 16)));
                QCOMPARE(all, everyRow(meta, rows, sel));
                if (starts.size() == 1) QCOMPARE(selectionLines(meta, rows, sel), all);
                else ++shared;
                ++checked;
            }
            QVERIFY(checked > 20);
            QVERIFY2(shared > 5, "the fixture shows too few nodes twice to mean anything");
        }
    }
};

QTEST_GUILESS_MAIN(TestRowInstances)
#include "test_row_instances.moc"
