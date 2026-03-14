/*
 * bench_project — benchmark project lifecycle operations:
 *   - New class creation
 *   - Loading large .rcx files (WinSDK, Vergilius)
 *   - Workspace model building
 *   - Workspace search filtering
 *   - JSON parsing vs model building breakdown
 */
#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include "core.h"
#include "controller.h"
#include "workspace_model.h"

using namespace rcx;

class BenchProject : public QObject {
    Q_OBJECT

private slots:
    void benchNewClass();
    void benchLoadVergilius();
    void benchLoadWinSDK();
    void benchJsonParse();
    void benchNodeTreeFromJson();
    void benchBuildWorkspaceModel();
    void benchWorkspaceSearch();
};

static QString findExample(const QString& name) {
    // Try relative to executable, then common build layout
    QStringList candidates = {
        QCoreApplication::applicationDirPath() + "/examples/" + name,
        QCoreApplication::applicationDirPath() + "/../src/examples/" + name,
        QStringLiteral("src/examples/") + name,
        QStringLiteral("../src/examples/") + name,
    };
    for (const auto& c : candidates)
        if (QFileInfo::exists(c)) return c;
    return {};
}

// ── New class (just the core operations, no UI) ──

void BenchProject::benchNewClass()
{
    const int ITERS = 1000;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i) {
        NodeTree tree;
        tree.baseAddress = 0x00400000;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = QStringLiteral("NewClass");
        root.structTypeName = QStringLiteral("NewClass");
        root.classKeyword = QStringLiteral("class");
        tree.addNode(root);
        // Add 8 hex64 padding fields (what buildEmptyStruct does)
        uint64_t rootId = tree.nodes[0].id;
        for (int j = 0; j < 8; ++j) {
            Node pad;
            pad.kind = NodeKind::Hex64;
            pad.name = QString();
            pad.parentId = rootId;
            pad.offset = j * 8;
            tree.addNode(pad);
        }
    }
    qint64 elapsed = timer.elapsed();

    qDebug() << "";
    qDebug() << "=== New Class (core tree build) ===";
    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-new:" << (double)elapsed / ITERS << "ms";
}

// ── Load .rcx files ──

static bool loadRcx(const QString& path, NodeTree& tree) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonDocument jdoc = QJsonDocument::fromJson(f.readAll());
    tree = NodeTree::fromJson(jdoc.object());
    return !tree.nodes.isEmpty();
}

void BenchProject::benchLoadVergilius()
{
    QString path = findExample("Vergilius_25H2.rcx");
    if (path.isEmpty()) { QSKIP("Vergilius_25H2.rcx not found"); return; }

    const int ITERS = 5;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i) {
        NodeTree tree;
        QVERIFY(loadRcx(path, tree));
        if (i == 0)
            qDebug() << "  Nodes:" << tree.nodes.size();
    }
    qint64 elapsed = timer.elapsed();

    qDebug() << "";
    qDebug() << "=== Load Vergilius_25H2.rcx ===";
    qDebug() << "  File:" << QFileInfo(path).size() / 1024 << "KB";
    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-load:" << (double)elapsed / ITERS << "ms";
}

void BenchProject::benchLoadWinSDK()
{
    QString path = findExample("WinSDK.rcx");
    if (path.isEmpty()) { QSKIP("WinSDK.rcx not found"); return; }

    const int ITERS = 5;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i) {
        NodeTree tree;
        QVERIFY(loadRcx(path, tree));
        if (i == 0)
            qDebug() << "  Nodes:" << tree.nodes.size();
    }
    qint64 elapsed = timer.elapsed();

    qDebug() << "";
    qDebug() << "=== Load WinSDK.rcx ===";
    qDebug() << "  File:" << QFileInfo(path).size() / 1024 << "KB";
    qDebug() << "  Iterations:" << ITERS;
    qDebug() << "  Total:" << elapsed << "ms";
    qDebug() << "  Per-load:" << (double)elapsed / ITERS << "ms";
}

// ── Breakdown: JSON parse vs NodeTree build ──

