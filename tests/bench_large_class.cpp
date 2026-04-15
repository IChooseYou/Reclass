/*
 * bench_large_class — benchmark compose, applyDocument, hover highlight,
 * and selection overlay on a large struct (500+ fields).
 *
 * Simulates EPROCESS-class structures to measure editor performance.
 */
#include <QtTest/QtTest>
#include <QElapsedTimer>
#include "core.h"
#include "editor.h"
#include "providers/buffer_provider.h"

using namespace rcx;

/* ── Build a large struct tree with N fields of mixed types ──────── */

static NodeTree buildLargeTree(int fieldCount)
{
    NodeTree tree;
    tree.baseAddress = 0x7FF600000000ULL;

    // Root struct
    Node root;
    root.id = 1;
    root.kind = NodeKind::Struct;
    root.name = QStringLiteral("EPROCESS");
    root.structTypeName = QStringLiteral("_EPROCESS");
    root.parentId = 0;
    root.offset = 0;
    tree.addNode(root);

    // Cycle through common field types
    const NodeKind kinds[] = {
        NodeKind::Int32, NodeKind::UInt64, NodeKind::Float,
        NodeKind::Pointer64, NodeKind::Int16, NodeKind::UInt32,
        NodeKind::Double, NodeKind::Bool, NodeKind::Hex8
    };
    const int kindCount = sizeof(kinds) / sizeof(kinds[0]);

    int offset = 0;
    for (int i = 0; i < fieldCount; ++i) {
        Node n;
        n.id = (uint64_t)(i + 2);
        n.kind = kinds[i % kindCount];
        n.name = QStringLiteral("field_%1").arg(i, 4, 10, QChar('0'));
        n.parentId = 1;
        n.offset = offset;
        tree.addNode(n);
        offset += sizeForKind(n.kind);
    }
    tree.m_nextId = (uint64_t)(fieldCount + 2);
    return tree;
}

/* ══════════════════════════════════════════════════════════════════ */

class BenchLargeClass : public QObject {
    Q_OBJECT

private:
    NodeTree m_tree;
    BufferProvider m_prov;
    ComposeResult m_result;

private slots:
    void initTestCase();
    void benchCompose();
    void benchComposeLarge();
    void benchCompose45K();
    void benchApplyDocument();
    void benchApplyDocument45K();
    void benchRemoveNode45K();
    void benchChangeKind45K();
    void benchHoverHighlight();
    void benchSelectionOverlay();
    void benchHoverHighlightRepeated();

public:
    BenchLargeClass() : m_prov(QByteArray()) {}
};

void BenchLargeClass::initTestCase()
{
    m_tree = buildLargeTree(2000);

    // Create buffer large enough for all fields
    QByteArray buf(0x10000, '\0');
    // Fill with pattern so values are non-zero
    for (int i = 0; i < buf.size(); ++i)
        buf[i] = (char)(i & 0xFF);
    m_prov = BufferProvider(buf, QStringLiteral("bench_data"));

    // Pre-compose for tests that need the result
    m_result = rcx::compose(m_tree, m_prov);
    qDebug() << "Tree:" << m_tree.nodes.size() << "nodes,"
             << m_result.meta.size() << "display lines,"
             << m_result.text.size() << "chars";
}

void BenchLargeClass::benchCompose()
{
    const int ITERS = 100;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i) {
        ComposeResult r = rcx::compose(m_tree, m_prov);
        Q_UNUSED(r);
    }
    qint64 elapsed = timer.elapsed();

    qDebug() << "";
    qDebug() << "=== Compose Benchmark (500 fields) ===";
    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-compose:" << (double)elapsed / ITERS << "ms";
    QVERIFY(elapsed > 0);
}

void BenchLargeClass::benchComposeLarge()
{
    // Build a 2000-field tree to stress-test compose at scale
    NodeTree bigTree = buildLargeTree(2000);
    QByteArray buf(0x40000, '\0');
    for (int i = 0; i < buf.size(); ++i) buf[i] = (char)(i & 0xFF);
    BufferProvider bigProv(buf, QStringLiteral("bench_large"));

    // Warmup
    { ComposeResult w = rcx::compose(bigTree, bigProv); Q_UNUSED(w); }

    const int ITERS = 50;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i) {
        ComposeResult r = rcx::compose(bigTree, bigProv);
        Q_UNUSED(r);
    }
    qint64 elapsed = timer.elapsed();

    qDebug() << "";
    qDebug() << "=== Compose Benchmark (2000 fields) ===";
    qDebug() << "  Tree:" << bigTree.nodes.size() << "nodes";
    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-compose:" << (double)elapsed / ITERS << "ms";
    QVERIFY(elapsed > 0);
}

