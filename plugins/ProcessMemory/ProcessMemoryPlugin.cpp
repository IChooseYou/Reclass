#include "ProcessMemoryPlugin.h"

#include "../../src/processpicker.h"

#include <QStyle>
#include <QApplication>
#include <QRegularExpression>
#include <QMessageBox>
#include <QPixmap>
#include <QImage>
#include <QDir>
#include <QFileInfo>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && defined(_WIN32)
#include <QtWin>
#endif

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>

typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING;
typedef struct _CLIENT_ID { HANDLE UniqueProcess; HANDLE UniqueThread; } CLIENT_ID;
typedef struct _SYSTEM_THREAD_INFORMATION {
    LARGE_INTEGER KernelTime, UserTime, CreateTime;
    ULONG WaitTime; PVOID StartAddress; CLIENT_ID ClientId;
    LONG Priority, BasePriority; ULONG ContextSwitches, ThreadState, WaitReason;
} SYSTEM_THREAD_INFORMATION;
typedef struct _SYSTEM_PROCESS_INFORMATION {
    ULONG NextEntryOffset;                  // 0x000
    ULONG NumberOfThreads;                  // 0x004
    LARGE_INTEGER WorkingSetPrivateSize;    // 0x008
    ULONG HardFaultCount;                   // 0x010
    ULONG NumberOfThreadsHighWatermark;     // 0x014
    ULONGLONG CycleTime;                    // 0x018
    LARGE_INTEGER CreateTime;               // 0x020
    LARGE_INTEGER UserTime;                 // 0x028
    LARGE_INTEGER KernelTime;               // 0x030
    UNICODE_STRING ImageName;               // 0x038
    LONG BasePriority;                      // 0x048
    HANDLE UniqueProcessId;                 // 0x050
    PVOID InheritedFromUniqueProcessId;     // 0x058
    ULONG HandleCount;                      // 0x060
    ULONG SessionId;                        // 0x064
    ULONG_PTR UniqueProcessKey;             // 0x068
    SIZE_T PeakVirtualSize;                 // 0x070
    SIZE_T VirtualSize;                     // 0x078
    ULONG PageFaultCount;                   // 0x080
    ULONG _pad0;                            // 0x084
    SIZE_T PeakWorkingSetSize;              // 0x088
    SIZE_T WorkingSetSize;                  // 0x090
    SIZE_T QuotaPeakPagedPoolUsage;         // 0x098
    SIZE_T QuotaPagedPoolUsage;             // 0x0A0
    SIZE_T QuotaPeakNonPagedPoolUsage;      // 0x0A8
    SIZE_T QuotaNonPagedPoolUsage;          // 0x0B0
    SIZE_T PagefileUsage;                   // 0x0B8
    SIZE_T PeakPagefileUsage;               // 0x0C0
    SIZE_T PrivatePageCount;                // 0x0C8
    LARGE_INTEGER ReadOperationCount;       // 0x0D0
    LARGE_INTEGER WriteOperationCount;      // 0x0D8
    LARGE_INTEGER OtherOperationCount;      // 0x0E0
    LARGE_INTEGER ReadTransferCount;        // 0x0E8
    LARGE_INTEGER WriteTransferCount;       // 0x0F0
    LARGE_INTEGER OtherTransferCount;       // 0x0F8
} SYSTEM_PROCESS_INFORMATION; // sizeof = 0x100
typedef struct alignas(8) _THREAD_BASIC_INFORMATION {
    NTSTATUS  ExitStatus;       // 0x00
    ULONG     _pad;             // 0x04
    PVOID     TebBaseAddress;   // 0x08
    CLIENT_ID ClientId;         // 0x10
    ULONG_PTR AffinityMask;     // 0x20
    LONG      Priority;         // 0x28
    LONG      BasePriority;     // 0x2C
} THREAD_BASIC_INFORMATION;
#elif defined(__linux__)
#include <climits>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <fstream>
#include <sstream>
#include <cstring>
#endif

