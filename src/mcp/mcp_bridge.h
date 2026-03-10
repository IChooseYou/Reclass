#pragma once
#include "mainwindow.h"
#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QByteArray>
#include <QTimer>

namespace rcx {

class McpBridge : public QObject {
    Q_OBJECT
public:
    explicit McpBridge(MainWindow* mainWindow, QObject* parent = nullptr);
    ~McpBridge() override;

    void start();
    void stop();
    bool isRunning() const { return m_server != nullptr; }

    bool slowMode() const { return m_slowMode; }
    void setSlowMode(bool v) { m_slowMode = v; }

    // Call from controller refresh / data change to notify MCP clients
    void notifyTreeChanged();
    void notifyDataChanged();

private:
    struct ClientState {
        QLocalSocket* socket = nullptr;
        QByteArray    readBuffer;
        bool          initialized = false;
    };

    MainWindow*    m_mainWindow;
    QLocalServer*  m_server    = nullptr;
    QVector<ClientState> m_clients;
    QLocalSocket*  m_currentSender = nullptr;  // set during request processing
    bool           m_slowMode    = false;
    QTimer*        m_notifyTimer = nullptr;

    // Serial request queue. Some tool calls (scanner, tree.apply) spin nested
    // event loops which would let another client's readyRead interleave and
    // clobber m_currentSender. Simplest fix without refactoring those tools:
    // queue incoming lines while a request is in flight, drain after.
    bool m_processing = false;
    struct PendingRequest { QLocalSocket* socket; QByteArray line; };
    QVector<PendingRequest> m_pendingRequests;


    ClientState* findClient(QLocalSocket* sock);
    void removeClient(QLocalSocket* sock);
    void drainPendingRequests();

    // JSON-RPC plumbing
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void processLine(const QByteArray& line);
    void sendJson(const QJsonObject& obj);
    QJsonObject okReply(const QJsonValue& id, const QJsonObject& result);
    QJsonObject errReply(const QJsonValue& id, int code, const QString& msg);
    void sendNotification(const QString& method, const QJsonObject& params = {});

    // MCP method handlers
    QJsonObject handleInitialize(const QJsonValue& id, const QJsonObject& params);
    QJsonObject handleToolsList(const QJsonValue& id);
    QJsonObject handleToolsCall(const QJsonValue& id, const QJsonObject& params);

    // Tool implementations
    QJsonObject toolProjectState(const QJsonObject& args);
    QJsonObject toolTreeApply(const QJsonObject& args);
    QJsonObject toolSourceSwitch(const QJsonObject& args);
    QJsonObject toolSourceModules(const QJsonObject& args);
    QJsonObject toolHexRead(const QJsonObject& args);
    QJsonObject toolHexWrite(const QJsonObject& args);
    QJsonObject toolStatusSet(const QJsonObject& args);
    QJsonObject toolUiAction(const QJsonObject& args);
    QJsonObject toolTreeSearch(const QJsonObject& args);
    QJsonObject toolNodeHistory(const QJsonObject& args);
    QJsonObject toolScannerScan(const QJsonObject& args);
    QJsonObject toolScannerScanPattern(const QJsonObject& args);
    QJsonObject toolReconnect(const QJsonObject& args);
    QJsonObject toolProcessInfo(const QJsonObject& args);

    // Helpers
    QJsonObject makeTextResult(const QString& text, bool isError = false);
    QString resolvePlaceholder(const QString& ref,
                               const QHash<QString, uint64_t>& placeholderMap,
                               bool* ok = nullptr);

    // Smart tab resolution: tabIndex arg → activeTab → first tab → auto-create
    MainWindow::TabState* resolveTab(const QJsonObject& args, int* resolvedIndex = nullptr);
};

} // namespace rcx
