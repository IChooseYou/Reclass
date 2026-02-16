#include <QtTest/QtTest>
#include <QTemporaryFile>
#include "core.h"
#include "export_reclass_xml.h"
#include "import_reclass_xml.h"

using namespace rcx;

class TestExportXml : public QObject {
    Q_OBJECT
private slots:
    void exportEmptyTree();
    void exportSingleStruct();
    void exportPointerRef();
    void exportEmbeddedStruct();
    void exportArray();
    void exportTextNodes();
    void exportVectors();
    void exportHexCollapse();
    void exportMultiClass();
    void roundTripImportExport();
};

static int countRoots(const NodeTree& tree) {
    int n = 0;
    for (const auto& node : tree.nodes)
        if (node.parentId == 0 && node.kind == NodeKind::Struct) n++;
    return n;
}

static QVector<int> childrenOf(const NodeTree& tree, uint64_t parentId) {
    QVector<int> result;
    for (int i = 0; i < tree.nodes.size(); i++)
        if (tree.nodes[i].parentId == parentId) result.append(i);
    return result;
}

static QString exportToString(const NodeTree& tree) {
    QTemporaryFile tmp;
    tmp.setAutoRemove(true);
    if (!tmp.open()) return {};
    QString path = tmp.fileName();
    tmp.close();

    QString err;
    if (!exportReclassXml(tree, path, &err)) return {};

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll());
}

static NodeTree roundTrip(const NodeTree& tree) {
    QTemporaryFile tmp;
    tmp.setAutoRemove(true);
    if (!tmp.open()) return {};
    QString path = tmp.fileName();
    tmp.close();

    QString err;
    if (!exportReclassXml(tree, path, &err)) return {};
    return importReclassXml(path, &err);
}

// ── Tests ──

void TestExportXml::exportEmptyTree() {
    NodeTree tree;
    QString err;
    QVERIFY(!exportReclassXml(tree, "dummy.xml", &err));
    QVERIFY(!err.isEmpty());
}

void TestExportXml::exportSingleStruct() {
    NodeTree tree;
    Node s; s.kind = NodeKind::Struct; s.name = QStringLiteral("Player");
    s.structTypeName = QStringLiteral("Player"); s.parentId = 0;
    int si = tree.addNode(s);
    uint64_t sid = tree.nodes[si].id;

    Node f1; f1.kind = NodeKind::Int32; f1.name = QStringLiteral("health");
    f1.parentId = sid; f1.offset = 0; tree.addNode(f1);

    Node f2; f2.kind = NodeKind::Float; f2.name = QStringLiteral("speed");
    f2.parentId = sid; f2.offset = 4; tree.addNode(f2);

    Node f3; f3.kind = NodeKind::UInt64; f3.name = QStringLiteral("id");
    f3.parentId = sid; f3.offset = 8; tree.addNode(f3);

    QString xml = exportToString(tree);
    QVERIFY(!xml.isEmpty());
    QVERIFY(xml.contains(QStringLiteral("Player")));
    QVERIFY(xml.contains(QStringLiteral("health")));
    QVERIFY(xml.contains(QStringLiteral("speed")));
    QVERIFY(xml.contains(QStringLiteral("ReClassEx")));

    // Round-trip
    NodeTree rt = roundTrip(tree);
    QCOMPARE(countRoots(rt), 1);
    QCOMPARE(rt.nodes[0].name, QStringLiteral("Player"));
    auto kids = childrenOf(rt, rt.nodes[0].id);
    QCOMPARE(kids.size(), 3);
    QCOMPARE(rt.nodes[kids[0]].kind, NodeKind::Int32);
    QCOMPARE(rt.nodes[kids[1]].kind, NodeKind::Float);
    QCOMPARE(rt.nodes[kids[2]].kind, NodeKind::UInt64);
}

