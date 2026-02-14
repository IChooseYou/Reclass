/**
 * test_com_security.cpp — DebugConnect transport diagnostic
 *
 * Tests EVERY transport to find what works from MinGW:
 *   1. TCP to WinDbg .server (port 5055)
 *   2. Named pipe to WinDbg .server
 *   3. TCP with various COM security configs
 *   4. DebugCreate local (baseline)
 *
 * SETUP: In WinDbg, run BOTH of these:
 *   .server tcp:port=5055
 *   .server npipe:pipe=reclass
 *
 * Then run this test.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <dbgeng.h>
#endif

#ifdef _WIN32
static void try_connect(const char* label, const char* connStr)
{
    printf("  %-40s → ", label);
    fflush(stdout);

    IDebugClient* client = nullptr;
    HRESULT hr = DebugConnect(connStr, IID_IDebugClient, (void**)&client);

    if (SUCCEEDED(hr) && client) {
        printf("SUCCESS (hr=0x%08lX)\n", (unsigned long)hr);

        // Try to get data spaces and read something
        IDebugDataSpaces* ds = nullptr;
        IDebugSymbols* sym = nullptr;
        IDebugControl* ctrl = nullptr;
        client->QueryInterface(IID_IDebugDataSpaces, (void**)&ds);
        client->QueryInterface(IID_IDebugSymbols, (void**)&sym);
        client->QueryInterface(IID_IDebugControl, (void**)&ctrl);

        if (ctrl) {
            HRESULT hrWait = ctrl->WaitForEvent(0, 5000);
            printf("    WaitForEvent: hr=0x%08lX\n", (unsigned long)hrWait);
        }

        if (sym) {
            ULONG numMods = 0, numUnloaded = 0;
            sym->GetNumberModules(&numMods, &numUnloaded);
            printf("    Modules: %lu loaded\n", numMods);

            if (numMods > 0 && ds) {
                ULONG64 base = 0;
                sym->GetModuleByIndex(0, &base);
                unsigned char buf[2] = {};
                ULONG got = 0;
                ds->ReadVirtual(base, buf, 2, &got);
                printf("    Read at 0x%llX: got=%lu bytes=[%02X %02X]\n",
                       (unsigned long long)base, got, buf[0], buf[1]);
            }
        }

        if (sym) sym->Release();
        if (ds) ds->Release();
        if (ctrl) ctrl->Release();
        client->Release();
    } else {
        char buf[256] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, (DWORD)hr, 0, buf, sizeof(buf), nullptr);
        for (char* p = buf + strlen(buf) - 1; p >= buf && (*p == '\r' || *p == '\n'); --p)
            *p = '\0';
        printf("FAIL hr=0x%08lX (%s)\n", (unsigned long)hr, buf);
    }
}
#endif

int main()
{
#ifdef _WIN32
    char hostname[256] = {};
    DWORD hsize = sizeof(hostname);
    GetComputerNameA(hostname, &hsize);

    printf("=== DebugConnect Transport Diagnostic ===\n");
    printf("Machine: %s\n\n", hostname);

    // ── Baseline: DebugCreate (local) ──
    printf("[1] DebugCreate (local, no network)\n");
    {
        IDebugClient* client = nullptr;
        HRESULT hr = DebugCreate(IID_IDebugClient, (void**)&client);
        printf("  DebugCreate: %s (hr=0x%08lX)\n\n",
               SUCCEEDED(hr) ? "OK" : "FAIL", (unsigned long)hr);
        if (client) client->Release();
    }

    // ── TCP variants ──
    printf("[2] TCP connections (need: .server tcp:port=5055)\n");
    try_connect("tcp:Port=5055,Server=localhost",
                "tcp:Port=5055,Server=localhost");
    try_connect("tcp:Port=5055,Server=127.0.0.1",
                "tcp:Port=5055,Server=127.0.0.1");
    {
        char conn[512];
        snprintf(conn, sizeof(conn), "tcp:Port=5055,Server=%s", hostname);
        try_connect(conn, conn);
    }
    printf("\n");

    // ── Named pipe variants ──
    printf("[3] Named pipe connections (need: .server npipe:pipe=reclass)\n");
    try_connect("npipe:Pipe=reclass,Server=localhost",
                "npipe:Pipe=reclass,Server=localhost");
    {
        char conn[512];
        snprintf(conn, sizeof(conn), "npipe:Pipe=reclass,Server=%s", hostname);
        try_connect(conn, conn);
    }
    try_connect("npipe:Pipe=reclass",
                "npipe:Pipe=reclass");
    printf("\n");

    // ── TCP with COM security ──
    printf("[4] TCP with explicit COM init (MTA + IMPERSONATE)\n");
    {
        // This runs in-process so CoInitialize affects subsequent calls
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoInitializeSecurity(
            nullptr, -1, nullptr, nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE, nullptr);
        try_connect("tcp:Port=5055,Server=localhost (MTA+SEC)",
                    "tcp:Port=5055,Server=localhost");
        try_connect("npipe:Pipe=reclass (MTA+SEC)",
                    "npipe:Pipe=reclass,Server=localhost");
        CoUninitialize();
    }
    printf("\n");

    // ── Check if dbgeng.dll is the system one ──
    printf("[5] DbgEng DLL info\n");
    {
        HMODULE hmod = GetModuleHandleA("dbgeng.dll");
        if (hmod) {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(hmod, path, MAX_PATH);
            printf("  dbgeng.dll loaded from: %s\n", path);

            // Get version
            DWORD verSize = GetFileVersionInfoSizeA(path, nullptr);
            if (verSize > 0) {
                auto* verData = (char*)malloc(verSize);
                if (GetFileVersionInfoA(path, 0, verSize, verData)) {
                    VS_FIXEDFILEINFO* fileInfo = nullptr;
                    UINT len = 0;
                    if (VerQueryValueA(verData, "\\", (void**)&fileInfo, &len)) {
                        printf("  Version: %d.%d.%d.%d\n",
                               HIWORD(fileInfo->dwFileVersionMS),
                               LOWORD(fileInfo->dwFileVersionMS),
                               HIWORD(fileInfo->dwFileVersionLS),
                               LOWORD(fileInfo->dwFileVersionLS));
                    }
                }
                free(verData);
            }
        } else {
            printf("  dbgeng.dll not loaded yet\n");
        }
    }

    printf("\n=== Done ===\n");
    return 0;
#else
    printf("Windows only.\n");
    return 0;
#endif
}
