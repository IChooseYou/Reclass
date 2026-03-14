#include "RemoteProcessMemoryPlugin.h"
#include "rcx_rpc_protocol.h"
#include "../../src/processpicker.h"

#include <QStyle>
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QDir>
#include <QFileInfo>
#include <QPixmap>
#include <QImage>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && defined(_WIN32)
#include <QtWin>
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <tlhelp32.h>
#  include <psapi.h>
#  include <shellapi.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <dlfcn.h>
#  include <sys/mman.h>
#  include <sys/wait.h>
#  include <sys/ptrace.h>
#  include <sys/user.h>
#  include <semaphore.h>
#  include <signal.h>
#  include <link.h>
#  include <climits>
#  include <cstring>
#  include <fstream>
#  include <sstream>
#endif

/* ══════════════════════════════════════════════════════════════════════
 *  IPC Client
 * ══════════════════════════════════════════════════════════════════════ */

struct IpcClient {
#ifdef _WIN32
    HANDLE hShm      = nullptr;
    HANDLE hReqEvent  = nullptr;
    HANDLE hRspEvent  = nullptr;
#else
    int    shmFd      = -1;
    sem_t* reqSem     = SEM_FAILED;
    sem_t* rspSem     = SEM_FAILED;
    char   shmNameBuf[128] = {};
    char   reqNameBuf[128] = {};
    char   rspNameBuf[128] = {};
#endif
    void*  mappedView = nullptr;
    QMutex mutex;
    bool   connected  = false;

    RcxRpcHeader* header() const {
        return mappedView ? reinterpret_cast<RcxRpcHeader*>(mappedView) : nullptr;
    }

    ~IpcClient() { disconnect(); }

    /* ── connect / disconnect ──────────────────────────────────────── */

    bool connect(uint32_t pid, int timeoutMs = 5000)
    {
        char shmName[128], reqName[128], rspName[128];
        rcx_rpc_shm_name(shmName, sizeof(shmName), pid);
        rcx_rpc_req_name(reqName, sizeof(reqName), pid);
        rcx_rpc_rsp_name(rspName, sizeof(rspName), pid);

#ifdef _WIN32
        /* poll for shared memory to appear (payload creating it) */
        auto deadline = GetTickCount64() + (uint64_t)timeoutMs;
        while (!(hShm = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shmName))) {
            if (GetTickCount64() >= deadline) return false;
            Sleep(10);
        }

        mappedView = MapViewOfFile(hShm, FILE_MAP_ALL_ACCESS, 0, 0, RCX_RPC_SHM_SIZE);
        if (!mappedView) { CloseHandle(hShm); hShm = nullptr; return false; }

        hReqEvent = OpenEventA(EVENT_ALL_ACCESS, FALSE, reqName);
        hRspEvent = OpenEventA(EVENT_ALL_ACCESS, FALSE, rspName);
        if (!hReqEvent || !hRspEvent) { disconnect(); return false; }
#else
        strncpy(shmNameBuf, shmName, sizeof(shmNameBuf) - 1);
        strncpy(reqNameBuf, reqName, sizeof(reqNameBuf) - 1);
        strncpy(rspNameBuf, rspName, sizeof(rspNameBuf) - 1);

        /* poll for shared memory */
        auto start = std::chrono::steady_clock::now();
        while (true) {
            shmFd = shm_open(shmName, O_RDWR, 0);
            if (shmFd >= 0) break;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeoutMs) return false;
            usleep(10000);
        }

        mappedView = mmap(nullptr, RCX_RPC_SHM_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED, shmFd, 0);
        if (mappedView == MAP_FAILED) { mappedView = nullptr; close(shmFd); shmFd = -1; return false; }

        reqSem = sem_open(reqName, 0);
        rspSem = sem_open(rspName, 0);
        if (reqSem == SEM_FAILED || rspSem == SEM_FAILED) { disconnect(); return false; }
#endif

        /* wait for payloadReady */
        auto* hdr = static_cast<RcxRpcHeader*>(mappedView);
#ifdef _WIN32
        while (!hdr->payloadReady) {
            if (GetTickCount64() >= deadline) { disconnect(); return false; }
            Sleep(5);
        }
#else
        while (!__atomic_load_n(&hdr->payloadReady, __ATOMIC_ACQUIRE)) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeoutMs) { disconnect(); return false; }
            usleep(5000);
        }
