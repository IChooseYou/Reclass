#include "KernelMemoryPlugin.h"
#include "../../src/processpicker.h"

#include <QStyle>
#include <QApplication>
#include <QMenu>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLibrary>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtWin>
#endif
#endif

// ─────────────────────────────────────────────────────────────────────────
// Helper: DeviceIoControl wrapper
// ─────────────────────────────────────────────────────────────────────────

#ifdef _WIN32

static bool ioctlCall(HANDLE h, DWORD code,
                      const void* in, DWORD inLen,
                      void* out, DWORD outLen,
                      DWORD* bytesReturned = nullptr)
{
    DWORD br = 0;
    BOOL ok = DeviceIoControl(h, code, const_cast<LPVOID>(in), inLen,
                              out, outLen, &br, nullptr);
    if (bytesReturned) *bytesReturned = br;
    return ok != FALSE;
}

#endif // _WIN32

// ─────────────────────────────────────────────────────────────────────────
// KernelProcessProvider
// ─────────────────────────────────────────────────────────────────────────

KernelProcessProvider::KernelProcessProvider(void* driverHandle, uint32_t pid, const QString& processName)
    : m_driverHandle(driverHandle)
    , m_pid(pid)
    , m_processName(processName)
{
    if (m_driverHandle) {
        queryPeb();
        cacheModules();
    }
}

bool KernelProcessProvider::read(uint64_t addr, void* buf, int len) const
{
#ifdef _WIN32
    if (!m_driverHandle || len <= 0) return false;
    if (len > RCX_DRV_MAX_VIRTUAL) len = RCX_DRV_MAX_VIRTUAL;

    RcxDrvReadRequest req{};
    req.pid     = m_pid;
    req.address = addr;
    req.length  = (uint32_t)len;

    DWORD br = 0;
    BOOL ok = DeviceIoControl((HANDLE)m_driverHandle,
                              IOCTL_RCX_READ_MEMORY,
                              &req, sizeof(req),
                              buf, (DWORD)len, &br, nullptr);
    // Zero unread portion (partial copy)
    if ((int)br < len)
        memset((char*)buf + br, 0, len - br);
    return ok || br > 0;
#else
    Q_UNUSED(addr); Q_UNUSED(buf); Q_UNUSED(len);
    return false;
#endif
}

int KernelProcessProvider::size() const
{
    return m_driverHandle ? 0x10000 : 0;
}

bool KernelProcessProvider::write(uint64_t addr, const void* buf, int len)
{
#ifdef _WIN32
    if (!m_driverHandle || len <= 0) return false;
    if (len > RCX_DRV_MAX_VIRTUAL) return false;

    // Build request: header + inline data
    QByteArray packet(sizeof(RcxDrvWriteRequest) + len, Qt::Uninitialized);
    auto* req = reinterpret_cast<RcxDrvWriteRequest*>(packet.data());
    req->pid     = m_pid;
    req->_pad0   = 0;
    req->address = addr;
    req->length  = (uint32_t)len;
    req->_pad1   = 0;
    memcpy(packet.data() + sizeof(RcxDrvWriteRequest), buf, len);

    return ioctlCall((HANDLE)m_driverHandle, IOCTL_RCX_WRITE_MEMORY,
                     packet.constData(), (DWORD)packet.size(),
                     nullptr, 0);
#else
    Q_UNUSED(addr); Q_UNUSED(buf); Q_UNUSED(len);
    return false;
#endif
}

QString KernelProcessProvider::getSymbol(uint64_t addr) const
{
    for (const auto& mod : m_modules) {
        if (addr >= mod.base && addr < mod.base + mod.size) {
            uint64_t offset = addr - mod.base;
            return QStringLiteral("%1+0x%2")
                .arg(mod.name)
                .arg(offset, 0, 16, QChar('0'));
        }
    }
    return {};
}

uint64_t KernelProcessProvider::symbolToAddress(const QString& name) const
{
    for (const auto& mod : m_modules) {
        if (mod.name.compare(name, Qt::CaseInsensitive) == 0)
            return mod.base;
    }
    return 0;
}

