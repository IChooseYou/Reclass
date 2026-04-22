#include "mcp_bridge.h"
#include "addressparser.h"
#include "core.h"
#include "controller.h"
#include "generator.h"
#include "mainwindow.h"
#include "scanner.h"
#include "symbolstore.h"
#include "imports/import_pdb.h"
#include "imports/import_source.h"
#include "typeinfer.h"
#include "themes/thememanager.h"
#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTimer>
#include <QDebug>
#include <cstring>
#include <algorithm>

namespace rcx {

static constexpr int kMaxReadBuffer = 10 * 1024 * 1024; // 10 MB

// Parse a number from JSON; accepts string (hex "0x..." or decimal) or number.
// Use for offset, length, pid, limit, tabIndex, etc. to avoid double precision loss
// and to allow clients to send exact values as decimal/hex strings.
static int64_t parseInteger(const QJsonValue& v, int64_t defaultVal = 0) {
    if (v.isUndefined() || v.isNull())
        return defaultVal;
    if (v.isString()) {
        QString s = v.toString().trimmed();
        if (s.isEmpty())
            return defaultVal;
        bool ok;
        qint64 val = s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
            ? s.mid(2).toLongLong(&ok, 16)
            : s.toLongLong(&ok, 10);
        return ok ? val : defaultVal;
    }
    if (v.isDouble())
        return static_cast<int64_t>(v.toDouble());
    return defaultVal;
}

// Format a preview value string for an inference suggestion.
// Used by hex.read (interpret mode) and analysis.infer_types.
static QString inferPreview(const uint8_t* data, int len, const TypeSuggestion& s) {
    if (s.kinds.isEmpty()) return {};
    NodeKind k = s.kinds[0];
    if (s.kinds.size() == 1) {
        switch (k) {
        case NodeKind::Float:     return fmt::fmtFloat(detail::loadF32(data));
        case NodeKind::Double:    return fmt::fmtDouble(detail::loadF64(data));
        case NodeKind::Int32:     return fmt::fmtInt32((int32_t)detail::loadU32(data));
        case NodeKind::UInt32:    return fmt::fmtUInt32(detail::loadU32(data));
        case NodeKind::Int16:     return fmt::fmtInt16((int16_t)detail::loadU16(data));
        case NodeKind::UInt16:    return fmt::fmtUInt16(detail::loadU16(data));
        case NodeKind::Int64:     return fmt::fmtInt64((int64_t)detail::loadU64(data));
        case NodeKind::UInt64:    return fmt::fmtUInt64(detail::loadU64(data));
        case NodeKind::Pointer64: return fmt::fmtPointer64(detail::loadU64(data));
        case NodeKind::Pointer32: return fmt::fmtPointer32(detail::loadU32(data));
        case NodeKind::Bool:      return fmt::fmtBool(data[0]);
        case NodeKind::UTF8: {
            int n = std::min(len, 8);
            QString out;
            for (int i = 0; i < n && data[i] >= 0x20 && data[i] <= 0x7E; ++i)
                out += QLatin1Char(data[i]);
            return out.isEmpty() ? QString() : (QStringLiteral("\"") + out + QStringLiteral("\""));
        }
        default: return {};
        }
    }
    // Split: show each part
    int partSz = len / s.kinds.size();
    QStringList parts;
    for (int i = 0; i < s.kinds.size(); ++i) {
        TypeSuggestion sub;
        sub.kinds = {s.kinds[i]};
        sub.score = s.score;
        sub.strength = s.strength;
        parts << inferPreview(data + i * partSz, partSz, sub);
    }
    return parts.join(QStringLiteral(", "));
}

// ════════════════════════════════════════════════════════════════════
// Construction / lifecycle
// ════════════════════════════════════════════════════════════════════

McpBridge::McpBridge(MainWindow* mainWindow, QObject* parent)
    : QObject(parent), m_mainWindow(mainWindow)
{
    m_notifyTimer = new QTimer(this);
    m_notifyTimer->setSingleShot(true);
    m_notifyTimer->setInterval(100);
    connect(m_notifyTimer, &QTimer::timeout, this, [this]() {
        if (!m_clients.isEmpty())
            sendNotification("notifications/resources/updated",
                             QJsonObject{{"uri", "project://tree"}});
    });
}

McpBridge::~McpBridge() {
    stop();
}

void McpBridge::start() {
    if (m_server) return;

    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::WorldAccessOption);

    // Remove stale socket (Linux/Mac leave files behind)
    QLocalServer::removeServer("ReclassMcpBridge");

    if (!m_server->listen("ReclassMcpBridge")) {
        qWarning() << "[MCP] Failed to start server:" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return;
    }

    connect(m_server, &QLocalServer::newConnection,
            this, &McpBridge::onNewConnection);
    qDebug() << "[MCP] Server listening on pipe: ReclassMcpBridge";
}

void McpBridge::stop() {
    for (auto& c : m_clients) {
        c.socket->disconnect(this);
        c.socket->disconnectFromServer();
        c.socket->deleteLater();
    }
    m_clients.clear();
    m_currentSender = nullptr;
    m_processing = false;
    m_pendingRequests.clear();
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

// ════════════════════════════════════════════════════════════════════
// Connection handling
// ════════════════════════════════════════════════════════════════════

McpBridge::ClientState* McpBridge::findClient(QLocalSocket* sock) {
    for (auto& c : m_clients)
        if (c.socket == sock) return &c;
    return nullptr;
}

void McpBridge::removeClient(QLocalSocket* sock) {
    for (int i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].socket == sock) {
            sock->disconnect(this);
            sock->deleteLater();
            m_clients.removeAt(i);
            return;
        }
    }
}

void McpBridge::onNewConnection() {
    auto* pending = m_server->nextPendingConnection();
    if (!pending) return;

    m_clients.push_back(ClientState{pending, {}, false});

    connect(pending, &QLocalSocket::readyRead,
            this, &McpBridge::onReadyRead);
    connect(pending, &QLocalSocket::disconnected,
            this, &McpBridge::onDisconnected);

    qDebug() << "[MCP] Client connected (" << m_clients.size() << "total)";
}

void McpBridge::onReadyRead() {
    auto* sock = qobject_cast<QLocalSocket*>(sender());
    auto* cs = findClient(sock);
    if (!cs) return;
    cs->readBuffer.append(sock->readAll());

    if (cs->readBuffer.size() > kMaxReadBuffer) {
        qWarning() << "[MCP] Read buffer exceeded 10MB, disconnecting client";
        sock->disconnectFromServer();
        return;
    }

    // Extract complete lines from this client's buffer.
    // If a request is already in flight (m_processing), queue the line
    // instead of processing it -- nested event loops in scanner/tree.apply
    // would otherwise let interleaved requests clobber m_currentSender.
    while (findClient(sock)) {
        cs = findClient(sock);
        int idx = cs->readBuffer.indexOf('\n');
        if (idx < 0) break;
        QByteArray line = cs->readBuffer.left(idx).trimmed();
        cs->readBuffer.remove(0, idx + 1);
        if (line.isEmpty()) continue;

        if (m_processing) {
            m_pendingRequests.push_back(PendingRequest{sock, line});
            continue;
        }
        m_processing = true;
        m_currentSender = sock;
        processLine(line);
        m_currentSender = nullptr;
        m_processing = false;
        drainPendingRequests();
    }
}

void McpBridge::drainPendingRequests() {
    while (!m_pendingRequests.isEmpty()) {
        auto req = m_pendingRequests.takeFirst();
        if (!findClient(req.socket)) continue;  // client disconnected while queued
        m_processing = true;
        m_currentSender = req.socket;
        processLine(req.line);
        m_currentSender = nullptr;
        m_processing = false;
    }
}

void McpBridge::onDisconnected() {
    auto* sock = qobject_cast<QLocalSocket*>(sender());
    qDebug() << "[MCP] Client disconnected (" << m_clients.size() - 1 << "remaining)";
    // Purge any queued requests from this client
    m_pendingRequests.erase(
        std::remove_if(m_pendingRequests.begin(), m_pendingRequests.end(),
            [sock](const PendingRequest& r) { return r.socket == sock; }),
        m_pendingRequests.end());
    removeClient(sock);
}

// ════════════════════════════════════════════════════════════════════
// JSON-RPC plumbing
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::okReply(const QJsonValue& id, const QJsonObject& result) {
    return QJsonObject{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };
}

QJsonObject McpBridge::errReply(const QJsonValue& id, int code, const QString& msg) {
    return QJsonObject{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", QJsonObject{{"code", code}, {"message", msg}}}
    };
}

void McpBridge::sendJson(const QJsonObject& obj) {
    QLocalSocket* target = m_currentSender;
    if (!target || !findClient(target)) return;
    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    qDebug() << "[MCP] >>" << data.left(200);
    data.append('\n');
    target->write(data);
    target->flush();
}

void McpBridge::sendNotification(const QString& method, const QJsonObject& params) {
    QJsonObject n{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.isEmpty()) n["params"] = params;
    QByteArray data = QJsonDocument(n).toJson(QJsonDocument::Compact);
    data.append('\n');
    for (auto& c : m_clients) {
        if (c.initialized) {
            c.socket->write(data);
            c.socket->flush();
        }
    }
}

QJsonObject McpBridge::makeTextResult(const QString& text, bool isError) {
    QJsonObject entry;
    entry["type"] = QStringLiteral("text");
    entry["text"] = text;
    QJsonArray content;
    content.append(entry);
    QJsonObject result;
    result["content"] = content;
    if (isError) result["isError"] = true;
    return result;
}

// ════════════════════════════════════════════════════════════════════
// Dispatch
// ════════════════════════════════════════════════════════════════════

void McpBridge::processLine(const QByteArray& line) {
  try {
    qDebug() << "[MCP] <<" << line.trimmed().left(200);
    auto doc = QJsonDocument::fromJson(line);
    if (!doc.isObject()) {
        sendJson(errReply(QJsonValue(), -32700, "Parse error"));
        return;
    }

    QJsonObject req = doc.object();
    QJsonValue id = req.value("id");
    QString method = req.value("method").toString();

    // Client notifications (no response)
    if (method == "notifications/initialized" ||
        method == "notifications/cancelled") {
        return;
    }

    if (method == "initialize") {
        m_mainWindow->setMcpStatus(QStringLiteral("MCP: client connected"));
        sendJson(handleInitialize(id, req.value("params").toObject()));
        m_mainWindow->clearMcpStatus();
    } else if (method == "tools/list") {
        m_mainWindow->setMcpStatus(QStringLiteral("MCP: tools/list"));
        sendJson(handleToolsList(id));
        m_mainWindow->clearMcpStatus();
    } else if (method == "tools/call") {
        sendJson(handleToolsCall(id, req.value("params").toObject()));
    } else {
        sendJson(errReply(id, -32601, "Method not found: " + method));
    }
  } catch (const std::exception& e) {
    qWarning() << "[MCP] Exception:" << e.what();
    sendJson(errReply(QJsonValue(), -32603,
        QStringLiteral("Internal error: %1").arg(e.what())));
  } catch (...) {
    qWarning() << "[MCP] Unknown exception";
    sendJson(errReply(QJsonValue(), -32603, "Internal error"));
  }
}

// ════════════════════════════════════════════════════════════════════
// MCP: initialize
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::handleInitialize(const QJsonValue& id, const QJsonObject&) {
    if (auto* cs = findClient(m_currentSender)) cs->initialized = true;

    QJsonObject caps;
    caps["tools"] = QJsonObject{{"listChanged", false}};

    QJsonObject result{
        {"protocolVersion", "2024-11-05"},
        {"capabilities", caps},
        {"serverInfo", QJsonObject{
            {"name", "reclass-mcp"},
            {"version", "1.0.0"}
        }},
        {"instructions",
            "You are connected to ReClass, a live memory structure editor for reverse engineering. "
            "You have two types of data available:\n"
            "1. STRUCTURE: The node tree defines typed fields (project.state, tree.search, tree.apply). "
            "Each node has a kind (the data type: UInt32, Float, Hex64, etc.) and a name.\n"
            "2. LIVE DATA: The provider reads real memory from an attached process (hex.read, hex.write). "
            "node.history returns timestamped value changes with heat levels (0=static, 1=cold, 2=warm, 3=hot).\n\n"
            "CRITICAL RULES:\n"
            "- When labeling/identifying a field, ALWAYS change BOTH name AND kind in one tree.apply call. "
            "Example: [{op:'rename',nodeId:'X',name:'health'},{op:'change_kind',nodeId:'X',kind:'Int32'}]. "
            "A node named 'health' with kind Hex64 is WRONG — the kind must match the actual data type.\n"
            "- To detect what changed after an in-game event: call ui.action with action:'reset_tracking', "
            "then have the user perform the action, then call node.history on the relevant nodes "
            "to see which ones have new timestamped entries.\n"
            "- hex.read offset is an absolute virtual address by default. "
            "Use baseRelative=true to make it relative to the struct base address (0 = start of struct).\n"
            "- tree.apply operations are atomic (undo macro). Batch related changes into one call.\n"
            "- Use tree.search to quickly find nodes by name instead of paging through project.state.\n"
            "- project.state returns structure metadata only (kinds, names, offsets), NOT live values. "
            "Use hex.read for actual memory values and node.history for tracking changes over time."
        }
    };
    return okReply(id, result);
}

