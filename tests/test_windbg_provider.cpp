#include <QTest>
#include <QByteArray>
#include <QProcess>
#include <QThread>
#include <QtConcurrent>
#include <QFuture>
#include <cstring>

#include "providers/provider.h"
#include "../plugins/WinDbgMemory/WinDbgMemoryPlugin.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <initguid.h>
#include <dbgeng.h>
#endif

using namespace rcx;

static const char* CDB_PATH = "C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64\\cdb.exe";
static const int   DBG_PORT = 5055;

class TestWinDbgProvider : public QObject {
    Q_OBJECT

private:
    QProcess* m_cdbProcess = nullptr;
    uint32_t  m_notepadPid = 0;
    bool      m_weSpawnedNotepad = false;
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
    static bool canConnect(const QString& connStr)
    {
#ifdef _WIN32
        IDebugClient* probe = nullptr;
        QByteArray utf8 = connStr.toUtf8();
        HRESULT hr = DebugConnect(utf8.constData(), IID_IDebugClient, (void**)&probe);
        if (SUCCEEDED(hr) && probe) {
            probe->EndSession(DEBUG_END_DISCONNECT);
            probe->Release();
            return true;
        }
        return false;
#else
        Q_UNUSED(connStr);
        return false;
#endif
    }

    void initTestCase()
    {
        m_connString = QString("tcp:Port=%1,Server=localhost").arg(DBG_PORT);

        // If a debug server is already listening (e.g. WinDbg with .server),
        // skip launching our own cdb.exe.
        if (canConnect(m_connString)) {
            qDebug() << "Debug server already running on port" << DBG_PORT << "— using it";
            return;
        }

        // No server running — launch cdb ourselves
        m_notepadPid = findProcess(L"notepad.exe");
        if (m_notepadPid == 0) {
            m_notepadPid = launchNotepad();
            m_weSpawnedNotepad = true;
        }
        QVERIFY2(m_notepadPid != 0, "Need notepad.exe running");
        qDebug() << "Using notepad.exe PID:" << m_notepadPid;

        m_cdbProcess = new QProcess(this);
        QStringList args;
        args << "-server" << QString("tcp:port=%1").arg(DBG_PORT)
             << "-pv"
             << "-p" << QString::number(m_notepadPid);

        m_cdbProcess->setProgram(CDB_PATH);
        m_cdbProcess->setArguments(args);
        m_cdbProcess->start();

        QVERIFY2(m_cdbProcess->waitForStarted(5000), "Failed to start cdb.exe");
        QThread::sleep(3);

        qDebug() << "cdb.exe debug server started on port" << DBG_PORT;
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
        QVERIFY(plugin.canHandle("tcp:Port=5055,Server=localhost"));
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
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY2(prov.isValid(), "Should connect to cdb debug server");
        QCOMPARE(prov.kind(), QStringLiteral("WinDbg"));
        QVERIFY(prov.size() > 0);
    }

    void provider_name()
    {
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());
        QVERIFY(!prov.name().isEmpty());
        qDebug() << "Provider name:" << prov.name();
    }

    void provider_isLive()
    {
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());
        QVERIFY(prov.isLive());
    }

    void provider_baseAddress()
    {
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());
        QVERIFY2(prov.base() != 0, "Should have a non-zero base from first module");
        qDebug() << "Base address:" << QString("0x%1").arg(prov.base(), 0, 16);
    }

    void provider_setBase()
    {
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());
        uint64_t orig = prov.base();
        prov.setBase(0x1000);
        QCOMPARE(prov.base(), (uint64_t)0x1000);
        prov.setBase(orig);
        QCOMPARE(prov.base(), orig);
    }

    // ── Read: MZ header on main thread ──

    void provider_read_mz_mainThread()
    {
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());

        uint8_t buf[2] = {};
        bool ok = prov.read(0, buf, 2);
        QVERIFY2(ok, "Failed to read from debug session (main thread)");
        QCOMPARE(buf[0], (uint8_t)'M');
        QCOMPARE(buf[1], (uint8_t)'Z');
    }

    // ── Read: MZ header from a background thread (the actual failure case) ──

    void provider_read_mz_backgroundThread()
    {
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());

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
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());

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
            if (data[i] != 0) { allZero = false; break; }
        }
        QVERIFY2(!allZero, "Data is all zeros — background thread read failed");
    }

    // ── Multiple sequential background reads (simulates refresh timer) ──

    void provider_read_multipleRefreshes()
    {
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());

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
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());
        QCOMPARE(prov.readU16(0), (uint16_t)0x5A4D); // "MZ" little-endian
    }

    void provider_read_peSignature()
    {
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());

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
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());
        uint8_t buf = 0xFF;
        QVERIFY(!prov.read(0, &buf, 0));
    }

    void provider_read_negativeLength()
    {
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());
        uint8_t buf = 0xFF;
        QVERIFY(!prov.read(0, &buf, -1));
    }

    // ── getSymbol ──

    void provider_getSymbol()
    {
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());
        QString sym = prov.getSymbol(0);
        qDebug() << "Symbol at base+0:" << sym;
        // Should not crash; may or may not resolve
    }

    void provider_getSymbol_backgroundThread()
    {
        WinDbgMemoryProvider prov(m_connString);
        QVERIFY(prov.isValid());

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
        WinDbgMemoryPlugin plugin;
        QString error;
        auto prov = plugin.createProvider(m_connString, &error);
        QVERIFY2(prov != nullptr, qPrintable("createProvider failed: " + error));
        QVERIFY(prov->isValid());

        uint8_t mz[2] = {};
        QVERIFY(prov->read(0, mz, 2));
        QCOMPARE(mz[0], (uint8_t)'M');
        QCOMPARE(mz[1], (uint8_t)'Z');
    }

    // ── Multiple concurrent connections ──

    void provider_multipleConcurrent()
    {
        WinDbgMemoryProvider prov1(m_connString);
        WinDbgMemoryProvider prov2(m_connString);

        QVERIFY(prov1.isValid());
        QVERIFY(prov2.isValid());

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
};

QTEST_MAIN(TestWinDbgProvider)
#include "test_windbg_provider.moc"
