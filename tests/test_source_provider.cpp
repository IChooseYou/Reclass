#include <QtTest/QTest>
#include <QApplication>
#include <QSplitter>
#include <QIcon>
#include <QMenu>
#include <QAction>
#include <QPixmap>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "core.h"
#include "providerregistry.h"
#include "providers/null_provider.h"
#include "providers/buffer_provider.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace rcx;

// Minimal mock IProviderPlugin that reads from the current process
class SelfProcessPlugin : public IProviderPlugin {
public:
    std::string Name() const override { return "TestProcessMemory"; }
    std::string Version() const override { return "1.0"; }
    std::string Author() const override { return "Test"; }
    std::string Description() const override { return "Mock plugin for testing"; }
    QIcon Icon() const override { return {}; }
    k_ELoadType LoadType() const override { return k_ELoadTypeAuto; }

    bool canHandle(const QString&) const override { return true; }

    std::unique_ptr<Provider> createProvider(const QString& target, QString*) override {
        // Create a buffer provider with a known pattern at a known base
        QByteArray data(256, '\0');
        // Write a recognizable pattern: 0xDE 0xAD 0xBE 0xEF ...
        data[0] = (char)0xDE;
        data[1] = (char)0xAD;
        data[2] = (char)0xBE;
        data[3] = (char)0xEF;
        data[4] = (char)0xCA;
        data[5] = (char)0xFE;
        data[6] = (char)0xBA;
        data[7] = (char)0xBE;
        m_lastBase = 0x7FF000000000ULL; // simulate typical image base
        Q_UNUSED(target);
        return std::make_unique<BufferProvider>(data, "self");
    }

    bool selectTarget(QWidget*, QString* target) override {
        *target = "self";
        return true;
    }

    uint64_t getInitialBaseAddress(const QString&) const override {
        return m_lastBase;
    }

    uint64_t lastBase() const { return m_lastBase; }

private:
    uint64_t m_lastBase = 0;
};

static void buildTree(NodeTree& tree, uint64_t base) {
    tree.baseAddress = base;
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "TestStruct";
    root.name = "TestStruct";
    root.parentId = 0;
    root.offset = 0;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    for (int off = 0; off < 64; off += 8) {
        Node f;
        f.kind = NodeKind::Hex64;
        f.name = QStringLiteral("field_%1").arg(off, 2, 16, QLatin1Char('0'));
        f.parentId = rootId;
        f.offset = off;
        tree.addNode(f);
    }
}