void BenchProject::benchJsonParse()
{
    QString path = findExample("Vergilius_25H2.rcx");
    if (path.isEmpty()) path = findExample("WinSDK.rcx");
    if (path.isEmpty()) { QSKIP("No large .rcx found"); return; }

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QByteArray data = f.readAll();
    f.close();

    const int ITERS = 5;

    // Phase 1: raw JSON parse
    QElapsedTimer timer;
    timer.start();
    QJsonDocument jdoc;
    for (int i = 0; i < ITERS; ++i)
        jdoc = QJsonDocument::fromJson(data);
    qint64 jsonMs = timer.elapsed();

    // Phase 2: NodeTree::fromJson
    QJsonObject root = jdoc.object();
    timer.start();
    NodeTree tree;
    for (int i = 0; i < ITERS; ++i)
        tree = NodeTree::fromJson(root);
    qint64 treeMs = timer.elapsed();

    qDebug() << "";
    qDebug() << "=== JSON Parse Breakdown ===" << QFileInfo(path).fileName();
    qDebug() << "  File:" << data.size() / 1024 << "KB," << tree.nodes.size() << "nodes";
    qDebug() << "  JSON parse:" << (double)jsonMs / ITERS << "ms/iter";
    qDebug() << "  NodeTree build:" << (double)treeMs / ITERS << "ms/iter";
    qDebug() << "  Total per-load:" << (double)(jsonMs + treeMs) / ITERS << "ms";
}

void BenchProject::benchNodeTreeFromJson()
{
    // Already covered by benchJsonParse breakdown
    QVERIFY(true);
}

// ── Workspace model building ──

void BenchProject::benchBuildWorkspaceModel()
{
    // Load both large examples if available
    QVector<NodeTree> trees;
    for (const auto& name : {QStringLiteral("Vergilius_25H2.rcx"), QStringLiteral("WinSDK.rcx")}) {
        QString path = findExample(name);
        if (path.isEmpty()) continue;
        NodeTree t;
        if (loadRcx(path, t)) trees.append(std::move(t));
    }
    if (trees.isEmpty()) { QSKIP("No .rcx examples found"); return; }

    // Build TabInfo array
    QVector<TabInfo> tabs;
    for (const auto& t : trees)
        tabs.push_back(TabInfo{ &t, QStringLiteral("test"), nullptr });

    QStandardItemModel model;
    const int ITERS = 20;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i)
        buildProjectExplorer(&model, tabs);
    qint64 elapsed = timer.elapsed();

    // Count items
    int topLevel = model.rowCount();
    int totalChildren = 0;
    for (int i = 0; i < topLevel; ++i)
        totalChildren += model.item(i)->rowCount();

    int totalNodes = 0;
    for (const auto& t : trees) totalNodes += t.nodes.size();
    fprintf(stderr, "\n=== Build Workspace Model ===\n");
    fprintf(stderr, "  Trees: %d  total nodes: %d\n", (int)trees.size(), totalNodes);
    fprintf(stderr, "  Top-level items: %d  child items: %d\n", topLevel, totalChildren);
    fprintf(stderr, "  Iterations: %d\n", ITERS);
    fprintf(stderr, "  Total: %lld ms\n", (long long)elapsed);
    fprintf(stderr, "  Per-build: %.1f ms\n", (double)elapsed / ITERS);
}

// ── Workspace search filtering ──

void BenchProject::benchWorkspaceSearch()
{
    QVector<NodeTree> trees;
    for (const auto& name : {QStringLiteral("Vergilius_25H2.rcx"), QStringLiteral("WinSDK.rcx")}) {
        QString path = findExample(name);
        if (path.isEmpty()) continue;
        NodeTree t;
        if (loadRcx(path, t)) trees.append(std::move(t));
    }
    if (trees.isEmpty()) { QSKIP("No .rcx examples found"); return; }

    QVector<TabInfo> tabs;
    for (const auto& t : trees)
        tabs.push_back(TabInfo{ &t, QStringLiteral("test"), nullptr });

    QStandardItemModel model;
    buildProjectExplorer(&model, tabs);

    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&model);
    proxy.setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy.setRecursiveFilteringEnabled(true);

    const QStringList queries = {
        "EPROCESS", "KTHREAD", "LIST_ENTRY", "HAL", "DMA",
        "xyz_no_match", "a", "Dispatch"
    };

    const int ITERS = 50;
    QElapsedTimer timer;

    timer.start();
    for (int i = 0; i < ITERS; ++i) {
        for (const auto& q : queries)
            proxy.setFilterFixedString(q);
        proxy.setFilterFixedString(QString());  // clear
    }
    qint64 elapsed = timer.elapsed();

    int totalOps = ITERS * (queries.size() + 1);
    fprintf(stderr, "\n=== Workspace Search Filter ===\n");
    fprintf(stderr, "  Model rows: %d  queries: %d\n", model.rowCount(), (int)queries.size());
    fprintf(stderr, "  Iterations: %d  total filter ops: %d\n", ITERS, totalOps);
    fprintf(stderr, "  Total: %lld ms\n", (long long)elapsed);
    fprintf(stderr, "  Per-filter: %.2f ms\n", (double)elapsed / totalOps);
}

QTEST_MAIN(BenchProject)
#include "bench_project.moc"
