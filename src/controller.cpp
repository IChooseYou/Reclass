#include "controller.h"
#include "providers/process_provider.h"
#include "providerregistry.h"
#include "processpicker.h"
#include <Qsci/qsciscintilla.h>
#include <QSplitter>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMenu>
#include <QInputDialog>
#include <QClipboard>
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QtConcurrent/QtConcurrentRun>
#ifdef _WIN32
#include <psapi.h>
#endif

namespace rcx {

static thread_local const RcxDocument* s_composeDoc = nullptr;

static QString docTypeNameProvider(NodeKind k) {
    if (s_composeDoc) return s_composeDoc->resolveTypeName(k);
    auto* m = kindMeta(k);
    return m ? QString::fromLatin1(m->typeName) : QStringLiteral("???");
}

static QString elide(QString s, int max) {
    if (max <= 0) return {};
    if (s.size() <= max) return s;
    if (max == 1) return QStringLiteral("\u2026");
    return s.left(max - 1) + QChar(0x2026);
}

static QString elideLeft(const QString& s, int max) {
    if (s.size() <= max) return s;
    if (max <= 1) return QStringLiteral("\u2026").left(max);
    return QStringLiteral("\u2026") + s.right(max - 1);
}

static QString crumbFor(const rcx::NodeTree& t, uint64_t nodeId) {
    QStringList parts;
    QSet<uint64_t> seen;
    uint64_t cur = nodeId;
    while (cur != 0 && !seen.contains(cur)) {
        seen.insert(cur);
        int idx = t.indexOfId(cur);
        if (idx < 0) break;
        const auto& n = t.nodes[idx];
        parts << (n.name.isEmpty() ? QStringLiteral("<unnamed>") : n.name);
        cur = n.parentId;
    }
    std::reverse(parts.begin(), parts.end());
    if (parts.size() > 4)
        parts = {parts.front(), QStringLiteral("\u2026"), parts[parts.size() - 2], parts.back()};
    return parts.join(QStringLiteral(" \u00B7 "));
}

// ── RcxDocument ──

RcxDocument::RcxDocument(QObject* parent)
    : QObject(parent)
    , provider(std::make_shared<NullProvider>())
{
    connect(&undoStack, &QUndoStack::cleanChanged, this, [this](bool clean) {
        modified = !clean;
    });
}

ComposeResult RcxDocument::compose(uint64_t viewRootId) const {
    return rcx::compose(tree, *provider, viewRootId);
}

bool RcxDocument::save(const QString& path) {
    QJsonObject json = tree.toJson();

    // Save type aliases
    if (!typeAliases.isEmpty()) {
        QJsonObject aliasObj;
        for (auto it = typeAliases.begin(); it != typeAliases.end(); ++it)
            aliasObj[kindToString(it.key())] = it.value();
        json["typeAliases"] = aliasObj;
    }

    QJsonDocument jdoc(json);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(jdoc.toJson(QJsonDocument::Indented));
    filePath = path;
    undoStack.setClean();
    modified = false;
    return true;
}

bool RcxDocument::load(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    undoStack.clear();
    QJsonDocument jdoc = QJsonDocument::fromJson(file.readAll());
    QJsonObject root = jdoc.object();
    tree = NodeTree::fromJson(root);

    // Load type aliases
    typeAliases.clear();
    QJsonObject aliasObj = root["typeAliases"].toObject();
    for (auto it = aliasObj.begin(); it != aliasObj.end(); ++it) {
        NodeKind k = kindFromString(it.key());
        QString v = it.value().toString();
        if (!v.isEmpty())
            typeAliases[k] = v;
    }

    filePath = path;
    modified = false;
    emit documentChanged();
    return true;
}

void RcxDocument::loadData(const QString& binaryPath) {
    QFile file(binaryPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    undoStack.clear();
    provider = std::make_shared<BufferProvider>(
        file.readAll(), QFileInfo(binaryPath).fileName());
    dataPath = binaryPath;
    tree.baseAddress = 0;
    emit documentChanged();
}

void RcxDocument::loadData(const QByteArray& data) {
    undoStack.clear();
    provider = std::make_shared<BufferProvider>(data);
    tree.baseAddress = 0;
    emit documentChanged();
}

// ── RcxCommand ──

RcxCommand::RcxCommand(RcxController* ctrl, Command cmd)
    : m_ctrl(ctrl), m_cmd(cmd) {}

void RcxCommand::undo() { m_ctrl->applyCommand(m_cmd, true); }
void RcxCommand::redo() { m_ctrl->applyCommand(m_cmd, false); }

// ── RcxController ──

RcxController::RcxController(RcxDocument* doc, QWidget* parent)
    : QObject(parent), m_doc(doc)
{
    fmt::setTypeNameProvider(docTypeNameProvider);
    connect(m_doc, &RcxDocument::documentChanged, this, &RcxController::refresh);
    setupAutoRefresh();
}

RcxController::~RcxController() {
    if (m_refreshWatcher) {
        m_refreshWatcher->cancel();
        m_refreshWatcher->waitForFinished();
    }
}

RcxEditor* RcxController::primaryEditor() const {
    return m_editors.isEmpty() ? nullptr : m_editors.first();
}

RcxEditor* RcxController::addSplitEditor(QSplitter* splitter) {
    auto* editor = new RcxEditor(splitter);
    splitter->addWidget(editor);
    m_editors.append(editor);
    connectEditor(editor);

    if (!m_lastResult.text.isEmpty()) {
        editor->applyDocument(m_lastResult);
    }
    updateCommandRow();
    return editor;
}

void RcxController::removeSplitEditor(RcxEditor* editor) {
    m_editors.removeOne(editor);
    editor->deleteLater();
}

void RcxController::connectEditor(RcxEditor* editor) {
    connect(editor, &RcxEditor::marginClicked,
            this, [this, editor](int margin, int line, Qt::KeyboardModifiers mods) {
        handleMarginClick(editor, margin, line, mods);
    });
    connect(editor, &RcxEditor::contextMenuRequested,
            this, [this, editor](int line, int nodeIdx, int subLine, QPoint globalPos) {
        showContextMenu(editor, line, nodeIdx, subLine, globalPos);
    });
    connect(editor, &RcxEditor::nodeClicked,
            this, [this, editor](int line, uint64_t nodeId, Qt::KeyboardModifiers mods) {
        handleNodeClick(editor, line, nodeId, mods);
    });

    // Inline editing signals
    connect(editor, &RcxEditor::inlineEditCommitted,
            this, [this](int nodeIdx, int subLine, EditTarget target, const QString& text) {
        // CommandRow BaseAddress/Source/RootClass edit has nodeIdx=-1
        if (nodeIdx < 0 && target != EditTarget::BaseAddress && target != EditTarget::Source
            && target != EditTarget::RootClassType && target != EditTarget::RootClassName) { refresh(); return; }
        switch (target) {
        case EditTarget::Name: {
            if (text.isEmpty()) break;
            const Node& node = m_doc->tree.nodes[nodeIdx];
            // ASCII edit on Hex/Padding nodes
            if (isHexPreview(node.kind)) {
                setNodeValue(nodeIdx, subLine, text, /*isAscii=*/true);
            } else {
                renameNode(nodeIdx, text);
            }
            break;
        }
        case EditTarget::Type: {
            // Check for array type syntax: "type[count]" e.g. "int32_t[10]"
            int bracketPos = text.indexOf('[');
            if (bracketPos > 0 && text.endsWith(']')) {
                QString elemTypeName = text.left(bracketPos).trimmed();
                QString countStr = text.mid(bracketPos + 1, text.size() - bracketPos - 2);
                bool countOk;
                int newCount = countStr.toInt(&countOk);
                if (countOk && newCount > 0) {
                    bool typeOk;
                    NodeKind elemKind = kindFromTypeName(elemTypeName, &typeOk);
                    if (typeOk && nodeIdx < m_doc->tree.nodes.size()) {
                        const Node& node = m_doc->tree.nodes[nodeIdx];
                        if (node.kind == NodeKind::Array) {
                            m_doc->undoStack.push(new RcxCommand(this,
                                cmd::ChangeArrayMeta{node.id,
                                    node.elementKind, elemKind,
                                    node.arrayLen, newCount}));
                        }
                    }
                }
            } else {
                // Regular type change
                bool ok;
                NodeKind k = kindFromTypeName(text, &ok);
                if (ok) {
                    changeNodeKind(nodeIdx, k);
                } else if (nodeIdx < m_doc->tree.nodes.size()) {
                    // Check if it's a defined struct type name
                    bool isStructType = false;
                    for (const auto& n : m_doc->tree.nodes) {
                        if (n.kind == NodeKind::Struct && n.structTypeName == text) {
                            isStructType = true;
                            break;
                        }
                    }
                    if (isStructType) {
                        auto& node = m_doc->tree.nodes[nodeIdx];
                        if (node.kind != NodeKind::Struct)
                            changeNodeKind(nodeIdx, NodeKind::Struct);
                        int idx = m_doc->tree.indexOfId(node.id);
                        if (idx >= 0) {
                            QString oldTypeName = m_doc->tree.nodes[idx].structTypeName;
                            if (oldTypeName != text) {
                                m_doc->undoStack.push(new RcxCommand(this,
                                    cmd::ChangeStructTypeName{node.id, oldTypeName, text}));
                            }
                        }
                    }
                }
            }
            break;
        }
        case EditTarget::Value:
            setNodeValue(nodeIdx, subLine, text);
            break;
        case EditTarget::BaseAddress: {
            QString s = text.trimmed();
            // Support simple equations: 0x10+0x4, 0x100-0x10, etc.
            uint64_t newBase = 0;
            bool ok = true;
            int pos = 0;
            bool firstTerm = true;
            bool adding = true;

            while (pos < s.size() && ok) {
                // Skip whitespace
                while (pos < s.size() && s[pos].isSpace()) pos++;
                if (pos >= s.size()) break;

                // Check for +/- operator (except first term)
                if (!firstTerm) {
                    if (s[pos] == '+') { adding = true; pos++; }
                    else if (s[pos] == '-') { adding = false; pos++; }
                    else { ok = false; break; }
                    while (pos < s.size() && s[pos].isSpace()) pos++;
                }

                // Parse hex number (with or without 0x prefix)
                int start = pos;
                bool hasPrefix = (pos + 1 < s.size() &&
                    s[pos] == '0' && (s[pos+1] == 'x' || s[pos+1] == 'X'));
                if (hasPrefix) pos += 2;

                int numStart = pos;
                while (pos < s.size() && (s[pos].isDigit() ||
                       (s[pos] >= 'a' && s[pos] <= 'f') ||
                       (s[pos] >= 'A' && s[pos] <= 'F'))) pos++;

                if (pos == numStart) { ok = false; break; }

                QString numStr = s.mid(numStart, pos - numStart);
                uint64_t val = numStr.toULongLong(&ok, 16);
                if (!ok) break;

                if (adding) newBase += val;
                else newBase -= val;

                firstTerm = false;
            }

            if (ok && newBase != m_doc->tree.baseAddress) {
                uint64_t oldBase = m_doc->tree.baseAddress;
                m_doc->undoStack.push(new RcxCommand(this,
                    cmd::ChangeBase{oldBase, newBase}));
            }
            break;
        }
        case EditTarget::Source: {
            if (text.startsWith(QStringLiteral("#saved:"))) {
                int idx = text.mid(7).toInt();
                switchToSavedSource(idx);
            } else if (text == QStringLiteral("file")) {
                auto* w = qobject_cast<QWidget*>(parent());
                QString path = QFileDialog::getOpenFileName(w, "Load Binary Data", {}, "All Files (*)");
                if (!path.isEmpty()) {
                    // Save current source's base address before switching
                    if (m_activeSourceIdx >= 0 && m_activeSourceIdx < m_savedSources.size())
                        m_savedSources[m_activeSourceIdx].baseAddress = m_doc->tree.baseAddress;

                    m_doc->loadData(path);

                    // Check if this file is already saved
                    int existingIdx = -1;
                    for (int i = 0; i < m_savedSources.size(); i++) {
                        if (m_savedSources[i].kind == QStringLiteral("File")
                            && m_savedSources[i].filePath == path) {
                            existingIdx = i;
                            break;
                        }
                    }
                    if (existingIdx >= 0) {
                        m_activeSourceIdx = existingIdx;
                        m_doc->tree.baseAddress = m_savedSources[existingIdx].baseAddress;
                    } else {
                        SavedSourceEntry entry;
                        entry.kind = QStringLiteral("File");
                        entry.displayName = QFileInfo(path).fileName();
                        entry.filePath = path;
                        entry.baseAddress = m_doc->tree.baseAddress;
                        m_savedSources.append(entry);
                        m_activeSourceIdx = m_savedSources.size() - 1;
                    }
                    refresh();
                }
            }
            else if (text == QStringLiteral("process")) {
#ifdef _WIN32
                auto* w = qobject_cast<QWidget*>(parent());
                ProcessPicker picker(w);
                if (picker.exec() == QDialog::Accepted) {
                    // Save current source's base address before switching
                    if (m_activeSourceIdx >= 0 && m_activeSourceIdx < m_savedSources.size())
                        m_savedSources[m_activeSourceIdx].baseAddress = m_doc->tree.baseAddress;

                    uint32_t pid = picker.selectedProcessId();
                    QString procName = picker.selectedProcessName();
                    attachToProcess(pid, procName);

                    // Check if this process is already saved
                    int existingIdx = -1;
                    for (int i = 0; i < m_savedSources.size(); i++) {
                        if (m_savedSources[i].kind == QStringLiteral("Process")
                            && m_savedSources[i].pid == pid) {
                            existingIdx = i;
                            break;
                        }
                    }
                    if (existingIdx >= 0) {
                        m_activeSourceIdx = existingIdx;
                        m_savedSources[existingIdx].baseAddress = m_doc->tree.baseAddress;
                    } else {
                        SavedSourceEntry entry;
                        entry.kind = QStringLiteral("Process");
                        entry.displayName = procName;
                        entry.pid = pid;
                        entry.processName = procName;
                        entry.baseAddress = m_doc->tree.baseAddress;
                        m_savedSources.append(entry);
                        m_activeSourceIdx = m_savedSources.size() - 1;
                    }
                    refresh();
                }
#endif
            }
            else
            {
                // Look up provider in registry
                const auto* providerInfo = ProviderRegistry::instance().findProvider(text.toLower().replace(" ", ""));

                if (providerInfo) {
                    QString target;
                    bool selected = false;

                    // Execute provider's target selection
                    if (providerInfo->isBuiltin) {
                        // Built-in provider with factory function
                        if (providerInfo->factory) {
                            selected = providerInfo->factory(qobject_cast<QWidget*>(parent()), &target);
                        }
                    } else {
                        // Plugin-based provider
                        if (providerInfo->plugin) {
                            selected = providerInfo->plugin->selectTarget(qobject_cast<QWidget*>(parent()), &target);
                        }
                    }

                    if (selected && !target.isEmpty()) {
                        // Create provider from target
                        std::unique_ptr<Provider> provider;
                        QString errorMsg;

                        if (providerInfo->plugin)
                        {
                            provider = providerInfo->plugin->createProvider(target, &errorMsg);
                        }

                        // Apply provider or show error
                        if (provider) {
                            m_doc->undoStack.clear();
                            m_doc->provider = std::move(provider);
                            m_doc->dataPath.clear();
                            emit m_doc->documentChanged();
                            refresh();
                        } else if (!errorMsg.isEmpty()) {
                            QMessageBox::warning(qobject_cast<QWidget*>(parent()), "Provider Error", errorMsg);
                        }
                    }
                }
            }
            break;
        }
        case EditTarget::ArrayElementType: {
            if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) break;
            const Node& node = m_doc->tree.nodes[nodeIdx];
            if (node.kind != NodeKind::Array) break;
            bool ok;
            NodeKind elemKind = kindFromTypeName(text, &ok);
            if (ok && elemKind != node.elementKind) {
                m_doc->undoStack.push(new RcxCommand(this,
                    cmd::ChangeArrayMeta{node.id,
                        node.elementKind, elemKind,
                        node.arrayLen, node.arrayLen}));
            }
            break;
        }
        case EditTarget::ArrayElementCount: {
            if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) break;
            const Node& node = m_doc->tree.nodes[nodeIdx];
            if (node.kind != NodeKind::Array) break;
            bool ok;
            int newLen = text.toInt(&ok);
            if (ok && newLen > 0 && newLen <= 100000 && newLen != node.arrayLen) {
                m_doc->undoStack.push(new RcxCommand(this,
                    cmd::ChangeArrayMeta{node.id,
                        node.elementKind, node.elementKind,
                        node.arrayLen, newLen}));
            }
            break;
        }
        case EditTarget::PointerTarget: {
            if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) break;
            Node& node = m_doc->tree.nodes[nodeIdx];
            if (node.kind != NodeKind::Pointer32 && node.kind != NodeKind::Pointer64) break;
            // Find the struct with matching name or structTypeName
            uint64_t newRefId = 0;
            for (const auto& n : m_doc->tree.nodes) {
                if (n.kind == NodeKind::Struct &&
                    (n.structTypeName == text || n.name == text)) {
                    newRefId = n.id;
                    break;
                }
            }
            if (newRefId != node.refId) {
                m_doc->undoStack.push(new RcxCommand(this,
                    cmd::ChangePointerRef{node.id, node.refId, newRefId}));
            }
            break;
        }
        case EditTarget::RootClassType: {
            QString kw = text.toLower().trimmed();
            if (kw != QStringLiteral("struct") && kw != QStringLiteral("class") && kw != QStringLiteral("enum")) break;
            for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
                auto& n = m_doc->tree.nodes[i];
                if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                    QString oldKw = n.resolvedClassKeyword();
                    if (oldKw != kw) {
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ChangeClassKeyword{n.id, oldKw, kw}));
                    }
                    break;
                }
            }
            break;
        }
        case EditTarget::RootClassName: {
            // Rename the root struct's structTypeName
            if (!text.isEmpty()) {
                for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
                    auto& n = m_doc->tree.nodes[i];
                    if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                        QString oldName = n.structTypeName;
                        if (oldName != text) {
                            m_doc->undoStack.push(new RcxCommand(this,
                                cmd::ChangeStructTypeName{n.id, oldName, text}));
                        }
                        break;
                    }
                }
            }
            break;
        }
        case EditTarget::ArrayIndex:
        case EditTarget::ArrayCount:
            // Array navigation removed - these cases are unreachable
            break;
        }
        // Always refresh to restore canonical text (handles parse failures, no-ops, etc.)
        refresh();
    });
    connect(editor, &RcxEditor::inlineEditCancelled,
            this, [this]() { refresh(); });
}

