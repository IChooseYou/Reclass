#include <QTest>
#ifdef _WIN32
#include "providers/process_provider.h"

using namespace rcx;

class TestProcessProviderSymbol : public QObject {
    Q_OBJECT

private slots:

    void getSymbol_selfProcess() {
        // Attach to our own process for testing
        HANDLE self = GetCurrentProcess();

        // DuplicateHandle to get a real handle we can pass
        HANDLE hReal = nullptr;
        DuplicateHandle(self, self, self, &hReal, 0, FALSE, DUPLICATE_SAME_ACCESS);

        HMODULE hMod = nullptr;
        DWORD needed = 0;
        EnumProcessModulesEx(hReal, &hMod, sizeof(hMod), &needed, LIST_MODULES_ALL);

        MODULEINFO mi{};
        GetModuleInformation(hReal, hMod, &mi, sizeof(mi));
        uint64_t base = (uint64_t)mi.lpBaseOfDll;
        int regionSize = (int)mi.SizeOfImage;

        // ProcessProvider takes ownership of the handle
        ProcessProvider prov(hReal, base, regionSize, "self_test");

        QCOMPARE(prov.kind(), QStringLiteral("Process"));
        QCOMPARE(prov.name(), QStringLiteral("self_test"));
        QVERIFY(prov.isValid());
        QVERIFY(prov.size() > 0);

        // getSymbol for our own base address should resolve to our exe name
        QString sym = prov.getSymbol(base);
        QVERIFY(!sym.isEmpty());
        // Should contain +0x
        QVERIFY(sym.contains("+0x"));

        // getSymbol for a bogus address should return empty
        QString bogus = prov.getSymbol(0xDEAD);
        QVERIFY(bogus.isEmpty());

        // Read our own PE signature as a sanity check
        // (first two bytes of any PE are 'MZ')
        uint16_t mz = prov.readU16(0);
        QCOMPARE(mz, (uint16_t)0x5A4D); // 'MZ' in little-endian
    }

    void getSymbol_ntdllResolvable() {
        // ntdll is loaded in every process
        HANDLE self = GetCurrentProcess();
        HANDLE hReal = nullptr;
        DuplicateHandle(self, self, self, &hReal, 0, FALSE, DUPLICATE_SAME_ACCESS);

        HMODULE mods[256];
        DWORD needed = 0;
        EnumProcessModulesEx(hReal, mods, sizeof(mods), &needed, LIST_MODULES_ALL);

        // Find ntdll
        uint64_t ntdllBase = 0;
        int count = (int)(needed / sizeof(HMODULE));
        for (int i = 0; i < count; ++i) {
            WCHAR name[MAX_PATH];
            if (GetModuleBaseNameW(hReal, mods[i], name, MAX_PATH)) {
                if (QString::fromWCharArray(name).toLower() == "ntdll.dll") {
                    MODULEINFO mi{};
                    GetModuleInformation(hReal, mods[i], &mi, sizeof(mi));
                    ntdllBase = (uint64_t)mi.lpBaseOfDll;
                    break;
                }
            }
        }
        QVERIFY(ntdllBase != 0);

        // Use main module as the "base" for the provider
        MODULEINFO mainMi{};
        GetModuleInformation(hReal, mods[0], &mainMi, sizeof(mainMi));

        ProcessProvider prov(hReal, (uint64_t)mainMi.lpBaseOfDll,
                             (int)mainMi.SizeOfImage, "self_test");

        // Resolve ntdll base -- should return "ntdll.dll+0x0"
        QString sym = prov.getSymbol(ntdllBase);
        QVERIFY(sym.toLower().startsWith("ntdll.dll+0x"));
    }
};

QTEST_MAIN(TestProcessProviderSymbol)
#include "test_provider_getSymbol.moc"

#else
// Non-Windows: empty test that passes
#include <QTest>
class TestProcessProviderSymbol : public QObject {
    Q_OBJECT
private slots:
    void skip() { QSKIP("ProcessProvider tests are Windows-only"); }
};
QTEST_MAIN(TestProcessProviderSymbol)
#include "test_provider_getSymbol.moc"
#endif
