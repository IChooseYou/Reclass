#include "iplugin.h"
#include "providers/provider.h"
#include <QCoreApplication>
#include <QLibrary>
#include <QtTest>
#include <windows.h>
#include <array>

class TestProcessMemoryRead : public QObject {
    Q_OBJECT
    QLibrary library;
    std::unique_ptr<IPlugin> plugin;
    std::unique_ptr<rcx::Provider> provider;
private slots:
    void initTestCase() {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        library.setFileName(QCoreApplication::applicationDirPath() + "/Plugins/libProcessMemoryPlugin.dll");
        QVERIFY2(library.load(), qPrintable(library.errorString()));
        const auto factory = reinterpret_cast<CreatePluginFunc>(library.resolve("CreatePlugin"));
        QVERIFY(factory);
        plugin.reset(factory());
        auto* source = dynamic_cast<IProviderPlugin*>(plugin.get());
        QVERIFY(source);
        QString error;
        provider = source->createProvider(QString::number(GetCurrentProcessId()) + ":read-test", &error);
        QVERIFY2(provider, qPrintable(error));
    }

    void invalidAddressesDoNotOverwriteDestination() {
        // The first address is the speculative Itanium RTTI read in the
        // BouncingBall AUTO crash dump. Exercise the shipped DLL itself.
        for (uint64_t address : {0xc11cf5c2fffffff8ULL, 0ULL, 0xfffffffffffffff8ULL,
                                 0x0000800000000000ULL, 0xffff800000000000ULL}) {
            std::array<unsigned char, 24> output;
            output.fill(0xa5);
            QVERIFY(!provider->read(address, output.data() + 8, 8));
            for (int i = 0; i < 8; ++i) {
                QCOMPARE(output[i], static_cast<unsigned char>(0xa5));
                QCOMPARE(output[i + 16], static_cast<unsigned char>(0xa5));
            }
        }
    }

    void completeAndPartialReads() {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const SIZE_T page = info.dwPageSize;
        auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 2,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        QVERIFY(memory);
        const auto release = qScopeGuard([&] { VirtualFree(memory, 0, MEM_RELEASE); });
        std::memset(memory, 0x3c, page);
        DWORD oldProtect = 0;
        QVERIFY(VirtualProtect(memory + page, page, PAGE_NOACCESS, &oldProtect));
        std::array<unsigned char, 24> output;
        output.fill(0xa5);
        QVERIFY(provider->read(uint64_t(memory + page - 8), output.data() + 8, 8));
        for (int i = 8; i < 16; ++i) QCOMPARE(output[i], static_cast<unsigned char>(0x3c));
        // Partial reads must not be accepted as complete type evidence.
        QVERIFY(!provider->read(uint64_t(memory + page - 4), output.data() + 8, 8));
        for (int i = 0; i < 8; ++i) {
            QCOMPARE(output[i], static_cast<unsigned char>(0xa5));
            QCOMPARE(output[i + 16], static_cast<unsigned char>(0xa5));
        }
    }
};

QTEST_GUILESS_MAIN(TestProcessMemoryRead)
#include "test_process_memory_read.moc"