void RcxController::setViewRootId(uint64_t id) {
    if (m_viewRootId == id) return;
    m_viewRootId = id;
    refresh();
}

void RcxController::scrollToNodeId(uint64_t nodeId) {
    if (auto* editor = primaryEditor())
        editor->scrollToNodeId(nodeId);
}

void RcxController::refresh() {
    // Bracket compose with thread-local doc pointer for type name resolution
    s_composeDoc = m_doc;

    // Compose against snapshot provider if active, otherwise real provider
    if (m_snapshotProv)
        m_lastResult = rcx::compose(m_doc->tree, *m_snapshotProv, m_viewRootId);
    else
        m_lastResult = m_doc->compose(m_viewRootId);

    s_composeDoc = nullptr;

    // Mark lines whose node data changed since last refresh
    if (!m_changedOffsets.isEmpty()) {
        for (auto& lm : m_lastResult.meta) {
            if (lm.nodeIdx < 0 || lm.nodeIdx >= m_doc->tree.nodes.size()) continue;
            int64_t offset = m_doc->tree.computeOffset(lm.nodeIdx);
            const Node& node = m_doc->tree.nodes[lm.nodeIdx];

            if (isHexPreview(node.kind)) {
                // Per-byte tracking for hex preview nodes
                int lineOff = (node.kind == NodeKind::Padding) ? lm.subLine * 8 : 0;
                int byteCount = lm.lineByteCount;
                for (int b = 0; b < byteCount; b++) {
                    if (m_changedOffsets.contains(offset + lineOff + b)) {
                        lm.changedByteIndices.append(b);
                        lm.dataChanged = true;
                    }
                }
            } else {
                // Use structSpan for containers (byteSize returns 0 for Array-of-Struct)
                int sz = (node.kind == NodeKind::Struct || node.kind == NodeKind::Array)
                    ? m_doc->tree.structSpan(node.id) : node.byteSize();
                for (int64_t b = offset; b < offset + sz; b++) {
                    if (m_changedOffsets.contains(b)) {
                        lm.dataChanged = true;
                        break;
                    }
                }
            }
        }
    }

    // Prune stale selections (nodes removed by undo/redo/delete)
    QSet<uint64_t> valid;
    for (uint64_t id : m_selIds) {
        uint64_t nodeId = id & ~kFooterIdBit;  // Strip footer bit for lookup
        if (m_doc->tree.indexOfId(nodeId) >= 0)
            valid.insert(id);  // Keep original ID (with footer bit if present)
    }
    m_selIds = valid;

    // Collect unique struct type names for the type picker
    QStringList customTypes;
    QSet<QString> seen;
    for (const auto& node : m_doc->tree.nodes) {
        if (node.kind == NodeKind::Struct && !node.structTypeName.isEmpty()) {
            if (!seen.contains(node.structTypeName)) {
                seen.insert(node.structTypeName);
                customTypes << node.structTypeName;
            }
        }
    }

    for (auto* editor : m_editors) {
        editor->setCustomTypeNames(customTypes);
        ViewState vs = editor->saveViewState();
        editor->applyDocument(m_lastResult);
        editor->restoreViewState(vs);
    }
    applySelectionOverlays();
    pushSavedSourcesToEditors();
    updateCommandRow();
}