// ──────────────────────────────────────────────────────────────────────────
// ProcessMemoryProvider implementation
// ──────────────────────────────────────────────────────────────────────────

#ifdef _WIN32

ProcessMemoryProvider::ProcessMemoryProvider(uint32_t pid, const QString& processName)
    : m_handle(nullptr)
    , m_pid(pid)
    , m_processName(processName)
    , m_writable(false)
    , m_base(0)
{
    // Try to open with write access first
    m_handle = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
                          FALSE, pid);
    if (m_handle)
        m_writable = true;
    else
    {
        // Fall back to read-only
        m_handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        m_writable = false;
    }

    if (m_handle) {
        // Detect 32-bit (WoW64) process
        BOOL isWow64 = FALSE;
        if (IsWow64Process(m_handle, &isWow64) && isWow64)
            m_pointerSize = 4;
        // Query PEB address via NtQueryInformationProcess
        {
            typedef NTSTATUS(NTAPI* NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
            static NtQIP_t pNtQIP = (NtQIP_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
            if (pNtQIP) {
                struct { PVOID r1; PVOID PebBaseAddress; PVOID r2[2]; ULONG_PTR pid; PVOID r3; } pbi = {};
                ULONG retLen = 0;
                if (pNtQIP(m_handle, /*ProcessBasicInformation*/0, &pbi, sizeof(pbi), &retLen) >= 0 && pbi.PebBaseAddress)
                    m_peb = (uint64_t)(uintptr_t)pbi.PebBaseAddress;
            }
        }
        cacheModules();
    }
}

bool ProcessMemoryProvider::read(uint64_t addr, void* buf, int len) const
{
    if (!m_handle || len <= 0) return false;

    SIZE_T bytesRead = 0;
    ReadProcessMemory(m_handle, (LPCVOID)(addr), buf, (SIZE_T)len, &bytesRead);
    if ((int)bytesRead < len)
        memset((char*)buf + bytesRead, 0, len - bytesRead);
    return bytesRead > 0;
}

bool ProcessMemoryProvider::write(uint64_t addr, const void* buf, int len)
{
    if (!m_handle || !m_writable || len <= 0) return false;

    SIZE_T bytesWritten = 0;
    if (WriteProcessMemory(m_handle, (LPVOID)(addr), buf, (SIZE_T)len, &bytesWritten))
        return bytesWritten == (SIZE_T)len;
    return false;
}

QString ProcessMemoryProvider::getSymbol(uint64_t addr) const
{
    for (const auto& mod : m_modules)
    {
        if (addr >= mod.base && addr < mod.base + mod.size)
        {
            uint64_t offset = addr - mod.base;
            return QStringLiteral("%1+0x%2")
                .arg(mod.name)
                .arg(offset, 0, 16, QChar('0'));
        }
    }
    return {};
}

void ProcessMemoryProvider::cacheModules()
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(m_handle, mods, sizeof(mods),
                              &needed, LIST_MODULES_ALL))
        return;
    int count = qMin((int)(needed / sizeof(HMODULE)), 1024);
    m_modules.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        MODULEINFO mi{};
        WCHAR modName[MAX_PATH];
        if (GetModuleInformation(m_handle, mods[i], &mi, sizeof(mi))
            && GetModuleBaseNameW(m_handle, mods[i], modName, MAX_PATH))
        {
            if ( i == 0 )
                m_base = (uint64_t)mi.lpBaseOfDll;

            m_modules.append({
                QString::fromWCharArray(modName),
                (uint64_t)mi.lpBaseOfDll,
                (uint64_t)mi.SizeOfImage
            });
        }
    }
}

