/*
 * bench_spam_append — simulate holding Down on a class to spam-append fields,
 * with the runtime profiler enabled, then dump aggregated bucket stats so
 * we can verify the meta-diff narrowing works correctly AND see what's
 * left after correctness fixes.
 *
 * The flow per iteration mirrors what happens in production:
 *   1. Insert a new Hex64 child node at the current end of the struct.
 *   2. Re-compose the tree.
 *   3. editor.applyDocument(result) — exercises the diff/patch path,
 *      narrowed per-line passes, scoped colourise, etc.
 *
 * On startup the profiler is reset and enabled; on teardown we dump a
 * summary table sorted by total_ms.
 */
#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <algorithm>
#include "core.h"
#include "editor.h"
#include "controller.h"
#include "providers/buffer_provider.h"
#include "profiler.h"
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

using namespace rcx;

class BenchSpamAppend : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void benchSpamAppend100();
    void testHexDimOnLastAppendedLine();
    void testControllerSpamDownPreservesAllDim();
    void benchTypeCycleColouringCorrect();
    void cleanupTestCase();
};

/* ── Helpers ─────────────────────────────────────────────────────── */

static NodeTree buildSeedTree(int seedFields)
{
    NodeTree tree;
    tree.baseAddress = 0x7FF600000000ULL;
    tree.m_nextId = 1;

    Node root;
    root.id = tree.m_nextId++;
    root.kind = NodeKind::Struct;
    root.name = QStringLiteral("instance");
    root.structTypeName = QStringLiteral("UnnamedClass0");
    root.classKeyword = QStringLiteral("class");
    root.parentId = 0;
    root.offset = 0;
    tree.addNode(root);

    int offset = 0;
    for (int i = 0; i < seedFields; ++i) {
        Node n;
        n.id = tree.m_nextId++;
        n.kind = NodeKind::Hex64;
        n.name = QStringLiteral("field_%1").arg(offset, 4, 16, QChar('0'));
        n.parentId = root.id;
        n.offset = offset;
        tree.addNode(n);
        offset += 8;
    }
    return tree;
}

static int treeEndOffset(const NodeTree& tree, uint64_t parentId)
{
    int maxEnd = 0;
    for (int ci : tree.childrenOf(parentId)) {
        const auto& n = tree.nodes[ci];
        int sz = (n.kind == NodeKind::Struct || n.kind == NodeKind::Array)
            ? tree.structSpan(n.id) : n.byteSize();
        int end = n.offset + sz;
        if (end > maxEnd) maxEnd = end;
    }
    return maxEnd;
}

static uint64_t appendHex64(NodeTree& tree, uint64_t parentId)
{
    Node n;
    n.id = tree.m_nextId++;
    n.kind = NodeKind::Hex64;
    int off = treeEndOffset(tree, parentId);
    n.name = QStringLiteral("field_%1").arg(off, 4, 16, QChar('0'));
    n.parentId = parentId;
    n.offset = off;
    tree.addNode(n);
    tree.invalidateIdCache();
    tree.touch();
    return n.id;
}

static void dumpProfile(const char* label)
{
    auto snap = Profiler::instance().snapshot();
    QVector<QPair<QString, ProfileStats>> rows;
    for (auto it = snap.constBegin(); it != snap.constEnd(); ++it)
        rows.emplaceBack(it.key(), it.value());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.totalNs > b.second.totalNs; });

    qDebug() << "";
    qDebug() << "=== Profile dump:" << label << "===";
    qDebug() << QString::asprintf(
        "  %-32s %6s %12s %12s %12s %12s",
        "name", "count", "total_ms", "mean_us", "min_us", "max_us").toUtf8().constData();
    for (const auto& r : rows) {
        const auto& s = r.second;
        double mean = s.count ? double(s.totalNs) / s.count : 0;
        qDebug() << QString::asprintf(
            "  %-32s %6d %12.4f %12.3f %12.3f %12.3f",
            r.first.toUtf8().constData(),
            s.count,
            s.totalNs / 1.0e6,
            mean / 1.0e3,
            s.minNs / 1.0e3,
            s.maxNs / 1.0e3).toUtf8().constData();
    }
}