void BenchLargeClass::benchCompose45K()
{
    NodeTree bigTree = buildLargeTree(45000);
    QByteArray buf(0x100000, '\0');
    for (int i = 0; i < buf.size(); ++i) buf[i] = (char)(i & 0xFF);
    BufferProvider bigProv(buf, QStringLiteral("bench_45k"));

    // Warmup
    { ComposeResult w = rcx::compose(bigTree, bigProv); Q_UNUSED(w); }

    const int ITERS = 5;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i) {
        ComposeResult r = rcx::compose(bigTree, bigProv);
        Q_UNUSED(r);
    }
    qint64 elapsed = timer.elapsed();

    ComposeResult r = rcx::compose(bigTree, bigProv);
    qDebug() << "";
    qDebug() << "=== Compose Benchmark (45000 fields) ===";
    qDebug() << "  Tree:" << bigTree.nodes.size() << "nodes,"
             << r.meta.size() << "lines," << r.text.size() << "chars";
    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-compose:" << (double)elapsed / ITERS << "ms";
    QVERIFY(elapsed > 0);
}

void BenchLargeClass::benchApplyDocument()
{
    RcxEditor editor;
    editor.resize(800, 600);

    const int ITERS = 50;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i)
        editor.applyDocument(m_result);
    qint64 elapsed = timer.elapsed();

    qDebug() << "";
    qDebug() << "=== ApplyDocument Benchmark (2000 fields) ===";
    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-apply:" << (double)elapsed / ITERS << "ms";
    QVERIFY(elapsed > 0);
}

void BenchLargeClass::benchApplyDocument45K()
{
    NodeTree bigTree = buildLargeTree(45000);
    QByteArray buf(0x100000, '\0');
    for (int i = 0; i < buf.size(); ++i) buf[i] = (char)(i & 0xFF);
    BufferProvider bigProv(buf, QStringLiteral("bench_45k_apply"));

    ComposeResult bigResult = rcx::compose(bigTree, bigProv);
    qDebug() << "";
    qDebug() << "=== ApplyDocument Benchmark (45000 fields) ===";
    qDebug() << "  Lines:" << bigResult.meta.size()
             << "Text:" << bigResult.text.size() << "chars";

    RcxEditor editor;
    editor.resize(800, 600);

    // Warmup
    editor.applyDocument(bigResult);

    const int ITERS = 3;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i)
        editor.applyDocument(bigResult);
    qint64 elapsed = timer.elapsed();

    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-apply:" << (double)elapsed / ITERS << "ms";
    QVERIFY(elapsed > 0);
}

void BenchLargeClass::benchRemoveNode45K()
{
    // Benchmark the expensive operations that happen during removeNode on 45K tree:
    // structSpan (without childMap), childrenOf, subtreeIndices, compose, applyDocument
    NodeTree tree = buildLargeTree(45000);
    QByteArray buf(0x100000, '\0');
    for (int i = 0; i < buf.size(); ++i) buf[i] = (char)(i & 0xFF);
    BufferProvider prov(buf, QStringLiteral("bench_rm"));

    qDebug() << "";
    qDebug() << "=== RemoveNode Component Costs (45000 fields) ===";

    QElapsedTimer timer;

    // 1. structSpan WITHOUT childMap (the removeNode path)
    timer.start();
    for (int i = 0; i < 5; ++i) {
        int s = tree.structSpan(1);
        Q_UNUSED(s);
    }
    qDebug() << "  structSpan(root) no childMap x5:" << timer.elapsed() << "ms"
             << "(" << timer.elapsed() / 5.0 << "ms each)";

    // 2. structSpan WITH childMap
    QHash<uint64_t, QVector<int>> childMap;
    for (int i = 0; i < tree.nodes.size(); i++)
        childMap[tree.nodes[i].parentId].append(i);
    timer.start();
    for (int i = 0; i < 5; ++i) {
        int s = tree.structSpan(1, &childMap);
        Q_UNUSED(s);
    }
    qDebug() << "  structSpan(root) WITH childMap x5:" << timer.elapsed() << "ms"
             << "(" << timer.elapsed() / 5.0 << "ms each)";

    // 3. childrenOf (triggers cache rebuild)
    tree.invalidateIdCache();
    timer.start();
    auto kids = tree.childrenOf(1);
    qDebug() << "  childrenOf(root) cold cache:" << timer.elapsed() << "ms (" << kids.size() << "children)";

    timer.start();
    kids = tree.childrenOf(1);
    qDebug() << "  childrenOf(root) warm cache:" << timer.elapsed() << "ms";

    // 4. subtreeIndices
    tree.invalidateIdCache();
    timer.start();
    auto sub = tree.subtreeIndices(1);
    qDebug() << "  subtreeIndices(root) cold cache:" << timer.elapsed() << "ms (" << sub.size() << "nodes)";

    // 5. invalidateIdCache + reaccess
    timer.start();
    for (int i = 0; i < 10; ++i) {
        tree.invalidateIdCache();
        tree.indexOfId(1);  // triggers rebuild
    }
    qDebug() << "  invalidate+rebuild idCache x10:" << timer.elapsed() << "ms"
             << "(" << timer.elapsed() / 10.0 << "ms each)";

    // 6. Full compose
    timer.start();
    ComposeResult r = rcx::compose(tree, prov);
    qDebug() << "  compose:" << timer.elapsed() << "ms";

    // 7. Full applyDocument
    RcxEditor editor;
    editor.resize(800, 600);
    timer.start();
    editor.applyDocument(r);
    qDebug() << "  applyDocument:" << timer.elapsed() << "ms";

    // Total simulated removeNode cost
    qDebug() << "  --- Simulated total: structSpan + childrenOf + subtreeIndices + compose + apply ---";

    QVERIFY(true);
}

