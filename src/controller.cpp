#include "controller.h"
#include "addressparser.h"
#include "symbolstore.h"
#include "typeselectorpopup.h"
#include "hextoolbarpopup.h"
#include <cmath>
#include <cstring>
#include "providerregistry.h"
#include "themes/thememanager.h"
#include <Qsci/qsciscintilla.h>
#include <QSplitter>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMenu>
#include <QInputDialog>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QClipboard>
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>
#include <limits>

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

// Themed comment input dialog matching the editor style
static QString showCommentDialog(QWidget* parent, const QString& title,
                                 const QString& existing, bool* ok) {
    *ok = false;
    const auto& theme = ThemeManager::instance().current();
    QSettings settings("Reclass", "Reclass");
    QFont editorFont(settings.value("font", "JetBrains Mono").toString(), 12);
    editorFont.setFixedPitch(true);

    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.setMinimumWidth(380);

    QPalette pal = dlg.palette();
    pal.setColor(QPalette::Window, theme.background);
    pal.setColor(QPalette::WindowText, theme.text);
    pal.setColor(QPalette::Base, theme.backgroundAlt);
    pal.setColor(QPalette::Text, theme.text);
    dlg.setPalette(pal);
    dlg.setAutoFillBackground(true);

    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);

    auto* label = new QLabel(QStringLiteral("Comment:"), &dlg);
    label->setFont(editorFont);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(theme.textDim.name()));
    layout->addWidget(label);

    auto* input = new QLineEdit(&dlg);
    input->setText(existing);
    input->setFont(editorFont);
    input->selectAll();
    input->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3;"
        " padding: 6px 8px; selection-background-color: %4; }")
        .arg(theme.backgroundAlt.name(), theme.text.name(),
             theme.border.name(), theme.selected.name()));
    layout->addWidget(input);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Save"));
    QString btnStyle = QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3;"
        " padding: 4px 16px; border-radius: 3px; font-family: '%5'; font-size: 11px; }"
        "QPushButton:hover { background: %4; }")
        .arg(theme.background.name(), theme.text.name(), theme.border.name(),
             theme.hover.name(), editorFont.family());
    buttons->setStyleSheet(btnStyle);
    layout->addWidget(buttons);

    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    input->setFocus();
    if (dlg.exec() == QDialog::Accepted) {
        *ok = true;
        return input->text();
    }
    return {};
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
        parts = QStringList{parts.front(), QStringLiteral("\u2026"), parts[parts.size() - 2], parts.back()};
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

ComposeResult RcxDocument::compose(uint64_t viewRootId, bool compactColumns,
                                   bool treeLines, bool braceWrap, bool typeHints,
                                   bool showComments,
                                   SymbolLookupFn symbolLookup) const {
    return rcx::compose(tree, *provider, viewRootId, compactColumns, treeLines, braceWrap, typeHints,
                        showComments, std::move(symbolLookup));
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

    // Hex toolbar: no longer auto-shows (replaced by type-cycling tooltip).
    // Still available via context menu for insert/join/fill operations.
    connect(this, &RcxController::nodeSelected, this, [this](int /*nodeIdx*/) {
        hideHexToolbar();
    });
}

RcxController::~RcxController() {
    if (m_refreshWatcher) {
        m_refreshWatcher->cancel();
        m_refreshWatcher->waitForFinished();
    }

    m_snapshotProv.reset();
}

void RcxController::resetProvider() {
    m_snapshotProv.reset();
}

RcxEditor* RcxController::primaryEditor() const {
    return m_editors.isEmpty() ? nullptr : m_editors.first();
}

RcxEditor* RcxController::addSplitEditor(QWidget* parent) {
    auto* editor = new RcxEditor(parent);
    m_editors.append(editor);
    connectEditor(editor);

    if (!m_lastResult.text.isEmpty()) {
        editor->applyDocument(m_lastResult);
    }
    updateCommandRow();

    // Eagerly pre-warm the type popup so first click isn't slow (~350ms cold start).
    if (!m_cachedPopup) {
        QPointer<RcxEditor> safeEditor = editor;
        QTimer::singleShot(0, this, [this, safeEditor]() {
            if (!m_cachedPopup && !m_editors.isEmpty() && safeEditor)
                ensurePopup(safeEditor);
        });
    }
    return editor;
}

void RcxController::removeSplitEditor(RcxEditor* editor) {
    m_editors.removeOne(editor);
    editor->disconnect(this);
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
    connect(editor, &RcxEditor::keywordConvertRequested,
            this, &RcxController::convertRootKeyword);
    connect(editor, &RcxEditor::nodeClicked,
            this, [this, editor](int line, uint64_t nodeId, Qt::KeyboardModifiers mods) {
        handleNodeClick(editor, line, nodeId, mods);
    });

    // Type selector popup (command row chevron)
    connect(editor, &RcxEditor::typeSelectorRequested,
            this, [this, editor]() {
        showTypePopup(editor, TypePopupMode::Root, -1, QPoint());
    });

    // Type picker popup (array element type / pointer target)
    connect(editor, &RcxEditor::typePickerRequested,
            this, [this, editor](EditTarget target, int nodeIdx, QPoint globalPos) {
        TypePopupMode mode = TypePopupMode::FieldType;
        if (target == EditTarget::ArrayElementType)
            mode = TypePopupMode::ArrayElement;
        // PointerTarget is handled as FieldType — modifiers * / ** will be pre-selected
        showTypePopup(editor, mode, nodeIdx, globalPos);
    });

    // Delete key shortcut
    connect(editor, &RcxEditor::deleteSelectedRequested,
            this, [this]() {
        QSet<uint64_t> ids = m_selIds;
        QVector<int> indices;
        for (uint64_t id : ids) {
            int idx = m_doc->tree.indexOfId(
                id & ~(kFooterIdBit | kArrayElemBit | kArrayElemMask
                       | kMemberBit | kMemberSubMask));
            if (idx >= 0) indices.append(idx);
        }
        if (indices.size() > 1)
            batchRemoveNodes(indices);
        else if (indices.size() == 1)
            removeNode(indices.first());
    });

    // Ctrl+D duplicate shortcut
    connect(editor, &RcxEditor::duplicateSelectedRequested,
            this, [this]() {
        QSet<uint64_t> ids = m_selIds;
        for (uint64_t id : ids) {
            int idx = m_doc->tree.indexOfId(
                id & ~(kFooterIdBit | kArrayElemBit | kArrayElemMask
                       | kMemberBit | kMemberSubMask));
            if (idx >= 0) duplicateNode(idx);
        }
    });

    // Quick type change (Space, 1-5, P, F, S, U keys)
    connect(editor, &RcxEditor::quickTypeChangeRequested,
            this, [this](int nodeIdx, NodeKind targetKind) {
        if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
        const auto& node = m_doc->tree.nodes[nodeIdx];
        if (isHexNode(targetKind) && isHexNode(node.kind)) {
            int curSz = sizeForKind(node.kind);
            int tgtSz = sizeForKind(targetKind);
            if (tgtSz <= curSz)
                changeNodeKind(nodeIdx, targetKind);
            else
                joinHexNodes(node.id, targetKind);
        } else {
            changeNodeKind(nodeIdx, targetKind);
        }
    });

    // Left/Right arrow: cycle through same-size type variants
    connect(editor, &RcxEditor::cycleSameSizeTypeRequested,
            this, [this](int nodeIdx, int direction) {
        if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
        NodeKind cur = m_doc->tree.nodes[nodeIdx].kind;
        int sz = sizeForKind(cur);
        // Build list of same-size types
        QVector<NodeKind> variants;
        for (const auto& m : kKindMeta) {
            if (m.size == sz && m.kind != NodeKind::Struct && m.kind != NodeKind::Array)
                variants.append(m.kind);
        }
        if (variants.size() <= 1) return;
        int idx = variants.indexOf(cur);
        if (idx < 0) return;
        int next = (idx + direction + variants.size()) % variants.size();
        changeNodeKind(nodeIdx, variants[next]);
    });

    // Insert key shortcut
    connect(editor, &RcxEditor::insertAboveRequested,
            this, [this](int nodeIdx, NodeKind kind) {
        if (nodeIdx >= 0)
            insertNodeAbove(nodeIdx, kind, QStringLiteral("field"));
        else {
            uint64_t target = m_viewRootId ? m_viewRootId : 0;
            insertNode(target, -1, kind, QStringLiteral("field"));
        }
    });

    // Ctrl+Shift+Up/Down: reorder field among siblings
    connect(editor, &RcxEditor::moveNodeRequested,
            this, [this](int nodeIdx, int direction) {
        if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
        const Node& node = m_doc->tree.nodes[nodeIdx];
        auto siblings = m_doc->tree.childrenOf(node.parentId);
        // Sort siblings by offset
        std::sort(siblings.begin(), siblings.end(), [&](int a, int b) {
            return m_doc->tree.nodes[a].offset < m_doc->tree.nodes[b].offset;
        });
        int pos = siblings.indexOf(nodeIdx);
        if (pos < 0) return;
        int swapPos = pos + direction;
        if (swapPos < 0 || swapPos >= siblings.size()) return;
        int swapIdx = siblings[swapPos];
        // Swap offsets
        int offA = m_doc->tree.nodes[nodeIdx].offset;
        int offB = m_doc->tree.nodes[swapIdx].offset;
        m_doc->undoStack.beginMacro(QStringLiteral("Reorder field"));
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeOffset{node.id, offA, offB}));
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeOffset{m_doc->tree.nodes[swapIdx].id, offB, offA}));
        m_doc->undoStack.endMacro();
    });

    // Collapse all / Expand all (Ctrl+Shift+[ / ])
    connect(editor, &RcxEditor::collapseAllRequested, this, [this]() {
        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Collapse all"));
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            auto& n = m_doc->tree.nodes[i];
            if (isContainerKind(n.kind) && !n.collapsed)
                m_doc->undoStack.push(new RcxCommand(this, cmd::Collapse{n.id, false, true}));
        }
        m_doc->undoStack.endMacro();
        m_suppressRefresh = false;
        refresh();
    });
    connect(editor, &RcxEditor::expandAllRequested, this, [this]() {
        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Expand all"));
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            auto& n = m_doc->tree.nodes[i];
            if (isContainerKind(n.kind) && n.collapsed)
                m_doc->undoStack.push(new RcxCommand(this, cmd::Collapse{n.id, true, false}));
        }
        m_doc->undoStack.endMacro();
        m_suppressRefresh = false;
        refresh();
    });

    // Comment edit (';' key) — respects selection
    connect(editor, &RcxEditor::commentEditRequested,
            this, [this, editor]() {
        if (!m_showComments) return;
        QSet<uint64_t> ids = m_selIds;
        // Strip footer/array/member bits to get real node IDs
        QSet<uint64_t> nodeIds;
        for (uint64_t id : ids) {
            uint64_t nid = id & ~(kFooterIdBit | kArrayElemBit | kArrayElemMask
                                  | kMemberBit | kMemberSubMask);
            if (m_doc->tree.indexOfId(nid) >= 0)
                nodeIds.insert(nid);
        }

        if (nodeIds.size() <= 1) {
            // Single selection (or empty): find the selected node's first line and edit inline
            uint64_t targetId = nodeIds.isEmpty() ? 0 : *nodeIds.begin();
            if (targetId == 0) {
                // Nothing selected — use cursor position
                editor->beginInlineEdit(EditTarget::Comment);
                return;
            }
            // Find the display line for this node
            for (int i = 0; i < m_lastResult.meta.size(); i++) {
                const auto& lm = m_lastResult.meta[i];
                if (lm.nodeId == targetId && lm.lineKind == LineKind::Field
                    && !lm.isContinuation && !lm.isMemberLine) {
                    editor->beginInlineEdit(EditTarget::Comment, i);
                    return;
                }
            }
            // Fallback: try cursor position
            editor->beginInlineEdit(EditTarget::Comment);
        } else {
            // Multi-selection: prompt for comment text and apply to all
            // Gather existing comment from first selected node as default
            QString existingComment;
            for (uint64_t nid : nodeIds) {
                int idx = m_doc->tree.indexOfId(nid);
                if (idx >= 0 && !m_doc->tree.nodes[idx].comment.isEmpty()) {
                    existingComment = m_doc->tree.nodes[idx].comment;
                    break;
                }
            }
            bool ok = false;
            QString text = showCommentDialog(
                qobject_cast<QWidget*>(parent()),
                QStringLiteral("Comment %1 nodes").arg(nodeIds.size()),
                existingComment, &ok);
            if (!ok) return;
            QString comment = text.trimmed();

            m_suppressRefresh = true;
            m_doc->undoStack.beginMacro(
                QStringLiteral("Comment %1 nodes").arg(nodeIds.size()));
            for (uint64_t nid : nodeIds) {
                int idx = m_doc->tree.indexOfId(nid);
                if (idx < 0) continue;
                const Node& node = m_doc->tree.nodes[idx];
                if (node.comment != comment) {
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::ChangeComment{nid, node.comment, comment}));
                }
            }
            m_doc->undoStack.endMacro();
            m_suppressRefresh = false;
            refresh();
        }
    });

    // Footer "+1024" button
    connect(editor, &RcxEditor::appendBytesRequested,
            this, [this](uint64_t structId, int byteCount) {
        // If this is an embedded struct with refId (virtual children),
        // append to the referenced root class definition instead
        uint64_t targetId = structId;
        int si = m_doc->tree.indexOfId(structId);
        if (si >= 0 && m_doc->tree.childrenOf(structId).isEmpty()
            && m_doc->tree.nodes[si].refId != 0)
            targetId = m_doc->tree.nodes[si].refId;
        int hex64Count = byteCount / 8;
        int remainBytes = byteCount % 8;
        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Append %1 bytes").arg(byteCount));
        for (int i = 0; i < hex64Count; i++)
            insertNode(targetId, -1, NodeKind::Hex64,
                       QStringLiteral("field_%1").arg(i));
        for (int i = 0; i < remainBytes; i++)
            insertNode(targetId, -1, NodeKind::Hex8,
                       QStringLiteral("field_%1").arg(hex64Count + i));
        m_doc->undoStack.endMacro();
        m_suppressRefresh = false;
        refresh();
    });

    // Footer "Trim" button — remove trailing hex nodes from end of struct
    connect(editor, &RcxEditor::trimHexRequested,
            this, [this](uint64_t structId) {
        // Unions don't have trailing padding — all members overlap at offset 0
        int si = m_doc->tree.indexOfId(structId);
        if (si < 0) return;
        if (m_doc->tree.nodes[si].classKeyword == QStringLiteral("union"))
            return;
        // If this is an embedded struct with refId (virtual children),
        // operate on the referenced root class definition instead
        uint64_t targetId = structId;
        QVector<int> children = m_doc->tree.childrenOf(structId);
        if (children.isEmpty() && m_doc->tree.nodes[si].refId != 0) {
            targetId = m_doc->tree.nodes[si].refId;
            children = m_doc->tree.childrenOf(targetId);
        }
        if (children.isEmpty()) return;

        // Sort by offset descending to find trailing hex nodes
        std::sort(children.begin(), children.end(), [&](int a, int b) {
            return m_doc->tree.nodes[a].offset > m_doc->tree.nodes[b].offset;
        });

        // Collect trailing hex nodes to remove
        QVector<int> toRemove;
        for (int ci : children) {
            const Node& n = m_doc->tree.nodes[ci];
            if (!isHexNode(n.kind)) break;
            toRemove.append(ci);
        }
        if (toRemove.isEmpty()) return;

        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Trim %1 trailing hex nodes").arg(toRemove.size()));
        for (int ni : toRemove)
            removeNode(ni);
        m_doc->undoStack.endMacro();
        m_suppressRefresh = false;
        refresh();
    });

    // Footer "+10" button — append enum members sequentially from highest value
    connect(editor, &RcxEditor::appendEnumMembersRequested,
            this, [this](uint64_t enumId, int count) {
        int ni = m_doc->tree.indexOfId(enumId);
        if (ni < 0) return;
        auto members = m_doc->tree.nodes[ni].enumMembers;
        int64_t nextVal = members.isEmpty() ? 0 : members.last().second + 1;
        auto oldMembers = members;
        for (int i = 0; i < count; i++)
            members.emplaceBack(QStringLiteral("Member%1").arg(nextVal + i), nextVal + i);
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeEnumMembers{enumId, oldMembers, members}));
    });

    // Live expression evaluation for BaseAddress editing
    editor->setExprEvaluator([this](const QString& text) -> QString {
        QString s = text.trimmed();
        s.remove('`');
        s.remove('\'');
        if (s.isEmpty()) return {};
        AddressParserCallbacks cbs;
        if (m_doc->provider) {
            auto* prov = m_doc->provider.get();
            cbs.resolveModule = [prov](const QString& name, bool* ok) -> uint64_t {
                uint64_t base = prov->symbolToAddress(name);
                *ok = (base != 0);
                return base;
            };
            int ptrSz = m_doc->tree.pointerSize;
            cbs.readPointer = [prov, ptrSz](uint64_t addr, bool* ok) -> uint64_t {
                uint64_t val = 0;
                *ok = prov->read(addr, &val, ptrSz);
                return val;
            };
            cbs.resolveIdentifier = [prov](const QString& name, bool* ok) -> uint64_t {
                return SymbolStore::instance().resolve(name, prov, ok);
            };
        }
        auto result = AddressParser::evaluate(s, m_doc->tree.pointerSize, &cbs);
        if (!result.ok) return {};
        return QStringLiteral("0x") + QString::number(result.value, 16).toUpper();
    });

    // Inline editing signals
    connect(editor, &RcxEditor::inlineEditCommitted,
            this, [this](int nodeIdx, int subLine, EditTarget target, const QString& text,
                         uint64_t resolvedAddr) {
        // CommandRow BaseAddress/Source/RootClass edit has nodeIdx=-1
        if (nodeIdx < 0 && target != EditTarget::BaseAddress && target != EditTarget::Source
            && target != EditTarget::RootClassType && target != EditTarget::RootClassName) { refresh(); return; }
        switch (target) {
        case EditTarget::Name: {
            if (text.isEmpty()) break;
            if (nodeIdx >= m_doc->tree.nodes.size()) break;
            const Node& node = m_doc->tree.nodes[nodeIdx];
            // Enum member name edit
            if (node.isEnum()
                && subLine >= 0 && subLine < node.enumMembers.size()) {
                auto members = node.enumMembers;
                members[subLine].first = text;
                m_doc->undoStack.push(new RcxCommand(this,
                    cmd::ChangeEnumMembers{node.id, node.enumMembers, members}));
                break;
            }
            // ASCII edit on Hex nodes
            if (isHexPreview(node.kind)) {
                setNodeValue(nodeIdx, subLine, text, /*isAscii=*/true, resolvedAddr);
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
                        const uint64_t nodeId = m_doc->tree.nodes[nodeIdx].id;
                        bool wasSuppressed = m_suppressRefresh;
                        m_suppressRefresh = true;
                        m_doc->undoStack.beginMacro(QStringLiteral("Change to array"));
                        if (m_doc->tree.nodes[nodeIdx].kind != NodeKind::Array)
                            changeNodeKind(nodeIdx, NodeKind::Array);
                        int idx = m_doc->tree.indexOfId(nodeId);
                        if (idx >= 0) {
                            auto& n = m_doc->tree.nodes[idx];
                            if (n.elementKind != elemKind || n.arrayLen != newCount)
                                m_doc->undoStack.push(new RcxCommand(this,
                                    cmd::ChangeArrayMeta{nodeId, n.elementKind, elemKind,
                                                         n.arrayLen, newCount}));
                        }
                        m_doc->undoStack.endMacro();
                        m_suppressRefresh = wasSuppressed;
                        if (!m_suppressRefresh) refresh();
                    }
                }
            } else {
                // Regular type change
                bool ok;
                NodeKind k = kindFromTypeName(text, &ok);
                if (ok && k != NodeKind::Struct && k != NodeKind::Array) {
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
        case EditTarget::Value: {
            // Enum member value edit
            if (nodeIdx >= 0 && nodeIdx < m_doc->tree.nodes.size()) {
                const Node& node = m_doc->tree.nodes[nodeIdx];
                if (node.isEnum()
                    && subLine >= 0 && subLine < node.enumMembers.size()) {
                    bool ok;
                    int64_t val = text.toLongLong(&ok);
                    if (!ok) val = text.toLongLong(&ok, 16);
                    if (ok) {
                        auto members = node.enumMembers;
                        members[subLine].second = val;
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ChangeEnumMembers{node.id, node.enumMembers, members}));
                    }
                    break;
                }
            }
            setNodeValue(nodeIdx, subLine, text, /*isAscii=*/false, resolvedAddr);
            break;
        }
        case EditTarget::BaseAddress: {
            QString s = text.trimmed();
            s.remove('`');          // WinDbg backtick separators (e.g. 7ff6`6cce0000)
            s.remove('\n');
            s.remove('\r');

            AddressParserCallbacks cbs;
            if (m_doc->provider) {
                auto* prov = m_doc->provider.get();
                cbs.resolveModule = [prov](const QString& name, bool* ok) -> uint64_t {
                    uint64_t base = prov->symbolToAddress(name);
                    *ok = (base != 0);
                    return base;
                };
                int ptrSz = m_doc->tree.pointerSize;
                cbs.readPointer = [prov, ptrSz](uint64_t addr, bool* ok) -> uint64_t {
                    uint64_t val = 0;
                    *ok = prov->read(addr, &val, ptrSz);
                    return val;
                };
                cbs.resolveIdentifier = [prov](const QString& name, bool* ok) -> uint64_t {
                    return SymbolStore::instance().resolve(name, prov, ok);
                };
                // Wire kernel paging callbacks if provider supports it
                if (prov->hasKernelPaging()) {
                    cbs.vtop = [prov](uint32_t pid, uint64_t va, bool* ok) -> uint64_t {
                        Q_UNUSED(pid);
                        auto r = prov->translateAddress(va);
                        *ok = r.valid;
                        return r.physical;
                    };
                    cbs.cr3 = [prov](uint32_t pid, bool* ok) -> uint64_t {
                        Q_UNUSED(pid);
                        uint64_t cr3 = prov->getCr3();
                        *ok = (cr3 != 0);
                        return cr3;
                    };
                    cbs.physRead = [prov](uint64_t physAddr, bool* ok) -> uint64_t {
                        auto entries = prov->readPageTable(physAddr, 0, 1);
                        *ok = !entries.isEmpty();
                        return entries.isEmpty() ? 0 : entries[0];
                    };
                }
            }
            auto result = AddressParser::evaluate(s, m_doc->tree.pointerSize, &cbs);
            if (result.ok && result.value != m_doc->tree.baseAddress) {
                uint64_t oldBase = m_doc->tree.baseAddress;
                QString oldFormula = m_doc->tree.baseAddressFormula;
                // Store formula if input uses module/deref/kernel-function/symbol syntax
                static const QRegularExpression formulaRx(
                    QStringLiteral("[<\\[]|\\b(?:vtop|cr3|phys)\\s*\\(|\\w+!\\w+"));
                QString newFormula = formulaRx.match(s).hasMatch() ? s : QString();
                m_doc->undoStack.push(new RcxCommand(this,
                    cmd::ChangeBase{oldBase, result.value, oldFormula, newFormula}));
            }
            break;
        }
        case EditTarget::Source:
            selectSource(text);
            break;
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
            uint64_t targetId = m_viewRootId;
            if (targetId == 0) {
                for (const auto& n : m_doc->tree.nodes) {
                    if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                        targetId = n.id;
                        break;
                    }
                }
            }
            if (targetId != 0) {
                int idx = m_doc->tree.indexOfId(targetId);
                if (idx >= 0) {
                    QString oldKw = m_doc->tree.nodes[idx].resolvedClassKeyword();
                    if (oldKw != kw) {
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ChangeClassKeyword{targetId, oldKw, kw}));
                    }
                }
            }
            break;
        }
        case EditTarget::RootClassName: {
            // Rename the viewed root struct's structTypeName
            if (!text.isEmpty()) {
                uint64_t targetId = m_viewRootId;
                if (targetId == 0) {
                    for (const auto& n : m_doc->tree.nodes) {
                        if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                            targetId = n.id;
                            break;
                        }
                    }
                }
                if (targetId != 0) {
                    int idx = m_doc->tree.indexOfId(targetId);
                    if (idx >= 0) {
                        QString oldName = m_doc->tree.nodes[idx].structTypeName;
                        if (oldName != text) {
                            m_doc->undoStack.push(new RcxCommand(this,
                                cmd::ChangeStructTypeName{targetId, oldName, text}));
                        }
                    }
                }
            }
            break;
        }
        case EditTarget::StaticExpr: {
            if (nodeIdx >= 0 && nodeIdx < m_doc->tree.nodes.size()) {
                const Node& node = m_doc->tree.nodes[nodeIdx];
                if (node.isStatic && text != node.offsetExpr) {
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::ChangeOffsetExpr{node.id, node.offsetExpr, text}));
                }
            }
            break;
        }
        case EditTarget::Comment: {
            if (nodeIdx >= 0 && nodeIdx < m_doc->tree.nodes.size()) {
                const Node& node = m_doc->tree.nodes[nodeIdx];
                QString newComment = text.trimmed();
                if (newComment != node.comment) {
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::ChangeComment{node.id, node.comment, newComment}));
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

void RcxController::setTrackValues(bool on) {
    m_trackValues = on;
    if (!on) {
        m_valueHistory.clear();
        m_lastValueAddr.clear();
        for (auto& lm : m_lastResult.meta)
            lm.heatLevel = 0;
        refresh();
    }
}

void RcxController::resetChangeTracking() {
    m_changedOffsets.clear();
    m_valueHistory.clear();
    m_lastValueAddr.clear();
    m_prevPages.clear();
    m_valueTrackCooldown = 5; // suppress tracking for ~1s
    for (auto& lm : m_lastResult.meta)
        lm.heatLevel = 0;
}

void RcxController::refresh() {
    // Bracket compose with thread-local doc pointer for type name resolution
    s_composeDoc = m_doc;

    // Build symbol lookup callback if PDB symbols are loaded
    SymbolLookupFn symLookup;
    if (SymbolStore::instance().hasSymbols() && m_doc->provider) {
        auto* prov = m_doc->provider.get();
        symLookup = [prov](uint64_t addr) -> QString {
            return SymbolStore::instance().getSymbolForAddress(addr, prov);
        };
    }

    // Compose against snapshot provider if active, otherwise real provider
    if (m_snapshotProv)
        m_lastResult = rcx::compose(m_doc->tree, *m_snapshotProv, m_viewRootId, m_compactColumns, m_treeLines, m_braceWrap, m_typeHints, m_showComments, symLookup);
    else
        m_lastResult = m_doc->compose(m_viewRootId, m_compactColumns, m_treeLines, m_braceWrap, m_typeHints, m_showComments, symLookup);

    s_composeDoc = nullptr;

    // Mark lines whose node data changed since last refresh
    if (!m_changedOffsets.isEmpty()) {
        for (auto& lm : m_lastResult.meta) {
            if (lm.nodeIdx < 0 || lm.nodeIdx >= m_doc->tree.nodes.size()) continue;
            int64_t offset = m_doc->tree.computeOffset(lm.nodeIdx);
            if (offset < 0) continue;
            const Node& node = m_doc->tree.nodes[lm.nodeIdx];

            if (isHexPreview(node.kind)) {
                // Per-byte tracking for hex preview nodes
                int lineOff = 0;
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

    // Update value history and compute heat levels
    // Only run when a live provider is attached (not for static file/buffer sources)
    {
        const Provider* prov = nullptr;
        if (m_snapshotProv && m_snapshotProv->isLive())
            prov = m_snapshotProv.get();
        else if (m_doc->provider && m_doc->provider->isValid() && m_doc->provider->isLive())
            prov = m_doc->provider.get();

        if (m_valueTrackCooldown > 0) --m_valueTrackCooldown;
        if (m_trackValues && prov && m_valueTrackCooldown <= 0) {
            for (auto& lm : m_lastResult.meta) {
                if (lm.nodeIdx < 0 || lm.nodeIdx >= m_doc->tree.nodes.size()) continue;
                if (isSyntheticLine(lm) || lm.isContinuation) continue;
                if (lm.lineKind != LineKind::Field) continue;

                const Node& node = m_doc->tree.nodes[lm.nodeIdx];
                // Skip containers — they don't have scalar values
                if (node.kind == NodeKind::Struct || node.kind == NodeKind::Array) continue;
                // Skip FuncPtr nodes — vtable entries don't change; tracking them
                // causes false heatmap and popup fighting with the disasm popup.
                if (isFuncPtr(node.kind)) continue;

                // Use the absolute address from compose (correct for pointer-expanded nodes)
                uint64_t addr = lm.offsetAddr;
                int sz = node.byteSize();
                if (sz <= 0 || !prov->isReadable(addr, sz)) continue;

                QString val = fmt::readValue(node, *prov, addr, lm.subLine);
                if (!val.isEmpty()) {
                    // Clear stale history if this node's effective address changed
                    // (e.g. viewRoot switch, pointer expand/collapse, MCP restructure)
                    auto addrIt = m_lastValueAddr.find(lm.nodeId);
                    if (addrIt != m_lastValueAddr.end() && addrIt.value() != addr)
                        m_valueHistory.remove(lm.nodeId);
                    m_lastValueAddr[lm.nodeId] = addr;
                    m_valueHistory[lm.nodeId].record(val);
                    lm.heatLevel = m_valueHistory[lm.nodeId].heatLevel();
                }
            }
        }
    }

    // Prune stale selections (nodes removed by undo/redo/delete)
    QSet<uint64_t> valid;
    for (uint64_t id : m_selIds) {
        uint64_t nodeId = id & ~(kFooterIdBit | kArrayElemBit | kArrayElemMask
                                  | kMemberBit | kMemberSubMask);
        if (m_doc->tree.indexOfId(nodeId) >= 0)
            valid.insert(id);  // Keep original ID (with footer/array/member bits if present)
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

    // Resolve providers for disasm popup:
    // - snapProv: snapshot or real — for reading pointer values within the tree
    // - realProv: always the real process provider — for reading code at arbitrary addresses
    const Provider* snapProv = m_snapshotProv
        ? static_cast<const Provider*>(m_snapshotProv.get())
        : (m_doc->provider ? m_doc->provider.get() : nullptr);
    const Provider* realProv = m_doc->provider ? m_doc->provider.get() : nullptr;

    for (auto* editor : m_editors) {
        editor->setCustomTypeNames(customTypes);
        editor->setValueHistoryRef(&m_valueHistory);
        editor->setProviderRef(snapProv, realProv, &m_doc->tree);
        ViewState vs = editor->saveViewState();
        editor->applyDocument(m_lastResult);
        editor->restoreViewState(vs);
    }
    // Text-modifying passes first (command row replaces line 0 text),
    // then overlays last so hover indicators survive the refresh.
    pushSavedSourcesToEditors();
    updateCommandRow();
    applySelectionOverlays();
}

void RcxController::convertRootKeyword(const QString& newKeyword) {
    uint64_t targetId = m_viewRootId;
    if (targetId == 0) {
        for (const auto& n : m_doc->tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                targetId = n.id;
                break;
            }
        }
    }
    if (targetId == 0) return;
    int idx = m_doc->tree.indexOfId(targetId);
    if (idx < 0) return;
    QString oldKw = m_doc->tree.nodes[idx].resolvedClassKeyword();
    if (oldKw == newKeyword) return;
    // Only allow class↔struct conversion
    if (oldKw == QStringLiteral("enum") || newKeyword == QStringLiteral("enum")) return;
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::ChangeClassKeyword{targetId, oldKw, newKeyword}));
}

void RcxController::changeNodeKind(int nodeIdx, NodeKind newKind) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    auto& node = m_doc->tree.nodes[nodeIdx];

    int oldSize = node.byteSize();
    // For containers, byteSize() returns 0 — use structSpan for the real footprint
    if (oldSize == 0 && (node.kind == NodeKind::Struct || node.kind == NodeKind::Array))
        oldSize = m_doc->tree.structSpan(node.id);
    // Compute what byteSize() would be with the new kind
    Node tmp = node;
    tmp.kind = newKind;
    int newSize = tmp.byteSize();

    // When converting TO a container (Struct/Array), the final size depends on
    // refId/arrayMeta set by follow-up commands. Don't pad or shift here —
    // applyTypePopupResult's post-mutation block handles size adjustments.
    if (newKind == NodeKind::Struct || newKind == NodeKind::Array)
        newSize = 0;

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

        // Hex nodes don't display names (ASCII preview instead), so the stored
        // name may be empty or stale.  Give it a sensible default.
        if (isHexNode(node.kind) && !isHexNode(newKind)) {
            QString autoName = QStringLiteral("field_%1")
                .arg(node.offset, 4, 16, QChar('0'));
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::Rename{node.id, node.name, autoName}));
        }

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
                    adjs.push_back(cmd::OffsetAdj{sib.id, sib.offset, sib.offset + delta});
            }
        }
        bool needsRename = isHexNode(node.kind) && !isHexNode(newKind);
        if (needsRename) {
            m_doc->undoStack.beginMacro(QStringLiteral("Change type"));
        }
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeKind{node.id, node.kind, newKind, adjs}));
        if (needsRename) {
            QString autoName = QStringLiteral("field_%1")
                .arg(node.offset, 4, 16, QChar('0'));
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::Rename{node.id, node.name, autoName}));
            m_doc->undoStack.endMacro();
        }
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

