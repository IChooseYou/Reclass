#include <QtTest/QtTest>
#include <QElapsedTimer>
#include "core.h"
#include "imports/import_pdb.h"

using namespace rcx;

class BenchImportPdb : public QObject {
    Q_OBJECT
private slots:
    void benchEnumerateAll();
    void benchImportAll();
};

static const QString kPdbPath = QStringLiteral(
    "C:/Symbols/ntkrnlmp.pdb/0762CF42EF7F3E8116EF7329ADAA09A31/ntkrnlmp.pdb");

void BenchImportPdb::benchEnumerateAll() {
    if (!QFile::exists(kPdbPath))
        QSKIP("ntkrnlmp.pdb not found at expected path");

    QString err;
    QElapsedTimer timer;
    timer.start();
    QVector<PdbTypeInfo> types = enumeratePdbTypes(kPdbPath, &err);
    qint64 elapsed = timer.elapsed();

    QVERIFY2(!types.isEmpty(), qPrintable(err));
    qDebug() << "enumeratePdbTypes:" << types.size() << "types in" << elapsed << "ms";
}

void BenchImportPdb::benchImportAll() {
    if (!QFile::exists(kPdbPath))
        QSKIP("ntkrnlmp.pdb not found at expected path");

    // Phase 1: enumerate
    QString err;
    QElapsedTimer timer;
    timer.start();
    QVector<PdbTypeInfo> types = enumeratePdbTypes(kPdbPath, &err);
    qint64 enumerateMs = timer.elapsed();
    QVERIFY2(!types.isEmpty(), qPrintable(err));

    // Collect all type indices
    QVector<uint32_t> indices;
    indices.reserve(types.size());
    for (const auto& t : types)
        indices.append(t.typeIndex);

    // Phase 2: import all
    timer.restart();
    int lastProgress = 0;
    NodeTree tree = importPdbSelected(kPdbPath, indices, &err,
        [&](int cur, int total) -> bool {
            // Report progress at 25% intervals
            int pct = (cur * 100) / total;
            if (pct >= lastProgress + 25) {
                qDebug() << "  progress:" << cur << "/" << total
                         << "(" << pct << "%)";
                lastProgress = pct;
            }
            return true;
        });
    qint64 importMs = timer.elapsed();

    QVERIFY2(!tree.nodes.isEmpty(), qPrintable(err));

    // Count root structs
    int rootCount = 0;
    for (const auto& n : tree.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) rootCount++;

    qDebug() << "";
    qDebug() << "=== PDB Import Benchmark (ntkrnlmp.pdb) ===";
    qDebug() << "  Enumerate:" << types.size() << "types in" << enumerateMs << "ms";
    qDebug() << "  Import all:" << rootCount << "root structs,"
             << tree.nodes.size() << "total nodes in" << importMs << "ms";
    qDebug() << "  Total:" << (enumerateMs + importMs) << "ms";
    qDebug() << "============================================";
}

QTEST_MAIN(BenchImportPdb)
#include "bench_import_pdb.moc"