void TestExportXml::exportPointerRef() {
    NodeTree tree;
    Node s1; s1.kind = NodeKind::Struct; s1.name = QStringLiteral("Target");
    s1.structTypeName = QStringLiteral("Target"); s1.parentId = 0;
    int s1i = tree.addNode(s1);
    uint64_t s1id = tree.nodes[s1i].id;

    Node f; f.kind = NodeKind::Int32; f.name = QStringLiteral("val");
    f.parentId = s1id; f.offset = 0; tree.addNode(f);

    Node s2; s2.kind = NodeKind::Struct; s2.name = QStringLiteral("HasPtr");
    s2.structTypeName = QStringLiteral("HasPtr"); s2.parentId = 0;
    int s2i = tree.addNode(s2);
    uint64_t s2id = tree.nodes[s2i].id;

    Node ptr; ptr.kind = NodeKind::Pointer64; ptr.name = QStringLiteral("pTarget");
    ptr.parentId = s2id; ptr.offset = 0; ptr.refId = s1id;
    tree.addNode(ptr);

    QString xml = exportToString(tree);
    QVERIFY(xml.contains(QStringLiteral("Pointer=\"Target\"")));

    // Round-trip: pointer should resolve
    NodeTree rt = roundTrip(tree);
    QCOMPARE(countRoots(rt), 2);
    bool foundPtr = false;
    for (const auto& n : rt.nodes) {
        if (n.kind == NodeKind::Pointer64 && n.name == QStringLiteral("pTarget")) {
            QVERIFY(n.refId != 0);
            foundPtr = true;
        }
    }
    QVERIFY(foundPtr);
}

void TestExportXml::exportEmbeddedStruct() {
    NodeTree tree;
    Node inner; inner.kind = NodeKind::Struct; inner.name = QStringLiteral("Inner");
    inner.structTypeName = QStringLiteral("Inner"); inner.parentId = 0;
    int ii = tree.addNode(inner);
    uint64_t iid = tree.nodes[ii].id;

    Node iv; iv.kind = NodeKind::Int32; iv.name = QStringLiteral("x");
    iv.parentId = iid; iv.offset = 0; tree.addNode(iv);

    Node outer; outer.kind = NodeKind::Struct; outer.name = QStringLiteral("Outer");
    outer.structTypeName = QStringLiteral("Outer"); outer.parentId = 0;
    int oi = tree.addNode(outer);
    uint64_t oid = tree.nodes[oi].id;

    Node embed; embed.kind = NodeKind::Struct; embed.name = QStringLiteral("embedded");
    embed.structTypeName = QStringLiteral("Inner"); embed.parentId = oid;
    embed.offset = 0; embed.refId = iid;
    tree.addNode(embed);

    QString xml = exportToString(tree);
    QVERIFY(xml.contains(QStringLiteral("Instance=\"Inner\"")));
}

void TestExportXml::exportArray() {
    NodeTree tree;
    Node s; s.kind = NodeKind::Struct; s.name = QStringLiteral("Container");
    s.structTypeName = QStringLiteral("Container"); s.parentId = 0;
    int si = tree.addNode(s);
    uint64_t sid = tree.nodes[si].id;

    Node arr; arr.kind = NodeKind::Array; arr.name = QStringLiteral("items");
    arr.parentId = sid; arr.offset = 0; arr.arrayLen = 10;
    arr.elementKind = NodeKind::Int32;
    tree.addNode(arr);

    QString xml = exportToString(tree);
    QVERIFY(xml.contains(QStringLiteral("Total=\"10\"")));
    QVERIFY(xml.contains(QStringLiteral("<Array")));
}

