#include "WinDbgMemoryPlugin.h"

#include <QStyle>
#include <QApplication>
#include <QMessageBox>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QDebug>
#include <QClipboard>
#include <QGuiApplication>

#ifdef _WIN32
#include <windows.h>
#include <initguid.h>
#include <dbgeng.h>
#pragma comment(lib, "dbgeng.lib")
#endif

// ──────────────────────────────────────────────────────────────────────────
// Thread dispatch helper
// ──────────────────────────────────────────────────────────────────────────

template<typename Fn>
void WinDbgMemoryProvider::dispatchToOwner(Fn&& fn) const
{
    if (!m_dispatcher) { fn(); return; }

    if (QThread::currentThread() == m_dispatcher->thread()) {
        // Already on the owning thread — call directly
        fn();
    } else {
        // Marshal to the owning thread and block until done
        QMetaObject::invokeMethod(m_dispatcher, std::forward<Fn>(fn),
                                  Qt::BlockingQueuedConnection);
    }
}

// ──────────────────────────────────────────────────────────────────────────
// WinDbgMemoryProvider implementation
// ──────────────────────────────────────────────────────────────────────────

WinDbgMemoryProvider::WinDbgMemoryProvider(const QString& target)
{
    // Create a dedicated thread for all DbgEng COM operations.
    // DbgEng's remote transport (TCP/named-pipe) is thread-affine — all
    // calls must happen on the thread that called DebugConnect/DebugCreate.
    // A private thread with its own event loop guarantees:
    //   1. dispatchToOwner() works from any calling thread (main, thread-pool, etc.)
    //   2. No deadlock — the DbgEng thread is never blocked by the caller
    m_dbgThread = new QThread();
    m_dbgThread->setObjectName(QStringLiteral("DbgEngThread"));
    m_dbgThread->start();

    m_dispatcher = new DbgEngDispatcher();
    m_dispatcher->moveToThread(m_dbgThread);

#ifdef _WIN32
    // Run all DbgEng initialization on the dedicated thread.
    // BlockingQueuedConnection blocks us until the lambda finishes,
    // so member variables written inside are visible after the call.
    dispatchToOwner([this, &target]() {
        HRESULT hr;

        qDebug() << "[WinDbg] Opening target:" << target
                 << "on DbgEng thread" << QThread::currentThread();

        if (target.startsWith("tcp:", Qt::CaseInsensitive)
            || target.startsWith("npipe:", Qt::CaseInsensitive))
        {
            // ── Remote: connect to existing WinDbg debug server ──
            QByteArray connUtf8 = target.toUtf8();
            qDebug() << "[WinDbg] DebugConnect:" << target;
            hr = DebugConnect(connUtf8.constData(), IID_IDebugClient, (void**)&m_client);
            qDebug() << "[WinDbg] DebugConnect hr=" << Qt::hex << (unsigned long)hr
                     << "client=" << (void*)m_client;
            if (FAILED(hr) || !m_client) {
                qWarning() << "[WinDbg] DebugConnect FAILED hr=0x" << Qt::hex << (unsigned long)hr;
                return;
            }
            m_isRemote = true;
        }
        else
        {
            // ── Local: create debug client for pid/dump ──
            hr = DebugCreate(IID_IDebugClient, (void**)&m_client);
            qDebug() << "[WinDbg] DebugCreate hr=" << Qt::hex << (unsigned long)hr
                     << "client=" << (void*)m_client;
            if (FAILED(hr) || !m_client) {
                qWarning() << "[WinDbg] DebugCreate FAILED hr=0x" << Qt::hex << (unsigned long)hr;
                return;
            }

            if (target.startsWith("pid:", Qt::CaseInsensitive))
            {
                bool ok = false;
                ULONG pid = target.mid(4).trimmed().toULong(&ok);
                if (!ok || pid == 0) {
                    qWarning() << "[WinDbg] Invalid PID in target:" << target;
                    cleanup();
                    return;
                }

                qDebug() << "[WinDbg] Attaching to PID" << pid << "(non-invasive)";
                hr = m_client->AttachProcess(
                    0, pid,
                    DEBUG_ATTACH_NONINVASIVE | DEBUG_ATTACH_NONINVASIVE_NO_SUSPEND);
                qDebug() << "[WinDbg] AttachProcess hr=" << Qt::hex << (unsigned long)hr;
                if (FAILED(hr)) {
                    qWarning() << "[WinDbg] AttachProcess FAILED";
                    cleanup();
                    return;
                }
            }
            else if (target.startsWith("dump:", Qt::CaseInsensitive))
            {
                QString path = target.mid(5).trimmed();
                QByteArray pathUtf8 = path.toUtf8();

                qDebug() << "[WinDbg] Opening dump file:" << path;
                hr = m_client->OpenDumpFile(pathUtf8.constData());
                qDebug() << "[WinDbg] OpenDumpFile hr=" << Qt::hex << (unsigned long)hr;
                if (FAILED(hr)) {
                    qWarning() << "[WinDbg] OpenDumpFile FAILED";
                    cleanup();
                    return;
                }
            }
            else
            {
                qWarning() << "[WinDbg] Unknown target format:" << target;
                cleanup();
                return;
            }
        }

        initInterfaces();

        // WaitForEvent to finalize the attach/dump load.
        // For remote connections the server session is already active — skip.
        if (m_control && !m_isRemote) {
            qDebug() << "[WinDbg] WaitForEvent...";
            hr = m_control->WaitForEvent(0, 10000);
            qDebug() << "[WinDbg] WaitForEvent hr=" << Qt::hex << (unsigned long)hr;
        }

        querySessionInfo();
    });

#else
    Q_UNUSED(target);
#endif
}