#endif

        connected = true;
        return true;
    }

    void disconnect()
    {
#ifdef _WIN32
        if (mappedView) { UnmapViewOfFile(mappedView); mappedView = nullptr; }
        if (hShm)       { CloseHandle(hShm);       hShm       = nullptr; }
        if (hReqEvent)  { CloseHandle(hReqEvent);   hReqEvent  = nullptr; }
        if (hRspEvent)  { CloseHandle(hRspEvent);   hRspEvent  = nullptr; }
#else
        if (mappedView) { munmap(mappedView, RCX_RPC_SHM_SIZE); mappedView = nullptr; }
        if (shmFd >= 0) { close(shmFd); shmFd = -1; }
        if (reqSem != SEM_FAILED) { sem_close(reqSem); reqSem = SEM_FAILED; }
        if (rspSem != SEM_FAILED) { sem_close(rspSem); rspSem = SEM_FAILED; }
#endif
        connected = false;
    }

    /* ── low-level RPC round-trip ──────────────────────────────────── */

    bool signalAndWait(int timeoutMs = 2000)
    {
#ifdef _WIN32
        SetEvent(hReqEvent);
        return WaitForSingleObject(hRspEvent, (DWORD)timeoutMs) == WAIT_OBJECT_0;
#else
        sem_post(reqSem);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeoutMs / 1000;
        ts.tv_nsec += (timeoutMs % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        return sem_timedwait(rspSem, &ts) == 0;
#endif
    }

    /* ── public API ────────────────────────────────────────────────── */

    bool readSingle(uint64_t addr, void* buf, int len)
    {
        QMutexLocker lock(&mutex);
        if (!connected || len <= 0) return false;

        auto* hdr  = static_cast<RcxRpcHeader*>(mappedView);
        auto* data = static_cast<uint8_t*>(mappedView) + RCX_RPC_DATA_OFFSET;

        hdr->command      = RPC_CMD_READ_BATCH;
        hdr->requestCount = 1;
        hdr->status       = RCX_RPC_STATUS_OK;

        auto* entry       = reinterpret_cast<RcxRpcReadEntry*>(data);
        entry->address    = addr;
        entry->length     = (uint32_t)len;
        entry->dataOffset = sizeof(RcxRpcReadEntry);

        if (!signalAndWait()) { connected = false; return false; }

        memcpy(buf, data + entry->dataOffset, len);
        return true;
    }

    bool writeSingle(uint64_t addr, const void* buf, int len)
    {
        QMutexLocker lock(&mutex);
        if (!connected || len <= 0) return false;

        auto* hdr  = static_cast<RcxRpcHeader*>(mappedView);
        auto* data = static_cast<uint8_t*>(mappedView) + RCX_RPC_DATA_OFFSET;

        hdr->command      = RPC_CMD_WRITE;
        hdr->writeAddress = addr;
        hdr->writeLength  = (uint32_t)len;
        hdr->status       = RCX_RPC_STATUS_OK;

        memcpy(data, buf, len);

        if (!signalAndWait()) { connected = false; return false; }

        return hdr->status == RCX_RPC_STATUS_OK;
    }

    QVector<RemoteProcessProvider::ModuleInfo> enumerateModules()
    {
        QVector<RemoteProcessProvider::ModuleInfo> result;
        QMutexLocker lock(&mutex);
        if (!connected) return result;

        auto* hdr  = static_cast<RcxRpcHeader*>(mappedView);
        auto* data = static_cast<uint8_t*>(mappedView) + RCX_RPC_DATA_OFFSET;

        hdr->command = RPC_CMD_ENUM_MODULES;
        hdr->status  = RCX_RPC_STATUS_OK;

        if (!signalAndWait()) { connected = false; return result; }
        if (hdr->status != RCX_RPC_STATUS_OK) return result;

        uint32_t count = hdr->responseCount;
        result.reserve((int)count);

        for (uint32_t i = 0; i < count; ++i) {
            auto* entry = reinterpret_cast<const RcxRpcModuleEntry*>(
                data + i * sizeof(RcxRpcModuleEntry));

            QString modName;
#ifdef _WIN32
            modName = QString::fromWCharArray(
                reinterpret_cast<const wchar_t*>(data + entry->nameOffset),
                (int)(entry->nameLength / sizeof(wchar_t)));
#else
            modName = QString::fromUtf8(
                reinterpret_cast<const char*>(data + entry->nameOffset),
                (int)entry->nameLength);
#endif
            result.push_back(RemoteProcessProvider::ModuleInfo{modName, entry->base, entry->size});
        }
        return result;
    }

    bool ping()
    {
        QMutexLocker lock(&mutex);
        if (!connected) return false;

        auto* hdr     = static_cast<RcxRpcHeader*>(mappedView);
        hdr->command  = RPC_CMD_PING;
        hdr->status   = RCX_RPC_STATUS_OK;

        if (!signalAndWait()) { connected = false; return false; }
        return true;
    }

    void shutdown()
    {
        QMutexLocker lock(&mutex);
        if (!connected) return;

        auto* hdr     = static_cast<RcxRpcHeader*>(mappedView);
        hdr->command  = RPC_CMD_SHUTDOWN;
        hdr->status   = RCX_RPC_STATUS_OK;

        signalAndWait(500);
        connected = false;
    }
};

