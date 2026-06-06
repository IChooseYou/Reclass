// Tests for the self-attached "New Class" project flow.
//
// What's under test:
//   - RcxDocument exposes m_ownedBuffer + m_ownedBufferSize fields
//   - The document can hold a unique_ptr<uint8_t[]> stable across CoW
//   - Buffer pointer stays valid for the document's lifetime
//   - On Windows, the processmemory provider attached to our own PID
//     can read AND write the owned buffer through RPM/WPM
//
// We don't go through MainWindow::project_new directly (that requires
// a full QMainWindow + start page + dock infrastructure) — instead we
// replicate its core contract: allocate buffer, point baseAddress at
// it, attach processmemory targeting current PID, write via provider,
// observe bytes in the buffer. If THIS works, project_new's wrapper
// works by construction.
//
// Skipped on non-Windows because the processmemory plugin is
// Windows-only at the moment.

#include <QtTest/QTest>
#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QLibrary>
#include <QSplitter>
#include "controller.h"
#include "core.h"
#include "iplugin.h"
#include "providerregistry.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace rcx;

namespace {

// Load the processmemory plugin DLL manually so this test can exercise
// the RW round-trip without depending on MainWindow's PluginManager
// bootstrap. Mirrors the same QLibrary + CreatePlugin handshake the
// real PluginManager does. Returns true if the provider now appears
// in the global registry. Idempotent — calling twice doesn't double-
// register.
bool ensureProcessMemoryPluginLoaded() {
    if (ProviderRegistry::instance().findProvider(
            QStringLiteral("processmemory")))
        return true;

    // Try a few likely paths relative to the test binary location.
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        appDir + "/Plugins/libProcessMemoryPlugin.dll",
        appDir + "/plugins/libProcessMemoryPlugin.dll",
        appDir + "/Plugins/ProcessMemoryPlugin.dll",
        appDir + "/plugins/ProcessMemoryPlugin.dll",
    };
    QString found;
    for (const QString& p : candidates) {
        if (QFileInfo::exists(p)) { found = p; break; }
    }
    if (found.isEmpty()) return false;

    auto* lib = new QLibrary(found);
    if (!lib->load()) { delete lib; return false; }
    using CreatePluginFunc = IPlugin* (*)();
    auto createFn = reinterpret_cast<CreatePluginFunc>(lib->resolve("CreatePlugin"));
    if (!createFn) { lib->unload(); delete lib; return false; }
    IPlugin* plug = createFn();
    if (!plug || plug->Type() != IPlugin::ProviderPlugin) {
        lib->unload(); delete lib;
        return false;
    }
    auto* provPlug = static_cast<IProviderPlugin*>(plug);
    QString name = QString::fromStdString(plug->Name());
    QString identifier = name.toLower().replace(" ", "");
    ProviderRegistry::instance().registerProvider(
        name, identifier, provPlug, QFileInfo(found).fileName());
    return true;
}

} // namespace