QVector<rcx::MemoryRegion> KernelProcessProvider::enumerateRegions() const
{
    QVector<rcx::MemoryRegion> regions;
#ifdef _WIN32
    if (!m_driverHandle) return regions;

    RcxDrvQueryRegionsRequest req{};
    req.pid = m_pid;

    // Allocate generous output buffer for region entries
    constexpr int kMaxEntries = 8192;
    QByteArray outBuf(kMaxEntries * sizeof(RcxDrvRegionEntry), Qt::Uninitialized);

    DWORD br = 0;
    if (!ioctlCall((HANDLE)m_driverHandle, IOCTL_RCX_QUERY_REGIONS,
                   &req, sizeof(req),
                   outBuf.data(), (DWORD)outBuf.size(), &br))
        return regions;

    int count = (int)(br / sizeof(RcxDrvRegionEntry));
    auto* entries = reinterpret_cast<const RcxDrvRegionEntry*>(outBuf.constData());

    for (int i = 0; i < count; ++i) {
        const auto& e = entries[i];
        // Only include committed, accessible regions
        if (!(e.state & 0x1000)) continue;  // MEM_COMMIT = 0x1000
        uint32_t p = e.protect;
        if (p & 0x01) continue;             // PAGE_NOACCESS
        if (p & 0x100) continue;            // PAGE_GUARD

        rcx::MemoryRegion region;
        region.base = e.base;
        region.size = e.size;
        region.readable = true;
        region.writable = (p & 0x04) || (p & 0x08) || (p & 0x40) || (p & 0x80);
        region.executable = (p & 0x10) || (p & 0x20) || (p & 0x40) || (p & 0x80);

        // Match module name
        for (const auto& mod : m_modules) {
            if (region.base >= mod.base && region.base < mod.base + mod.size) {
                region.moduleName = mod.name;
                break;
            }
        }

        regions.append(region);
    }
#endif
    return regions;
}

void KernelProcessProvider::queryPeb()
{
#ifdef _WIN32
    RcxDrvQueryPebRequest req{};
    req.pid = m_pid;

    RcxDrvQueryPebResponse resp{};
    if (ioctlCall((HANDLE)m_driverHandle, IOCTL_RCX_QUERY_PEB,
                  &req, sizeof(req), &resp, sizeof(resp))) {
        m_peb = resp.pebAddress;
        if (resp.pointerSize == 4)
            m_pointerSize = 4;
    }
#endif
}

QVector<rcx::Provider::ThreadInfo> KernelProcessProvider::tebs() const
{
    QVector<ThreadInfo> result;
#ifdef _WIN32
    if (!m_driverHandle) return result;

    RcxDrvQueryTebsRequest req{};
    req.pid = m_pid;

    constexpr int kMaxThreads = 4096;
    QByteArray outBuf(kMaxThreads * sizeof(RcxDrvTebEntry), Qt::Uninitialized);

    DWORD br = 0;
    if (!ioctlCall((HANDLE)m_driverHandle, IOCTL_RCX_QUERY_TEBS,
                   &req, sizeof(req),
                   outBuf.data(), (DWORD)outBuf.size(), &br))
        return result;

    int count = (int)(br / sizeof(RcxDrvTebEntry));
    auto* entries = reinterpret_cast<const RcxDrvTebEntry*>(outBuf.constData());

    for (int i = 0; i < count; ++i)
        result.push_back(ThreadInfo{entries[i].tebAddress, entries[i].threadId});
#endif
    return result;
}