/* ══════════════════════════════════════════════════════════════════════
 *  RemoteProcessProvider
 * ══════════════════════════════════════════════════════════════════════ */

RemoteProcessProvider::RemoteProcessProvider(
        uint32_t pid, const QString& processName,
        std::shared_ptr<IpcClient> ipc)
    : m_pid(pid)
    , m_processName(processName)
    , m_connected(ipc && ipc->connected)
    , m_base(0)
    , m_ipc(std::move(ipc))
{
    if (m_connected) {
        cacheModules();
        // Read pointer size from payload's SHM header (0 means not set → default 8)
        auto* hdr = m_ipc ? m_ipc->header() : nullptr;
        if (hdr) {
            uint32_t ps = hdr->pointerSize;
            if (ps == 4 || ps == 8)
                m_pointerSize = (int)ps;
        }
    }
}

RemoteProcessProvider::~RemoteProcessProvider() = default;

bool RemoteProcessProvider::read(uint64_t addr, void* buf, int len) const
{
    if (!m_connected || len <= 0) return false;
    bool ok = m_ipc->readSingle(addr, buf, len);
    if (!ok) {
        memset(buf, 0, (size_t)len);
        /* update connectivity flag through mutable ipc */
        const_cast<RemoteProcessProvider*>(this)->m_connected = m_ipc->connected;
    }
    return ok;
}

int RemoteProcessProvider::size() const
{
    return m_connected ? 0x10000 : 0;
}

bool RemoteProcessProvider::write(uint64_t addr, const void* buf, int len)
{
    if (!m_connected || len <= 0) return false;
    bool ok = m_ipc->writeSingle(addr, buf, len);
    if (!ok) m_connected = m_ipc->connected;
    return ok;
}

QString RemoteProcessProvider::getSymbol(uint64_t addr) const
{
    for (const auto& mod : m_modules) {
        if (addr >= mod.base && addr < mod.base + mod.size) {
            uint64_t off = addr - mod.base;
            return QStringLiteral("%1+0x%2")
                .arg(mod.name)
                .arg(off, 0, 16, QChar('0'));
        }
    }
    return {};
}

uint64_t RemoteProcessProvider::symbolToAddress(const QString& n) const
{
    for (const auto& mod : m_modules) {
        if (mod.name.compare(n, Qt::CaseInsensitive) == 0)
            return mod.base;
    }
    return 0;
}

void RemoteProcessProvider::cacheModules()
{
    m_modules = m_ipc->enumerateModules();
    if (!m_modules.isEmpty())
        m_base = m_modules.first().base;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Injection helpers
 * ══════════════════════════════════════════════════════════════════════ */

namespace {

/* Resolve payload DLL/SO path next to this plugin DLL/SO */
static QString payloadPath()
{
#ifdef _WIN32
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&payloadPath), &hSelf);
    WCHAR buf[MAX_PATH];
    GetModuleFileNameW(hSelf, buf, MAX_PATH);
    QFileInfo fi(QString::fromWCharArray(buf));
    return fi.absolutePath() + QStringLiteral("/rcx_payload.dll");
#else
    Dl_info info;
    dladdr(reinterpret_cast<void*>(&payloadPath), &info);
    QFileInfo fi(QString::fromUtf8(info.dli_fname));
    return fi.absolutePath() + QStringLiteral("/rcx_payload.so");
#endif
}

