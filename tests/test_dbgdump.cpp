#include <cstdio>
#include <cstdint>
#include <windows.h>
#include <initguid.h>
#include <dbgeng.h>

int main(int argc, char* argv[])
{
    const char* dumpPath = "F:\\MEMORY_EaService2024.DMP";
    if (argc > 1) dumpPath = argv[1];

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    printf("CoInitializeEx: 0x%08lX\n", hrCom);
    fflush(stdout);

    IDebugClient* client = nullptr;
    HRESULT hr = DebugCreate(IID_IDebugClient, (void**)&client);
    printf("DebugCreate: 0x%08lX, client=%p\n", hr, (void*)client);
    fflush(stdout);

    if (FAILED(hr) || !client) {
        printf("FAILED to create debug client\n");
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return 1;
    }

    printf("Opening dump: %s\n", dumpPath);
    fflush(stdout);
    hr = client->OpenDumpFile(dumpPath);
    printf("OpenDumpFile: 0x%08lX\n", hr);
    fflush(stdout);

    if (FAILED(hr)) {
        printf("FAILED to open dump\n");
        client->Release();
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return 1;
    }

    IDebugControl* ctrl = nullptr;
    client->QueryInterface(IID_IDebugControl, (void**)&ctrl);

    if (ctrl) {
        printf("WaitForEvent(10s)...\n");
        fflush(stdout);
        hr = ctrl->WaitForEvent(0, 10000);
        printf("WaitForEvent: 0x%08lX\n", hr);
        fflush(stdout);

        ULONG debugClass = 0, debugQual = 0;
        hr = ctrl->GetDebuggeeType(&debugClass, &debugQual);
        printf("GetDebuggeeType: 0x%08lX, class=%lu, qualifier=%lu\n",
               hr, debugClass, debugQual);
        printf("  -> %s\n", debugQual >= 1024 ? "DUMP" : "LIVE");
        fflush(stdout);
    }

    IDebugDataSpaces* ds = nullptr;
    client->QueryInterface(IID_IDebugDataSpaces, (void**)&ds);

    IDebugSymbols* sym = nullptr;
    client->QueryInterface(IID_IDebugSymbols, (void**)&sym);

    if (sym) {
        ULONG numMods = 0, numUnloaded = 0;
        hr = sym->GetNumberModules(&numMods, &numUnloaded);
        printf("GetNumberModules: 0x%08lX, loaded=%lu, unloaded=%lu\n",
               hr, numMods, numUnloaded);
        fflush(stdout);

        if (numMods > 0) {
            ULONG64 base = 0;
            hr = sym->GetModuleByIndex(0, &base);
            printf("Module[0] base: 0x%llX (hr=0x%08lX)\n", base, hr);
            fflush(stdout);

            if (SUCCEEDED(hr) && base && ds) {
                uint8_t buf[16] = {};
                ULONG got = 0;
                hr = ds->ReadVirtual(base, buf, 16, &got);
                printf("ReadVirtual(0x%llX, 16): hr=0x%08lX, got=%lu\n", base, hr, got);
                printf("  data: ");
                for (int i = 0; i < 16; i++) printf("%02X ", buf[i]);
                printf("\n");
                fflush(stdout);
            }
        }
    }

    // Try reading kernel base directly
    uint64_t ntBase = 0xfffff80123c00000ULL;
    if (ds) {
        uint8_t buf[16] = {};
        ULONG got = 0;
        hr = ds->ReadVirtual(ntBase, buf, 16, &got);
        printf("ReadVirtual(nt base 0x%llX, 16): hr=0x%08lX, got=%lu\n", ntBase, hr, got);
        printf("  data: ");
        for (int i = 0; i < 16; i++) printf("%02X ", buf[i]);
        printf("\n");
        fflush(stdout);
    }

    if (sym) sym->Release();
    if (ds) ds->Release();
    if (ctrl) ctrl->Release();
    client->DetachProcesses();
    client->Release();

    printf("Done.\n");
    if (SUCCEEDED(hrCom)) CoUninitialize();
    return 0;
}