/* ── Benchmarks ──────────────────────────────────────────────────── */

void BenchSpamAppend::initTestCase()
{
    Profiler::instance().reset();
    Profiler::instance().setEnabled(true);
}

void BenchSpamAppend::cleanupTestCase()
{
    Profiler::instance().setEnabled(false);
}

// Simulate holding Down at the end of a class — append N hex64 fields
// one at a time, re-compose and applyDocument each iteration.
void BenchSpamAppend::benchSpamAppend100()
{
    // Seed size chosen to match the user's prod profile (~110 lines)
    // so per-pass numbers compare apples-to-apples.
    NodeTree tree = buildSeedTree(/*seed=*/100);
    QByteArray buf(0x4000, '\0');
    for (int i = 0; i < buf.size(); ++i) buf[i] = char(i & 0xFF);
    BufferProvider prov(buf, QStringLiteral("bench_spam"));

    RcxEditor editor;
    editor.resize(900, 700);

    // Warmup: prime m_prevText and m_prevMeta so the diff path actually
    // runs (otherwise the first applyDocument hits the fullReplace branch).
    {
        ComposeResult r = rcx::compose(tree, prov);
        editor.applyDocument(r);
        editor.applyDocument(r);  // second call exercises diff with empty diff
    }

    Profiler::instance().reset();

    const int ITERS = 100;
    QElapsedTimer wall;
    wall.start();
    for (int i = 0; i < ITERS; ++i) {
        appendHex64(tree, /*parentId=*/1);  // root id = 1
        ComposeResult r = rcx::compose(tree, prov);
        editor.applyDocument(r);
    }
    qint64 wallMs = wall.elapsed();

    qDebug() << "";
    qDebug() << "=== Spam-append benchmark ===";
    qDebug() << "  Seed fields:" << 30 << " | appends:" << ITERS;
    qDebug() << "  Wall time:" << wallMs << "ms ("
             << QString::number(double(wallMs) / ITERS, 'f', 2) << "ms/append)";

    dumpProfile("after spam-append");

    QVERIFY(wallMs > 0);
}

// Direct regression for the "last hex64 isn't dimmed" report: append five
// fields one at a time, then for each frame verify Scintilla actually has
// IND_HEX_DIM applied on the byte range of the most-recently-added line.
// This is the exact symptom the user is seeing in production.
void BenchSpamAppend::testHexDimOnLastAppendedLine()
{
    NodeTree tree = buildSeedTree(/*seed=*/3);
    QByteArray buf(0x400, '\0');
    BufferProvider prov(buf, QStringLiteral("bench_lastline"));

    RcxEditor editor;
    editor.resize(900, 700);
    {
        ComposeResult r = rcx::compose(tree, prov);
        editor.applyDocument(r);
        editor.applyDocument(r);  // prime m_prevText/m_prevMeta for diff path
    }

    // Same indicator id used by editor.cpp
    static constexpr int IND_HEX_DIM = 9;

    auto* sci = editor.scintilla();

    QVector<uint64_t> appendedIds;
    for (int press = 1; press <= 5; ++press) {
        uint64_t newId = appendHex64(tree, /*parentId=*/1);
        appendedIds.append(newId);
        ComposeResult r = rcx::compose(tree, prov);
        editor.applyDocument(r);

        // After every press, verify IND_HEX_DIM is on EVERY hex64 line
        // we've ever appended — not just the most recent. The user
        // report is "previous hex64s lose their dim while only the
        // newest stays dim", so we must check the cumulative set.
        for (int p = 0; p < appendedIds.size(); ++p) {
            uint64_t id = appendedIds[p];
            int line = -1;
            for (int i = 0; i < r.meta.size(); ++i) {
                if (r.meta[i].nodeId == id
                    && r.meta[i].lineKind == LineKind::Field) {
                    line = i;
                    break;
                }
            }
            QVERIFY2(line >= 0, "couldn't locate field line by id");

            long byteStart = sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE,
                                                (unsigned long)line);
            long byteEnd = sci->SendScintilla(QsciScintillaBase::SCI_GETLINEENDPOSITION,
                                              (unsigned long)line);
            QVERIFY2(byteEnd > byteStart, "empty line");

            int sampleHits = 0, sampleTotal = 0;
            for (long off = byteStart; off < byteEnd;
                 off += qMax<long>(1, (byteEnd - byteStart) / 8)) {
                ++sampleTotal;
                long val = sci->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT,
                                              (unsigned long)IND_HEX_DIM, off);
                if (val) ++sampleHits;
            }
            qDebug() << "  press" << press << "field#" << p << "line=" << line
                     << "byteRange=[" << byteStart << ".." << byteEnd << ")"
                     << "IND_HEX_DIM hits:" << sampleHits << "/" << sampleTotal;
            QVERIFY2(sampleHits == sampleTotal,
                     QStringLiteral("press %1, field %2: IND_HEX_DIM missing")
                         .arg(press).arg(p).toUtf8().constData());
        }
    }
}