// ════════════════════════════════════════════════════════════════════
// MCP: tools/list
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::handleToolsList(const QJsonValue& id) {
    QJsonArray tools;

    // 1. project.state
    tools.append(QJsonObject{
        {"name", "project.state"},
        {"description", "Returns project state with paginated node tree. "
                        "NOTE: This returns structure metadata only (kinds, names, offsets), NOT live memory values. "
                        "Use hex.read to read actual values and node.history to track value changes over time. "
                        "Responses return max 'limit' nodes (default 50). "
                        "Use depth:1 first, then parentId to drill into a struct. "
                        "Enum/bitfield member arrays are omitted by default (counts shown instead); "
                        "pass includeMembers:true to get full arrays. "
                        "Response includes returned/total/nextOffset for paging."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"depth", QJsonObject{{"type", "integer"},
                    {"description", "Max tree depth to return (default 1)."}}},
                {"parentId", QJsonObject{{"type", "string"},
                    {"description", "Only return children of this node."}}},
                {"includeTree", QJsonObject{{"type", "boolean"},
                    {"description", "If false, return only provider/source info, no tree. Default true."}}},
                {"includeMembers", QJsonObject{{"type", "boolean"},
                    {"description", "If true, include full enumMembers/bitfieldMembers arrays. Default false (shows counts only)."}}},
                {"limit", QJsonObject{{"type", "integer"},
                    {"description", "Max nodes to return (default 50, max 500)."}}},
                {"offset", QJsonObject{{"type", "integer"},
                    {"description", "Skip this many nodes (for pagination). Use nextOffset from previous response."}}}
            }}
        }}
    });

    // 2. tree.apply
    tools.append(QJsonObject{
        {"name", "tree.apply"},
        {"description", "Apply batch of tree operations atomically (undo macro). "
                        "IMPORTANT: When identifying/labeling a field, you MUST use BOTH rename AND change_kind "
                        "in the same batch. A renamed node still has its original kind (e.g. Hex64) unless you "
                        "explicitly change it. Example: "
                        "[{op:'rename',nodeId:'ID',name:'health'},{op:'change_kind',nodeId:'ID',kind:'Int32'}]. "
                        "Each op is a JSON object with an 'op' field for the operation type and 'nodeId' (string) for the target node. "
                        "Operations: "
                        "remove: {op:'remove', nodeId:'ID'}. "
                        "rename: {op:'rename', nodeId:'ID', name:'newName'}. "
                        "insert: {op:'insert', kind:'Hex64', name:'field', parentId:'ID', offset:0} — "
                        "optional fields: structTypeName, classKeyword, strLen, elementKind, arrayLen, refId, "
                        "ptrDepth (0=struct ptr, 1=prim*, 2=prim**), isStatic (bool), offsetExpr (string), "
                        "isRelative (bool, RVA pointer), "
                        "enumMembers ([{name:'X',value:0},...]), bitfieldMembers ([{name:'X',bitOffset:0,bitWidth:1},...]). "
                        "change_kind: {op:'change_kind', nodeId:'ID', kind:'UInt32'}. "
                        "change_offset: {op:'change_offset', nodeId:'ID', offset:16}. "
                        "change_base: {op:'change_base', baseAddress:'0x400000', formula:'[0x233CA80]'} — formula is optional, enables auto-resolve on provider attach. "
                        "change_struct_type: {op:'change_struct_type', nodeId:'ID', structTypeName:'Name'}. "
                        "change_class_keyword: {op:'change_class_keyword', nodeId:'ID', classKeyword:'class'}. "
                        "change_pointer_ref: {op:'change_pointer_ref', nodeId:'ID', refId:'targetID'}. "
                        "change_array_meta: {op:'change_array_meta', nodeId:'ID', elementKind:'UInt32', arrayLen:10}. "
                        "collapse: {op:'collapse', nodeId:'ID', collapsed:true}. "
                        "change_enum_members: {op:'change_enum_members', nodeId:'ID', members:[{name:'X',value:0},...]}. "
                        "change_offset_expr: {op:'change_offset_expr', nodeId:'ID', offsetExpr:'base + 0x10'}. "
                        "toggle_static: {op:'toggle_static', nodeId:'ID', isStatic:true}. "
                        "toggle_relative: {op:'toggle_relative', nodeId:'ID', isRelative:true}. "
                        "group_into_union: {op:'group_into_union', nodeIds:['ID1','ID2',...]} — groups siblings into a union. "
                        "dissolve_union: {op:'dissolve_union', nodeId:'ID'} — flattens a union back to parent scope. "
                        "Insert ops get auto-assigned IDs; use $0, $1 etc. to reference them in later ops. "
                        "Kinds: Hex8 Hex16 Hex32 Hex64 Int8 Int16 Int32 Int64 UInt8 UInt16 UInt32 UInt64 "
                        "Float Double Bool Pointer32 Pointer64 FuncPtr32 FuncPtr64 Vec2 Vec3 Vec4 Mat4x4 UTF8 UTF16 Struct Array"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"operations", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "object"}}}}},
                {"macroName", QJsonObject{{"type", "string"}}}
            }},
            {"required", QJsonArray{"operations"}}
        }}
    });

    // 3. source.switch
    tools.append(QJsonObject{
        {"name", "source.switch"},
        {"description", "Switch active data source (provider). Use sourceIndex for saved sources, "
                        "filePath to load a binary file, or pid to attach to a live process."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"sourceIndex", QJsonObject{{"type", "integer"}}},
                {"filePath", QJsonObject{{"type", "string"}}},
                {"pid", QJsonObject{{"type", "integer"},
                    {"description", "Process ID to attach to for live memory reading."}}},
                {"processName", QJsonObject{{"type", "string"},
                    {"description", "Display name for the process (optional with pid)."}}},
                {"allViews", QJsonObject{{"type", "boolean"}}}
            }}
        }}
    });

    // 3b. source.modules
    tools.append(QJsonObject{
        {"name", "source.modules"},
        {"description", "List modules for the current data source. Returns name, base (hex), and size for each module. "
                        "Only available when the provider reports module info (e.g. after attaching to a process). "
                        "Use these names in baseAddressFormula for tree base, e.g. '<Module.exe> + 0x1000'."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}}
            }}
        }}
    });

    // 4. hex.read
    tools.append(QJsonObject{
        {"name", "hex.read"},
        {"description", "Read raw bytes from provider (live process memory). Returns hex dump, ASCII, and multi-type "
                        "interpretations (u8/u16/u32/u64/i32/f32/f64/ptr/string heuristics). "
                        "Use this to see what actual values are in memory at any offset. "
                        "By default offset is an absolute virtual address in the target process. "
                        "Set baseRelative=true to make offset relative to the struct base address "
                        "(e.g. offset=0 reads at baseAddress, offset=0x10 reads at baseAddress+0x10)."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"offset", QJsonObject{{"type", "integer"},
                    {"description", "Address to read from. Absolute VA by default, or relative to struct base if baseRelative=true."}}},
                {"length", QJsonObject{{"type", "integer"},
                    {"description", "Number of bytes to read (1-4096, default 64)."}}},
                {"baseRelative", QJsonObject{{"type", "boolean"},
                    {"description", "If true, offset is relative to the tree's base address (added automatically). Default false (offset is absolute VA)."}}},
                {"interpret", QJsonObject{{"type", "boolean"},
                    {"description", "If true, append per-field type inference results (8-byte aligned chunks analyzed by the inference engine). "
                                    "Returns scored suggestions like [float] score=80, [ptr64] score=75 for each chunk."}}}
            }},
            {"required", QJsonArray{"offset", "length"}}
        }}
    });

    // 5. hex.write
    tools.append(QJsonObject{
        {"name", "hex.write"},
        {"description", "Write raw bytes to provider (through undo stack). Hex string format: '4D5A9000'. "
                        "By default offset is an absolute virtual address. "
                        "Set baseRelative=true to make offset relative to the struct base address."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"offset", QJsonObject{{"type", "integer"},
                    {"description", "Address to write to. Absolute VA by default, or relative to struct base if baseRelative=true."}}},
                {"hexBytes", QJsonObject{{"type", "string"},
                    {"description", "Hex byte string to write, e.g. '4D5A9000'. Spaces allowed."}}},
                {"baseRelative", QJsonObject{{"type", "boolean"},
                    {"description", "If true, offset is relative to the tree's base address. Default false (absolute VA)."}}}
            }},
            {"required", QJsonArray{"offset", "hexBytes"}}
        }}
    });

    // bookmarks.list / add / remove
    tools.append(QJsonObject{
        {"name", "bookmarks.list"},
        {"description", "List bookmarks for the active project. Returns array of {name, addressFormula, address}."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"}}}
            }}
        }}
    });
    tools.append(QJsonObject{
        {"name", "bookmarks.add"},
        {"description", "Add a bookmark to the active project. addressFormula is an AddressParser expression "
                        "such as '<game.exe>+0x12340' or '0x7ff7e0001234'. Survives base-address rebases."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"}}},
                {"name", QJsonObject{{"type", "string"}}},
                {"addressFormula", QJsonObject{{"type", "string"}}}
            }},
            {"required", QJsonArray{"name", "addressFormula"}}
        }}
    });
    tools.append(QJsonObject{
        {"name", "bookmarks.remove"},
        {"description", "Remove a bookmark by name (first match) or by 0-based index."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"}}},
                {"name", QJsonObject{{"type", "string"}}},
                {"index", QJsonObject{{"type", "integer"}}}
            }}
        }}
    });

    // 6. status.set
    tools.append(QJsonObject{
        {"name", "status.set"},
        {"description", "Show status text to user. Updates command row (editor line 0) and/or "
                        "the window status bar."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"text", QJsonObject{{"type", "string"}}},
                {"target", QJsonObject{{"type", "string"},
                    {"enum", QJsonArray{"commandRow", "statusBar", "both"}}}}
            }},
            {"required", QJsonArray{"text"}}
        }}
    });

    // 7. ui.action
    tools.append(QJsonObject{
        {"name", "ui.action"},
        {"description", "Trigger a UI action. Fallback for operations without dedicated tools. "
                        "Actions: undo, redo, new_file, open_file, save_file, save_file_as, "
                        "export_cpp, set_view_root, scroll_to_node, collapse_node, expand_node, "
                        "select_node, refresh, reset_tracking. "
                        "export_cpp accepts optional nodeId to export a single struct (recommended for large projects). "
                        "reset_tracking clears all value change histories — use before an in-game event, "
                        "then check node.history afterward to see what changed."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"action", QJsonObject{{"type", "string"}}},
                {"nodeId", QJsonObject{{"type", "string"}}},
                {"filePath", QJsonObject{{"type", "string"}}}
            }},
            {"required", QJsonArray{"action"}}
        }}
    });

    // 8. tree.search
    tools.append(QJsonObject{
        {"name", "tree.search"},
        {"description", "Search for nodes by name (substring, case-insensitive). "
                        "Returns compact results: id, name, kind, parentId, offset, childCount. "
                        "Use kindFilter to narrow (e.g. 'Struct'). Max 100 results. "
                        "Much faster than paging through project.state to find a specific type."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"query", QJsonObject{{"type", "string"},
                    {"description", "Name substring to search for (case-insensitive)."}}},
                {"kindFilter", QJsonObject{{"type", "string"},
                    {"description", "Filter by node kind (e.g. 'Struct', 'Hex64', 'Array')."}}},
                {"limit", QJsonObject{{"type", "integer"},
                    {"description", "Max results to return (default 20, max 100)."}}}
            }}
        }}
    });

    // 9. node.history
    tools.append(QJsonObject{
        {"name", "node.history"},
        {"description", "Returns timestamped value change history (up to 10 entries) for specified nodes. "
                        "Use this to detect what changed after an in-game event — no need to manually snapshot memory. "
                        "Each node returns: entries[] with {value, timestamp}, heatLevel (0=static to 3=hot), "
                        "and uniqueCount. Heat level 3 means the field is actively changing. "
                        "Requires live provider with value tracking enabled."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"nodeIds", QJsonObject{{"type", "array"},
                    {"items", QJsonObject{{"type", "string"}}},
                    {"description", "Array of node IDs to get history for."}}},
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index. Omit for active tab."}}}
            }},
            {"required", QJsonArray{"nodeIds"}}
        }}
    });

    // 10. scanner.scan
    tools.append(QJsonObject{
        {"name", "scanner.scan"},
        {"description", "Run a value scan on the active tab's provider and wait for completion. "
                        "Use after source.switch (e.g. attach to process). Value type: int8, int16, int32, int64, "
                        "uint8, uint16, uint32, uint64, float, double. Results appear in the Scanner panel. "
                        "For value scans (e.g. float 120) prefer scanning readable/writable (data) regions, not executable: "
                        "set filterWritable: true and filterExecutable: false. "
                        "Use 'regions' to restrict scan to specific address ranges (intersected with provider regions)."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"valueType", QJsonObject{{"type", "string"},
                    {"description", "Value type: float, double, int32, uint32, int64, uint64, int16, uint16, int8, uint8."}}},
                {"value", QJsonObject{{"type", "string"},
                    {"description", "Value to search for (e.g. \"120\" for float 120)."}}},
                {"filterExecutable", QJsonObject{{"type", "boolean"},
                    {"description", "Only scan executable regions (default false). For value scans use false; use writable instead."}}},
                {"filterWritable", QJsonObject{{"type", "boolean"},
                    {"description", "Only scan writable regions (default false). Recommended true for value scans to hit data/heap, not code."}}},
                {"regions", QJsonObject{{"type", "array"},
                    {"description", "Restrict scan to these address ranges. Each element is [startHex, endHex], e.g. [[\"0x10000\",\"0x20000\"],[\"0x50000\",\"0x60000\"]]. Ranges are intersected with the provider's real memory regions."},
                    {"items", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}}}}}
            }},
            {"required", QJsonArray{"valueType", "value"}}
        }}
    });

    // 10. scanner.scan_pattern
    tools.append(QJsonObject{
        {"name", "scanner.scan_pattern"},
        {"description", "Run a pattern/signature scan on the active tab's provider and wait for completion. "
                        "Pattern is space-separated hex bytes, e.g. '00 00 20 42 00 00 20 42'. Use ?? for wildcards. "
                        "Results appear in the Scanner panel. Uses the same region list as value scans. "
                        "Use 'regions' to restrict scan to specific address ranges (intersected with provider regions)."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"pattern", QJsonObject{{"type", "string"},
                    {"description", "Hex pattern, e.g. '00 00 20 42 00 00 20 42 00 00 00 00 00 00 00 00'. Use ?? for wildcard bytes."}}},
                {"filterExecutable", QJsonObject{{"type", "boolean"},
                    {"description", "Only scan executable regions (default false)."}}},
                {"filterWritable", QJsonObject{{"type", "boolean"},
                    {"description", "Only scan writable regions (default false)."}}},
                {"regions", QJsonObject{{"type", "array"},
                    {"description", "Restrict scan to these address ranges. Each element is [startHex, endHex], e.g. [[\"0x10000\",\"0x20000\"],[\"0x50000\",\"0x60000\"]]. Ranges are intersected with the provider's real memory regions."},
                    {"items", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}}}}}
            }},
            {"required", QJsonArray{"pattern"}}
        }}
    });

    // 11. mcp.reconnect
    tools.append(QJsonObject{
        {"name", "mcp.reconnect"},
        {"description", "Disconnect the current MCP client so it can reconnect to Reclass (e.g. after Reclass was restarted or to reset connection state). "
                        "The client process will exit; your IDE may restart it automatically, reconnecting to Reclass like at startup."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{}}
        }}
    });


    // process.info
    tools.append(QJsonObject{
        {"name", "process.info"},
        {"description", "Returns PEB address and enumerates all Thread Environment Blocks (TEBs) for the attached process. "
                        "TEBs are discovered via NtQuerySystemInformation and NtQueryInformationThread. "
                        "Each TEB entry includes: address, threadId. "
                        "Requires a live process provider with PEB support."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}}
            }}
        }}
    });

    // symbols.load
    tools.append(QJsonObject{
        {"name", "symbols.load"},
        {"description", "Load PDB symbols from a file path into the global symbol store. "
                        "Symbols are used for address annotations (e.g. 'ntdll!RtlInitUnicodeString') "
                        "and can be resolved via symbols.lookup. "
                        "Returns the number of symbols loaded and the module name."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"pdbPath", QJsonObject{{"type", "string"},
                    {"description", "Absolute path to a .pdb file."}}},
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab. Used to refresh annotations after loading."}}}
            }},
            {"required", QJsonArray{"pdbPath"}}
        }}
    });

    // symbols.lookup
    tools.append(QJsonObject{
        {"name", "symbols.lookup"},
        {"description", "Resolve a symbol name to an absolute virtual address in the attached process. "
                        "Supports qualified names like 'ntdll!RtlInitUnicodeString' and bare names. "
                        "Requires symbols to be loaded (via symbols.load or the UI) and a live provider."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"symbol", QJsonObject{{"type", "string"},
                    {"description", "Symbol to resolve. Use 'module!name' for qualified lookup, or bare 'name' for unqualified."}}},
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}}
            }},
            {"required", QJsonArray{"symbol"}}
        }}
    });

    // symbols.importType
    tools.append(QJsonObject{
        {"name", "symbols.importType"},
        {"description", "Import the type definition for a global symbol from its PDB into the active project. "
                        "Given a qualified symbol like 'ntdll!g_pShimEngineModule', resolves its typeIndex from "
                        "the PDB, follows pointer/modifier chains to find the underlying struct/class/union/enum, "
                        "and imports it with full recursive child types. "
                        "Requires symbols to be loaded first (via symbols.load). "
                        "Returns the imported type name and node count, or an error if the symbol has no type info."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"symbol", QJsonObject{{"type", "string"},
                    {"description", "Qualified symbol name (e.g. 'ntdll!g_pShimEngineModule'). Must include 'module!' prefix."}}},
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}}
            }},
            {"required", QJsonArray{"symbol"}}
        }}
    });

    // node.read_value
    tools.append(QJsonObject{
        {"name", "node.read_value"},
        {"description", "Read the formatted typed value for one or more nodes. Unlike hex.read (which returns raw bytes), "
                        "this returns the value as the user sees it in the editor: e.g. '120.0f' for Float, '0x7FF61234' for Pointer64, "
                        "'true' for Bool, '1.0, 2.0, 3.0' for Vec3. "
                        "For Hex nodes returns the hex byte preview. For Struct/Array returns the computed size. "
                        "Requires a live provider for meaningful values."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"nodeIds", QJsonObject{{"type", "array"},
                    {"items", QJsonObject{{"type", "string"}}},
                    {"description", "Array of node IDs to read values for."}}},
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}}
            }},
            {"required", QJsonArray{"nodeIds"}}
        }}
    });

    // analysis.infer_types
    tools.append(QJsonObject{
        {"name", "analysis.infer_types"},
        {"description", "Run the type inference engine on hex nodes to get scored type suggestions. "
                        "Returns top candidates (e.g. float, ptr64, int32_t×2) with confidence scores (0-100) and "
                        "strength levels (1=weak, 2=moderate, 3=strong). Uses value change history for better accuracy "
                        "when available. Much more accurate than manually interpreting hex bytes."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"nodeIds", QJsonObject{{"type", "array"},
                    {"items", QJsonObject{{"type", "string"}}},
                    {"description", "Array of node IDs to analyze (should be Hex8/16/32/64 nodes)."}}},
                {"useHistory", QJsonObject{{"type", "boolean"},
                    {"description", "Feed value change history into inference for better accuracy (default true)."}}},
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}}
            }},
            {"required", QJsonArray{"nodeIds"}}
        }}
    });

    // analysis.import_header
    tools.append(QJsonObject{
        {"name", "analysis.import_header"},
        {"description", "Import C/C++ struct definitions from source code into the active project. "
                        "Accepts standard C/C++ struct/class/union/enum syntax with optional offset comments (// 0xNN). "
                        "This is far more efficient than building structs field-by-field via tree.apply. "
                        "Supports Windows types (DWORD, HANDLE, etc.), stdint types, pointers, arrays, bitfields, and nested structs."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"sourceCode", QJsonObject{{"type", "string"},
                    {"description", "C/C++ source code containing struct/class/union/enum definitions."}}},
                {"pointerSize", QJsonObject{{"type", "integer"},
                    {"description", "Pointer size: 4 for 32-bit, 8 for 64-bit (default 8)."}}},
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}}
            }},
            {"required", QJsonArray{"sourceCode"}}
        }}
    });

    // analysis.pointer_chain
    tools.append(QJsonObject{
        {"name", "analysis.pointer_chain"},
        {"description", "Follow a chain of pointers from a starting address, returning hex dump + type inference + "
                        "vtable detection + symbol annotations at each level. Stops on null, unreadable, or maxDepth. "
                        "Use this to explore what a pointer points to without multiple sequential hex.read calls."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"address", QJsonObject{{"type", "string"},
                    {"description", "Starting address (hex string, e.g. '0x7FF618570000'). "
                                    "Or use baseRelative:true with an offset from struct base."}}},
                {"baseRelative", QJsonObject{{"type", "boolean"},
                    {"description", "If true, address is relative to struct base address."}}},
                {"maxDepth", QJsonObject{{"type", "integer"},
                    {"description", "Maximum pointer levels to follow (default 3, max 8)."}}},
                {"readLength", QJsonObject{{"type", "integer"},
                    {"description", "Bytes to read and analyze at each level (default 64, max 512)."}}},
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}}
            }},
            {"required", QJsonArray{"address"}}
        }}
    });

    // ui.inspect
    tools.append(QJsonObject{
        {"name", "ui.inspect"},
        {"description", "Query the UI region the user selected via Ctrl+Shift+Click. "
                        "Returns widget type, region name, theme colors that control it, and generic properties "
                        "(fontSize, width, height, etc.). The user Ctrl+Shift+Clicks on any part of the window to select it, "
                        "then you call this to see what they selected."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"region", QJsonObject{{"type", "string"},
                    {"description", "Query a named region directly (e.g. 'editor.typeColumn') without needing Ctrl+Click."}}},
                {"clear", QJsonObject{{"type", "boolean"},
                    {"description", "Clear the current selection and hide overlay."}}}
            }}
        }}
    });

    // theme.get
    tools.append(QJsonObject{
        {"name", "theme.get"},
        {"description", "Return all theme colors (30 fields). Each has key, current hex value, label, and group."},
        {"inputSchema", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}
    });

    // theme.set
    tools.append(QJsonObject{
        {"name", "theme.set"},
        {"description", "Change one or more theme colors with instant live preview. Non-destructive — use theme.save to persist "
                        "or theme.revert to undo. Returns old values for each changed key."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"colors", QJsonObject{{"type", "object"},
                    {"description", "Map of theme field key → new hex color value. "
                                    "E.g. {\"textDim\": \"#999999\", \"hover\": \"#2A2A2A\"}. "
                                    "Use theme.get or ui.inspect to discover valid keys."}}}
            }},
            {"required", QJsonArray{"colors"}}
        }}
    });

    // theme.save
    tools.append(QJsonObject{
        {"name", "theme.save"},
        {"description", "Persist the current previewed theme changes (from theme.set). Saves to disk."},
        {"inputSchema", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}
    });

    // theme.revert
    tools.append(QJsonObject{
        {"name", "theme.revert"},
        {"description", "Revert theme to state before any theme.set calls. Undoes all unsaved preview changes."},
        {"inputSchema", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}
    });

    return okReply(id, QJsonObject{{"tools", tools}});
}