void RcxController::changeNodeKind(int nodeIdx, NodeKind newKind) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    auto& node = m_doc->tree.nodes[nodeIdx];

    int oldSize = node.byteSize();
    // Compute what byteSize() would be with the new kind
    Node tmp = node;
    tmp.kind = newKind;
    int newSize = tmp.byteSize();

    if (newSize > 0 && newSize < oldSize) {
        // Shrinking: insert hex padding to fill gap (no offset shift)
        int gap = oldSize - newSize;
        uint64_t parentId = node.parentId;
        int baseOffset = node.offset + newSize;

        bool wasSuppressed = m_suppressRefresh;
        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Change type"));

        // Push type change with no offset adjustments
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeKind{node.id, node.kind, newKind, {}}));

        // Insert hex nodes to fill the gap (largest first for alignment)
        int padOffset = baseOffset;
        while (gap > 0) {
            NodeKind padKind;
            int padSize;
            if (gap >= 8)      { padKind = NodeKind::Hex64; padSize = 8; }
            else if (gap >= 4) { padKind = NodeKind::Hex32; padSize = 4; }
            else if (gap >= 2) { padKind = NodeKind::Hex16; padSize = 2; }
            else               { padKind = NodeKind::Hex8;  padSize = 1; }

            insertNode(parentId, padOffset, padKind,
                       QString("pad_%1").arg(padOffset, 2, 16, QChar('0')));
            padOffset += padSize;
            gap -= padSize;
        }

        m_doc->undoStack.endMacro();
        m_suppressRefresh = wasSuppressed;
        if (!m_suppressRefresh) refresh();
    } else {
        // Same size or larger: adjust sibling offsets as before
        int delta = newSize - oldSize;
        QVector<cmd::OffsetAdj> adjs;
        if (delta != 0 && oldSize > 0 && newSize > 0) {
            int oldEnd = node.offset + oldSize;
            auto siblings = m_doc->tree.childrenOf(node.parentId);
            for (int si : siblings) {
                if (si == nodeIdx) continue;
                auto& sib = m_doc->tree.nodes[si];
                if (sib.offset >= oldEnd)
                    adjs.append({sib.id, sib.offset, sib.offset + delta});
            }
        }
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeKind{node.id, node.kind, newKind, adjs}));
    }
}

