// Test MCP multi-client protocol: connect, initialize, tools/list,
// disconnect one client, notification broadcast, serial requests.
// Uses a MockMcpServer with the same multi-client architecture as McpBridge.

#include <QTest>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QTimer>

// ── Mock server (same pattern as McpBridge multi-client) ──

class MockMcpServer : public QObject {
    Q_OBJECT
public:
    struct Client { QLocalSocket* socket; QByteArray buf; bool initialized; };
    QLocalServer* m_server = nullptr;
    QVector<Client> m_clients;

    bool start(const QString& name) {
        QLocalServer::removeServer(name);
        m_server = new QLocalServer(this);
        if (!m_server->listen(name)) return false;
        connect(m_server, &QLocalServer::newConnection, this, [this]() {
            while (auto* s = m_server->nextPendingConnection()) {
                m_clients.push_back(Client{s, {}, false});
                connect(s, &QLocalSocket::readyRead, this, [this, s]() { processSocket(s); });
                connect(s, &QLocalSocket::disconnected, this, [this, s]() {
                    for (int i = 0; i < m_clients.size(); i++)
                        if (m_clients[i].socket == s) { s->deleteLater(); m_clients.removeAt(i); break; }
                });
            }
        });
        return true;
    }
    void stop() {
        for (auto& c : m_clients) { c.socket->disconnect(this); c.socket->disconnectFromServer(); c.socket->deleteLater(); }
        m_clients.clear();
        if (m_server) { m_server->close(); delete m_server; m_server = nullptr; }
    }
    int clientCount() const { return m_clients.size(); }
    int initializedCount() const { int n=0; for (auto& c:m_clients) if(c.initialized) n++; return n; }

    void broadcast(const QJsonObject& obj) {
        QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
        for (auto& c : m_clients)
            if (c.initialized) { c.socket->write(data); c.socket->flush(); }
    }

private:
    void sendTo(QLocalSocket* s, const QJsonObject& obj) {
        s->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
        s->flush();
    }
    void processSocket(QLocalSocket* s) {
        Client* cs = nullptr;
        for (auto& c : m_clients) if (c.socket == s) { cs = &c; break; }
        if (!cs) return;
        cs->buf.append(s->readAll());
        while (true) {
            int idx = cs->buf.indexOf('\n');
            if (idx < 0) break;
            QByteArray line = cs->buf.left(idx).trimmed();
            cs->buf.remove(0, idx + 1);
            if (line.isEmpty()) continue;
            auto doc = QJsonDocument::fromJson(line);
            if (!doc.isObject()) {
                sendTo(s, {{"jsonrpc","2.0"},{"id",QJsonValue()},
                    {"error",QJsonObject{{"code",-32700},{"message","Parse error"}}}});
                continue;
            }
            auto req = doc.object();
            QString method = req["method"].toString();
            QJsonValue id = req["id"];
            if (method.isEmpty()) {
                sendTo(s, {{"jsonrpc","2.0"},{"id",id},
                    {"error",QJsonObject{{"code",-32600},{"message","Missing method"}}}});
            } else if (method == "initialize") {
                cs->initialized = true;
                sendTo(s, {{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{
                    {"protocolVersion","2024-11-05"},
                    {"serverInfo",QJsonObject{{"name","mock-mcp"},{"version","1.0"}}}}}});
            } else if (method == "notifications/initialized" || method == "notifications/cancelled") {
                // no-op client notifications
            } else if (method == "tools/list") {
                sendTo(s, {{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{
                    {"tools",QJsonArray{QJsonObject{{"name","test.tool"},{"description","A test"}}}}}}});
            } else if (method == "tools/call") {
                QString toolName = req["params"].toObject()["name"].toString();
                if (toolName == "mcp.reconnect") {
                    sendTo(s, {{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{
                        {"content",QJsonArray{QJsonObject{{"type","text"},{"text","Disconnected."}}}}}}});
                    // Disconnect after response is flushed
                    QTimer::singleShot(0, this, [this, s]() {
                        for (auto& cc : m_clients) if (cc.socket == s) { s->disconnectFromServer(); break; }
                    });
                } else if (toolName.isEmpty()) {
                    sendTo(s, {{"jsonrpc","2.0"},{"id",id},
                        {"error",QJsonObject{{"code",-32602},{"message","Missing tool name"}}}});
                } else {
                    sendTo(s, {{"jsonrpc","2.0"},{"id",id},
                        {"error",QJsonObject{{"code",-32601},{"message","Unknown tool"}}}});
                }
            } else {
                sendTo(s, {{"jsonrpc","2.0"},{"id",id},
                    {"error",QJsonObject{{"code",-32601},{"message","Method not found"}}}});
            }
        }
    }
};