// ════════════════════════════════════════════════════════════════════
// MCP: tools/call — dispatch to tool implementations
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::handleToolsCall(const QJsonValue& id, const QJsonObject& params) {
    QString toolName = params.value("name").toString();
    QJsonObject args = params.value("arguments").toObject();

    // Show tool activity in status bar (with shimmer)
    m_mainWindow->setMcpStatus(QStringLiteral("MCP: %1").arg(toolName));
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    QJsonObject result;
    if      (toolName == "project.state")  result = toolProjectState(args);
    else if (toolName == "tree.apply")     result = toolTreeApply(args);
    else if (toolName == "source.switch")  result = toolSourceSwitch(args);
    else if (toolName == "source.modules") result = toolSourceModules(args);
    else if (toolName == "hex.read")       result = toolHexRead(args);
    else if (toolName == "hex.write")      result = toolHexWrite(args);
    else if (toolName == "status.set")     result = toolStatusSet(args);
    else if (toolName == "ui.action")      result = toolUiAction(args);
    else if (toolName == "tree.search")   result = toolTreeSearch(args);
    else if (toolName == "node.history")  result = toolNodeHistory(args);
    else if (toolName == "scanner.scan")  result = toolScannerScan(args);
    else if (toolName == "scanner.scan_pattern") result = toolScannerScanPattern(args);
    else if (toolName == "mcp.reconnect") result = toolReconnect(args);
    else if (toolName == "process.info") result = toolProcessInfo(args);
    else if (toolName == "symbols.load") result = toolSymbolsLoad(args);
    else if (toolName == "symbols.lookup") result = toolSymbolsLookup(args);
    else if (toolName == "symbols.importType") result = toolSymbolsImportType(args);
    else if (toolName == "node.read_value") result = toolNodeReadValue(args);
    else if (toolName == "analysis.infer_types") result = toolAnalysisInferTypes(args);
    else if (toolName == "analysis.import_header") result = toolAnalysisImportHeader(args);
    else if (toolName == "analysis.pointer_chain") result = toolAnalysisPointerChain(args);
    else if (toolName == "ui.inspect")    result = toolUiInspect(args);
    else if (toolName == "theme.get")     result = toolThemeGet(args);
    else if (toolName == "theme.set")     result = toolThemeSet(args);
    else if (toolName == "theme.save")    result = toolThemeSave(args);
    else if (toolName == "theme.revert")  result = toolThemeRevert(args);
    else if (toolName == "bookmarks.list")   result = toolBookmarksList(args);
    else if (toolName == "bookmarks.add")    result = toolBookmarksAdd(args);
    else if (toolName == "bookmarks.remove") result = toolBookmarksRemove(args);
    else return errReply(id, -32601, "Unknown tool: " + toolName);

    // Presentation mode: brief focus glow for single-node tools (not tree.apply, which handles its own)
    if (m_mainWindow->presentationMode() && toolName != "tree.apply") {
        QString nodeIdStr = args.value("nodeId").toString();
        if (!nodeIdStr.isEmpty()) {
            uint64_t nid = nodeIdStr.toULongLong();
            if (nid != 0) {
                auto* ctrl = m_mainWindow->activeController();
                if (ctrl) {
                    for (auto* editor : ctrl->editors()) {
                        editor->setFocusNode(nid);
                        editor->smoothScrollToNodeId(nid);
                    }
                    QEventLoop loop;
                    QTimer::singleShot(150, &loop, &QEventLoop::quit);
                    loop.exec(QEventLoop::ExcludeUserInputEvents);
                    for (auto* editor : ctrl->editors())
                        editor->clearFocusNode();
                }
            }
        }
    }

    m_mainWindow->clearMcpStatus();

    return okReply(id, result);
}

// ════════════════════════════════════════════════════════════════════
// Helper: resolve "$N" placeholder references
// ════════════════════════════════════════════════════════════════════

QString McpBridge::resolvePlaceholder(const QString& ref,
                                       const QHash<QString, uint64_t>& placeholderMap,
                                       bool* ok) {
    if (ok) *ok = true;
    if (ref.startsWith('$')) {
        auto it = placeholderMap.find(ref);
        if (it != placeholderMap.end())
            return QString::number(it.value());
        if (ok) *ok = false;
        return ref;  // unresolved placeholder
    }
    return ref;  // not a placeholder — return as-is
}

// ════════════════════════════════════════════════════════════════════
// Smart tab resolution
// ════════════════════════════════════════════════════════════════════

MainWindow::TabState* McpBridge::resolveTab(const QJsonObject& args, int* resolvedIndex) {
    if (resolvedIndex) *resolvedIndex = -1;

    // 1) Explicit tab index from args
    if (args.contains("tabIndex")) {
        int idx = (int)parseInteger(args.value("tabIndex"));
        auto* t = m_mainWindow->tabByIndex(idx);
        if (t) { if (resolvedIndex) *resolvedIndex = idx; return t; }
    }

    // 2) Active sub-window (user clicked on it)
    auto* t = m_mainWindow->activeTab();
    if (t) {
        if (resolvedIndex) {
            for (int i = 0; i < m_mainWindow->tabCount(); i++) {
                if (m_mainWindow->tabByIndex(i) == t) { *resolvedIndex = i; break; }
            }
        }
        return t;
    }

    // 3) Fall back to first available tab
    if (m_mainWindow->tabCount() > 0) {
        t = m_mainWindow->tabByIndex(0);
        if (t) { if (resolvedIndex) *resolvedIndex = 0; return t; }
    }

    // 4) No tabs at all — auto-create a project
    m_mainWindow->project_new();
    if (resolvedIndex) *resolvedIndex = 0;
    return m_mainWindow->tabByIndex(0);
}