#ifdef _WIN32
/* ── Windows injection: CreateRemoteThread + LoadLibraryA ─────────── */

static bool injectPayload(uint32_t pid, QString* errorMsg)
{
    QString path = payloadPath();
    QByteArray pathUtf8 = QDir::toNativeSeparators(path).toLocal8Bit();

    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) {
        if (errorMsg)
            *errorMsg = QStringLiteral("OpenProcess failed (error %1).\n"
                                       "Try running as Administrator.")
                            .arg(GetLastError());
        return false;
    }

    /* allocate + write path string in target */
    SIZE_T pathLen = (SIZE_T)(pathUtf8.size() + 1);
    void* remotePath = VirtualAllocEx(hProc, nullptr, pathLen,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        if (errorMsg) *errorMsg = QStringLiteral("VirtualAllocEx failed.");
        CloseHandle(hProc);
        return false;
    }

    WriteProcessMemory(hProc, remotePath, pathUtf8.constData(), pathLen, nullptr);

    /* Step 1: LoadLibraryA — loads the DLL (DllMain is minimal) */
    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
    auto pLoadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(hK32, "LoadLibraryA"));

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
                                        pLoadLib, remotePath, 0, nullptr);
    if (!hThread) {
        if (errorMsg) *errorMsg = QStringLiteral("CreateRemoteThread failed (error %1).")
                                      .arg(GetLastError());
        VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    WaitForSingleObject(hThread, 10000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);

    if (exitCode == 0) {
        CloseHandle(hProc);
        if (errorMsg) *errorMsg = QStringLiteral("LoadLibrary returned NULL in target.\n"
                                                  "Ensure rcx_payload.dll is in: %1").arg(path);
        return false;
    }

    /* Step 2: Call RcxPayloadInit() — safe to create timer queues now
       (loader lock is no longer held after LoadLibrary returned) */
    HMODULE hPayloadRemote = (HMODULE)(uintptr_t)exitCode;
    auto pGetProcAddr = reinterpret_cast<FARPROC(WINAPI*)(HMODULE, LPCSTR)>(
        GetProcAddress(hK32, "GetProcAddress"));

    /* Write "RcxPayloadInit\0" into target, call GetProcAddress remotely */
    const char initName[] = "RcxPayloadInit";
    void* remoteInitName = VirtualAllocEx(hProc, nullptr, sizeof(initName),
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteInitName) {
        WriteProcessMemory(hProc, remoteInitName, initName, sizeof(initName), nullptr);

        /* We need to call GetProcAddress(hPayload, "RcxPayloadInit") then call the result.
           Simpler approach: write small shellcode that does both calls. */
        uint8_t shellcode[128];
        int off = 0;

        /* sub rsp, 40                   ; shadow space + alignment */
        shellcode[off++] = 0x48; shellcode[off++] = 0x83; shellcode[off++] = 0xEC; shellcode[off++] = 0x28;
        /* mov rcx, hPayloadRemote       ; first arg = module handle */
        shellcode[off++] = 0x48; shellcode[off++] = 0xB9;
        uint64_t hMod = (uint64_t)(uintptr_t)hPayloadRemote;
        memcpy(shellcode + off, &hMod, 8); off += 8;
        /* mov rdx, remoteInitName       ; second arg = "RcxPayloadInit" */
        shellcode[off++] = 0x48; shellcode[off++] = 0xBA;
        uint64_t pName = (uint64_t)(uintptr_t)remoteInitName;
        memcpy(shellcode + off, &pName, 8); off += 8;
        /* mov rax, GetProcAddress       */
        shellcode[off++] = 0x48; shellcode[off++] = 0xB8;
        uint64_t pGPA = (uint64_t)(uintptr_t)pGetProcAddr;
        memcpy(shellcode + off, &pGPA, 8); off += 8;
        /* call rax                      ; rax = RcxPayloadInit */
        shellcode[off++] = 0xFF; shellcode[off++] = 0xD0;
        /* test rax, rax */
        shellcode[off++] = 0x48; shellcode[off++] = 0x85; shellcode[off++] = 0xC0;
        /* jz skip (jump over the call if null) */
        shellcode[off++] = 0x74; shellcode[off++] = 0x02;
        /* call rax                      ; RcxPayloadInit() */
        shellcode[off++] = 0xFF; shellcode[off++] = 0xD0;
        /* skip: add rsp, 40 */
        shellcode[off++] = 0x48; shellcode[off++] = 0x83; shellcode[off++] = 0xC4; shellcode[off++] = 0x28;
        /* ret */
        shellcode[off++] = 0xC3;

        void* remoteCode = VirtualAllocEx(hProc, nullptr, (SIZE_T)off,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (remoteCode) {
            WriteProcessMemory(hProc, remoteCode, shellcode, (SIZE_T)off, nullptr);

            HANDLE hThread2 = CreateRemoteThread(hProc, nullptr, 0,
                (LPTHREAD_START_ROUTINE)remoteCode, nullptr, 0, nullptr);
            if (hThread2) {
                WaitForSingleObject(hThread2, 10000);
                CloseHandle(hThread2);
            }
            VirtualFreeEx(hProc, remoteCode, 0, MEM_RELEASE);
        }
        VirtualFreeEx(hProc, remoteInitName, 0, MEM_RELEASE);
    }

    CloseHandle(hProc);
    return true;
}

#else
/* ── Linux injection: ptrace + dlopen ─────────────────────────────── */

static uint64_t findLibBase(pid_t pid, const char* libName)
{
    char mapsPath[64];
    snprintf(mapsPath, sizeof(mapsPath), "/proc/%d/maps", pid);
    FILE* f = fopen(mapsPath, "r");
    if (!f) return 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, libName)) {
            uint64_t base;
            if (sscanf(line, "%lx-", &base) == 1) {
                fclose(f);
                return base;
            }
        }
    }
    fclose(f);
    return 0;
}