// Mirror the production "Down-at-end" path: drive
// `appendSingleFieldRequested` through the controller so we exercise
// insertNode → undoStack.push → applyCommand → tree.touch → refresh.
// This is the path the user actually triggers by holding Down.
void BenchSpamAppend::testControllerSpamDownPreservesAllDim()
{
    auto* doc = new RcxDocument;
    doc->tree = buildSeedTree(/*seed=*/3);
    QByteArray buf(0x4000, '\0');
    doc->loadData(buf);
    doc->tree.baseAddress = 0x7FF600000000ULL;

    RcxController ctrl(doc);
    auto* editor = ctrl.addSplitEditor();
    editor->resize(900, 700);
    ctrl.refresh();
    ctrl.refresh();  // prime diff path

    static constexpr int IND_HEX_DIM = 9;
    auto* sci = editor->scintilla();

    // Find the root struct id (the only Struct node with parentId 0).
    uint64_t rootId = 0;
    for (const auto& n : doc->tree.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) { rootId = n.id; break; }
    QVERIFY2(rootId != 0, "couldn't locate root struct");

    QVector<uint64_t> appendedIds;
    for (int press = 1; press <= 8; ++press) {
        // Find the LAST data node id (what editor's Down handler emits).
        uint64_t lastDataNodeId = 0;
        for (int i = ctrl.lastResult().meta.size() - 1; i >= 0; --i) {
            const auto& lm = ctrl.lastResult().meta[i];
            if (lm.nodeId == 0 || lm.isContinuation) continue;
            if (lm.lineKind != LineKind::Field
                && lm.lineKind != LineKind::Header) continue;
            lastDataNodeId = lm.nodeId;
            break;
        }
        QVERIFY2(lastDataNodeId != 0, "no last data node");

        // Drive the same signal the editor would on Down-at-end.
        emit editor->appendSingleFieldRequested(lastDataNodeId);

        // The insert command + refresh should have completed synchronously.
        // Locate the just-added node (last child of root).
        uint64_t newId = 0;
        auto kids = doc->tree.childrenOf(rootId);
        if (!kids.isEmpty()) newId = doc->tree.nodes[kids.last()].id;
        QVERIFY2(newId != 0, "couldn't locate appended node");
        appendedIds.append(newId);

        for (int p = 0; p < appendedIds.size(); ++p) {
            uint64_t id = appendedIds[p];
            int line = -1;
            for (int i = 0; i < ctrl.lastResult().meta.size(); ++i) {
                if (ctrl.lastResult().meta[i].nodeId == id
                    && ctrl.lastResult().meta[i].lineKind == LineKind::Field) {
                    line = i;
                    break;
                }
            }
            QVERIFY2(line >= 0, "couldn't locate field line");

            long byteStart = sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE,
                                                (unsigned long)line);
            long byteEnd = sci->SendScintilla(QsciScintillaBase::SCI_GETLINEENDPOSITION,
                                              (unsigned long)line);
            QVERIFY2(byteEnd > byteStart, "empty line");

            int sampleHits = 0, sampleTotal = 0;
            for (long off = byteStart; off < byteEnd;
                 off += qMax<long>(1, (byteEnd - byteStart) / 8)) {
                ++sampleTotal;
                long val = sci->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT,
                                              (unsigned long)IND_HEX_DIM, off);
                if (val) ++sampleHits;
            }
            qDebug() << "  ctrl press" << press << "field#" << p << "line=" << line
                     << "byteRange=[" << byteStart << ".." << byteEnd << ")"
                     << "IND_HEX_DIM hits:" << sampleHits << "/" << sampleTotal;
            if (sampleHits != sampleTotal) {
                QFAIL(QStringLiteral("press %1, field %2 (line %3): IND_HEX_DIM missing — %4/%5 hits")
                      .arg(press).arg(p).arg(line).arg(sampleHits).arg(sampleTotal)
                      .toUtf8().constData());
            }
        }
    }
    delete doc;
}