QVector<rcx::MemoryRegion> ProcessMemoryProvider::enumerateRegions() const
{
    QVector<rcx::MemoryRegion> regions;
    if (!m_handle) return regions;

    MEMORY_BASIC_INFORMATION mbi;
    uint64_t addr = 0;

    while (VirtualQueryEx(m_handle, (LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & PAGE_NOACCESS) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            rcx::MemoryRegion region;
            region.base = (uint64_t)mbi.BaseAddress;
            region.size = mbi.RegionSize;
            region.readable = true;
            region.writable = (mbi.Protect & PAGE_READWRITE) ||
                              (mbi.Protect & PAGE_WRITECOPY) ||
                              (mbi.Protect & PAGE_EXECUTE_READWRITE) ||
                              (mbi.Protect & PAGE_EXECUTE_WRITECOPY);
            region.executable = (mbi.Protect & PAGE_EXECUTE) ||
                                (mbi.Protect & PAGE_EXECUTE_READ) ||
                                (mbi.Protect & PAGE_EXECUTE_READWRITE) ||
                                (mbi.Protect & PAGE_EXECUTE_WRITECOPY);

            // Match module name from cached module list
            for (const auto& mod : m_modules) {
                if (region.base >= mod.base && region.base < mod.base + mod.size) {
                    region.moduleName = mod.name;
                    break;
                }
            }

            regions.append(region);
        }

        uint64_t next = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break; // overflow protection
        addr = next;
    }

    return regions;
}

#elif defined(__linux__)

ProcessMemoryProvider::ProcessMemoryProvider(uint32_t pid, const QString& processName)
    : m_fd(-1)
    , m_pid(pid)
    , m_processName(processName)
    , m_writable(false)
    , m_base(0)
{
    QString memPath = QStringLiteral("/proc/%1/mem").arg(pid);
    QByteArray pathUtf8 = memPath.toUtf8();

    // Try read-write first
    m_fd = ::open(pathUtf8.constData(), O_RDWR);
    if (m_fd >= 0)
        m_writable = true;
    else
    {
        // Fall back to read-only
        m_fd = ::open(pathUtf8.constData(), O_RDONLY);
        m_writable = false;
    }

    if (m_fd >= 0) {
        // Detect 32-bit ELF process
        QString exePath = QStringLiteral("/proc/%1/exe").arg(pid);
        QByteArray exePathUtf8 = exePath.toUtf8();
        int exeFd = ::open(exePathUtf8.constData(), O_RDONLY);
        if (exeFd >= 0) {
            unsigned char elfClass = 0;
            // ELF e_ident[EI_CLASS] is at offset 4
            if (::pread(exeFd, &elfClass, 1, 4) == 1 && elfClass == 1) // ELFCLASS32
                m_pointerSize = 4;
            ::close(exeFd);
        }
        cacheModules();
    }
}

bool ProcessMemoryProvider::read(uint64_t addr, void* buf, int len) const
{
    if (m_fd < 0 || len <= 0) return false;

    // Try process_vm_readv first (faster, no fd seek contention)
    struct iovec local;
    local.iov_base = buf;
    local.iov_len = static_cast<size_t>(len);

    struct iovec remote;
    remote.iov_base = reinterpret_cast<void*>(addr);
    remote.iov_len = static_cast<size_t>(len);

    ssize_t nread = process_vm_readv(m_pid, &local, 1, &remote, 1, 0);
    if (nread == static_cast<ssize_t>(len))
        return true;

    // Fallback: pread on /proc/<pid>/mem
    nread = ::pread(m_fd, buf, static_cast<size_t>(len), static_cast<off_t>(addr));
    return nread == static_cast<ssize_t>(len);
}

bool ProcessMemoryProvider::write(uint64_t addr, const void* buf, int len)
{
    if (m_fd < 0 || !m_writable || len <= 0) return false;

    // Try process_vm_writev first
    struct iovec local;
    local.iov_base = const_cast<void*>(buf);
    local.iov_len = static_cast<size_t>(len);

    struct iovec remote;
    remote.iov_base = reinterpret_cast<void*>(addr);
    remote.iov_len = static_cast<size_t>(len);

    ssize_t nwritten = process_vm_writev(m_pid, &local, 1, &remote, 1, 0);
    if (nwritten == static_cast<ssize_t>(len))
        return true;

    // Fallback: pwrite on /proc/<pid>/mem
    nwritten = ::pwrite(m_fd, buf, static_cast<size_t>(len), static_cast<off_t>(addr));
    return nwritten == static_cast<ssize_t>(len);
}

