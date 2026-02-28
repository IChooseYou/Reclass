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
    void benchApplyDocument();
    void benchHoverHighlight();
    void benchSelectionOverlay();
    void benchHoverHighlightRepeated();

public:
    BenchLargeClass() : m_prov(QByteArray()) {}
};

void BenchLargeClass::initTestCase()
{
    m_tree = buildLargeTree(500);

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
    qDebug() << "=== ApplyDocument Benchmark (500 fields) ===";
    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-apply:" << (double)elapsed / ITERS << "ms";
    QVERIFY(elapsed > 0);
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