void RcxController::renameNode(int nodeIdx, const QString& newName) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    auto& node = m_doc->tree.nodes[nodeIdx];
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::Rename{node.id, node.name, newName}));
}

void RcxController::insertNode(uint64_t parentId, int offset, NodeKind kind, const QString& name) {
    Node n;
    n.kind     = kind;
    n.name     = name;
    n.parentId = parentId;

    if (offset < 0) {
        // Auto-place after last sibling with alignment
        int maxEnd = 0;
        auto siblings = m_doc->tree.childrenOf(parentId);
        for (int si : siblings) {
            auto& sn = m_doc->tree.nodes[si];
            int sz  = (sn.kind == NodeKind::Struct || sn.kind == NodeKind::Array)
                ? m_doc->tree.structSpan(sn.id) : sn.byteSize();
            int end = sn.offset + sz;
            if (end > maxEnd) maxEnd = end;
        }
        int align = alignmentFor(kind);
        n.offset = (maxEnd + align - 1) / align * align;
    } else {
        n.offset = offset;
    }

    // Reserve unique ID atomically before pushing command
    n.id = m_doc->tree.reserveId();

    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{n}));
}

void RcxController::removeNode(int nodeIdx) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    const Node& node = m_doc->tree.nodes[nodeIdx];
    uint64_t nodeId = node.id;
    uint64_t parentId = node.parentId;

    // Compute size of deleted node/subtree
    int deletedSize = (node.kind == NodeKind::Struct || node.kind == NodeKind::Array)
        ? m_doc->tree.structSpan(node.id) : node.byteSize();
    int deletedEnd = node.offset + deletedSize;

    // Find siblings after this node and compute offset adjustments
    QVector<cmd::OffsetAdj> adjs;
    if (parentId != 0) {  // only adjust if not root-level
        auto siblings = m_doc->tree.childrenOf(parentId);
        for (int si : siblings) {
            if (si == nodeIdx) continue;
            auto& sib = m_doc->tree.nodes[si];
            if (sib.offset >= deletedEnd) {
                adjs.append({sib.id, sib.offset, sib.offset - deletedSize});
            }
        }
    }

    // Collect subtree
    QVector<int> indices = m_doc->tree.subtreeIndices(nodeId);
    QVector<Node> subtree;
    for (int i : indices)
        subtree.append(m_doc->tree.nodes[i]);

    m_doc->undoStack.push(new RcxCommand(this,
        cmd::Remove{nodeId, subtree, adjs}));
}

void RcxController::toggleCollapse(int nodeIdx) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    auto& node = m_doc->tree.nodes[nodeIdx];
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::Collapse{node.id, node.collapsed, !node.collapsed}));
}

void RcxController::applyCommand(const Command& command, bool isUndo) {
    auto& tree = m_doc->tree;

    std::visit([&](auto&& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, cmd::ChangeKind>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].kind = isUndo ? c.oldKind : c.newKind;
            for (const auto& adj : c.offAdjs) {
                int ai = tree.indexOfId(adj.nodeId);
                if (ai >= 0)
                    tree.nodes[ai].offset = isUndo ? adj.oldOffset : adj.newOffset;
            }
        } else if constexpr (std::is_same_v<T, cmd::Rename>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].name = isUndo ? c.oldName : c.newName;
        } else if constexpr (std::is_same_v<T, cmd::Collapse>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].collapsed = isUndo ? c.oldState : c.newState;
        } else if constexpr (std::is_same_v<T, cmd::Insert>) {
            if (isUndo) {
                // Revert offset adjustments
                for (const auto& adj : c.offAdjs) {
                    int ai = tree.indexOfId(adj.nodeId);
                    if (ai >= 0) tree.nodes[ai].offset = adj.oldOffset;
                }
                int idx = tree.indexOfId(c.node.id);
                if (idx >= 0) {
                    tree.nodes.remove(idx);
                    tree.invalidateIdCache();
                }
            } else {
                tree.addNode(c.node);
                // Apply offset adjustments
                for (const auto& adj : c.offAdjs) {
                    int ai = tree.indexOfId(adj.nodeId);
                    if (ai >= 0) tree.nodes[ai].offset = adj.newOffset;
                }
            }
        } else if constexpr (std::is_same_v<T, cmd::Remove>) {
            if (isUndo) {
                // Restore nodes first
                for (const Node& n : c.subtree)
                    tree.addNode(n);
                // Revert offset adjustments
                for (const auto& adj : c.offAdjs) {
                    int ai = tree.indexOfId(adj.nodeId);
                    if (ai >= 0) tree.nodes[ai].offset = adj.oldOffset;
                }
            } else {
                // Apply offset adjustments first (before removing changes indices)
                for (const auto& adj : c.offAdjs) {
                    int ai = tree.indexOfId(adj.nodeId);
                    if (ai >= 0) tree.nodes[ai].offset = adj.newOffset;
                }
                // Remove nodes
                QVector<int> indices = tree.subtreeIndices(c.nodeId);
                std::sort(indices.begin(), indices.end(), std::greater<int>());
                for (int idx : indices)
                    tree.nodes.remove(idx);
                tree.invalidateIdCache();
            }
        } else if constexpr (std::is_same_v<T, cmd::ChangeBase>) {
            tree.baseAddress = isUndo ? c.oldBase : c.newBase;
        } else if constexpr (std::is_same_v<T, cmd::WriteBytes>) {
            const QByteArray& bytes = isUndo ? c.oldBytes : c.newBytes;
            if (!m_doc->provider->writeBytes(c.addr, bytes))
                qWarning() << "WriteBytes failed at address" << Qt::hex << c.addr;
            // Patch snapshot so compose sees the new value immediately
            if (m_snapshotProv)
                m_snapshotProv->patchSnapshot(c.addr, bytes.constData(), bytes.size());
        } else if constexpr (std::is_same_v<T, cmd::ChangeArrayMeta>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0) {
                tree.nodes[idx].elementKind = isUndo ? c.oldElementKind : c.newElementKind;
                tree.nodes[idx].arrayLen = isUndo ? c.oldArrayLen : c.newArrayLen;
                if (tree.nodes[idx].viewIndex >= tree.nodes[idx].arrayLen)
                    tree.nodes[idx].viewIndex = qMax(0, tree.nodes[idx].arrayLen - 1);
            }
        } else if constexpr (std::is_same_v<T, cmd::ChangePointerRef>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0) {
                tree.nodes[idx].refId = isUndo ? c.oldRefId : c.newRefId;
                if (tree.nodes[idx].refId != 0)
                    tree.nodes[idx].collapsed = true;
            }
        } else if constexpr (std::is_same_v<T, cmd::ChangeStructTypeName>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].structTypeName = isUndo ? c.oldName : c.newName;
        } else if constexpr (std::is_same_v<T, cmd::ChangeClassKeyword>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].classKeyword = isUndo ? c.oldKeyword : c.newKeyword;
        } else if constexpr (std::is_same_v<T, cmd::ChangeOffset>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].offset = isUndo ? c.oldOffset : c.newOffset;
        }
    }, command);

    if (!m_suppressRefresh)
        refresh();
}