void RcxController::insertNodeAbove(int beforeIdx, NodeKind kind, const QString& name) {
    if (beforeIdx < 0 || beforeIdx >= m_doc->tree.nodes.size()) return;
    const Node& before = m_doc->tree.nodes[beforeIdx];

    Node n;
    n.kind     = kind;
    n.name     = name;
    n.parentId = before.parentId;
    n.offset   = before.offset;
    n.id       = m_doc->tree.reserveId();

    int insertSize = sizeForKind(kind);

    // Shift siblings at or after the insertion offset down
    QVector<cmd::OffsetAdj> adjs;
    auto siblings = m_doc->tree.childrenOf(before.parentId);
    for (int si : siblings) {
        auto& sib = m_doc->tree.nodes[si];
        if (sib.offset >= before.offset)
            adjs.push_back(cmd::OffsetAdj{sib.id, sib.offset, sib.offset + insertSize});
    }

    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{n, adjs}));
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
                adjs.push_back(cmd::OffsetAdj{sib.id, sib.offset, sib.offset - deletedSize});
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

void RcxController::deleteRootStruct(uint64_t structId) {
    int ni = m_doc->tree.indexOfId(structId);
    if (ni < 0) return;
    const Node& node = m_doc->tree.nodes[ni];
    if (node.parentId != 0 || node.kind != NodeKind::Struct) return;

    bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Delete root struct"));

    // Clear all refId references pointing to this struct
    for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
        auto& n = m_doc->tree.nodes[i];
        if (n.refId == structId) {
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::ChangePointerRef{n.id, n.refId, (uint64_t)0}));
        }
    }

    // Remove the struct + subtree (re-lookup since commands may shift indices)
    ni = m_doc->tree.indexOfId(structId);
    if (ni >= 0)
        removeNode(ni);

    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;

    // Switch view if we just deleted the viewed root
    if (m_viewRootId == structId) {
        uint64_t nextRoot = 0;
        for (const auto& n : m_doc->tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                nextRoot = n.id;
                break;
            }
        }
        setViewRootId(nextRoot);
    }

    if (!m_suppressRefresh) refresh();
}

void RcxController::groupIntoUnion(const QSet<uint64_t>& nodeIds) {
    if (nodeIds.size() < 2) return;

    // Collect nodes and verify they share the same parent
    QVector<int> indices;
    uint64_t parentId = 0;
    bool first = true;
    for (uint64_t id : nodeIds) {
        int idx = m_doc->tree.indexOfId(id);
        if (idx < 0) return;
        if (first) { parentId = m_doc->tree.nodes[idx].parentId; first = false; }
        else if (m_doc->tree.nodes[idx].parentId != parentId) return;
        indices.append(idx);
    }

    // Sort by offset to find the union's insertion point
    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
        return m_doc->tree.nodes[a].offset < m_doc->tree.nodes[b].offset;
    });
    int unionOffset = m_doc->tree.nodes[indices.first()].offset;

    bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Group into union"));

    // Save copies of nodes before removal (subtrees included)
    struct SavedNode { Node node; QVector<Node> subtree; };
    QVector<SavedNode> saved;
    for (int idx : indices) {
        SavedNode sn;
        sn.node = m_doc->tree.nodes[idx];
        auto sub = m_doc->tree.subtreeIndices(sn.node.id);
        for (int si : sub)
            if (si != idx) sn.subtree.append(m_doc->tree.nodes[si]);
        saved.append(sn);
    }

    // Remove selected nodes (in reverse order to keep indices valid)
    for (int i = indices.size() - 1; i >= 0; i--) {
        int idx = m_doc->tree.indexOfId(saved[i].node.id);
        if (idx >= 0) {
            QVector<Node> subtree;
            for (int si : m_doc->tree.subtreeIndices(saved[i].node.id))
                subtree.append(m_doc->tree.nodes[si]);
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::Remove{saved[i].node.id, subtree, {}}));
        }
    }

    // Insert union node
    Node unionNode;
    unionNode.kind = NodeKind::Struct;
    unionNode.classKeyword = QStringLiteral("union");
    unionNode.parentId = parentId;
    unionNode.offset = unionOffset;
    unionNode.id = m_doc->tree.reserveId();
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{unionNode}));
    uint64_t unionId = unionNode.id;

    // Re-insert nodes as children of the union, all at offset 0
    for (const auto& sn : saved) {
        Node copy = sn.node;
        copy.parentId = unionId;
        copy.offset = 0;
        copy.id = m_doc->tree.reserveId();
        m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{copy}));

        // Re-insert subtree with updated parentId for direct children
        uint64_t oldId = sn.node.id;
        uint64_t newId = copy.id;
        for (const auto& child : sn.subtree) {
            Node cc = child;
            if (cc.parentId == oldId) cc.parentId = newId;
            cc.id = m_doc->tree.reserveId();
            m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{cc}));
        }
    }

    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();
}

void RcxController::dissolveUnion(uint64_t unionId) {
    int ui = m_doc->tree.indexOfId(unionId);
    if (ui < 0) return;
    const Node& unionNode = m_doc->tree.nodes[ui];
    if (unionNode.kind != NodeKind::Struct || !unionNode.isUnion()) return;

    uint64_t parentId = unionNode.parentId;
    int unionOffset = unionNode.offset;

    // Collect union children
    auto children = m_doc->tree.childrenOf(unionId);
    struct SavedNode { Node node; QVector<Node> subtree; };
    QVector<SavedNode> saved;
    for (int ci : children) {
        SavedNode sn;
        sn.node = m_doc->tree.nodes[ci];
        auto sub = m_doc->tree.subtreeIndices(sn.node.id);
        for (int si : sub)
            if (si != ci) sn.subtree.append(m_doc->tree.nodes[si]);
        saved.append(sn);
    }

    bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Dissolve union"));

    // Remove the union (and all its children)
    {
        QVector<Node> subtree;
        for (int si : m_doc->tree.subtreeIndices(unionId))
            subtree.append(m_doc->tree.nodes[si]);
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::Remove{unionId, subtree, {}}));
    }

    // Re-insert children under the union's parent, at the union's offset
    for (const auto& sn : saved) {
        Node copy = sn.node;
        copy.parentId = parentId;
        copy.offset = unionOffset + sn.node.offset;
        copy.id = m_doc->tree.reserveId();
        m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{copy}));

        uint64_t oldId = sn.node.id;
        uint64_t newId = copy.id;
        for (const auto& child : sn.subtree) {
            Node cc = child;
            if (cc.parentId == oldId) cc.parentId = newId;
            cc.id = m_doc->tree.reserveId();
            m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{cc}));
        }
    }

    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();
}

