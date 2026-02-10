#pragma once
#include "mainwindow.h"
#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QByteArray>

namespace rcx {

class McpBridge : public QObject {
    Q_OBJECT
public:
    explicit McpBridge(MainWindow* mainWindow, QObject* parent = nullptr);
    ~McpBridge() override;

    void start();
    void stop();
    bool isRunning() const { return m_server != nullptr; }

    // Call from controller refresh / data change to notify MCP clients
    void notifyTreeChanged();
    void notifyDataChanged();

private:
    MainWindow*    m_mainWindow;
    QLocalServer*  m_server    = nullptr;
    QLocalSocket*  m_client    = nullptr;  // single client for v1
    QByteArray     m_readBuffer;
    bool           m_initialized = false;

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
    QJsonObject toolHexRead(const QJsonObject& args);
    QJsonObject toolHexWrite(const QJsonObject& args);
    QJsonObject toolStatusSet(const QJsonObject& args);
    QJsonObject toolUiAction(const QJsonObject& args);

    // Helpers
    QJsonObject makeTextResult(const QString& text, bool isError = false);
    QString resolvePlaceholder(const QString& ref,
                               const QHash<QString, uint64_t>& placeholderMap);

    // Smart tab resolution: tabIndex arg → activeTab → first tab → auto-create
    MainWindow::TabState* resolveTab(const QJsonObject& args);
};

} // namespace rcx