static uint64_t findSyscallInsn(pid_t pid)
{
    char mapsPath[64];
    snprintf(mapsPath, sizeof(mapsPath), "/proc/%d/maps", pid);
    FILE* f = fopen(mapsPath, "r");
    if (!f) return 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libc") && strstr(line, "r-xp")) {
            uint64_t start, end;
            if (sscanf(line, "%lx-%lx", &start, &end) != 2) continue;
            fclose(f);

            /* scan for 0F 05 (syscall) */
            char memPath[64];
            snprintf(memPath, sizeof(memPath), "/proc/%d/mem", pid);
            int memFd = open(memPath, O_RDONLY);
            if (memFd < 0) return 0;

            uint8_t buf[4096];
            for (uint64_t off = start; off < end; off += sizeof(buf)) {
                ssize_t n = pread(memFd, buf, sizeof(buf), (off_t)off);
                if (n <= 1) break;
                for (ssize_t i = 0; i + 1 < n; ++i) {
                    if (buf[i] == 0x0F && buf[i + 1] == 0x05) {
                        close(memFd);
                        return off + (uint64_t)i;
                    }
                }
            }
            close(memFd);
            return 0;
        }
    }
    fclose(f);
    return 0;
}

static bool writeTargetMem(pid_t pid, uint64_t addr, const void* src, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < len; i += sizeof(long)) {
        long val = 0;
        size_t chunk = (len - i < sizeof(long)) ? (len - i) : sizeof(long);
        if (chunk < sizeof(long)) {
            errno = 0;
            val = ptrace(PTRACE_PEEKDATA, pid, (void*)(addr + i), nullptr);
            if (errno) return false;
        }
        memcpy(&val, p + i, chunk);
        if (ptrace(PTRACE_POKEDATA, pid, (void*)(addr + i), (void*)val) < 0)
            return false;
    }
    return true;
}