void RcxController::setNodeValue(int nodeIdx, int subLine, const QString& text,
                                  bool isAscii) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    if (!m_doc->provider->isWritable()) return;

    const Node& node = m_doc->tree.nodes[nodeIdx];
    uint64_t addr = m_doc->tree.computeOffset(nodeIdx);

    // For vector components, redirect to float parsing at sub-offset
    NodeKind editKind = node.kind;
    if ((node.kind == NodeKind::Vec2 || node.kind == NodeKind::Vec3 ||
         node.kind == NodeKind::Vec4) && subLine >= 0) {
        addr += subLine * 4;
        editKind = NodeKind::Float;
    }

    bool ok;
    QByteArray newBytes;
    if (isAscii) {
        int expectedSize = sizeForKind(editKind);
        newBytes = fmt::parseAsciiValue(text, expectedSize, &ok);
    } else {
        newBytes = fmt::parseValue(editKind, text, &ok);
    }
    if (!ok) return;

    // For strings, pad/truncate to full buffer size
    if (node.kind == NodeKind::UTF8 || node.kind == NodeKind::UTF16) {
        int fullSize = node.byteSize();
        newBytes = newBytes.left(fullSize);
        if (newBytes.size() < fullSize)
            newBytes.append(QByteArray(fullSize - newBytes.size(), '\0'));
    }

    if (newBytes.isEmpty()) return;

    int writeSize = newBytes.size();

    // Validate write range before pushing command
    if (!m_doc->provider->isReadable(addr, writeSize)) return;

    QByteArray oldBytes = m_doc->provider->readBytes(addr, writeSize);

    m_doc->undoStack.push(new RcxCommand(this,
        cmd::WriteBytes{addr, oldBytes, newBytes}));
}

void RcxController::duplicateNode(int nodeIdx) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    const Node& src = m_doc->tree.nodes[nodeIdx];
    if (src.kind == NodeKind::Struct || src.kind == NodeKind::Array) return;

    int copySize   = src.byteSize();
    int copyOffset = src.offset + copySize;

    // Shift later siblings down to make room for the copy
    QVector<cmd::OffsetAdj> adjs;
    if (src.parentId != 0) {
        auto siblings = m_doc->tree.childrenOf(src.parentId);
        for (int si : siblings) {
            if (si == nodeIdx) continue;
            auto& sib = m_doc->tree.nodes[si];
            if (sib.offset >= copyOffset)
                adjs.append({sib.id, sib.offset, sib.offset + copySize});
        }
    }

    Node n;
    n.kind     = src.kind;
    n.name     = src.name + "_copy";
    n.parentId = src.parentId;
    n.offset   = copyOffset;
    n.id       = m_doc->tree.reserveId();

    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{n, adjs}));
}