void WinDbgMemoryProvider::initInterfaces()
{
#ifdef _WIN32
    if (!m_client) return;

    HRESULT hr;
    hr = m_client->QueryInterface(IID_IDebugDataSpaces, (void**)&m_dataSpaces);
    qDebug() << "[WinDbg] IDebugDataSpaces hr=" << Qt::hex << (unsigned long)hr
             << "ptr=" << (void*)m_dataSpaces;

    hr = m_client->QueryInterface(IID_IDebugControl, (void**)&m_control);
    qDebug() << "[WinDbg] IDebugControl hr=" << Qt::hex << (unsigned long)hr
             << "ptr=" << (void*)m_control;

    hr = m_client->QueryInterface(IID_IDebugSymbols, (void**)&m_symbols);
    qDebug() << "[WinDbg] IDebugSymbols hr=" << Qt::hex << (unsigned long)hr
             << "ptr=" << (void*)m_symbols;

    if (!m_dataSpaces) {
        qWarning() << "[WinDbg] No IDebugDataSpaces — cleaning up";
        cleanup();
    }
#endif
}

void WinDbgMemoryProvider::querySessionInfo()
{
#ifdef _WIN32
    if (!m_client) return;
    HRESULT hr;

    if (m_control) {
        ULONG debugClass = 0, debugQualifier = 0;
        hr = m_control->GetDebuggeeType(&debugClass, &debugQualifier);
        qDebug() << "[WinDbg] GetDebuggeeType hr=" << Qt::hex << (unsigned long)hr
                 << "class=" << debugClass << "qualifier=" << debugQualifier;
        if (SUCCEEDED(hr)) {
            m_isLive = (debugQualifier < DEBUG_DUMP_SMALL);
            m_writable = m_isLive;
        }
    }

    if (m_symbols) {
        ULONG numModules = 0, numUnloaded = 0;
        hr = m_symbols->GetNumberModules(&numModules, &numUnloaded);
        qDebug() << "[WinDbg] GetNumberModules hr=" << Qt::hex << (unsigned long)hr
                 << "loaded=" << numModules << "unloaded=" << numUnloaded;
        if (SUCCEEDED(hr) && numModules > 0) {
            char modName[256] = {};
            ULONG modSize = 0;
            hr = m_symbols->GetModuleNames(0, 0, nullptr, 0, nullptr,
                                            modName, sizeof(modName), &modSize,
                                            nullptr, 0, nullptr);
            if (SUCCEEDED(hr) && modSize > 0)
                m_name = QString::fromUtf8(modName);
        }
    }

    if (m_name.isEmpty())
        m_name = m_isLive ? QStringLiteral("DbgEng (Live)") : QStringLiteral("DbgEng (Dump)");

    if (m_symbols) {
        ULONG numModules = 0, numUnloaded = 0;
        hr = m_symbols->GetNumberModules(&numModules, &numUnloaded);
        if (SUCCEEDED(hr) && numModules > 0) {
            ULONG64 moduleBase = 0;
            hr = m_symbols->GetModuleByIndex(0, &moduleBase);
            qDebug() << "[WinDbg] Module 0 base=" << Qt::hex << moduleBase;
            if (SUCCEEDED(hr))
                m_base = moduleBase;
        }
    }

    if (m_base && m_dataSpaces) {
        uint8_t probe[2] = {};
        ULONG got = 0;
        hr = m_dataSpaces->ReadVirtual(m_base, probe, 2, &got);
        qDebug() << "[WinDbg] Probe read at" << Qt::hex << m_base
                 << "hr=" << (unsigned long)hr << "got=" << got
                 << "bytes:" << (int)probe[0] << (int)probe[1];
        if (FAILED(hr) || got == 0) {
            qWarning() << "[WinDbg] Probe read FAILED — cleaning up";
            cleanup();
            return;
        }
    }

    qDebug() << "[WinDbg] Ready. name=" << m_name
             << "base=" << Qt::hex << m_base << "isLive=" << m_isLive;
#endif
}

