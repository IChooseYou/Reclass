#include <QtTest/QTest>
#include "core.h"
#include "generator.h"
#include "imports/import_source.h"
#include "imports/import_reclass_xml.h"
#include "providers/provider.h"
#include "addressparser.h"
#include "iplugin.h"
#include "processpicker.h"

// Include RPC protocol for header size test
#include "rcx_rpc_protocol.h"

using namespace rcx;

// ── Test provider that reports a configurable pointer size ──

class TestProvider32 : public Provider {
public:
    QByteArray m_data;
    int m_ptrSize;

    TestProvider32(int ptrSize, int dataSize = 256)
        : m_ptrSize(ptrSize), m_data(dataSize, '\0') {}

    bool read(uint64_t addr, void* buf, int len) const override {
        if ((int)addr + len > m_data.size()) {
            memset(buf, 0, len);
            return false;
        }
        memcpy(buf, m_data.constData() + addr, len);
        return true;
    }
    int size() const override { return m_data.size(); }
    int pointerSize() const override { return m_ptrSize; }
};

class Test32BitSupport : public QObject {
    Q_OBJECT

private slots:

    // ── 1. Provider::pointerSize() default is 8 ──

    void providerDefaultPointerSize() {
        // NullProvider inherits default
        NullProvider np;
        QCOMPARE(np.pointerSize(), 8);
    }

    void providerCustomPointerSize() {
        TestProvider32 p32(4);
        QCOMPARE(p32.pointerSize(), 4);
        TestProvider32 p64(8);
        QCOMPARE(p64.pointerSize(), 8);
    }

    // ── 2. NodeTree pointerSize field ──

    void nodeTreeDefaultPointerSize() {
        NodeTree tree;
        QCOMPARE(tree.pointerSize, 8);
    }

    void nodeTreePointerSizeRoundTrip() {
        // 32-bit tree persists to JSON and back
        NodeTree tree;
        tree.pointerSize = 4;
        tree.baseAddress = 0x00400000;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Test";
        root.structTypeName = "Test";
        root.parentId = 0;
        tree.addNode(root);

        QJsonObject json = tree.toJson();
        QCOMPARE(json["pointerSize"].toInt(), 4);

        NodeTree restored = NodeTree::fromJson(json);
        QCOMPARE(restored.pointerSize, 4);
    }

    void nodeTreePointerSizeOmittedForDefault() {
        // 64-bit (default) should not write pointerSize key
        NodeTree tree;
        tree.pointerSize = 8;
        QJsonObject json = tree.toJson();
        QVERIFY(!json.contains("pointerSize"));
    }

    void nodeTreePointerSizeDefaultOnMissing() {
        // Legacy JSON without pointerSize should default to 8
        QJsonObject json;
        json["baseAddress"] = "400000";
        json["nextId"] = "1";
        json["nodes"] = QJsonArray();

        NodeTree tree = NodeTree::fromJson(json);
        QCOMPARE(tree.pointerSize, 8);
    }

    // ── 3. Source import respects pointer size ──

    void sourceImport64bitDefault() {
        QString src = R"(
            struct Test {
                PVOID ptr;     // 0x0
                SIZE_T sz;     // 0x8
            };
        )";
        QString error;
        NodeTree tree = importFromSource(src, &error);
        QVERIFY2(!tree.nodes.isEmpty(), qPrintable(error));