class TestNewClassSelfAttach : public QObject {
    Q_OBJECT

private slots:
    void testOwnedBufferAllocates() {
        // Smallest viable smoke test — fields exist and can be assigned.
        RcxDocument doc;
        QCOMPARE(doc.m_ownedBuffer.get(), nullptr);
        QCOMPARE(doc.m_ownedBufferSize, size_t(0));

        constexpr size_t kSize = 1024;
        doc.m_ownedBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[kSize]());
        doc.m_ownedBufferSize = kSize;
        QVERIFY(doc.m_ownedBuffer.get() != nullptr);
        QCOMPARE(doc.m_ownedBufferSize, kSize);
    }

    void testBufferZeroInitialized() {
        // value-init `new uint8_t[N]()` must zero every byte.
        RcxDocument doc;
        constexpr size_t kSize = 4096;
        doc.m_ownedBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[kSize]());
        doc.m_ownedBufferSize = kSize;
        for (size_t i = 0; i < kSize; ++i)
            QCOMPARE(doc.m_ownedBuffer[i], uint8_t(0));
    }

    void testBaseAddressPointsAtBuffer() {
        RcxDocument doc;
        doc.m_ownedBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[16384]());
        doc.m_ownedBufferSize = 16384;
        doc.tree.baseAddress = reinterpret_cast<uint64_t>(doc.m_ownedBuffer.get());

        // The address should be a real heap pointer — not a placeholder
        // sentinel like 0x00400000.
        QVERIFY(doc.tree.baseAddress != 0);
        QVERIFY(doc.tree.baseAddress != 0x00400000ULL);
        QCOMPARE(reinterpret_cast<void*>(doc.tree.baseAddress),
                 static_cast<void*>(doc.m_ownedBuffer.get()));
    }

    void testBufferPointerStableAcrossMoves() {
        // Make sure storing the buffer in a class field doesn't cause
        // the pointer to shift. unique_ptr<uint8_t[]> backs raw heap
        // memory; once allocated, the pointer never moves.
        auto buf = std::unique_ptr<uint8_t[]>(new uint8_t[1024]());
        uint8_t* before = buf.get();
        RcxDocument doc;
        doc.m_ownedBuffer = std::move(buf);
        QCOMPARE(doc.m_ownedBuffer.get(), before);

        // And after taking the address through QString roundtrip
        // (simulating baseAddressFormula manipulation):
        doc.tree.baseAddress = reinterpret_cast<uint64_t>(doc.m_ownedBuffer.get());
        QString addrStr = QString::number(doc.tree.baseAddress, 16);
        bool ok;
        uint64_t roundtrip = addrStr.toULongLong(&ok, 16);
        QVERIFY(ok);
        QCOMPARE(reinterpret_cast<uint8_t*>(roundtrip), doc.m_ownedBuffer.get());
    }

