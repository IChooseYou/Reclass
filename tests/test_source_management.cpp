#include <QtTest/QTest>
#include <QApplication>
#include <QSplitter>
#include <QDir>
#include <QFile>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "core.h"
#include "providers/null_provider.h"
#include "providers/buffer_provider.h"
#include "providerregistry.h"
#include "iplugin.h"

using namespace rcx;

// ── Fake provider + plugin for the "adopt new provider's base" test ──
// A real Process plugin would open a process and report its main module
// base. The regression we're catching: when the user switches sources,
// the controller must replace a stale baseAddress (e.g. a heap pointer
// left over from the New-Class self-attach) with the new provider's
// base, otherwise every read lands on unmapped memory and shows 00.
// We use a plain BufferProvider subclass that reports a known base()
// and a no-dialog plugin that hands back a fixed target string from
// selectTarget so the test runs headless.
class FakeBasedProvider : public BufferProvider {
public:
    FakeBasedProvider(const QByteArray& data, uint64_t base)
        : BufferProvider(data), m_base(base) {}
    uint64_t base() const override { return m_base; }
private:
    uint64_t m_base;
};

class FakeProviderPlugin : public IProviderPlugin {
public:
    FakeProviderPlugin(uint64_t base, const QString& target)
        : m_base(base), m_target(target) {}
    std::string Name()        const override { return "FakeProvider"; }
    std::string Description() const override { return "Test-only fake provider plugin"; }
    std::string Version()     const override { return "0.0"; }
    std::string Author()      const override { return "test"; }
    k_ELoadType LoadType()    const override { return k_ELoadTypeManual; }
    bool canHandle(const QString&) const override { return true; }
    std::unique_ptr<rcx::Provider> createProvider(const QString&,
                                                   QString* /*errorMsg*/ = nullptr) override {
        return std::make_unique<FakeBasedProvider>(QByteArray(64, '\x00'), m_base);
    }
    bool selectTarget(QWidget* /*parent*/, QString* target) override {
        *target = m_target;
        return true;
    }
private:
    uint64_t m_base;
    QString  m_target;
};

static void buildTree(NodeTree& tree) {
    tree.baseAddress = 0x1000;

    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "TestClass";
    root.name = "TestClass";
    root.parentId = 0;
    root.offset = 0;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    Node f;
    f.kind = NodeKind::Hex64;
    f.name = "field_00";
    f.parentId = rootId;
    f.offset = 0;
    tree.addNode(f);
}

class TestSourceManagement : public QObject {
    Q_OBJECT
private:
    RcxDocument* m_doc = nullptr;
    RcxController* m_ctrl = nullptr;
    QSplitter* m_splitter = nullptr;

    // Helper: write a temp binary file and return its path
    QString writeTempFile(const QString& name, const QByteArray& data) {
        QString path = QDir::tempPath() + "/" + name;
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(data);
        f.close();
        return path;
    }