void RcxController::toggleCollapse(int nodeIdx) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    auto& node = m_doc->tree.nodes[nodeIdx];
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::Collapse{node.id, node.collapsed, !node.collapsed}));
}

void RcxController::materializeRefChildren(int nodeIdx) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    auto& tree = m_doc->tree;

    // Snapshot values before any mutation invalidates references
    const uint64_t parentId   = tree.nodes[nodeIdx].id;
    const uint64_t refId      = tree.nodes[nodeIdx].refId;
    const NodeKind parentKind = tree.nodes[nodeIdx].kind;
    const QString  parentName = tree.nodes[nodeIdx].name;

    if (refId == 0) return;
    if (!tree.childrenOf(parentId).isEmpty()) return;  // already materialized

    // Collect children to clone (copy by value to avoid reference invalidation)
    QVector<int> refChildren = tree.childrenOf(refId);
    if (refChildren.isEmpty()) return;

    QVector<Node> clones;
    clones.reserve(refChildren.size());
    for (int ci : refChildren) {
        Node copy = tree.nodes[ci];  // copy by value before any mutation
        copy.id = tree.reserveId();
        copy.parentId = parentId;
        copy.collapsed = true;
        clones.append(copy);
    }

    // Wrap all mutations in an undo macro
    bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Materialize ref children"));

    for (const Node& clone : clones) {
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::Insert{clone, {}}));
    }

    // Auto-expand the self-referential child (the one that was the cycle)
    // so the user gets expand in a single click
    for (const Node& clone : clones) {
        if (clone.kind == parentKind && clone.name == parentName && clone.refId == refId) {
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::Collapse{clone.id, true, false}));
            break;
        }
    }

    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();
}

void RcxController::applyCommand(const Command& command, bool isUndo) {
    auto& tree = m_doc->tree;

    // Clear value history for nodes whose effective offset changed.
    // When offsets shift (insert/delete/resize), old recorded values came from
    // a different memory address, so keeping them would show false heat.
    // Also invalidates any in-flight async read so that stale snapshot data
    // from before the offset change doesn't re-introduce false heat.
    auto clearNodeHistory = [&](uint64_t id) {
        m_valueHistory.remove(id);
        m_lastValueAddr.remove(id);
    };

    auto clearHistoryForAdjs = [&](const QVector<cmd::OffsetAdj>& adjs) {
        if (adjs.isEmpty()) return;
        m_refreshGen++;  // discard in-flight async read (stale layout)
        for (const auto& adj : adjs) {
            // Clear the adjusted node itself
            clearNodeHistory(adj.nodeId);
            // Clear all descendants (their effective address also shifted)
            for (int ci : tree.subtreeIndices(adj.nodeId))
                clearNodeHistory(tree.nodes[ci].id);
        }
    };

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
            // The changed node's value format changed; clear its history.
            // If offAdjs is empty (same-size change), still bump gen to
            // discard in-flight reads that would record the old format.
            if (c.offAdjs.isEmpty()) m_refreshGen++;
            clearNodeHistory(c.nodeId);
            clearHistoryForAdjs(c.offAdjs);
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
            clearHistoryForAdjs(c.offAdjs);
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
                // Remove nodes and their value history
                QVector<int> indices = tree.subtreeIndices(c.nodeId);
                std::sort(indices.begin(), indices.end(), std::greater<int>());
                for (int idx : indices) {
                    clearNodeHistory(tree.nodes[idx].id);
                    tree.nodes.remove(idx);
                }
                tree.invalidateIdCache();
            }
            // Siblings shifted — their old values are from wrong addresses
            clearHistoryForAdjs(c.offAdjs);
        } else if constexpr (std::is_same_v<T, cmd::ChangeBase>) {
            tree.baseAddress = isUndo ? c.oldBase : c.newBase;
            tree.baseAddressFormula = isUndo ? c.oldFormula : c.newFormula;
            resetSnapshot();
        } else if constexpr (std::is_same_v<T, cmd::WriteBytes>) {
            const QByteArray& bytes = isUndo ? c.oldBytes : c.newBytes;
            // Write through snapshot (patches pages only on success) or provider directly.
            // If write fails, the snapshot is NOT patched, so the next compose shows the
            // real unchanged value — no optimistic visual leak.
            bool ok = m_snapshotProv
                ? m_snapshotProv->write(c.addr, bytes.constData(), bytes.size())
                : m_doc->provider->writeBytes(c.addr, bytes);
            if (!ok)
                qWarning() << "WriteBytes failed at address" << QString::number(c.addr, 16);
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
            // Node and its descendants read from a different address now
            m_refreshGen++;  // discard in-flight async read (stale layout)
            clearNodeHistory(c.nodeId);
            for (int ci : tree.subtreeIndices(c.nodeId))
                clearNodeHistory(tree.nodes[ci].id);
        } else if constexpr (std::is_same_v<T, cmd::ChangeEnumMembers>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].enumMembers = isUndo ? c.oldMembers : c.newMembers;
        } else if constexpr (std::is_same_v<T, cmd::ChangeOffsetExpr>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].offsetExpr = isUndo ? c.oldExpr : c.newExpr;
        } else if constexpr (std::is_same_v<T, cmd::ToggleStatic>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].isStatic = isUndo ? c.oldVal : c.newVal;
        } else if constexpr (std::is_same_v<T, cmd::ChangeComment>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].comment = isUndo ? c.oldComment : c.newComment;
        }
    }, command);

    if (!m_suppressRefresh)
        refresh();
}

void RcxController::setNodeValue(int nodeIdx, int subLine, const QString& text,
                                  bool isAscii, uint64_t resolvedAddr) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    if (!m_doc->provider->isWritable()) return;

    const Node& node = m_doc->tree.nodes[nodeIdx];

    // Use the compose-resolved address when available (correct for pointer children).
    // Fall back to tree.baseAddress + computeOffset for callers that don't supply it.
    uint64_t addr;
    if (resolvedAddr != 0) {
        addr = resolvedAddr;
    } else {
        int64_t signedAddr = m_doc->tree.computeOffset(nodeIdx);
        if (signedAddr < 0) return;  // malformed tree: negative offset
        addr = m_doc->tree.baseAddress + static_cast<uint64_t>(signedAddr);
    }

    // For vector components, redirect to float parsing at sub-offset
    NodeKind editKind = node.kind;
    if ((node.kind == NodeKind::Vec2 || node.kind == NodeKind::Vec3 ||
         node.kind == NodeKind::Vec4) && subLine >= 0) {
        addr += subLine * 4;
        editKind = NodeKind::Float;
    }
    // For Mat4x4 components: subLine encodes flat index (row*4 + col), 0-15
    if (node.kind == NodeKind::Mat4x4 && subLine >= 0 && subLine < 16) {
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

    // Read old bytes before writing (for undo)
    QByteArray oldBytes = m_doc->provider->readBytes(addr, writeSize);

    // Test the write first — don't push a command that will silently fail.
    // This prevents optimistic visual updates for read-only providers.
    bool writeOk = m_snapshotProv
        ? m_snapshotProv->write(addr, newBytes.constData(), newBytes.size())
        : m_doc->provider->writeBytes(addr, newBytes);
    if (!writeOk) {
        qWarning() << "Write failed at address" << QString::number(addr, 16);
        refresh();  // refresh to show the real unchanged value
        return;
    }

    // Write succeeded — push undo command (redo will write again, which is harmless)
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
                adjs.push_back(cmd::OffsetAdj{sib.id, sib.offset, sib.offset + copySize});
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

void RcxController::convertToTypedPointer(uint64_t nodeId) {
    int ni = m_doc->tree.indexOfId(nodeId);
    if (ni < 0) return;
    const Node& node = m_doc->tree.nodes[ni];

    // Determine pointer kind from document's target pointer size
    NodeKind ptrKind = (m_doc->tree.pointerSize >= 8)
        ? NodeKind::Pointer64
        : NodeKind::Pointer32;

    // Generate unique struct name: "NewClass", "NewClass_2", "NewClass_3", ...
    QString baseName = QStringLiteral("NewClass");
    QString typeName = baseName;
    int suffix = 2;
    while (true) {
        bool exists = false;
        for (const auto& n : m_doc->tree.nodes) {
            if (n.kind == NodeKind::Struct && n.structTypeName == typeName) {
                exists = true; break;
            }
        }
        if (!exists) break;
        typeName = QStringLiteral("%1_%2").arg(baseName).arg(suffix++);
    }

    // Create the new root struct node
    Node rootStruct;
    rootStruct.kind = NodeKind::Struct;
    rootStruct.name = QStringLiteral("instance");
    rootStruct.structTypeName = typeName;
    rootStruct.classKeyword = QStringLiteral("class");
    rootStruct.parentId = 0;
    rootStruct.offset = 0;
    rootStruct.id = m_doc->tree.reserveId();

    // Create child hex fields for the new struct, sized to target arch
    constexpr int kDefaultFields = 16;
    bool is32 = (m_doc->tree.pointerSize < 8);
    NodeKind hexKind = is32 ? NodeKind::Hex32 : NodeKind::Hex64;
    int stride = is32 ? 4 : 8;
    QVector<Node> children;
    for (int i = 0; i < kDefaultFields; i++) {
        Node c;
        c.kind = hexKind;
        c.name = QStringLiteral("field_%1").arg(i * stride, 2, 16, QChar('0'));
        c.parentId = rootStruct.id;
        c.offset = i * stride;
        c.id = m_doc->tree.reserveId();
        children.append(c);
    }

    uint64_t oldRefId = node.refId;

    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Change to ptr*"));

    // 1. Change kind to Pointer64/32 (if not already)
    if (node.kind != ptrKind)
        changeNodeKind(ni, ptrKind);

    // 2. Insert the new root struct
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{rootStruct, {}}));

    // 3. Insert its children
    for (const Node& c : children)
        m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{c, {}}));

    // 4. Set refId to point to the new struct
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::ChangePointerRef{nodeId, oldRefId, rootStruct.id}));

    m_doc->undoStack.endMacro();
    m_suppressRefresh = false;
    refresh();
}

void RcxController::splitHexNode(uint64_t nodeId) {
    int ni = m_doc->tree.indexOfId(nodeId);
    if (ni < 0) return;
    const Node& node = m_doc->tree.nodes[ni];

    NodeKind halfKind;
    int halfSize;
    if (node.kind == NodeKind::Hex128)     { halfKind = NodeKind::Hex64; halfSize = 8; }
    else if (node.kind == NodeKind::Hex64)  { halfKind = NodeKind::Hex32; halfSize = 4; }
    else if (node.kind == NodeKind::Hex32)  { halfKind = NodeKind::Hex16; halfSize = 2; }
    else if (node.kind == NodeKind::Hex16)  { halfKind = NodeKind::Hex8;  halfSize = 1; }
    else return;

    uint64_t parentId = node.parentId;
    int baseOffset = node.offset;
    QString baseName = node.name;

    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Split Hex node"));

    // Remove the original node
    QVector<Node> subtree;
    subtree.append(node);
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::Remove{nodeId, subtree, {}}));

    // Insert two half-sized nodes
    Node lo;
    lo.kind = halfKind;
    lo.name = baseName;
    lo.parentId = parentId;
    lo.offset = baseOffset;
    lo.id = m_doc->tree.reserveId();
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{lo, {}}));

    Node hi;
    hi.kind = halfKind;
    hi.name = baseName + QStringLiteral("_hi");
    hi.parentId = parentId;
    hi.offset = baseOffset + halfSize;
    hi.id = m_doc->tree.reserveId();
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{hi, {}}));

    m_doc->undoStack.endMacro();
    m_suppressRefresh = false;
    refresh();
}

// ── Hex toolbar popup ──

void RcxController::showHexToolbar(RcxEditor* editor, int nodeIdx) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    const auto& node = m_doc->tree.nodes[nodeIdx];
    if (!isHexNode(node.kind)) return;

    if (!m_hexToolbar) {
        m_hexToolbar = new HexToolbarPopup(editor);
        connect(m_hexToolbar, &HexToolbarPopup::sizeSelected,
                this, [this](uint64_t nid, NodeKind newKind) {
            int ni = m_doc->tree.indexOfId(nid);
            if (ni < 0) return;
            const auto& n = m_doc->tree.nodes[ni];
            if (isHexNode(newKind)) {
                if (sizeForKind(newKind) <= sizeForKind(n.kind))
                    changeNodeKind(ni, newKind);
                else
                    joinHexNodes(nid, newKind);
            } else {
                changeNodeKind(ni, newKind);  // smart suggestion (ptr/float/utf8)
            }
        });
        connect(m_hexToolbar, &HexToolbarPopup::insertAbove,
                this, [this](uint64_t nid) {
            int ni = m_doc->tree.indexOfId(nid);
            if (ni >= 0) insertNodeAbove(ni, NodeKind::Hex64, QStringLiteral("field"));
        });
        connect(m_hexToolbar, &HexToolbarPopup::insertBelow,
                this, [this](uint64_t nid) {
            int ni = m_doc->tree.indexOfId(nid);
            if (ni < 0) return;
            const auto& n = m_doc->tree.nodes[ni];
            insertNode(n.parentId, n.offset + sizeForKind(n.kind),
                       NodeKind::Hex64, QStringLiteral("field"));
        });
        connect(m_hexToolbar, &HexToolbarPopup::joinSelected,
                this, [this]() {
            if (m_selIds.size() < 2) return;
            // Find first selected hex node
            uint64_t firstId = 0;
            int totalBytes = 0;
            for (uint64_t sid : m_selIds) {
                int ni = m_doc->tree.indexOfId(sid);
                if (ni < 0 || !isHexNode(m_doc->tree.nodes[ni].kind)) continue;
                if (firstId == 0 || m_doc->tree.nodes[ni].offset < m_doc->tree.nodes[m_doc->tree.indexOfId(firstId)].offset)
                    firstId = sid;
                totalBytes += sizeForKind(m_doc->tree.nodes[ni].kind);
            }
            if (!firstId || totalBytes < 2) return;
            NodeKind target = NodeKind::Hex8;
            if      (totalBytes >= 16) target = NodeKind::Hex128;
            else if (totalBytes >= 8)  target = NodeKind::Hex64;
            else if (totalBytes >= 4)  target = NodeKind::Hex32;
            else if (totalBytes >= 2)  target = NodeKind::Hex16;
            joinHexNodes(firstId, target);
        });
        connect(m_hexToolbar, &HexToolbarPopup::fillToOffset,
                this, [this](uint64_t nid, int targetOffset) {
            int ni = m_doc->tree.indexOfId(nid);
            if (ni < 0) return;
            const auto& n = m_doc->tree.nodes[ni];
            int curEnd = n.offset + sizeForKind(n.kind);
            int gap = targetOffset - curEnd;
            if (gap <= 0) return;
            m_suppressRefresh = true;
            m_doc->undoStack.beginMacro(QStringLiteral("Fill to offset 0x%1").arg(targetOffset, 0, 16));
            int padOff = curEnd;
            while (gap > 0) {
                NodeKind pk; int ps;
                if      (gap >= 16) { pk = NodeKind::Hex128; ps = 16; }
                else if (gap >= 8)  { pk = NodeKind::Hex64;  ps = 8; }
                else if (gap >= 4)  { pk = NodeKind::Hex32;  ps = 4; }
                else if (gap >= 2)  { pk = NodeKind::Hex16;  ps = 2; }
                else                { pk = NodeKind::Hex8;   ps = 1; }
                Node pad;
                pad.kind = pk;
                pad.name = QStringLiteral("pad_%1").arg(padOff, 0, 16);
                pad.parentId = n.parentId;
                pad.offset = padOff;
                pad.id = m_doc->tree.reserveId();
                m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{pad, {}}));
                padOff += ps;
                gap -= ps;
            }
            m_doc->undoStack.endMacro();
            m_suppressRefresh = false;
            refresh();
        });
    }

    // Build context
    HexPopupContext ctx;
    ctx.nodeId = node.id;
    ctx.currentKind = node.kind;
    int curSz = sizeForKind(node.kind);
    uint64_t addr = m_doc->tree.baseAddress + m_doc->tree.computeOffset(nodeIdx);
    ctx.data = m_doc->provider ? m_doc->provider->readBytes(addr, curSz) : QByteArray(curSz, '\0');

    // Collect adjacent same-parent hex nodes
    uint64_t parentId = node.parentId;
    int nextOff = node.offset + curSz;
    for (int i = nodeIdx + 1; i < m_doc->tree.nodes.size() && ctx.nexts.size() < 15; i++) {
        const auto& sib = m_doc->tree.nodes[i];
        if (sib.parentId != parentId) break;
        if (sib.offset != nextOff) break;
        if (!isHexNode(sib.kind)) break;
        HexPopupContext::Adjacent adj;
        adj.exists = true;
        adj.kind = sib.kind;
        int sibSz = sizeForKind(sib.kind);
        uint64_t sibAddr = m_doc->tree.baseAddress + m_doc->tree.computeOffset(i);
        adj.data = m_doc->provider ? m_doc->provider->readBytes(sibAddr, sibSz) : QByteArray(sibSz, '\0');
        ctx.nexts.append(adj);
        nextOff += sibSz;
    }

    // Smart suggestions (only when pinned — avoids overhead on every selection)
    if (m_hexToolbar->isPinned() && m_doc->provider) {
        // Pointer check: interpret bytes as uint64, check if readable address
        if (curSz >= 8) {
            uint64_t ptrVal = 0;
            memcpy(&ptrVal, ctx.data.constData(), qMin(curSz, 8));
            if (ptrVal > 0x10000 && m_doc->provider->isReadable(ptrVal, 1)) {
                ctx.hasPtr = true;
                ctx.ptrSymbol = m_doc->provider->getSymbol(ptrVal);
            }
        } else if (curSz == 4) {
            uint32_t ptrVal = 0;
            memcpy(&ptrVal, ctx.data.constData(), 4);
            if (ptrVal > 0x10000 && m_doc->provider->isReadable(ptrVal, 1)) {
                ctx.hasPtr = true;
                ctx.ptrSymbol = m_doc->provider->getSymbol(ptrVal);
            }
        }
        // Float check
        if (curSz >= 4) {
            float fv = 0;
            memcpy(&fv, ctx.data.constData(), 4);
            if (std::isfinite(fv) && std::fabs(fv) < 1e6f && fv != 0.0f
                && std::fabs(fv) > 1e-6f) {
                ctx.hasFloat = true;
                ctx.floatVal = fv;
            }
        }
        // String check: count leading printable ASCII bytes
        {
            int printable = 0;
            for (int i = 0; i < ctx.data.size(); i++) {
                uint8_t c = (uint8_t)ctx.data[i];
                if (c >= 0x20 && c <= 0x7E) printable++;
                else break;
            }
            if (printable >= 4) {
                ctx.hasString = true;
                ctx.stringPreview = QString::fromLatin1(ctx.data.constData(), printable);
            }
        }
    }

    // Multi-select info
    if (m_selIds.size() > 1) {
        int count = 0, bytes = 0;
        bool contiguous = true;
        NodeKind commonKind = NodeKind::Hex8;
        int lastOff = -1;
        uint64_t commonParent = 0;
        for (uint64_t sid : m_selIds) {
            int si = m_doc->tree.indexOfId(sid);
            if (si < 0 || !isHexNode(m_doc->tree.nodes[si].kind)) { contiguous = false; continue; }
            const auto& sn = m_doc->tree.nodes[si];
            if (count == 0) { commonKind = sn.kind; commonParent = sn.parentId; }
            else {
                if (sn.kind != commonKind || sn.parentId != commonParent) contiguous = false;
                if (lastOff >= 0 && sn.offset != lastOff) contiguous = false;
            }
            count++;
            bytes += sizeForKind(sn.kind);
            lastOff = sn.offset + sizeForKind(sn.kind);
        }
        ctx.multiSelectCount = count;
        ctx.multiSelectBytes = bytes;
        ctx.multiSelectContiguous = contiguous;
        ctx.multiSelectKind = commonKind;
    }

    m_hexToolbar->setFont(editor->scintilla()->font());
    m_hexToolbar->setContext(ctx);

    // Position below the selected line, left-aligned to the type text
    auto* sci = editor->scintilla();
    int line = -1;
    for (int i = 0; i < m_lastResult.meta.size(); i++) {
        if (m_lastResult.meta[i].nodeId == node.id) { line = i; break; }
    }
    if (line < 0) return;
    const auto& lm = m_lastResult.meta[line];
    ColumnSpan ts = typeSpanFor(lm);
    int linePos = sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, line);
    int typePos = linePos + (ts.start > 0 ? ts.start : 0);
    int xPos = sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION, (uintptr_t)0, typePos);
    int yPos = sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, (uintptr_t)0, linePos);
    int lineH = sci->SendScintilla(QsciScintillaBase::SCI_TEXTHEIGHT, line);
    QPoint gp = sci->viewport()->mapToGlobal(QPoint(xPos, yPos + lineH));
    m_hexToolbar->popup(gp);
}

