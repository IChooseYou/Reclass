#include <QtTest/QTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <random>
#include "core.h"
#include "imports/import_source.h"
#include "generator.h"

using namespace rcx;

class TestRoundtripWinSdk : public QObject {
    Q_OBJECT
private:
    NodeTree fullTree;
    QVector<int> rootIndices;

private slots:
    void initTestCase();
    void importCount();
    void pebOffsets();
    void roundTrip30();
    void generateRcx();
};

void TestRoundtripWinSdk::initTestCase()
{
    QString path = QStringLiteral(WINSDK_HEADER_PATH);
    QFile file(path);
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable("Cannot open " + path));
    QString source = QString::fromUtf8(file.readAll());
    QVERIFY(!source.isEmpty());

    QString err;
    fullTree = importFromSource(source, &err, 8);

    for (int i = 0; i < fullTree.nodes.size(); i++) {
        const auto& n = fullTree.nodes[i];
        if (n.parentId == 0 && n.kind == NodeKind::Struct)
            rootIndices.append(i);
    }
    qDebug() << "Imported" << fullTree.nodes.size() << "total nodes,"
             << rootIndices.size() << "root structs";
}

void TestRoundtripWinSdk::importCount()
{
    QVERIFY2(rootIndices.size() >= 3000,
             qPrintable(QString("Expected >= 3000 roots, got %1").arg(rootIndices.size())));
}

