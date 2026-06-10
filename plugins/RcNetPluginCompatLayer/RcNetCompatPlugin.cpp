#include "RcNetCompatPlugin.h"
#include "RcNetCompatProvider.h"
#include "../../src/processpicker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStyle>

#include <windows.h>

// -- Helpers --------------------------------------------------------------

QIcon RcNetCompatPlugin::Icon() const
{
    return qApp->style()->standardIcon(QStyle::SP_TrashIcon);
}

// --.NET assembly detection ----------------------------------------------

static bool isDotNetAssembly(const QString& path)
{
    // A .NET assembly has a non-zero CLR header directory entry in the PE
    // optional header.  We check this by loading the PE without running
    // DllMain and inspecting the IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR.
    HMODULE hMod = GetModuleHandleW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if (!hMod)
        hMod = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()),
                              nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!hMod) return false;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hMod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(hMod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    constexpr DWORD kClrIndex = IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR; // 14
    DWORD rva = 0, dirSize = 0;

    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        auto* opt = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(&nt->OptionalHeader);
        if (opt->NumberOfRvaAndSizes > kClrIndex) {
            rva     = opt->DataDirectory[kClrIndex].VirtualAddress;
            dirSize = opt->DataDirectory[kClrIndex].Size;
        }
    } else {
        auto* opt = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(&nt->OptionalHeader);
        if (opt->NumberOfRvaAndSizes > kClrIndex) {
            rva     = opt->DataDirectory[kClrIndex].VirtualAddress;
            dirSize = opt->DataDirectory[kClrIndex].Size;
        }
    }

    return rva != 0 && dirSize != 0;
}

// --Unified loader (dispatches native vs managed) ------------------------

bool RcNetCompatPlugin::loadPlugin(const QString& path, QString* errorMsg)
{
    if (m_dllPath == path && (m_lib || m_isManaged))
        return true; // Already loaded

    if (isDotNetAssembly(path)) {
#ifdef HAS_CLR_BRIDGE
        return loadManagedDll(path, errorMsg);
#else
        if (errorMsg)
            *errorMsg = QStringLiteral(
                "This is a .NET assembly.\n\n"
                "This build does not include .NET bridge support.\n"
                "Rebuild with the .NET SDK installed to enable managed plugin loading.");
        return false;
#endif
    }
    return loadNativeDll(path, errorMsg);
}

// --Native DLL loading ---------------------------------------------------

bool RcNetCompatPlugin::loadNativeDll(const QString& path, QString* errorMsg)
{
    unloadNativeDll();

    m_lib = std::make_unique<QLibrary>(path);
    if (!m_lib->load()) {
        if (errorMsg)
            *errorMsg = QStringLiteral("Failed to load DLL: %1").arg(m_lib->errorString());
        m_lib.reset();
        return false;
    }

    // Resolve all function pointers
    m_fns.EnumerateProcesses =
        reinterpret_cast<FnEnumerateProcesses>(m_lib->resolve("EnumerateProcesses"));
    m_fns.OpenRemoteProcess =
        reinterpret_cast<FnOpenRemoteProcess>(m_lib->resolve("OpenRemoteProcess"));
    m_fns.IsProcessValid =
        reinterpret_cast<FnIsProcessValid>(m_lib->resolve("IsProcessValid"));
    m_fns.CloseRemoteProcess =
        reinterpret_cast<FnCloseRemoteProcess>(m_lib->resolve("CloseRemoteProcess"));
    m_fns.ReadRemoteMemory =
        reinterpret_cast<FnReadRemoteMemory>(m_lib->resolve("ReadRemoteMemory"));
    m_fns.WriteRemoteMemory =
        reinterpret_cast<FnWriteRemoteMemory>(m_lib->resolve("WriteRemoteMemory"));
    m_fns.EnumerateRemoteSectionsAndModules =
        reinterpret_cast<FnEnumerateRemoteSectionsAndModules>(
            m_lib->resolve("EnumerateRemoteSectionsAndModules"));
    m_fns.ControlRemoteProcess =
        reinterpret_cast<FnControlRemoteProcess>(m_lib->resolve("ControlRemoteProcess"));

    // At minimum we need read + open + close
    if (!m_fns.ReadRemoteMemory || !m_fns.OpenRemoteProcess || !m_fns.CloseRemoteProcess || !m_fns.EnumerateProcesses) {
        if (errorMsg)
            *errorMsg = QStringLiteral(
                "DLL is missing required exports (ReadRemoteMemory, OpenRemoteProcess, "
                "CloseRemoteProcess, EnumerateProcesses). Is this a ReClass.NET native plugin?");
        m_lib->unload();
        m_lib.reset();
        m_fns = {};
        return false;
    }

    m_dllPath  = path;
    m_isManaged = false;
    return true;
}

void RcNetCompatPlugin::unloadNativeDll()
{
    if (m_lib) {
        m_lib->unload();
        m_lib.reset();
    }
    m_fns = {};
    m_dllPath.clear();
    m_isManaged = false;
}

// --Managed (.NET) DLL loading via CLR bridge ----------------------------

#ifdef HAS_CLR_BRIDGE