void RcxController::hideHexToolbar() {
    if (m_hexToolbar && m_hexToolbar->isVisible() && !m_hexToolbar->isPinned())
        m_hexToolbar->hide();
}

void RcxController::joinHexNodes(uint64_t nodeId, NodeKind targetKind) {
    int ni = m_doc->tree.indexOfId(nodeId);
    if (ni < 0) return;
    const auto& node = m_doc->tree.nodes[ni];
    int curSz = sizeForKind(node.kind);
    int tgtSz = sizeForKind(targetKind);
    int needed = tgtSz / curSz;  // total nodes needed (including this one)
    if (needed <= 1) return;

    // Collect the nodes to merge
    QVector<int> mergeIndices;
    mergeIndices.append(ni);
    uint64_t parentId = node.parentId;
    int nextOff = node.offset + curSz;
    for (int i = ni + 1; i < m_doc->tree.nodes.size() && mergeIndices.size() < needed; i++) {
        const auto& sib = m_doc->tree.nodes[i];
        if (sib.parentId != parentId || sib.offset != nextOff || sib.kind != node.kind) break;
        mergeIndices.append(i);
        nextOff += curSz;
    }
    if (mergeIndices.size() < needed) return;

    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Join Hex nodes"));

    // Remove all nodes (in reverse to keep indices valid)
    for (int j = mergeIndices.size() - 1; j >= 0; j--) {
        int idx = mergeIndices[j];
        QVector<Node> subtree;
        subtree.append(m_doc->tree.nodes[idx]);
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::Remove{m_doc->tree.nodes[idx].id, subtree, {}}));
    }

    // Insert one joined node
    Node joined;
    joined.kind = targetKind;
    joined.name = node.name;
    joined.parentId = parentId;
    joined.offset = node.offset;
    joined.id = m_doc->tree.reserveId();
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{joined, {}}));

    m_doc->undoStack.endMacro();
    m_suppressRefresh = false;
    refresh();
}

void RcxController::toggleBitfieldBit(uint64_t nodeId, int memberIdx) {
    int ni = m_doc->tree.indexOfId(nodeId);
    if (ni < 0) return;
    const Node& node = m_doc->tree.nodes[ni];
    if (!node.isBitfield()) return;
    if (memberIdx < 0 || memberIdx >= node.bitfieldMembers.size()) return;
    if (!m_doc->provider || !m_doc->provider->isWritable()) return;

    const auto& bm = node.bitfieldMembers[memberIdx];
    int64_t signedOff = m_doc->tree.computeOffset(ni);
    if (signedOff < 0) return;
    uint64_t addr = m_doc->tree.baseAddress + static_cast<uint64_t>(signedOff);
    int containerSize = sizeForKind(node.elementKind);
    if (containerSize <= 0) containerSize = 4;

    QByteArray oldBytes(containerSize, 0);
    m_doc->provider->read(addr, oldBytes.data(), containerSize);

    QByteArray newBytes = oldBytes;
    // Toggle the bit
    int byteIdx = bm.bitOffset / 8;
    int bitInByte = bm.bitOffset % 8;
    if (byteIdx < containerSize)
        newBytes[byteIdx] = newBytes[byteIdx] ^ (1 << bitInByte);

    m_doc->undoStack.push(new RcxCommand(this,
        cmd::WriteBytes{addr, oldBytes, newBytes}));
    refresh();
}

void RcxController::editBitfieldValue(uint64_t nodeId, int memberIdx) {
    int ni = m_doc->tree.indexOfId(nodeId);
    if (ni < 0) return;
    const Node& node = m_doc->tree.nodes[ni];
    if (!node.isBitfield()) return;
    if (memberIdx < 0 || memberIdx >= node.bitfieldMembers.size()) return;
    if (!m_doc->provider || !m_doc->provider->isWritable()) return;

    const auto& bm = node.bitfieldMembers[memberIdx];
    int64_t signedOff = m_doc->tree.computeOffset(ni);
    if (signedOff < 0) return;
    uint64_t addr = m_doc->tree.baseAddress + static_cast<uint64_t>(signedOff);
    int containerSize = sizeForKind(node.elementKind);
    if (containerSize <= 0) containerSize = 4;

    // Read current value
    uint64_t curVal = fmt::extractBits(*m_doc->provider, addr, node.elementKind,
                                       bm.bitOffset, bm.bitWidth);
    uint64_t maxVal = (bm.bitWidth >= 64) ? UINT64_MAX : ((1ULL << bm.bitWidth) - 1);

    bool ok = false;
    QString input = QInputDialog::getText(nullptr,
        QStringLiteral("Edit Bitfield Value"),
        QStringLiteral("%1 (%2 bits, max %3):")
            .arg(bm.name).arg(bm.bitWidth).arg(maxVal),
        QLineEdit::Normal,
        QString::number(curVal), &ok);
    if (!ok || input.isEmpty()) return;

    // Parse value (support hex with 0x prefix)
    uint64_t newVal;
    if (input.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        newVal = input.mid(2).toULongLong(&ok, 16);
    else
        newVal = input.toULongLong(&ok, 10);
    if (!ok) return;
    newVal &= maxVal;

    QByteArray oldBytes(containerSize, 0);
    m_doc->provider->read(addr, oldBytes.data(), containerSize);

    // Read-modify-write: clear target bits and set new value
    QByteArray newBytes = oldBytes;
    uint64_t container = 0;
    memcpy(&container, newBytes.constData(), qMin(containerSize, (int)sizeof(container)));
    uint64_t mask = maxVal << bm.bitOffset;
    container = (container & ~mask) | ((newVal & maxVal) << bm.bitOffset);
    memcpy(newBytes.data(), &container, qMin(containerSize, (int)sizeof(container)));

    m_doc->undoStack.push(new RcxCommand(this,
        cmd::WriteBytes{addr, oldBytes, newBytes}));
    refresh();
}

void RcxController::insertStaticField(uint64_t parentId) {
    Node sf;
    sf.id = m_doc->tree.reserveId();
    sf.kind = NodeKind::Hex64;
    sf.name = QStringLiteral("static_field");
    sf.parentId = parentId;
    sf.offset = 0;
    sf.isStatic = true;
    sf.offsetExpr = QStringLiteral("base");
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{sf, {}}));
}

