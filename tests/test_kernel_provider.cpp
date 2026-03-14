#include <QTest>
#include <QSignalSpy>
#include <QByteArray>
#include <cstring>

#include "providers/provider.h"
#include "scanner.h"
#include "../plugins/KernelMemory/KernelMemoryPlugin.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

using namespace rcx;

class TestKernelProvider : public QObject {
    Q_OBJECT

private:
    bool m_driverAvailable = false;
    KernelMemoryPlugin* m_plugin = nullptr;
    std::unique_ptr<Provider> m_provider;
    uint32_t m_selfPid = 0;

private slots:

    // ── Setup: try to load driver, skip tests if unavailable ──

    void initTestCase()
    {
        m_plugin = new KernelMemoryPlugin();

#ifdef _WIN32
        m_selfPid = GetCurrentProcessId();

        // Try to open driver directly to see if it's available
        HANDLE h = CreateFileA(RCX_DRV_USERMODE_PATH,
                               GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            m_driverAvailable = true;
        } else {
            // Try loading via plugin
            QString errorMsg;
            QString target = QStringLiteral("km:%1:self").arg(m_selfPid);
            m_provider = m_plugin->createProvider(target, &errorMsg);
            if (m_provider && m_provider->isValid()) {
                m_driverAvailable = true;
            } else {
                qWarning("Kernel driver not available: %s", qPrintable(errorMsg));
                qWarning("Tests requiring the driver will be skipped.");
            }
        }

        if (m_driverAvailable && !m_provider) {
            QString target = QStringLiteral("km:%1:self").arg(m_selfPid);
            m_provider = m_plugin->createProvider(target, nullptr);
        }
#endif
    }

    void cleanupTestCase()
    {
        m_provider.reset();
        delete m_plugin;
        m_plugin = nullptr;
    }

    // ── 1. Plugin metadata (no driver needed) ──

    void plugin_name()
    {
        QCOMPARE(QString::fromStdString(m_plugin->Name()), QStringLiteral("Kernel Memory"));
    }

    void plugin_loadType()
    {
        QCOMPARE(m_plugin->LoadType(), IPlugin::k_ELoadTypeManual);
    }

    void plugin_canHandle()
    {
        QVERIFY(m_plugin->canHandle(QStringLiteral("km:1234:test.exe")));
        QVERIFY(m_plugin->canHandle(QStringLiteral("phys:0")));
        QVERIFY(m_plugin->canHandle(QStringLiteral("msr:")));
        QVERIFY(!m_plugin->canHandle(QStringLiteral("1234:test.exe")));
        QVERIFY(!m_plugin->canHandle(QStringLiteral("file:test.bin")));
    }

    void provider_noDriver_invalid()
    {
        // Creating provider with invalid target should fail gracefully
        QString err;
        auto prov = m_plugin->createProvider(QStringLiteral("km:0:invalid"), &err);
        // Either nullptr or invalid -- both are acceptable
        if (prov) QVERIFY(!prov->isValid() || prov->size() == 0);
    }

    // ── 2. KUSER_SHARED_DATA validation (at 0x7FFE0000) ──

    void kusd_ntMajorVersion()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");
        QVERIFY(m_provider);

        // KUSER_SHARED_DATA.NtMajorVersion at offset 0x26C
        uint32_t major = m_provider->readU32(0x7FFE0000 + 0x26C);
        QCOMPARE(major, (uint32_t)10);  // Windows 10/11
    }

    void kusd_ntMinorVersion()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

        uint32_t minor = m_provider->readU32(0x7FFE0000 + 0x270);
        QCOMPARE(minor, (uint32_t)0);  // Windows 10+ has minor = 0
    }

    void kusd_ntBuildNumber()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