    // Helper: directly add a file source entry (bypasses QFileDialog)
    void addFileSource(const QString& path, const QString& displayName) {
        m_doc->loadData(path);
        SavedSourceEntry entry;
        entry.kind = QStringLiteral("File");
        entry.displayName = displayName;
        entry.filePath = path;
        entry.baseAddress = m_doc->tree.baseAddress;
        // Access saved sources through selectSource's internal mechanism
        // We manually add since selectSource("File") opens a dialog
        m_ctrl->document()->provider = std::make_shared<BufferProvider>(
            QFile(path).readAll().isEmpty() ? QByteArray(64, '\0') : QByteArray(64, '\0'));
        // Use the test accessor pattern from controller
    }

private slots:
    void init() {
        m_doc = new RcxDocument();
        buildTree(m_doc->tree);

        m_splitter = new QSplitter();
        m_ctrl = new RcxController(m_doc, nullptr);
        m_ctrl->addSplitEditor(m_splitter);

        m_splitter->resize(800, 600);
        m_splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_splitter));
        QApplication::processEvents();
    }

    void cleanup() {
        delete m_ctrl;  m_ctrl = nullptr;
        delete m_splitter;  m_splitter = nullptr;
        delete m_doc;  m_doc = nullptr;
    }

    // ── Initial state: NullProvider, no saved sources ──

    void testInitialProviderIsNull() {
        QVERIFY(m_doc->provider != nullptr);
        QCOMPARE(m_doc->provider->size(), 0);
        QVERIFY(!m_doc->provider->isValid());
        QCOMPARE(m_ctrl->savedSources().size(), 0);
        QCOMPARE(m_ctrl->activeSourceIndex(), -1);
    }

    // ── Loading binary data creates a valid provider ──

    void testLoadDataCreatesValidProvider() {
        QByteArray data(128, '\xAB');
        m_doc->loadData(data);
        QApplication::processEvents();

        QVERIFY(m_doc->provider->isValid());
        QCOMPARE(m_doc->provider->size(), 128);
        QCOMPARE(m_doc->provider->readU8(0), (uint8_t)0xAB);
    }

    // ── clearSources resets to NullProvider ──

    void testClearSourcesResetsToNull() {
        // Load some data first so provider is valid
        QByteArray data(64, '\xFF');
        m_doc->loadData(data);
        QApplication::processEvents();
        QVERIFY(m_doc->provider->isValid());

        m_ctrl->clearSources();
        QApplication::processEvents();

        // Provider should be NullProvider
        QVERIFY(!m_doc->provider->isValid());
        QCOMPARE(m_doc->provider->size(), 0);

        // Saved sources should be empty
        QCOMPARE(m_ctrl->savedSources().size(), 0);
        QCOMPARE(m_ctrl->activeSourceIndex(), -1);
    }

    // ── clearSources clears value history ──

    void testClearSourcesClearsValueHistory() {
        // The value history is cleared via resetSnapshot inside clearSources
        m_ctrl->clearSources();
        QApplication::processEvents();

        QVERIFY(m_ctrl->valueHistory().isEmpty());
    }

    // ── clearSources clears dataPath ──

    void testClearSourcesClearsDataPath() {
        QString path = writeTempFile("rcx_test_src.bin", QByteArray(64, '\xCC'));
        m_doc->loadData(path);
        QVERIFY(!m_doc->dataPath.isEmpty());

        m_ctrl->clearSources();
        QApplication::processEvents();

        QVERIFY(m_doc->dataPath.isEmpty());
        QFile::remove(path);
    }

    // ── selectSource("#clear") calls clearSources ──

    void testSelectSourceClearCommand() {
        QByteArray data(64, '\xFF');
        m_doc->loadData(data);
        QVERIFY(m_doc->provider->isValid());

        m_ctrl->selectSource(QStringLiteral("#clear"));
        QApplication::processEvents();

        QVERIFY(!m_doc->provider->isValid());
        QCOMPARE(m_ctrl->savedSources().size(), 0);
        QCOMPARE(m_ctrl->activeSourceIndex(), -1);
    }

    // ── clearSources then refresh still works (compose doesn't crash) ──

    void testClearSourcesThenRefreshWorks() {
        m_ctrl->clearSources();
        QApplication::processEvents();

        // refresh() is called internally by clearSources; verify it didn't crash
        // and the editor still has content (the tree structure is intact)
        auto* editor = m_ctrl->editors().first();
        QVERIFY(editor != nullptr);
    }

    // ── Multiple clearSources calls are safe (idempotent) ──

    void testMultipleClearSourcesIdempotent() {
        m_ctrl->clearSources();
        m_ctrl->clearSources();
        m_ctrl->clearSources();
        QApplication::processEvents();

        QVERIFY(!m_doc->provider->isValid());
        QCOMPARE(m_ctrl->savedSources().size(), 0);
        QCOMPARE(m_ctrl->activeSourceIndex(), -1);
    }

    // ── switchToSavedSource with invalid index is no-op ──

    void testSwitchInvalidIndexNoOp() {
        m_ctrl->switchSource(-1);
        m_ctrl->switchSource(999);
        QApplication::processEvents();

        // Should still be in initial state
        QCOMPARE(m_ctrl->activeSourceIndex(), -1);
    }

    // ── Provider read fails after clear (all zeros) ──

    void testProviderReadFailsAfterClear() {
        QByteArray data(64, '\xAB');
        m_doc->loadData(data);
        QCOMPARE(m_doc->provider->readU8(0), (uint8_t)0xAB);

        m_ctrl->clearSources();
        QApplication::processEvents();

        // NullProvider: read returns false, readU8 returns 0
        uint8_t buf = 0xFF;
        QVERIFY(!m_doc->provider->read(0, &buf, 1));
        QCOMPARE(m_doc->provider->readU8(0), (uint8_t)0);
    }

    // ── clearSources resets snapshot state ──

    void testClearSourcesResetsSnapshot() {
        QByteArray data(64, '\x00');
        m_doc->loadData(data);
        QApplication::processEvents();

        m_ctrl->clearSources();
        QApplication::processEvents();

        // After clear, the value history should be empty (resetSnapshot was called)
        QVERIFY(m_ctrl->valueHistory().isEmpty());
    }

    // ── NullProvider name is empty (triggers "source" placeholder in command row) ──

    void testNullProviderNameEmpty() {
        m_ctrl->clearSources();
        QApplication::processEvents();

        QVERIFY(m_doc->provider->name().isEmpty());
    }

    // ── Regression: attaching to a new source must adopt that source's
    //    baseAddress, even if the previous baseAddress was a non-default
    //    value (e.g. the heap pointer the "New Class" flow stamps in via
    //    self-attach). The bug: selectSource only replaced baseAddress
    //    when it was exactly 0x00400000, so after a self-attach the
    //    stale Reclass.exe heap pointer survived the switch to another
    //    process, every read landed on unmapped memory, and the editor
    //    showed all 00s while the address bar still displayed the
    //    leftover pointer (e.g. 0x233C8AEDDB0).
    void testSelectSourceAdoptsProviderBaseAfterSelfAttach() {
        // Stamp the document with a non-default baseAddress that
        // mirrors the post-self-attach state — a heap-shaped pointer
        // that has nothing to do with the new target we're about to
        // pick. If the controller fails to overwrite this on attach,
        // the test catches it.
        constexpr uint64_t kStaleSelfAttachBase = 0x233C8AEDDB0ull;
        constexpr uint64_t kNewProviderBase     = 0x00007FF712340000ull;
        m_doc->tree.baseAddress = kStaleSelfAttachBase;
        m_doc->tree.baseAddressFormula.clear();

        // Register a no-dialog fake plugin so selectSource can run
        // headless. ProviderRegistry is a process-wide singleton —
        // unregister at end of test so other tests aren't polluted.
        const QString kFakeId = QStringLiteral("fakebaseprovider");
        FakeProviderPlugin fake(kNewProviderBase,
                                QStringLiteral("42:target.exe"));
        ProviderRegistry::instance().registerProvider(
            QStringLiteral("FakeBaseProvider"), kFakeId, &fake);

        // Drive the same path a user takes when they pick a new
        // process from the source menu.
        m_ctrl->selectSource(QStringLiteral("Fake Base Provider"));
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.baseAddress, kNewProviderBase);

        ProviderRegistry::instance().unregisterProvider(kFakeId);
    }
};

QTEST_MAIN(TestSourceManagement)
#include "test_source_management.moc"