void RcxController::appendBytesDialog(QWidget* parent, uint64_t targetId) {
    bool ok;
    QString input = QInputDialog::getText(parent,
        QStringLiteral("Append bytes"),
        QStringLiteral("Byte count (decimal or 0x hex):"),
        QLineEdit::Normal, QStringLiteral("128"), &ok);
    if (!ok || input.trimmed().isEmpty()) return;
    QString trimmed = input.trimmed();
    int byteCount = 0;
    if (trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        byteCount = trimmed.mid(2).toInt(&ok, 16);
    else
        byteCount = trimmed.toInt(&ok, 10);
    if (!ok || byteCount <= 0) return;
    int hex64Count = byteCount / 8;
    int remainBytes = byteCount % 8;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Append %1 bytes").arg(byteCount));
    int idx = 0;
    for (int i = 0; i < hex64Count; i++, idx++)
        insertNode(targetId, -1, NodeKind::Hex64, QStringLiteral("field_%1").arg(idx));
    for (int i = 0; i < remainBytes; i++, idx++)
        insertNode(targetId, -1, NodeKind::Hex8, QStringLiteral("field_%1").arg(idx));
    m_doc->undoStack.endMacro();
    m_suppressRefresh = false;
    refresh();
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

    // Multi-select batch actions
    if (hasNode && m_selIds.size() > 1) {
        QMenu menu;
        int count = m_selIds.size();
        QSet<uint64_t> ids = m_selIds;

        // Helper: collect indices from selected ids
        auto collectIndices = [this, &ids]() {
            QVector<int> indices;
            for (uint64_t id : ids) {
                int idx = m_doc->tree.indexOfId(id);
                if (idx >= 0) indices.append(idx);
            }
            return indices;
        };

        // Quick-convert shortcuts when all selected nodes share the same kind
        NodeKind commonKind = NodeKind::Hex64;
        bool allSame = true;
        {
            bool first = true;
            for (uint64_t id : ids) {
                int idx = m_doc->tree.indexOfId(id);
                if (idx < 0) continue;
                if (first) { commonKind = m_doc->tree.nodes[idx].kind; first = false; }
                else if (m_doc->tree.nodes[idx].kind != commonKind) { allSame = false; break; }
            }
        }
        bool addedQuickConvert = false;
        if (allSame) {
            if (commonKind == NodeKind::Hex64) {
                menu.addAction("Change to uint64_t", [this, collectIndices]() {
                    batchChangeKind(collectIndices(), NodeKind::UInt64); });
                menu.addAction("Change to uint32_t", [this, collectIndices]() {
                    batchChangeKind(collectIndices(), NodeKind::UInt32); });
                addedQuickConvert = true;
            } else if (commonKind == NodeKind::Hex32) {
                menu.addAction("Change to uint32_t", [this, collectIndices]() {
                    batchChangeKind(collectIndices(), NodeKind::UInt32); });
                menu.addAction("Change to float", [this, collectIndices]() {
                    batchChangeKind(collectIndices(), NodeKind::Float); });
                addedQuickConvert = true;
            } else if (commonKind == NodeKind::Hex16) {
                menu.addAction("Change to int16_t", [this, collectIndices]() {
                    batchChangeKind(collectIndices(), NodeKind::Int16); });
                addedQuickConvert = true;
            }
            if (commonKind == NodeKind::Hex64 || commonKind == NodeKind::Pointer64) {
                menu.addAction("Change to fnptr64", [this, collectIndices]() {
                    batchChangeKind(collectIndices(), NodeKind::FuncPtr64); });
                addedQuickConvert = true;
            }
            if (commonKind == NodeKind::Hex32 || commonKind == NodeKind::Pointer32) {
                menu.addAction("Change to fnptr32", [this, collectIndices]() {
                    batchChangeKind(collectIndices(), NodeKind::FuncPtr32); });
                addedQuickConvert = true;
            }
            if (commonKind == NodeKind::FuncPtr64) {
                menu.addAction("Change to ptr64", [this, collectIndices]() {
                    batchChangeKind(collectIndices(), NodeKind::Pointer64); });
                addedQuickConvert = true;
            }
            if (commonKind == NodeKind::FuncPtr32) {
                menu.addAction("Change to ptr32", [this, collectIndices]() {
                    batchChangeKind(collectIndices(), NodeKind::Pointer32); });
                addedQuickConvert = true;
            }
        }
        // Check if any selected nodes are non-hex primitives (for "Convert to Hex")
        bool anyNonHex = false;
        bool allConvertible = true;  // all non-container, non-hex
        for (uint64_t id : ids) {
            int idx = m_doc->tree.indexOfId(id);
            if (idx < 0) continue;
            NodeKind k = m_doc->tree.nodes[idx].kind;
            if (k == NodeKind::Struct || k == NodeKind::Array)
                allConvertible = false;
            else if (!isHexNode(k))
                anyNonHex = true;
        }
        if (anyNonHex && allConvertible) {
            menu.addAction("Convert to Hex", [this, collectIndices]() {
                auto indices = collectIndices();
                // Convert each to hex equivalent based on size
                m_suppressRefresh = true;
                m_doc->undoStack.beginMacro(QStringLiteral("Convert to Hex"));
                for (int idx : indices) {
                    if (idx < 0 || idx >= m_doc->tree.nodes.size()) continue;
                    const Node& n = m_doc->tree.nodes[idx];
                    if (isHexNode(n.kind) || n.kind == NodeKind::Struct || n.kind == NodeKind::Array)
                        continue;
                    int sz = n.byteSize();
                    NodeKind hexKind;
                    if (sz >= 8)      hexKind = NodeKind::Hex64;
                    else if (sz >= 4) hexKind = NodeKind::Hex32;
                    else if (sz >= 2) hexKind = NodeKind::Hex16;
                    else              hexKind = NodeKind::Hex8;
                    changeNodeKind(idx, hexKind);
                }
                m_doc->undoStack.endMacro();
                m_suppressRefresh = false;
                refresh();
            });
        }

        if (addedQuickConvert || (anyNonHex && allConvertible))
            menu.addSeparator();

        menu.addAction(icon("symbol-structure.svg"), QString("Change type of %1 nodes...").arg(count),
                       [this, ids, collectIndices]() {
            QStringList types;
            for (const auto& e : kKindMeta) types << e.name;
            bool ok;
            QString sel = QInputDialog::getItem(nullptr, "Change Type", "Type:",
                                                types, 0, false, &ok);
            if (ok)
                batchChangeKind(collectIndices(), kindFromString(sel));
        });

        menu.addSeparator();

        // ── Insert ► submenu ──
        {
            // Find earliest selected node (lowest offset) for insert-above
            int firstIdx = -1;
            int lowestOff = INT_MAX;
            for (uint64_t id : ids) {
                int idx = m_doc->tree.indexOfId(id);
                if (idx >= 0 && m_doc->tree.nodes[idx].offset < lowestOff) {
                    lowestOff = m_doc->tree.nodes[idx].offset;
                    firstIdx = idx;
                }
            }
            auto* insertMenu = menu.addMenu(icon("diff-added.svg"), "Insert");
            insertMenu->addAction("Insert 4 Above\tShift+Ins", [this, firstIdx]() {
                if (firstIdx >= 0)
                    insertNodeAbove(firstIdx, NodeKind::Hex32, QStringLiteral("field"));
            });
            insertMenu->addAction("Insert 8 Above\tIns", [this, firstIdx]() {
                if (firstIdx >= 0)
                    insertNodeAbove(firstIdx, NodeKind::Hex64, QStringLiteral("field"));
            });
        }

        // Check if all selected nodes share the same parent (required for grouping)
        {
            bool sameParent = true;
            uint64_t firstParent = 0;
            bool fp = true;
            for (uint64_t id : ids) {
                int idx = m_doc->tree.indexOfId(id);
                if (idx < 0) { sameParent = false; break; }
                if (fp) { firstParent = m_doc->tree.nodes[idx].parentId; fp = false; }
                else if (m_doc->tree.nodes[idx].parentId != firstParent) { sameParent = false; break; }
            }
            if (sameParent)
                menu.addAction("Group into Union", [this, ids]() { groupIntoUnion(ids); });
        }

        menu.addSeparator();

        // Batch comment (only when comments are enabled)
        if (m_showComments) menu.addAction(icon("edit.svg"), QString("&Comment %1 nodes\t;").arg(count), [this, ids]() {
            // Gather existing comment from first node as default
            QString existing;
            for (uint64_t id : ids) {
                int idx = m_doc->tree.indexOfId(id);
                if (idx >= 0 && !m_doc->tree.nodes[idx].comment.isEmpty()) {
                    existing = m_doc->tree.nodes[idx].comment;
                    break;
                }
            }
            bool ok = false;
            QString text = showCommentDialog(
                qobject_cast<QWidget*>(parent()),
                QStringLiteral("Comment %1 nodes").arg(ids.size()),
                existing, &ok);
            if (!ok) return;
            QString comment = text.trimmed();
            m_suppressRefresh = true;
            m_doc->undoStack.beginMacro(QStringLiteral("Comment %1 nodes").arg(ids.size()));
            for (uint64_t id : ids) {
                int idx = m_doc->tree.indexOfId(id);
                if (idx < 0) continue;
                const Node& node = m_doc->tree.nodes[idx];
                if (node.comment != comment)
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::ChangeComment{node.id, node.comment, comment}));
            }
            m_doc->undoStack.endMacro();
            m_suppressRefresh = false;
            refresh();
        });

        menu.addAction(icon("files.svg"), QString("Duplicate %1 nodes").arg(count), [this, ids]() {
            for (uint64_t id : ids) {
                int idx = m_doc->tree.indexOfId(id);
                if (idx >= 0) duplicateNode(idx);
            }
        });
        menu.addAction(icon("trash.svg"), QString("Delete %1 nodes").arg(count), [this, collectIndices]() {
            batchRemoveNodes(collectIndices());
        });

        menu.addSeparator();

        {
            auto* act = menu.addAction("Track Value Changes");
            act->setCheckable(true);
            act->setChecked(m_trackValues);
            connect(act, &QAction::toggled, this, &RcxController::setTrackValues);
        }
        {
            auto* act = menu.addAction("Clear Value History");
            act->setToolTip(QStringLiteral("Reset change tracking for selected nodes"));
            connect(act, &QAction::triggered, this, [this, ids]() {
                for (uint64_t id : ids) {
                    m_valueHistory.remove(id);
                    m_lastValueAddr.remove(id);
                    for (int ci : m_doc->tree.subtreeIndices(id)) {
                        m_valueHistory.remove(m_doc->tree.nodes[ci].id);
                        m_lastValueAddr.remove(m_doc->tree.nodes[ci].id);
                    }
                }
                m_refreshGen++;
                m_prevPages.clear();
                m_changedOffsets.clear();
                m_valueTrackCooldown = 5;
                refresh();
                for (auto* editor : m_editors)
                    editor->dismissHistoryPopup();
            });
        }

        menu.addSeparator();

        QMenu* copyMenu = menu.addMenu(icon("clippy.svg"), "Copy");
        copyMenu->addAction(icon("link.svg"), "Copy &Address", [this, ids]() {
            QStringList addrs;
            for (uint64_t id : ids) {
                int ni = m_doc->tree.indexOfId(id);
                if (ni < 0) continue;
                int64_t off = m_doc->tree.computeOffset(ni);
                if (off < 0) continue;
                uint64_t addr = m_doc->tree.baseAddress + static_cast<uint64_t>(off);
                addrs << QStringLiteral("0x") + QString::number(addr, 16).toUpper();
            }
            QApplication::clipboard()->setText(addrs.join('\n'));
        });

        emit contextMenuAboutToShow(&menu, line);
        menu.exec(globalPos);
        return;
    }

    QMenu menu;

    // ── Node-specific actions (only when clicking on a node) ──
    if (hasNode) {
        const Node& node = m_doc->tree.nodes[nodeIdx];
        uint64_t nodeId = node.id;
        uint64_t parentId = node.parentId;

        // ── Member line: enum or bitfield member ──
        bool isEnumMember = node.isEnum()
            && !node.enumMembers.isEmpty()
            && subLine >= 0 && subLine < node.enumMembers.size();
        bool isBitfieldMember = node.isBitfield()
            && !node.bitfieldMembers.isEmpty()
            && subLine >= 0 && subLine < node.bitfieldMembers.size();

        bool isEnumNode = node.isEnum();

        if (isEnumMember || isBitfieldMember) {
            if (isEnumMember) {
                menu.addAction(icon("diff-added.svg"), "Add Member Above", [this, nodeId, subLine]() {
                    int ni = m_doc->tree.indexOfId(nodeId);
                    if (ni < 0) return;
                    auto members = m_doc->tree.nodes[ni].enumMembers;
                    int64_t val = (subLine > 0) ? members[subLine - 1].second + 1 : 0;
                    auto oldMembers = members;
                    members.insert(subLine, {QStringLiteral("NewMember"), val});
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::ChangeEnumMembers{nodeId, oldMembers, members}));
                });
                menu.addAction(icon("diff-added.svg"), "Add Member Below", [this, nodeId, subLine]() {
                    int ni = m_doc->tree.indexOfId(nodeId);
                    if (ni < 0) return;
                    auto members = m_doc->tree.nodes[ni].enumMembers;
                    int64_t val = members[subLine].second + 1;
                    auto oldMembers = members;
                    members.insert(subLine + 1, {QStringLiteral("NewMember"), val});
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::ChangeEnumMembers{nodeId, oldMembers, members}));
                });
                menu.addAction(icon("trash.svg"), "Remove Member", [this, nodeId, subLine]() {
                    int ni = m_doc->tree.indexOfId(nodeId);
                    if (ni < 0) return;
                    auto members = m_doc->tree.nodes[ni].enumMembers;
                    auto oldMembers = members;
                    members.remove(subLine);
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::ChangeEnumMembers{nodeId, oldMembers, members}));
                });
                menu.addSeparator();
            }
            if (isBitfieldMember) {
                const auto& bm = node.bitfieldMembers[subLine];
                if (bm.bitWidth == 1) {
                    menu.addAction("Toggle Bit", [this, nodeId, subLine]() {
                        toggleBitfieldBit(nodeId, subLine);
                    });
                } else {
                    menu.addAction("Edit Value...", [this, nodeId, subLine]() {
                        editBitfieldValue(nodeId, subLine);
                    });
                }
                menu.addSeparator();
            }
            // Fall through to always-available actions
        } else if (isEnumNode) {
            // Enum header line — enum-specific actions only (no struct ops)
            menu.addAction(icon("diff-added.svg"), "Add Member", [this, nodeId]() {
                int ni = m_doc->tree.indexOfId(nodeId);
                if (ni < 0) return;
                auto members = m_doc->tree.nodes[ni].enumMembers;
                int64_t nextVal = members.isEmpty() ? 0 : members.last().second + 1;
                auto oldMembers = members;
                members.emplaceBack(QStringLiteral("NewMember"), nextVal);
                m_doc->undoStack.push(new RcxCommand(this,
                    cmd::ChangeEnumMembers{nodeId, oldMembers, members}));
            });
            menu.addAction(icon("edit.svg"), "&Rename...", [this, editor, line]() {
                editor->beginInlineEdit(EditTarget::Name, line);
            });
            menu.addSeparator();
            menu.addAction(icon("trash.svg"), "&Delete", [this, nodeId]() {
                int ni = m_doc->tree.indexOfId(nodeId);
                if (ni >= 0) removeNode(ni);
            });
            menu.addSeparator();
            // Fall through to always-available actions
        } else {

        // ── New Class / Ptr to New Class (promoted near top) ──
        if (node.kind != NodeKind::Struct && node.kind != NodeKind::Array) {
            int nodeSz = node.byteSize();
            // "New Class" — convert this node to an embedded struct referencing a new root class
            menu.addAction(icon("symbol-structure.svg"), "New Class", [this, nodeId, nodeSz]() {
                int ni = m_doc->tree.indexOfId(nodeId);
                if (ni < 0) return;
                const uint64_t parentId = m_doc->tree.nodes[ni].parentId;
                const int nodeOffset = m_doc->tree.nodes[ni].offset;

                // Create new root struct
                QString baseName = QStringLiteral("NewClass");
                QString typeName = baseName;
                int counter = 2;
                QSet<QString> existing;
                for (const auto& nd : m_doc->tree.nodes)
                    if (nd.kind == NodeKind::Struct && !nd.structTypeName.isEmpty())
                        existing.insert(nd.structTypeName);
                while (existing.contains(typeName))
                    typeName = QStringLiteral("%1_%2").arg(baseName).arg(counter++);

                m_suppressRefresh = true;
                m_doc->undoStack.beginMacro(QStringLiteral("New Class"));

                // 1. Create the root class definition (8 × Hex64 = 64 bytes)
                Node root;
                root.kind = NodeKind::Struct;
                root.structTypeName = typeName;
                root.name = QStringLiteral("instance");
                root.classKeyword = QStringLiteral("class");
                root.parentId = 0;
                root.offset = 0;
                root.id = m_doc->tree.reserveId();
                m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{root}));
                constexpr int kDefaultFields = 8;
                for (int i = 0; i < kDefaultFields; i++)
                    insertNode(root.id, i * 8, NodeKind::Hex64,
                               QString("field_%1").arg(i * 8, 2, 16, QChar('0')));

                // 2. Convert this node to Struct with refId
                ni = m_doc->tree.indexOfId(nodeId);
                if (ni >= 0) {
                    changeNodeKind(ni, NodeKind::Struct);
                    ni = m_doc->tree.indexOfId(nodeId);
                    if (ni >= 0) {
                        auto& n = m_doc->tree.nodes[ni];
                        if (n.structTypeName != typeName)
                            m_doc->undoStack.push(new RcxCommand(this,
                                cmd::ChangeStructTypeName{nodeId, n.structTypeName, typeName}));
                        if (n.refId != root.id)
                            m_doc->undoStack.push(new RcxCommand(this,
                                cmd::ChangePointerRef{nodeId, n.refId, root.id}));
                    }
                }

                // 3. Shift siblings to make room for the embedded struct
                int newSpan = m_doc->tree.structSpan(root.id);
                int delta = newSpan - nodeSz;
                if (delta > 0) {
                    int oldEnd = nodeOffset + nodeSz;
                    auto siblings = m_doc->tree.childrenOf(parentId);
                    for (int si : siblings) {
                        auto& sib = m_doc->tree.nodes[si];
                        if (sib.id == nodeId || sib.isStatic) continue;
                        if (sib.offset >= oldEnd) {
                            m_doc->undoStack.push(new RcxCommand(this,
                                cmd::ChangeOffset{sib.id, sib.offset, sib.offset + delta}));
                        }
                    }
                }

                m_doc->undoStack.endMacro();
                m_suppressRefresh = false;
                refresh();
            });
            // "Ptr to New Class" — convert to typed pointer + create new root class
            if (nodeSz == 8 || nodeSz == 4) {
                menu.addAction(icon("symbol-structure.svg"), "Ptr to New Class", [this, nodeId]() {
                    convertToTypedPointer(nodeId);
                });
            }
            menu.addSeparator();
        }

        // ── Inference-based quick convert (from type hints) ──
        if (isHexNode(node.kind) && line >= 0 && line < m_lastResult.meta.size()) {
            const auto& lm = m_lastResult.meta[line];
            if (!lm.typeHintKinds.isEmpty()) {
                NodeKind suggested = lm.typeHintKinds[0];
                if (lm.typeHintKinds.size() == 1) {
                    auto* m = kindMeta(suggested);
                    QString label = QStringLiteral("Convert to %1").arg(QString::fromLatin1(m->typeName));
                    menu.addAction(label, [this, nodeId, suggested]() {
                        int ni = m_doc->tree.indexOfId(nodeId);
                        if (ni >= 0) changeNodeKind(ni, suggested);
                    });
                } else {
                    auto* m = kindMeta(lm.typeHintKinds[0]);
                    QString label = QStringLiteral("Split into %1\u00D7%2")
                        .arg(QString::fromLatin1(m->typeName))
                        .arg(lm.typeHintKinds.size());
                    menu.addAction(label, [this, nodeId, kinds = lm.typeHintKinds]() {
                        int ni = m_doc->tree.indexOfId(nodeId);
                        if (ni < 0) return;
                        changeNodeKind(ni, kinds[0]);
                        for (int k = 1; k < kinds.size(); ++k) {
                            ni = m_doc->tree.indexOfId(nodeId);
                            if (ni < 0) break;
                            int next = ni + 1;
                            if (next < m_doc->tree.nodes.size() && isHexNode(m_doc->tree.nodes[next].kind))
                                changeNodeKind(next, kinds[k]);
                        }
                    });
                }
                menu.addSeparator();
            }
        }

        // ── Quick-convert + discoverable shortcuts ──
        bool addedQuickConvert = false;

        // "Next Type" shows what → arrow does (discoverable!)
        {
            int sz = sizeForKind(node.kind);
            if (sz > 0) {
                QVector<NodeKind> variants;
                for (const auto& m : kKindMeta)
                    if (m.size == sz && !isContainerKind(m.kind)) variants.append(m.kind);
                int ci = variants.indexOf(node.kind);
                if (ci >= 0 && variants.size() > 1) {
                    auto* nextMeta = kindMeta(variants[(ci + 1) % variants.size()]);
                    menu.addAction(QStringLiteral("Next Type: %1\t\u2192")
                        .arg(QString::fromLatin1(nextMeta->typeName)),
                        [this, nodeIdx, variants, ci]() {
                            changeNodeKind(nodeIdx, variants[(ci + 1) % variants.size()]);
                        });
                    addedQuickConvert = true;
                }
            }
        }

        // Hex resize shortcut
        if (isHexNode(node.kind)) {
            static constexpr NodeKind hexCycle[] = {
                NodeKind::Hex8, NodeKind::Hex16, NodeKind::Hex32,
                NodeKind::Hex64, NodeKind::Hex128 };
            int hi = -1;
            for (int i = 0; i < 5; i++) if (hexCycle[i] == node.kind) { hi = i; break; }
            if (hi >= 0) {
                auto* nm = kindMeta(hexCycle[(hi + 1) % 5]);
                menu.addAction(QStringLiteral("Resize to %1\tSpace")
                    .arg(QString::fromLatin1(nm->typeName)),
                    [this, nodeId, hexCycle, hi]() {
                        int ni = m_doc->tree.indexOfId(nodeId);
                        if (ni >= 0) changeNodeKind(ni, hexCycle[(hi + 1) % 5]);
                    });
                addedQuickConvert = true;
            }
        }

        if (addedQuickConvert)
            menu.addSeparator();

        // ── Hex byte / ASCII inline editing ──
        if (isHexNode(node.kind) && m_doc->provider->isWritable()) {
            menu.addAction(icon("edit.svg"), "Edit He&x Bytes", [editor, line]() {
                editor->setHexEditPending(true);
                editor->beginInlineEdit(EditTarget::Value, line);
            });
            menu.addAction(icon("edit.svg"), "Edit &ASCII", [editor, line]() {
                editor->setHexEditPending(true);
                editor->beginInlineEdit(EditTarget::Name, line);
            });
            menu.addSeparator();
        }

        // ── Edit Value / Rename / Change Type ──
        bool isEditable = node.kind != NodeKind::Struct && node.kind != NodeKind::Array
                          && !isHexNode(node.kind)
                          && m_doc->provider->isWritable();
        if (isEditable) {
            menu.addAction(icon("edit.svg"), "Edit &Value\tEnter", [editor, line]() {
                editor->beginInlineEdit(EditTarget::Value, line);
            });
        }

        if (!isHexNode(node.kind)) {
            menu.addAction(icon("rename.svg"), "Re&name\tF2", [editor, line]() {
                editor->beginInlineEdit(EditTarget::Name, line);
            });
        }

        menu.addAction("Change &Type\tT", [editor, line]() {
            editor->beginInlineEdit(EditTarget::Type, line);
        });

        if (m_showComments) {
            menu.addAction(icon("edit.svg"), "&Comment\t;", [editor, line]() {
                editor->beginInlineEdit(EditTarget::Comment, line);
            });
        }

        menu.addSeparator();

        // ── Insert ► submenu ──
        {
            auto* insertMenu = menu.addMenu(icon("diff-added.svg"), "Insert");
            insertMenu->addAction("Insert 4 Above\tShift+Ins",
                [this, nodeIdx]() {
                insertNodeAbove(nodeIdx, NodeKind::Hex32, QStringLiteral("field"));
            });
            insertMenu->addAction("Insert 8 Above\tIns",
                [this, nodeIdx]() {
                insertNodeAbove(nodeIdx, NodeKind::Hex64, QStringLiteral("field"));
            });
            insertMenu->addSeparator();
            insertMenu->addAction("Append bytes...", [this, &menu]() {
                appendBytesDialog(menu.parentWidget(), m_viewRootId ? m_viewRootId : 0);
            });
        }

        // ── Convert ► submenu ──
        {
            auto* convertMenu = menu.addMenu(icon("symbol-structure.svg"), "Convert");
            bool hasConvert = false;

            // Quick-convert shortcuts (with keyboard hint)
            if (node.kind == NodeKind::Hex64) {
                convertMenu->addAction("uint64_t\tU", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId); if (ni >= 0) changeNodeKind(ni, NodeKind::UInt64); });
                convertMenu->addAction("double\tF", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId); if (ni >= 0) changeNodeKind(ni, NodeKind::Double); });
                hasConvert = true;
            } else if (node.kind == NodeKind::Hex32) {
                convertMenu->addAction("uint32_t\tU", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId); if (ni >= 0) changeNodeKind(ni, NodeKind::UInt32); });
                convertMenu->addAction("float\tF", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId); if (ni >= 0) changeNodeKind(ni, NodeKind::Float); });
                hasConvert = true;
            } else if (node.kind == NodeKind::Hex16) {
                convertMenu->addAction("int16_t\tS", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId); if (ni >= 0) changeNodeKind(ni, NodeKind::Int16); });
                hasConvert = true;
            }
            if (sizeForKind(node.kind) >= 4) {
                convertMenu->addAction("ptr\tP", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId); if (ni >= 0) changeNodeKind(ni,
                        sizeForKind(m_doc->tree.nodes[ni].kind) >= 8 ? NodeKind::Pointer64 : NodeKind::Pointer32); });
                hasConvert = true;
            }
            if (node.kind == NodeKind::Hex64 || node.kind == NodeKind::Pointer64)
                convertMenu->addAction("fnptr64", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId); if (ni >= 0) changeNodeKind(ni, NodeKind::FuncPtr64); });
            if (node.kind == NodeKind::Hex32 || node.kind == NodeKind::Pointer32)
                convertMenu->addAction("fnptr32", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId); if (ni >= 0) changeNodeKind(ni, NodeKind::FuncPtr32); });
            if (node.kind == NodeKind::FuncPtr64)
                convertMenu->addAction("ptr64", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId); if (ni >= 0) changeNodeKind(ni, NodeKind::Pointer64); });
            if (node.kind == NodeKind::FuncPtr32)
                convertMenu->addAction("ptr32", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId); if (ni >= 0) changeNodeKind(ni, NodeKind::Pointer32); });
            if (hasConvert)
                convertMenu->addSeparator();

            // "Change to ptr*" — convert any pointer-sized node to typed pointer
            {
                int sz = node.byteSize();
                bool canPtrStar = (sz == 8 || sz == 4)
                    && node.kind != NodeKind::Struct && node.kind != NodeKind::Array
                    && !(  (node.kind == NodeKind::Pointer64 || node.kind == NodeKind::Pointer32)
                         && node.refId != 0);  // already typed pointer
                if (canPtrStar) {
                    convertMenu->addAction("Change to ptr*", [this, nodeId]() {
                        convertToTypedPointer(nodeId);
                    });
                    hasConvert = true;
                }
            }

            // Split hex node into two half-sized hex nodes
            if (node.kind == NodeKind::Hex128) {
                convertMenu->addAction("Split to hex64+hex64", [this, nodeId]() {
                    splitHexNode(nodeId);
                });
                hasConvert = true;
            } else if (node.kind == NodeKind::Hex64) {
                convertMenu->addAction("Split to hex32+hex32", [this, nodeId]() {
                    splitHexNode(nodeId);
                });
                hasConvert = true;
            } else if (node.kind == NodeKind::Hex32) {
                convertMenu->addAction("Split to hex16+hex16", [this, nodeId]() {
                    splitHexNode(nodeId);
                });
                hasConvert = true;
            } else if (node.kind == NodeKind::Hex16) {
                convertMenu->addAction("Split to hex8+hex8", [this, nodeId]() {
                    splitHexNode(nodeId);
                });
                hasConvert = true;
            }

            // Convert to Hex nodes (decompose non-hex types)
            if (!isHexNode(node.kind) && node.kind != NodeKind::Struct && node.kind != NodeKind::Array) {
                convertMenu->addAction("Convert to &Hex", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId);
                    if (ni < 0) return;
                    const Node& n = m_doc->tree.nodes[ni];
                    int totalSize = n.byteSize();
                    if (totalSize <= 0) return;

                    uint64_t parentId = n.parentId;
                    int baseOffset = n.offset;

                    bool wasSuppressed = m_suppressRefresh;
                    m_suppressRefresh = true;
                    m_doc->undoStack.beginMacro(QStringLiteral("Convert to Hex"));

                    QVector<Node> subtree;
                    subtree.append(n);
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::Remove{nodeId, subtree, {}}));

                    int padOffset = baseOffset;
                    int gap = totalSize;
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
                });
                hasConvert = true;
            }

            if (!hasConvert)
                convertMenu->setEnabled(false);
        }

        // ── Structure ► submenu (only when relevant) ──
        {
            auto* structMenu = menu.addMenu("Static");
            bool hasStructAction = false;

            if (node.kind == NodeKind::Struct || node.kind == NodeKind::Array) {
                structMenu->addAction(icon("diff-added.svg"), "Add &Child", [this, nodeId]() {
                    insertNode(nodeId, 0, NodeKind::Hex64, "newField");
                });
                structMenu->addAction("Add Static Field", [this, nodeId]() {
                    insertStaticField(nodeId);
                });
                if (node.collapsed) {
                    structMenu->addAction(icon("expand-all.svg"), "&Expand", [this, nodeId]() {
                        int ni = m_doc->tree.indexOfId(nodeId);
                        if (ni >= 0) toggleCollapse(ni);
                    });
                } else {
                    structMenu->addAction(icon("collapse-all.svg"), "&Collapse", [this, nodeId]() {
                        int ni = m_doc->tree.indexOfId(nodeId);
                        if (ni >= 0) toggleCollapse(ni);
                    });
                }
                hasStructAction = true;
            }

            // Add Static Field as sibling (for child nodes of a struct)
            if (node.parentId != 0 && node.kind != NodeKind::Struct && node.kind != NodeKind::Array) {
                uint64_t pid = node.parentId;
                int pi = m_doc->tree.indexOfId(pid);
                if (pi >= 0 && (m_doc->tree.nodes[pi].kind == NodeKind::Struct
                             || m_doc->tree.nodes[pi].kind == NodeKind::Array)) {
                    structMenu->addAction("Add Static Field", [this, pid]() {
                        insertStaticField(pid);
                    });
                    hasStructAction = true;
                }
            }

            // Static field: Edit Expression
            if (node.isStatic) {
                structMenu->addAction("Edit E&xpression", [this, editor, line, nodeId]() {
                    QStringList completions;
                    completions << QStringLiteral("base");
                    int ni = m_doc->tree.indexOfId(nodeId);
                    if (ni >= 0) {
                        uint64_t pid = m_doc->tree.nodes[ni].parentId;
                        for (const Node& sib : m_doc->tree.nodes) {
                            if (sib.parentId == pid && !sib.isStatic && !sib.name.isEmpty())
                                completions << sib.name;
                        }
                    }
                    editor->setStaticCompletions(completions);
                    editor->beginInlineEdit(EditTarget::StaticExpr, line);
                });
                hasStructAction = true;
            }

            // Dissolve Union
            {
                uint64_t targetUnionId = 0;
                if (node.kind == NodeKind::Struct
                    && node.isUnion()) {
                    targetUnionId = nodeId;
                } else if (node.parentId != 0) {
                    int pi = m_doc->tree.indexOfId(node.parentId);
                    if (pi >= 0 && m_doc->tree.nodes[pi].kind == NodeKind::Struct
                        && m_doc->tree.nodes[pi].isUnion()) {
                        targetUnionId = node.parentId;
                    }
                }
                if (targetUnionId != 0) {
                    structMenu->addAction("Dissolve Union", [this, targetUnionId]() {
                        dissolveUnion(targetUnionId);
                    });
                    hasStructAction = true;
                }
            }

            if (!hasStructAction)
                structMenu->setEnabled(false);
        }

        menu.addSeparator();

        // ── Duplicate / Delete ──
        menu.addAction(icon("files.svg"), "D&uplicate\tCtrl+D", [this, nodeId]() {
            int ni = m_doc->tree.indexOfId(nodeId);
            if (ni >= 0) duplicateNode(ni);
        });
        menu.addAction(icon("trash.svg"), "&Delete\tDelete", [this, nodeId]() {
            int ni = m_doc->tree.indexOfId(nodeId);
            if (ni >= 0) removeNode(ni);
        });

        menu.addSeparator();

        // ── Tracking ──
        {
            auto* act = menu.addAction("Track Value Changes");
            act->setCheckable(true);
            act->setChecked(m_trackValues);
            connect(act, &QAction::toggled, this, &RcxController::setTrackValues);
        }
        {
            auto* act = menu.addAction("Clear Value History");
            act->setToolTip(QStringLiteral("Reset change tracking for this node"));
            connect(act, &QAction::triggered, this, [this, nodeId]() {
                m_valueHistory.remove(nodeId);
                m_lastValueAddr.remove(nodeId);
                for (int ci : m_doc->tree.subtreeIndices(nodeId)) {
                    m_valueHistory.remove(m_doc->tree.nodes[ci].id);
                    m_lastValueAddr.remove(m_doc->tree.nodes[ci].id);
                }
                m_refreshGen++;
                m_prevPages.clear();
                m_changedOffsets.clear();
                m_valueTrackCooldown = 5;
                refresh();
                for (auto* editor : m_editors)
                    editor->dismissHistoryPopup();
            });
        }

        menu.addSeparator();
        } // else (non-member node actions)
    }

    // ── Always-available actions ──

    if (!hasNode) {
        // Insert submenu for empty area
        auto* insertMenu = menu.addMenu(icon("diff-added.svg"), "Insert");
        insertMenu->addAction("Insert 4", [this]() {
            uint64_t target = m_viewRootId ? m_viewRootId : 0;
            insertNode(target, -1, NodeKind::Hex32, QStringLiteral("field"));
        });
        insertMenu->addAction("Insert 8", [this]() {
            uint64_t target = m_viewRootId ? m_viewRootId : 0;
            insertNode(target, -1, NodeKind::Hex64, QStringLiteral("field"));
        });
        insertMenu->addSeparator();
        insertMenu->addAction("Append bytes...", [this, &menu]() {
            appendBytesDialog(menu.parentWidget(), m_viewRootId ? m_viewRootId : 0);
        });

        // Add Static Field to current view root
        if (m_viewRootId != 0) {
            int ri = m_doc->tree.indexOfId(m_viewRootId);
            if (ri >= 0 && (m_doc->tree.nodes[ri].kind == NodeKind::Struct
                         || m_doc->tree.nodes[ri].kind == NodeKind::Array)) {
                uint64_t rootId = m_viewRootId;
                menu.addAction("Add Static Field", [this, rootId]() {
                    insertStaticField(rootId);
                });
            }
        }

        menu.addSeparator();

        auto* act = menu.addAction("Track Value Changes");
        act->setCheckable(true);
        act->setChecked(m_trackValues);
        connect(act, &QAction::toggled, this, &RcxController::setTrackValues);

        menu.addSeparator();
    }

    // ── Fold ──
    {
        auto* foldMenu = menu.addMenu("Fold");
        foldMenu->addAction("Collapse All\tCtrl+Shift+[", [this, editor]() {
            emit editor->collapseAllRequested();
        });
        foldMenu->addAction("Expand All\tCtrl+Shift+]", [this, editor]() {
            emit editor->expandAllRequested();
        });
    }

    // ── Copy ──
    QMenu* copyMenu = menu.addMenu(icon("clippy.svg"), "Copy");
    if (hasNode) {
        uint64_t copyNodeId = m_doc->tree.nodes[nodeIdx].id;
        copyMenu->addAction(icon("link.svg"), "Copy &Address\tCtrl+C", [this, copyNodeId]() {
            int ni = m_doc->tree.indexOfId(copyNodeId);
            if (ni < 0) return;
            int64_t off = m_doc->tree.computeOffset(ni);
            if (off < 0) return;
            uint64_t addr = m_doc->tree.baseAddress + static_cast<uint64_t>(off);
            QApplication::clipboard()->setText(
                QStringLiteral("0x") + QString::number(addr, 16).toUpper());
        });
        copyMenu->addAction(icon("whole-word.svg"), "Copy &Offset", [this, copyNodeId]() {
            int ni = m_doc->tree.indexOfId(copyNodeId);
            if (ni < 0) return;
            int off = m_doc->tree.nodes[ni].offset;
            QApplication::clipboard()->setText(
                QStringLiteral("+0x") + QString::number(off, 16).toUpper().rightJustified(4, '0'));
        });
        copyMenu->addSeparator();
    }
    copyMenu->addAction("Copy Line\tCtrl+X", [editor, line]() {
        auto* sci = editor->scintilla();
        int len = (int)sci->SendScintilla(QsciScintillaBase::SCI_LINELENGTH, (unsigned long)line);
        if (len > 0) {
            QByteArray buf(len + 1, '\0');
            sci->SendScintilla(QsciScintillaBase::SCI_GETLINE, (unsigned long)line, (void*)buf.data());
            QString text = QString::fromUtf8(buf.data(), len).trimmed();
            if (!text.isEmpty())
                QApplication::clipboard()->setText(text);
        }
    });
    copyMenu->addAction("Copy All as Text", [editor]() {
        QApplication::clipboard()->setText(editor->textWithMargins());
    });

    menu.addSeparator();

    menu.addAction(icon("search.svg"), "Search...\tCtrl+F", [editor]() {
        QTimer::singleShot(0, editor, &RcxEditor::showFindBar);
    });
    menu.addAction(icon("arrow-left.svg"), "Undo\tCtrl+Z", [this]() {
        m_doc->undoStack.undo();
    })->setEnabled(m_doc->undoStack.canUndo());
    menu.addAction(icon("arrow-right.svg"), "Redo\tCtrl+Y", [this]() {
        m_doc->undoStack.redo();
    })->setEnabled(m_doc->undoStack.canRedo());

    // ── Kernel paging menu items ──
    if (m_doc->provider && m_doc->provider->hasKernelPaging()) {
        menu.addSeparator();
        auto* kernelMenu = menu.addMenu(icon("symbol-key.svg"), "Kernel");

        // Show Physical Address — translate the node's VA to physical
        if (hasNode) {
            int64_t nodeOff = m_doc->tree.computeOffset(nodeIdx);
            uint64_t nodeAddr = (nodeOff >= 0)
                ? m_doc->tree.baseAddress + static_cast<uint64_t>(nodeOff) : 0;
            kernelMenu->addAction("Show Physical Address", [this, nodeAddr, &menu]() {
                auto result = m_doc->provider->translateAddress(nodeAddr);
                if (result.valid) {
                    const char* pageSz = result.pageSize == 2 ? "1 GB"
                                       : result.pageSize == 1 ? "2 MB" : "4 KB";
                    QString msg = QStringLiteral(
                        "Virtual:   0x%1\n"
                        "Physical:  0x%2\n"
                        "Page Size: %3\n\n"
                        "PML4E:  0x%4\n"
                        "PDPTE:  0x%5\n"
                        "PDE:    0x%6\n"
                        "PTE:    0x%7")
                        .arg(nodeAddr, 16, 16, QChar('0'))
                        .arg(result.physical, 16, 16, QChar('0'))
                        .arg(pageSz)
                        .arg(result.pml4e, 16, 16, QChar('0'))
                        .arg(result.pdpte, 16, 16, QChar('0'))
                        .arg(result.pde, 16, 16, QChar('0'))
                        .arg(result.pte, 16, 16, QChar('0'));
                    QMessageBox::information(
                        qobject_cast<QWidget*>(parent()),
                        QStringLiteral("Physical Address"), msg);
                } else {
                    QMessageBox::warning(
                        qobject_cast<QWidget*>(parent()),
                        QStringLiteral("Translation Failed"),
                        QStringLiteral("Address 0x%1 is not mapped")
                            .arg(nodeAddr, 16, 16, QChar('0')));
                }
            });
        }

        // Browse Page Tables — open PML4 in a new physical tab
        kernelMenu->addAction("Browse Page Tables", [this]() {
            uint64_t cr3 = m_doc->provider->getCr3();
            if (cr3 == 0) {
                QMessageBox::warning(qobject_cast<QWidget*>(parent()),
                    QStringLiteral("Error"),
                    QStringLiteral("Failed to read CR3"));
                return;
            }
            emit requestOpenProviderTab(
                QStringLiteral("kernelmemory"),
                QStringLiteral("phys:%1").arg(cr3, 0, 16),
                QStringLiteral("PML4 @ 0x%1").arg(cr3, 0, 16));
        });

        // Follow Physical Frame — on a PTE bitfield, extract PhysAddr and open
        if (hasNode) {
            const auto& node = m_doc->tree.nodes[nodeIdx];
            if (node.isBitfield()) {
                for (const auto& bf : node.bitfieldMembers) {
                    if (bf.name == QStringLiteral("PhysAddr")) {
                        int bitOff = bf.bitOffset;
                        int bitWid = bf.bitWidth;
                        int64_t nodeOff = m_doc->tree.computeOffset(nodeIdx);
                        if (nodeOff < 0) break;
                        uint64_t nodeAddr = m_doc->tree.baseAddress
                            + static_cast<uint64_t>(nodeOff);
                        kernelMenu->addAction("Follow Physical Frame",
                            [this, nodeAddr, bitOff, bitWid]() {
                            uint64_t pteValue = 0;
                            if (!m_doc->provider->read(nodeAddr, &pteValue, 8)) {
                                QMessageBox::warning(qobject_cast<QWidget*>(parent()),
                                    QStringLiteral("Error"),
                                    QStringLiteral("Failed to read PTE at 0x%1")
                                        .arg(nodeAddr, 0, 16));
                                return;
                            }
                            uint64_t mask = (1ULL << bitWid) - 1;
                            uint64_t frame = ((pteValue >> bitOff) & mask) << bitOff;
                            if (frame == 0) {
                                QMessageBox::warning(qobject_cast<QWidget*>(parent()),
                                    QStringLiteral("Error"),
                                    QStringLiteral("Physical frame is zero (not present?)"));
                                return;
                            }
                            emit requestOpenProviderTab(
                                QStringLiteral("kernelmemory"),
                                QStringLiteral("phys:%1").arg(frame, 0, 16),
                                QStringLiteral("PT @ 0x%1").arg(frame, 0, 16));
                        });
                        break;
                    }
                }
            }
        }
    }

    emit contextMenuAboutToShow(&menu, line);
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

    // Compute effective selection ID:
    //   footers        → nodeId | kFooterIdBit
    //   array elements → nodeId | kArrayElemBit | (elemIdx << 48)
    //   everything else → nodeId
    auto effectiveId = [this](int ln, uint64_t nid) -> uint64_t {
        if (ln < 0 || ln >= m_lastResult.meta.size()) return nid;
        const auto& lm = m_lastResult.meta[ln];
        if (lm.lineKind == LineKind::Footer)
            return nid | kFooterIdBit;
        if (lm.isArrayElement && lm.arrayElementIdx >= 0)
            return makeArrayElemSelId(nid, lm.arrayElementIdx);
        if (lm.isMemberLine && lm.subLine >= 0)
            return makeMemberSelId(nid, lm.subLine);
        return nid;
    };

    // Escape / deselect: nodeId=0 means clear selection
    if (nodeId == 0) {
        clearSelection();
        return;
    }

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

    updateCommandRow();
    applySelectionOverlays();

    if (m_selIds.size() == 1) {
        uint64_t sid = *m_selIds.begin();
        // Strip footer/array/member bits for node lookup
        int idx = m_doc->tree.indexOfId(sid & ~(kFooterIdBit | kArrayElemBit | kArrayElemMask
                                                | kMemberBit | kMemberSubMask));
        if (idx >= 0) emit nodeSelected(idx);
    }
}

