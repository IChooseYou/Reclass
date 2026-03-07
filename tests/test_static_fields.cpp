#include <QtTest/QTest>
#include "core.h"
#include "addressparser.h"

using namespace rcx;

class TestStaticFields : public QObject {
    Q_OBJECT

    // Convenience: build a struct with N regular fields + static fields
    struct TestTree {
        NodeTree tree;
        uint64_t rootId = 0;

        TestTree() {
            Node root;
            root.kind = NodeKind::Struct;
            root.name = "TestStruct";
            root.structTypeName = "TestStruct";
            root.parentId = 0;
            int ri = tree.addNode(root);
            rootId = tree.nodes[ri].id;
        }

        int addField(const QString& name, NodeKind kind, int offset) {
            Node f;
            f.kind = kind;
            f.name = name;
            f.parentId = rootId;
            f.offset = offset;
            return tree.addNode(f);
        }

        int addStaticField(const QString& name, const QString& expr,
                      NodeKind kind = NodeKind::Hex64) {
            Node h;
            h.kind = kind;
            h.name = name;
            h.parentId = rootId;
            h.offset = 0;
            h.isStatic = true;
            h.offsetExpr = expr;
            return tree.addNode(h);
        }
    };

private slots:

    // ── Basic properties ──

    void testStaticFieldFlag() {
        TestTree t;
        int hi = t.addStaticField("h", "base");
        QCOMPARE(t.tree.nodes[hi].isStatic, true);
        QCOMPARE(t.tree.nodes[hi].offsetExpr, QStringLiteral("base"));
    }

    void testRegularFieldNotStatic() {
        TestTree t;
        int fi = t.addField("x", NodeKind::UInt32, 0);
        QCOMPARE(t.tree.nodes[fi].isStatic, false);
        QCOMPARE(t.tree.nodes[fi].offsetExpr, QString());
    }

    void testStaticFieldIsChild() {
        TestTree t;
        int hi = t.addStaticField("h", "base");
        QCOMPARE(t.tree.nodes[hi].parentId, t.rootId);
        auto children = t.tree.childrenOf(t.rootId);
        QVERIFY(children.contains(hi));
    }

    // ── JSON serialization ──

    void testStaticFieldJsonRoundTrip() {
        TestTree t;
        t.addField("e_lfanew", NodeKind::UInt32, 0x3C);
        t.addStaticField("nt_hdr", "base + e_lfanew", NodeKind::Struct);

        QJsonObject json = t.tree.toJson();
        NodeTree t2 = NodeTree::fromJson(json);

        QCOMPARE(t2.nodes.size(), 3);
        // root
        QCOMPARE(t2.nodes[0].isStatic, false);
        // field
        QCOMPARE(t2.nodes[1].isStatic, false);
        QCOMPARE(t2.nodes[1].name, QStringLiteral("e_lfanew"));
        // static field
        QCOMPARE(t2.nodes[2].isStatic, true);
        QCOMPARE(t2.nodes[2].offsetExpr, QStringLiteral("base + e_lfanew"));
        QCOMPARE(t2.nodes[2].name, QStringLiteral("nt_hdr"));
        QCOMPARE(t2.nodes[2].kind, NodeKind::Struct);
    }

    void testStaticFieldJsonBackwardCompat() {
        // Old JSON without isStatic should default to false
        NodeTree tree;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Old";
        root.parentId = 0;
        tree.addNode(root);

        QJsonObject json = tree.toJson();
        NodeTree t2 = NodeTree::fromJson(json);
        QCOMPARE(t2.nodes[0].isStatic, false);
        QCOMPARE(t2.nodes[0].offsetExpr, QString());
    }

    void testMultipleStaticFieldsRoundTrip() {
        TestTree t;
        t.addField("ptr", NodeKind::Pointer64, 0);
        t.addStaticField("h1", "base");
        t.addStaticField("h2", "base + ptr");
        t.addStaticField("h3", "base + 0x100");

        QJsonObject json = t.tree.toJson();
        NodeTree t2 = NodeTree::fromJson(json);

        int staticFieldCount = 0;
        for (const auto& n : t2.nodes)
            if (n.isStatic) staticFieldCount++;
        QCOMPARE(staticFieldCount, 3);
    }

    // ── Struct span exclusion ──