WinDbgMemoryProvider::~WinDbgMemoryProvider()
{
#ifdef _WIN32
    // Dispatch COM cleanup to the DbgEng thread (thread-affine release)
    if (m_dbgThread && m_dbgThread->isRunning() && m_dispatcher) {
        dispatchToOwner([this]() {
            if (m_client) {
                if (m_isRemote)
                    m_client->EndSession(DEBUG_END_DISCONNECT);
                else
                    m_client->DetachProcesses();
            }
            cleanup();
        });
    } else {
        // Thread not running — clean up directly (best-effort)
        if (m_client) {
            if (m_isRemote)
                m_client->EndSession(DEBUG_END_DISCONNECT);
            else
                m_client->DetachProcesses();
        }
        cleanup();
    }
#else
    cleanup();
#endif

    // Stop the dedicated thread
    if (m_dbgThread) {
        m_dbgThread->quit();
        m_dbgThread->wait(3000);
        delete m_dbgThread;
        m_dbgThread = nullptr;
    }
    delete m_dispatcher;
    m_dispatcher = nullptr;
}

void WinDbgMemoryProvider::cleanup()
{
#ifdef _WIN32
    if (m_symbols)    { m_symbols->Release();    m_symbols = nullptr; }
    if (m_control)    { m_control->Release();    m_control = nullptr; }
    if (m_dataSpaces) { m_dataSpaces->Release(); m_dataSpaces = nullptr; }
    if (m_client)     { m_client->Release();     m_client = nullptr; }
#endif
}

bool WinDbgMemoryProvider::read(uint64_t addr, void* buf, int len) const
{
#ifdef _WIN32
    if (!m_dataSpaces || len <= 0) return false;

    bool result = false;
    dispatchToOwner([&]() {
        ULONG bytesRead = 0;
        HRESULT hr = m_dataSpaces->ReadVirtual(m_base + addr, buf, (ULONG)len, &bytesRead);
        if (FAILED(hr) || (int)bytesRead < len)
            memset((char*)buf + bytesRead, 0, len - bytesRead);
        result = bytesRead > 0;
    });
    return result;
#else
    Q_UNUSED(addr); Q_UNUSED(buf); Q_UNUSED(len);
    return false;
#endif
}

bool WinDbgMemoryProvider::write(uint64_t addr, const void* buf, int len)
{
#ifdef _WIN32
    if (!m_dataSpaces || !m_writable || len <= 0) return false;

    bool result = false;
    dispatchToOwner([&]() {
        ULONG bytesWritten = 0;
        HRESULT hr = m_dataSpaces->WriteVirtual(m_base + addr, const_cast<void*>(buf),
                                                 (ULONG)len, &bytesWritten);
        result = SUCCEEDED(hr) && bytesWritten == (ULONG)len;
    });
    return result;
#else
    Q_UNUSED(addr); Q_UNUSED(buf); Q_UNUSED(len);
    return false;
#endif
}

int WinDbgMemoryProvider::size() const
{
#ifdef _WIN32
    return m_dataSpaces ? 0x10000 : 0;
#else
    return 0;
#endif
}

bool WinDbgMemoryProvider::isReadable(uint64_t /*addr*/, int len) const
{
#ifdef _WIN32
    // DbgEng's ReadVirtual can read any mapped virtual address.
    return m_dataSpaces != nullptr && len >= 0;
#else
    return false;
#endif
}

QString WinDbgMemoryProvider::getSymbol(uint64_t addr) const
{
#ifdef _WIN32
    if (!m_symbols) return {};

    QString result;
    dispatchToOwner([&]() {
        char nameBuf[512] = {};
        ULONG nameSize = 0;
        ULONG64 displacement = 0;
        HRESULT hr = m_symbols->GetNameByOffset(m_base + addr, nameBuf, sizeof(nameBuf),
                                                 &nameSize, &displacement);
        if (SUCCEEDED(hr) && nameSize > 0) {
            result = QString::fromUtf8(nameBuf);
            if (displacement > 0)
                result += QStringLiteral("+0x%1").arg(displacement, 0, 16);
        }
    });
    return result;
#else
    Q_UNUSED(addr);
    return {};
#endif
}

