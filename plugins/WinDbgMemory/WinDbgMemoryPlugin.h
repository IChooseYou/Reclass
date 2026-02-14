#pragma once
#include "../../src/iplugin.h"
#include "../../src/core.h"

#include <cstdint>
#include <QObject>
#include <QThread>

// Forward declarations for DbgEng COM interfaces
struct IDebugClient;
struct IDebugDataSpaces;
struct IDebugControl;
struct IDebugSymbols;

/**
 * WinDbg memory provider
 *
 * Uses DbgEng to read memory from:
 *   - An existing WinDbg debug server via DebugConnect (tcp/npipe)
 *   - A live process by PID via DebugCreate (non-invasive attach)
 *   - A crash dump (.dmp) file via DebugCreate
 *
 * Target string format:
 *   "tcp:Port=5055,Server=localhost"   - connect to WinDbg debug server (TCP)
 *   "npipe:Pipe=name,Server=localhost" - connect to WinDbg debug server (named pipe)
 *   "pid:1234"                         - attach to process 1234
 *   "dump:C:/path/to/file.dmp"        - open dump file
 *
 * Threading: All DbgEng COM calls are dispatched to the thread that created
 * the connection (DebugConnect/DebugCreate).  This is required because the
 * remote transport (TCP/named-pipe) binds to the creating thread.  The
 * controller's background refresh threads call read() which transparently
 * marshals to the owning thread via BlockingQueuedConnection.
 */

// Helper QObject that lives on the DbgEng-owning thread.
// Used as a target for QMetaObject::invokeMethod to marshal calls.
class DbgEngDispatcher : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
};

class WinDbgMemoryProvider : public rcx::Provider
{
public:
    /// Create a provider from a target string
    WinDbgMemoryProvider(const QString& target);
    ~WinDbgMemoryProvider() override;

    // Required overrides
    bool read(uint64_t addr, void* buf, int len) const override;
    int size() const override;

    // Optional overrides
    bool isReadable(uint64_t addr, int len) const override;
    bool write(uint64_t addr, const void* buf, int len) override;
    bool isWritable() const override { return m_writable; }
    QString name() const override { return m_name; }
    QString kind() const override { return QStringLiteral("WinDbg"); }
    QString getSymbol(uint64_t addr) const override;

    bool isLive() const override { return m_isLive; }
    uint64_t base() const override { return m_base; }
    void setBase(uint64_t b) override { m_base = b; }

private:
    void initInterfaces();   // get IDebugDataSpaces/Control/Symbols from client
    void querySessionInfo(); // determine live/dump, writable, name, base
    void cleanup();

    // Marshal a lambda to the DbgEng-owning thread.  If already on that
    // thread, calls directly.  Otherwise blocks via QueuedConnection.
    template<typename Fn>
    void dispatchToOwner(Fn&& fn) const;

    IDebugClient*     m_client = nullptr;
    IDebugDataSpaces* m_dataSpaces = nullptr;
    IDebugControl*    m_control = nullptr;
    IDebugSymbols*    m_symbols = nullptr;

    QString  m_name;
    uint64_t m_base = 0;
    bool     m_isLive = false;
    bool     m_writable = false;
    bool     m_isRemote = false;   // true when connected via DebugConnect (tcp/npipe)

    // Dedicated thread for DbgEng COM operations.  The remote TCP/pipe
    // transport is thread-affine — all calls must happen on the thread
    // that called DebugConnect.  A private thread with its own event loop
    // ensures dispatchToOwner() works from any calling thread (including
    // QtConcurrent workers and the main/GUI thread) without deadlock.
    QThread*          m_dbgThread  = nullptr;
    DbgEngDispatcher* m_dispatcher = nullptr;
};

/**
 * Plugin that provides WinDbgMemoryProvider
 *
 * Uses DbgEng to read memory via:
 *   - Remote connection to an existing WinDbg debug server (tcp/npipe)
 *   - Local non-invasive attach to a live process (pid)
 *   - Local crash dump file (dump)
 */
class WinDbgMemoryPlugin : public IProviderPlugin
{
public:
    std::string Name() const override { return "WinDbg Memory"; }
    std::string Version() const override { return "2.0.0"; }
    std::string Author() const override { return "Reclass"; }
    std::string Description() const override { return "Read memory via DbgEng (live process attach or crash dump)"; }
    k_ELoadType LoadType() const override { return k_ELoadTypeAuto; }
    QIcon Icon() const override;

    bool canHandle(const QString& target) const override;
    std::unique_ptr<rcx::Provider> createProvider(const QString& target, QString* errorMsg) override;
    uint64_t getInitialBaseAddress(const QString& target) const override;
    bool selectTarget(QWidget* parent, QString* target) override;
};

// Plugin export
extern "C" RCX_PLUGIN_EXPORT IPlugin* CreatePlugin();