    void testStructSpanExcludesStaticFields() {
        TestTree t;
        t.addField("a", NodeKind::UInt32, 0);   // 0+4 = 4
        t.addField("b", NodeKind::UInt64, 4);   // 4+8 = 12
        t.addStaticField("h", "base");           // should NOT affect span

        QCOMPARE(t.tree.structSpan(t.rootId), 12);
    }

    void testStructSpanWithOnlyStaticFields() {
        TestTree t;
        t.addStaticField("h1", "base");
        t.addStaticField("h2", "base + 0x100");

        // No regular fields -> span = 0
        QCOMPARE(t.tree.structSpan(t.rootId), 0);
    }

    void testStructSpanMixedOrder() {
        // Static fields interleaved with regular fields
        TestTree t;
        t.addField("x", NodeKind::Float, 0);    // 0+4 = 4
        t.addStaticField("h1", "base");
        t.addField("y", NodeKind::Float, 4);    // 4+4 = 8
        t.addStaticField("h2", "base + x");
        t.addField("z", NodeKind::Float, 8);    // 8+4 = 12

        QCOMPARE(t.tree.structSpan(t.rootId), 12);
    }

    // ── Address expression evaluation ──

    void testExprBase() {
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            if (name == "base") { *ok = true; return 0x1000; }
            *ok = false; return 0;
        };
        auto r = AddressParser::evaluate("base", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, (uint64_t)0x1000);
    }

    void testExprBaseAddHex() {
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            if (name == "base") { *ok = true; return 0x1000; }
            *ok = false; return 0;
        };
        auto r = AddressParser::evaluate("base + 0x3C", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, (uint64_t)0x103C);
    }

    void testExprBaseAddField() {
        // Simulate: base=0x1000, e_lfanew value=0xE8
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            if (name == "base") { *ok = true; return 0x1000; }
            if (name == "e_lfanew") { *ok = true; return 0xE8; }
            *ok = false; return 0;
        };
        auto r = AddressParser::evaluate("base + e_lfanew", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, (uint64_t)0x10E8);
    }

    void testExprSubtraction() {
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            if (name == "base") { *ok = true; return 0x2000; }
            *ok = false; return 0;
        };
        auto r = AddressParser::evaluate("base - 0x10", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, (uint64_t)0x1FF0);
    }

    void testExprMultiplication() {
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            if (name == "base") { *ok = true; return 0x100; }
            if (name == "index") { *ok = true; return 3; }
            *ok = false; return 0;
        };
        auto r = AddressParser::evaluate("base + index * 8", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, (uint64_t)0x118);  // 0x100 + 3*8
    }

    void testExprUnresolvedIdentifier() {
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            if (name == "base") { *ok = true; return 0x1000; }
            *ok = false; return 0;
        };
        auto r = AddressParser::evaluate("base + unknown_field", 8, &cbs);
        QVERIFY(!r.ok);
    }

    void testExprPureHex() {
        auto r = AddressParser::evaluate("0x7FF600000000", 8, nullptr);
        QVERIFY(r.ok);
        QCOMPARE(r.value, (uint64_t)0x7FF600000000ULL);
    }

    void testExprParentheses() {
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            if (name == "base") { *ok = true; return 0x1000; }
            if (name == "offset") { *ok = true; return 0x10; }
            *ok = false; return 0;
        };
        auto r = AddressParser::evaluate("(base + offset) * 2", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, (uint64_t)0x2020);  // (0x1000 + 0x10) * 2
    }

    void testExprEmptyString() {
        auto r = AddressParser::evaluate("", 8, nullptr);
        QVERIFY(!r.ok);
    }

    // ── Static field with BufferProvider (simulates live resolution) ──

    void testStaticFieldResolveFromBuffer() {
        // Build tree: struct with UInt32 "offset_field" at +0x10
        // Static field expression: "base + offset_field"
        // Buffer has 0x000000E8 at address 0x10
        QByteArray data(64, '\0');
        // Write 0xE8 at offset 0x10 (little-endian uint32)
        data[0x10] = (char)0xE8;
        data[0x11] = 0;
        data[0x12] = 0;
        data[0x13] = 0;

        BufferProvider prov(data);

        // Build resolver mimicking compose.cpp's makeResolver
        uint64_t baseAddr = 0;  // buffer starts at 0
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [&prov, baseAddr](const QString& name, bool* ok) -> uint64_t {
            if (name == "base") { *ok = true; return baseAddr; }
            if (name == "offset_field") {
                uint64_t addr = baseAddr + 0x10;
                if (prov.isReadable(addr, 4)) {
                    *ok = true;
                    return (uint64_t)prov.readU32(addr);
                }
            }
            *ok = false; return 0;
        };

        auto r = AddressParser::evaluate("base + offset_field", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, (uint64_t)0xE8);  // 0 + 0xE8
    }

    void testStaticFieldResolvePointerChain() {
        // Buffer: addr 0x00 has pointer to 0x20, addr 0x20 has pointer to 0x40
        QByteArray data(64, '\0');
        // Write pointer at 0x00 -> 0x20 (little-endian uint64)
        data[0x00] = 0x20;
        // Write pointer at 0x20 -> 0x40
        data[0x20] = 0x40;

        BufferProvider prov(data);

        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            if (name == "base") { *ok = true; return 0; }
            *ok = false; return 0;
        };
        cbs.readPointer = [&prov](uint64_t addr, bool* ok) -> uint64_t {
            if (prov.isReadable(addr, 8)) {
                *ok = true;
                return prov.readU64(addr);
            }
            *ok = false; return 0;
        };

        // [base] = deref pointer at base (0x00) -> 0x20
        auto r = AddressParser::evaluate("[base]", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, (uint64_t)0x20);

        // [[base]] = double deref: [0x00]->0x20, [0x20]->0x40
        auto r2 = AddressParser::evaluate("[[base]]", 8, &cbs);
        QVERIFY(r2.ok);
        QCOMPARE(r2.value, (uint64_t)0x40);
    }

    // ── Compose output ──

    void testComposeStaticFieldHeader() {
        TestTree t;
        t.addField("x", NodeKind::Float, 0);
        t.addStaticField("h", "base");

        NullProvider prov;
        ComposeResult result = compose(t.tree, prov);

        // Static field header should contain "static" keyword
        bool foundStatic = false;
        QStringList lines = result.text.split('\n');
        for (const auto& line : lines) {
            if (line.contains(QStringLiteral("static ")) && line.contains(QStringLiteral("{"))) {
                foundStatic = true;
                break;
            }
        }
        QVERIFY2(foundStatic, "Static field header line not found in compose output");
    }

    void testComposeStaticFieldLine() {
        TestTree t;
        t.addField("x", NodeKind::Float, 0);
        t.addStaticField("h", "base");

        NullProvider prov;
        ComposeResult result = compose(t.tree, prov);

        // Find the static field line and check its meta
        bool foundStaticField = false;
        for (const auto& lm : result.meta) {
            if (lm.isStaticLine) {
                foundStaticField = true;
                break;
            }
        }
        QVERIFY2(foundStaticField, "Static field line metadata not found in compose output");
    }

    void testComposeNoStaticFieldsWhenCollapsed() {
        // Use a non-root struct to test collapsed behavior
        // (root structs are always expanded via isRootHeader)
        NodeTree tree;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node child;
        child.kind = NodeKind::Struct;
        child.name = "Child";
        child.parentId = rootId;
        child.offset = 0;
        child.collapsed = true;  // collapsed child struct
        int ci = tree.addNode(child);
        uint64_t childId = tree.nodes[ci].id;

        Node f;
        f.kind = NodeKind::Float;
        f.name = "x";
        f.parentId = childId;
        f.offset = 0;
        tree.addNode(f);

        Node sf;
        sf.kind = NodeKind::Hex64;
        sf.name = "h";
        sf.parentId = childId;
        sf.offset = 0;
        sf.isStatic = true;
        sf.offsetExpr = QStringLiteral("base");
        tree.addNode(sf);

        NullProvider prov;
        ComposeResult result = compose(tree, prov);

        // When collapsed, no static field lines should appear
        for (const auto& lm : result.meta)
            QVERIFY2(!lm.isStaticLine,
                     "Static field line should not appear when struct is collapsed");
    }

    void testComposeStaticFieldExprDisplay() {
        TestTree t;
        t.addField("offset", NodeKind::UInt32, 0);
        t.addStaticField("target", "base + offset");

        NullProvider prov;
        ComposeResult result = compose(t.tree, prov);

        // Static field line should contain the expression text
        bool foundExpr = false;
        QStringList lines = result.text.split('\n');
        for (const auto& line : lines) {
            if (line.contains(QStringLiteral("base + offset"))) {
                foundExpr = true;
                break;
            }
        }
        QVERIFY2(foundExpr, "Static field expression not found in compose output");
    }

    void testComposeStaticFieldsAfterRegularFields() {
        TestTree t;
        t.addField("a", NodeKind::UInt32, 0);
        t.addField("b", NodeKind::UInt64, 4);
        t.addStaticField("h", "base");

        NullProvider prov;
        ComposeResult result = compose(t.tree, prov);

        // Find meta indices: last regular field vs first static field line
        int lastFieldMeta = -1;
        int firstStaticFieldMeta = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].lineKind == LineKind::Field
                && !result.meta[i].isStaticLine)
                lastFieldMeta = i;
            if (result.meta[i].isStaticLine && firstStaticFieldMeta < 0)
                firstStaticFieldMeta = i;
        }

        QVERIFY(lastFieldMeta >= 0);
        QVERIFY(firstStaticFieldMeta >= 0);
        QVERIFY2(firstStaticFieldMeta > lastFieldMeta,
                 "Static field lines must come after all regular fields");
    }

    // ── Node byteSize for static fields ──

    void testStaticFieldByteSize() {
        // Static field nodes should still report their kind's byte size
        Node h;
        h.kind = NodeKind::Hex64;
        h.isStatic = true;
        h.offsetExpr = "base";
        QCOMPARE(h.byteSize(), 8);

        h.kind = NodeKind::Struct;
        QCOMPARE(h.byteSize(), 0);  // struct static fields have 0 size (children determine)
    }

    // ── Children ordering ──

    void testChildrenOfIncludesStaticFields() {
        TestTree t;
        t.addField("a", NodeKind::UInt32, 0);
        int hi = t.addStaticField("h", "base");

        auto children = t.tree.childrenOf(t.rootId);
        QCOMPARE(children.size(), 2);
        QVERIFY(children.contains(hi));
    }

    // ── Edge cases ──

    void testStaticFieldWithEmptyExpr() {
        TestTree t;
        Node h;
        h.kind = NodeKind::Hex64;
        h.name = "h";
        h.parentId = t.rootId;
        h.isStatic = true;
        h.offsetExpr = QString();  // empty expression
        int hi = t.tree.addNode(h);

        QCOMPARE(t.tree.nodes[hi].isStatic, true);
        QCOMPARE(t.tree.nodes[hi].offsetExpr, QString());

        // JSON round-trip should preserve empty expr
        QJsonObject json = t.tree.toJson();
        NodeTree t2 = NodeTree::fromJson(json);
        QCOMPARE(t2.nodes[hi].isStatic, true);
    }

    void testStaticFieldStructType() {
        // Static field can be a struct (pointing to a different address)
        TestTree t;
        int hi = t.addStaticField("nt_headers", "base + 0xE8", NodeKind::Struct);
        t.tree.nodes[hi].structTypeName = "IMAGE_NT_HEADERS";

        QCOMPARE(t.tree.nodes[hi].kind, NodeKind::Struct);
        QCOMPARE(t.tree.nodes[hi].isStatic, true);
        QCOMPARE(t.tree.nodes[hi].structTypeName, QStringLiteral("IMAGE_NT_HEADERS"));
    }

    void testStaticFieldPointerType() {
        // Static field can be a pointer type
        TestTree t;
        int hi = t.addStaticField("indirect", "base + 0x20", NodeKind::Pointer64);

        QCOMPARE(t.tree.nodes[hi].kind, NodeKind::Pointer64);
        QCOMPARE(t.tree.nodes[hi].isStatic, true);
    }

    // ── Validate expression syntax ──

    void testExprValidate() {
        // Valid expressions
        QCOMPARE(AddressParser::validate("base"), QString());
        QCOMPARE(AddressParser::validate("base + 0x10"), QString());
        QCOMPARE(AddressParser::validate("0x1000"), QString());
        QCOMPARE(AddressParser::validate("(base + offset) * 2"), QString());

        // Invalid expressions
        QVERIFY(!AddressParser::validate("").isEmpty());
        QVERIFY(!AddressParser::validate("+ +").isEmpty());
        QVERIFY(!AddressParser::validate("(base").isEmpty());
    }
};

QTEST_MAIN(TestStaticFields)
#include "test_static_fields.moc"