#ifdef Q_OS_WIN
    // ── End-to-end: processmemory provider can RW the owned buffer ──
    //
    // This test makes the strong claim that backs the whole "New Class
    // self-attach" feature: when the document's baseAddress points at
    // a buffer in our own process AND the processmemory provider is
    // attached to our own PID, the provider's read/write paths hit
    // the buffer correctly.
    // registerAsSavedSource=true must add the attach to the controller's
    // saved-sources list so the dropdown shows the attached source.
    // registerAsSavedSource=false (default) must NOT touch the list —
    // matches the MCP / requestOpenProviderTab callers' expectation that
    // attachViaPlugin is otherwise side-effect-free on UI state.
    void testRegisterAsSavedSourceFlag() {
        if (!ensureProcessMemoryPluginLoaded())
            QSKIP("processmemory plugin DLL not found next to test binary");

        // First: default (no register) → saved sources stay empty.
        {
            RcxDocument doc;
            doc.m_ownedBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[4096]());
            doc.m_ownedBufferSize = 4096;
            doc.tree.baseAddress = reinterpret_cast<uint64_t>(doc.m_ownedBuffer.get());

            QSplitter splitter;
            RcxController ctrl(&doc, nullptr);
            ctrl.addSplitEditor(&splitter);
            DWORD pid = GetCurrentProcessId();
            QString target = QString("%1:Reclass.exe").arg(pid);
            ctrl.attachViaPlugin(QStringLiteral("processmemory"), target);
            QCOMPARE(ctrl.savedSources().size(), 0);
            QCOMPARE(ctrl.activeSourceIndex(), -1);
        }

        // Second: opt-in → entry appears with correct fields.
        {
            RcxDocument doc;
            doc.m_ownedBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[4096]());
            doc.m_ownedBufferSize = 4096;
            doc.tree.baseAddress = reinterpret_cast<uint64_t>(doc.m_ownedBuffer.get());

            QSplitter splitter;
            RcxController ctrl(&doc, nullptr);
            ctrl.addSplitEditor(&splitter);
            DWORD pid = GetCurrentProcessId();
            QString target = QString("%1:Reclass.exe").arg(pid);
            ctrl.attachViaPlugin(QStringLiteral("processmemory"), target,
                /*registerAsSavedSource=*/true);

            const auto& sources = ctrl.savedSources();
            QCOMPARE(sources.size(), 1);
            QCOMPARE(sources[0].kind, QStringLiteral("processmemory"));
            QCOMPARE(sources[0].providerTarget, target);
            QCOMPARE(sources[0].displayName, QStringLiteral("Reclass.exe"));
            QCOMPARE(ctrl.activeSourceIndex(), 0);
        }
    }

    // Repeating the same attach should dedup, not grow the list.
    void testRegisterAsSavedSourceDedupsTarget() {
        if (!ensureProcessMemoryPluginLoaded())
            QSKIP("processmemory plugin DLL not found next to test binary");

        RcxDocument doc;
        doc.m_ownedBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[4096]());
        doc.m_ownedBufferSize = 4096;
        doc.tree.baseAddress = reinterpret_cast<uint64_t>(doc.m_ownedBuffer.get());

        QSplitter splitter;
        RcxController ctrl(&doc, nullptr);
        ctrl.addSplitEditor(&splitter);
        DWORD pid = GetCurrentProcessId();
        QString target = QString("%1:Reclass.exe").arg(pid);
        ctrl.attachViaPlugin(QStringLiteral("processmemory"), target, true);
        ctrl.attachViaPlugin(QStringLiteral("processmemory"), target, true);
        ctrl.attachViaPlugin(QStringLiteral("processmemory"), target, true);
        QCOMPARE(ctrl.savedSources().size(), 1);
        QCOMPARE(ctrl.activeSourceIndex(), 0);
    }

    void testProcessMemoryReadWritesBuffer() {
        if (!ensureProcessMemoryPluginLoaded())
            QSKIP("processmemory plugin DLL not found next to test binary");

        // Build a minimal doc+controller fixture similar to
        // test_byte_selection_controller.cpp.
        RcxDocument doc;
        constexpr size_t kSize = 64 * 1024;
        doc.m_ownedBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[kSize]());
        doc.m_ownedBufferSize = kSize;

        // Plant a recognizable byte pattern in our buffer.
        for (size_t i = 0; i < 16; ++i)
            doc.m_ownedBuffer[i] = uint8_t(0x10 + i);

        doc.tree.baseAddress = reinterpret_cast<uint64_t>(doc.m_ownedBuffer.get());

        // Attach processmemory targeting our own PID.
        QSplitter splitter;
        RcxController ctrl(&doc, nullptr);
        ctrl.addSplitEditor(&splitter);
        DWORD pid = GetCurrentProcessId();
        QString target = QString("%1:Reclass.exe").arg(pid);
        ctrl.attachViaPlugin(QStringLiteral("processmemory"), target);
        // attachViaPlugin can re-set baseAddress; re-pin it.
        doc.tree.baseAddress = reinterpret_cast<uint64_t>(doc.m_ownedBuffer.get());

        // Provider should be present and writable.
        QVERIFY(doc.provider != nullptr);
        QVERIFY(doc.provider->isValid());
        QVERIFY(doc.provider->isWritable());

        // Read through the provider: bytes should match what we planted.
        uint8_t readBuf[16] = {};
        bool readOk = doc.provider->read(doc.tree.baseAddress, readBuf, 16);
        QVERIFY2(readOk, "Provider read failed");
        for (int i = 0; i < 16; ++i) {
            QCOMPARE((int)readBuf[i], (int)(0x10 + i));
        }

        // Write through the provider: buffer bytes should change.
        uint8_t pattern[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        bool writeOk = doc.provider->writeBytes(
            doc.tree.baseAddress + 4,
            QByteArray(reinterpret_cast<const char*>(pattern), 4));
        QVERIFY2(writeOk, "Provider writeBytes failed");

        // Our buffer should now show the new pattern at offset 4..7.
        QCOMPARE((int)doc.m_ownedBuffer[4], 0xDE);
        QCOMPARE((int)doc.m_ownedBuffer[5], 0xAD);
        QCOMPARE((int)doc.m_ownedBuffer[6], 0xBE);
        QCOMPARE((int)doc.m_ownedBuffer[7], 0xEF);
        // And bytes around the write are untouched.
        QCOMPARE((int)doc.m_ownedBuffer[3], 0x13);
        QCOMPARE((int)doc.m_ownedBuffer[8], 0x18);
    }
#endif // Q_OS_WIN
};

QTEST_MAIN(TestNewClassSelfAttach)
#include "test_new_class_selfattach.moc"
