#include <QTest>
#include <QSignalSpy>
#include <QByteArray>
#include <QProcess>
#include <QThread>
#include <QtConcurrent>
#include <QFuture>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>

#include "providers/provider.h"
#include "scanner.h"
#include "../plugins/WinDbgMemory/WinDbgMemoryPlugin.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <initguid.h>
#include <dbgeng.h>
#endif

using namespace rcx;

// Skip tests that require a live debug session
#define REQUIRE_SESSION() \
    if (!m_hasSession) QSKIP("No debug server available")

static const char* CDB_PATH = "C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64\\cdb.exe";
static const int   DBG_PORT = 5056;

class TestWinDbgProvider : public QObject {
    Q_OBJECT

private:
    QProcess* m_cdbProcess = nullptr;
    uint32_t  m_notepadPid = 0;
    bool      m_weSpawnedNotepad = false;
    bool      m_hasSession = false;  // true if a debug server is reachable
    QString   m_connString;

    static uint32_t findProcess(const wchar_t* name)
    {
#ifdef _WIN32
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(entry);
        uint32_t pid = 0;
        if (Process32FirstW(snap, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, name) == 0) {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &entry));
        }
        CloseHandle(snap);
        return pid;
#else
        Q_UNUSED(name); return 0;
#endif
    }

    static uint32_t launchNotepad()
    {
#ifdef _WIN32
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(L"C:\\Windows\\notepad.exe", nullptr, nullptr, nullptr,
                           FALSE, 0, nullptr, nullptr, &si, &pi)) {
            WaitForInputIdle(pi.hProcess, 3000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return pi.dwProcessId;
        }
        return 0;
#else
        return 0;
#endif
    }

    static void terminateProcess(uint32_t pid)
    {
#ifdef _WIN32
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
#else
        Q_UNUSED(pid);
#endif
    }