// ════════════════════════════════════════════════════════════════════
// TOOL: project.state
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolProjectState(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    auto* doc = tab->doc;
    auto* ctrl = tab->ctrl;
    const auto& tree = doc->tree;

    int maxDepth = (int)parseInteger(args.value("depth"), 1);
    bool includeTree = args.contains("includeTree") ? args.value("includeTree").toBool() : true;
    bool includeMembers = args.value("includeMembers").toBool(false);
    int limit = qBound(1, (int)parseInteger(args.value("limit"), 50), 500);
    int offset = qMax(0, (int)parseInteger(args.value("offset"), 0));
    QString parentIdStr = args.value("parentId").toString();
    uint64_t filterParentId = parentIdStr.isEmpty() ? 0 : parentIdStr.toULongLong();

    QJsonObject state;
    state["baseAddress"] = "0x" + QString::number(tree.baseAddress, 16).toUpper();
    if (!tree.baseAddressFormula.isEmpty())
        state["baseAddressFormula"] = tree.baseAddressFormula;
    state["viewRootId"] = QString::number(ctrl->viewRootId());
    state["nodeCount"] = tree.nodes.size();

    // Provider info
    QJsonObject provInfo;
    if (doc->provider) {
        provInfo["name"] = doc->provider->name();
        provInfo["writable"] = doc->provider->isWritable();
        provInfo["live"] = doc->provider->isLive();
        provInfo["size"] = doc->provider->size();
        provInfo["kind"] = doc->provider->kind();
    }
    state["provider"] = provInfo;

    // Saved sources
    QJsonArray srcs;
    const auto& savedSources = ctrl->savedSources();
    int activeIdx = ctrl->activeSourceIndex();
    for (int i = 0; i < savedSources.size(); i++) {
        const auto& s = savedSources[i];
        srcs.append(QJsonObject{
            {"index", i},
            {"kind", s.kind},
            {"displayName", s.displayName},
            {"active", i == activeIdx}
        });
    }
    state["sources"] = srcs;

    // Selection
    QJsonArray selArr;
    for (uint64_t sid : ctrl->selectedIds())
        selArr.append(QString::number(sid));
    state["selectedNodeIds"] = selArr;

    // Document info
    state["filePath"] = doc->filePath;
    state["modified"] = doc->modified;
    state["undoAvailable"] = doc->undoStack.canUndo();
    state["redoAvailable"] = doc->undoStack.canRedo();
    state["statusText"] = m_mainWindow->m_appStatus;

    // Filtered tree: only emit nodes up to maxDepth from the filter root
    if (includeTree) {
        // Build parent→children map once
        QHash<uint64_t, QVector<int>> childMap;
        for (int i = 0; i < tree.nodes.size(); i++)
            childMap[tree.nodes[i].parentId].append(i);

        // BFS from filterParentId, respecting maxDepth + pagination
        QJsonArray nodeArr;
        struct QueueEntry { uint64_t parentId; int depth; };
        QVector<QueueEntry> queue;
        queue.push_back(QueueEntry{filterParentId, 0});

        int totalCount = 0;  // total nodes that match depth filter
        int emitted = 0;

        while (!queue.isEmpty()) {
            auto entry = queue.takeFirst();
            if (entry.depth > maxDepth) continue;

            const auto& kids = childMap.value(entry.parentId);
            for (int ci : kids) {
                const Node& n = tree.nodes[ci];

                // Count all matching nodes for pagination metadata
                totalCount++;

                // Apply offset/limit pagination
                if (totalCount <= offset) {
                    // Still skipping — but enqueue children for counting
                    if (entry.depth + 1 <= maxDepth)
                        queue.push_back(QueueEntry{n.id, entry.depth + 1});
                    continue;
                }
                if (emitted >= limit) {
                    // Past limit — just keep counting total
                    if (entry.depth + 1 <= maxDepth)
                        queue.push_back(QueueEntry{n.id, entry.depth + 1});
                    continue;
                }

                QJsonObject nj = n.toJson();

                // Strip inline member arrays unless requested
                if (!includeMembers) {
                    if (nj.contains("enumMembers")) {
                        int count = nj.value("enumMembers").toArray().size();
                        nj.remove("enumMembers");
                        nj["enumMemberCount"] = count;
                    }
                    if (nj.contains("bitfieldMembers")) {
                        int count = nj.value("bitfieldMembers").toArray().size();
                        nj.remove("bitfieldMembers");
                        nj["bitfieldMemberCount"] = count;
                    }
                }

                // Add computed size for containers
                if (n.kind == NodeKind::Struct || n.kind == NodeKind::Array) {
                    nj["computedSize"] = tree.structSpan(n.id, &childMap);
                    nj["childCount"] = childMap.value(n.id).size();
                }
                nodeArr.append(nj);
                emitted++;

                // Enqueue children if we haven't hit depth limit
                if (entry.depth + 1 <= maxDepth)
                    queue.push_back(QueueEntry{n.id, entry.depth + 1});
            }
        }

        QJsonObject treeObj;
        treeObj["baseAddress"] = QString::number(tree.baseAddress, 16);
        if (!tree.baseAddressFormula.isEmpty())
            treeObj["baseAddressFormula"] = tree.baseAddressFormula;
        treeObj["nextId"] = QString::number(tree.m_nextId);
        treeObj["nodes"] = nodeArr;
        treeObj["returned"] = emitted;
        treeObj["total"] = totalCount;
        if (emitted < totalCount)
            treeObj["nextOffset"] = offset + emitted;
        state["tree"] = treeObj;
    }

    return makeTextResult(QString::fromUtf8(
        QJsonDocument(state).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: tree.apply
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolTreeApply(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    auto* doc = tab->doc;
    auto* ctrl = tab->ctrl;
    auto& tree = doc->tree;

    QJsonArray ops = args.value("operations").toArray();
    QString macroName = args.value("macroName").toString("MCP batch");

    if (ops.isEmpty())
        return makeTextResult("No operations provided", true);

    // Phase 1: Pre-scan inserts and reserve IDs
    QHash<QString, uint64_t> placeholders;  // "$0" → reserved ID
    for (int i = 0; i < ops.size(); i++) {
        QJsonObject op = ops[i].toObject();
        if (op.value("op").toString() == "insert") {
            uint64_t newId = tree.reserveId();
            placeholders[QStringLiteral("$%1").arg(i)] = newId;
        }
    }

    // Phase 2: Execute in undo macro
    if (!m_slowMode)
        ctrl->setSuppressRefresh(true);
    doc->undoStack.beginMacro(macroName);

    int applied = 0;
    uint64_t lastRootStructId = 0;  // track root-level struct inserts
    QStringList skippedOps;
    for (int i = 0; i < ops.size(); i++) {
        // Safety valve: keep paint events flowing for large batches
        if (i % 100 == 0 && ops.size() > 200) {
            m_mainWindow->setMcpStatus(
                QStringLiteral("MCP: tree.apply %1/%2").arg(i).arg(ops.size()));
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 5);
        }

        QJsonObject op = ops[i].toObject();
        QString opType = op.value("op").toString();

        if (opType == "insert") {
            Node n;
            n.id = placeholders.value(QStringLiteral("$%1").arg(i), tree.reserveId());
            n.kind = kindFromString(op.value("kind").toString("Hex64"));
            n.name = op.value("name").toString();
            bool pidOk;
            QString pid = resolvePlaceholder(op.value("parentId").toString("0"), placeholders, &pidOk);
            if (!pidOk) {
                skippedOps.append(QStringLiteral("op[%1]: unresolved placeholder for parentId").arg(i));
                continue;
            }
            n.parentId = pid.toULongLong();
            if (n.parentId != 0 && tree.indexOfId(n.parentId) < 0) {
                skippedOps.append(QStringLiteral("op[%1]: parentId '%2' not found").arg(i).arg(pid));
                continue;
            }
            n.offset = (int)parseInteger(op.value("offset"), 0);
            n.structTypeName = op.value("structTypeName").toString();
            n.classKeyword = op.value("classKeyword").toString();
            n.strLen = qBound(1, (int)parseInteger(op.value("strLen"), 64), 1000000);
            n.elementKind = kindFromString(op.value("elementKind").toString("UInt8"));
            n.arrayLen = qBound(1, (int)parseInteger(op.value("arrayLen"), 1), kMaxArrayLen);
            n.ptrDepth = qBound(0, (int)parseInteger(op.value("ptrDepth"), 0), 2);
            n.isStatic = op.value("isStatic").toBool(false);
            n.offsetExpr = op.value("offsetExpr").toString();
            n.isRelative = op.value("isRelative").toBool(false);
            // Enum members
            if (op.contains("enumMembers")) {
                QJsonArray emArr = op.value("enumMembers").toArray();
                for (const auto& ev : emArr) {
                    QJsonObject eo = ev.toObject();
                    n.enumMembers.emplaceBack(eo.value("name").toString(),
                                              (int64_t)parseInteger(eo.value("value")));
                }
            }
            // Bitfield members
            if (op.contains("bitfieldMembers")) {
                QJsonArray bmArr = op.value("bitfieldMembers").toArray();
                for (const auto& bv : bmArr) {
                    QJsonObject bo = bv.toObject();
                    BitfieldMember bm;
                    bm.name = bo.value("name").toString();
                    bm.bitOffset = (uint8_t)qBound(0, (int)parseInteger(bo.value("bitOffset")), 255);
                    bm.bitWidth = (uint8_t)qBound(1, (int)parseInteger(bo.value("bitWidth"), 1), 64);
                    n.bitfieldMembers.append(bm);
                }
            }
            bool refOk;
            QString refStr = resolvePlaceholder(op.value("refId").toString("0"), placeholders, &refOk);
            if (!refOk) {
                skippedOps.append(QStringLiteral("op[%1]: unresolved placeholder for refId").arg(i));
                continue;
            }
            n.refId = refStr.toULongLong();

            // Auto-place: offset -1 means "after last sibling"
            if (n.offset < 0) {
                int maxEnd = 0;
                auto siblings = tree.childrenOf(n.parentId);
                for (int si : siblings) {
                    auto& sn = tree.nodes[si];
                    int sz = (sn.kind == NodeKind::Struct || sn.kind == NodeKind::Array)
                        ? tree.structSpan(sn.id) : sn.byteSize();
                    int end = sn.offset + sz;
                    if (end > maxEnd) maxEnd = end;
                }
                int align = alignmentFor(n.kind);
                n.offset = (maxEnd + align - 1) / align * align;
            }

            doc->undoStack.push(new RcxCommand(ctrl, cmd::Insert{n, {}}));
            if (n.parentId == 0 && n.kind == NodeKind::Struct)
                lastRootStructId = n.id;
            applied++;
        }
        else if (opType == "remove") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                const Node& node = tree.nodes[idx];
                QVector<int> indices = tree.subtreeIndices(node.id);
                QVector<Node> subtree;
                for (int si : indices) subtree.append(tree.nodes[si]);
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::Remove{node.id, subtree, {}}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: remove nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "rename") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::Rename{tree.nodes[idx].id, tree.nodes[idx].name,
                                op.value("name").toString()}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: rename nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "change_kind") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                NodeKind newKind = kindFromString(op.value("kind").toString());
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::ChangeKind{tree.nodes[idx].id, tree.nodes[idx].kind, newKind, {}}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: change_kind nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "change_offset") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                int newOff = (int)parseInteger(op.value("offset"));
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::ChangeOffset{tree.nodes[idx].id, tree.nodes[idx].offset, newOff}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: change_offset nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "change_base") {
            uint64_t newBase = op.value("baseAddress").toString().toULongLong(nullptr, 16);
            QString oldFormula = tree.baseAddressFormula;
            QString newFormula = op.value("formula").toString();
            doc->undoStack.push(new RcxCommand(ctrl,
                cmd::ChangeBase{tree.baseAddress, newBase, oldFormula, newFormula}));
            applied++;
        }
        else if (opType == "change_struct_type") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::ChangeStructTypeName{tree.nodes[idx].id,
                        tree.nodes[idx].structTypeName,
                        op.value("structTypeName").toString()}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: change_struct_type nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "change_class_keyword") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::ChangeClassKeyword{tree.nodes[idx].id,
                        tree.nodes[idx].classKeyword,
                        op.value("classKeyword").toString()}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: change_class_keyword nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "change_pointer_ref") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            QString refStr = resolvePlaceholder(op.value("refId").toString("0"), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::ChangePointerRef{tree.nodes[idx].id,
                        tree.nodes[idx].refId, refStr.toULongLong()}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: change_pointer_ref nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "change_array_meta") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                NodeKind newElemKind = kindFromString(op.value("elementKind").toString());
                int newLen = qBound(1, (int)parseInteger(op.value("arrayLen"), 1), kMaxArrayLen);
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::ChangeArrayMeta{tree.nodes[idx].id,
                        tree.nodes[idx].elementKind, newElemKind,
                        tree.nodes[idx].arrayLen, newLen}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: change_array_meta nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "collapse") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                bool newState = op.value("collapsed").toBool();
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::Collapse{tree.nodes[idx].id, tree.nodes[idx].collapsed, newState}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: collapse nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "change_enum_members") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                QVector<QPair<QString, int64_t>> newMembers;
                QJsonArray membersArr = op.value("members").toArray();
                for (const auto& mv : membersArr) {
                    QJsonObject mo = mv.toObject();
                    newMembers.emplaceBack(mo.value("name").toString(),
                                           (int64_t)parseInteger(mo.value("value")));
                }
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::ChangeEnumMembers{tree.nodes[idx].id,
                        tree.nodes[idx].enumMembers, newMembers}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: change_enum_members nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "change_offset_expr") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::ChangeOffsetExpr{tree.nodes[idx].id,
                        tree.nodes[idx].offsetExpr,
                        op.value("offsetExpr").toString()}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: change_offset_expr nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "toggle_static") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                bool newVal = op.value("isStatic").toBool();
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::ToggleStatic{tree.nodes[idx].id,
                        tree.nodes[idx].isStatic, newVal}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: toggle_static nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "toggle_relative") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            int idx = tree.indexOfId(nid.toULongLong());
            if (idx >= 0) {
                bool newVal = op.value("isRelative").toBool();
                doc->undoStack.push(new RcxCommand(ctrl,
                    cmd::ToggleRelative{tree.nodes[idx].id,
                        tree.nodes[idx].isRelative, newVal}));
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: toggle_relative nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else if (opType == "group_into_union") {
            QJsonArray idsArr = op.value("nodeIds").toArray();
            QSet<uint64_t> ids;
            for (const auto& v : idsArr) {
                QString resolved = resolvePlaceholder(v.toString(), placeholders);
                ids.insert(resolved.toULongLong());
            }
            if (ids.size() >= 2) {
                ctrl->groupIntoUnion(ids);
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: group_into_union needs >= 2 nodeIds").arg(i));
            }
        }
        else if (opType == "dissolve_union") {
            QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders);
            uint64_t unionId = nid.toULongLong();
            int idx = tree.indexOfId(unionId);
            if (idx >= 0) {
                ctrl->dissolveUnion(unionId);
                applied++;
            } else {
                skippedOps.append(QStringLiteral("op[%1]: dissolve_union nodeId '%2' not found").arg(i).arg(nid));
            }
        }
        else {
            skippedOps.append(QStringLiteral("op[%1]: unknown op '%2'").arg(i).arg(opType));
        }

        // Slow mode / presentation mode: refresh after each operation for visual feedback
        if (m_slowMode && applied > 0) {
            bool presentation = m_mainWindow->presentationMode();

            // Un-suppress temporarily so refresh() actually runs
            ctrl->setSuppressRefresh(false);
            ctrl->refresh();

            if (presentation) {
                // Extract target node ID for glow + scroll
                uint64_t targetNodeId = 0;
                if (opType == "insert") {
                    targetNodeId = placeholders.value(QStringLiteral("$%1").arg(i), 0);
                    if (targetNodeId == 0) {
                        // Look up the node we just inserted by scanning recent additions
                        QString nid = op.value("nodeId").toString();
                        if (!nid.isEmpty()) targetNodeId = nid.toULongLong();
                    }
                } else {
                    bool nidOk;
                    QString nid = resolvePlaceholder(op.value("nodeId").toString(), placeholders, &nidOk);
                    if (nidOk) targetNodeId = nid.toULongLong();
                }

                // Extract next op's target to skip animation for paired ops on same node
                bool skipAnim = false;
                if (i + 1 < ops.size()) {
                    QJsonObject nextOp = ops[i + 1].toObject();
                    QString nextNodeId = nextOp.value("nodeId").toString();
                    QString curNodeId = op.value("nodeId").toString();
                    if (!curNodeId.isEmpty() && curNodeId == nextNodeId)
                        skipAnim = true;
                }

                if (!skipAnim && targetNodeId != 0) {
                    // Apply focus glow + smooth scroll to affected node
                    for (auto* editor : ctrl->editors()) {
                        editor->setFocusNode(targetNodeId);
                        editor->smoothScrollToNodeId(targetNodeId);
                    }

                    // Update status with progress
                    m_mainWindow->setMcpStatus(
                        QStringLiteral("MCP: %1 [%2/%3]").arg(opType).arg(i + 1).arg(ops.size()));

                    // Pause to let animation play (125ms per visible op)
                    QEventLoop loop;
                    QTimer::singleShot(125, &loop, &QEventLoop::quit);
                    loop.exec(QEventLoop::ExcludeUserInputEvents);

                    // Clear focus glow
                    for (auto* editor : ctrl->editors())
                        editor->clearFocusNode();
                } // else: skipped (paired op on same node) — no delay
            } else {
                // Basic slow mode: brief pause for visual feedback
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 16);
            }

            // Re-suppress for next operation
            ctrl->setSuppressRefresh(true);
        }
    }

    doc->undoStack.endMacro();
    if (!m_slowMode)
        ctrl->setSuppressRefresh(false);

    // Auto-switch view to newly created root struct
    if (lastRootStructId)
        ctrl->setViewRootId(lastRootStructId);

    ctrl->refresh();

    // Build response with assigned placeholder IDs
    QJsonObject assignedIds;
    for (auto it = placeholders.begin(); it != placeholders.end(); ++it)
        assignedIds[it.key()] = QString::number(it.value());

    QString msg = QStringLiteral("Applied %1 operations").arg(applied);
    if (!skippedOps.isEmpty())
        msg += QStringLiteral("\nSkipped %1:\n").arg(skippedOps.size()) + skippedOps.join('\n');

    QJsonObject result = makeTextResult(msg, !skippedOps.isEmpty() && applied == 0);
    result["assignedIds"] = assignedIds;
    return result;
}