void BenchLargeClass::benchChangeKind45K()
{
    NodeTree tree = buildLargeTree(45000);
    QByteArray buf(0x100000, '\0');
    for (int i = 0; i < buf.size(); ++i) buf[i] = (char)(i & 0xFF);
    BufferProvider prov(buf, QStringLiteral("bench_ck"));

    qDebug() << "";
    qDebug() << "=== ChangeKind Component Costs (45000 fields) ===";

    QElapsedTimer timer;

    // Simulate offset adjustment: iterate 45K siblings
    auto siblings = tree.childrenOf(1);
    timer.start();
    QVector<std::pair<uint64_t, int>> adjs;
    for (int si : siblings) {
        auto& sib = tree.nodes[si];
        if (sib.offset >= 100)
            adjs.push_back({sib.id, sib.offset - 4});
    }
    qDebug() << "  Build offset adjustments (" << adjs.size() << "siblings):" << timer.elapsed() << "ms";

    // Apply adjustments (simulates what applyCommand does)
    timer.start();
    for (auto& [id, newOff] : adjs) {
        int idx = tree.indexOfId(id);
        if (idx >= 0) tree.nodes[idx].offset = newOff;
    }
    qDebug() << "  Apply offset adjustments:" << timer.elapsed() << "ms";

    // Simulate clearHistoryForAdjs: subtreeIndices per adjusted sibling (OLD path)
    timer.start();
    int totalDescendants = 0;
    for (int i = 0; i < qMin((int)adjs.size(), 100); ++i) {
        auto sub = tree.subtreeIndices(adjs[i].first);
        totalDescendants += sub.size();
    }
    qint64 sample100 = timer.elapsed();
    qDebug() << "  subtreeIndices x100 siblings (sample):" << sample100 << "ms"
             << "(extrapolated full 45K:" << (sample100 * adjs.size() / 100) << "ms)";

    QVERIFY(true);
}

void BenchLargeClass::benchHoverHighlight()
{
    RcxEditor editor;
    editor.resize(800, 600);
    editor.applyDocument(m_result);

    // Simulate hovering over the first field
    // We need access to internals, so we measure via public methods
    // by toggling selection which triggers applyHoverHighlight internally
    QSet<uint64_t> sel;
    sel.insert(2);  // first field node id

    const int ITERS = 200;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i) {
        editor.applySelectionOverlay(i % 2 == 0 ? sel : QSet<uint64_t>{});
    }
    qint64 elapsed = timer.elapsed();

    qDebug() << "";
    qDebug() << "=== Hover/Selection Overlay Benchmark (500 fields) ===";
    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-cycle:" << (double)elapsed / ITERS << "ms";
    QVERIFY(elapsed > 0);
}

void BenchLargeClass::benchSelectionOverlay()
{
    RcxEditor editor;
    editor.resize(800, 600);
    editor.applyDocument(m_result);

    // Select many nodes (simulate multi-select of 50 fields)
    QSet<uint64_t> bigSel;
    for (int i = 0; i < 50; ++i)
        bigSel.insert((uint64_t)(i + 2));

    const int ITERS = 100;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i) {
        editor.applySelectionOverlay(bigSel);
    }
    qint64 elapsed = timer.elapsed();

    qDebug() << "";
    qDebug() << "=== Multi-Selection Overlay Benchmark (50 selected, 500 fields) ===";
    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-overlay:" << (double)elapsed / ITERS << "ms";
    QVERIFY(elapsed > 0);
}

void BenchLargeClass::benchHoverHighlightRepeated()
{
    RcxEditor editor;
    editor.resize(800, 600);
    editor.applyDocument(m_result);

    // Simulate rapid hover changes: alternate between two different nodes
    // This is the worst case - every call does a full marker clear + rescan
    QSet<uint64_t> empty;
    QSet<uint64_t> sel1; sel1.insert(10);
    QSet<uint64_t> sel2; sel2.insert(100);

    const int ITERS = 500;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i) {
        editor.applySelectionOverlay(i % 3 == 0 ? sel1 : (i % 3 == 1 ? sel2 : empty));
    }
    qint64 elapsed = timer.elapsed();

    qDebug() << "";
    qDebug() << "=== Rapid Hover Change Benchmark (500 fields, alternating nodes) ===";
    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-change:" << (double)elapsed / ITERS << "ms";
    qDebug() << "  Simulated events/sec:" << (ITERS * 1000.0 / elapsed);
    QVERIFY(elapsed > 0);
}

QTEST_MAIN(BenchLargeClass)
#include "bench_large_class.moc"