        // Default: 64-bit pointers
        bool foundPtr64 = false, foundUInt64 = false;
        for (const auto& n : tree.nodes) {
            if (n.name == "ptr" && n.kind == NodeKind::Pointer64) foundPtr64 = true;
            if (n.name == "sz" && n.kind == NodeKind::UInt64) foundUInt64 = true;
        }
        QVERIFY2(foundPtr64, "PVOID should be Pointer64 in 64-bit mode");
        QVERIFY2(foundUInt64, "SIZE_T should be UInt64 in 64-bit mode");
    }

    void sourceImport32bit() {
        QString src = R"(
            struct Test {
                PVOID ptr;     // 0x0
                SIZE_T sz;     // 0x4
            };
        )";
        QString error;
        NodeTree tree = importFromSource(src, &error, 4);
        QVERIFY2(!tree.nodes.isEmpty(), qPrintable(error));

        QCOMPARE(tree.pointerSize, 4);

        bool foundPtr32 = false, foundUInt32 = false;
        for (const auto& n : tree.nodes) {
            if (n.name == "ptr" && n.kind == NodeKind::Pointer32) foundPtr32 = true;
            if (n.name == "sz" && n.kind == NodeKind::UInt32) foundUInt32 = true;
        }
        QVERIFY2(foundPtr32, "PVOID should be Pointer32 in 32-bit mode");
        QVERIFY2(foundUInt32, "SIZE_T should be UInt32 in 32-bit mode");
    }

    void sourceImportPointerField32bit() {
        // A generic pointer (void* field) should become Pointer32 in 32-bit mode
        QString src = R"(
            struct Test {
                void* ptr;     // 0x0
                int value;     // 0x4
            };
        )";
        QString error;
        NodeTree tree = importFromSource(src, &error, 4);
        QVERIFY2(!tree.nodes.isEmpty(), qPrintable(error));

        bool foundPtr32 = false;
        for (const auto& n : tree.nodes) {
            if (n.name == "ptr" && n.kind == NodeKind::Pointer32) foundPtr32 = true;
        }
        QVERIFY2(foundPtr32, "void* should be Pointer32 in 32-bit mode");
    }

    void sourceImportPointerSizeTypes32bit() {
        // All pointer-size-dependent types should be 32-bit
        QString src = R"(
            struct Test {
                HANDLE h;        // 0x0
                ULONG_PTR up;    // 0x4
                LONG_PTR lp;     // 0x8
                uintptr_t uip;   // 0xC
                intptr_t ip;     // 0x10
                size_t sz;       // 0x14
                LPVOID lv;       // 0x18
                PCHAR pc;        // 0x1C
            };
        )";
        QString error;
        NodeTree tree = importFromSource(src, &error, 4);
        QVERIFY2(!tree.nodes.isEmpty(), qPrintable(error));

        for (const auto& n : tree.nodes) {
            if (n.parentId == 0) continue; // skip root struct
            int sz = n.byteSize();
            QVERIFY2(sz == 4,
                qPrintable(QString("Field '%1' has size %2, expected 4")
                    .arg(n.name).arg(sz)));
        }
    }

    // ── 4. Generator respects pointer size ──

    void generatorPointer32NativeVoidStar() {
        // For 32-bit target, untyped Pointer32 should emit void*
        NodeTree tree;
        tree.pointerSize = 4;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Test";
        root.structTypeName = "Test";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node p;
        p.kind = NodeKind::Pointer32;
        p.name = "ptr";
        p.parentId = rootId;
        p.offset = 0;
        tree.addNode(p);

        QString result = renderCpp(tree, rootId);
        QVERIFY2(result.contains("void* ptr;"),
            qPrintable("32-bit native Pointer32 should emit void*:\n" + result));
    }

    void generatorPointer64NativeVoidStar() {
        // For 64-bit target (default), untyped Pointer64 should emit void*
        NodeTree tree;
        tree.pointerSize = 8;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Test";
        root.structTypeName = "Test";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node p;
        p.kind = NodeKind::Pointer64;
        p.name = "ptr";
        p.parentId = rootId;
        p.offset = 0;
        tree.addNode(p);

        QString result = renderCpp(tree, rootId);
        QVERIFY2(result.contains("void* ptr;"),
            qPrintable("64-bit native Pointer64 should emit void*:\n" + result));
    }

    void generatorPointer32CrossSizeInt() {
        // For 64-bit target, Pointer32 should emit uint32_t (cross-size)
        NodeTree tree;
        tree.pointerSize = 8;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Test";
        root.structTypeName = "Test";
        root.parentId = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node p;
        p.kind = NodeKind::Pointer32;
        p.name = "ptr32";
        p.parentId = rootId;
        p.offset = 0;
        tree.addNode(p);

        QString result = renderCpp(tree, rootId);
        QVERIFY2(result.contains("uint32_t ptr32;"),
            qPrintable("Cross-size Pointer32 on 64-bit target should emit uint32_t:\n" + result));
    }

    void generatorTypedPointerBothSizes() {
        // Typed pointers (with refId) always emit struct X* regardless of size
        NodeTree tree;
        tree.pointerSize = 4;

        Node target;
        target.kind = NodeKind::Struct;
        target.name = "Target";
        target.structTypeName = "TargetData";
        target.parentId = 0;
        int ti = tree.addNode(target);
        uint64_t targetId = tree.nodes[ti].id;

        Node main;
        main.kind = NodeKind::Struct;
        main.name = "Main";
        main.structTypeName = "MainStruct";
        main.parentId = 0;
        int mi = tree.addNode(main);
        uint64_t mainId = tree.nodes[mi].id;

        Node p;
        p.kind = NodeKind::Pointer32;
        p.name = "pTarget";
        p.parentId = mainId;
        p.offset = 0;
        p.refId = targetId;
        tree.addNode(p);

        QString result = renderCpp(tree, mainId);
        QVERIFY2(result.contains("struct TargetData* pTarget;"),
            qPrintable("Typed Pointer32 should emit struct X*:\n" + result));
    }

    // ── 5. RPC protocol header has pointerSize field ──

    void rpcHeaderHasPointerSize() {
        // Verify the field exists and header is still 4096 bytes
        RcxRpcHeader hdr = {};
        hdr.pointerSize = 4;
        QCOMPARE(hdr.pointerSize, (uint32_t)4);
        QCOMPARE((int)sizeof(RcxRpcHeader), RCX_RPC_HEADER_SIZE);
    }

    // ── 6. PluginProcessInfo has is32Bit field ──

    void pluginProcessInfoIs32Bit() {
        PluginProcessInfo info;
        QCOMPARE(info.is32Bit, false); // default

        info.is32Bit = true;
        QCOMPARE(info.is32Bit, true);
    }

    // ── 7. ProcessInfo has is32Bit field ──

    void processInfoIs32Bit() {
        ProcessInfo info;
        QCOMPARE(info.is32Bit, false); // default

        info.is32Bit = true;
        QCOMPARE(info.is32Bit, true);
    }

    // ── 8. AddressParser readPointer uses correct size ──

    void addressParserReadPointer32bit() {
        // Create a test provider with a 32-bit pointer at address 0
        TestProvider32 prov(4, 16);
        uint32_t val32 = 0xDEADBEEF;
        memcpy(prov.m_data.data(), &val32, 4);
        // Write garbage in bytes 4-7 to verify we only read 4 bytes
        memset(prov.m_data.data() + 4, 0xFF, 4);

        AddressParserCallbacks cbs;
        int ptrSz = prov.pointerSize();
        auto* p = &prov;
        cbs.readPointer = [p, ptrSz](uint64_t addr, bool* ok) -> uint64_t {
            uint64_t val = 0;
            *ok = p->read(addr, &val, ptrSz);
            return val;
        };

        auto result = AddressParser::evaluate("[0]", ptrSz, &cbs);
        QVERIFY(result.ok);
        QCOMPARE(result.value, (uint64_t)0xDEADBEEF);
    }

    void addressParserReadPointer64bit() {
        TestProvider32 prov(8, 16);
        uint64_t val64 = 0x0000DEADBEEF1234ULL;
        memcpy(prov.m_data.data(), &val64, 8);

        AddressParserCallbacks cbs;
        int ptrSz = prov.pointerSize();
        auto* p = &prov;
        cbs.readPointer = [p, ptrSz](uint64_t addr, bool* ok) -> uint64_t {
            uint64_t val = 0;
            *ok = p->read(addr, &val, ptrSz);
            return val;
        };

        auto result = AddressParser::evaluate("[0]", ptrSz, &cbs);
        QVERIFY(result.ok);
        QCOMPARE(result.value, (uint64_t)0x0000DEADBEEF1234ULL);
    }

    // ── 9. Source import HANDLE/LPVOID remain 64-bit by default ──

    void sourceImportBackwardsCompat() {
        QString src = R"(
            struct Test {
                HANDLE h;      // 0x0
                LPVOID lv;     // 0x8
            };
        )";
        QString error;
        NodeTree tree = importFromSource(src, &error);
        QVERIFY(!tree.nodes.isEmpty());

        // Default (no pointerSize arg) should be 64-bit
        for (const auto& n : tree.nodes) {
            if (n.name == "h") QCOMPARE(n.kind, NodeKind::Pointer64);
            if (n.name == "lv") QCOMPARE(n.kind, NodeKind::Pointer64);
        }
    }

    // ── 10. Full round-trip: 32-bit import → generate → verify ──

    void fullRoundTrip32bit() {
        QString src = R"(
            struct EPROCESS_32 {
                PVOID Pcb;         // 0x0
                HANDLE UniqueProcessId;  // 0x4
                DWORD ActiveProcessLinks;  // 0x8
            };
        )";
        QString error;
        NodeTree tree = importFromSource(src, &error, 4);
        QVERIFY2(!tree.nodes.isEmpty(), qPrintable(error));
        QCOMPARE(tree.pointerSize, 4);

        // Find the root struct
        uint64_t rootId = 0;
        for (const auto& n : tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                rootId = n.id;
                break;
            }
        }
        QVERIFY(rootId != 0);

        // Generate C++ code
        QString code = renderCpp(tree, rootId);
        QVERIFY2(code.contains("void* Pcb;"),
            qPrintable("PVOID in 32-bit should generate void*:\n" + code));
        QVERIFY2(code.contains("void* UniqueProcessId;"),
            qPrintable("HANDLE in 32-bit should generate void*:\n" + code));

        // Verify JSON persistence
        QJsonObject json = tree.toJson();
        QCOMPARE(json["pointerSize"].toInt(), 4);
        NodeTree restored = NodeTree::fromJson(json);
        QCOMPARE(restored.pointerSize, 4);
    }
};

QTEST_MAIN(Test32BitSupport)
#include "test_32bit_support.moc"