void TestExportXml::exportTextNodes() {
    NodeTree tree;
    Node s; s.kind = NodeKind::Struct; s.name = QStringLiteral("TextStruct");
    s.structTypeName = QStringLiteral("TextStruct"); s.parentId = 0;
    int si = tree.addNode(s);
    uint64_t sid = tree.nodes[si].id;

    Node u8; u8.kind = NodeKind::UTF8; u8.name = QStringLiteral("name");
    u8.parentId = sid; u8.offset = 0; u8.strLen = 32; tree.addNode(u8);

    Node u16; u16.kind = NodeKind::UTF16; u16.name = QStringLiteral("wname");
    u16.parentId = sid; u16.offset = 32; u16.strLen = 16; tree.addNode(u16);

    NodeTree rt = roundTrip(tree);
    QCOMPARE(countRoots(rt), 1);
    auto kids = childrenOf(rt, rt.nodes[0].id);
    QCOMPARE(kids.size(), 2);
    QCOMPARE(rt.nodes[kids[0]].kind, NodeKind::UTF8);
    QCOMPARE(rt.nodes[kids[0]].strLen, 32);
    QCOMPARE(rt.nodes[kids[1]].kind, NodeKind::UTF16);
    QCOMPARE(rt.nodes[kids[1]].strLen, 16);
}

void TestExportXml::exportVectors() {
    NodeTree tree;
    Node s; s.kind = NodeKind::Struct; s.name = QStringLiteral("Vectors");
    s.structTypeName = QStringLiteral("Vectors"); s.parentId = 0;
    int si = tree.addNode(s);
    uint64_t sid = tree.nodes[si].id;

    Node v2; v2.kind = NodeKind::Vec2; v2.name = QStringLiteral("pos2");
    v2.parentId = sid; v2.offset = 0; tree.addNode(v2);

    Node v3; v3.kind = NodeKind::Vec3; v3.name = QStringLiteral("pos3");
    v3.parentId = sid; v3.offset = 8; tree.addNode(v3);

    Node v4; v4.kind = NodeKind::Vec4; v4.name = QStringLiteral("rot");
    v4.parentId = sid; v4.offset = 20; tree.addNode(v4);

    Node m; m.kind = NodeKind::Mat4x4; m.name = QStringLiteral("matrix");
    m.parentId = sid; m.offset = 36; tree.addNode(m);

    NodeTree rt = roundTrip(tree);
    auto kids = childrenOf(rt, rt.nodes[0].id);
    QCOMPARE(kids.size(), 4);
    QCOMPARE(rt.nodes[kids[0]].kind, NodeKind::Vec2);
    QCOMPARE(rt.nodes[kids[1]].kind, NodeKind::Vec3);
    QCOMPARE(rt.nodes[kids[2]].kind, NodeKind::Vec4);
    QCOMPARE(rt.nodes[kids[3]].kind, NodeKind::Mat4x4);
}

void TestExportXml::exportHexCollapse() {
    NodeTree tree;
    Node s; s.kind = NodeKind::Struct; s.name = QStringLiteral("HexTest");
    s.structTypeName = QStringLiteral("HexTest"); s.parentId = 0;
    int si = tree.addNode(s);
    uint64_t sid = tree.nodes[si].id;

    // 4 consecutive Hex8 nodes should collapse to one Custom node
    for (int i = 0; i < 4; i++) {
        Node h; h.kind = NodeKind::Hex8; h.parentId = sid; h.offset = i;
        tree.addNode(h);
    }
    // Followed by a real field
    Node f; f.kind = NodeKind::Int32; f.name = QStringLiteral("val");
    f.parentId = sid; f.offset = 4; tree.addNode(f);

    QString xml = exportToString(tree);
    // Should have Type="21" (Custom) for the collapsed hex
    QVERIFY(xml.contains(QStringLiteral("Type=\"21\"")));
    // Size should be 4
    QVERIFY(xml.contains(QStringLiteral("Size=\"4\"")));

    // Round-trip: custom expands back to hex nodes
    NodeTree rt = roundTrip(tree);
    QCOMPARE(countRoots(rt), 1);
    auto kids = childrenOf(rt, rt.nodes[0].id);
    // Import expands Custom(4 bytes) to best-fit hex: Hex32 (1 node) + Int32 = 2
    QVERIFY(kids.size() >= 2);
    // Last child should be Int32
    QCOMPARE(rt.nodes[kids.last()].kind, NodeKind::Int32);
}

