#include "ProcessMemoryPlugin.h"
#include "../../src/processpicker.h"
#include <QStyle>
#include <QApplication>
#include <QRegularExpression>
#include <QMessageBox>
#include <QPixmap>
#include <QImage>

// ──────────────────────────────────────────────────────────────────────────
// ProcessMemoryProvider implementation
// ──────────────────────────────────────────────────────────────────────────

ProcessMemoryProvider::ProcessMemoryProvider(DWORD pid, const QString& processName)
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

    if (m_handle)
    {
        cacheModules();
    }
}

ProcessMemoryProvider::~ProcessMemoryProvider()
{
    if (m_handle)
        CloseHandle(m_handle);
}

bool ProcessMemoryProvider::read(uint64_t addr, void* buf, int len) const
{
    if (!m_handle || len <= 0) return false;
    
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(m_handle, (LPCVOID)(m_base + addr), buf, (SIZE_T)len, &bytesRead))
        return bytesRead == (SIZE_T)len;
    return false;
}

bool ProcessMemoryProvider::write(uint64_t addr, const void* buf, int len)
{
    if (!m_handle || !m_writable || len <= 0) return false;
    
    SIZE_T bytesWritten = 0;
    if (WriteProcessMemory(m_handle, (LPVOID)(m_base + addr), buf, (SIZE_T)len, &bytesWritten))
        return bytesWritten == (SIZE_T)len;
    return false;
}

QString ProcessMemoryProvider::getSymbol(uint64_t addr) const
{
    // TODO: Implement module enumeration with EnumProcessModules
    // For now, just return empty (no symbol resolution)
    Q_UNUSED(addr);
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
    DWORD pid = parts[0].toUInt(&ok);
    
    if (!ok || pid == 0) {
        if (errorMsg) *errorMsg = "Invalid PID: " + target;
        return nullptr;
    }
    
    QString name = parts.size() > 1 ? parts[1] : QString("PID %1").arg(pid);
    
    auto provider = std::make_unique<ProcessMemoryProvider>(pid, name);
    if (!provider->isValid())
    {
        if (errorMsg)
        {
            *errorMsg = QString("Failed to open process %1 (PID: %2)\n"
                               "Ensure the process is running and you have sufficient permissions.")
                        .arg(name).arg(pid);
        }
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
        {
            base = (uint64_t)mi.lpBaseOfDll;
        }
    }
    
    CloseHandle(hProc);
    return base;
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
                            QPixmap pixmap = QPixmap::fromImage(QImage::fromHICON(sfi.hIcon));
                            info.icon = QIcon(pixmap);
                            DestroyIcon(sfi.hIcon);
                        }
                    }
                }
                
                CloseHandle(hProcess);
            }
            
            processes.append(info);
            
        } while (Process32NextW(snapshot, &entry));
    }
    
    CloseHandle(snapshot);
#endif
    
    return processes;
}

// ──────────────────────────────────────────────────────────────────────────
// Plugin factory
// ──────────────────────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) IPlugin* CreatePlugin()
{
    return new ProcessMemoryPlugin();
}
