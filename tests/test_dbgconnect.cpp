#include <cstdio>
#include <cstdint>
#include <windows.h>
#include <initguid.h>
#include <dbgeng.h>

int main()
{
    const char* connStr = "tcp:Port=5057,Server=localhost";
    printf("Attempting DebugConnect(\"%s\")...\n", connStr);

    IDebugClient* client = nullptr;
    HRESULT hr = DebugConnect(connStr, IID_IDebugClient, (void**)&client);
    printf("DebugConnect returned: 0x%08lX\n", hr);

    if (SUCCEEDED(hr) && client) {
        printf("Connected! Getting IDebugDataSpaces...\n");

        IDebugDataSpaces* ds = nullptr;
        hr = client->QueryInterface(IID_IDebugDataSpaces, (void**)&ds);
        printf("QueryInterface(IDebugDataSpaces) = 0x%08lX\n", hr);

        if (ds) {
            IDebugControl* ctrl = nullptr;
            client->QueryInterface(IID_IDebugControl, (void**)&ctrl);

            if (ctrl) {
                printf("Waiting for event...\n");
                hr = ctrl->WaitForEvent(0, 5000);
                printf("WaitForEvent = 0x%08lX\n", hr);
                ctrl->Release();
            }

            // Try to read 2 bytes
            IDebugSymbols* sym = nullptr;
            client->QueryInterface(IID_IDebugSymbols, (void**)&sym);
            if (sym) {
                ULONG numMods = 0, numUnloaded = 0;
                hr = sym->GetNumberModules(&numMods, &numUnloaded);
                printf("GetNumberModules = 0x%08lX, numMods=%lu\n", hr, numMods);

                if (numMods > 0) {
                    ULONG64 base = 0;
                    hr = sym->GetModuleByIndex(0, &base);
                    printf("Module[0] base = 0x%llX (hr=0x%08lX)\n", base, hr);

                    if (SUCCEEDED(hr) && base) {
                        uint8_t buf[4] = {};
                        ULONG got = 0;
                        hr = ds->ReadVirtual(base, buf, 4, &got);
                        printf("ReadVirtual(%llX, 4) = 0x%08lX, got=%lu, data=[%02X %02X %02X %02X]\n",
                               base, hr, got, buf[0], buf[1], buf[2], buf[3]);
                    }
                }
                sym->Release();
            }
            ds->Release();
        }
        client->Release();
    } else {
        printf("DebugConnect FAILED. hr=0x%08lX\n", hr);
    }

    return 0;
}