QString ProcessMemoryProvider::getSymbol(uint64_t addr) const
{
    for (const auto& mod : m_modules)
    {
        if (addr >= mod.base && addr < mod.base + mod.size)
        {
            uint64_t offset = addr - mod.base;
            return QStringLiteral("%1+0x%2")
                .arg(mod.name)
                .arg(offset, 0, 16, QChar('0'));
        }
    }
    return {};
}

void ProcessMemoryProvider::cacheModules()
{
    // Parse /proc/<pid>/maps to discover loaded modules
    QString mapsPath = QStringLiteral("/proc/%1/maps").arg(m_pid);
    std::ifstream mapsFile(mapsPath.toStdString());
    if (!mapsFile.is_open()) return;

    // Accumulate base/end per path, then convert to ModuleInfo
    struct Range { uint64_t base; uint64_t end; };
    QMap<QString, Range> moduleRanges;

    std::string line;
    bool firstExec = true;
    while (std::getline(mapsFile, line))
    {
        // Format: addr_start-addr_end perms offset dev inode pathname
        // Example: 00400000-00452000 r-xp 00000000 08:02 173521 /usr/bin/foo
        std::istringstream iss(line);
        std::string addrRange, perms, offset, dev, inode, pathname;
        iss >> addrRange >> perms >> offset >> dev >> inode;
        std::getline(iss, pathname);

        // Trim leading whitespace from pathname
        size_t start = pathname.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        pathname = pathname.substr(start);

        // Skip non-file mappings
        if (pathname.empty() || pathname[0] != '/') continue;
        // Skip special mappings
        if (pathname.find("/dev/") == 0 || pathname.find("/memfd:") == 0) continue;

        // Parse address range
        auto dash = addrRange.find('-');
        if (dash == std::string::npos) continue;
        uint64_t addrStart = std::stoull(addrRange.substr(0, dash), nullptr, 16);
        uint64_t addrEnd = std::stoull(addrRange.substr(dash + 1), nullptr, 16);

        QString qpath = QString::fromStdString(pathname);

        // Track first executable mapping as the base address
        if (firstExec && perms.size() >= 3 && perms[2] == 'x')
        {
            m_base = addrStart;
            firstExec = false;
        }

        auto it = moduleRanges.find(qpath);
        if (it != moduleRanges.end())
        {
            if (addrStart < it->base) it->base = addrStart;
            if (addrEnd > it->end) it->end = addrEnd;
        }
        else
        {
            moduleRanges.insert(qpath, {addrStart, addrEnd});
        }
    }

    m_modules.reserve(moduleRanges.size());
    for (auto it = moduleRanges.begin(); it != moduleRanges.end(); ++it)
    {
        QFileInfo fi(it.key());
        m_modules.append({
            fi.fileName(),
            it->base,
            it->end - it->base
        });
    }
}