// ── Helpers ──

static QLocalSocket* makeClient(const QString& pipe, QObject* parent) {
    auto* s = new QLocalSocket(parent);
    s->connectToServer(pipe);
    return s->waitForConnected(2000) ? s : nullptr;
}

// Send JSON-RPC and pump the event loop until we get a response line.
static QJsonObject rpc(QLocalSocket* s, const QJsonObject& req, int ms = 3000) {
    s->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    s->flush();
    QByteArray buf;
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (s->bytesAvailable()) buf.append(s->readAll());
        int idx = buf.indexOf('\n');
        if (idx >= 0) return QJsonDocument::fromJson(buf.left(idx).trimmed()).object();
    }
    return {};
}

static QJsonObject initRpc(QLocalSocket* s) {
    return rpc(s, {{"jsonrpc","2.0"},{"id",1},{"method","initialize"},
        {"params",QJsonObject{{"protocolVersion","2024-11-05"},
                              {"capabilities",QJsonObject{}},
                              {"clientInfo",QJsonObject{{"name","test"}}}}}});
}

static QVector<QJsonObject> drain(QLocalSocket* s, int ms = 300) {
    QVector<QJsonObject> out;
    QByteArray buf;
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        if (s->bytesAvailable()) buf.append(s->readAll());
    }
    while (true) {
        int idx = buf.indexOf('\n');
        if (idx < 0) break;
        auto line = buf.left(idx).trimmed();
        buf.remove(0, idx + 1);
        if (!line.isEmpty()) out.append(QJsonDocument::fromJson(line).object());
    }
    return out;
}

// ── Tests ──

class TestMcp : public QObject {
    Q_OBJECT
    MockMcpServer* m_srv = nullptr;
    static constexpr const char* P = "ReclassMcpTest";
private slots:
    void init()    { m_srv = new MockMcpServer; QVERIFY(m_srv->start(P)); }
    void cleanup() { m_srv->stop(); delete m_srv; m_srv = nullptr; }

    void singleClient_initialize() {
        auto* c = makeClient(P, this); QVERIFY(c);
        auto r = initRpc(c);
        QCOMPARE(r["id"].toInt(), 1);
        QVERIFY(r.contains("result"));
        QCOMPARE(r["result"].toObject()["serverInfo"].toObject()["name"].toString(), QString("mock-mcp"));
        QCOMPARE(m_srv->initializedCount(), 1);
        c->disconnectFromServer(); delete c;
    }

    void singleClient_toolsList() {
        auto* c = makeClient(P, this); QVERIFY(c);
        initRpc(c);
        auto r = rpc(c, {{"jsonrpc","2.0"},{"id",2},{"method","tools/list"}});
        QCOMPARE(r["id"].toInt(), 2);
        QCOMPARE(r["result"].toObject()["tools"].toArray().size(), 1);
        c->disconnectFromServer(); delete c;
    }

    void singleClient_unknownMethod() {
        auto* c = makeClient(P, this); QVERIFY(c);
        auto r = rpc(c, {{"jsonrpc","2.0"},{"id",1},{"method","bogus"}});
        QVERIFY(r.contains("error"));
        QCOMPARE(r["error"].toObject()["code"].toInt(), -32601);
        c->disconnectFromServer(); delete c;
    }

    void multiClient_bothInitialize() {
        auto* c1 = makeClient(P, this); auto* c2 = makeClient(P, this);
        QVERIFY(c1); QVERIFY(c2);
        QCoreApplication::processEvents();
        QCOMPARE(m_srv->clientCount(), 2);
        auto r1 = initRpc(c1); auto r2 = initRpc(c2);
        QVERIFY(r1.contains("result"));
        QVERIFY(r2.contains("result"));
        QCOMPARE(m_srv->initializedCount(), 2);
        c1->disconnectFromServer(); c2->disconnectFromServer(); delete c1; delete c2;
    }

