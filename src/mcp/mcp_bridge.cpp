#include "mcp_bridge.h"
#include "core.h"
#include "controller.h"
#include "generator.h"
#include "mainwindow.h"
#include <QCoreApplication>
#include <QSettings>
#include <QDebug>
#include <cstring>

namespace rcx {

// ════════════════════════════════════════════════════════════════════
// Construction / lifecycle
// ════════════════════════════════════════════════════════════════════

McpBridge::McpBridge(MainWindow* mainWindow, QObject* parent)
    : QObject(parent), m_mainWindow(mainWindow)
{}

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
    if (m_client) {
        m_client->disconnectFromServer();
        m_client = nullptr;
    }
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

// ════════════════════════════════════════════════════════════════════
// Connection handling
// ════════════════════════════════════════════════════════════════════

void McpBridge::onNewConnection() {
    auto* pending = m_server->nextPendingConnection();
    if (!pending) return;

    // Single client — disconnect previous
    if (m_client) {
        m_client->disconnectFromServer();
        m_client->deleteLater();
    }

    m_client = pending;
    m_readBuffer.clear();
    m_initialized = false;

    connect(m_client, &QLocalSocket::readyRead,
            this, &McpBridge::onReadyRead);
    connect(m_client, &QLocalSocket::disconnected,
            this, &McpBridge::onDisconnected);

    qDebug() << "[MCP] Client connected";
}

void McpBridge::onReadyRead() {
    m_readBuffer.append(m_client->readAll());

    // Newline-delimited JSON framing
    while (true) {
        int idx = m_readBuffer.indexOf('\n');
        if (idx < 0) break;
        QByteArray line = m_readBuffer.left(idx).trimmed();
        m_readBuffer.remove(0, idx + 1);
        if (!line.isEmpty())
            processLine(line);
    }
}

void McpBridge::onDisconnected() {
    qDebug() << "[MCP] Client disconnected";
    m_client = nullptr;
    m_initialized = false;
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
    if (!m_client) return;
    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    qDebug() << "[MCP] >>" << data.left(200);
    data.append('\n');
    m_client->write(data);
    m_client->flush();
}

void McpBridge::sendNotification(const QString& method, const QJsonObject& params) {
    QJsonObject n{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.isEmpty()) n["params"] = params;
    sendJson(n);
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
        QCoreApplication::processEvents();
        sendJson(handleInitialize(id, req.value("params").toObject()));
        m_mainWindow->clearMcpStatus();
    } else if (method == "tools/list") {
        m_mainWindow->setMcpStatus(QStringLiteral("MCP: tools/list"));
        QCoreApplication::processEvents();
        sendJson(handleToolsList(id));
        m_mainWindow->clearMcpStatus();
    } else if (method == "tools/call") {
        sendJson(handleToolsCall(id, req.value("params").toObject()));
    } else {
        sendJson(errReply(id, -32601, "Method not found: " + method));
    }
}