void RcxController::showContextMenu(RcxEditor* editor, int line, int nodeIdx,
                                     int subLine, const QPoint& globalPos) {
    auto icon = [](const char* name) { return QIcon(QStringLiteral(":/vsicons/%1").arg(name)); };

    const bool hasNode = nodeIdx >= 0 && nodeIdx < m_doc->tree.nodes.size();

    // Selection policy
    if (hasNode) {
        uint64_t clickedId = m_doc->tree.nodes[nodeIdx].id;
        if (!m_selIds.contains(clickedId)) {
            m_selIds.clear();
            m_selIds.insert(clickedId);
            m_anchorLine = line;
            applySelectionOverlays();
        }
    }

    // Multi-select batch actions at top
    if (hasNode && m_selIds.size() > 1) {
        QMenu menu;
        int count = m_selIds.size();
        QSet<uint64_t> ids = m_selIds;
        menu.addAction(icon("trash.svg"), QString("Delete %1 nodes").arg(count), [this, ids]() {
            QVector<int> indices;
            for (uint64_t id : ids) {
                int idx = m_doc->tree.indexOfId(id);
                if (idx >= 0) indices.append(idx);
            }
            batchRemoveNodes(indices);
        });
        menu.addAction(icon("symbol-structure.svg"), QString("Change type of %1 nodes...").arg(count),
                       [this, ids]() {
            QStringList types;
            for (const auto& e : kKindMeta) types << e.name;
            bool ok;
            QString sel = QInputDialog::getItem(nullptr, "Change Type", "Type:",
                                                types, 0, false, &ok);
            if (ok) {
                QVector<int> indices;
                for (uint64_t id : ids) {
                    int idx = m_doc->tree.indexOfId(id);
                    if (idx >= 0) indices.append(idx);
                }
                batchChangeKind(indices, kindFromString(sel));
            }
        });
        menu.exec(globalPos);
        return;
    }

    QMenu menu;

    // ── Node-specific actions (only when clicking on a node) ──
    if (hasNode) {
        const Node& node = m_doc->tree.nodes[nodeIdx];
        uint64_t nodeId = node.id;
        uint64_t parentId = node.parentId;

        // Quick-convert suggestions for Hex nodes
        bool addedQuickConvert = false;
        if (node.kind == NodeKind::Hex64) {
            menu.addAction(icon("symbol-numeric.svg"), "Change to uint64_t", [this, nodeId]() {
                int ni = m_doc->tree.indexOfId(nodeId);
                if (ni >= 0) changeNodeKind(ni, NodeKind::UInt64);
            });
            menu.addAction(icon("symbol-numeric.svg"), "Change to uint32_t", [this, nodeId]() {
                int ni = m_doc->tree.indexOfId(nodeId);
                if (ni >= 0) changeNodeKind(ni, NodeKind::UInt32);
            });
            addedQuickConvert = true;
        } else if (node.kind == NodeKind::Hex32) {
            menu.addAction(icon("symbol-numeric.svg"), "Change to uint32_t", [this, nodeId]() {
                int ni = m_doc->tree.indexOfId(nodeId);
                if (ni >= 0) changeNodeKind(ni, NodeKind::UInt32);
            });
            addedQuickConvert = true;
        } else if (node.kind == NodeKind::Hex16) {
            menu.addAction(icon("symbol-numeric.svg"), "Change to int16_t", [this, nodeId]() {
                int ni = m_doc->tree.indexOfId(nodeId);
                if (ni >= 0) changeNodeKind(ni, NodeKind::Int16);
            });
            addedQuickConvert = true;
        }
        if (addedQuickConvert)
            menu.addSeparator();

        bool isEditable = node.kind != NodeKind::Struct && node.kind != NodeKind::Array
                          && node.kind != NodeKind::Padding && node.kind != NodeKind::Mat4x4
                          && m_doc->provider->isWritable();
        if (isEditable) {
            menu.addAction(icon("edit.svg"), "Edit &Value\tEnter", [editor, line]() {
                editor->beginInlineEdit(EditTarget::Value, line);
            });
        }

        menu.addAction(icon("rename.svg"), "Re&name\tF2", [editor, line]() {
            editor->beginInlineEdit(EditTarget::Name, line);
        });

        menu.addAction(icon("symbol-structure.svg"), "Change &Type\tT", [editor, line]() {
            editor->beginInlineEdit(EditTarget::Type, line);
        });

        menu.addSeparator();

        menu.addAction(icon("add.svg"), "&Add Field Below\tInsert", [this, parentId]() {
            insertNode(parentId, -1, NodeKind::Hex64, "newField");
        });

        if (node.kind == NodeKind::Struct || node.kind == NodeKind::Array) {
            menu.addAction(icon("diff-added.svg"), "Add &Child", [this, nodeId]() {
                insertNode(nodeId, 0, NodeKind::Hex64, "newField");
            });
            if (node.collapsed) {
                menu.addAction(icon("expand-all.svg"), "&Expand", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId);
                    if (ni >= 0) toggleCollapse(ni);
                });
            } else {
                menu.addAction(icon("collapse-all.svg"), "&Collapse", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId);
                    if (ni >= 0) toggleCollapse(ni);
                });
            }

            // Align Members submenu
            if (node.kind == NodeKind::Struct) {
                int curAlign = m_doc->tree.computeStructAlignment(nodeId);
                auto* alignMenu = menu.addMenu(icon("symbol-ruler.svg"), "Align &Members");
                static const int alignValues[] = {1, 2, 4, 8, 16, 32, 64, 128};
                for (int av : alignValues) {
                    QString label = (av == 1)
                        ? QStringLiteral("1 (packed)")
                        : QString::number(av);
                    auto* act = alignMenu->addAction(label, [this, nodeId, av]() {
                        performRealignment(nodeId, av);
                    });
                    act->setCheckable(true);
                    act->setChecked(av == curAlign);
                }
            }
        }

        menu.addAction(icon("files.svg"), "D&uplicate\tCtrl+D", [this, nodeId]() {
            int ni = m_doc->tree.indexOfId(nodeId);
            if (ni >= 0) duplicateNode(ni);
        });
        menu.addAction(icon("trash.svg"), "&Delete\tDelete", [this, nodeId]() {
            int ni = m_doc->tree.indexOfId(nodeId);
            if (ni >= 0) removeNode(ni);
        });

        menu.addSeparator();

        menu.addAction(icon("link.svg"), "Copy &Address", [this, nodeId]() {
            int ni = m_doc->tree.indexOfId(nodeId);
            if (ni < 0) return;
            uint64_t addr = m_doc->tree.baseAddress + m_doc->tree.computeOffset(ni);
            QApplication::clipboard()->setText(
                QStringLiteral("0x") + QString::number(addr, 16).toUpper());
        });

        menu.addAction(icon("whole-word.svg"), "Copy &Offset", [this, nodeId]() {
            int ni = m_doc->tree.indexOfId(nodeId);
            if (ni < 0) return;
            int off = m_doc->tree.nodes[ni].offset;
            QApplication::clipboard()->setText(
                QStringLiteral("+0x") + QString::number(off, 16).toUpper().rightJustified(4, '0'));
        });

        menu.addSeparator();
    }

    // ── Always-available actions ──

    // Root struct alignment (always available if a root struct exists)
    {
        uint64_t rootStructId = 0;
        for (const auto& n : m_doc->tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                rootStructId = n.id;
                break;
            }
        }
        if (rootStructId != 0) {
            int curAlign = m_doc->tree.computeStructAlignment(rootStructId);
            auto* alignMenu = menu.addMenu(icon("symbol-ruler.svg"), "Align &Members");
            static const int alignValues[] = {1, 2, 4, 8, 16, 32, 64, 128};
            for (int av : alignValues) {
                QString label = (av == 1)
                    ? QStringLiteral("1 (packed)")
                    : QString::number(av);
                auto* act = alignMenu->addAction(label, [this, rootStructId, av]() {
                    performRealignment(rootStructId, av);
                });
                act->setCheckable(true);
                act->setChecked(av == curAlign);
            }
            menu.addSeparator();
        }
    }

    menu.addAction(icon("add.svg"), "Add Hex64 at Root", [this]() {
        uint64_t target = m_viewRootId ? m_viewRootId : 0;
        insertNode(target, -1, NodeKind::Hex64, "newField");
    });
    menu.addAction(icon("symbol-structure.svg"), "Add Struct at Root", [this]() {
        insertNode(0, -1, NodeKind::Struct, "NewClass");
        setViewRootId(0);  // show all so the new struct is visible
    });
    menu.addAction(icon("diff-added.svg"), "Append 128 bytes", [this]() {
        uint64_t target = m_viewRootId ? m_viewRootId : 0;
        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Append 128 bytes"));
        for (int i = 0; i < 16; i++)
            insertNode(target, -1, NodeKind::Hex64,
                       QStringLiteral("field_%1").arg(i));
        m_doc->undoStack.endMacro();
        m_suppressRefresh = false;
        refresh();
    });

    menu.addSeparator();

    menu.addAction(icon("arrow-left.svg"), "Undo", [this]() {
        m_doc->undoStack.undo();
    })->setEnabled(m_doc->undoStack.canUndo());
    menu.addAction(icon("arrow-right.svg"), "Redo", [this]() {
        m_doc->undoStack.redo();
    })->setEnabled(m_doc->undoStack.canRedo());

    menu.addSeparator();

    menu.addAction(icon("clippy.svg"), "Copy All as Text", [editor]() {
        QApplication::clipboard()->setText(editor->scintilla()->text());
    });

    menu.exec(globalPos);
}

void RcxController::batchRemoveNodes(const QVector<int>& nodeIndices) {
    QSet<uint64_t> idSet;
    for (int idx : nodeIndices) {
        if (idx >= 0 && idx < m_doc->tree.nodes.size())
            idSet.insert(m_doc->tree.nodes[idx].id);
    }
    idSet = m_doc->tree.normalizePreferAncestors(idSet);
    if (idSet.isEmpty()) return;

    // Clear selection before delete (prevents stale highlight on shifted lines)
    m_selIds.clear();
    m_anchorLine = -1;

    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QString("Delete %1 nodes").arg(idSet.size()));
    for (uint64_t id : idSet) {
        int idx = m_doc->tree.indexOfId(id);
        if (idx >= 0) removeNode(idx);
    }
    m_doc->undoStack.endMacro();
    m_suppressRefresh = false;
    refresh();
}