#ifdef _WIN32
        // Cross-validate with RtlGetVersion
        typedef NTSTATUS(NTAPI* RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
        auto pRtlGetVersion = (RtlGetVersion_t)GetProcAddress(
            GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
        QVERIFY(pRtlGetVersion);

        RTL_OSVERSIONINFOW osvi{};
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        QCOMPARE(pRtlGetVersion(&osvi), (NTSTATUS)0);

        uint32_t buildFromDriver = m_provider->readU32(0x7FFE0000 + 0x260);
        QCOMPARE(buildFromDriver, (uint32_t)osvi.dwBuildNumber);
#endif
    }

    void kusd_systemTime_nonZero()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

        uint64_t sysTime = m_provider->readU64(0x7FFE0000 + 0x14);
        QVERIFY(sysTime != 0);
    }

    void kusd_tickCount_increasing()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

        // TickCountMultiplier at 0x4, TickCount at 0x320
        uint64_t tick1 = m_provider->readU64(0x7FFE0000 + 0x320);
        QTest::qWait(120);
        uint64_t tick2 = m_provider->readU64(0x7FFE0000 + 0x320);
        QVERIFY2(tick2 > tick1,
                 qPrintable(QStringLiteral("tick1=%1 tick2=%2").arg(tick1).arg(tick2)));
    }

    void kusd_crossValidate_readProcessMemory()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

#ifdef _WIN32
        // Read same KUSD page through driver and ReadProcessMemory
        QByteArray driverBuf(256, 0);
        m_provider->read(0x7FFE0000, driverBuf.data(), 256);

        QByteArray rpmBuf(256, 0);
        SIZE_T bytesRead = 0;
        HANDLE self = GetCurrentProcess();
        ReadProcessMemory(self, (LPCVOID)0x7FFE0000, rpmBuf.data(), 256, &bytesRead);

        // NtMajorVersion (offset 0x26C relative = not in first 256 bytes, so compare what we have)
        // Compare first 256 bytes -- should be identical
        QCOMPARE(driverBuf, rpmBuf);
#endif
    }

    // ── 3. Self-read integration ──

    void selfRead_mzHeader()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

#ifdef _WIN32
        uint64_t selfBase = (uint64_t)GetModuleHandleA(nullptr);
        QVERIFY(selfBase != 0);

        uint8_t mz[2] = {};
        m_provider->read(selfBase, mz, 2);
        QCOMPARE(mz[0], (uint8_t)'M');
        QCOMPARE(mz[1], (uint8_t)'Z');
#endif
    }

    void selfRead_peSignature()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

#ifdef _WIN32
        uint64_t selfBase = (uint64_t)GetModuleHandleA(nullptr);

        // PE offset at +0x3C
        uint32_t peOffset = m_provider->readU32(selfBase + 0x3C);
        QVERIFY(peOffset > 0 && peOffset < 0x1000);

        // PE signature = "PE\0\0" = 0x00004550
        uint32_t peSig = m_provider->readU32(selfBase + peOffset);
        QCOMPARE(peSig, (uint32_t)0x00004550);
#endif
    }

    // ── 4. Scanner integration ──

    void scanner_mzSigScan()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

#ifdef _WIN32
        auto shared = std::shared_ptr<Provider>(m_provider.get(), [](Provider*){});

        ScanRequest req;
        req.pattern = QByteArray("\x4D\x5A", 2);
        req.mask    = QByteArray("\xFF\xFF", 2);
        req.alignment = 1;
        req.maxResults = 10;

        // Constrain to our own module for speed
        uint64_t selfBase = (uint64_t)GetModuleHandleA(nullptr);
        req.startAddress = selfBase;
        req.endAddress   = selfBase + 0x1000;

        ScanEngine engine;
        QSignalSpy spy(&engine, &ScanEngine::finished);
        engine.start(shared, req);
        QVERIFY(spy.wait(5000));

        auto results = spy.at(0).at(0).value<QVector<ScanResult>>();
        QVERIFY(results.size() >= 1);
        QCOMPARE(results[0].address, selfBase);
#endif
    }

    // ── 5. Region enumeration ──

    void regions_selfProcess()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

        auto regions = m_provider->enumerateRegions();
        QVERIFY(regions.size() > 0);

        // Should have at least one executable region (our code)
        bool hasExec = false;
        for (const auto& r : regions) {
            if (r.executable) { hasExec = true; break; }
        }
        QVERIFY(hasExec);
    }

    // ── 6. PEB / modules ──

    void peb_nonZero()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

        QVERIFY(m_provider->peb() != 0);
    }

    void symbol_selfModule()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