// ════════════════════════════════════════════════════════════════════
// MCP: initialize
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::handleInitialize(const QJsonValue& id, const QJsonObject&) {
    m_initialized = true;

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
            "- hex.read offset is relative to the struct base address by default. "
            "Use baseRelative=true for absolute virtual addresses in the process.\n"
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
                        "insert: {op:'insert', kind:'Hex64', name:'field', parentId:'ID', offset:0}. "
                        "change_kind: {op:'change_kind', nodeId:'ID', kind:'UInt32'}. "
                        "change_offset: {op:'change_offset', nodeId:'ID', offset:16}. "
                        "change_base: {op:'change_base', baseAddress:'0x400000', formula:'[0x233CA80]'} — formula is optional, enables auto-resolve on provider attach. "
                        "change_struct_type: {op:'change_struct_type', nodeId:'ID', structTypeName:'Name'}. "
                        "change_class_keyword: {op:'change_class_keyword', nodeId:'ID', classKeyword:'class'}. "
                        "change_pointer_ref: {op:'change_pointer_ref', nodeId:'ID', refId:'targetID'}. "
                        "change_array_meta: {op:'change_array_meta', nodeId:'ID', elementKind:'UInt32', arrayLen:10}. "
                        "collapse: {op:'collapse', nodeId:'ID', collapsed:true}. "
                        "Insert ops get auto-assigned IDs; use $0, $1 etc. to reference them in later ops. "
                        "Kinds: Hex8 Hex16 Hex32 Hex64 Int8 Int16 Int32 Int64 UInt8 UInt16 UInt32 UInt64 "
                        "Float Double Bool Pointer32 Pointer64 Vec2 Vec3 Vec4 Mat4x4 UTF8 UTF16 Struct Array"},
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

    // 4. hex.read
    tools.append(QJsonObject{
        {"name", "hex.read"},
        {"description", "Read raw bytes from provider (live process memory). Returns hex dump, ASCII, and multi-type "
                        "interpretations (u8/u16/u32/u64/i32/f32/f64/ptr/string heuristics). "
                        "Use this to see what actual values are in memory at any offset. "
                        "Offset is tree-relative (0-based, baseAddress added automatically) "
                        "unless baseRelative=true (offset is absolute virtual address in the process)."},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"offset", QJsonObject{{"type", "integer"}}},
                {"length", QJsonObject{{"type", "integer"}}},
                {"baseRelative", QJsonObject{{"type", "boolean"}}}
            }},
            {"required", QJsonArray{"offset", "length"}}
        }}
    });

    // 5. hex.write
    tools.append(QJsonObject{
        {"name", "hex.write"},
        {"description", "Write raw bytes to provider (through undo stack). Hex string format: '4D5A9000'"},
        {"inputSchema", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"tabIndex", QJsonObject{{"type", "integer"},
                    {"description", "MDI tab index (0-based). Omit for active tab."}}},
                {"offset", QJsonObject{{"type", "integer"}}},
                {"hexBytes", QJsonObject{{"type", "string"}}},
                {"baseRelative", QJsonObject{{"type", "boolean"}}}
            }},
            {"required", QJsonArray{"offset", "hexBytes"}}
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
    QCoreApplication::processEvents();  // paint immediately

    QJsonObject result;
    if      (toolName == "project.state")  result = toolProjectState(args);
    else if (toolName == "tree.apply")     result = toolTreeApply(args);
    else if (toolName == "source.switch")  result = toolSourceSwitch(args);
    else if (toolName == "hex.read")       result = toolHexRead(args);
    else if (toolName == "hex.write")      result = toolHexWrite(args);
    else if (toolName == "status.set")     result = toolStatusSet(args);
    else if (toolName == "ui.action")      result = toolUiAction(args);
    else if (toolName == "tree.search")   result = toolTreeSearch(args);
    else if (toolName == "node.history")  result = toolNodeHistory(args);
    else return errReply(id, -32601, "Unknown tool: " + toolName);

    m_mainWindow->clearMcpStatus();

    return okReply(id, result);
}

// ════════════════════════════════════════════════════════════════════
// Helper: resolve "$N" placeholder references
// ════════════════════════════════════════════════════════════════════

QString McpBridge::resolvePlaceholder(const QString& ref,
                                       const QHash<QString, uint64_t>& placeholderMap) {
    if (ref.startsWith('$')) {
        auto it = placeholderMap.find(ref);
        if (it != placeholderMap.end())
            return QString::number(it.value());
    }
    return ref;  // not a placeholder — return as-is
}

// ════════════════════════════════════════════════════════════════════
// Smart tab resolution
// ════════════════════════════════════════════════════════════════════