    void multiClient_disconnectOne() {
        auto* c1 = makeClient(P, this); auto* c2 = makeClient(P, this);
        QVERIFY(c1); QVERIFY(c2);
        initRpc(c1); initRpc(c2);
        c1->disconnectFromServer(); QTest::qWait(200);
        QCOMPARE(m_srv->clientCount(), 1);
        auto r = rpc(c2, {{"jsonrpc","2.0"},{"id",5},{"method","tools/list"}});
        QCOMPARE(r["id"].toInt(), 5);
        QVERIFY(r["result"].toObject()["tools"].toArray().size() > 0);
        c2->disconnectFromServer(); delete c1; delete c2;
    }

    void multiClient_notificationBroadcast() {
        auto* c1 = makeClient(P, this);
        auto* c2 = makeClient(P, this);
        auto* c3 = makeClient(P, this);  // not initialized
        QVERIFY(c1); QVERIFY(c2); QVERIFY(c3);
        initRpc(c1); initRpc(c2);

        m_srv->broadcast({{"jsonrpc","2.0"},
            {"method","notifications/resources/updated"},
            {"params",QJsonObject{{"uri","project://tree"}}}});

        auto l1 = drain(c1); auto l2 = drain(c2); auto l3 = drain(c3);
        QVERIFY(l1.size() >= 1);
        QCOMPARE(l1.last()["method"].toString(), QString("notifications/resources/updated"));
        QVERIFY(l2.size() >= 1);
        QCOMPARE(l2.last()["method"].toString(), QString("notifications/resources/updated"));
        QCOMPARE(l3.size(), 0);
        c1->disconnectFromServer(); c2->disconnectFromServer(); c3->disconnectFromServer();
        delete c1; delete c2; delete c3;
    }

    void multiClient_serialRequests() {
        auto* c1 = makeClient(P, this); auto* c2 = makeClient(P, this);
        QVERIFY(c1); QVERIFY(c2);
        initRpc(c1); initRpc(c2);
        auto r1 = rpc(c1, {{"jsonrpc","2.0"},{"id",10},{"method","tools/list"}});
        auto r2 = rpc(c2, {{"jsonrpc","2.0"},{"id",20},{"method","tools/list"}});
        QCOMPARE(r1["id"].toInt(), 10);
        QCOMPARE(r2["id"].toInt(), 20);
        c1->disconnectFromServer(); c2->disconnectFromServer(); delete c1; delete c2;
    }

    void allDisconnect_serverSurvives() {
        auto* c1 = makeClient(P, this); QVERIFY(c1);
        initRpc(c1);
        c1->disconnectFromServer(); QTest::qWait(200);
        QCOMPARE(m_srv->clientCount(), 0);
        auto* c2 = makeClient(P, this); QVERIFY(c2);
        auto r = initRpc(c2);
        QVERIFY(r.contains("result"));
        QCOMPARE(m_srv->clientCount(), 1);
        c2->disconnectFromServer(); delete c1; delete c2;
    }

    void protocol_invalidJson() {
        auto* c = makeClient(P, this); QVERIFY(c);
        c->write("this is not json\n");
        c->flush();
        QByteArray buf;
        QElapsedTimer t; t.start();
        while (t.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (c->bytesAvailable()) buf.append(c->readAll());
            if (buf.indexOf('\n') >= 0) break;
        }
        auto r = QJsonDocument::fromJson(buf.left(buf.indexOf('\n')).trimmed()).object();
        QVERIFY(r.contains("error"));
        QCOMPARE(r["error"].toObject()["code"].toInt(), -32700);
        c->disconnectFromServer(); delete c;
    }

    void protocol_missingMethod() {
        auto* c = makeClient(P, this); QVERIFY(c);
        auto r = rpc(c, {{"jsonrpc","2.0"},{"id",1}});  // no "method" key
        QVERIFY(r.contains("error"));
        QCOMPARE(r["error"].toObject()["code"].toInt(), -32600);
        c->disconnectFromServer(); delete c;
    }