void KernelProcessProvider::cacheModules()
{
#ifdef _WIN32
    if (!m_driverHandle) return;

    RcxDrvQueryModulesRequest req{};
    req.pid = m_pid;

    constexpr int kMaxModules = 1024;
    QByteArray outBuf(kMaxModules * sizeof(RcxDrvModuleEntry), Qt::Uninitialized);

    DWORD br = 0;
    if (!ioctlCall((HANDLE)m_driverHandle, IOCTL_RCX_QUERY_MODULES,
                   &req, sizeof(req),
                   outBuf.data(), (DWORD)outBuf.size(), &br))
        return;

    int count = (int)(br / sizeof(RcxDrvModuleEntry));
    auto* entries = reinterpret_cast<const RcxDrvModuleEntry*>(outBuf.constData());

    m_modules.reserve(count);
    for (int i = 0; i < count; ++i) {
        QString modName = QString::fromUtf16(reinterpret_cast<const char16_t*>(entries[i].name));
        if (i == 0)
            m_base = entries[i].base;

        m_modules.push_back(ModuleInfo{modName, entries[i].base, entries[i].size});
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────
// KernelProcessProvider — paging / address translation
// ─────────────────────────────────────────────────────────────────────────

uint64_t KernelProcessProvider::getCr3() const
{
#ifdef _WIN32
    if (m_cr3Cache) return m_cr3Cache;
    if (!m_driverHandle) return 0;

    RcxDrvReadCr3Request req{};
    req.pid = m_pid;

    RcxDrvReadCr3Response resp{};
    if (ioctlCall((HANDLE)m_driverHandle, IOCTL_RCX_READ_CR3,
                  &req, sizeof(req), &resp, sizeof(resp))) {
        m_cr3Cache = resp.cr3;
        return m_cr3Cache;
    }
#endif
    return 0;
}

rcx::VtopResult KernelProcessProvider::translateAddress(uint64_t va) const
{
    rcx::VtopResult result{};
#ifdef _WIN32
    if (!m_driverHandle) return result;

    RcxDrvVtopRequest req{};
    req.pid = m_pid;
    req.virtualAddress = va;

    RcxDrvVtopResponse resp{};
    if (ioctlCall((HANDLE)m_driverHandle, IOCTL_RCX_VTOP,
                  &req, sizeof(req), &resp, sizeof(resp))) {
        result.physical = resp.physicalAddress;
        result.pml4e    = resp.pml4e;
        result.pdpte    = resp.pdpte;
        result.pde      = resp.pde;
        result.pte      = resp.pte;
        result.pageSize = resp.pageSize;
        result.valid    = resp.valid != 0;
    }
#else
    Q_UNUSED(va);
#endif
    return result;
}

QVector<uint64_t> KernelProcessProvider::readPageTable(uint64_t physAddr, int startIdx, int count) const
{
    QVector<uint64_t> entries;
#ifdef _WIN32
    if (!m_driverHandle) return entries;
    if (startIdx < 0 || startIdx >= 512) return entries;
    if (count <= 0) return entries;
    if (startIdx + count > 512) count = 512 - startIdx;

    // Read the full 4KB page table via physical read
    int byteOffset = startIdx * 8;
    int byteLen = count * 8;
    QByteArray buf(byteLen, 0);

    RcxDrvPhysReadRequest req{};
    req.physAddress = physAddr + byteOffset;
    req.length = (uint32_t)byteLen;
    req.width = 0;  // memcpy mode

    DWORD br = 0;
    if (ioctlCall((HANDLE)m_driverHandle, IOCTL_RCX_READ_PHYS,
                  &req, sizeof(req), buf.data(), (DWORD)byteLen, &br)) {
        entries.resize(count);
        memcpy(entries.data(), buf.constData(), byteLen);
    }
#else
    Q_UNUSED(physAddr); Q_UNUSED(startIdx); Q_UNUSED(count);
#endif
    return entries;
}

// ─────────────────────────────────────────────────────────────────────────
// KernelPhysProvider
// ─────────────────────────────────────────────────────────────────────────

KernelPhysProvider::KernelPhysProvider(void* driverHandle, uint64_t baseAddr)
    : m_driverHandle(driverHandle)
    , m_baseAddr(baseAddr)
{
}

bool KernelPhysProvider::read(uint64_t addr, void* buf, int len) const
{
#ifdef _WIN32
    if (!m_driverHandle || len <= 0) return false;

    // Read in 4KB chunks (driver cap)
    int offset = 0;
    while (offset < len) {
        int chunk = qMin(len - offset, (int)RCX_DRV_MAX_PHYSICAL);

        RcxDrvPhysReadRequest req{};
        req.physAddress = addr + offset;
        req.length = (uint32_t)chunk;
        req.width  = 0;  // memcpy mode

        DWORD br = 0;
        BOOL ok = DeviceIoControl((HANDLE)m_driverHandle,
                                  IOCTL_RCX_READ_PHYS,
                                  &req, sizeof(req),
                                  (char*)buf + offset, (DWORD)chunk, &br, nullptr);
        if (!ok && br == 0) {
            memset((char*)buf + offset, 0, len - offset);
            return offset > 0;
        }
        if ((int)br < chunk)
            memset((char*)buf + offset + br, 0, chunk - br);
        offset += chunk;
    }
    return true;
#else
    Q_UNUSED(addr); Q_UNUSED(buf); Q_UNUSED(len);
    return false;
#endif
}

bool KernelPhysProvider::write(uint64_t addr, const void* buf, int len)
{
#ifdef _WIN32
    if (!m_driverHandle || len <= 0) return false;

    int offset = 0;
    while (offset < len) {
        int chunk = qMin(len - offset, (int)RCX_DRV_MAX_PHYSICAL);

        QByteArray packet(sizeof(RcxDrvPhysWriteRequest) + chunk, Qt::Uninitialized);
        auto* req = reinterpret_cast<RcxDrvPhysWriteRequest*>(packet.data());
        req->physAddress = addr + offset;
        req->length = (uint32_t)chunk;
        req->width  = 0;
        memcpy(packet.data() + sizeof(RcxDrvPhysWriteRequest), (const char*)buf + offset, chunk);

        if (!ioctlCall((HANDLE)m_driverHandle, IOCTL_RCX_WRITE_PHYS,
                       packet.constData(), (DWORD)packet.size(),
                       nullptr, 0))
            return false;
        offset += chunk;
    }
    return true;
#else
    Q_UNUSED(addr); Q_UNUSED(buf); Q_UNUSED(len);
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────
// KernelMemoryPlugin
// ─────────────────────────────────────────────────────────────────────────

KernelMemoryPlugin::KernelMemoryPlugin()
{
}

KernelMemoryPlugin::~KernelMemoryPlugin()
{
    stopDriver();
}

QIcon KernelMemoryPlugin::Icon() const
{
    return qApp->style()->standardIcon(QStyle::SP_DriveHDIcon);
}

bool KernelMemoryPlugin::canHandle(const QString& target) const
{
    return target.startsWith(QStringLiteral("km:"))
        || target.startsWith(QStringLiteral("phys:"));
}

std::unique_ptr<rcx::Provider> KernelMemoryPlugin::createProvider(const QString& target, QString* errorMsg)
{
    if (!ensureDriverLoaded(errorMsg))
        return nullptr;

#ifdef _WIN32
    if (target.startsWith(QStringLiteral("km:"))) {
        // km:{pid}:{name}
        QStringList parts = target.mid(3).split(':');
        bool ok = false;
        uint32_t pid = parts[0].toUInt(&ok);
        if (!ok || pid == 0) {
            if (errorMsg) *errorMsg = QStringLiteral("Invalid PID in target: ") + target;
            return nullptr;
        }
        QString name = parts.size() > 1 ? parts[1] : QStringLiteral("PID %1").arg(pid);
        auto prov = std::make_unique<KernelProcessProvider>((void*)m_driverHandle, pid, name);
        if (!prov->isValid()) {
            if (errorMsg)
                *errorMsg = QStringLiteral("Failed to read process %1 (PID: %2) via kernel driver.")
                    .arg(name).arg(pid);
            return nullptr;
        }
        return prov;
    }

    if (target.startsWith(QStringLiteral("phys:"))) {
        // phys:{baseAddr}
        bool ok = false;
        uint64_t baseAddr = target.mid(5).toULongLong(&ok, 16);
        if (!ok) baseAddr = 0;
        return std::make_unique<KernelPhysProvider>((void*)m_driverHandle, baseAddr);
    }

#endif

    if (errorMsg) *errorMsg = QStringLiteral("Unknown target format: ") + target;
    return nullptr;
}

uint64_t KernelMemoryPlugin::getInitialBaseAddress(const QString& target) const
{
    if (target.startsWith(QStringLiteral("phys:"))) {
        bool ok = false;
        uint64_t addr = target.mid(5).toULongLong(&ok, 16);
        return ok ? addr : 0;
    }
    // For process mode, the provider discovers base via modules
    return 0;
}

bool KernelMemoryPlugin::selectTarget(QWidget* parent, QString* target)
{
    // Show process picker directly (physical memory is accessed via
    // context menu "Browse Page Tables" / "Follow Physical Frame" on an
    // attached kernel process).
    QVector<PluginProcessInfo> pluginProcesses = enumerateProcesses();
    QList<ProcessInfo> processes;
    for (const auto& pinfo : pluginProcesses) {
        ProcessInfo info;
        info.pid    = pinfo.pid;
        info.name   = pinfo.name;
        info.path   = pinfo.path;
        info.icon   = pinfo.icon;
        info.is32Bit = pinfo.is32Bit;
        processes.append(info);
    }

    ProcessPicker picker(processes, parent);
    if (picker.exec() == QDialog::Accepted) {
        uint32_t pid = picker.selectedProcessId();
        QString name = picker.selectedProcessName();
        *target = QStringLiteral("km:%1:%2").arg(pid).arg(name);
        return true;
    }
    return false;
}

QVector<PluginProcessInfo> KernelMemoryPlugin::enumerateProcesses()
{
    QVector<PluginProcessInfo> processes;

#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return processes;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            PluginProcessInfo info;
            info.pid  = entry.th32ProcessID;
            info.name = QString::fromWCharArray(entry.szExeFile);

            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (hProcess) {
                wchar_t path[MAX_PATH * 2];
                DWORD pathLen = sizeof(path) / sizeof(wchar_t);

                if (QueryFullProcessImageNameW(hProcess, 0, path, &pathLen)) {
                    info.path = QString::fromWCharArray(path);

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

                BOOL isWow64 = FALSE;
                if (IsWow64Process(hProcess, &isWow64) && isWow64)
                    info.is32Bit = true;

                CloseHandle(hProcess);
            }

            processes.append(info);
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
#endif

    return processes;
}

void KernelMemoryPlugin::populatePluginMenu(QMenu* menu)
{
    if (!m_driverLoaded) return;
    menu->addAction(QStringLiteral("Unload Kernel Driver"), [this]() { unloadDriver(); });
}

// ─────────────────────────────────────────────────────────────────────────
// Driver service management
// ─────────────────────────────────────────────────────────────────────────

QString KernelMemoryPlugin::driverPath() const
{
    // Resolve rcxdrv.sys next to the plugin DLL
    QString pluginDir = QCoreApplication::applicationDirPath() + QStringLiteral("/Plugins");
    return pluginDir + QStringLiteral("/rcxdrv.sys");
}

bool KernelMemoryPlugin::ensureDriverLoaded(QString* errorMsg)
{
#ifdef _WIN32
    // Already connected?
    if (m_driverLoaded && m_driverHandle != INVALID_HANDLE_VALUE) {
        RcxDrvPingResponse ping{};
        if (ioctlCall(m_driverHandle, IOCTL_RCX_PING, nullptr, 0, &ping, sizeof(ping)))
            return true;
        // Handle went stale — close it and try to reconnect
        CloseHandle(m_driverHandle);
        m_driverHandle = INVALID_HANDLE_VALUE;
        m_driverLoaded = false;
    }

    // Show wait cursor (SCM + StartService can take seconds on first load)
    struct WaitCursorGuard {
        WaitCursorGuard() { QGuiApplication::setOverrideCursor(Qt::WaitCursor); }
        ~WaitCursorGuard() { QGuiApplication::restoreOverrideCursor(); }
    } waitCursor;

    // Fast path: driver may already be running (previous session, or after disconnect).
    // Just try to open the device handle directly.
    m_driverHandle = CreateFileA(RCX_DRV_USERMODE_PATH,
                                 GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_driverHandle != INVALID_HANDLE_VALUE) {
        RcxDrvPingResponse ping{};
        if (ioctlCall(m_driverHandle, IOCTL_RCX_PING, nullptr, 0, &ping, sizeof(ping))) {
            m_driverLoaded = true;
            return true;
        }
        CloseHandle(m_driverHandle);
        m_driverHandle = INVALID_HANDLE_VALUE;
    }

    // Slow path: need to install/start the service.
    QString sysPath = driverPath();
    if (!QFileInfo::exists(sysPath)) {
        if (errorMsg)
            *errorMsg = QStringLiteral("Driver not found: %1\n\n"
                "Place rcxdrv.sys in the Plugins folder next to the plugin DLL.").arg(sysPath);
        return false;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        if (errorMsg)
            *errorMsg = QStringLiteral("Failed to open Service Control Manager.\n"
                "Run Reclass as Administrator to load the kernel driver.");
        return false;
    }

    // Try to open existing service first
    SC_HANDLE svc = OpenServiceW(scm, L"RcxDrv", SERVICE_ALL_ACCESS);
    if (!svc) {
        // Service doesn't exist — create it
        std::wstring wPath = sysPath.toStdWString();
        svc = CreateServiceW(scm, L"RcxDrv", L"RcxDrv",
                             SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
                             SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                             wPath.c_str(),
                             nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!svc) {
            DWORD err = GetLastError();
            if (errorMsg)
                *errorMsg = QStringLiteral("Failed to create driver service (error %1).\n"
                    "Ensure test signing is enabled: bcdedit /set testsigning on").arg(err);
            CloseServiceHandle(scm);
            return false;
        }
    }

    // Start service (ERROR_SERVICE_ALREADY_RUNNING is fine — means it's already up)
    if (!StartServiceW(svc, 0, nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            if (errorMsg)
                *errorMsg = QStringLiteral("Failed to start driver (error %1).\n"
                    "Ensure test signing is enabled and the driver is properly signed.").arg(err);
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
    }

    // Done with SCM — don't hold handles open
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    // Open device handle
    m_driverHandle = CreateFileA(RCX_DRV_USERMODE_PATH,
                                 GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_driverHandle == INVALID_HANDLE_VALUE) {
        if (errorMsg)
            *errorMsg = QStringLiteral("Driver started but could not open device handle.\n"
                "Device path: %1").arg(QString::fromLatin1(RCX_DRV_USERMODE_PATH));
        return false;
    }

    // Verify with ping
    RcxDrvPingResponse ping{};
    if (!ioctlCall(m_driverHandle, IOCTL_RCX_PING, nullptr, 0, &ping, sizeof(ping))) {
        if (errorMsg)
            *errorMsg = QStringLiteral("Driver opened but ping failed.");
        CloseHandle(m_driverHandle);
        m_driverHandle = INVALID_HANDLE_VALUE;
        return false;
    }

    m_driverLoaded = true;
    return true;
#else
    if (errorMsg)
        *errorMsg = QStringLiteral("Kernel driver is only supported on Windows.");
    return false;
#endif
}

void KernelMemoryPlugin::unloadDriver()
{
#ifdef _WIN32
    // Close device handle only — service stays running so we can reconnect
    if (m_driverHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_driverHandle);
        m_driverHandle = INVALID_HANDLE_VALUE;
    }
    m_driverLoaded = false;
#endif
}

void KernelMemoryPlugin::stopDriver()
{
#ifdef _WIN32
    unloadDriver();

    // Full cleanup: stop + delete the service
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, L"RcxDrv", SERVICE_ALL_ACCESS);
        if (svc) {
            SERVICE_STATUS ss;
            ControlService(svc, SERVICE_CONTROL_STOP, &ss);
            DeleteService(svc);
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────
// Plugin factory
// ─────────────────────────────────────────────────────────────────────────

extern "C" RCX_PLUGIN_EXPORT IPlugin* CreatePlugin()
{
    return new KernelMemoryPlugin();
}