// ════════════════════════════════════════════════════════════════════
// TOOL: source.switch
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolSourceSwitch(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    auto* ctrl = tab->ctrl;
    auto* doc = tab->doc;

    if (args.contains("sourceIndex")) {
        int idx = (int)parseInteger(args.value("sourceIndex"));
        const auto& sources = ctrl->savedSources();
        if (idx < 0 || idx >= sources.size())
            return makeTextResult("Source index out of range: " + QString::number(idx), true);

        if (args.value("allViews").toBool()) {
            // Switch all tabs to this source
            for (auto& t : m_mainWindow->m_tabs)
                t.ctrl->switchSource(idx);
        } else {
            ctrl->switchSource(idx);
        }
        return makeTextResult("Switched to source " + QString::number(idx) +
                              " (" + sources[idx].displayName + ")");
    }

    if (args.contains("pid")) {
        uint32_t pid = (uint32_t)parseInteger(args.value("pid"));
        QString name = args.value("processName").toString();
        if (name.isEmpty()) name = QString("PID %1").arg(pid);
        QString target = QString("%1:%2").arg(pid).arg(name);
        ctrl->attachViaPlugin(QStringLiteral("processmemory"), target);
        // attachViaPlugin does not set tree.baseAddress; set it from the new provider (like selectSource does).
        if (doc->provider && doc->provider->base() != 0) {
            doc->tree.baseAddress = doc->provider->base();
            doc->tree.baseAddressFormula.clear();
            ctrl->refresh();
        }
        return makeTextResult("Attached to process " + name + " (PID " + QString::number(pid) + ")");
    }

    if (args.contains("filePath")) {
        QString path = args.value("filePath").toString();
        doc->loadData(path);
        ctrl->refresh();
        return makeTextResult("Loaded file: " + path);
    }

    return makeTextResult("Provide sourceIndex, filePath, or pid", true);
}