    void protocol_notificationsIgnored() {
        // notifications/initialized and notifications/cancelled should not produce a response
        auto* c = makeClient(P, this); QVERIFY(c);
        initRpc(c);
        c->write(QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}}).toJson(QJsonDocument::Compact) + '\n');
        c->write(QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",QJsonObject{{"requestId",1}}}}).toJson(QJsonDocument::Compact) + '\n');
        c->flush();
        auto lines = drain(c, 500);
        QCOMPARE(lines.size(), 0);  // no response for notifications
        c->disconnectFromServer(); delete c;
    }

    void toolsCall_unknownTool() {
        auto* c = makeClient(P, this); QVERIFY(c);
        initRpc(c);
        auto r = rpc(c, {{"jsonrpc","2.0"},{"id",2},{"method","tools/call"},
            {"params",QJsonObject{{"name","nonexistent.tool"},{"arguments",QJsonObject{}}}}});
        QVERIFY(r.contains("error"));
        QCOMPARE(r["error"].toObject()["code"].toInt(), -32601);
        c->disconnectFromServer(); delete c;
    }

    void toolsCall_missingToolName() {
        auto* c = makeClient(P, this); QVERIFY(c);
        initRpc(c);
        auto r = rpc(c, {{"jsonrpc","2.0"},{"id",3},{"method","tools/call"},
            {"params",QJsonObject{{"arguments",QJsonObject{}}}}});
        QVERIFY(r.contains("error"));
        QCOMPARE(r["error"].toObject()["code"].toInt(), -32602);
        c->disconnectFromServer(); delete c;
    }

    void toolsCall_reconnect() {
        auto* c = makeClient(P, this); QVERIFY(c);
        initRpc(c);
        QCOMPARE(m_srv->clientCount(), 1);

        // Call mcp.reconnect — should get response then get disconnected
        auto r = rpc(c, {{"jsonrpc","2.0"},{"id",7},{"method","tools/call"},
            {"params",QJsonObject{{"name","mcp.reconnect"},{"arguments",QJsonObject{}}}}});
        QCOMPARE(r["id"].toInt(), 7);
        QVERIFY(r.contains("result"));
        QVERIFY(r["result"].toObject()["content"].toArray()[0].toObject()["text"]
                .toString().contains("Disconnected"));

        // Wait for server-side disconnect
        QTest::qWait(300);
        QCOMPARE(m_srv->clientCount(), 0);

        // Reconnect — should work fine
        auto* c2 = makeClient(P, this); QVERIFY(c2);
        auto r2 = initRpc(c2);
        QVERIFY(r2.contains("result"));
        QCOMPARE(m_srv->clientCount(), 1);

        // Verify the new connection works
        auto r3 = rpc(c2, {{"jsonrpc","2.0"},{"id",8},{"method","tools/list"}});
        QCOMPARE(r3["id"].toInt(), 8);
        QVERIFY(r3["result"].toObject()["tools"].toArray().size() > 0);

        c2->disconnectFromServer(); delete c; delete c2;
    }

    void toolsCall_reconnect_otherClientUnaffected() {
        auto* c1 = makeClient(P, this); auto* c2 = makeClient(P, this);
        QVERIFY(c1); QVERIFY(c2);
        initRpc(c1); initRpc(c2);
        QCOMPARE(m_srv->clientCount(), 2);

        // c1 calls reconnect — only c1 should disconnect
        rpc(c1, {{"jsonrpc","2.0"},{"id",1},{"method","tools/call"},
            {"params",QJsonObject{{"name","mcp.reconnect"},{"arguments",QJsonObject{}}}}});
        QTest::qWait(300);
        QCOMPARE(m_srv->clientCount(), 1);

        // c2 still works
        auto r = rpc(c2, {{"jsonrpc","2.0"},{"id",2},{"method","tools/list"}});
        QCOMPARE(r["id"].toInt(), 2);
        QVERIFY(r["result"].toObject()["tools"].toArray().size() > 0);

        c2->disconnectFromServer(); delete c1; delete c2;
    }
};

QTEST_GUILESS_MAIN(TestMcp)
#include "test_mcp.moc"