void TestRoundtripWinSdk::pebOffsets()
{
    // Verify _PEB field offsets match WinDbg dt ntdll!_PEB
    int pebIdx = -1;
    for (int i = 0; i < fullTree.nodes.size(); i++) {
        if (fullTree.nodes[i].parentId == 0 &&
            fullTree.nodes[i].structTypeName == QStringLiteral("_PEB")) {
            pebIdx = i;
            break;
        }
    }
    QVERIFY2(pebIdx >= 0, "Could not find _PEB root struct");

    uint64_t pebId = fullTree.nodes[pebIdx].id;

    // Collect direct children with offsets and sizes
    struct ChildInfo { QString name; int offset; int size; NodeKind kind; };
    QVector<ChildInfo> children;
    for (int i = 0; i < fullTree.nodes.size(); i++) {
        if (fullTree.nodes[i].parentId == pebId) {
            int sz = sizeForKind(fullTree.nodes[i].kind);
            if (sz == 0) sz = fullTree.structSpan(fullTree.nodes[i].id);
            if (sz == 0) sz = 1;
            children.push_back(ChildInfo{fullTree.nodes[i].name, fullTree.nodes[i].offset, sz, fullTree.nodes[i].kind});
        }
    }

    // Sort by offset
    std::sort(children.begin(), children.end(),
              [](const ChildInfo& a, const ChildInfo& b) { return a.offset < b.offset; });

    // Dump all children for diagnostics
    for (const auto& c : children) {
        qDebug() << "  " << Qt::hex << c.offset << c.name
                 << "kind=" << kindToString(c.kind) << "size=" << c.size;
    }

    // Check for overlaps
    int overlapCount = 0;
    for (int i = 1; i < children.size(); i++) {
        int prevEnd = children[i-1].offset + children[i-1].size;
        if (children[i].offset < prevEnd && children[i-1].kind != NodeKind::Struct) {
            // Only flag overlaps where previous field has a known size (not struct references)
            overlapCount++;
            if (overlapCount <= 10)
                qDebug() << "  OVERLAP:" << children[i].name << "at" << Qt::hex << children[i].offset
                         << "overlaps" << children[i-1].name << "(ends at" << Qt::hex << prevEnd << ")";
        }
    }

    // Build name→offset map for field checks
    QHash<QString, int> offsets;
    QHash<QString, NodeKind> kinds;
    for (const auto& c : children) {
        offsets[c.name] = c.offset;
        kinds[c.name] = c.kind;
    }

    int failCount = 0;
    auto checkField = [&](const QString& name, int expected, bool mustBePointer = false) {
        if (!offsets.contains(name)) {
            qDebug() << "  MISSING:" << name;
            failCount++;
            return;
        }
        if (offsets[name] != expected) {
            qDebug() << "  OFFSET MISMATCH:" << name << "got" << Qt::hex << offsets[name]
                     << "expected" << Qt::hex << expected;
            failCount++;
            return;
        }
        if (mustBePointer) {
            NodeKind k = kinds[name];
            if (k != NodeKind::Pointer64 && k != NodeKind::Pointer32) {
                qDebug() << "  NOT POINTER:" << name << "kind=" << kindToString(k);
                failCount++;
            }
        }
    };

    // Expected offsets computed from the source header layout (Vergilius-style)
    // Note: This header has union ALIGN(8) { KernelCallbackTable; UserSharedInfoPtr; }
    // after CrossProcessFlags, which shifts fields +0xC compared to some WinDbg versions.
    checkField(QStringLiteral("InheritedAddressSpace"),  0x000);
    checkField(QStringLiteral("ReadImageFileExecOptions"), 0x001);
    checkField(QStringLiteral("BeingDebugged"),          0x002);
    checkField(QStringLiteral("Mutant"),                 0x008, true);
    checkField(QStringLiteral("ImageBaseAddress"),       0x010, true);
    checkField(QStringLiteral("Ldr"),                    0x018, true);
    checkField(QStringLiteral("ProcessParameters"),      0x020, true);
    checkField(QStringLiteral("SubSystemData"),          0x028, true);
    checkField(QStringLiteral("ProcessHeap"),            0x030, true);
    checkField(QStringLiteral("FastPebLock"),            0x038, true);
    checkField(QStringLiteral("AtlThunkSListPtr"),       0x040, true);
    checkField(QStringLiteral("IFEOKey"),                0x048, true);
    checkField(QStringLiteral("SystemReserved"),         0x060);
    checkField(QStringLiteral("AtlThunkSListPtr32"),     0x064);
    checkField(QStringLiteral("ApiSetMap"),              0x068, true);
    checkField(QStringLiteral("TlsExpansionCounter"),    0x070);
    checkField(QStringLiteral("TlsBitmap"),              0x078, true);
    checkField(QStringLiteral("TlsBitmapBits"),          0x080);
    checkField(QStringLiteral("ReadOnlySharedMemoryBase"), 0x088, true);
    checkField(QStringLiteral("SharedData"),             0x090, true);
    checkField(QStringLiteral("ReadOnlyStaticServerData"), 0x098, true);
    checkField(QStringLiteral("AnsiCodePageData"),       0x0A0, true);
    checkField(QStringLiteral("OemCodePageData"),        0x0A8, true);
    checkField(QStringLiteral("UnicodeCaseTableData"),   0x0B0, true);
    checkField(QStringLiteral("NumberOfProcessors"),     0x0B8);
    checkField(QStringLiteral("NtGlobalFlag"),           0x0BC);
    checkField(QStringLiteral("HeapSegmentReserve"),     0x0C8);
    checkField(QStringLiteral("NumberOfHeaps"),          0x0E8);
    checkField(QStringLiteral("MaximumNumberOfHeaps"),   0x0EC);
    checkField(QStringLiteral("ProcessHeaps"),           0x0F0, true);
    checkField(QStringLiteral("OSMajorVersion"),         0x118);
    checkField(QStringLiteral("OSMinorVersion"),         0x11C);
    checkField(QStringLiteral("OSBuildNumber"),          0x120);
    checkField(QStringLiteral("SessionId"),              0x2C0);
    checkField(QStringLiteral("CsrServerReadOnlySharedMemoryBase"), 0x380);
    checkField(QStringLiteral("TppWorkerpListLock"),     0x388, true);
    checkField(QStringLiteral("WaitOnAddressHashTable"), 0x3A0);
    checkField(QStringLiteral("TelemetryCoverageHeader"), 0x7A0, true);
    checkField(QStringLiteral("CloudFileFlags"),         0x7A8);
    checkField(QStringLiteral("CloudFileDiagFlags"),     0x7AC);
    checkField(QStringLiteral("PlaceholderCompatibilityMode"), 0x7B0);
    checkField(QStringLiteral("LeapSecondData"),         0x7B8, true);
    checkField(QStringLiteral("NtGlobalFlag2"),          0x7C4);

    QVERIFY2(failCount == 0,
             qPrintable(QString("%1 PEB field(s) have wrong offsets or are missing").arg(failCount)));
}