// ════════════════════════════════════════════════════════════════════
// TOOL: source.modules
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolSourceModules(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    auto* prov = tab->doc->provider.get();
    if (!prov) return makeTextResult("No data source attached", true);

    QVector<MemoryRegion> regions = prov->enumerateRegions();
    // Build unique modules: name -> { minBase, maxEnd }
    QHash<QString, QPair<uint64_t, uint64_t>> moduleMap;
    for (const auto& r : regions) {
        if (r.moduleName.isEmpty()) continue;
        uint64_t end = r.base + r.size;
        auto it = moduleMap.find(r.moduleName);
        if (it == moduleMap.end()) {
            moduleMap[r.moduleName] = qMakePair(r.base, end);
        } else {
            it->first = qMin(it->first, r.base);
            it->second = qMax(it->second, end);
        }
    }

    QJsonArray arr;
    QStringList names = moduleMap.keys();
    std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) {
        return QString::compare(a, b, Qt::CaseInsensitive) < 0;
    });
    for (const QString& name : names) {
        const auto& p = moduleMap[name];
        uint64_t base = p.first;
        uint64_t size = p.second - p.first;
        arr.append(QJsonObject{
            {"name", name},
            {"base", "0x" + QString::number(base, 16).toUpper()},
            {"size", QJsonValue(static_cast<qint64>(size))}
        });
    }

    QJsonObject out;
    out["modules"] = arr;
    out["count"] = arr.size();
    return makeTextResult(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: hex.read
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolHexRead(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    auto* prov = tab->doc->provider.get();
    if (!prov) return makeTextResult("No provider", true);

    int64_t offset = parseInteger(args.value("offset"));
    int length = qBound(1, (int)parseInteger(args.value("length"), 64), 4096);
    bool baseRel = args.value("baseRelative").toBool();

    if (baseRel)
        offset += (int64_t)tab->doc->tree.baseAddress;

    if (offset < 0 || !prov->isReadable((uint64_t)offset, length))
        return makeTextResult("Cannot read at offset " + QString::number(offset), true);

    QByteArray data = prov->readBytes((uint64_t)offset, length);

    // Format hex dump (16 bytes per line)
    QString dump;
    for (int i = 0; i < data.size(); i += 16) {
        int lineLen = qMin(16, data.size() - i);
        dump += QString("%1: ").arg((uint64_t)(offset + i), 8, 16, QChar('0'));
        for (int j = 0; j < 16; j++) {
            if (j < lineLen)
                dump += QString("%1 ").arg((uint8_t)data[i+j], 2, 16, QChar('0'));
            else
                dump += "   ";
            if (j == 7) dump += " ";
        }
        dump += " |";
        for (int j = 0; j < lineLen; j++) {
            uint8_t c = (uint8_t)data[i+j];
            dump += (c >= 0x20 && c <= 0x7e) ? QChar(c) : QChar('.');
        }
        dump += "|\n";
    }

    // Type interpretations at start of read
    if (data.size() >= 1) {
        dump += "\n--- Interpretations at offset ---\n";
        dump += "u8:  " + QString::number((uint8_t)data[0]) + "\n";
        if (data.size() >= 2) {
            uint16_t v; memcpy(&v, data.data(), 2);
            dump += "u16: " + QString::number(v) + "\n";
        }
        if (data.size() >= 4) {
            uint32_t v; memcpy(&v, data.data(), 4);
            int32_t iv; memcpy(&iv, data.data(), 4);
            float fv; memcpy(&fv, data.data(), 4);
            dump += "u32: " + QString::number(v) + " (0x" + QString::number(v, 16) + ")\n";
            dump += "i32: " + QString::number(iv) + "\n";
            dump += "f32: " + QString::number((double)fv) + "\n";
        }
        if (data.size() >= 8) {
            uint64_t v; memcpy(&v, data.data(), 8);
            double dv; memcpy(&dv, data.data(), 8);
            dump += "u64: " + QString::number(v) + " (0x" + QString::number(v, 16) + ")\n";
            dump += "f64: " + QString::number(dv) + "\n";

            // Pointer-likeness
            uint64_t base = tab->doc->tree.baseAddress;
            int provSize = prov->size();
            if (v >= base && v < base + (uint64_t)provSize)
                dump += "ptr?: LIKELY (within provider range)\n";
        }
        // String-likeness
        int printable = 0;
        for (int i = 0; i < data.size() && (uint8_t)data[i] >= 0x20 && (uint8_t)data[i] <= 0x7e; i++)
            printable++;
        if (printable >= 4)
            dump += "str?: " + QString::number(printable) + " printable ASCII bytes\n";
    }

    // Per-field type inference (when interpret flag is set)
    if (args.value("interpret").toBool() && data.size() >= 8) {
        int ptrSize = tab->doc->tree.pointerSize;
        int chunkSize = (ptrSize >= 8) ? 8 : 4;
        dump += QStringLiteral("\n--- Per-field inference (%1-byte aligned) ---\n").arg(chunkSize);
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(data.constData());
        for (int off = 0; off + chunkSize <= data.size(); off += chunkSize) {
            InferHints hints;
            hints.ptrSize = ptrSize;
            auto suggestions = inferTypes(raw + off, chunkSize, hints);
            dump += QStringLiteral("+0x%1: ").arg(off, 2, 16, QChar('0'));
            if (suggestions.isEmpty()) {
                dump += QStringLiteral("(zero / unknown)\n");
            } else {
                const auto& top = suggestions[0];
                QString label = formatHint(top);
                QString preview = inferPreview(raw + off, chunkSize, top);
                dump += QStringLiteral("[%1] score=%2").arg(label).arg(top.score);
                if (!preview.isEmpty())
                    dump += QStringLiteral("  %1").arg(preview);
                dump += QStringLiteral("\n");
            }
        }
    }

    return makeTextResult(dump);
}

// ════════════════════════════════════════════════════════════════════
// TOOL: hex.write
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolHexWrite(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    auto* ctrl = tab->ctrl;
    auto* doc = tab->doc;
    auto* prov = doc->provider.get();

    int64_t offset = parseInteger(args.value("offset"));
    QString hexStr = args.value("hexBytes").toString().remove(' ');

    if (args.value("baseRelative").toBool())
        offset += (int64_t)doc->tree.baseAddress;

    if (hexStr.size() % 2 != 0)
        return makeTextResult("Hex string must have even length", true);

    QByteArray newBytes;
    for (int i = 0; i < hexStr.size(); i += 2) {
        bool ok;
        uint8_t byte = hexStr.mid(i, 2).toUInt(&ok, 16);
        if (!ok) return makeTextResult("Invalid hex at position " + QString::number(i), true);
        newBytes.append((char)byte);
    }

    if (!prov || !prov->isWritable())
        return makeTextResult("Provider is not writable", true);
    if (!prov->isReadable((uint64_t)offset, newBytes.size()))
        return makeTextResult("Offset out of range", true);

    QByteArray oldBytes = prov->readBytes((uint64_t)offset, newBytes.size());
    doc->undoStack.push(new RcxCommand(ctrl,
        cmd::WriteBytes{(uint64_t)offset, oldBytes, newBytes}));

    return makeTextResult("Wrote " + QString::number(newBytes.size()) + " bytes at offset 0x"
                          + QString::number(offset, 16));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: status.set
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolStatusSet(const QJsonObject& args) {
    QString text = args.value("text").toString();
    QString target = args.value("target").toString("both");

    auto* tab = resolveTab(args);

    if (target == "commandRow" || target == "both") {
        if (tab) {
            for (auto& pane : tab->panes) {
                if (pane.editor) {
                    pane.editor->setCommandRowText(
                        QStringLiteral("[\xE2\x96\xB8] [Claude: %1]").arg(text));
                }
            }
        }
    }
    if (target == "statusBar" || target == "both") {
        m_mainWindow->setAppStatus(text);
    }

    return makeTextResult("Status set: " + text);
}

// ════════════════════════════════════════════════════════════════════
// TOOL: ui.action
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolUiAction(const QJsonObject& args) {
    QString action = args.value("action").toString();
    QString nodeIdStr = args.value("nodeId").toString();

    auto* tab = resolveTab(args);
    auto* doc = tab ? tab->doc : nullptr;
    auto* ctrl = tab ? tab->ctrl : nullptr;

    if (action == "undo") {
        if (!doc) return makeTextResult("No active tab", true);
        if (!doc->undoStack.canUndo()) return makeTextResult("Nothing to undo", true);
        doc->undoStack.undo();
        return makeTextResult("Undo performed");
    }
    if (action == "redo") {
        if (!doc) return makeTextResult("No active tab", true);
        if (!doc->undoStack.canRedo()) return makeTextResult("Nothing to redo", true);
        doc->undoStack.redo();
        return makeTextResult("Redo performed");
    }
    if (action == "refresh") {
        if (!ctrl) return makeTextResult("No active tab", true);
        ctrl->refresh();
        return makeTextResult("Refreshed");
    }
    if (action == "set_view_root") {
        if (!ctrl) return makeTextResult("No active tab", true);
        ctrl->setViewRootId(nodeIdStr.toULongLong());
        return makeTextResult("View root set to " + nodeIdStr);
    }
    if (action == "scroll_to_node") {
        if (!ctrl) return makeTextResult("No active tab", true);
        ctrl->scrollToNodeId(nodeIdStr.toULongLong());
        return makeTextResult("Scrolled to node " + nodeIdStr);
    }
    if (action == "export_cpp") {
        if (!doc) return makeTextResult("No active tab", true);
        const QHash<NodeKind, QString>* aliases = doc->typeAliases.isEmpty() ? nullptr : &doc->typeAliases;
        bool asserts = QSettings("Reclass", "Reclass").value("generatorAsserts", false).toBool();
        QString code;
        if (!nodeIdStr.isEmpty()) {
            // Per-struct export
            uint64_t nid = nodeIdStr.toULongLong();
            code = renderCpp(doc->tree, nid, aliases, asserts);
            if (code.isEmpty())
                return makeTextResult("Node not found or not a struct: " + nodeIdStr, true);
        } else {
            code = renderCppAll(doc->tree, aliases, asserts);
        }
        // Truncate if too large (64 KB limit)
        if (code.size() > 65536) {
            int totalSize = code.size();
            code.truncate(65536);
            code += QStringLiteral("\n\n... truncated (%1 bytes total, showing first 64KB)"
                                   "\nUse nodeId param to export a single struct.")
                    .arg(totalSize);
        }
        return makeTextResult(code);
    }
    if (action == "save_file") {
        m_mainWindow->project_save();
        return makeTextResult("Saved");
    }
    if (action == "new_file") {
        m_mainWindow->project_new();
        return makeTextResult("New project created");
    }
    if (action == "open_file") {
        QString path = args.value("filePath").toString();
        if (path.isEmpty())
            return makeTextResult("filePath required for open_file", true);
        m_mainWindow->project_open(path);
        return makeTextResult("Opened: " + path);
    }
    if (action == "collapse_node") {
        if (!ctrl || !doc) return makeTextResult("No active tab", true);
        int idx = doc->tree.indexOfId(nodeIdStr.toULongLong());
        if (idx < 0) return makeTextResult("Node not found: " + nodeIdStr, true);
        doc->undoStack.push(new RcxCommand(ctrl,
            cmd::Collapse{doc->tree.nodes[idx].id, doc->tree.nodes[idx].collapsed, true}));
        ctrl->refresh();
        return makeTextResult("Collapsed " + nodeIdStr);
    }
    if (action == "expand_node") {
        if (!ctrl || !doc) return makeTextResult("No active tab", true);
        int idx = doc->tree.indexOfId(nodeIdStr.toULongLong());
        if (idx < 0) return makeTextResult("Node not found: " + nodeIdStr, true);
        doc->undoStack.push(new RcxCommand(ctrl,
            cmd::Collapse{doc->tree.nodes[idx].id, doc->tree.nodes[idx].collapsed, false}));
        ctrl->refresh();
        return makeTextResult("Expanded " + nodeIdStr);
    }
    if (action == "select_node") {
        if (!ctrl) return makeTextResult("No active tab", true);
        uint64_t nid = nodeIdStr.toULongLong();
        ctrl->clearSelection();
        auto* editor = ctrl->primaryEditor();
        if (editor)
            ctrl->handleNodeClick(editor, -1, nid, Qt::NoModifier);
        return makeTextResult("Selected node " + nodeIdStr);
    }

    if (action == "reset_tracking") {
        int count = m_mainWindow->tabCount();
        for (int i = 0; i < count; ++i) {
            auto* t = m_mainWindow->tabByIndex(i);
            if (t && t->ctrl)
                t->ctrl->resetChangeTracking();
        }
        return makeTextResult(QStringLiteral("Value tracking reset on all %1 tabs.").arg(count));
    }

    return makeTextResult("Unknown action: " + action, true);
}

// ════════════════════════════════════════════════════════════════════
// TOOL: tree.search
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolTreeSearch(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    const auto& tree = tab->doc->tree;
    QString query = args.value("query").toString();
    QString kindFilter = args.value("kindFilter").toString();
    int limit = qBound(1, (int)parseInteger(args.value("limit"), 20), 100);

    if (query.isEmpty() && kindFilter.isEmpty())
        return makeTextResult("Provide 'query' (name substring) and/or 'kindFilter' (e.g. 'Struct')", true);

    // Build parent→children map for childCount
    QHash<uint64_t, int> childCounts;
    for (const auto& n : tree.nodes)
        childCounts[n.parentId]++;

    QJsonArray results;
    for (const auto& n : tree.nodes) {
        // Kind filter
        if (!kindFilter.isEmpty()) {
            if (kindToString(n.kind) != kindFilter) continue;
        }
        // Name substring match (case-insensitive)
        if (!query.isEmpty()) {
            bool nameMatch = n.name.contains(query, Qt::CaseInsensitive);
            bool typeMatch = n.structTypeName.contains(query, Qt::CaseInsensitive);
            if (!nameMatch && !typeMatch) continue;
        }

        QJsonObject nj;
        nj["id"] = QString::number(n.id);
        nj["name"] = n.name;
        nj["kind"] = kindToString(n.kind);
        nj["parentId"] = QString::number(n.parentId);
        nj["offset"] = n.offset;
        if (!n.structTypeName.isEmpty())
            nj["structTypeName"] = n.structTypeName;
        if (!n.classKeyword.isEmpty())
            nj["classKeyword"] = n.classKeyword;
        if (n.kind == NodeKind::Struct || n.kind == NodeKind::Array)
            nj["childCount"] = childCounts.value(n.id, 0);
        if (!n.enumMembers.isEmpty())
            nj["enumMemberCount"] = n.enumMembers.size();
        if (!n.bitfieldMembers.isEmpty())
            nj["bitfieldMemberCount"] = n.bitfieldMembers.size();
        results.append(nj);

        if (results.size() >= limit) break;
    }

    QJsonObject out;
    out["results"] = results;
    out["count"] = results.size();
    out["query"] = query;
    if (!kindFilter.isEmpty()) out["kindFilter"] = kindFilter;
    return makeTextResult(QString::fromUtf8(
        QJsonDocument(out).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// Tool: node.history — return timestamped value history for nodes
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolNodeHistory(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab.", true);

    const auto& histMap = tab->ctrl->valueHistory();
    QJsonArray requestedIds = args.value("nodeIds").toArray();
    if (requestedIds.isEmpty())
        return makeTextResult("nodeIds array is required.", true);

    QJsonObject result;
    for (const auto& idVal : requestedIds) {
        QString idStr = idVal.toString();
        uint64_t nodeId = idStr.toULongLong();
        auto it = histMap.find(nodeId);
        QJsonArray entries;
        if (it != histMap.end()) {
            it->forEachWithTime([&](const QString& val, qint64 msec) {
                QJsonObject entry;
                entry.insert(QStringLiteral("value"), val);
                entry.insert(QStringLiteral("timestamp"), msec);
                entries.append(entry);
            });
        }
        QJsonObject nodeResult;
        nodeResult.insert(QStringLiteral("entries"), entries);
        nodeResult.insert(QStringLiteral("heatLevel"), it != histMap.end() ? it->heatLevel() : 0);
        nodeResult.insert(QStringLiteral("uniqueCount"), it != histMap.end() ? it->uniqueCount() : 0);
        result.insert(idStr, nodeResult);
    }
    return makeTextResult(QString::fromUtf8(
        QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// TOOL: scanner.scan
// ════════════════════════════════════════════════════════════════════

static ValueType valueTypeFromString(const QString& s) {
    QString lower = s.trimmed().toLower();
    if (lower == QStringLiteral("int8"))   return ValueType::Int8;
    if (lower == QStringLiteral("int16"))  return ValueType::Int16;
    if (lower == QStringLiteral("int32"))  return ValueType::Int32;
    if (lower == QStringLiteral("int64"))  return ValueType::Int64;
    if (lower == QStringLiteral("uint8"))  return ValueType::UInt8;
    if (lower == QStringLiteral("uint16")) return ValueType::UInt16;
    if (lower == QStringLiteral("uint32")) return ValueType::UInt32;
    if (lower == QStringLiteral("uint64")) return ValueType::UInt64;
    if (lower == QStringLiteral("float"))  return ValueType::Float;
    if (lower == QStringLiteral("double")) return ValueType::Double;
    return ValueType::Float; // default
}

static QVector<AddressRange> parseRegionsArg(const QJsonObject& args, QString* errOut = nullptr) {
    QVector<AddressRange> out;
    QJsonArray arr = args.value("regions").toArray();
    if (arr.isEmpty()) return out;
    out.reserve(arr.size());
    for (int i = 0; i < arr.size(); i++) {
        QJsonArray pair = arr[i].toArray();
        if (pair.size() != 2) {
            if (errOut) *errOut = QStringLiteral("regions[%1]: expected [startHex, endHex]").arg(i);
            return {};
        }
        bool ok1 = false, ok2 = false;
        uint64_t start = pair[0].toString().toULongLong(&ok1, 0);
        uint64_t end   = pair[1].toString().toULongLong(&ok2, 0);
        if (!ok1 || !ok2) {
            if (errOut) *errOut = QStringLiteral("regions[%1]: invalid hex address").arg(i);
            return {};
        }
        if (end <= start) {
            if (errOut) *errOut = QStringLiteral("regions[%1]: end must be > start").arg(i);
            return {};
        }
        out.push_back(AddressRange{start, end});
    }
    return out;
}

QJsonObject McpBridge::toolScannerScan(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    ScannerPanel* panel = m_mainWindow->m_scannerPanel;
    if (!panel) return makeTextResult("Scanner panel not available", true);

    QString valueTypeStr = args.value("valueType").toString();
    QString value = args.value("value").toString();
    bool filterExec = args.value("filterExecutable").toBool();
    bool filterWrite = args.value("filterWritable").toBool();

    if (value.isEmpty())
        return makeTextResult("Missing 'value' (e.g. \"120\")", true);

    QString regErr;
    auto constrainRegions = parseRegionsArg(args, &regErr);
    if (!regErr.isEmpty())
        return makeTextResult(regErr, true);

    ValueType vt = valueTypeFromString(valueTypeStr);
    QVector<ScanResult> results = panel->runValueScanAndWait(vt, value, filterExec, filterWrite, constrainRegions);

    QString msg = QStringLiteral("Scan (%1 = %2): %3 result(s).")
        .arg(valueTypeStr.isEmpty() ? QStringLiteral("float") : valueTypeStr)
        .arg(value)
        .arg(results.size());
    if (!constrainRegions.isEmpty()) {
        uint64_t totalConstrained = 0;
        for (const auto& r : constrainRegions) totalConstrained += r.end - r.start;
        msg += QStringLiteral("\nRegion constraint: %1 range(s), %2 bytes total requested.")
            .arg(constrainRegions.size()).arg(totalConstrained);
    }
    const int showAddrs = 15;
    if (!results.isEmpty()) {
        msg += QStringLiteral("\nFirst addresses:");
        for (int i = 0; i < qMin(results.size(), showAddrs); i++) {
            msg += QStringLiteral("\n  0x%1").arg(results[i].address, 16, 16, QChar('0'));
            if (!results[i].regionModule.isEmpty())
                msg += QStringLiteral(" (%1)").arg(results[i].regionModule);
        }
        if (results.size() > showAddrs)
            msg += QStringLiteral("\n  ... and %1 more").arg(results.size() - showAddrs);
    }
    return makeTextResult(msg);
}

// ════════════════════════════════════════════════════════════════════
// TOOL: scanner.scan_pattern
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolScannerScanPattern(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    ScannerPanel* panel = m_mainWindow->m_scannerPanel;
    if (!panel) return makeTextResult("Scanner panel not available", true);

    QString pattern = args.value("pattern").toString().trimmed();
    bool filterExec = args.value("filterExecutable").toBool();
    bool filterWrite = args.value("filterWritable").toBool();

    if (pattern.isEmpty())
        return makeTextResult("Missing 'pattern' (e.g. \"00 00 20 42 00 00 20 42\")", true);

    QString regErr;
    auto constrainRegions = parseRegionsArg(args, &regErr);
    if (!regErr.isEmpty())
        return makeTextResult(regErr, true);

    // Use the resolved tab's provider so the scan runs on the same tab we attached to (source_switch).
    // If we used the panel's default getter we'd get the *active* tab's provider, which may be different.
    std::shared_ptr<rcx::Provider> provider = (tab->doc && tab->doc->provider) ? tab->doc->provider : nullptr;
    if (!provider) {
        return makeTextResult("No provider on this tab — the scan did not run. Use source_switch to attach to a process (or open a file), then run the pattern scan again. If you already ran source_switch, ensure the tab that was switched is the one used (e.g. pass tabIndex: 0 for the first tab).", true);
    }

    QVector<ScanResult> results = panel->runPatternScanAndWait(provider, pattern, filterExec, filterWrite, constrainRegions);

    QString msg = QStringLiteral("Pattern scan (%1): %2 result(s).")
        .arg(pattern)
        .arg(results.size());
    if (!constrainRegions.isEmpty()) {
        uint64_t totalConstrained = 0;
        for (const auto& r : constrainRegions) totalConstrained += r.end - r.start;
        msg += QStringLiteral("\nRegion constraint: %1 range(s), %2 bytes total requested.")
            .arg(constrainRegions.size()).arg(totalConstrained);
    }
    const int showAddrs = 15;
    if (!results.isEmpty()) {
        msg += QStringLiteral("\nFirst addresses:");
        for (int i = 0; i < qMin(results.size(), showAddrs); i++) {
            msg += QStringLiteral("\n  0x%1").arg(results[i].address, 16, 16, QChar('0'));
            if (!results[i].regionModule.isEmpty())
                msg += QStringLiteral(" (%1)").arg(results[i].regionModule);
        }
        if (results.size() > showAddrs)
            msg += QStringLiteral("\n  ... and %1 more").arg(results.size() - showAddrs);
    }
    return makeTextResult(msg);
}

// ════════════════════════════════════════════════════════════════════
// TOOL: mcp.reconnect
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolReconnect(const QJsonObject&) {
    QLocalSocket* sock = m_currentSender;
    if (!sock)
        return makeTextResult("No client connected.", true);
    // Disconnect after this response is sent so the client receives the result
    QTimer::singleShot(0, this, [this, sock]() {
        if (findClient(sock))
            sock->disconnectFromServer();
    });
    return makeTextResult("Disconnected. The MCP client will exit; your IDE may restart it and reconnect to Reclass.");
}

// ════════════════════════════════════════════════════════════════════
// TOOL: process.info — PEB address + TEB enumeration
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolProcessInfo(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    auto* prov = tab->doc->provider.get();
    if (!prov) return makeTextResult("No data source attached", true);
    if (!prov->isLive()) return makeTextResult("Not a live provider", true);

    uint64_t pebAddr = prov->peb();
    if (!pebAddr) return makeTextResult("PEB not available for this provider", true);

    QJsonObject out;
    out["peb"] = "0x" + QString::number(pebAddr, 16).toUpper();

    auto tebList = prov->tebs();
    QJsonArray tebArr;
    for (const auto& t : tebList) {
        tebArr.append(QJsonObject{
            {"address", "0x" + QString::number(t.tebAddress, 16).toUpper()},
            {"threadId", (qint64)t.threadId}
        });
    }

    out["tebs"] = tebArr;
    out["tebCount"] = tebArr.size();
    return makeTextResult(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: symbols.load — load PDB symbols into the global store
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolSymbolsLoad(const QJsonObject& args) {
    QString pdbPath = args.value("pdbPath").toString();
    if (pdbPath.isEmpty())
        return makeTextResult("pdbPath is required", true);

    if (!QFile::exists(pdbPath))
        return makeTextResult("File not found: " + pdbPath, true);

    QString symErr;
    auto result = extractPdbSymbols(pdbPath, &symErr);
    if (result.symbols.isEmpty())
        return makeTextResult(symErr.isEmpty()
            ? QStringLiteral("No symbols found in PDB") : symErr, true);

    QVector<QPair<QString, uint32_t>> pairs;
    QHash<QString, uint32_t> typeIndices;
    pairs.reserve(result.symbols.size());
    for (const auto& s : result.symbols) {
        pairs.emplaceBack(s.name, s.rva);
        if (s.typeIndex != 0)
            typeIndices.insert(s.name, s.typeIndex);
    }

    int count = SymbolStore::instance().addModule(result.moduleName, pdbPath, pairs);
    if (!typeIndices.isEmpty())
        SymbolStore::instance().addModuleTypeIndices(result.moduleName, typeIndices);

    // Refresh the active tab so annotations pick up new symbols
    auto* tab = resolveTab(args);
    if (tab && tab->ctrl)
        tab->ctrl->refresh();

    m_mainWindow->rebuildSymbolsModel();

    QJsonObject out;
    out["moduleName"] = result.moduleName;
    out["symbolCount"] = count;
    out["pdbPath"] = pdbPath;
    return makeTextResult(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: symbols.lookup — resolve symbol name to address
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolSymbolsLookup(const QJsonObject& args) {
    QString symbol = args.value("symbol").toString();
    if (symbol.isEmpty())
        return makeTextResult("symbol is required", true);

    auto* tab = resolveTab(args);
    if (!tab || !tab->doc->provider)
        return makeTextResult("No active tab or provider", true);

    auto* prov = tab->doc->provider.get();
    bool ok = false;
    uint64_t addr = SymbolStore::instance().resolve(symbol, prov, &ok);
    if (!ok || addr == 0)
        return makeTextResult("Symbol not found: " + symbol, true);

    QJsonObject out;
    out["symbol"] = symbol;
    out["address"] = "0x" + QString::number(addr, 16).toUpper();
    return makeTextResult(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: symbols.importType — import type definition for a symbol
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolSymbolsImportType(const QJsonObject& args) {
    QString symbol = args.value("symbol").toString();
    if (symbol.isEmpty())
        return makeTextResult("symbol is required", true);

    int bangIdx = symbol.indexOf('!');
    if (bangIdx <= 0 || bangIdx >= symbol.size() - 1)
        return makeTextResult("Symbol must be qualified: 'module!name'", true);

    QString modPart = symbol.left(bangIdx);

    // Look up the typeIndex from the symbol store
    uint32_t typeIdx = SymbolStore::instance().typeIndexForSymbol(symbol);
    if (typeIdx == 0)
        return makeTextResult("No type info for symbol '" + symbol +
            "'. The PDB may not have been loaded with symbols.load, or this "
            "is a public symbol (S_PUB32) without type metadata.", true);

    // Find the PDB path for this module
    QString canonical = SymbolStore::instance().resolveAlias(modPart);
    const auto* modData = SymbolStore::instance().moduleData(canonical);
    if (!modData || modData->pdbPath.isEmpty())
        return makeTextResult("No PDB path found for module '" + modPart + "'", true);

    // Import the type from the PDB
    QString importedTypeName;
    QString importErr;
    NodeTree importedTree = importTypeForSymbol(modData->pdbPath, typeIdx,
                                                &importedTypeName, &importErr);
    if (importedTree.nodes.isEmpty())
        return makeTextResult(importErr.isEmpty()
            ? QStringLiteral("Failed to import type for typeIndex %1").arg(typeIdx)
            : importErr, true);

    // Merge imported nodes into the active document
    auto* tab = resolveTab(args);
    if (!tab)
        return makeTextResult("No active tab", true);

    auto& tree = tab->doc->tree;
    tab->ctrl->setSuppressRefresh(true);
    tab->doc->undoStack.beginMacro(
        QStringLiteral("Import type for ") + symbol);

    // Map old IDs to new IDs to preserve parent-child relationships
    QHash<uint64_t, uint64_t> idMap;
    for (const auto& node : importedTree.nodes) {
        uint64_t newId = tree.reserveId();
        idMap[node.id] = newId;
    }

    for (const auto& node : importedTree.nodes) {
        Node copy = node;
        copy.id = idMap.value(node.id, node.id);
        copy.parentId = idMap.value(node.parentId, node.parentId);
        if (copy.refId != 0)
            copy.refId = idMap.value(node.refId, node.refId);
        tab->doc->undoStack.push(new RcxCommand(tab->ctrl,
            cmd::Insert{copy}));
    }

    tab->doc->undoStack.endMacro();
    tab->ctrl->setSuppressRefresh(false);
    tab->ctrl->refresh();
    m_mainWindow->rebuildWorkspaceModel();

    QJsonObject out;
    out["symbol"] = symbol;
    out["typeName"] = importedTypeName;
    out["typeIndex"] = (int)typeIdx;
    out["nodesImported"] = importedTree.nodes.size();
    return makeTextResult(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: node.read_value — read formatted typed values for nodes
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolNodeReadValue(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    const auto& tree = tab->doc->tree;
    auto* prov = tab->doc->provider.get();
    if (!prov) return makeTextResult("No provider", true);

    QJsonArray requestedIds = args.value("nodeIds").toArray();
    if (requestedIds.isEmpty())
        return makeTextResult("nodeIds array is required", true);

    QJsonObject result;
    for (const auto& idVal : requestedIds) {
        QString idStr = idVal.toString();
        uint64_t nodeId = idStr.toULongLong();
        int idx = tree.indexOfId(nodeId);
        if (idx < 0) {
            QJsonObject entry;
            entry["error"] = "node not found";
            result[idStr] = entry;
            continue;
        }

        const Node& node = tree.nodes[idx];

        // Compute absolute address
        int64_t signedOff = tree.computeOffset(idx);
        uint64_t addr = (signedOff >= 0)
            ? tree.baseAddress + static_cast<uint64_t>(signedOff) : 0;

        QJsonObject entry;
        entry["kind"] = kindToString(node.kind);
        entry["name"] = node.name;
        entry["offset"] = node.offset;
        entry["address"] = "0x" + QString::number(addr, 16).toUpper();

        if (node.kind == NodeKind::Struct || node.kind == NodeKind::Array) {
            // Containers don't have scalar values — return computed size
            int span = tree.structSpan(node.id);
            entry["computedSize"] = span;
            entry["value"] = QStringLiteral("(container, size=0x%1)")
                .arg(QString::number(span, 16).toUpper());
        } else if (addr != 0 && prov->isReadable(addr, node.byteSize())) {
            // Read formatted value using the same formatting as the editor
            int numLines = linesForKind(node.kind);
            if (numLines <= 1) {
                entry["value"] = fmt::readValue(node, *prov, addr, 0);
            } else {
                // Multi-line types (Mat4x4): return all sub-lines
                QJsonArray lines;
                for (int sub = 0; sub < numLines; sub++)
                    lines.append(fmt::readValue(node, *prov, addr, sub));
                entry["value"] = lines;
            }
        } else {
            entry["value"] = QJsonValue();
            entry["error"] = "not readable";
        }

        result[idStr] = entry;
    }

    return makeTextResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: analysis.infer_types — batch type inference on hex nodes
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolAnalysisInferTypes(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    const auto& tree = tab->doc->tree;
    auto* prov = tab->doc->provider.get();
    if (!prov) return makeTextResult("No provider", true);

    QJsonArray requestedIds = args.value("nodeIds").toArray();
    if (requestedIds.isEmpty())
        return makeTextResult("nodeIds array is required", true);

    bool useHistory = !args.contains("useHistory") || args.value("useHistory").toBool();
    const auto& valueHistory = tab->ctrl->valueHistory();

    QString output;
    int analyzed = 0;

    for (const auto& idVal : requestedIds) {
        uint64_t nodeId = idVal.toString().toULongLong();
        int idx = tree.indexOfId(nodeId);
        if (idx < 0) {
            output += QStringLiteral("node %1: not found\n").arg(nodeId);
            continue;
        }

        const Node& node = tree.nodes[idx];
        int sz = sizeForKind(node.kind);
        if (sz <= 0) {
            output += QStringLiteral("node %1 (%2): not a fixed-size type\n")
                .arg(nodeId).arg(node.name);
            continue;
        }

        int64_t signedOff = tree.computeOffset(idx);
        uint64_t addr = (signedOff >= 0)
            ? tree.baseAddress + static_cast<uint64_t>(signedOff) : 0;

        if (addr == 0 || !prov->isReadable(addr, sz)) {
            output += QStringLiteral("node %1 (%2): not readable at 0x%3\n")
                .arg(nodeId).arg(node.name)
                .arg(QString::number(addr, 16).toUpper());
            continue;
        }

        QByteArray bytes = prov->readBytes(addr, sz);
        const uint8_t* data = reinterpret_cast<const uint8_t*>(bytes.constData());

        // Build inference hints from value history
        InferHints hints;
        hints.ptrSize = tree.pointerSize;
        if (useHistory) {
            auto histIt = valueHistory.constFind(nodeId);
            if (histIt != valueHistory.constEnd()) {
                hints.sampleCount = histIt->uniqueCount();
                hints.neverChanged = (histIt->heatLevel() == 0 && histIt->count > 0);
            }
        }

        auto suggestions = inferTypes(data, sz, hints);

        output += QStringLiteral("node %1 (%2) at 0x%3, %4 bytes")
            .arg(nodeId).arg(node.name)
            .arg(QString::number(addr, 16).toUpper())
            .arg(sz);
        if (hints.sampleCount > 0)
            output += QStringLiteral(" [%1 samples, heat=%2]")
                .arg(hints.sampleCount).arg(hints.neverChanged ? 0 : 1);
        output += QStringLiteral(":\n");

        if (suggestions.isEmpty()) {
            output += QStringLiteral("  (no suggestions — likely all zeros)\n");
        } else {
            for (const auto& s : suggestions) {
                QString label = formatHint(s);
                QString preview = inferPreview(data, sz, s);
                QString strengthStr;
                switch (s.strength) {
                case 1: strengthStr = QStringLiteral("weak"); break;
                case 2: strengthStr = QStringLiteral("moderate"); break;
                case 3: strengthStr = QStringLiteral("strong"); break;
                default: strengthStr = QStringLiteral("none"); break;
                }
                output += QStringLiteral("  [%1] score=%2 (%3)")
                    .arg(label).arg(s.score).arg(strengthStr);
                if (!preview.isEmpty())
                    output += QStringLiteral("  value: %1").arg(preview);
                output += QStringLiteral("\n");
            }
        }
        analyzed++;
    }

    output += QStringLiteral("\nAnalyzed %1 node(s).\n").arg(analyzed);
    return makeTextResult(output);
}

// ════════════════════════════════════════════════════════════════════
// TOOL: analysis.import_header — import C/C++ struct definitions
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolAnalysisImportHeader(const QJsonObject& args) {
    QString sourceCode = args.value("sourceCode").toString();
    if (sourceCode.trimmed().isEmpty())
        return makeTextResult("sourceCode is required", true);

    int pointerSize = (int)parseInteger(args.value("pointerSize"), 8);
    if (pointerSize != 4 && pointerSize != 8) pointerSize = 8;

    // Parse source code into a NodeTree
    QString parseError;
    NodeTree importedTree = importFromSource(sourceCode, &parseError, pointerSize);
    if (importedTree.nodes.isEmpty())
        return makeTextResult(parseError.isEmpty()
            ? QStringLiteral("No struct definitions found in source code")
            : parseError, true);

    // Merge into active document
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    auto& tree = tab->doc->tree;
    tab->ctrl->setSuppressRefresh(true);

    // Count root structs for status
    int classCount = 0;
    for (const auto& n : importedTree.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) classCount++;

    tab->doc->undoStack.beginMacro(
        QStringLiteral("Import %1 type(s) from source").arg(classCount));

    // Map old IDs to new IDs to preserve parent-child relationships
    QHash<uint64_t, uint64_t> idMap;
    for (const auto& node : importedTree.nodes)
        idMap[node.id] = tree.reserveId();

    for (const auto& node : importedTree.nodes) {
        Node copy = node;
        copy.id = idMap.value(node.id, node.id);
        copy.parentId = idMap.value(node.parentId, node.parentId);
        if (copy.refId != 0)
            copy.refId = idMap.value(node.refId, node.refId);
        tab->doc->undoStack.push(new RcxCommand(tab->ctrl,
            cmd::Insert{copy}));
    }

    tab->doc->undoStack.endMacro();
    tab->ctrl->setSuppressRefresh(false);
    tab->ctrl->refresh();
    m_mainWindow->rebuildWorkspaceModel();

    // Build summary
    QStringList typeNames;
    for (const auto& n : importedTree.nodes) {
        if (n.parentId == 0 && n.kind == NodeKind::Struct) {
            QString name = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
            if (!name.isEmpty()) typeNames << name;
        }
    }

    QJsonObject out;
    out["typesImported"] = classCount;
    out["nodesImported"] = importedTree.nodes.size();
    out["typeNames"] = QJsonArray::fromStringList(typeNames);
    return makeTextResult(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: analysis.pointer_chain — follow pointers with inference
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolAnalysisPointerChain(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    auto* prov = tab->doc->provider.get();
    if (!prov) return makeTextResult("No provider", true);

    int64_t addr = parseInteger(args.value("address"));
    if (args.value("baseRelative").toBool())
        addr += (int64_t)tab->doc->tree.baseAddress;

    int maxDepth = qBound(1, (int)parseInteger(args.value("maxDepth"), 3), 8);
    int readLen = qBound(8, (int)parseInteger(args.value("readLength"), 64), 512);
    int ptrSize = tab->doc->tree.pointerSize;

    // Cache executable regions for vtable detection
    auto regions = prov->enumerateRegions();
    auto isExecutable = [&regions](uint64_t va) -> bool {
        for (const auto& r : regions) {
            if (va >= r.base && va < r.base + r.size)
                return r.executable;
        }
        return false;
    };

    // Get symbol lookup function
    bool hasSymbols = SymbolStore::instance().hasSymbols();
    auto symLookup = [&](uint64_t va) -> QString {
        if (!hasSymbols) return {};
        return SymbolStore::instance().getSymbolForAddress(va, prov);
    };

    QString output;
    QSet<uint64_t> visited; // cycle detection

    for (int depth = 0; depth < maxDepth; depth++) {
        uint64_t curAddr = static_cast<uint64_t>(addr);

        if (visited.contains(curAddr)) {
            output += QStringLiteral("\n--- Level %1: CYCLE at 0x%2 ---\n")
                .arg(depth).arg(QString::number(curAddr, 16).toUpper());
            break;
        }
        visited.insert(curAddr);

        if (!prov->isReadable(curAddr, readLen)) {
            output += QStringLiteral("\n--- Level %1: 0x%2 (not readable) ---\n")
                .arg(depth).arg(QString::number(curAddr, 16).toUpper());
            break;
        }

        QByteArray data = prov->readBytes(curAddr, readLen);
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(data.constData());

        // Symbol annotation for the address itself
        QString addrSym = symLookup(curAddr);
        output += QStringLiteral("\n--- Level %1: 0x%2")
            .arg(depth).arg(QString::number(curAddr, 16).toUpper());
        if (!addrSym.isEmpty())
            output += QStringLiteral(" (%1)").arg(addrSym);
        output += QStringLiteral(" ---\n");

        // Hex dump (compact: 16 bytes per line)
        for (int i = 0; i < data.size(); i += 16) {
            int lineLen = qMin(16, data.size() - i);
            output += QStringLiteral("+%1: ").arg(i, 4, 16, QChar('0'));
            for (int j = 0; j < 16; j++) {
                if (j < lineLen)
                    output += QStringLiteral("%1 ").arg((uint8_t)data[i+j], 2, 16, QChar('0'));
                else
                    output += QStringLiteral("   ");
                if (j == 7) output += QStringLiteral(" ");
            }
            output += QStringLiteral(" |");
            for (int j = 0; j < lineLen; j++) {
                uint8_t c = (uint8_t)data[i+j];
                output += (c >= 0x20 && c <= 0x7e) ? QChar(c) : QChar('.');
            }
            output += QStringLiteral("|\n");
        }

        // Per-field type inference (8-byte aligned for 64-bit, 4-byte for 32-bit)
        int chunkSize = (ptrSize >= 8) ? 8 : 4;
        output += QStringLiteral("\nField inference:\n");
        for (int off = 0; off + chunkSize <= data.size(); off += chunkSize) {
            InferHints hints;
            hints.ptrSize = ptrSize;
            auto suggestions = inferTypes(raw + off, chunkSize, hints);

            output += QStringLiteral("  +0x%1: ").arg(off, 2, 16, QChar('0'));
            if (suggestions.isEmpty()) {
                output += QStringLiteral("(zero / unknown)\n");
            } else {
                const auto& top = suggestions[0];
                QString label = formatHint(top);
                QString preview = inferPreview(raw + off, chunkSize, top);
                output += QStringLiteral("[%1] score=%2").arg(label).arg(top.score);
                if (!preview.isEmpty())
                    output += QStringLiteral("  %1").arg(preview);

                // Symbol annotation for pointer-like values
                if ((top.kinds.size() == 1) &&
                    (top.kinds[0] == NodeKind::Pointer64 || top.kinds[0] == NodeKind::Pointer32)) {
                    uint64_t ptrVal = (chunkSize >= 8)
                        ? detail::loadU64(raw + off)
                        : (uint64_t)detail::loadU32(raw + off);
                    QString sym = symLookup(ptrVal);
                    if (!sym.isEmpty())
                        output += QStringLiteral("  // %1").arg(sym);
                }
                output += QStringLiteral("\n");
            }
        }

        // Vtable detection: check if offset 0 points to an array of code pointers
        {
            uint64_t firstQword = (ptrSize >= 8)
                ? detail::loadU64(raw) : (uint64_t)detail::loadU32(raw);
            if (firstQword != 0 && firstQword != UINT64_MAX
                && prov->isReadable(firstQword, ptrSize * 4)) {
                QByteArray vtableData = prov->readBytes(firstQword, ptrSize * 16);
                const uint8_t* vtRaw = reinterpret_cast<const uint8_t*>(vtableData.constData());
                int codePointers = 0;
                int totalChecked = qMin(16, vtableData.size() / ptrSize);
                for (int i = 0; i < totalChecked; i++) {
                    uint64_t entry = (ptrSize >= 8)
                        ? detail::loadU64(vtRaw + i * ptrSize)
                        : (uint64_t)detail::loadU32(vtRaw + i * ptrSize);
                    if (entry != 0 && isExecutable(entry))
                        codePointers++;
                }
                if (codePointers >= 3 && codePointers >= totalChecked / 2) {
                    output += QStringLiteral("\nVtable detected at 0x%1 (%2/%3 entries are code pointers)\n")
                        .arg(QString::number(firstQword, 16).toUpper())
                        .arg(codePointers).arg(totalChecked);
                    // Show first few vtable entries with symbols
                    for (int i = 0; i < qMin(8, totalChecked); i++) {
                        uint64_t entry = (ptrSize >= 8)
                            ? detail::loadU64(vtRaw + i * ptrSize)
                            : (uint64_t)detail::loadU32(vtRaw + i * ptrSize);
                        QString sym = symLookup(entry);
                        output += QStringLiteral("  [%1] 0x%2")
                            .arg(i).arg(QString::number(entry, 16).toUpper());
                        if (!sym.isEmpty())
                            output += QStringLiteral("  %1").arg(sym);
                        output += QStringLiteral("\n");
                    }
                }
            }
        }

        // Follow the first pointer-like value to next level
        uint64_t nextAddr = 0;
        if (ptrSize >= 8 && data.size() >= 8) {
            uint64_t v = detail::loadU64(raw);
            // Canonical x64 user-mode address check
            if (v >= 0x10000 && v <= 0x00007FFFFFFFFFFFULL)
                nextAddr = v;
        } else if (data.size() >= 4) {
            uint32_t v = detail::loadU32(raw);
            if (v >= 0x10000)
                nextAddr = v;
        }

        if (nextAddr == 0 || !prov->isReadable(nextAddr, ptrSize)) {
            if (depth < maxDepth - 1)
                output += QStringLiteral("\n(offset 0 is not a followable pointer — chain ends)\n");
            break;
        }

        addr = (int64_t)nextAddr;
    }

    return makeTextResult(output);
}

// ════════════════════════════════════════════════════════════════════
// TOOL: ui.inspect — query Ctrl+Shift+Click-selected UI region
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolUiInspect(const QJsonObject& args) {
    if (args.value("clear").toBool()) {
        m_mainWindow->clearInspection();
        return makeTextResult("Inspection cleared.");
    }

    QString region = args.value("region").toString();
    if (!region.isEmpty()) {
        // Direct region query (no Ctrl+Click needed)
        auto result = m_mainWindow->inspectAt(nullptr, QPoint());
        // Build a synthetic result for the named region
        MainWindow::InspectionResult r;
        r.selected = true;
        r.region = region;
        r.description = region;
        // Look up theme colors for the named region
        QStringList keys;
        // Inline the themeKeysForRegion logic (it's a static in main.cpp, not accessible here)
        // Instead, use the result's themeColors if we can inspect a real widget, or build from the region name
        // Simplest: store and return what the MainWindow computed
        // For now, return what we have
        QJsonObject out;
        out["selected"] = true;
        out["region"] = region;
        out["description"] = QStringLiteral("Named region query");
        return makeTextResult(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
    }

    // Return the currently selected region (from user's Ctrl+Shift+Click)
    const auto& r = m_mainWindow->m_inspectedRegion;
    if (!r.selected) {
        return makeTextResult(QStringLiteral(
            "No region selected. The user must Ctrl+Shift+Click on a UI element first.\n"
            "Known regions: editor.typeColumn, editor.nameColumn, editor.valueColumn, "
            "editor.hexBytes, editor.margin, editor.commandRow, editor.footer, "
            "editor.background, workspace.tree, dockTabBar, statusBar, menuBar, "
            "scanner, symbols.tree, mainWindow.border"));
    }

    QJsonObject out;
    out["selected"] = true;
    out["widget"] = r.widgetName;
    out["region"] = r.region;
    out["description"] = r.description;
    out["themeColors"] = r.themeColors;
    out["properties"] = r.properties;
    out["rect"] = QJsonObject{
        {"x", r.globalRect.x()}, {"y", r.globalRect.y()},
        {"w", r.globalRect.width()}, {"h", r.globalRect.height()}
    };
    return makeTextResult(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: theme.get — return all theme colors
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolThemeGet(const QJsonObject&) {
    const Theme& t = ThemeManager::instance().current();
    QJsonObject colors;
    for (int i = 0; i < kThemeFieldCount; i++) {
        QJsonObject entry;
        entry["value"] = (t.*kThemeFields[i].ptr).name();
        entry["label"] = QString::fromLatin1(kThemeFields[i].label);
        entry["group"] = QString::fromLatin1(kThemeFields[i].group);
        colors[QLatin1String(kThemeFields[i].key)] = entry;
    }
    QJsonObject out;
    out["name"] = t.name;
    out["colors"] = colors;
    return makeTextResult(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: theme.set — change colors with live preview
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolThemeSet(const QJsonObject& args) {
    QJsonObject colorsObj = args.value("colors").toObject();
    if (colorsObj.isEmpty())
        return makeTextResult("colors object is required", true);

    Theme modified = ThemeManager::instance().current();
    QJsonObject oldValues;
    int changed = 0;

    for (auto it = colorsObj.begin(); it != colorsObj.end(); ++it) {
        QString key = it.key();
        QString newHex = it.value().toString();
        QColor newColor(newHex);
        if (!newColor.isValid()) {
            return makeTextResult(QStringLiteral("Invalid color '%1' for key '%2'")
                .arg(newHex, key), true);
        }

        bool found = false;
        for (int i = 0; i < kThemeFieldCount; i++) {
            if (key == QLatin1String(kThemeFields[i].key)) {
                QColor& field = modified.*kThemeFields[i].ptr;
                oldValues[key] = field.name();
                field = newColor;
                found = true;
                changed++;
                break;
            }
        }
        if (!found) {
            return makeTextResult(QStringLiteral("Unknown theme key '%1'. Use theme.get to see valid keys.")
                .arg(key), true);
        }
    }

    ThemeManager::instance().previewTheme(modified);

    QJsonObject out;
    out["changed"] = changed;
    out["oldValues"] = oldValues;
    out["note"] = QStringLiteral("Preview applied. Call theme.save to persist, theme.revert to undo.");
    return makeTextResult(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: theme.save — persist previewed changes
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolThemeSave(const QJsonObject&) {
    auto& tm = ThemeManager::instance();
    // Get the currently displayed theme (which is the previewed one)
    Theme current = tm.current();
    int idx = tm.currentIndex();
    tm.revertPreview(); // end preview state
    tm.updateTheme(idx, current); // persist the modified theme
    return makeTextResult(QStringLiteral("Theme saved as '%1'.").arg(current.name));
}

// ════════════════════════════════════════════════════════════════════
// TOOL: theme.revert — undo preview, restore original
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolThemeRevert(const QJsonObject&) {
    ThemeManager::instance().revertPreview();
    return makeTextResult("Theme reverted to original.");
}

// ════════════════════════════════════════════════════════════════════
// TOOL: bookmarks.list / add / remove
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolBookmarksList(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab || !tab->doc) return makeTextResult("No active project.");
    QJsonArray out;
    for (const auto& b : tab->doc->tree.bookmarks) {
        QJsonObject o;
        o["name"] = b.name;
        o["addressFormula"] = b.addressFormula;
        out.append(o);
    }
    return makeTextResult(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
}

QJsonObject McpBridge::toolBookmarksAdd(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab || !tab->ctrl) return makeTextResult("No active project.");
    QString name = args.value("name").toString().trimmed();
    QString formula = args.value("addressFormula").toString().trimmed();
    if (name.isEmpty() || formula.isEmpty())
        return makeTextResult("Both name and addressFormula are required.");
    tab->ctrl->addBookmark(name, formula);
    return makeTextResult(QStringLiteral("Bookmark added: %1 -> %2").arg(name, formula));
}

QJsonObject McpBridge::toolBookmarksRemove(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab || !tab->ctrl) return makeTextResult("No active project.");
    auto& bms = tab->doc->tree.bookmarks;
    int idx = -1;
    if (args.contains("index")) {
        idx = (int)parseInteger(args.value("index"));
    } else {
        QString name = args.value("name").toString();
        for (int i = 0; i < bms.size(); i++)
            if (bms[i].name == name) { idx = i; break; }
    }
    if (idx < 0 || idx >= bms.size())
        return makeTextResult("Bookmark not found.");
    QString removed = bms[idx].name;
    tab->ctrl->removeBookmark(idx);
    return makeTextResult(QStringLiteral("Removed bookmark: %1").arg(removed));
}

// ════════════════════════════════════════════════════════════════════
// Notifications (call from MainWindow/Controller hooks)
// ════════════════════════════════════════════════════════════════════

void McpBridge::notifyTreeChanged() {
    if (m_clients.isEmpty()) return;
    sendNotification("notifications/resources/updated",
                     QJsonObject{{"uri", "project://tree"}});
}

void McpBridge::notifyDataChanged() {
    if (m_clients.isEmpty()) return;
    sendNotification("notifications/resources/updated",
                     QJsonObject{{"uri", "project://data"}});
}

} // namespace rcx
