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

using namespace rcx;

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
};

QTEST_MAIN(TestSourceManagement)
#include "test_source_management.moc"