void TestRoundtripWinSdk::roundTrip30()
{
    const int kRequired = 30;

    // Deterministic shuffle
    QVector<int> shuffled = rootIndices;
    std::mt19937 rng(42);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);

    int passCount = 0;
    int failCount = 0;
    int skipCount = 0;

    for (int ri : shuffled) {
        uint64_t rootId = fullTree.nodes[ri].id;
        QString structName = fullTree.nodes[ri].structTypeName;

        // Pass 1: export from full tree
        QString cpp1 = renderCpp(fullTree, rootId, nullptr, true);
        if (cpp1.isEmpty()) {
            skipCount++;
            continue;
        }

        // Pass 2: re-import
        QString err;
        NodeTree tree2 = importFromSource(cpp1, &err);
        if (tree2.nodes.isEmpty()) {
            skipCount++;
            continue;
        }

        // Find the root in re-imported tree
        int rootIdx2 = -1;
        for (int i = 0; i < tree2.nodes.size(); i++) {
            if (tree2.nodes[i].parentId == 0 && tree2.nodes[i].kind == NodeKind::Struct) {
                if (tree2.nodes[i].structTypeName == structName) {
                    rootIdx2 = i;
                    break;
                }
            }
        }
        if (rootIdx2 < 0) {
            // Take first root
            for (int i = 0; i < tree2.nodes.size(); i++) {
                if (tree2.nodes[i].parentId == 0 && tree2.nodes[i].kind == NodeKind::Struct) {
                    rootIdx2 = i;
                    break;
                }
            }
        }
        if (rootIdx2 < 0) {
            skipCount++;
            continue;
        }

        // Pass 3: re-export
        QString cpp2 = renderCpp(tree2, tree2.nodes[rootIdx2].id, nullptr, true);

        if (cpp1 == cpp2) {
            passCount++;
            if (passCount <= kRequired)
                qDebug() << "  PASS" << passCount << structName;
        } else {
            failCount++;
            if (failCount <= 5) {
                // Log first few failures for diagnostics
                QStringList lines1 = cpp1.split('\n');
                QStringList lines2 = cpp2.split('\n');
                int diffLine = -1;
                for (int i = 0; i < qMin(lines1.size(), lines2.size()); i++) {
                    if (lines1[i] != lines2[i]) { diffLine = i; break; }
                }
                if (diffLine >= 0) {
                    qDebug() << "  FAIL" << structName << "first diff at line" << diffLine;
                    qDebug() << "    cpp1:" << lines1[diffLine].left(120);
                    qDebug() << "    cpp2:" << lines2[diffLine].left(120);
                } else {
                    qDebug() << "  FAIL" << structName << "line count differs:"
                             << lines1.size() << "vs" << lines2.size();
                }
            }
        }

        if (passCount >= kRequired && failCount > 5)
            break; // found enough passes and logged enough failures
    }

    qDebug() << "Round-trip results: pass=" << passCount
             << "fail=" << failCount << "skip=" << skipCount;
    QVERIFY2(passCount >= kRequired,
             qPrintable(QString("Need %1 stable round-trips, got %2")
                        .arg(kRequired).arg(passCount)));
}

void TestRoundtripWinSdk::generateRcx()
{
    // Set all root structs collapsed
    for (int ri : rootIndices)
        fullTree.nodes[ri].collapsed = true;

    fullTree.baseAddress = 0xFFFFF80000000000ULL;

    QJsonObject json = fullTree.toJson();
    QJsonDocument jdoc(json);
    QByteArray data = jdoc.toJson(QJsonDocument::Indented);

    QVERIFY2(data.size() > 1000000,
             qPrintable(QString("RCX too small: %1 bytes").arg(data.size())));

    QString outPath = QStringLiteral(WINSDK_RCX_OUTPUT);
    QFile file(outPath);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
             qPrintable("Cannot write " + outPath));
    file.write(data);
    file.close();

    qDebug() << "Wrote" << data.size() << "bytes to" << outPath;
}

QTEST_MAIN(TestRoundtripWinSdk)
#include "test_roundtrip_winsdk.moc"