void TestExportXml::exportMultiClass() {
    NodeTree tree;
    for (int c = 0; c < 5; c++) {
        Node s; s.kind = NodeKind::Struct;
        s.name = QStringLiteral("Class%1").arg(c);
        s.structTypeName = s.name; s.parentId = 0;
        int si = tree.addNode(s);
        uint64_t sid = tree.nodes[si].id;

        Node f; f.kind = NodeKind::Int32;
        f.name = QStringLiteral("field%1").arg(c);
        f.parentId = sid; f.offset = 0; tree.addNode(f);
    }

    NodeTree rt = roundTrip(tree);
    QCOMPARE(countRoots(rt), 5);

    // All class names preserved
    QSet<QString> names;
    for (const auto& n : rt.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) names.insert(n.name);
    for (int c = 0; c < 5; c++)
        QVERIFY(names.contains(QStringLiteral("Class%1").arg(c)));
}

void TestExportXml::roundTripImportExport() {
    // Build a comprehensive tree and verify it survives export->import
    NodeTree tree;

    Node s; s.kind = NodeKind::Struct; s.name = QStringLiteral("FullTest");
    s.structTypeName = QStringLiteral("FullTest"); s.parentId = 0;
    int si = tree.addNode(s);
    uint64_t sid = tree.nodes[si].id;

    int offset = 0;
    auto addField = [&](NodeKind kind, const QString& name) {
        Node n; n.kind = kind; n.name = name; n.parentId = sid; n.offset = offset;
        tree.addNode(n);
        offset += sizeForKind(kind);
    };

    addField(NodeKind::Int8, QStringLiteral("a"));
    addField(NodeKind::Int16, QStringLiteral("b"));
    addField(NodeKind::Int32, QStringLiteral("c"));
    addField(NodeKind::Int64, QStringLiteral("d"));
    addField(NodeKind::UInt8, QStringLiteral("e"));
    addField(NodeKind::UInt16, QStringLiteral("f"));
    addField(NodeKind::UInt32, QStringLiteral("g"));
    addField(NodeKind::UInt64, QStringLiteral("h"));
    addField(NodeKind::Float, QStringLiteral("i"));
    addField(NodeKind::Double, QStringLiteral("j"));
    addField(NodeKind::Vec2, QStringLiteral("k"));
    addField(NodeKind::Vec3, QStringLiteral("l"));
    addField(NodeKind::Vec4, QStringLiteral("m"));

    // Self-pointer
    Node ptr; ptr.kind = NodeKind::Pointer64; ptr.name = QStringLiteral("self");
    ptr.parentId = sid; ptr.offset = offset; ptr.refId = sid;
    tree.addNode(ptr);
    offset += 8;

    // UTF8
    Node u8; u8.kind = NodeKind::UTF8; u8.name = QStringLiteral("str");
    u8.parentId = sid; u8.offset = offset; u8.strLen = 64;
    tree.addNode(u8);

    NodeTree rt = roundTrip(tree);
    QCOMPARE(countRoots(rt), 1);
    QCOMPARE(rt.nodes[0].name, QStringLiteral("FullTest"));

    auto origKids = childrenOf(tree, sid);
    auto rtKids = childrenOf(rt, rt.nodes[0].id);
    QCOMPARE(rtKids.size(), origKids.size());

    // Verify each field kind matches
    for (int i = 0; i < origKids.size(); i++) {
        QCOMPARE(rt.nodes[rtKids[i]].kind, tree.nodes[origKids[i]].kind);
        QCOMPARE(rt.nodes[rtKids[i]].name, tree.nodes[origKids[i]].name);
    }

    // Verify self-pointer resolved
    bool foundSelf = false;
    for (const auto& n : rt.nodes) {
        if (n.name == QStringLiteral("self") && n.kind == NodeKind::Pointer64) {
            QVERIFY(n.refId != 0);
            QCOMPARE(n.refId, rt.nodes[0].id);
            foundSelf = true;
        }
    }
    QVERIFY(foundSelf);
}

QTEST_MAIN(TestExportXml)
#include "test_export_xml.moc"