bool RcNetCompatPlugin::loadManagedDll(const QString& path, QString* errorMsg)
{
    unloadNativeDll();

    // Lazily create the CLR host (one per plugin lifetime)
    if (!m_clrHost)
        m_clrHost = std::make_unique<ClrHost>();

    if (!m_clrHost->isAvailable()) {
        if (errorMsg)
            *errorMsg = QStringLiteral(
                ".NET Framework 4.x is not available on this machine.\n"
                "Install the .NET Framework 4.7.2+ runtime to load managed plugins.");
        return false;
    }

    // Locate RcNetBridge.dll next to our own plugin DLL
    // Use native separators -- the CLR expects Windows-style backslash paths.
    QString bridgePath = QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath()
        + QStringLiteral("/Plugins/RcNetBridge.dll"));

    if (!QFileInfo::exists(bridgePath)) {
        if (errorMsg)
            *errorMsg = QStringLiteral(
                "RcNetBridge.dll not found in the Plugins folder.\n"
                "Expected at: %1").arg(bridgePath);
        return false;
    }

    m_fns = {};
    QString nativePath = QDir::toNativeSeparators(path);
    if (!m_clrHost->loadManagedPlugin(bridgePath, nativePath, &m_fns, errorMsg))
        return false;

    m_dllPath  = path;
    m_isManaged = true;
    return true;
}

#endif // HAS_CLR_BRIDGE

// --IProviderPlugin ------------------------------------------------------

bool RcNetCompatPlugin::canHandle(const QString& target) const
{
    // Target format: "dllpath|pid:name"
    return target.contains('|');
}

std::unique_ptr<rcx::Provider> RcNetCompatPlugin::createProvider(
    const QString& target, QString* errorMsg)
{
    // Parse "dllpath|pid:name"
    int sep = target.indexOf('|');
    if (sep < 0) {
        if (errorMsg) *errorMsg = QStringLiteral("Invalid target format");
        return nullptr;
    }

    QString dllPath = target.left(sep);
    QString pidPart = target.mid(sep + 1);

    // Load (or reuse) the plugin DLL
    if (!loadPlugin(dllPath, errorMsg))
        return nullptr;

    // Parse pid:name
    QStringList parts = pidPart.split(':');
    bool ok = false;
    uint32_t pid = parts[0].toUInt(&ok);
    if (!ok || pid == 0) {
        if (errorMsg) *errorMsg = QStringLiteral("Invalid PID: %1").arg(parts[0]);
        return nullptr;
    }
    QString procName = parts.size() > 1 ? parts[1] : QStringLiteral("PID %1").arg(pid);

    auto provider = std::make_unique<RcNetCompatProvider>(m_fns, pid, procName);
    if (!provider->isValid()) {
        if (errorMsg)
            *errorMsg = QStringLiteral(
                "Failed to open process %1 (PID: %2) via ReClass.NET plugin.\n"
                "Ensure the process is running and the plugin supports it.")
                .arg(procName).arg(pid);
        return nullptr;
    }

    return provider;
}

uint64_t RcNetCompatPlugin::getInitialBaseAddress(const QString& target) const
{
    Q_UNUSED(target);
    // The provider sets its own base from module enumeration.
    return 0;
}

bool RcNetCompatPlugin::selectTarget(QWidget* parent, QString* target)
{
    // Step 1: Pick a ReClass.NET plugin DLL (native or .NET)
    QString dllPath = QFileDialog::getOpenFileName(
        parent,
        QStringLiteral("Select ReClass.NET Plugin"),
        QString(),
        QStringLiteral("DLL Files (*.dll)"));

    if (dllPath.isEmpty())
        return false;

    // Step 2: Load and validate the DLL
    QString loadErr;
    if (!loadPlugin(dllPath, &loadErr)) {
        QMessageBox::warning(parent,
                             QStringLiteral("ReClass.NET Compat Layer"),
                             loadErr);
        return false;
    }

    // Step 3: Enumerate processes and show picker
    QVector<PluginProcessInfo> pluginProcesses = enumerateProcesses();

    QList<ProcessInfo> processes;
    for (const auto& p : pluginProcesses) {
        ProcessInfo info;
        info.pid  = p.pid;
        info.name = p.name;
        info.path = p.path;
        info.icon = p.icon;
        processes.append(info);
    }

    ProcessPicker picker(processes, parent);
    if (picker.exec() != QDialog::Accepted)
        return false;

    uint32_t pid = picker.selectedProcessId();
    QString name = picker.selectedProcessName();

    // Step 4: Format target as "dllpath|pid:name"
    *target = QStringLiteral("%1|%2:%3").arg(dllPath).arg(pid).arg(name);
    return true;
}

// --Process enumeration --------------------------------------------------

namespace {

struct ProcessCollector {
    QVector<PluginProcessInfo>* dest = nullptr;
};
thread_local ProcessCollector g_processCollector;

void RC_CALLCONV processCallback(EnumerateProcessData* data)
{
    if (!data || !g_processCollector.dest) return;

    PluginProcessInfo info;
    info.pid  = static_cast<uint32_t>(data->Id);
    info.name = QString::fromUtf16(data->Name);
    info.path = QString::fromUtf16(data->Path);
    g_processCollector.dest->append(info);
}

} // anonymous namespace

QVector<PluginProcessInfo> RcNetCompatPlugin::enumerateProcesses()
{
    QVector<PluginProcessInfo> result;

    if (!m_fns.EnumerateProcesses)
        return result;

    g_processCollector.dest = &result;
    m_fns.EnumerateProcesses(processCallback);
    g_processCollector.dest = nullptr;

    return result;
}

// --Plugin factory -------------------------------------------------------

extern "C" RCX_PLUGIN_EXPORT IPlugin* CreatePlugin()
{
    return new RcNetCompatPlugin();
}

// ABI guard token — lets the host reject this DLL if it was built against an
// incompatible rcx::Provider layout instead of crashing. See iplugin.h.
RCX_DEFINE_PLUGIN_ABI()