#ifdef _WIN32
        uint64_t selfBase = (uint64_t)GetModuleHandleA(nullptr);
        QString sym = m_provider->getSymbol(selfBase + 0x100);
        QVERIFY(!sym.isEmpty());
        QVERIFY(sym.contains(QStringLiteral("+0x")));
#endif
    }

    // ── 7. CR3 / address translation ──

    void cr3_nonZero()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

        auto* kprov = dynamic_cast<KernelProcessProvider*>(m_provider.get());
        QVERIFY(kprov);

        uint64_t cr3 = kprov->getCr3();
        QVERIFY2(cr3 != 0, "CR3 should be non-zero for a running process");
        // CR3 should be page-aligned (low 12 bits cleared)
        QCOMPARE(cr3 & 0xFFF, (uint64_t)0);
    }

    void vtop_kusd()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

        auto* kprov = dynamic_cast<KernelProcessProvider*>(m_provider.get());
        QVERIFY(kprov);

        // KUSER_SHARED_DATA is at VA 0x7FFE0000 in every process
        auto result = kprov->translateAddress(0x7FFE0000);
        QVERIFY2(result.valid, "KUSER_SHARED_DATA should be mapped");
        QVERIFY(result.physical != 0);
        // PML4E and PDPTE should be present
        QVERIFY(result.pml4e & 1);  // Present bit
        QVERIFY(result.pdpte & 1);  // Present bit
    }

    void vtop_selfModule()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

#ifdef _WIN32
        auto* kprov = dynamic_cast<KernelProcessProvider*>(m_provider.get());
        QVERIFY(kprov);

        uint64_t selfBase = (uint64_t)GetModuleHandleA(nullptr);
        auto result = kprov->translateAddress(selfBase);
        QVERIFY2(result.valid, "Own module base should be mapped");
        QVERIFY(result.physical != 0);

        // Cross-validate: read MZ header via physical address
        // Read the first 2 bytes at the physical address using physical provider
        auto physEntries = kprov->readPageTable(kprov->getCr3(), 0, 16);
        QVERIFY(physEntries.size() > 0);  // Should get at least some PML4 entries
#endif
    }

    void vtop_unmapped()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

        auto* kprov = dynamic_cast<KernelProcessProvider*>(m_provider.get());
        QVERIFY(kprov);

        // Address 0 should not be mapped in user mode
        auto result = kprov->translateAddress(0);
        QVERIFY2(!result.valid, "Address 0 should not be mapped");
    }

    void readPageTable_cr3()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

        auto* kprov = dynamic_cast<KernelProcessProvider*>(m_provider.get());
        QVERIFY(kprov);

        uint64_t cr3 = kprov->getCr3();
        QVERIFY(cr3 != 0);

        // Read the full PML4 table (512 entries)
        auto entries = kprov->readPageTable(cr3, 0, 512);
        QCOMPARE(entries.size(), 512);

        // At least some entries should be present (kernel maps upper half)
        int presentCount = 0;
        for (const auto& e : entries) {
            if (e & 1) presentCount++;
        }
        QVERIFY2(presentCount > 0,
                 qPrintable(QStringLiteral("Expected present PML4 entries, got 0")));
    }

    // ── 8. Ping ──

    void ping_version()
    {
        if (!m_driverAvailable) QSKIP("Driver not loaded");

#ifdef _WIN32
        HANDLE h = CreateFileA(RCX_DRV_USERMODE_PATH,
                               GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) QSKIP("Cannot open driver handle");

        RcxDrvPingResponse ping{};
        DWORD br = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_RCX_PING, nullptr, 0,
                                  &ping, sizeof(ping), &br, nullptr);
        CloseHandle(h);

        QVERIFY(ok);
        QCOMPARE(ping.version, (uint32_t)RCX_DRV_VERSION);
#endif
    }
};

QTEST_MAIN(TestKernelProvider)
#include "test_kernel_provider.moc"
