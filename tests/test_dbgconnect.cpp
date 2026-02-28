#include <cstdio>
#include <cstdint>
#include <windows.h>
#include <initguid.h>
#include <dbgeng.h>

int main(int argc, char* argv[])
{
    const char* connStr = "tcp:Port=5055,Server=localhost";
    if (argc > 1) connStr = argv[1];

    // Initialize COM — required for DbgEng remote transport (TCP/named-pipe)
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    printf("CoInitializeEx: 0x%08lX\n", hrCom);
    fflush(stdout);

    printf("Attempting DebugConnect(\"%s\")...\n", connStr);
    fflush(stdout);

    IDebugClient* client = nullptr;
    HRESULT hr = DebugConnect(connStr, IID_IDebugClient, (void**)&client);
    printf("DebugConnect returned: 0x%08lX\n", hr);
    fflush(stdout);

    if (SUCCEEDED(hr) && client) {
        printf("Connected! Getting interfaces...\n");
        fflush(stdout);

        IDebugDataSpaces* ds = nullptr;
        hr = client->QueryInterface(IID_IDebugDataSpaces, (void**)&ds);
        printf("QueryInterface(IDebugDataSpaces) = 0x%08lX\n", hr);
        fflush(stdout);

        IDebugControl* ctrl = nullptr;
        client->QueryInterface(IID_IDebugControl, (void**)&ctrl);

        if (ctrl) {
            printf("Calling WaitForEvent(5000ms)...\n");
            fflush(stdout);
            hr = ctrl->WaitForEvent(0, 5000);
            printf("WaitForEvent = 0x%08lX\n", hr);
            fflush(stdout);

            ULONG debugClass = 0, debugQual = 0;
            hr = ctrl->GetDebuggeeType(&debugClass, &debugQual);
            printf("GetDebuggeeType = 0x%08lX, class=%lu, qualifier=%lu\n",
                   hr, debugClass, debugQual);
            printf("  -> %s\n", debugQual >= 1024 ? "DUMP" : "LIVE");
            fflush(stdout);
        }

        IDebugSymbols* sym = nullptr;
        client->QueryInterface(IID_IDebugSymbols, (void**)&sym);

        if (sym) {
            ULONG numMods = 0, numUnloaded = 0;
            hr = sym->GetNumberModules(&numMods, &numUnloaded);
            printf("GetNumberModules = 0x%08lX, loaded=%lu, unloaded=%lu\n",
                   hr, numMods, numUnloaded);
            fflush(stdout);

            if (numMods > 0) {
                ULONG64 base = 0;
                hr = sym->GetModuleByIndex(0, &base);
                printf("Module[0] base = 0x%llX (hr=0x%08lX)\n", base, hr);
                fflush(stdout);

                if (SUCCEEDED(hr) && base && ds) {
                    uint8_t buf[4] = {};
                    ULONG got = 0;
                    hr = ds->ReadVirtual(base, buf, 4, &got);
                    printf("ReadVirtual(0x%llX, 4) = 0x%08lX, got=%lu, data=[%02X %02X %02X %02X]\n",
                           base, hr, got, buf[0], buf[1], buf[2], buf[3]);
                    fflush(stdout);
                }
            }
            sym->Release();
        }

        if (ds) ds->Release();
        if (ctrl) ctrl->Release();

        printf("Disconnecting...\n");
        fflush(stdout);
        client->EndSession(DEBUG_END_DISCONNECT);
        client->Release();
        printf("Done.\n");
    } else {
        printf("DebugConnect FAILED. hr=0x%08lX\n", hr);
    }
    fflush(stdout);

    if (SUCCEEDED(hrCom)) CoUninitialize();
    return 0;
}