static bool injectPayload(uint32_t pid, QString* errorMsg)
{
    QString path = payloadPath();
    QByteArray pathUtf8 = path.toUtf8();

    if (ptrace(PTRACE_ATTACH, (pid_t)pid, nullptr, nullptr) < 0) {
        if (errorMsg)
            *errorMsg = QStringLiteral("ptrace attach failed: %1\n"
                                       "Check /proc/sys/kernel/yama/ptrace_scope or run as root.")
                            .arg(strerror(errno));
        return false;
    }

    int status;
    waitpid((pid_t)pid, &status, 0);

    /* save registers */
    struct user_regs_struct savedRegs, regs;
    ptrace(PTRACE_GETREGS, (pid_t)pid, nullptr, &savedRegs);
    regs = savedRegs;

    /* find syscall instruction in target's libc */
    uint64_t syscallAddr = findSyscallInsn((pid_t)pid);
    if (!syscallAddr) {
        ptrace(PTRACE_DETACH, (pid_t)pid, nullptr, nullptr);
        if (errorMsg) *errorMsg = QStringLiteral("Could not find syscall instruction in target.");
        return false;
    }

    /* find dlopen in target via libc offset technique */
    void* ourDlopen = dlsym(RTLD_DEFAULT, "dlopen");
    uint64_t ourLibcBase = findLibBase(getpid(), "libc");
    uint64_t targetLibcBase = findLibBase((pid_t)pid, "libc");

    if (!ourDlopen || !ourLibcBase || !targetLibcBase) {
        ptrace(PTRACE_DETACH, (pid_t)pid, nullptr, nullptr);
        if (errorMsg) *errorMsg = QStringLiteral("Could not resolve dlopen address.");
        return false;
    }

    uint64_t targetDlopen = targetLibcBase + ((uint64_t)ourDlopen - ourLibcBase);

    /* call mmap in target via syscall: mmap(0, 4096, RWX, MAP_PRIVATE|MAP_ANON, -1, 0) */
    regs.rax = 9;        /* __NR_mmap */
    regs.rdi = 0;
    regs.rsi = 4096;
    regs.rdx = 7;        /* PROT_READ|PROT_WRITE|PROT_EXEC */
    regs.r10 = 0x22;     /* MAP_PRIVATE|MAP_ANONYMOUS */
    regs.r8  = (uint64_t)-1;
    regs.r9  = 0;
    regs.rip = syscallAddr;

    ptrace(PTRACE_SETREGS, (pid_t)pid, nullptr, &regs);
    ptrace(PTRACE_SINGLESTEP, (pid_t)pid, nullptr, nullptr);
    waitpid((pid_t)pid, &status, 0);

    ptrace(PTRACE_GETREGS, (pid_t)pid, nullptr, &regs);
    uint64_t mmapPage = regs.rax;

    if ((int64_t)mmapPage < 0 || mmapPage == 0) {
        ptrace(PTRACE_SETREGS, (pid_t)pid, nullptr, &savedRegs);
        ptrace(PTRACE_DETACH, (pid_t)pid, nullptr, nullptr);
        if (errorMsg) *errorMsg = QStringLiteral("mmap in target failed.");
        return false;
    }

    /* write path string at start of page */
    writeTargetMem((pid_t)pid, mmapPage, pathUtf8.constData(), (size_t)(pathUtf8.size() + 1));

    /* write shellcode after path:
     *   mov rdi, pathAddr     (48 BF xxxxxxxx)
     *   mov rsi, 2            (48 BE 02000000 00000000)
     *   mov rax, dlopenAddr   (48 B8 xxxxxxxx)
     *   call rax              (FF D0)
     *   int3                  (CC)
     */
    uint64_t pathAddr = mmapPage;
    uint64_t codeAddr = mmapPage + ((pathUtf8.size() + 1 + 15) & ~15ULL);

    uint8_t sc[64];
    int len = 0;
    /* mov rdi, imm64 */
    sc[len++] = 0x48; sc[len++] = 0xBF;
    memcpy(sc + len, &pathAddr, 8); len += 8;
    /* mov rsi, 2 (RTLD_NOW) */
    sc[len++] = 0x48; sc[len++] = 0xBE;
    uint64_t rtldNow = 2;
    memcpy(sc + len, &rtldNow, 8); len += 8;
    /* mov rax, dlopen */
    sc[len++] = 0x48; sc[len++] = 0xB8;
    memcpy(sc + len, &targetDlopen, 8); len += 8;
    /* call rax */
    sc[len++] = 0xFF; sc[len++] = 0xD0;
    /* int3 */
    sc[len++] = 0xCC;

    writeTargetMem((pid_t)pid, codeAddr, sc, (size_t)len);

    /* execute shellcode */
    regs = savedRegs;
    regs.rip = codeAddr;
    regs.rsp = (mmapPage + 4096) & ~0xFULL;

    ptrace(PTRACE_SETREGS, (pid_t)pid, nullptr, &regs);
    ptrace(PTRACE_CONT, (pid_t)pid, nullptr, nullptr);
    waitpid((pid_t)pid, &status, 0);

    bool ok = false;
    if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
        ptrace(PTRACE_GETREGS, (pid_t)pid, nullptr, &regs);
        ok = (regs.rax != 0);
    }

    /* clean up: munmap the page via syscall */
    struct user_regs_struct cleanRegs = savedRegs;
    cleanRegs.rax = 11;   /* __NR_munmap */
    cleanRegs.rdi = mmapPage;
    cleanRegs.rsi = 4096;
    cleanRegs.rip = syscallAddr;
    ptrace(PTRACE_SETREGS, (pid_t)pid, nullptr, &cleanRegs);
    ptrace(PTRACE_SINGLESTEP, (pid_t)pid, nullptr, nullptr);
    waitpid((pid_t)pid, &status, 0);

    /* restore and detach */
    ptrace(PTRACE_SETREGS, (pid_t)pid, nullptr, &savedRegs);
    ptrace(PTRACE_DETACH, (pid_t)pid, nullptr, nullptr);

    if (!ok && errorMsg)
        *errorMsg = QStringLiteral("dlopen failed in target.\n"
                                    "Ensure payload is at: %1").arg(path);
    return ok;
}
#endif  /* _WIN32 / linux injection */

} /* anonymous namespace */