void RcxController::batchChangeKind(const QVector<int>& nodeIndices, NodeKind newKind) {
    QSet<uint64_t> idSet;
    for (int idx : nodeIndices) {
        if (idx >= 0 && idx < m_doc->tree.nodes.size())
            idSet.insert(m_doc->tree.nodes[idx].id);
    }
    idSet = m_doc->tree.normalizePreferDescendants(idSet);
    if (idSet.isEmpty()) return;

    // Clear selection before batch change
    m_selIds.clear();
    m_anchorLine = -1;

    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QString("Change type of %1 nodes").arg(idSet.size()));
    for (uint64_t id : idSet) {
        int idx = m_doc->tree.indexOfId(id);
        if (idx >= 0) changeNodeKind(idx, newKind);
    }
    m_doc->undoStack.endMacro();
    m_suppressRefresh = false;
    refresh();
}

void RcxController::handleNodeClick(RcxEditor* source, int line,
                                     uint64_t nodeId,
                                     Qt::KeyboardModifiers mods) {
    bool ctrl  = mods & Qt::ControlModifier;
    bool shift = mods & Qt::ShiftModifier;

    // Compute effective selection ID: footers use nodeId | kFooterIdBit
    auto effectiveId = [this](int ln, uint64_t nid) -> uint64_t {
        if (ln >= 0 && ln < m_lastResult.meta.size() &&
            m_lastResult.meta[ln].lineKind == LineKind::Footer)
            return nid | kFooterIdBit;
        return nid;
    };

    uint64_t selId = effectiveId(line, nodeId);

    if (!ctrl && !shift) {
        m_selIds.clear();
        m_selIds.insert(selId);
        m_anchorLine = line;
    } else if (ctrl && !shift) {
        if (m_selIds.contains(selId))
            m_selIds.remove(selId);
        else
            m_selIds.insert(selId);
        m_anchorLine = line;
    } else if (shift && !ctrl) {
        if (m_anchorLine < 0) {
            m_selIds.clear();
            m_selIds.insert(selId);
            m_anchorLine = line;
        } else {
            m_selIds.clear();
            int from = qMin(m_anchorLine, line);
            int to   = qMax(m_anchorLine, line);
            for (int i = from; i <= to && i < m_lastResult.meta.size(); i++) {
                uint64_t nid = m_lastResult.meta[i].nodeId;
                if (nid != 0 && nid != kCommandRowId) m_selIds.insert(effectiveId(i, nid));
            }
        }
    } else { // Ctrl+Shift
        if (m_anchorLine < 0) {
            m_selIds.insert(selId);
            m_anchorLine = line;
        } else {
            int from = qMin(m_anchorLine, line);
            int to   = qMax(m_anchorLine, line);
            for (int i = from; i <= to && i < m_lastResult.meta.size(); i++) {
                uint64_t nid = m_lastResult.meta[i].nodeId;
                if (nid != 0 && nid != kCommandRowId) m_selIds.insert(effectiveId(i, nid));
            }
        }
    }

    applySelectionOverlays();
    updateCommandRow();

    if (m_selIds.size() == 1) {
        uint64_t sid = *m_selIds.begin();
        // Strip footer bit for node lookup
        int idx = m_doc->tree.indexOfId(sid & ~kFooterIdBit);
        if (idx >= 0) emit nodeSelected(idx);
    }
}

void RcxController::clearSelection() {
    m_selIds.clear();
    m_anchorLine = -1;
    applySelectionOverlays();
    updateCommandRow();
}

void RcxController::applySelectionOverlays() {
    for (auto* editor : m_editors)
        editor->applySelectionOverlay(m_selIds);
}

void RcxController::performRealignment(uint64_t structId, int targetAlign) {
    auto& tree = m_doc->tree;
    int rootIdx = tree.indexOfId(structId);
    if (rootIdx < 0) return;

    // Gather direct children sorted by offset
    QVector<int> kids = tree.childrenOf(structId);
    std::sort(kids.begin(), kids.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });

    // Separate into real nodes (non-Padding) and padding nodes
    struct NodeInfo { uint64_t id; int offset; int size; };
    QVector<NodeInfo> realNodes;
    QVector<uint64_t> padIds;

    for (int ci : kids) {
        const Node& child = tree.nodes[ci];
        int sz = (child.kind == NodeKind::Struct || child.kind == NodeKind::Array)
            ? tree.structSpan(child.id) : child.byteSize();
        if (child.kind == NodeKind::Padding)
            padIds.append(child.id);
        else
            realNodes.append({child.id, child.offset, sz});
    }

    auto roundUp = [](int x, int align) -> int {
        return align <= 1 ? x : ((x + align - 1) / align) * align;
    };

    // Compute new offsets for real nodes
    struct OffChange { uint64_t id; int oldOff; int newOff; };
    QVector<OffChange> offChanges;
    int cursor = 0;
    for (auto& rn : realNodes) {
        int newOff = roundUp(cursor, targetAlign);
        if (newOff != rn.offset)
            offChanges.append({rn.id, rn.offset, newOff});
        rn.offset = newOff;  // update local copy for gap computation
        cursor = newOff + rn.size;
    }

    // Compute where padding is needed (gaps between consecutive nodes)
    struct PadInsert { int offset; int size; };
    QVector<PadInsert> padsNeeded;

    for (int i = 0; i < realNodes.size(); i++) {
        int gapStart = (i == 0) ? 0 : realNodes[i - 1].offset + realNodes[i - 1].size;
        int gapEnd = realNodes[i].offset;
        if (gapEnd > gapStart)
            padsNeeded.append({gapStart, gapEnd - gapStart});
    }

    // Check if anything actually changes
    if (offChanges.isEmpty() && padIds.isEmpty() && padsNeeded.isEmpty())
        return;

    // Apply as undoable macro
    bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Realign to %1").arg(targetAlign));

    // 1. Remove all existing Padding nodes (no offset adjustments — we recompute)
    for (uint64_t pid : padIds) {
        int idx = tree.indexOfId(pid);
        if (idx < 0) continue;
        QVector<Node> subtree;
        subtree.append(tree.nodes[idx]);
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::Remove{pid, subtree, {}}));
    }

    // 2. Reposition real nodes
    for (const auto& oc : offChanges) {
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeOffset{oc.id, oc.oldOff, oc.newOff}));
    }

    // 3. Insert new padding in gaps
    for (const auto& pi : padsNeeded) {
        Node pad;
        pad.kind = NodeKind::Padding;
        pad.parentId = structId;
        pad.offset = pi.offset;
        pad.arrayLen = pi.size;
        pad.id = tree.reserveId();
        m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{pad}));
    }

    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();
}

void RcxController::updateCommandRow() {
    // -- Source label: driven by provider metadata --
    QString src;
    QString provName = m_doc->provider->name();
    if (provName.isEmpty()) {
        src = QStringLiteral("source\u25BE");
    } else {
        src = QStringLiteral("'%1'\u25BE")
            .arg(provName);
    }

    // -- Symbol for selected node (getSymbol integration) --
    QString sym;
    if (m_selIds.size() == 1) {
        uint64_t sid = *m_selIds.begin();
        int idx = m_doc->tree.indexOfId(sid & ~kFooterIdBit);
        if (idx >= 0) {
            const auto& node = m_doc->tree.nodes[idx];
            uint64_t addr = m_doc->tree.baseAddress + m_doc->tree.computeOffset(idx);
            sym = m_doc->provider->getSymbol(addr);
        }
    }

    QString addr = QStringLiteral("0x") +
        QString::number(m_doc->tree.baseAddress, 16).toUpper();

    // Build the row. If we have a symbol, append it after the address.
    QString row;
    if (sym.isEmpty()) {
        row = QStringLiteral("%1 \u00B7 %2")
            .arg(elide(src, 40), elide(addr, 24));
    } else {
        row = QStringLiteral("%1 \u00B7 %2  %3")
            .arg(elide(src, 40), elide(addr, 24), elide(sym, 40));
    }

    // Build row 2: root class type + name
    QString row2;
    for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
        const auto& n = m_doc->tree.nodes[i];
        if (n.parentId == 0 && n.kind == NodeKind::Struct) {
            QString keyword = n.resolvedClassKeyword();
            QString className = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
            row2 = QStringLiteral("%1\u25BE %2 {")
                .arg(keyword, className);
            break;
        }
    }
    if (row2.isEmpty())
        row2 = QStringLiteral("struct\u25BE <no class> {");

    QString combined = row + QStringLiteral(" \u00B7 ") + row2;

    for (auto* ed : m_editors) {
        ed->setCommandRowText(combined);
    }
    emit selectionChanged(m_selIds.size());
}