// Correctness check: cycle a node's kind through several types and verify
// applyDocument doesn't cache stale per-line state. We can't directly
// inspect Scintilla indicators here, but we can verify the meta-diff
// detected the change (by asserting the relevant per-line pass ran).
void BenchSpamAppend::benchTypeCycleColouringCorrect()
{
    NodeTree tree = buildSeedTree(/*seed=*/8);
    QByteArray buf(0x400, '\0');
    BufferProvider prov(buf, QStringLiteral("bench_cycle"));

    RcxEditor editor;
    editor.resize(900, 700);
    {
        ComposeResult r = rcx::compose(tree, prov);
        editor.applyDocument(r);
    }

    // Find the first leaf field — index 1 (since root is index 0).
    int leafIdx = -1;
    for (int i = 0; i < tree.nodes.size(); ++i)
        if (tree.nodes[i].parentId != 0 && tree.nodes[i].kind == NodeKind::Hex64) {
            leafIdx = i; break;
        }
    QVERIFY(leafIdx > 0);

    Profiler::instance().reset();

    // Cycle through the kinds the user gets via Space / S / U / F / P shortcuts.
    const NodeKind cycle[] = {
        NodeKind::Hex8, NodeKind::Hex16, NodeKind::Hex32, NodeKind::Hex64,
        NodeKind::Int8, NodeKind::UInt32, NodeKind::Float, NodeKind::Pointer64,
        NodeKind::Hex64,
    };
    for (auto k : cycle) {
        tree.nodes[leafIdx].kind = k;
        tree.touch();
        ComposeResult r = rcx::compose(tree, prov);
        editor.applyDocument(r);
    }

    dumpProfile("after type-cycle");

    auto snap = Profiler::instance().snapshot();
    int hexDimCalls = snap.value(QStringLiteral("applyHexDimming")).count;
    int lineAttrCalls = snap.value(QStringLiteral("applyLineAttributes")).count;

    qDebug() << "";
    qDebug() << "=== Type cycle correctness ===";
    qDebug() << "  applyHexDimming calls:" << hexDimCalls << "(expect" << (int)(sizeof(cycle)/sizeof(cycle[0])) << ")";
    qDebug() << "  applyLineAttributes calls:" << lineAttrCalls;

    // Each kind change must have fired the per-line passes; the meta diff
    // must have flagged the changed line. If sameLine() ignored nodeKind
    // we'd see fewer pass invocations or stale state (no programmatic way
    // to assert state from here, but call-count parity is a sanity check).
    QCOMPARE(hexDimCalls, (int)(sizeof(cycle)/sizeof(cycle[0])));
    QCOMPARE(lineAttrCalls, (int)(sizeof(cycle)/sizeof(cycle[0])));
}

QTEST_MAIN(BenchSpamAppend)
#include "bench_spam_append.moc"