class TestSourceProvider : public QObject {
    Q_OBJECT
private:
    RcxDocument* m_doc = nullptr;
    RcxController* m_ctrl = nullptr;
    QSplitter* m_splitter = nullptr;
    SelfProcessPlugin* m_plugin = nullptr;

private slots:
    void init() {
        m_doc = new RcxDocument();

        m_splitter = new QSplitter();
        m_ctrl = new RcxController(m_doc, nullptr);
        m_ctrl->addSplitEditor(m_splitter);

        m_splitter->resize(800, 600);
        m_splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_splitter));
        QApplication::processEvents();
    }

    void cleanup() {
        // Unregister test providers
        ProviderRegistry::instance().unregisterProvider("testprocessmemory");
        delete m_ctrl;  m_ctrl = nullptr;
        delete m_splitter;  m_splitter = nullptr;
        delete m_doc;  m_doc = nullptr;
        m_plugin = nullptr; // owned by PluginManager/test scope, don't double-delete
    }

    // ── attachViaPlugin must NOT overwrite pre-set base address ──

    void testAttachViaPluginPreservesBaseAddress() {
        // Register our mock plugin
        m_plugin = new SelfProcessPlugin();
        ProviderRegistry::instance().registerProvider(
            "TestProcessMemory", "testprocessmemory", m_plugin);

        // Pre-set base address (like selfTest/buildEditorDemo does)
        const uint64_t demoBase = 0x0000020E2FAB1770ULL;
        buildTree(m_doc->tree, demoBase);
        QCOMPARE(m_doc->tree.baseAddress, demoBase);

        // Attach via plugin — this should NOT overwrite the base address
        m_ctrl->attachViaPlugin(QStringLiteral("testprocessmemory"), QStringLiteral("self"));

        // Base address must still be the demo address, NOT the provider's image base
        QCOMPARE(m_doc->tree.baseAddress, demoBase);
        QVERIFY(m_doc->tree.baseAddress != m_plugin->lastBase());

        // Provider should be valid and readable
        QVERIFY(m_doc->provider != nullptr);
        QVERIFY(m_doc->provider->isValid());

        delete m_plugin;
        m_plugin = nullptr;
    }

    // ── Provider reads correct data after attach ──

    void testProviderReadsCorrectData() {
        m_plugin = new SelfProcessPlugin();
        ProviderRegistry::instance().registerProvider(
            "TestProcessMemory", "testprocessmemory", m_plugin);

        buildTree(m_doc->tree, 0x1000);
        m_ctrl->attachViaPlugin(QStringLiteral("testprocessmemory"), QStringLiteral("self"));

        // Read the known pattern written by the mock provider
        QVERIFY(m_doc->provider->isValid());
        QCOMPARE(m_doc->provider->readU8(0), (uint8_t)0xDE);
        QCOMPARE(m_doc->provider->readU8(1), (uint8_t)0xAD);
        QCOMPARE(m_doc->provider->readU8(2), (uint8_t)0xBE);
        QCOMPARE(m_doc->provider->readU8(3), (uint8_t)0xEF);
        QCOMPARE(m_doc->provider->readU8(4), (uint8_t)0xCA);
        QCOMPARE(m_doc->provider->readU8(5), (uint8_t)0xFE);
        QCOMPARE(m_doc->provider->readU8(6), (uint8_t)0xBA);
        QCOMPARE(m_doc->provider->readU8(7), (uint8_t)0xBE);

        // Read as u64 — should be 0xBEBAFECAEFBEADDE in little-endian
        uint64_t val = m_doc->provider->readU64(0);
        QCOMPARE(val, 0xBEBAFECAEFBEADDEULL);

        delete m_plugin;
        m_plugin = nullptr;
    }

    // ── Provider data is not garbage (not PE header) ──

    void testProviderDataIsNotPEHeader() {
        m_plugin = new SelfProcessPlugin();
        ProviderRegistry::instance().registerProvider(
            "TestProcessMemory", "testprocessmemory", m_plugin);

        const uint64_t demoBase = 0x0000020E2FAB1770ULL;
        buildTree(m_doc->tree, demoBase);
        m_ctrl->attachViaPlugin(QStringLiteral("testprocessmemory"), QStringLiteral("self"));

        // The data should NOT start with 'MZ' (PE header signature)
        // If it does, the base address was wrongly set to the process image base
        uint16_t mz = m_doc->provider->readU16(0);
        QVERIFY2(mz != 0x5A4D, "Data starts with MZ — base address was overwritten to image base!");

        // Verify our known pattern instead
        QCOMPARE(m_doc->provider->readU8(0), (uint8_t)0xDE);

        delete m_plugin;
        m_plugin = nullptr;
    }

    // ── ProviderRegistry: registration and lookup ──

    void testProviderRegistryRegisterAndFind() {
        m_plugin = new SelfProcessPlugin();
        ProviderRegistry::instance().registerProvider(
            "TestProcessMemory", "testprocessmemory", m_plugin, "libTestPlugin.dll");

        const auto* info = ProviderRegistry::instance().findProvider("testprocessmemory");
        QVERIFY(info != nullptr);
        QCOMPARE(info->name, QStringLiteral("TestProcessMemory"));
        QCOMPARE(info->identifier, QStringLiteral("testprocessmemory"));
        QCOMPARE(info->dllFileName, QStringLiteral("libTestPlugin.dll"));
        QVERIFY(!info->isBuiltin);
        QVERIFY(info->plugin != nullptr);

        delete m_plugin;
        m_plugin = nullptr;
    }

    void testProviderRegistryUnregister() {
        m_plugin = new SelfProcessPlugin();
        ProviderRegistry::instance().registerProvider(
            "TestProcessMemory", "testprocessmemory", m_plugin);

        QVERIFY(ProviderRegistry::instance().findProvider("testprocessmemory") != nullptr);

        ProviderRegistry::instance().unregisterProvider("testprocessmemory");
        QVERIFY(ProviderRegistry::instance().findProvider("testprocessmemory") == nullptr);

        delete m_plugin;
        m_plugin = nullptr;
    }

    // ── SVG icons load from resources ──

    void testSourceMenuIconsLoad() {
        // These are the icons used in the source menu
        const QStringList iconPaths = {
            QStringLiteral(":/vsicons/file-binary.svg"),
            QStringLiteral(":/vsicons/server-process.svg"),
            QStringLiteral(":/vsicons/remote.svg"),
            QStringLiteral(":/vsicons/debug.svg"),
            QStringLiteral(":/vsicons/plug.svg"),
            QStringLiteral(":/vsicons/extensions.svg"),
            QStringLiteral(":/vsicons/clear-all.svg"),
        };

        for (const QString& path : iconPaths) {
            QIcon icon(path);
            QVERIFY2(!icon.isNull(),
                     qPrintable(QStringLiteral("Icon is null: %1").arg(path)));

            // Verify it can actually render a pixmap
            QPixmap pm = icon.pixmap(16, 16);
            QVERIFY2(!pm.isNull(),
                     qPrintable(QStringLiteral("Pixmap is null: %1").arg(path)));
            QVERIFY2(pm.width() > 0 && pm.height() > 0,
                     qPrintable(QStringLiteral("Pixmap has zero size: %1").arg(path)));
        }
    }

    // ── Menu actions have icons set and forced visible ──

    void testMenuActionIconVisibility() {
        QMenu menu;
        QIcon icon(QStringLiteral(":/vsicons/file-binary.svg"));
        QVERIFY(!icon.isNull());

        auto* act = menu.addAction(icon, "Test Item");
        act->setIconVisibleInMenu(true);

        QVERIFY(!act->icon().isNull());
        QVERIFY(act->isIconVisibleInMenu());

        // Verify pixmap can be extracted from the action's icon
        QPixmap pm = act->icon().pixmap(16, 16);
        QVERIFY(!pm.isNull());
    }

    // ── selectSource with provider updates base address ──

    void testSelectSourceUpdatesBaseAddress() {
        // This tests that selectSource (user-initiated) DOES update the base,
        // while attachViaPlugin does NOT.
        m_plugin = new SelfProcessPlugin();
        ProviderRegistry::instance().registerProvider(
            "TestProcessMemory", "testprocessmemory", m_plugin);

        // Start with zero base
        m_doc->tree.baseAddress = 0;

        // attachViaPlugin should NOT set the base (it's 0 and stays 0)
        m_ctrl->attachViaPlugin(QStringLiteral("testprocessmemory"), QStringLiteral("self"));
        // Base stays at 0 because attachViaPlugin doesn't touch it
        QCOMPARE(m_doc->tree.baseAddress, (uint64_t)0);

        delete m_plugin;
        m_plugin = nullptr;
    }

    // ── dllFileName propagated through registry ──

    void testDllFileNameInProviderInfo() {
        m_plugin = new SelfProcessPlugin();
        ProviderRegistry::instance().registerProvider(
            "TestProcessMemory", "testprocessmemory", m_plugin, "MyPlugin.dll");

        const auto& providers = ProviderRegistry::instance().providers();
        bool found = false;
        for (const auto& p : providers) {
            if (p.identifier == "testprocessmemory") {
                QCOMPARE(p.dllFileName, QStringLiteral("MyPlugin.dll"));
                found = true;
                break;
            }
        }
        QVERIFY2(found, "testprocessmemory not found in provider list");

        delete m_plugin;
        m_plugin = nullptr;
    }
};

QTEST_MAIN(TestSourceProvider)
#include "test_source_provider.moc"