void RcxController::attachToProcess(uint32_t pid, const QString& processName) {
#ifdef _WIN32
    HANDLE hProc = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION
        | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!hProc) {
        QMessageBox::warning(qobject_cast<QWidget*>(parent()),
            "Attach Failed",
            QString("Could not open process %1 (PID %2).\n"
                    "Try running as administrator.")
                .arg(processName).arg(pid));
        return;
    }

    // Grab main module for initial view region
    HMODULE hMod = nullptr;
    DWORD needed = 0;
    uint64_t base = 0;
    int regionSize = 0x10000;

    if (EnumProcessModulesEx(hProc, &hMod, sizeof(hMod), &needed, LIST_MODULES_ALL)
        && hMod)
    {
        MODULEINFO mi{};
        if (GetModuleInformation(hProc, hMod, &mi, sizeof(mi))) {
            base = (uint64_t)mi.lpBaseOfDll;
            regionSize = (int)mi.SizeOfImage;
        }
    }

    m_doc->undoStack.clear();
    m_doc->provider = std::make_shared<ProcessProvider>(
        hProc, base, regionSize, processName);
    m_doc->dataPath.clear();
    m_doc->tree.baseAddress = base;
    resetSnapshot();
    emit m_doc->documentChanged();
    refresh();
#else
    Q_UNUSED(pid); Q_UNUSED(processName);
#endif
}

void RcxController::switchToSavedSource(int idx) {
    if (idx < 0 || idx >= m_savedSources.size()) return;
    if (idx == m_activeSourceIdx) return;

    // Save current source's base address before switching
    if (m_activeSourceIdx >= 0 && m_activeSourceIdx < m_savedSources.size())
        m_savedSources[m_activeSourceIdx].baseAddress = m_doc->tree.baseAddress;

    m_activeSourceIdx = idx;
    const auto& entry = m_savedSources[idx];

    if (entry.kind == QStringLiteral("File")) {
        m_doc->loadData(entry.filePath);
        m_doc->tree.baseAddress = entry.baseAddress;
        refresh();
    } else if (entry.kind == QStringLiteral("Process")) {
#ifdef _WIN32
        attachToProcess(entry.pid, entry.processName);
#endif
    }
}

void RcxController::pushSavedSourcesToEditors() {
    QVector<SavedSourceDisplay> display;
    display.reserve(m_savedSources.size());
    for (int i = 0; i < m_savedSources.size(); i++) {
        SavedSourceDisplay d;
        d.text = QStringLiteral("%1 '%2'")
            .arg(m_savedSources[i].kind, m_savedSources[i].displayName);
        d.active = (i == m_activeSourceIdx);
        display.append(d);
    }
    for (auto* editor : m_editors)
        editor->setSavedSources(display);
}

// ── Auto-refresh ──

void RcxController::setupAutoRefresh() {
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(2000);
    connect(m_refreshTimer, &QTimer::timeout, this, &RcxController::onRefreshTick);
    m_refreshTimer->start();

    m_refreshWatcher = new QFutureWatcher<QByteArray>(this);
    connect(m_refreshWatcher, &QFutureWatcher<QByteArray>::finished,
            this, &RcxController::onReadComplete);
}

void RcxController::onRefreshTick() {
    if (m_readInFlight) return;
    if (!m_doc->provider || !m_doc->provider->isLive()) return;
    if (m_suppressRefresh) return;
    for (auto* editor : m_editors)
        if (editor->isEditing()) return;

    int extent = computeDataExtent();
    if (extent <= 0) return;

    m_readInFlight = true;
    m_readGen = m_refreshGen;

    // Capture shared_ptr copy — keeps provider alive during async read
    auto prov = m_doc->provider;
    m_refreshWatcher->setFuture(QtConcurrent::run([prov, extent]() -> QByteArray {
        return prov->readBytes(0, extent);
    }));
}

void RcxController::onReadComplete() {
    m_readInFlight = false;

    // Stale read (provider changed while we were reading) — discard
    if (m_readGen != m_refreshGen) return;

    QByteArray newData = m_refreshWatcher->result();

    // Fast path: no changes at all — skip full recompose
    if (!m_prevSnapshot.isEmpty() && m_prevSnapshot.size() == newData.size()
        && memcmp(m_prevSnapshot.constData(), newData.constData(), newData.size()) == 0)
        return;

    // Compute which byte offsets changed
    m_changedOffsets.clear();
    if (!m_prevSnapshot.isEmpty()) {
        int compareLen = qMin(m_prevSnapshot.size(), newData.size());
        const char* oldP = m_prevSnapshot.constData();
        const char* newP = newData.constData();
        for (int i = 0; i < compareLen; i++) {
            if (oldP[i] != newP[i])
                m_changedOffsets.insert(i);
        }
        // Bytes beyond old snapshot are all "new"
        for (int i = compareLen; i < newData.size(); i++)
            m_changedOffsets.insert(i);
    }
    m_prevSnapshot = newData;

    // Update or create snapshot provider
    if (m_snapshotProv)
        m_snapshotProv->updateSnapshot(std::move(newData));
    else
        m_snapshotProv = std::make_unique<SnapshotProvider>(m_doc->provider, std::move(newData));

    refresh();

    // Clear changed offsets after refresh consumed them
    m_changedOffsets.clear();
}

int RcxController::computeDataExtent() const {
    // Use provider size as the extent (for ProcessProvider this is the module/region size)
    int provSize = m_doc->provider->size();
    if (provSize > 0) return provSize;

    // Fallback: walk tree to find maximum byte offset
    int maxEnd = 0;
    for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
        int64_t off = m_doc->tree.computeOffset(i);
        int sz = m_doc->tree.nodes[i].byteSize();
        int end = (int)(off + sz);
        if (end > maxEnd) maxEnd = end;
    }
    return maxEnd;
}

void RcxController::resetSnapshot() {
    m_refreshGen++;
    m_readInFlight = false;
    m_snapshotProv.reset();
    m_prevSnapshot.clear();
    m_changedOffsets.clear();
}

void RcxController::handleMarginClick(RcxEditor* editor, int margin,
                                       int line, Qt::KeyboardModifiers) {
    const LineMeta* lm = editor->metaForLine(line);
    if (!lm) return;

    if (lm->foldHead && (margin == 0 || margin == 1)) {
        toggleCollapse(lm->nodeIdx);
    } else if (margin == 0 || margin == 1) {
        emit nodeSelected(lm->nodeIdx);
    }
}

void RcxController::setEditorFont(const QString& fontName) {
    for (auto* editor : m_editors)
        editor->setEditorFont(fontName);
}

} // namespace rcx