MainWindow::TabState* McpBridge::resolveTab(const QJsonObject& args) {
    // 1) Explicit tab index from args
    if (args.contains("tabIndex")) {
        int idx = args.value("tabIndex").toInt();
        auto* t = m_mainWindow->tabByIndex(idx);
        if (t) return t;
    }

    // 2) Active sub-window (user clicked on it)
    auto* t = m_mainWindow->activeTab();
    if (t) return t;

    // 3) Fall back to first available tab
    if (m_mainWindow->tabCount() > 0) {
        t = m_mainWindow->tabByIndex(0);
        if (t) return t;
    }

    // 4) No tabs at all — auto-create a project
    m_mainWindow->project_new();
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

    int maxDepth = args.value("depth").toInt(1);
    bool includeTree = args.contains("includeTree") ? args.value("includeTree").toBool() : true;
    bool includeMembers = args.value("includeMembers").toBool(false);
    int limit = qBound(1, args.value("limit").toInt(50), 500);
    int offset = qMax(0, args.value("offset").toInt(0));
    QString parentIdStr = args.value("parentId").toString();
    uint64_t filterParentId = parentIdStr.isEmpty() ? 0 : parentIdStr.toULongLong();

    QJsonObject state;
    state["baseAddress"] = "0x" + QString::number(tree.baseAddress, 16).toUpper();
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
        queue.append({filterParentId, 0});

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
                        queue.append({n.id, entry.depth + 1});
                    continue;
                }
                if (emitted >= limit) {
                    // Past limit — just keep counting total
                    if (entry.depth + 1 <= maxDepth)
                        queue.append({n.id, entry.depth + 1});
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
                    queue.append({n.id, entry.depth + 1});
            }
        }

        QJsonObject treeObj;
        treeObj["baseAddress"] = QString::number(tree.baseAddress, 16);
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
        if (i % 100 == 0 && ops.size() > 200)
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 5);

        QJsonObject op = ops[i].toObject();
        QString opType = op.value("op").toString();

        if (opType == "insert") {
            Node n;
            n.id = placeholders.value(QStringLiteral("$%1").arg(i), tree.reserveId());
            n.kind = kindFromString(op.value("kind").toString("Hex64"));
            n.name = op.value("name").toString();
            QString pid = resolvePlaceholder(op.value("parentId").toString("0"), placeholders);
            n.parentId = pid.toULongLong();
            n.offset = op.value("offset").toInt(0);
            n.structTypeName = op.value("structTypeName").toString();
            n.classKeyword = op.value("classKeyword").toString();
            n.strLen = op.value("strLen").toInt(64);
            n.elementKind = kindFromString(op.value("elementKind").toString("UInt8"));
            n.arrayLen = op.value("arrayLen").toInt(1);
            QString refStr = resolvePlaceholder(op.value("refId").toString("0"), placeholders);
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
                int newOff = op.value("offset").toInt();
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
                int newLen = op.value("arrayLen").toInt(1);
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
        else {
            skippedOps.append(QStringLiteral("op[%1]: unknown op '%2'").arg(i).arg(opType));
        }

        // Slow mode: refresh after each operation for visual feedback
        if (m_slowMode && applied > 0) {
            ctrl->refresh();
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 16);
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
        int idx = args.value("sourceIndex").toInt();
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
        uint32_t pid = (uint32_t)args.value("pid").toInt();
        QString name = args.value("processName").toString();
        if (name.isEmpty()) name = QString("PID %1").arg(pid);
        QString target = QString("%1:%2").arg(pid).arg(name);
        ctrl->attachViaPlugin(QStringLiteral("processmemory"), target);
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
// TOOL: hex.read
// ════════════════════════════════════════════════════════════════════

QJsonObject McpBridge::toolHexRead(const QJsonObject& args) {
    auto* tab = resolveTab(args);
    if (!tab) return makeTextResult("No active tab", true);

    auto* prov = tab->doc->provider.get();
    if (!prov) return makeTextResult("No provider", true);

    int64_t offset = static_cast<int64_t>(args.value("offset").toDouble());
    int length = qMin(args.value("length").toInt(64), 4096);

    if (!args.value("baseRelative").toBool())
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

    int64_t offset = static_cast<int64_t>(args.value("offset").toDouble());
    QString hexStr = args.value("hexBytes").toString().remove(' ');

    if (!args.value("baseRelative").toBool())
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
    int limit = qBound(1, args.value("limit").toInt(20), 100);

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

// ════════════════════════════════════════════════════════════════════
// Notifications (call from MainWindow/Controller hooks)
// ════════════════════════════════════════════════════════════════════

void McpBridge::notifyTreeChanged() {
    if (!m_client || !m_initialized) return;
    sendNotification("notifications/resources/updated",
                     QJsonObject{{"uri", "project://tree"}});
}

void McpBridge::notifyDataChanged() {
    if (!m_client || !m_initialized) return;
    sendNotification("notifications/resources/updated",
                     QJsonObject{{"uri", "project://data"}});
}

} // namespace rcx