private slots:

    // ── Fixture ──

    /// Try a quick DebugConnect to see if the port is already serving.
    /// Runs in a detached thread with a timeout because DebugConnect can
    /// hang indefinitely with WinDbg Preview servers.
    static bool canConnect(const QString& connStr, int timeoutMs = 8000)
    {
#ifdef _WIN32
        QByteArray utf8 = connStr.toUtf8();
        std::atomic<int> state{0}; // 0=pending, 1=connected, -1=failed
        std::thread t([&state, utf8]() {
            CoInitializeEx(NULL, COINIT_MULTITHREADED);
            IDebugClient* probe = nullptr;
            HRESULT hr = DebugConnect(utf8.constData(), IID_IDebugClient, (void**)&probe);
            if (SUCCEEDED(hr) && probe) {
                probe->EndSession(DEBUG_END_DISCONNECT);
                probe->Release();
                state.store(1);
            } else {
                state.store(-1);
            }
            CoUninitialize();
        });
        t.detach(); // Don't block on join — DebugConnect may hang forever

        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeoutMs);
        while (state.load() == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                qDebug() << "canConnect: DebugConnect timed out after" << timeoutMs << "ms";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return state.load() == 1;
#else
        Q_UNUSED(connStr); Q_UNUSED(timeoutMs);
        return false;
#endif
    }

    void initTestCase()
    {
        m_connString = QString("tcp:Port=%1,Server=127.0.0.1").arg(DBG_PORT);

        // If a debug server is already listening (e.g. WinDbg with .server),
        // skip launching our own cdb.exe.
        if (canConnect(m_connString)) {
            qDebug() << "Debug server already running on port" << DBG_PORT << "— using it";
            m_hasSession = true;
            return;
        }

        // No server running — try to launch cdb ourselves.
        // If cdb isn't available, user-mode tests will be skipped but
        // kernel/dump tests can still run via WINDBG_KERNEL_CONN.
        m_notepadPid = findProcess(L"notepad.exe");
        if (m_notepadPid == 0) {
            m_notepadPid = launchNotepad();
            m_weSpawnedNotepad = true;
        }
        if (m_notepadPid == 0) {
            qDebug() << "No notepad.exe and could not launch — user-mode tests will skip";
            return;
        }
        qDebug() << "Using notepad.exe PID:" << m_notepadPid;

        m_cdbProcess = new QProcess(this);
        QStringList args;
        args << "-server" << QString("tcp:port=%1").arg(DBG_PORT)
             << "-pv"
             << "-p" << QString::number(m_notepadPid);

        m_cdbProcess->setProgram(CDB_PATH);
        m_cdbProcess->setArguments(args);
        m_cdbProcess->start();

        if (!m_cdbProcess->waitForStarted(5000)) {
            qDebug() << "Failed to start cdb.exe — user-mode tests will skip";
            delete m_cdbProcess;
            m_cdbProcess = nullptr;
            return;
        }
        QThread::sleep(3);

        qDebug() << "cdb.exe debug server started on port" << DBG_PORT;
        m_hasSession = true;
    }

    void cleanupTestCase()
    {
        if (m_cdbProcess) {
            m_cdbProcess->write("q\n");
            if (!m_cdbProcess->waitForFinished(5000))
                m_cdbProcess->kill();
            delete m_cdbProcess;
            m_cdbProcess = nullptr;
        }

        if (m_weSpawnedNotepad && m_notepadPid)
            terminateProcess(m_notepadPid);
    }

    // ── Plugin metadata ──

    void plugin_name()
    {
        WinDbgMemoryPlugin plugin;
        QCOMPARE(plugin.Name(), std::string("WinDbg Memory"));
    }

    void plugin_version()
    {
        WinDbgMemoryPlugin plugin;
        QCOMPARE(plugin.Version(), std::string("2.0.0"));
    }

    void plugin_canHandle_tcp()
    {
        WinDbgMemoryPlugin plugin;
        QVERIFY(plugin.canHandle("tcp:Port=5056,Server=localhost"));
        QVERIFY(plugin.canHandle("TCP:Port=1234,Server=10.0.0.1"));
    }

    void plugin_canHandle_npipe()
    {
        WinDbgMemoryPlugin plugin;
        QVERIFY(plugin.canHandle("npipe:Pipe=test,Server=localhost"));
    }

    void plugin_canHandle_pid()
    {
        WinDbgMemoryPlugin plugin;
        QVERIFY(plugin.canHandle("pid:1234"));
    }

    void plugin_canHandle_dump()
    {
        WinDbgMemoryPlugin plugin;
        QVERIFY(plugin.canHandle("dump:C:/test.dmp"));
    }

    void plugin_canHandle_invalid()
    {
        WinDbgMemoryPlugin plugin;
        QVERIFY(!plugin.canHandle(""));
        QVERIFY(!plugin.canHandle("1234"));
        QVERIFY(!plugin.canHandle("file:///test.bin"));
    }

    // ── Connection failure ──

    void provider_connect_badPort()
    {
        WinDbgMemoryProvider prov("tcp:Port=59999,Server=localhost");
        QVERIFY(!prov.isValid());
        QCOMPARE(prov.size(), 0);
    }

    void provider_connect_badPipe()
    {
        WinDbgMemoryProvider prov("npipe:Pipe=nonexistent_reclass_test_pipe,Server=localhost");
        QVERIFY(!prov.isValid());
        QCOMPARE(prov.size(), 0);
    }

    void plugin_createProvider_badConnection()
    {
        WinDbgMemoryPlugin plugin;
        QString error;
        auto prov = plugin.createProvider("tcp:Port=59999,Server=localhost", &error);
        QVERIFY(prov == nullptr);
        QVERIFY(!error.isEmpty());
    }

    // ── Connect and read (main thread) ──

    void provider_connect_valid()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");
        QCOMPARE(prov.kind(), QStringLiteral("WinDbg"));
        QVERIFY(prov.size() > 0);
    }

    void provider_name()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");
        QVERIFY(!prov.name().isEmpty());
        qDebug() << "Provider name:" << prov.name();
    }

    void provider_isLive()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");
        QVERIFY(prov.isLive());
    }

    void provider_baseAddress()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");
        // WinDbg provider no longer auto-selects a module base — it returns 0
        // so the controller doesn't override the user's chosen base address.
        QCOMPARE(prov.base(), (uint64_t)0);
    }

    // ── Read: MZ header on main thread ──

    void provider_read_mz_mainThread()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");

        uint8_t buf[2] = {};
        bool ok = prov.read(0, buf, 2);
        QVERIFY2(ok, "Failed to read from debug session (main thread)");
        QCOMPARE(buf[0], (uint8_t)'M');
        QCOMPARE(buf[1], (uint8_t)'Z');
    }

    // ── Read: MZ header from a background thread (the actual failure case) ──

    void provider_read_mz_backgroundThread()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");

        // Simulate what the controller's refresh does:
        // read from a QtConcurrent worker thread.
        QFuture<QByteArray> future = QtConcurrent::run([&prov]() -> QByteArray {
            return prov.readBytes(0, 128);
        });
        future.waitForFinished();
        QByteArray data = future.result();

        QCOMPARE(data.size(), 128);
        QCOMPARE((uint8_t)data[0], (uint8_t)'M');
        QCOMPARE((uint8_t)data[1], (uint8_t)'Z');
    }

    // ── Read: bulk data from background thread ──

    void provider_read_4k_backgroundThread()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");

        QFuture<QByteArray> future = QtConcurrent::run([&prov]() -> QByteArray {
            return prov.readBytes(0, 4096);
        });
        future.waitForFinished();
        QByteArray data = future.result();

        QCOMPARE(data.size(), 4096);
        QCOMPARE((uint8_t)data[0], (uint8_t)'M');
        QCOMPARE((uint8_t)data[1], (uint8_t)'Z');

        // Verify it's not all zeros (the old failure mode)
        bool allZero = true;
        for (int i = 0; i < data.size(); ++i) {
            if (data[i] != '\0') { allZero = false; break; }
        }
        QVERIFY2(!allZero, "Data is all zeros — background thread read failed");
    }

    // ── Multiple sequential background reads (simulates refresh timer) ──

    void provider_read_multipleRefreshes()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");

        for (int i = 0; i < 5; ++i) {
            QFuture<QByteArray> future = QtConcurrent::run([&prov]() -> QByteArray {
                return prov.readBytes(0, 128);
            });
            future.waitForFinished();
            QByteArray data = future.result();
            QCOMPARE(data.size(), 128);
            QCOMPARE((uint8_t)data[0], (uint8_t)'M');
            QCOMPARE((uint8_t)data[1], (uint8_t)'Z');
        }
    }

    // ── Read helpers ──

    void provider_readU16()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");
        QCOMPARE(prov.readU16(0), (uint16_t)0x5A4D); // "MZ" little-endian
    }

    void provider_read_peSignature()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");

        uint32_t peOffset = prov.readU32(0x3C);
        QVERIFY2(peOffset > 0 && peOffset < 0x1000, "PE offset should be reasonable");

        uint8_t sig[4] = {};
        bool ok = prov.read(peOffset, sig, 4);
        QVERIFY(ok);
        QCOMPARE(sig[0], (uint8_t)'P');
        QCOMPARE(sig[1], (uint8_t)'E');
        QCOMPARE(sig[2], (uint8_t)0);
        QCOMPARE(sig[3], (uint8_t)0);
    }

    // ── Edge cases ──

    void provider_read_zeroLength()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");
        uint8_t buf = 0xFF;
        QVERIFY(!prov.read(0, &buf, 0));
    }

    void provider_read_negativeLength()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");
        uint8_t buf = 0xFF;
        QVERIFY(!prov.read(0, &buf, -1));
    }

    // ── getSymbol ──

    void provider_getSymbol()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");
        QString sym = prov.getSymbol(0);
        qDebug() << "Symbol at base+0:" << sym;
        // Should not crash; may or may not resolve
    }

    void provider_getSymbol_backgroundThread()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");

        QFuture<QString> future = QtConcurrent::run([&prov]() -> QString {
            return prov.getSymbol(0);
        });
        future.waitForFinished();
        // Should not crash from background thread
        qDebug() << "Symbol (bg thread):" << future.result();
    }

    // ── createProvider full flow ──

    void plugin_createProvider_valid()
    {
        REQUIRE_SESSION();
        WinDbgMemoryPlugin plugin;
        QString error;
        auto prov = plugin.createProvider(m_connString, &error);
        if (!prov || !prov->isValid()) QSKIP("Debug session not connected");

        uint8_t mz[2] = {};
        QVERIFY(prov->read(0, mz, 2));
        QCOMPARE(mz[0], (uint8_t)'M');
        QCOMPARE(mz[1], (uint8_t)'Z');
    }

    // ── Multiple concurrent connections ──

    void provider_multipleConcurrent()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov1(m_connString);
        WinDbgMemoryProvider prov2(m_connString);

        if (!prov1.isValid() || !prov2.isValid()) QSKIP("Debug session not connected");

        QCOMPARE(prov1.readU16(0), (uint16_t)0x5A4D);
        QCOMPARE(prov2.readU16(0), (uint16_t)0x5A4D);
    }

    // ── Factory ──

    void factory_createPlugin()
    {
        IPlugin* raw = CreatePlugin();
        QVERIFY(raw != nullptr);
        QCOMPARE(raw->Type(), IPlugin::ProviderPlugin);
        QCOMPARE(raw->Name(), std::string("WinDbg Memory"));
        delete raw;
    }

    // ── enumerateRegions ──

    void provider_enumerateRegions()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");

        auto regions = prov.enumerateRegions();
        qDebug() << "enumerateRegions returned" << regions.size() << "regions";
        QVERIFY2(!regions.isEmpty(), "Should return at least one memory region");

        // Every region should have sane values
        for (const auto& r : regions) {
            QVERIFY(r.size > 0);
            QVERIFY(r.readable);
        }
    }

    void provider_enumerateRegions_hasModuleNames()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");

        auto regions = prov.enumerateRegions();
        QVERIFY(!regions.isEmpty());

        // At least one region should have a module name
        bool hasModule = false;
        for (const auto& r : regions) {
            if (!r.moduleName.isEmpty()) {
                hasModule = true;
                qDebug() << "Region base=0x" + QString::number(r.base, 16)
                         << "size=" << r.size
                         << "module=" << r.moduleName
                         << "r/w/x:" << r.readable << r.writable << r.executable;
                break;
            }
        }
        QVERIFY2(hasModule, "At least one region should have a module name");
    }

    void provider_enumerateRegions_hasExecutable()
    {
        REQUIRE_SESSION();
        WinDbgMemoryProvider prov(m_connString);
        if (!prov.isValid()) QSKIP("Debug session not connected");

        auto regions = prov.enumerateRegions();
        QVERIFY(!regions.isEmpty());

        bool hasExec = false;
        for (const auto& r : regions) {
            if (r.executable) { hasExec = true; break; }
        }
        QVERIFY2(hasExec, "Should have at least one executable region (code)");
    }

    // ── Scanner integration ──

    void scanner_signature_mz()
    {
        // Scan for the MZ header — should find at least one match
        auto prov = std::make_shared<WinDbgMemoryProvider>(m_connString);
        if (!prov->isValid()) QSKIP("Debug session not connected");

        auto regions = prov->enumerateRegions();
        QVERIFY2(!regions.isEmpty(), "Need regions for scan");

        rcx::ScanRequest req;
        QString err;
        QVERIFY(rcx::parseSignature("4D 5A", req.pattern, req.mask, &err));
        req.alignment = 1;
        req.maxResults = 100;

        rcx::ScanEngine engine;
        QSignalSpy spy(&engine, &rcx::ScanEngine::finished);

        engine.start(prov, req);
        QVERIFY(spy.wait(30000));

        auto results = spy.at(0).at(0).value<QVector<rcx::ScanResult>>();
        qDebug() << "MZ scan found" << results.size() << "results";
        QVERIFY2(!results.isEmpty(), "Should find at least one MZ header");

        // Verify the first result is actually 'MZ'
        uint8_t buf[2] = {};
        prov->read(results[0].address, buf, 2);
        QCOMPARE(buf[0], (uint8_t)'M');
        QCOMPARE(buf[1], (uint8_t)'Z');
    }

    void scanner_value_int32()
    {
        // Read a known 4-byte value from offset 0x3C (PE offset) then scan for it.
        // This only works for user-mode targets where address 0 is the main module.
        auto prov = std::make_shared<WinDbgMemoryProvider>(m_connString);
        if (!prov->isValid()) QSKIP("Debug session not connected");

        auto regions = prov->enumerateRegions();
        QVERIFY2(!regions.isEmpty(), "Need regions for scan");

        uint32_t peOffset = prov->readU32(0x3C);
        if (peOffset == 0 || peOffset >= 0x1000)
            QSKIP("Address 0 not readable (kernel session) — value scan test requires user-mode target");

        rcx::ScanRequest req;
        QString err;
        QVERIFY(rcx::serializeValue(rcx::ValueType::UInt32,
                                     QString::number(peOffset), req.pattern, req.mask, &err));
        req.alignment = 4;
        req.maxResults = 100;

        rcx::ScanEngine engine;
        QSignalSpy spy(&engine, &rcx::ScanEngine::finished);

        engine.start(prov, req);
        QVERIFY(spy.wait(30000));

        auto results = spy.at(0).at(0).value<QVector<rcx::ScanResult>>();
        qDebug() << "Value scan for" << peOffset << "found" << results.size() << "results";
        QVERIFY2(!results.isEmpty(), "Should find the PE offset value somewhere");
    }

    // ── Kernel/dump session tests ──
    // Set WINDBG_KERNEL_CONN to a target string:
    //   "dump:F:/path/to/file.dmp"          — open dump directly
    //   "tcp:Port=5056,Server=localhost"     — connect to debug server
    // Set WINDBG_KERNEL_ADDR to a readable hex address (e.g. kernel base).

    static QString kernelTarget()
    {
        return qEnvironmentVariable("WINDBG_KERNEL_CONN", "");
    }

    void provider_kernel_connect()
    {
        QString target = kernelTarget();
        if (target.isEmpty())
            QSKIP("Set WINDBG_KERNEL_CONN (e.g. dump:F:/file.dmp)");

        WinDbgMemoryProvider prov(target);
        QVERIFY2(prov.isValid(),
                 qPrintable("Should connect to " + target));
        QCOMPARE(prov.kind(), QStringLiteral("WinDbg"));

        qDebug() << "Kernel provider name:" << prov.name();
        qDebug() << "Kernel provider base:" << QString("0x%1").arg(prov.base(), 0, 16);
        qDebug() << "Kernel provider isLive:" << prov.isLive();
    }

    void provider_kernel_read_base()
    {
        QString target = kernelTarget();
        if (target.isEmpty())
            QSKIP("Set WINDBG_KERNEL_CONN");

        QString addrStr = qEnvironmentVariable("WINDBG_KERNEL_ADDR", "");
        if (addrStr.isEmpty())
            QSKIP("Set WINDBG_KERNEL_ADDR to a readable kernel address");

        WinDbgMemoryProvider prov(target);
        QVERIFY2(prov.isValid(),
                 qPrintable("Failed to connect to " + target));

        bool ok = false;
        uint64_t addr = addrStr.toULongLong(&ok, 16);
        QVERIFY2(ok && addr != 0, "WINDBG_KERNEL_ADDR must be a valid hex address");

        uint8_t buf[16] = {};
        ok = prov.read(addr, buf, 16);
        QVERIFY2(ok, "Should read from kernel address");

        bool allZero = true;
        for (int i = 0; i < 16; ++i) {
            if (buf[i] != 0) { allZero = false; break; }
        }
        QVERIFY2(!allZero, "Kernel read returned all zeros");

        qDebug() << "Read 16 bytes at" << QString("0x%1").arg(addr, 0, 16)
                 << "first 4:" << QString("%1 %2 %3 %4")
                    .arg(buf[0], 2, 16, QChar('0'))
                    .arg(buf[1], 2, 16, QChar('0'))
                    .arg(buf[2], 2, 16, QChar('0'))
                    .arg(buf[3], 2, 16, QChar('0'));
    }

    void provider_kernel_read_high_address()
    {
        QString target = kernelTarget();
        if (target.isEmpty())
            QSKIP("Set WINDBG_KERNEL_CONN");

        QString addrStr = qEnvironmentVariable("WINDBG_KERNEL_ADDR", "");
        uint64_t addr = 0;
        if (!addrStr.isEmpty()) {
            bool ok = false;
            addr = addrStr.toULongLong(&ok, 16);
            if (!ok) addr = 0;
        }

        WinDbgMemoryProvider prov(target);
        QVERIFY2(prov.isValid(),
                 qPrintable("Failed to connect to " + target));

        if (addr == 0) addr = prov.base();
        if (addr == 0)
            QSKIP("No kernel address available (set WINDBG_KERNEL_ADDR)");

        uint8_t buf[64] = {};
        bool ok = prov.read(addr, buf, 64);
        QVERIFY2(ok, qPrintable(QString("Should read kernel addr 0x%1")
                                 .arg(addr, 0, 16)));

        bool allZero = true;
        for (int i = 0; i < 64; ++i) {
            if (buf[i] != 0) { allZero = false; break; }
        }
        QVERIFY2(!allZero, "Kernel high-address read returned all zeros");

        qDebug() << "Read 64 bytes at" << QString("0x%1").arg(addr, 0, 16)
                 << "first 8:" << QString("%1 %2 %3 %4 %5 %6 %7 %8")
                    .arg(buf[0], 2, 16, QChar('0'))
                    .arg(buf[1], 2, 16, QChar('0'))
                    .arg(buf[2], 2, 16, QChar('0'))
                    .arg(buf[3], 2, 16, QChar('0'))
                    .arg(buf[4], 2, 16, QChar('0'))
                    .arg(buf[5], 2, 16, QChar('0'))
                    .arg(buf[6], 2, 16, QChar('0'))
                    .arg(buf[7], 2, 16, QChar('0'));
    }

    void provider_kernel_enumerateRegions()
    {
        QString target = kernelTarget();
        if (target.isEmpty())
            QSKIP("Set WINDBG_KERNEL_CONN");

        WinDbgMemoryProvider prov(target);
        QVERIFY2(prov.isValid(),
                 qPrintable("Failed to connect to " + target));

        auto regions = prov.enumerateRegions();
        qDebug() << "Kernel enumerateRegions returned" << regions.size() << "regions";
        QVERIFY2(!regions.isEmpty(), "Should return kernel memory regions");

        // Log first few regions
        int logged = 0;
        for (const auto& r : regions) {
            if (logged++ >= 10) break;
            qDebug() << "  base=0x" + QString::number(r.base, 16)
                     << "size=" << r.size
                     << "module=" << r.moduleName
                     << "r/w/x:" << r.readable << r.writable << r.executable;
        }
    }

    void provider_kernel_scan_signature()
    {
        QString target = kernelTarget();
        if (target.isEmpty())
            QSKIP("Set WINDBG_KERNEL_CONN");

        auto prov = std::make_shared<WinDbgMemoryProvider>(target);
        QVERIFY2(prov->isValid(),
                 qPrintable("Failed to connect to " + target));

        auto regions = prov->enumerateRegions();
        if (regions.isEmpty())
            QSKIP("No regions enumerated — QueryVirtual may not be supported for this target");

        // Scan for MZ header in executable regions
        rcx::ScanRequest req;
        QString err;
        QVERIFY(rcx::parseSignature("4D 5A 90 00", req.pattern, req.mask, &err));
        req.alignment = 1;
        req.filterExecutable = true;
        req.maxResults = 50;

        rcx::ScanEngine engine;
        QSignalSpy spy(&engine, &rcx::ScanEngine::finished);

        engine.start(prov, req);
        QVERIFY(spy.wait(60000));

        auto results = spy.at(0).at(0).value<QVector<rcx::ScanResult>>();
        qDebug() << "Kernel MZ scan (exec only) found" << results.size() << "results";
        for (const auto& r : results)
            qDebug() << "  0x" + QString::number(r.address, 16) << r.regionModule;

        QVERIFY2(!results.isEmpty(), "Should find MZ headers in kernel modules");
    }

    void provider_kernel_read_backgroundThread()
    {
        QString target = kernelTarget();
        if (target.isEmpty())
            QSKIP("Set WINDBG_KERNEL_CONN");

        QString addrStr = qEnvironmentVariable("WINDBG_KERNEL_ADDR", "");
        if (addrStr.isEmpty())
            QSKIP("Set WINDBG_KERNEL_ADDR to a readable kernel address");

        bool ok = false;
        uint64_t addr = addrStr.toULongLong(&ok, 16);
        QVERIFY2(ok && addr != 0, "WINDBG_KERNEL_ADDR must be a valid hex address");

        WinDbgMemoryProvider prov(target);
        QVERIFY2(prov.isValid(),
                 qPrintable("Failed to connect to " + target));

        // Simulate the controller's async refresh pattern
        QFuture<QByteArray> future = QtConcurrent::run([&prov, addr]() -> QByteArray {
            return prov.readBytes(addr, 4096);
        });
        future.waitForFinished();
        QByteArray data = future.result();

        QCOMPARE(data.size(), 4096);
        bool allZero = true;
        for (int i = 0; i < data.size(); ++i) {
            if (data[i] != '\0') { allZero = false; break; }
        }
        QVERIFY2(!allZero, "Kernel background read returned all zeros");
    }
};

QTEST_MAIN(TestWinDbgProvider)
#include "test_windbg_provider.moc"