void RcxController::clearSelection() {
    m_selIds.clear();
    m_anchorLine = -1;
    updateCommandRow();
    applySelectionOverlays();
}

void RcxController::applySelectionOverlays() {
    for (auto* editor : m_editors)
        editor->applySelectionOverlay(m_selIds);
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

    QString addr;
    if (!m_doc->tree.baseAddressFormula.isEmpty())
        addr = m_doc->tree.baseAddressFormula;
    else
        addr = QStringLiteral("0x") +
            QString::number(m_doc->tree.baseAddress, 16).toUpper();

    QString row = QStringLiteral("%1  %2")
        .arg(elide(src, 40), elide(addr, 24));

    // Build row 2: root class type + name (uses current view root)
    QString brace = m_braceWrap ? QString() : QStringLiteral(" {");
    QString row2;
    if (m_viewRootId != 0) {
        int vi = m_doc->tree.indexOfId(m_viewRootId);
        if (vi >= 0) {
            const auto& n = m_doc->tree.nodes[vi];
            QString keyword = n.resolvedClassKeyword();
            QString className = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
            row2 = QStringLiteral("%1 %2%3")
                .arg(keyword, className.isEmpty() ? QStringLiteral("NoName") : className, brace);
        }
    }
    if (row2.isEmpty()) {
        // Fallback: find first root struct
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const auto& n = m_doc->tree.nodes[i];
            if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                QString keyword = n.resolvedClassKeyword();
                QString className = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
                row2 = QStringLiteral("%1 %2%3")
                    .arg(keyword, className.isEmpty() ? QStringLiteral("NoName") : className, brace);
                break;
            }
        }
    }
    if (row2.isEmpty())
        row2 = QStringLiteral("struct NoName") + brace;

    // Append struct total size
    uint64_t sizeRootId = m_viewRootId;
    if (sizeRootId == 0) {
        for (const auto& n : m_doc->tree.nodes)
            if (n.parentId == 0 && n.kind == NodeKind::Struct) { sizeRootId = n.id; break; }
    }
    if (sizeRootId != 0) {
        int sz = m_doc->tree.structSpan(sizeRootId);
        if (sz > 0)
            row2 += QStringLiteral("  // 0x%1").arg(QString::number(sz, 16).toUpper());
    }

    QString combined = QStringLiteral("[\u25B8] ") + row + QStringLiteral("  ") + row2;

    for (auto* ed : m_editors) {
        ed->setCommandRowText(combined);
    }
    emit selectionChanged(m_selIds.size());
}

TypeSelectorPopup* RcxController::ensurePopup(RcxEditor* editor) {
    if (!m_cachedPopup) {
        m_cachedPopup = new TypeSelectorPopup(editor);
        // Keep popup colors in sync when theme changes
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                m_cachedPopup, &TypeSelectorPopup::applyTheme);
        // Pre-warm: force native window creation so first visible show is fast
        m_cachedPopup->warmUp();
    }
    // Disconnect previous signals so we can reconnect fresh
    m_cachedPopup->disconnect(this);
    return m_cachedPopup;
}