// ──────────────────────────────────────────────────────────────────────────
// WinDbgMemoryPlugin implementation
// ──────────────────────────────────────────────────────────────────────────

QIcon WinDbgMemoryPlugin::Icon() const
{
    return qApp->style()->standardIcon(QStyle::SP_DriveNetIcon);
}

bool WinDbgMemoryPlugin::canHandle(const QString& target) const
{
    return target.startsWith("tcp:", Qt::CaseInsensitive)
        || target.startsWith("npipe:", Qt::CaseInsensitive)
        || target.startsWith("pid:", Qt::CaseInsensitive)
        || target.startsWith("dump:", Qt::CaseInsensitive);
}

std::unique_ptr<rcx::Provider> WinDbgMemoryPlugin::createProvider(const QString& target, QString* errorMsg)
{
    auto provider = std::make_unique<WinDbgMemoryProvider>(target);
    if (!provider->isValid())
    {
        if (errorMsg) {
            if (target.startsWith("tcp:", Qt::CaseInsensitive)
                || target.startsWith("npipe:", Qt::CaseInsensitive))
                *errorMsg = QString("Failed to connect to debug server.\n\n"
                                   "Target: %1\n\n"
                                   "Make sure WinDbg is running with a matching .server command\n"
                                   "(e.g. .server tcp:port=5055) and the port/pipe is reachable.")
                            .arg(target);
            else if (target.startsWith("pid:", Qt::CaseInsensitive))
                *errorMsg = QString("Failed to attach to process.\n\n"
                                   "Target: %1\n\n"
                                   "Make sure the process is running and you have "
                                   "sufficient privileges (try Run as Administrator).")
                            .arg(target);
            else
                *errorMsg = QString("Failed to open dump file.\n\n"
                                   "Target: %1\n\n"
                                   "Make sure the file exists and is a valid dump.")
                            .arg(target);
        }
        return nullptr;
    }
    return provider;
}

uint64_t WinDbgMemoryPlugin::getInitialBaseAddress(const QString& target) const
{
    Q_UNUSED(target);
    return 0;
}

bool WinDbgMemoryPlugin::selectTarget(QWidget* parent, QString* target)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("WinDbg Settings");
    dlg.resize(460, 260);

    QPalette dlgPal = qApp->palette();
    dlg.setPalette(dlgPal);
    dlg.setAutoFillBackground(true);

    auto* layout = new QVBoxLayout(&dlg);

    layout->addWidget(new QLabel(
        "Connect to a running WinDbg debug server.\n"
        "In WinDbg, run:  .server tcp:port=5055"));

    layout->addSpacing(8);
    layout->addWidget(new QLabel("Connection string:"));
    auto* connEdit = new QLineEdit;
    connEdit->setPlaceholderText("tcp:Port=5055,Server=localhost");
    connEdit->setText("tcp:Port=5055,Server=localhost");
    layout->addWidget(connEdit);

    layout->addSpacing(4);
    layout->addWidget(new QLabel("Run one of these in WinDbg first:"));

    auto addExample = [&](const QString& text) {
        auto* row = new QHBoxLayout;
        auto* label = new QLabel(text);
        QPalette lp = dlgPal;
        lp.setColor(QPalette::WindowText, dlgPal.color(QPalette::Disabled, QPalette::WindowText));
        label->setPalette(lp);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(label, 1);
        auto* copyBtn = new QPushButton("Copy");
        copyBtn->setFixedWidth(50);
        copyBtn->setToolTip("Copy to clipboard");
        QObject::connect(copyBtn, &QPushButton::clicked, [text]() {
            QGuiApplication::clipboard()->setText(text);
        });
        row->addWidget(copyBtn);
        layout->addLayout(row);
    };

    addExample(".server tcp:port=5055");
    addExample(".server npipe:pipe=reclass");
    layout->addStretch();

    auto* btnLayout = new QHBoxLayout;
    btnLayout->addStretch();
    auto* okBtn = new QPushButton("OK");
    auto* cancelBtn = new QPushButton("Cancel");
    btnLayout->addWidget(okBtn);
    btnLayout->addWidget(cancelBtn);
    layout->addLayout(btnLayout);

    QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    QString conn = connEdit->text().trimmed();
    if (conn.isEmpty()) return false;
    *target = conn;
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// Plugin factory
// ──────────────────────────────────────────────────────────────────────────

extern "C" RCX_PLUGIN_EXPORT IPlugin* CreatePlugin()
{
    return new WinDbgMemoryPlugin();
}