/* ══════════════════════════════════════════════════════════════════════
 *  RemoteProcessMemoryPlugin
 * ══════════════════════════════════════════════════════════════════════ */

RemoteProcessMemoryPlugin::RemoteProcessMemoryPlugin() = default;
RemoteProcessMemoryPlugin::~RemoteProcessMemoryPlugin() = default;

QIcon RemoteProcessMemoryPlugin::Icon() const
{
    return qApp->style()->standardIcon(QStyle::SP_DriveNetIcon);
}

bool RemoteProcessMemoryPlugin::canHandle(const QString& target) const
{
    return target.startsWith(QStringLiteral("rpm:"));
}

std::unique_ptr<rcx::Provider>
RemoteProcessMemoryPlugin::createProvider(const QString& target, QString* errorMsg)
{
    /* target = "rpm:{pid}:{name}" */
    QStringList parts = target.split(':');
    if (parts.size() < 3 || parts[0] != QStringLiteral("rpm")) {
        if (errorMsg) *errorMsg = QStringLiteral("Invalid target: ") + target;
        return nullptr;
    }

    bool ok;
    uint32_t pid = parts[1].toUInt(&ok);
    QString name = parts.mid(2).join(':');  /* name may contain colons */

    if (!ok || pid == 0) {
        if (errorMsg) *errorMsg = QStringLiteral("Invalid PID in target.");
        return nullptr;
    }

    auto ipc = getOrCreateConnection(pid, errorMsg);
    if (!ipc) return nullptr;

    return std::make_unique<RemoteProcessProvider>(pid, name, ipc);
}

uint64_t RemoteProcessMemoryPlugin::getInitialBaseAddress(const QString& target) const
{
    /* Read imageBase directly from the shared-memory header -- zero IPC cost.
       The payload filled it at init from PEB->Ldr (Win) / /proc/self/maps (Linux). */
    QStringList parts = target.split(':');
    if (parts.size() < 2 || parts[0] != QStringLiteral("rpm"))
        return 0;

    bool ok;
    uint32_t pid = parts[1].toUInt(&ok);
    if (!ok) return 0;

    QMutexLocker lock(&m_connectionsMutex);
    auto it = m_connections.constFind(pid);
    if (it == m_connections.constEnd() || !(*it)->connected)
        return 0;

    auto* hdr = static_cast<const RcxRpcHeader*>((*it)->mappedView);
    return hdr->imageBase;
}