void RcxController::showTypePopup(RcxEditor* editor, TypePopupMode mode,
                                  int nodeIdx, QPoint globalPos) {
    const Node* node = nullptr;
    if (nodeIdx >= 0 && nodeIdx < (int)m_doc->tree.nodes.size())
        node = &m_doc->tree.nodes[nodeIdx];

    // ── Determine modifier preset (cheap — only reads node properties) ──
    int preModId = 0;
    int preArrayCount = 0;
    if (mode == TypePopupMode::FieldType && node) {
        bool isPtr = (node->kind == NodeKind::Pointer32 || node->kind == NodeKind::Pointer64);
        bool isPrimPtr  = isPtr && node->ptrDepth > 0 && node->refId == 0;
        bool isTypedPtr = isPtr && node->refId != 0;
        bool isArray = node->kind == NodeKind::Array;
        if (isPrimPtr)       preModId = (node->ptrDepth >= 2) ? 2 : 1;
        else if (isTypedPtr) preModId = 1;
        else if (isArray)  { preModId = 3; preArrayCount = node->arrayLen; }
    }

    // ── Node size for same-size sorting (cheap) ──
    int nodeSize = 0;
    if (node) {
        if (mode == TypePopupMode::ArrayElement)
            nodeSize = sizeForKind(node->elementKind);
        else
            nodeSize = sizeForKind(node->kind);
    }

    // ── Font with zoom ──
    QSettings settings("Reclass", "Reclass");
    QString fontName = settings.value("font", "JetBrains Mono").toString();
    QFont font(fontName, 12);
    font.setFixedPitch(true);
    auto* sci = editor->scintilla();
    int zoom = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETZOOM);
    font.setPointSize(font.pointSize() + zoom);

    // ── Position ──
    QPoint pos = globalPos;
    if (mode == TypePopupMode::Root) {
        long lineStart = sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, 0);
        int lineH = (int)sci->SendScintilla(QsciScintillaBase::SCI_TEXTHEIGHT, 0);
        int x = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION,
                                         0, lineStart);
        int y = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION,
                                         0, lineStart);
        pos = sci->viewport()->mapToGlobal(QPoint(x, y + lineH));
    }

    // ── Configure popup + show skeleton instantly ──
    auto* popup = ensurePopup(editor);
    popup->setFont(font);
    popup->setMode(mode);
    if (preModId > 0)
        popup->setModifier(preModId, preArrayCount);
    popup->setCurrentNodeSize(nodeSize);
    popup->setPointerSize(m_doc->tree.pointerSize);

    connect(popup, &TypeSelectorPopup::typeSelected,
            this, [this, mode, nodeIdx](const TypeEntry& entry, const QString& fullText) {
        applyTypePopupResult(mode, nodeIdx, entry, fullText);
    });
    connect(popup, &TypeSelectorPopup::createNewTypeRequested,
            this, [this, mode, nodeIdx](int modifierId, int arrayCount) {
        bool wasSuppressed = m_suppressRefresh;
        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Create new type"));

        QString baseName = QStringLiteral("NewClass");
        QString typeName = baseName;
        int counter = 1;
        QSet<QString> existing;
        for (const auto& nd : m_doc->tree.nodes) {
            if (nd.kind == NodeKind::Struct && !nd.structTypeName.isEmpty())
                existing.insert(nd.structTypeName);
        }
        while (existing.contains(typeName))
            typeName = baseName + QString::number(counter++);

        Node n;
        n.kind = NodeKind::Struct;
        n.structTypeName = typeName;
        n.name = QStringLiteral("instance");
        n.parentId = 0;
        n.offset = 0;
        n.id = m_doc->tree.reserveId();
        m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{n}));

        for (int i = 0; i < 8; i++) {
            insertNode(n.id, i * 8, NodeKind::Hex64,
                       QString("field_%1").arg(i * 8, 2, 16, QChar('0')));
        }

        m_doc->undoStack.endMacro();
        m_suppressRefresh = wasSuppressed;

        TypeEntry newEntry;
        newEntry.entryKind = TypeEntry::Composite;
        newEntry.structId  = n.id;

        // Build fullText with modifier suffix so applyTypePopupResult
        // wraps the new type as pointer/array accordingly
        QString fullText = typeName;
        if (modifierId == 1)
            fullText += QStringLiteral("*");
        else if (modifierId == 2)
            fullText += QStringLiteral("**");
        else if (modifierId == 3 && arrayCount > 0)
            fullText += QStringLiteral("[%1]").arg(arrayCount);

        applyTypePopupResult(mode, nodeIdx, newEntry, fullText);
    });

    popup->popupLoading(pos);

    // ── Deferred: build entry list + fill content (runs next event-loop tick) ──
    int gen = ++m_typePopupGen;
    QTimer::singleShot(0, this, [this, popup, mode, nodeIdx, gen]() {
        if (gen != m_typePopupGen) return;  // popup was reopened, discard stale load

        const Node* node = nullptr;
        if (nodeIdx >= 0 && nodeIdx < (int)m_doc->tree.nodes.size())
            node = &m_doc->tree.nodes[nodeIdx];

        QVector<TypeEntry> entries;
        TypeEntry currentEntry;
        bool hasCurrent = false;

        auto addPrimitives = [&](bool enabled, bool excludeStructArrayPad) {
            for (const auto& m : kKindMeta) {
                if (excludeStructArrayPad &&
                    (m.kind == NodeKind::Struct || m.kind == NodeKind::Array))
                    continue;
                TypeEntry e;
                e.entryKind     = TypeEntry::Primitive;
                e.primitiveKind = m.kind;
                e.displayName   = QString::fromLatin1(m.typeName);
                e.enabled       = enabled;
                e.sizeBytes     = m.size;
                e.alignment     = m.align;
                entries.append(e);
            }
        };

        auto addComposites = [&](const std::function<bool(const Node&, const TypeEntry&)>& isCurrent) {
            for (const auto& n : m_doc->tree.nodes) {
                if (n.parentId != 0 || n.kind != NodeKind::Struct) continue;
                TypeEntry e;
                e.entryKind    = TypeEntry::Composite;
                e.structId     = n.id;
                e.displayName  = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
                e.classKeyword = n.resolvedClassKeyword();
                e.category     = (e.classKeyword == QStringLiteral("enum"))
                               ? TypeEntry::CatEnum : TypeEntry::CatType;
                e.sizeBytes    = m_doc->tree.structSpan(n.id);

                QVector<int> kids = m_doc->tree.childrenOf(n.id);
                int nonStaticCount = 0;
                int maxAlign = 1;
                for (int i = 0; i < kids.size(); i++) {
                    const Node& child = m_doc->tree.nodes[kids[i]];
                    if (child.isStatic) continue;
                    nonStaticCount++;
                    int childAlign = alignmentFor(child.kind);
                    if (childAlign > maxAlign) maxAlign = childAlign;
                    if (e.fieldSummary.size() < 6) {
                        auto* cm = kindMeta(child.kind);
                        QString typeName = cm ? QString::fromLatin1(cm->typeName)
                                              : QStringLiteral("???");
                        if (child.kind == NodeKind::Struct && child.refId != 0) {
                            int refIdx = m_doc->tree.indexOfId(child.refId);
                            if (refIdx >= 0) {
                                const Node& ref = m_doc->tree.nodes[refIdx];
                                typeName = ref.structTypeName.isEmpty()
                                         ? ref.name : ref.structTypeName;
                            }
                        }
                        e.fieldSummary << QStringLiteral("0x%1: %2 %3")
                            .arg(child.offset, 2, 16, QChar('0'))
                            .arg(typeName, child.name);
                    }
                }
                e.fieldCount = nonStaticCount;
                e.alignment  = maxAlign;

                entries.append(e);
                if (!hasCurrent && node && isCurrent(*node, e)) {
                    currentEntry = e;
                    hasCurrent = true;
                }
            }
        };

        switch (mode) {
        case TypePopupMode::Root:
            addComposites([this](const Node&, const TypeEntry& e) {
                return e.structId == m_viewRootId;
            });
            break;

        case TypePopupMode::FieldType: {
            addPrimitives(/*enabled=*/true, /*excludeStructArrayPad=*/true);
            bool isPtr = node
                && (node->kind == NodeKind::Pointer32 || node->kind == NodeKind::Pointer64);
            bool isTypedPtr = isPtr && node->refId != 0;
            bool isPrimPtr  = isPtr && node->ptrDepth > 0 && node->refId == 0;
            bool isArray = node && node->kind == NodeKind::Array;

            if (isPrimPtr) {
                for (auto& e : entries) {
                    if (e.entryKind == TypeEntry::Primitive && e.primitiveKind == node->elementKind) {
                        currentEntry = e;
                        hasCurrent = true;
                        break;
                    }
                }
            } else if (isTypedPtr) {
                // current set by addComposites below
            } else if (isArray) {
                if (node->elementKind != NodeKind::Struct) {
                    for (auto& e : entries) {
                        if (e.entryKind == TypeEntry::Primitive && e.primitiveKind == node->elementKind) {
                            currentEntry = e;
                            hasCurrent = true;
                            break;
                        }
                    }
                }
            } else if (node) {
                if (!(node->kind == NodeKind::Struct && node->refId != 0)) {
                    for (auto& e : entries) {
                        if (e.entryKind == TypeEntry::Primitive && e.primitiveKind == node->kind) {
                            currentEntry = e;
                            hasCurrent = true;
                            break;
                        }
                    }
                }
            }
            addComposites([&](const Node& n, const TypeEntry& e) {
                if (isTypedPtr && n.refId == e.structId) return true;
                if (isArray && n.elementKind == NodeKind::Struct && n.refId == e.structId) return true;
                if (!isPtr && !isArray && n.kind == NodeKind::Struct && n.refId == e.structId) return true;
                return false;
            });
            break;
        }

        case TypePopupMode::ArrayElement:
            addPrimitives(/*enabled=*/true, /*excludeStructArrayPad=*/true);
            if (node) {
                for (auto& e : entries) {
                    if (e.entryKind == TypeEntry::Primitive && e.primitiveKind == node->elementKind) {
                        currentEntry = e;
                        hasCurrent = true;
                        break;
                    }
                }
            }
            addComposites([](const Node& n, const TypeEntry& e) {
                return n.elementKind == NodeKind::Struct && n.refId == e.structId;
            });
            break;

        case TypePopupMode::PointerTarget: {
            TypeEntry voidEntry;
            voidEntry.entryKind     = TypeEntry::Primitive;
            voidEntry.primitiveKind = NodeKind::Hex8;
            voidEntry.displayName   = QStringLiteral("void");
            voidEntry.enabled       = true;
            entries.append(voidEntry);
            addPrimitives(/*enabled=*/true, /*excludeStructArrayPad=*/true);
            if (node && node->refId == 0 && node->ptrDepth <= 1) {
                currentEntry = voidEntry;
                hasCurrent = true;
            } else if (node && node->refId == 0 && node->ptrDepth > 0) {
                for (auto& e : entries) {
                    if (e.entryKind == TypeEntry::Primitive && e.primitiveKind == node->elementKind) {
                        currentEntry = e;
                        hasCurrent = true;
                        break;
                    }
                }
            }
            addComposites([](const Node& n, const TypeEntry& e) {
                return n.refId == e.structId;
            });
            break;
        }
        }

        // Add types from other open documents
        if (m_projectDocs) {
            QSet<QString> localNames;
            for (const auto& e : entries)
                if (e.entryKind == TypeEntry::Composite)
                    localNames.insert(e.displayName);
            for (auto* doc : *m_projectDocs) {
                if (doc == m_doc) continue;
                for (const auto& n : doc->tree.nodes) {
                    if (n.parentId != 0 || n.kind != NodeKind::Struct) continue;
                    QString name = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
                    if (name.isEmpty() || localNames.contains(name)) continue;
                    localNames.insert(name);
                    TypeEntry e;
                    e.entryKind    = TypeEntry::Composite;
                    e.structId     = 0;
                    e.displayName  = name;
                    e.classKeyword = n.resolvedClassKeyword();
                    e.category     = (e.classKeyword == QStringLiteral("enum"))
                                   ? TypeEntry::CatEnum : TypeEntry::CatType;
                    e.sizeBytes    = doc->tree.structSpan(n.id);
                    entries.append(e);
                }
            }
        }

        popup->setTypes(entries, hasCurrent ? &currentEntry : nullptr);
    });
}

void RcxController::applyTypePopupResult(TypePopupMode mode, int nodeIdx,
                                         const TypeEntry& entry, const QString& fullText) {
    // Resolve external types: structId==0 means from another document, import first
    TypeEntry resolved = entry;
    if (resolved.entryKind == TypeEntry::Composite && resolved.structId == 0
        && !resolved.displayName.isEmpty()) {
        resolved.structId = findOrCreateStructByName(resolved.displayName);
    }

    if (mode == TypePopupMode::Root) {
        if (resolved.entryKind == TypeEntry::Composite)
            setViewRootId(resolved.structId);
        return;
    }

    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;

    // BUG-1 fix: Copy needed fields to locals before any mutation.
    // changeNodeKind() can trigger insertNode() → addNode() → nodes.append(),
    // which may reallocate the QVector, invalidating any reference into it.
    const uint64_t nodeId   = m_doc->tree.nodes[nodeIdx].id;
    const NodeKind nodeKind = m_doc->tree.nodes[nodeIdx].kind;
    const NodeKind elemKind = m_doc->tree.nodes[nodeIdx].elementKind;
    const uint64_t nodeRefId = m_doc->tree.nodes[nodeIdx].refId;
    const int      arrLen   = m_doc->tree.nodes[nodeIdx].arrayLen;

    // Parse the full text for modifiers (e.g. "int32_t[10]", "Ball*")
    TypeSpec spec = parseTypeSpec(fullText);

    if (mode == TypePopupMode::FieldType) {
        // Capture old effective size before any mutations (for sibling offset adjustment)
        const uint64_t parentId = m_doc->tree.nodes[nodeIdx].parentId;
        const int nodeOffset = m_doc->tree.nodes[nodeIdx].offset;
        int oldEffectiveSize = m_doc->tree.nodes[nodeIdx].byteSize();
        if (oldEffectiveSize == 0 && (nodeKind == NodeKind::Struct || nodeKind == NodeKind::Array))
            oldEffectiveSize = m_doc->tree.structSpan(nodeId);

        if (resolved.entryKind == TypeEntry::Primitive) {
            if (spec.arrayCount > 0) {
                // Primitive array: e.g. "int32_t[10]"
                bool wasSuppressed = m_suppressRefresh;
                m_suppressRefresh = true;
                m_doc->undoStack.beginMacro(QStringLiteral("Change to primitive array"));
                if (nodeKind != NodeKind::Array)
                    changeNodeKind(nodeIdx, NodeKind::Array);
                int idx = m_doc->tree.indexOfId(nodeId);
                if (idx >= 0) {
                    auto& n = m_doc->tree.nodes[idx];
                    if (n.elementKind != resolved.primitiveKind || n.arrayLen != spec.arrayCount)
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ChangeArrayMeta{nodeId, n.elementKind, resolved.primitiveKind,
                                                 n.arrayLen, spec.arrayCount}));
                }
                m_doc->undoStack.endMacro();
                m_suppressRefresh = wasSuppressed;
                if (!m_suppressRefresh) refresh();
            } else if (spec.isPointer) {
                if (!isValidPrimitivePtrTarget(resolved.primitiveKind)) {
                    // Hex, pointer, fnptr types with * → plain void pointer
                    if (nodeKind != NodeKind::Pointer64)
                        changeNodeKind(nodeIdx, NodeKind::Pointer64);
                    int idx = m_doc->tree.indexOfId(nodeId);
                    if (idx >= 0) {
                        auto& n = m_doc->tree.nodes[idx];
                        n.ptrDepth = 0;
                        if (n.refId != 0)
                            m_doc->undoStack.push(new RcxCommand(this,
                                cmd::ChangePointerRef{nodeId, n.refId, 0}));
                    }
                } else {
                    // Primitive pointer: e.g. "int32*" or "f64**" → Pointer64 + elementKind + ptrDepth
                    bool wasSuppressed = m_suppressRefresh;
                    m_suppressRefresh = true;
                    m_doc->undoStack.beginMacro(QStringLiteral("Change to primitive pointer"));
                    if (nodeKind != NodeKind::Pointer64)
                        changeNodeKind(nodeIdx, NodeKind::Pointer64);
                    int idx = m_doc->tree.indexOfId(nodeId);
                    if (idx >= 0) {
                        auto& n = m_doc->tree.nodes[idx];
                        if (n.elementKind != resolved.primitiveKind || n.ptrDepth != spec.ptrDepth) {
                            NodeKind oldEK = n.elementKind;
                            int oldDepth = n.ptrDepth;
                            n.elementKind = resolved.primitiveKind;
                            n.ptrDepth = spec.ptrDepth;
                            if (n.refId != 0)
                                m_doc->undoStack.push(new RcxCommand(this,
                                    cmd::ChangePointerRef{nodeId, n.refId, 0}));
                            Q_UNUSED(oldEK); Q_UNUSED(oldDepth);
                        }
                    }
                    m_doc->undoStack.endMacro();
                    m_suppressRefresh = wasSuppressed;
                    if (!m_suppressRefresh) refresh();
                }
            } else {
                if (resolved.primitiveKind != nodeKind)
                    changeNodeKind(nodeIdx, resolved.primitiveKind);
            }
        } else if (resolved.entryKind == TypeEntry::Composite) {
            bool wasSuppressed = m_suppressRefresh;
            m_suppressRefresh = true;
            m_doc->undoStack.beginMacro(QStringLiteral("Change to composite type"));

            if (spec.isPointer) {
                // Pointer modifier: e.g. "Material*" or "Material**" → Pointer64 + refId + ptrDepth
                if (nodeKind != NodeKind::Pointer64)
                    changeNodeKind(nodeIdx, NodeKind::Pointer64);
                int idx = m_doc->tree.indexOfId(nodeId);
                if (idx >= 0) {
                    auto& n = m_doc->tree.nodes[idx];
                    // ptrDepth: 0 = single struct pointer (*), 1+ = extra indirection levels (**)
                    int newDepth = qMax(0, spec.ptrDepth - 1);
                    if (n.ptrDepth != newDepth)
                        n.ptrDepth = newDepth;
                    if (n.refId != resolved.structId)
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ChangePointerRef{nodeId, n.refId, resolved.structId}));
                }

            } else if (spec.arrayCount > 0) {
                // Array modifier: e.g. "Material[10]" → Array + Struct element
                if (nodeKind != NodeKind::Array)
                    changeNodeKind(nodeIdx, NodeKind::Array);
                int idx = m_doc->tree.indexOfId(nodeId);
                if (idx >= 0) {
                    auto& n = m_doc->tree.nodes[idx];
                    if (n.elementKind != NodeKind::Struct || n.arrayLen != spec.arrayCount)
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ChangeArrayMeta{nodeId, n.elementKind, NodeKind::Struct,
                                                 n.arrayLen, spec.arrayCount}));
                    if (n.refId != resolved.structId)
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ChangePointerRef{nodeId, n.refId, resolved.structId}));
                }

            } else {
                // Plain struct: e.g. "Material" → Struct + structTypeName + refId + collapsed
                if (nodeKind != NodeKind::Struct)
                    changeNodeKind(nodeIdx, NodeKind::Struct);
                int idx = m_doc->tree.indexOfId(nodeId);
                if (idx >= 0) {
                    int refIdx = m_doc->tree.indexOfId(resolved.structId);
                    QString targetName;
                    if (refIdx >= 0) {
                        const Node& ref = m_doc->tree.nodes[refIdx];
                        targetName = ref.structTypeName.isEmpty() ? ref.name : ref.structTypeName;
                    }
                    QString oldTypeName = m_doc->tree.nodes[idx].structTypeName;
                    if (oldTypeName != targetName)
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ChangeStructTypeName{nodeId, oldTypeName, targetName}));
                    // Set refId so compose can expand the referenced struct's children
                    if (m_doc->tree.nodes[idx].refId != resolved.structId)
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ChangePointerRef{nodeId, m_doc->tree.nodes[idx].refId, resolved.structId}));
                    // ChangePointerRef auto-sets collapsed=true when refId != 0
                }
            }

            m_doc->undoStack.endMacro();
            m_suppressRefresh = wasSuppressed;
            if (!m_suppressRefresh) refresh();
        }
        // ── Post-mutation sibling offset adjustment ──
        // After all kind/refId/arrayMeta changes, compute the new effective size.
        // If it differs from oldEffectiveSize, shift siblings to prevent overlap/gaps.
        {
            int ni = m_doc->tree.indexOfId(nodeId);
            if (ni >= 0) {
                const Node& updatedNode = m_doc->tree.nodes[ni];
                int newEffectiveSize = updatedNode.byteSize();
                if (newEffectiveSize == 0 && updatedNode.kind == NodeKind::Struct)
                    newEffectiveSize = m_doc->tree.structSpan(nodeId);
                // Array-of-Struct: byteSize() and structSpan() both return 0
                // because sizeForKind(Struct)==0. Compute from refId span × arrayLen.
                if (newEffectiveSize == 0 && updatedNode.kind == NodeKind::Array
                    && updatedNode.elementKind == NodeKind::Struct && updatedNode.refId != 0) {
                    int elemSpan = m_doc->tree.structSpan(updatedNode.refId);
                    newEffectiveSize = elemSpan * updatedNode.arrayLen;
                }
                if (newEffectiveSize == 0 && updatedNode.kind == NodeKind::Array
                    && updatedNode.elementKind != NodeKind::Struct)
                    newEffectiveSize = sizeForKind(updatedNode.elementKind) * updatedNode.arrayLen;
                int sizeDelta = newEffectiveSize - oldEffectiveSize;
                if (sizeDelta != 0 && oldEffectiveSize > 0) {
                    int oldEnd = nodeOffset + oldEffectiveSize;
                    auto siblings = m_doc->tree.childrenOf(parentId);
                    for (int si : siblings) {
                        const auto& sib = m_doc->tree.nodes[si];
                        if (sib.id == nodeId || sib.isStatic) continue;
                        if (sib.offset >= oldEnd) {
                            m_doc->undoStack.push(new RcxCommand(this,
                                cmd::ChangeOffset{sib.id, sib.offset,
                                                  sib.offset + sizeDelta}));
                        }
                    }
                }
            }
        }
    } else if (mode == TypePopupMode::ArrayElement) {
        if (resolved.entryKind == TypeEntry::Primitive) {
            if (resolved.primitiveKind != elemKind) {
                m_doc->undoStack.push(new RcxCommand(this,
                    cmd::ChangeArrayMeta{nodeId,
                        elemKind, resolved.primitiveKind,
                        arrLen, arrLen}));
            }
        } else if (resolved.entryKind == TypeEntry::Composite) {
            if (elemKind != NodeKind::Struct || nodeRefId != resolved.structId) {
                m_doc->undoStack.push(new RcxCommand(this,
                    cmd::ChangeArrayMeta{nodeId,
                        elemKind, NodeKind::Struct,
                        arrLen, arrLen}));
                if (nodeRefId != resolved.structId) {
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::ChangePointerRef{nodeId, nodeRefId, resolved.structId}));
                }
            }
        }
    } else if (mode == TypePopupMode::PointerTarget) {
        // "void" entry → refId 0; composite entry → real structId
        uint64_t realRefId = (resolved.entryKind == TypeEntry::Composite) ? resolved.structId : 0;
        if (realRefId != nodeRefId) {
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::ChangePointerRef{nodeId, nodeRefId, realRefId}));
        }
    }
}