QVector<rcx::MemoryRegion> ProcessMemoryProvider::enumerateRegions() const
{
    QVector<rcx::MemoryRegion> regions;
    if (m_fd < 0) return regions;

    QString mapsPath = QStringLiteral("/proc/%1/maps").arg(m_pid);
    std::ifstream mapsFile(mapsPath.toStdString());
    if (!mapsFile.is_open()) return regions;

    std::string line;
    while (std::getline(mapsFile, line)) {
        std::istringstream iss(line);
        std::string addrRange, perms, offset, dev, inode, pathname;
        iss >> addrRange >> perms >> offset >> dev >> inode;
        std::getline(iss, pathname);

        auto dash = addrRange.find('-');
        if (dash == std::string::npos) continue;
        uint64_t addrStart = std::stoull(addrRange.substr(0, dash), nullptr, 16);
        uint64_t addrEnd = std::stoull(addrRange.substr(dash + 1), nullptr, 16);

        if (perms.size() < 4) continue;
        bool readable   = (perms[0] == 'r');
        bool writable   = (perms[1] == 'w');
        bool executable = (perms[2] == 'x');

        if (!readable) continue;

        rcx::MemoryRegion region;
        region.base       = addrStart;
        region.size       = addrEnd - addrStart;
        region.readable   = readable;
        region.writable   = writable;
        region.executable = executable;

        // Extract module name from pathname
        size_t start = pathname.find_first_not_of(" \t");
        if (start != std::string::npos) {
            QString qpath = QString::fromStdString(pathname.substr(start));
            if (qpath.startsWith('/') && !qpath.startsWith("/dev/") &&
                !qpath.startsWith("/memfd:")) {
                QFileInfo fi(qpath);
                region.moduleName = fi.fileName();
            }
        }

        regions.append(region);
    }

    return regions;
}

#endif // platform

uint64_t ProcessMemoryProvider::symbolToAddress(const QString& name) const
{
    for (const auto& mod : m_modules) {
        if (mod.name.compare(name, Qt::CaseInsensitive) == 0)
            return mod.base;
    }
    return 0;
}

ProcessMemoryProvider::~ProcessMemoryProvider()
{
#ifdef _WIN32
    if (m_handle)
        CloseHandle(m_handle);
#elif defined(__linux__)
    if (m_fd >= 0)
        ::close(m_fd);
#endif
}

int ProcessMemoryProvider::size() const
{
#ifdef _WIN32
    return m_handle ? 0x10000 : 0;
#elif defined(__linux__)
    return (m_fd >= 0) ? 0x10000 : 0;
#endif
}