bool RemoteProcessMemoryPlugin::selectTarget(QWidget* parent, QString* target)
{
    /* ── 1. pick a process ── */
    QVector<PluginProcessInfo> pluginProcs = enumerateProcesses();
    QList<ProcessInfo> procs;
    for (const auto& pi : pluginProcs) {
        ProcessInfo info;
        info.pid  = pi.pid;
        info.name = pi.name;
        info.path = pi.path;
        info.icon = pi.icon;
        procs.append(info);
    }

    ProcessPicker picker(procs, parent);
    if (picker.exec() != QDialog::Accepted) return false;

    uint32_t pid  = picker.selectedProcessId();
    QString  name = picker.selectedProcessName();

    /* ── 2. ask inject or connect ── */
    QMessageBox box(parent);
    box.setWindowTitle(QStringLiteral("Remote Process Memory"));
    box.setText(QStringLiteral("Connect to %1 (PID %2)").arg(name).arg(pid));
    box.setInformativeText(QStringLiteral("Choose how to connect to the target:"));
    QAbstractButton* injectBtn  = box.addButton(QStringLiteral("Inject Payload"),   QMessageBox::ActionRole);
    QAbstractButton* connectBtn = box.addButton(QStringLiteral("Already Injected"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();

    QAbstractButton* clicked = box.clickedButton();
    if (clicked == injectBtn) {
        QString injectErr;
        if (!injectPayload(pid, &injectErr)) {
            QMessageBox::critical(parent, QStringLiteral("Injection Failed"), injectErr);
            return false;
        }

        *target = QStringLiteral("rpm:%1:%2").arg(pid).arg(name);
        return true;
    }
    else if (clicked == connectBtn) {
        *target = QStringLiteral("rpm:%1:%2").arg(pid).arg(name);
        return true;
    }

    return false;
}

QVector<PluginProcessInfo> RemoteProcessMemoryPlugin::enumerateProcesses()
{
    QVector<PluginProcessInfo> procs;

#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return procs;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snap, &entry)) {
        do {
            PluginProcessInfo info;
            info.pid  = entry.th32ProcessID;
            info.name = QString::fromWCharArray(entry.szExeFile);

            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, entry.th32ProcessID);
            if (hProc) {
                wchar_t path[MAX_PATH * 2];
                DWORD pathLen = sizeof(path) / sizeof(wchar_t);
                if (QueryFullProcessImageNameW(hProc, 0, path, &pathLen)) {
                    info.path = QString::fromWCharArray(path);
                    SHFILEINFOW sfi = {};
                    if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi),
                                       SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                        info.icon = QIcon(QPixmap::fromImage(QImage::fromHICON(sfi.hIcon)));
#else
                        info.icon = QIcon(QtWin::fromHICON(sfi.hIcon));
#endif
                        DestroyIcon(sfi.hIcon);
                    }
                }
                CloseHandle(hProc);
            }
            procs.append(info);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);

#else
    QDir procDir(QStringLiteral("/proc"));
    QIcon defIcon = qApp->style()->standardIcon(QStyle::SP_ComputerIcon);

    for (const QString& entry : procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok;
        uint32_t pid = entry.toUInt(&ok);
        if (!ok || pid == 0) continue;

        QFile commFile(QStringLiteral("/proc/%1/comm").arg(pid));
        if (!commFile.open(QIODevice::ReadOnly)) continue;
        QString procName = QString::fromUtf8(commFile.readAll()).trimmed();
        commFile.close();
        if (procName.isEmpty()) continue;

        QString memPath = QStringLiteral("/proc/%1/mem").arg(pid);
        if (::access(memPath.toUtf8().constData(), R_OK) != 0) continue;

        QFileInfo exeInfo(QStringLiteral("/proc/%1/exe").arg(pid));
        PluginProcessInfo info;
        info.pid  = pid;
        info.name = procName;
        info.path = exeInfo.exists() ? exeInfo.symLinkTarget() : QString();
        info.icon = defIcon;
        procs.append(info);
    }
#endif

    return procs;
}

std::shared_ptr<IpcClient>
RemoteProcessMemoryPlugin::getOrCreateConnection(
        uint32_t pid, QString* errorMsg)
{
    QMutexLocker lock(&m_connectionsMutex);

    auto it = m_connections.find(pid);
    if (it != m_connections.end() && (*it)->connected)
        return *it;

    auto ipc = std::make_shared<IpcClient>();
    if (!ipc->connect(pid)) {
        if (errorMsg)
            *errorMsg = QStringLiteral("Failed to connect IPC to PID %1.\n"
                                       "Is the payload running?").arg(pid);
        return nullptr;
    }

    m_connections[pid] = ipc;
    return ipc;
}

/* ── Plugin factory ───────────────────────────────────────────────── */

extern "C" RCX_PLUGIN_EXPORT IPlugin* CreatePlugin()
{
    return new RemoteProcessMemoryPlugin();
}