uint64_t RcxController::findOrCreateStructByName(const QString& typeName) {
    // Check if it already exists locally
    for (const auto& n : m_doc->tree.nodes) {
        if (n.parentId == 0 && n.kind == NodeKind::Struct
            && (n.structTypeName == typeName || (n.structTypeName.isEmpty() && n.name == typeName)))
            return n.id;
    }
    // Import: create a new root struct with that name + default hex fields
    bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Import type"));
    Node n;
    n.kind = NodeKind::Struct;
    n.structTypeName = typeName;
    n.name = QStringLiteral("instance");
    n.parentId = 0;
    n.offset = 0;
    n.id = m_doc->tree.reserveId();
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{n}));
    for (int i = 0; i < 8; i++)
        insertNode(n.id, i * 8, NodeKind::Hex64,
                   QString("field_%1").arg(i * 8, 2, 16, QChar('0')));
    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    return n.id;
}

void RcxController::attachViaPlugin(const QString& providerIdentifier, const QString& target) {
    const auto* info = ProviderRegistry::instance().findProvider(providerIdentifier);
    if (!info || !info->plugin) {
        QMessageBox::warning(qobject_cast<QWidget*>(parent()),
            "Provider Error",
            QString("Provider '%1' not found. Is the plugin loaded?").arg(providerIdentifier));
        return;
    }

    QString errorMsg;
    auto provider = info->plugin->createProvider(target, &errorMsg);
    if (!provider) {
        if (!errorMsg.isEmpty())
            QMessageBox::warning(qobject_cast<QWidget*>(parent()), "Provider Error", errorMsg);
        return;
    }

    m_doc->undoStack.clear();
    m_doc->provider = std::move(provider);
    m_doc->dataPath.clear();
    // Don't overwrite baseAddress — caller (e.g. selfTest) already set it.
    // User-initiated source switches go through selectSource() which does update it.

    // Adopt the provider's pointer size for this document
    m_doc->tree.pointerSize = m_doc->provider->pointerSize();

    // Re-evaluate stored formula against the new provider
    if (!m_doc->tree.baseAddressFormula.isEmpty()) {
        AddressParserCallbacks cbs;
        auto* prov = m_doc->provider.get();
        cbs.resolveModule = [prov](const QString& name, bool* ok) -> uint64_t {
            uint64_t base = prov->symbolToAddress(name);
            *ok = (base != 0);
            return base;
        };
        int ptrSz = m_doc->tree.pointerSize;
        cbs.readPointer = [prov, ptrSz](uint64_t addr, bool* ok) -> uint64_t {
            uint64_t val = 0;
            *ok = prov->read(addr, &val, ptrSz);
            return val;
        };
        cbs.resolveIdentifier = [prov](const QString& name, bool* ok) -> uint64_t {
            return SymbolStore::instance().resolve(name, prov, ok);
        };
        // Wire kernel paging callbacks if provider supports it
        if (prov->hasKernelPaging()) {
            cbs.vtop = [prov](uint32_t pid, uint64_t va, bool* ok) -> uint64_t {
                Q_UNUSED(pid); // current provider already targets a specific process
                auto r = prov->translateAddress(va);
                *ok = r.valid;
                return r.physical;
            };
            cbs.cr3 = [prov](uint32_t pid, bool* ok) -> uint64_t {
                Q_UNUSED(pid);
                uint64_t cr3 = prov->getCr3();
                *ok = (cr3 != 0);
                return cr3;
            };
            cbs.physRead = [prov](uint64_t physAddr, bool* ok) -> uint64_t {
                auto entries = prov->readPageTable(physAddr, 0, 1);
                *ok = !entries.isEmpty();
                return entries.isEmpty() ? 0 : entries[0];
            };
        }
        auto result = AddressParser::evaluate(m_doc->tree.baseAddressFormula, ptrSz, &cbs);
        if (result.ok)
            m_doc->tree.baseAddress = result.value;
    }

    resetSnapshot();
    emit m_doc->documentChanged();
    refresh();
}

void RcxController::switchToSavedSource(int idx) {
    if (idx < 0 || idx >= m_savedSources.size()) return;
    if (idx == m_activeSourceIdx) return;

    // Save current source's base address before switching
    if (m_activeSourceIdx >= 0 && m_activeSourceIdx < m_savedSources.size()) {
        m_savedSources[m_activeSourceIdx].baseAddress = m_doc->tree.baseAddress;
        m_savedSources[m_activeSourceIdx].baseAddressFormula = m_doc->tree.baseAddressFormula;
    }

    m_activeSourceIdx = idx;
    const auto& entry = m_savedSources[idx];

    if (entry.kind == QStringLiteral("File")) {
        m_doc->loadData(entry.filePath);
        m_doc->tree.baseAddress = entry.baseAddress;
        m_doc->tree.baseAddressFormula = entry.baseAddressFormula;
        refresh();
    } else if (!entry.providerTarget.isEmpty()) {
        // Plugin-based provider (e.g. "processmemory" with target "pid:name")
        // Restore formula before attach so it can be re-evaluated against the new provider
        m_doc->tree.baseAddressFormula = entry.baseAddressFormula;
        attachViaPlugin(entry.kind, entry.providerTarget);
        // Restore saved base address — always override with saved value on source switch
        if (entry.baseAddressFormula.isEmpty())
            m_doc->tree.baseAddress = entry.baseAddress;
    }
}

void RcxController::selectSource(const QString& text) {
    if (text == QStringLiteral("#clear")) {
        clearSources();
    } else if (text.startsWith(QStringLiteral("#saved:"))) {
        int idx = text.mid(7).toInt();
        switchToSavedSource(idx);
    } else if (text == QStringLiteral("File")) {
        auto* w = qobject_cast<QWidget*>(parent());
        QString path = QFileDialog::getOpenFileName(w, "Load Binary Data", {}, "All Files (*)");
        if (!path.isEmpty()) {
            if (m_activeSourceIdx >= 0 && m_activeSourceIdx < m_savedSources.size())
                m_savedSources[m_activeSourceIdx].baseAddress = m_doc->tree.baseAddress;

            m_doc->loadData(path);

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
    } else {
        const auto* providerInfo = ProviderRegistry::instance().findProvider(text.toLower().replace(" ", ""));
        if (providerInfo) {
            QString target;
            bool selected = false;

            if (providerInfo->isBuiltin) {
                if (providerInfo->factory)
                    selected = providerInfo->factory(qobject_cast<QWidget*>(parent()), &target);
            } else {
                if (providerInfo->plugin)
                    selected = providerInfo->plugin->selectTarget(qobject_cast<QWidget*>(parent()), &target);
            }

            if (selected && !target.isEmpty()) {
                std::unique_ptr<Provider> provider;
                QString errorMsg;
                if (providerInfo->plugin)
                    provider = providerInfo->plugin->createProvider(target, &errorMsg);

                if (provider) {
                    if (m_activeSourceIdx >= 0 && m_activeSourceIdx < m_savedSources.size())
                        m_savedSources[m_activeSourceIdx].baseAddress = m_doc->tree.baseAddress;

                    uint64_t newBase = provider->base();
                    QString displayName = provider->name();
                    m_doc->undoStack.clear();
                    m_doc->provider = std::move(provider);
                    m_doc->dataPath.clear();
                    m_doc->tree.pointerSize = m_doc->provider->pointerSize();

                    // Re-evaluate formula if present (mirrors attachViaPlugin)
                    if (!m_doc->tree.baseAddressFormula.isEmpty()) {
                        AddressParserCallbacks cbs;
                        auto* prov = m_doc->provider.get();
                        cbs.resolveModule = [prov](const QString& name, bool* ok) -> uint64_t {
                            uint64_t base = prov->symbolToAddress(name);
                            *ok = (base != 0);
                            return base;
                        };
                        int ptrSz = m_doc->tree.pointerSize;
                        cbs.readPointer = [prov, ptrSz](uint64_t addr, bool* ok) -> uint64_t {
                            uint64_t val = 0;
                            *ok = prov->read(addr, &val, ptrSz);
                            return val;
                        };
                        cbs.resolveIdentifier = [prov](const QString& name, bool* ok) -> uint64_t {
                            return SymbolStore::instance().resolve(name, prov, ok);
                        };
                        // Wire kernel paging callbacks if provider supports it
                        if (prov->hasKernelPaging()) {
                            cbs.vtop = [prov](uint32_t pid, uint64_t va, bool* ok) -> uint64_t {
                                Q_UNUSED(pid);
                                auto r = prov->translateAddress(va);
                                *ok = r.valid;
                                return r.physical;
                            };
                            cbs.cr3 = [prov](uint32_t pid, bool* ok) -> uint64_t {
                                Q_UNUSED(pid);
                                uint64_t cr3 = prov->getCr3();
                                *ok = (cr3 != 0);
                                return cr3;
                            };
                            cbs.physRead = [prov](uint64_t physAddr, bool* ok) -> uint64_t {
                                auto entries = prov->readPageTable(physAddr, 0, 1);
                                *ok = !entries.isEmpty();
                                return entries.isEmpty() ? 0 : entries[0];
                            };
                        }
                        auto result = AddressParser::evaluate(
                            m_doc->tree.baseAddressFormula, ptrSz, &cbs);
                        if (result.ok)
                            m_doc->tree.baseAddress = result.value;
                    } else if (newBase != 0 && m_doc->tree.baseAddress == 0x00400000) {
                        // Only apply provider base for fresh/default projects.
                        // If user loaded an .rcx with a custom base, preserve it.
                        m_doc->tree.baseAddress = newBase;
                    }
                    resetSnapshot();
                    emit m_doc->documentChanged();

                    QString identifier = providerInfo->identifier;
                    int existingIdx = -1;
                    for (int i = 0; i < m_savedSources.size(); i++) {
                        if (m_savedSources[i].kind == identifier
                            && m_savedSources[i].providerTarget == target) {
                            existingIdx = i;
                            break;
                        }
                    }
                    if (existingIdx >= 0) {
                        m_activeSourceIdx = existingIdx;
                        m_savedSources[existingIdx].baseAddress = m_doc->tree.baseAddress;
                    } else {
                        SavedSourceEntry entry;
                        entry.kind = identifier;
                        entry.displayName = displayName;
                        entry.providerTarget = target;
                        entry.baseAddress = m_doc->tree.baseAddress;
                        m_savedSources.append(entry);
                        m_activeSourceIdx = m_savedSources.size() - 1;
                    }
                    refresh();
                } else if (!errorMsg.isEmpty()) {
                    QMessageBox::warning(qobject_cast<QWidget*>(parent()), "Provider Error", errorMsg);
                }
            }
        }
    }
}

void RcxController::clearSources() {
    m_savedSources.clear();
    m_activeSourceIdx = -1;
    m_doc->provider = std::make_shared<NullProvider>();
    m_doc->dataPath.clear();
    resetSnapshot();
    pushSavedSourcesToEditors();
    refresh();
}

void RcxController::copySavedSources(const QVector<SavedSourceEntry>& sources, int activeIdx) {
    m_savedSources = sources;
    m_activeSourceIdx = activeIdx;
    pushSavedSourcesToEditors();
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

void RcxController::setRefreshInterval(int ms) {
    if (m_refreshTimer)
        m_refreshTimer->setInterval(qMax(1, ms));
}

void RcxController::setCompactColumns(bool v) {
    m_compactColumns = v;
    refresh();
}

void RcxController::setTreeLines(bool v) {
    m_treeLines = v;
    refresh();
}

void RcxController::setBraceWrap(bool v) {
    m_braceWrap = v;
    refresh();
}

void RcxController::setTypeHints(bool v) {
    m_typeHints = v;
    refresh();
}

void RcxController::setShowComments(bool v) {
    m_showComments = v;
    refresh();
}

void RcxController::setupAutoRefresh() {
    int ms = QSettings("Reclass", "Reclass").value("refreshMs", kDefaultRefreshMs).toInt();
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(qMax(1, ms));
    connect(m_refreshTimer, &QTimer::timeout, this, &RcxController::onRefreshTick);
    m_refreshTimer->start();

    m_refreshWatcher = new QFutureWatcher<PageMap>(this);
    connect(m_refreshWatcher, &QFutureWatcher<PageMap>::finished,
            this, &RcxController::onReadComplete);
}

// Recursively collect memory ranges for a struct and its pointer targets.
// memBase is the absolute address where this struct's data lives.
void RcxController::collectPointerRanges(
        uint64_t structId, uint64_t memBase,
        int depth, int maxDepth,
        QSet<QPair<uint64_t,uint64_t>>& visited,
        QVector<QPair<uint64_t,int>>& ranges) const
{
    if (depth >= maxDepth) return;
    QPair<uint64_t,uint64_t> key{structId, memBase};
    if (visited.contains(key)) return;
    visited.insert(key);

    int span = m_doc->tree.structSpan(structId);
    if (span <= 0) return;
    ranges.emplaceBack(memBase, span);

    if (!m_snapshotProv) return;

    // Walk children looking for non-collapsed pointers
    QVector<int> children = m_doc->tree.childrenOf(structId);
    for (int ci : children) {
        const Node& child = m_doc->tree.nodes[ci];
        if (child.kind != NodeKind::Pointer32 && child.kind != NodeKind::Pointer64)
            continue;
        if (child.collapsed || child.refId == 0) continue;

        uint64_t ptrAddr = memBase + child.offset;
        int ptrSize = child.byteSize();
        if (!m_snapshotProv->isReadable(ptrAddr, ptrSize)) continue;

        uint64_t ptrVal = (child.kind == NodeKind::Pointer32)
            ? (uint64_t)m_snapshotProv->readU32(ptrAddr)
            : m_snapshotProv->readU64(ptrAddr);
        if (ptrVal == 0 || ptrVal == UINT64_MAX) continue;

        uint64_t pBase = ptrVal;
        collectPointerRanges(child.refId, pBase, depth + 1, maxDepth,
                             visited, ranges);
    }

    // Embedded struct references (struct node with refId but no own children)
    int idx = m_doc->tree.indexOfId(structId);
    if (idx >= 0) {
        const Node& sn = m_doc->tree.nodes[idx];
        if (sn.kind == NodeKind::Struct && sn.refId != 0 && children.isEmpty())
            collectPointerRanges(sn.refId, memBase, depth, maxDepth,
                                 visited, ranges);
    }
}

void RcxController::onRefreshTick() {
    if (m_readInFlight) return;
    if (!m_doc->provider || !m_doc->provider->isLive()) return;
    if (m_suppressRefresh) return;
    for (auto* editor : m_editors)
        if (editor->isEditing()) return;

    int extent = computeDataExtent();
    if (extent <= 0) return;

    // Collect all needed ranges: main struct + pointer targets (absolute addresses)
    QVector<QPair<uint64_t,int>> ranges;
    ranges.emplaceBack(m_doc->tree.baseAddress, extent);

    if (m_snapshotProv) {
        QSet<QPair<uint64_t,uint64_t>> visited;
        uint64_t rootId = m_viewRootId;
        if (rootId == 0 && !m_doc->tree.nodes.isEmpty())
            rootId = m_doc->tree.nodes[0].id;
        collectPointerRanges(rootId, m_doc->tree.baseAddress, 0, 99, visited, ranges);
    }

    m_readInFlight = true;
    m_readGen = m_refreshGen;

    auto prov = m_doc->provider;
    m_refreshWatcher->setFuture(QtConcurrent::run([prov, ranges]() -> PageMap {
        constexpr uint64_t kPageSize = 4096;
        constexpr uint64_t kPageMask = ~(kPageSize - 1);
        PageMap pages;
        for (const auto& r : ranges) {
            uint64_t pageStart = r.first & kPageMask;
            uint64_t end = r.first + r.second;
            uint64_t pageEnd = (end + kPageSize - 1) & kPageMask;
            for (uint64_t p = pageStart; p < pageEnd; p += kPageSize) {
                if (!pages.contains(p))
                    pages[p] = prov->readBytes(p, static_cast<int>(kPageSize));
            }
        }
        return pages;
    }));
}

void RcxController::onReadComplete() {
    m_readInFlight = false;

    if (m_readGen != m_refreshGen) return;

    PageMap newPages;
    try {
        newPages = m_refreshWatcher->result();
    } catch (const std::exception& e) {
        qWarning() << "[Refresh] async read threw:" << e.what();
        return;
    } catch (...) {
        qWarning() << "[Refresh] async read threw unknown exception";
        return;
    }

    // All-zero guard: if page 0 is all zeros and we already have data, discard
    if (!m_prevPages.isEmpty() && newPages.contains(0)) {
        const QByteArray& p0 = newPages.value(0);
        bool allZero = true;
        for (int i = 0; i < p0.size(); ++i) {
            if (p0[i] != 0) { allZero = false; break; }
        }
        if (allZero) {
            qDebug() << "[Refresh] discarding all-zero page-0, keeping stale snapshot";
            return;
        }
    }

    // Fast path: no changes at all
    if (newPages == m_prevPages)
        return;

    // Compute which byte offsets changed (for change highlighting).
    // Skip on first snapshot — nothing to compare against.
    m_changedOffsets.clear();
    if (!m_prevPages.isEmpty()) {
        for (auto it = newPages.constBegin(); it != newPages.constEnd(); ++it) {
            uint64_t pageAddr = it.key();
            const QByteArray& newPage = it.value();
            auto oldIt = m_prevPages.constFind(pageAddr);
            if (oldIt == m_prevPages.constEnd())
                continue;   // new page, no previous data to diff against
            const QByteArray& oldPage = oldIt.value();
            int cmpLen = qMin(oldPage.size(), newPage.size());
            for (int i = 0; i < cmpLen; ++i) {
                if (oldPage[i] != newPage[i])
                    m_changedOffsets.insert(static_cast<int64_t>(pageAddr) + i);
            }
        }
    }

    int mainExtent = computeDataExtent();
    m_prevPages = newPages;

    if (m_snapshotProv)
        m_snapshotProv->updatePages(std::move(newPages), mainExtent);
    else
        m_snapshotProv = std::make_unique<SnapshotProvider>(
            m_doc->provider, std::move(newPages), mainExtent);

    refresh();
    m_changedOffsets.clear();
}

int RcxController::computeDataExtent() const {
    static constexpr int64_t kMaxMainExtent = 16 * 1024 * 1024; // 16 MB cap

    int64_t treeExtent = 0;
    for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
        const Node& node = m_doc->tree.nodes[i];
        int64_t off = m_doc->tree.computeOffset(i);
        if (off < 0) continue;
        int sz = (node.kind == NodeKind::Struct || node.kind == NodeKind::Array)
            ? m_doc->tree.structSpan(node.id) : node.byteSize();
        int64_t end = off + sz;
        if (end > treeExtent) treeExtent = end;
    }
    if (treeExtent > 0) return static_cast<int>(qMin(treeExtent, kMaxMainExtent));

    int provSize = m_doc->provider->size();
    if (provSize > 0) return provSize;
    return 0;
}

void RcxController::resetSnapshot() {
    m_refreshGen++;
    m_readInFlight = false;
    m_snapshotProv.reset();
    m_prevPages.clear();
    m_changedOffsets.clear();
    m_valueHistory.clear();
    m_lastValueAddr.clear();
}

void RcxController::handleMarginClick(RcxEditor* editor, int margin,
                                       int line, Qt::KeyboardModifiers) {
    const LineMeta* lm = editor->metaForLine(line);
    if (!lm) return;

    if (lm->foldHead && (margin == 0 || margin == 1)) {
        if (lm->markerMask & (1u << M_CYCLE))
            materializeRefChildren(lm->nodeIdx);
        else
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