QVector<rcx::Provider::ThreadInfo> ProcessMemoryProvider::tebs() const
{
#ifdef _WIN32
    QVector<ThreadInfo> result;
    if (!m_handle || !m_peb) return result;

    typedef NTSTATUS(NTAPI* NtQSI_t)(ULONG, PVOID, ULONG, PULONG);
    typedef NTSTATUS(NTAPI* NtQIT_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static auto pNtQSI = (NtQSI_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    static auto pNtQIT = (NtQIT_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    if (!pNtQSI || !pNtQIT) return result;

    // Enumerate threads via SystemProcessInformation (class 5)
    ULONG retLen = 0;
    ULONG bufSize = 1 << 20;
    QByteArray buf(bufSize, 0);
    NTSTATUS qsiSt;
    for (int attempt = 0; attempt < 8; ++attempt) {
        qsiSt = pNtQSI(5, buf.data(), bufSize, &retLen);
        if ((uint32_t)qsiSt != 0xC0000004u) break;
        bufSize *= 2;
        buf.resize(bufSize);
    }
    if (qsiSt < 0) return result;

    // Walk process entries to find ours
    auto* proc = (SYSTEM_PROCESS_INFORMATION*)buf.data();
    for (;;) {
        if ((uintptr_t)proc->UniqueProcessId == m_pid) {
            auto* threads = (SYSTEM_THREAD_INFORMATION*)((char*)proc + sizeof(*proc));
            for (ULONG i = 0; i < proc->NumberOfThreads; ++i) {
                DWORD tid = (DWORD)(uintptr_t)threads[i].ClientId.UniqueThread;
                HANDLE hThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
                if (!hThread) continue;
                THREAD_BASIC_INFORMATION tbi = {};
                ULONG tbiLen = 0;
                NTSTATUS qitSt = pNtQIT(hThread, 0, &tbi, sizeof(tbi), &tbiLen);
                if (qitSt >= 0 && tbi.TebBaseAddress)
                    result.append({(uint64_t)(uintptr_t)tbi.TebBaseAddress, tid});
                CloseHandle(hThread);
            }
            break;
        }
        if (!proc->NextEntryOffset) break;
        proc = (SYSTEM_PROCESS_INFORMATION*)((char*)proc + proc->NextEntryOffset);
    }
    return result;
#else
    return {};
#endif
}
// ──────────────────────────────────────────────────────────────────────────
// ProcessMemoryPlugin implementation
// ──────────────────────────────────────────────────────────────────────────

QIcon ProcessMemoryPlugin::Icon() const
{
    return qApp->style()->standardIcon(QStyle::SP_ComputerIcon);
}

bool ProcessMemoryPlugin::canHandle(const QString& target) const
{
    // Target format: "pid:name" or just "pid"
    QRegularExpression re("^\\d+");
    return re.match(target).hasMatch();
}

std::unique_ptr<rcx::Provider> ProcessMemoryPlugin::createProvider(const QString& target, QString* errorMsg)
{
    // Parse target: "pid:name" or just "pid"
    QStringList parts = target.split(':');
    bool ok = false;
    uint32_t pid = parts[0].toUInt(&ok);

    if (!ok || pid == 0)
    {
        if (errorMsg) *errorMsg = "Invalid PID: " + target;
        return nullptr;
    }

    QString name = parts.size() > 1 ? parts[1] : QString("PID %1").arg(pid);

    auto provider = std::make_unique<ProcessMemoryProvider>(pid, name);
    if (!provider->isValid())
    {
        if (errorMsg)
            *errorMsg = QString("Failed to open process %1 (PID: %2)\n"
                               "Ensure the process is running and you have sufficient permissions.")
                        .arg(name).arg(pid);
        return nullptr;
    }

    return provider;
}

uint64_t ProcessMemoryPlugin::getInitialBaseAddress(const QString& target) const
{
#ifdef _WIN32
    // Parse PID from target
    QStringList parts = target.split(':');
    bool ok = false;
    DWORD pid = parts[0].toUInt(&ok);
    if (!ok || pid == 0) return 0;

    // Open process to get main module base
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return 0;

    uint64_t base = 0;
    HMODULE hMod = nullptr;
    DWORD needed = 0;

    if (EnumProcessModulesEx(hProc, &hMod, sizeof(hMod), &needed, LIST_MODULES_ALL) && hMod)
    {
        MODULEINFO mi{};
        if (GetModuleInformation(hProc, hMod, &mi, sizeof(mi)))
            base = (uint64_t)mi.lpBaseOfDll;
    }

    CloseHandle(hProc);
    return base;
#elif defined(__linux__)
    // Parse PID from target
    QStringList parts = target.split(':');
    bool ok = false;
    uint32_t pid = parts[0].toUInt(&ok);
    if (!ok || pid == 0) return 0;

    // Find first executable mapping from /proc/<pid>/maps
    QString mapsPath = QStringLiteral("/proc/%1/maps").arg(pid);
    std::ifstream mapsFile(mapsPath.toStdString());
    if (!mapsFile.is_open()) return 0;

    std::string line;
    while (std::getline(mapsFile, line)) {
        std::istringstream iss(line);
        std::string addrRange, perms;
        iss >> addrRange >> perms;
        if (perms.size() >= 3 && perms[2] == 'x') {
            auto dash = addrRange.find('-');
            if (dash != std::string::npos) {
                return std::stoull(addrRange.substr(0, dash), nullptr, 16);
            }
        }
    }
    return 0;
#else
    Q_UNUSED(target);
    return 0;
#endif
}

bool ProcessMemoryPlugin::selectTarget(QWidget* parent, QString* target)
{
    // Use custom process enumeration from plugin
    QVector<PluginProcessInfo> pluginProcesses = enumerateProcesses();

    // Convert to ProcessInfo for ProcessPicker
    QList<ProcessInfo> processes;
    for (const auto& pinfo : pluginProcesses)
    {
        ProcessInfo info;
        info.pid = pinfo.pid;
        info.name = pinfo.name;
        info.path = pinfo.path;
        info.icon = pinfo.icon;
        info.is32Bit = pinfo.is32Bit;
        processes.append(info);
    }

    // Show ProcessPicker with custom process list
    ProcessPicker picker(processes, parent);
    if (picker.exec() == QDialog::Accepted) {
        uint32_t pid = picker.selectedProcessId();
        QString name = picker.selectedProcessName();

        // Format target as "pid:name"
        *target = QString("%1:%2").arg(pid).arg(name);
        return true;
    }

    return false;
}

QVector<PluginProcessInfo> ProcessMemoryPlugin::enumerateProcesses()
{
    QVector<PluginProcessInfo> processes;

#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            PluginProcessInfo info;
            info.pid = entry.th32ProcessID;
            info.name = QString::fromWCharArray(entry.szExeFile);

            // Try to get full path and icon
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (hProcess) {
                wchar_t path[MAX_PATH * 2];
                DWORD pathLen = sizeof(path) / sizeof(wchar_t);

                // Try QueryFullProcessImageNameW first
                if (QueryFullProcessImageNameW(hProcess, 0, path, &pathLen)) {
                    info.path = QString::fromWCharArray(path);

                    // Extract icon
                    SHFILEINFOW sfi = {};
                    if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON)) {
                        if (sfi.hIcon) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                            QPixmap pixmap = QPixmap::fromImage(QImage::fromHICON(sfi.hIcon));
#else
                            QPixmap pixmap = QtWin::fromHICON(sfi.hIcon);
#endif
                            info.icon = QIcon(pixmap);
                            DestroyIcon(sfi.hIcon);
                        }
                    }
                }

                // Detect 32-bit (WoW64) process
                BOOL isWow64 = FALSE;
                if (IsWow64Process(hProcess, &isWow64) && isWow64)
                    info.is32Bit = true;

                CloseHandle(hProcess);
            }

            processes.append(info);

        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
#elif defined(__linux__)
    QDir procDir("/proc");
    QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QIcon defaultIcon = qApp->style()->standardIcon(QStyle::SP_ComputerIcon);

    for (const QString& entry : entries) {
        bool ok = false;
        uint32_t pid = entry.toUInt(&ok);
        if (!ok || pid == 0) continue;

        // Read process name from /proc/<pid>/comm
        QString commPath = QStringLiteral("/proc/%1/comm").arg(pid);
        QFile commFile(commPath);
        QString procName;
        if (commFile.open(QIODevice::ReadOnly)) {
            procName = QString::fromUtf8(commFile.readAll()).trimmed();
            commFile.close();
        }
        if (procName.isEmpty()) continue;  // Skip kernel threads with no name

        // Read exe path from /proc/<pid>/exe symlink
        QString exePath = QStringLiteral("/proc/%1/exe").arg(pid);
        QFileInfo exeInfo(exePath);
        QString resolvedPath;
        if (exeInfo.exists())
            resolvedPath = exeInfo.symLinkTarget();

        // Skip if we can't read the process memory (no access)
        QString memPath = QStringLiteral("/proc/%1/mem").arg(pid);
        if (::access(memPath.toUtf8().constData(), R_OK) != 0)
            continue;

        PluginProcessInfo info;
        info.pid = pid;
        info.name = procName;
        info.path = resolvedPath;
        info.icon = defaultIcon;

        // Detect 32-bit ELF process
        int exeFd = ::open(exePath.toUtf8().constData(), O_RDONLY);
        if (exeFd >= 0) {
            unsigned char elfClass = 0;
            if (::pread(exeFd, &elfClass, 1, 4) == 1 && elfClass == 1) // ELFCLASS32
                info.is32Bit = true;
            ::close(exeFd);
        }

        processes.append(info);
    }
#endif

    return processes;
}

// ──────────────────────────────────────────────────────────────────────────
// Plugin factory
// ──────────────────────────────────────────────────────────────────────────

extern "C" RCX_PLUGIN_EXPORT IPlugin* CreatePlugin()
{
    return new ProcessMemoryPlugin();
}
