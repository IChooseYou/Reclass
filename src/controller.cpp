#include "controller.h"
#include "addressparser.h"
#include "address_callbacks.h"
#include "providers/file_provider.h"
#include "widgets/address_bar_model.h"
#include "widgets/address_bar.h"
#include "symbolstore.h"
#include "profiler.h"
#include "typeselectorpopup.h"
#include "sourcechooserpopup.h"
#include "hextoolbarpopup.h"
#include "commontypes.h"
#include "clipboard.h"
#include "diffutil.h"
#include "timeline/frame_provider.h"
#include "timeline/timeline_hub.h"
#include "timeline/timeline_service.h"
#include "timeline/tl_clock.h"
#include <cmath>
#include <cstring>
#include "providerregistry.h"
#include "themes/thememanager.h"
#include "svgicon.h"          // themedVsIcon — menu icons must be inked
#include "widgets/themed_messagebox.h"
#include "widgets/themed_inputdialog.h"
#include "widgets/dialog_button.h"
#include "widgets/enum_picker_popup.h"
#include "gotoaddressdialog.h"
#include <Qsci/qsciscintilla.h>
#include <QSplitter>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QMenu>
#include <QWidgetAction>
#include <QInputDialog>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QClipboard>
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QtConcurrent/QtConcurrentRun>
#include <limits>

namespace rcx {

static thread_local const RcxDocument* s_composeDoc = nullptr;

// RAII guard so any path out of compose — normal return, early return, or
// thrown exception — restores s_composeDoc to whatever it was before. The
// previous pattern (manual s_composeDoc = m_doc / s_composeDoc = nullptr)
// would leave the thread-local pointing at a destroyed document if compose
// threw, and subsequent type-name lookups would dereference freed memory.
// Stacks cleanly across nested composes: saves the prior value and restores
// it rather than blindly clearing to nullptr.
namespace {
struct ComposeDocGuard {
    const RcxDocument* prev;
    explicit ComposeDocGuard(const RcxDocument* doc) : prev(s_composeDoc) {
        s_composeDoc = doc;
    }
    ~ComposeDocGuard() { s_composeDoc = prev; }
    ComposeDocGuard(const ComposeDocGuard&) = delete;
    ComposeDocGuard& operator=(const ComposeDocGuard&) = delete;
};
}

static QString docTypeNameProvider(NodeKind k) {
    if (s_composeDoc) return s_composeDoc->resolveTypeName(k);
    auto* m = kindMeta(k);
    return m ? QString::fromLatin1(m->typeName) : QStringLiteral("???");
}

// Themed comment input dialog matching the editor style. ThemedDialog +
// DialogButton — was a raw QDialog with a QDialogButtonBox styled inline
// (border-radius 3px, font-size 11px), out of sync with the rest of the
// app's dialog language.
static QString showCommentDialog(QWidget* parent, const QString& title,
                                 const QString& existing, bool* ok) {
    *ok = false;
    const auto& theme = ThemeManager::instance().current();
    QSettings settings("RC", "RC");
    QFont editorFont(settings.value("font", "JetBrains Mono").toString(), 12);
    editorFont.setFixedPitch(true);

    rcx::ThemedDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.setMinimumWidth(380);

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
        " padding: 6px 8px; selection-background-color: %4; }"
        "QLineEdit:focus { border-color: %5; }")
        .arg(theme.backgroundAlt.name(), theme.text.name(),
             theme.border.name(), theme.selection.name(),
             theme.borderFocused.name()));
    layout->addWidget(input);

    auto* cancel = new rcx::DialogButton(QObject::tr("Cancel"),
        rcx::DialogButton::Secondary, &dlg);
    auto* save = new rcx::DialogButton(QObject::tr("Save"),
        rcx::DialogButton::Primary, &dlg);
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(save,   &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(input,  &QLineEdit::returnPressed, &dlg, &QDialog::accept);
    layout->addLayout(rcx::ThemedDialog::makeButtonRow({cancel, save}));
    save->setDefault(true);

    input->setFocus();
    if (dlg.exec() == QDialog::Accepted) {
        *ok = true;
        return input->text();
    }
    return {};
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

bool RcxDocument::save(const QString& path, const NodeTree* view) {
    QJsonObject json = (view ? *view : tree).toJson();

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
    PROFILE_SCOPE("RcxDocument::load");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QByteArray head = file.peek(4096);
    if (head.startsWith("\xEF\xBB\xBF")) head.remove(0, 3);
    if (file.size() > 0 && !head.trimmed().startsWith('{')) {
        qWarning() << "Not a JSON definition:" << path;
        return false;
    }
    QByteArray bytes;
    {
        PROFILE_SCOPE("RcxDocument::load.read");
        bytes = file.readAll();
    }

    // Empty file → fresh empty project (legitimate edge case: a brand-new
    // .rcx the user just touched). Any non-empty file MUST parse as JSON;
    // otherwise we'd silently swallow raw binaries (e.g. a .png picked
    // through "All Files (*)") and present them as a "struct NoName"
    // placeholder, which looks like a bug to the user.
    QJsonParseError jerr;
    QJsonDocument jdoc;
    {
        PROFILE_SCOPE("RcxDocument::load.json-parse");
        jdoc = bytes.isEmpty()
            ? QJsonDocument(QJsonObject{})
            : QJsonDocument::fromJson(bytes, &jerr);
    }
    if (jdoc.isNull() || !jdoc.isObject()) {
        qWarning().noquote() << "[load]" << path << "isn't a RC project ("
                             << (jdoc.isNull() ? jerr.errorString()
                                               : QStringLiteral("not a JSON object"))
                             << ") — refuse rather than show Untitled placeholder";
        return false;
    }

    undoStack.clear();
    QJsonObject root = jdoc.object();
    {
        PROFILE_SCOPE("RcxDocument::load.tree-from-json");
        tree = NodeTree::fromJson(root);
    }

    // Validate + repair on load: orphans re-rooted, cycles broken, duplicate
    // ids re-numbered. Silent on clean trees; non-fatal on dirty so the user
    // still gets a usable view of even partially-corrupted files.
    {
        PROFILE_SCOPE("RcxDocument::load.validate");
        auto vr = tree.validate(/*repair=*/true);
        if (!vr.clean())
            qWarning() << "[load] tree validation:" << path << vr.summary();
        // Overlap detection — non-repairable (the user has to choose
        // which field to shrink), so we log a warning per pair so the
        // issue is visible in the console + future margin-warning pass.
        auto overlaps = tree.findOverlaps();
        m_loadOverlapCount = overlaps.size();
        if (!overlaps.isEmpty()) {
            qWarning().nospace() << "[load] " << path
                << ": " << overlaps.size()
                << " sibling overlap(s) detected — manual review required";
            for (int i = 0; i < qMin(overlaps.size(), 5); ++i) {
                int ai = tree.indexOfId(overlaps[i].aId);
                int bi = tree.indexOfId(overlaps[i].bId);
                if (ai < 0 || bi < 0) continue;
                qWarning().noquote() << "  ├ overlap:"
                    << tree.nodes[ai].name
                    << QStringLiteral("@+0x%1").arg(tree.nodes[ai].offset, 0, 16)
                    << "vs" << tree.nodes[bi].name
                    << QStringLiteral("@+0x%1").arg(tree.nodes[bi].offset, 0, 16);
            }
            if (overlaps.size() > 5)
                qWarning().noquote() << "  └ ... (+"
                    << (overlaps.size() - 5) << "more)";
        }
    }

    // Load type aliases
    typeAliases.clear();
    QJsonObject aliasObj = root["typeAliases"].toObject();
    for (auto it = aliasObj.begin(); it != aliasObj.end(); ++it) {
        NodeKind k = kindFromString(it.key());
        QString v = it.value().toString();
        if (!v.isEmpty())
            typeAliases[k] = v;
    }

    // Restore saved sources (if the file shipped any). Stored as raw
    // JSON on the doc; the controller lifts them into its own
    // SavedSourceEntry list once it attaches and (for example) the
    // png.rcx demo can auto-attach its sibling sample .png.
    // Relative filePaths are resolved against the .rcx directory so
    // examples are relocatable across install layouts.
    pendingSavedSources = root["savedSources"].toArray();
    if (!pendingSavedSources.isEmpty()) {
        QString rcxDir = QFileInfo(path).absolutePath();
        for (int i = 0; i < pendingSavedSources.size(); ++i) {
            QJsonObject so = pendingSavedSources[i].toObject();
            QString fp = so["filePath"].toString();
            if (!fp.isEmpty() && QFileInfo(fp).isRelative()) {
                so["filePath"] = QDir(rcxDir).absoluteFilePath(fp);
                pendingSavedSources[i] = so;
            }
        }
    }

    filePath = path;
    modified = false;
    emit documentChanged();
    return true;
}

bool RcxDocument::loadData(const QString& binaryPath) {
    PROFILE_SCOPE("RcxDocument::loadData(path)");
    auto fileProvider = FileProvider::open(binaryPath);
    if (!fileProvider) return false;
    if (controllers.size() <= 1) undoStack.clear();
    provider = std::move(fileProvider);
    dataPath = binaryPath;
    tree.baseAddress = 0;
    tree.baseAddressFormula.clear();
    emit documentChanged();
    return true;
}

void RcxDocument::loadData(const QByteArray& data) {
    PROFILE_SCOPE("RcxDocument::loadData(bytes)");
    if (controllers.size() <= 1) undoStack.clear();
    provider = std::make_shared<BufferProvider>(data);
    tree.baseAddress = 0;
    emit documentChanged();
}

// ── RcxCommand ──

RcxCommand::RcxCommand(RcxController* ctrl, Command cmd)
    : m_ctrl(ctrl), m_doc(ctrl->document()), m_cmd(cmd) {
    if (std::holds_alternative<cmd::WriteBytes>(cmd)) m_writeProvider = ctrl->provider();
    m_localViewCommand = std::holds_alternative<cmd::Collapse>(cmd)
        || std::holds_alternative<cmd::ChangeBase>(cmd);
}

// If applyCommand reports the underlying op was rejected (e.g. the provider
// refused a WriteBytes), mark this command obsolete so QUndoStack drops it
// on its next walk. This prevents a later undo from pushing stale
// "oldBytes" over memory that never actually took the "newBytes" write.
// Failed WriteBytes are usually transient (target process gone, page
// protection changed, snapshot writethrough refused). Leaving them alive
// lets a later redo on a re-attached writable provider succeed. Tree-state
// commands genuinely can't recover, so they get marked obsolete on failure.
static bool isTransientCommand(const Command& cmd) {
    return std::holds_alternative<cmd::WriteBytes>(cmd);
}

void RcxCommand::undo() {
    execute(true);
}
void RcxCommand::redo() {
    execute(false);
}

void RcxCommand::execute(bool undo) {
    RcxController* ctrl = m_ctrl;
    if (!ctrl && !m_localViewCommand && m_doc && !m_doc->controllers.isEmpty())
        ctrl = m_doc->controllers.first();
    if (!ctrl) {
        setObsolete(true);
        return;
    }
    if (!ctrl->applyCommand(m_cmd, undo, m_writeProvider) && !isTransientCommand(m_cmd))
        setObsolete(true);
}

// ── RcxController ──

// The address bar model mirrors SourceStatus by numeric value (it is
// Core-only and cannot include this header); a reorder here must fail here.
static_assert(int(RcxController::SourceStatus::None)         == liveness::None
           && int(RcxController::SourceStatus::Static)       == liveness::Static
           && int(RcxController::SourceStatus::Live)         == liveness::Live
           && int(RcxController::SourceStatus::Stale)        == liveness::Stale
           && int(RcxController::SourceStatus::Disconnected) == liveness::Disconnected,
              "AddressBarState::liveness must mirror RcxController::SourceStatus");

RcxController::RcxController(RcxDocument* doc, QWidget* parent)
    : QObject(parent), m_doc(doc)
{
    PROFILE_SCOPE("RcxController::ctor");
    m_doc->controllers.append(this);
    fmt::setTypeNameProvider(docTypeNameProvider);
    connect(m_doc, &RcxDocument::documentChanged, this, [this] {
        m_viewTreeDirty = true;
        ++m_capturePlanEpoch;
        refresh();
    });
    connect(m_doc, &RcxDocument::timelineStateChanged, this, [this] {
        // Another tab on this document cleared the class: a moment this tab
        // holds from before the clear is gone too.
        if (isViewingPast() && m_timelineHub
            && m_timelineHub->isHiddenFor(timelineClassId(), m_pastRecordMs))
            leavePast();
        scheduleTimelineChanged();
    });
    static int s_nextTimelineRequester = 0;
    m_timelineRequester = ++s_nextTimelineRequester;
    m_timelineBackgroundCapture = tl::TimelineService::instance().budgets().captureWhenMinimized;
    m_timelineEnabled = QSettings(QStringLiteral("RC"), QStringLiteral("RC"))
                            .value(QStringLiteral("timeline/enabled"), true).toBool();
    connect(&m_doc->undoStack, &QUndoStack::indexChanged, this, [this] {
        if (m_doc->controllers.size() > 1) {
            resetChangeTracking();
            refresh();
        }
    });
    setupAutoRefresh();

    // Hex toolbar: no longer auto-shows (replaced by type-cycling tooltip).
    // Still available via context menu for insert/join/fill operations.
    connect(this, &RcxController::nodeSelected, this, [this](int /*nodeIdx*/) {
        hideHexToolbar();
    });

    // Lift any saved sources the .rcx shipped with into our own list,
    // and (if any) auto-attach the first one. Examples like png.rcx
    // use this so opening the file just shows the bytes — no manual
    // "now attach the .png as a source" step. Not a navigation gesture:
    // the source switch it performs must not seed the Back stack.
    m_loading = true;
    ingestPendingSavedSources();
    m_loading = false;
    // A tab opened on a document whose source is already attached.
    bindTimeline();
}

void RcxController::ingestPendingSavedSources() {
    PROFILE_SCOPE("RcxController::ingestPendingSavedSources");
    if (!m_doc || m_doc->pendingSavedSources.isEmpty()) return;
    QJsonArray arr = m_doc->pendingSavedSources;
    m_doc->pendingSavedSources = QJsonArray{};   // consume once
    for (const auto& v : arr) {
        QJsonObject so = v.toObject();
        SavedSourceEntry e;
        e.kind             = so["kind"].toString();
        e.displayName      = so["displayName"].toString();
        e.filePath         = so["filePath"].toString();
        e.providerTarget   = so["providerTarget"].toString();
        e.baseAddress      = so["baseAddress"].toString("0").toULongLong(nullptr, 16);
        e.baseAddressFormula = so["baseAddressFormula"].toString();
        m_savedSources.append(e);
    }
    if (m_savedSources.isEmpty()) return;
    // Auto-activate the first saved source. switchToSavedSource handles
    // the File / plugin distinction. Skip silently when the referenced
    // file is missing — the user still gets the layout, just no bytes.
    const auto& first = m_savedSources.first();
    if (first.kind == QStringLiteral("File")
        && !first.filePath.isEmpty()
        && !QFileInfo::exists(first.filePath)) {
        qWarning() << "[load] saved source missing:" << first.filePath;
        return;
    }
    switchToSavedSource(0);
}

RcxController::~RcxController() {
    if (m_doc) m_doc->controllers.removeAll(this);
    if (m_refreshWatcher) {
        m_refreshWatcher->cancel();
        m_refreshWatcher->waitForFinished();
    }

    detachTimeline();
    m_frameProv.reset();
    m_snapshotProv.reset();
}

uint64_t& RcxController::baseAddress() {
    return m_instance ? m_instance->baseAddress : m_doc->tree.baseAddress;
}
uint64_t RcxController::baseAddress() const {
    return m_instance ? m_instance->baseAddress : m_doc->tree.baseAddress;
}
QString& RcxController::baseAddressFormula() {
    return m_instance ? m_instance->formula : m_doc->tree.baseAddressFormula;
}
const QString& RcxController::baseAddressFormula() const {
    return m_instance ? m_instance->formula : m_doc->tree.baseAddressFormula;
}
std::shared_ptr<Provider>& RcxController::provider() {
    return m_instance ? m_instance->provider : m_doc->provider;
}
const std::shared_ptr<Provider>& RcxController::provider() const {
    return m_instance ? m_instance->provider : m_doc->provider;
}

bool RcxController::isCollapsed(const Node& node) const {
    return m_instance ? m_instance->collapsed.value(node.id, node.collapsed) : node.collapsed;
}

void RcxController::setCollapsed(uint64_t id, bool collapsed) {
    ++m_capturePlanEpoch;   // an expanded pointer's target joins (or leaves) the capture
    if (m_instance) {
        m_instance->collapsed.insert(id, collapsed);
        m_viewTreeDirty = true;
    } else {
        const int idx = m_doc->tree.indexOfId(id);
        if (idx >= 0) m_doc->tree.nodes[idx].collapsed = collapsed;
    }
}

const NodeTree& RcxController::viewTree() const {
    if (!m_instance) return m_doc->tree;
    if (m_viewTreeDirty || m_viewTreeGeneration != m_doc->tree.generation()) {
        m_viewTree = m_doc->tree;
        for (auto& node : m_viewTree.nodes)
            node.collapsed = isCollapsed(node);
        m_viewTreeGeneration = m_doc->tree.generation();
        m_viewTreeDirty = false;
    }
    m_viewTree.baseAddress = baseAddress();
    m_viewTree.baseAddressFormula = baseAddressFormula();
    const int root = m_viewTree.indexOfId(m_viewRootId);
    if (root >= 0) m_viewTree.initialClass = m_viewTree.nodes[root].structTypeName;
    return m_viewTree;
}

bool RcxController::configureInstance(const RcxController& source, uint64_t rootId,
                                      uint64_t address, const QString& expression,
                                      std::shared_ptr<Provider> instanceProvider) {
    const int idx = m_doc->tree.indexOfId(rootId);
    if (source.document() != m_doc || !source.provider() || idx < 0
        || m_doc->tree.nodes[idx].kind != NodeKind::Struct
        || m_doc->tree.nodes[idx].parentId != 0) return false;
    InstanceState instance;
    instance.baseAddress = address;
    instance.provider = instanceProvider ? std::move(instanceProvider) : source.provider();
    if (instance.provider->isLive()
        && instance.provider->pointerSize() != m_doc->tree.pointerSize) return false;
    const QString resolved = durableAddressExpression(address, instance.provider.get(),
                                                       m_doc->tree.pointerSize, expression);
    if (!isBareAddressLiteral(resolved)) instance.formula = resolved;
    for (const auto& node : source.viewTree().nodes)
        instance.collapsed.insert(node.id, node.collapsed);
    instance.collapsed.insert(rootId, false);
    m_instance = std::move(instance);
    m_viewTreeDirty = true;
    m_viewRootId = rootId;
    m_focusPath.clear();
    m_selIds.clear();
    m_nav = NavHistory();
    if (provider() == source.provider()) {
        m_savedSources = source.m_savedSources;
        m_activeSourceIdx = source.m_activeSourceIdx;
    } else {
        m_savedSources.clear();
        m_activeSourceIdx = -1;
    }
    m_readOnlyOverride = source.m_readOnlyOverride;
    resetSnapshot();
    refresh();
    return true;
}

QString RcxController::addressExpression(uint64_t address, const QString& preferred) const {
    return durableAddressExpression(address, provider().get(), m_doc->tree.pointerSize, preferred);
}

bool RcxController::navigateToAddress(uint64_t address, const QString& preferred) {
    return rebaseTo(addressExpression(address, preferred));
}

bool RcxController::loadSourceFile(const QString& path) {
    QString error;
    auto file = FileProvider::open(path, &error);
    if (!file) {
        emit statusHint(QStringLiteral("Couldn't open %1: %2").arg(QFileInfo(path).fileName(), error));
        return false;
    }
    if (!m_instance && m_doc->controllers.size() == 1) m_doc->undoStack.clear();
    provider() = std::move(file);
    if (!m_instance) m_doc->dataPath = path;
    baseAddress() = 0;
    baseAddressFormula().clear();
    resetSnapshot();
    emit m_doc->documentChanged();
    return true;
}

void RcxController::openPointerBeside(RcxEditor* editor, int line) {
    const LineMeta* lm = editor ? editor->metaForLine(line) : nullptr;
    if (!lm || lm->nodeIdx < 0 || lm->nodeIdx >= m_doc->tree.nodes.size() || !provider()) return;
    const Node& node = m_doc->tree.nodes[lm->nodeIdx];
    if (!isPointerKind(node.kind) || !node.refId) return;
    const Provider* data = displayedProvider();   // the pointer as shown: recorded values too
    uint64_t target = 0;
    if (!data->read(lm->offsetAddr, &target, sizeForKind(node.kind)) || !target) {
        emit statusHint(QStringLiteral("Pointer is null or unreadable"));
        return;
    }
    if (node.isRelative) {
        if (target > UINT64_MAX - baseAddress()) return;
        target += baseAddress();
    }
    if (!provider()->isReadable(target, 1)) {
        emit statusHint(QStringLiteral("Pointer target is unreadable"));
        return;
    }
    emit requestOpenInstanceBeside(node.refId, target, addressExpression(target));
}

void RcxController::resetProvider() {
    if (m_refreshTimer) m_refreshTimer->stop();
    if (m_refreshWatcher) {
        disconnect(m_refreshWatcher, nullptr, this, nullptr);
        m_refreshWatcher->cancel();
        m_refreshWatcher->waitForFinished();
    }
    returnToLive();
    detachTimeline();
    m_snapshotProv.reset();
    provider().reset();
}

RcxEditor* RcxController::primaryEditor() const {
    return m_editors.isEmpty() ? nullptr : m_editors.first();
}

RcxEditor* RcxController::addSplitEditor(QWidget* parent) {
    PROFILE_SCOPE("RcxController::addSplitEditor");
    auto* editor = new RcxEditor(parent);
    m_editors.append(editor);
    connectEditor(editor);
    editor->setTreeColumns(m_treeColumns);

    if (!m_lastResult.text.isEmpty()) {
        editor->applyDocument(m_lastResult);
    }
    updateCommandRow();
    // A new pane's bar starts empty and the per-refresh push is guarded on
    // change, so it must be seeded now or it shows nothing until the trail
    // next moves.
    if (m_doc) editor->setAddressBarState(addressBarState());

    // Eagerly pre-warm the type popup so first click isn't slow (~350ms cold start).
    if (!m_cachedPopup) {
        QPointer<RcxEditor> safeEditor = editor;
        QTimer::singleShot(0, this, [this, safeEditor]() {
            if (!m_editors.isEmpty() && safeEditor) {
                if (!m_cachedPopup)
                    ensurePopup(safeEditor);
                if (!m_cachedSourcePopup)
                    ensureSourcePopup(safeEditor);
            }
        });
    }
    return editor;
}

void RcxController::removeSplitEditor(RcxEditor* editor) {
    m_editors.removeOne(editor);
    editor->disconnect(this);
}

// ── Byte-selection op handlers ──
// Factored out of connectEditor's signal connections so the right-click
// "Selected bytes (N) ▸" submenu and the Ctrl+C/V/Del keyboard shortcuts
// share one implementation. Each reads the live byte range from
// editor->byteSelection() and does provider I/O via the active snapshot
// (snapshot wins over the real provider when both are present, so copied
// values match what the user sees).

QByteArray RcxController::readSelectionBytes(RcxEditor* editor) {
    auto range = editor->byteSelection();
    if (!range || !this->provider()) return {};
    uint64_t lo = range->first;
    int n = static_cast<int>(range->second - range->first);
    if (n <= 0 || n > 65536) return {};
    const Provider* prov = displayedProvider();   // what is on screen: recorded values too
    if (!prov->isReadable(lo, n)) {
        emit statusHint(QStringLiteral("Couldn't read %1 bytes at 0x%2")
            .arg(n).arg(lo, 0, 16));
        return {};
    }
    return prov->readBytes(lo, n);
}

void RcxController::byteCopyHex(RcxEditor* editor) {
    auto range = editor->byteSelection();
    if (!range || !this->provider()) return;
    uint64_t lo = range->first;
    int n = static_cast<int>(range->second - range->first);
    if (n <= 0 || n > 65536) return;
    const Provider* prov = displayedProvider();   // what is on screen: recorded values too
    QByteArray data = prov->isReadable(lo, n) ? prov->readBytes(lo, n)
                                              : QByteArray();
    if (data.size() < n) {
        emit statusHint(QStringLiteral("Couldn't read %1 bytes at 0x%2")
            .arg(n).arg(lo, 0, 16));
        return;
    }
    QString hex;
    hex.reserve(n * 3);
    for (int i = 0; i < n; ++i) {
        if (i > 0) hex += QLatin1Char(' ');
        hex += QStringLiteral("%1")
            .arg((uint8_t)data[i], 2, 16, QChar('0')).toUpper();
    }
    QApplication::clipboard()->setText(hex);
    emit statusHint(QStringLiteral("Copied %1 byte%2 as hex")
        .arg(n).arg(n == 1 ? "" : "s"));
}

void RcxController::byteCopyCArray(RcxEditor* editor) {
    QByteArray data = readSelectionBytes(editor);
    if (data.isEmpty()) return;
    // Format as `{0xDE, 0xAD, 0xBE, 0xEF}` — direct paste into a C/C++
    // array initializer. Linewrap at 16 bytes/row. Build the hex digits
    // separately so .toUpper() doesn't touch the literal lowercase `0x`.
    QString out;
    out.reserve(data.size() * 7);
    out += QLatin1Char('{');
    for (int i = 0; i < data.size(); ++i) {
        if (i > 0) out += QLatin1Char(',');
        if (i > 0 && (i % 16) == 0) out += QLatin1Char('\n');
        else if (i > 0)              out += QLatin1Char(' ');
        QString digits = QStringLiteral("%1")
            .arg((uint8_t)data[i], 2, 16, QChar('0')).toUpper();
        out += QStringLiteral("0x") + digits;
    }
    out += QLatin1Char('}');
    QApplication::clipboard()->setText(out);
    emit statusHint(QStringLiteral("Copied %1 byte%2 as C array")
        .arg(data.size()).arg(data.size() == 1 ? "" : "s"));
}

void RcxController::byteCopyPython(RcxEditor* editor) {
    QByteArray data = readSelectionBytes(editor);
    if (data.isEmpty()) return;
    // Python bytes literal `b'\xde\xad...'`, lowercase to match repr().
    QString out;
    out.reserve(4 + data.size() * 4);
    out += QStringLiteral("b'");
    for (int i = 0; i < data.size(); ++i) {
        out += QStringLiteral("\\x%1")
            .arg((uint8_t)data[i], 2, 16, QChar('0'));
    }
    out += QLatin1Char('\'');
    QApplication::clipboard()->setText(out);
    emit statusHint(QStringLiteral("Copied %1 byte%2 as Python bytes")
        .arg(data.size()).arg(data.size() == 1 ? "" : "s"));
}

void RcxController::byteSaveAsFile(RcxEditor* editor) {
    auto range = editor->byteSelection();
    if (!range) return;
    uint64_t lo = range->first;
    int n = static_cast<int>(range->second - range->first);
    if (n <= 0 || n > (1 << 27)) {  // 128 MB sanity cap
        emit statusHint(QStringLiteral("Selection too large to save"));
        return;
    }
    QString defaultName = QStringLiteral("bytes_%1_%2.bin")
        .arg(lo, 0, 16).arg(n);
    QString path = QFileDialog::getSaveFileName(
        qobject_cast<QWidget*>(parent()),
        QStringLiteral("Save Bytes"),
        defaultName,
        QStringLiteral("Binary (*.bin);;All Files (*)"));
    if (path.isEmpty()) return;
    QString err;
    if (writeSelectedBytesToFile(lo, n, path, &err)) {
        emit statusHint(QStringLiteral("Saved %1 byte%2 to %3")
            .arg(n).arg(n == 1 ? "" : "s")
            .arg(QFileInfo(path).fileName()));
    } else {
        emit statusHint(err.isEmpty()
            ? QStringLiteral("Save failed") : err);
    }
}

void RcxController::bytePasteHex(RcxEditor* editor) {
    auto range = editor->byteSelection();
    if (!range || !this->provider()) return;
    if (!this->provider()->isWritable() || m_readOnlyOverride || isViewingPast()) {
        emit statusHint(isViewingPast() ? viewingPastHint() : QStringLiteral("Target is read-only"));
        return;
    }
    uint64_t lo = range->first;
    int n = static_cast<int>(range->second - range->first);
    if (n <= 0 || n > 65536) return;

    // Parse via ClipboardCodec::parseLenientHex (per-token left-pad + 0x
    // handling are unit-tested in tests/test_lenient_hex.cpp).
    QString parseErr;
    QByteArray bytes = ClipboardCodec::parseLenientHex(
        QApplication::clipboard()->text(), &parseErr);
    if (bytes.isEmpty()) {
        emit statusHint(parseErr.isEmpty()
            ? QStringLiteral("Clipboard isn't valid hex")
            : QStringLiteral("Clipboard: ") + parseErr);
        return;
    }
    // Clamp to the selection length: truncate longer, zero-pad shorter.
    QByteArray write(n, '\0');
    int copyN = qMin(bytes.size(), n);
    memcpy(write.data(), bytes.constData(), copyN);

    QByteArray oldBytes = this->provider()->isReadable(lo, n)
        ? this->provider()->readBytes(lo, n)
        : QByteArray(n, '\0');
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::WriteBytes{lo, oldBytes, write}));
    emit statusHint(QStringLiteral("Pasted %1 byte%2 at 0x%3")
        .arg(n).arg(n == 1 ? "" : "s").arg(lo, 0, 16));
}

void RcxController::byteZeroFill(RcxEditor* editor) {
    auto range = editor->byteSelection();
    if (!range || !this->provider()) return;
    if (!this->provider()->isWritable() || m_readOnlyOverride || isViewingPast()) {
        emit statusHint(isViewingPast() ? viewingPastHint() : QStringLiteral("Target is read-only"));
        return;
    }
    uint64_t lo = range->first;
    int n = static_cast<int>(range->second - range->first);
    if (n <= 0 || n > 65536) return;
    QByteArray oldBytes = this->provider()->isReadable(lo, n)
        ? this->provider()->readBytes(lo, n)
        : QByteArray(n, '\0');
    QByteArray zeros(n, '\0');
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::WriteBytes{lo, oldBytes, zeros}));
    emit statusHint(QStringLiteral("Zero-filled %1 byte%2 at 0x%3")
        .arg(n).arg(n == 1 ? "" : "s").arg(lo, 0, 16));
}

void RcxController::onByteSelectionRows(const QSet<uint64_t>& selIds) {
    // The byte selection owns the row selection while active: replace
    // m_selIds wholesale with the covered rows (may be empty → clears).
    // The editor marks them where the bytes are, so no anchors.
    m_selIds = selIds;
    m_selAnchors.clear();
    m_anchorLine = -1;
    updateCommandRow();
    applySelectionOverlays();
    emit selectionChanged(m_selIds.size());
}

void RcxController::addByteSubmenu(QMenu& menu, RcxEditor* editor) {
    if (!editor || !editor->hasByteSelection()) return;
    // Headline byte-selection action — promoted to the very top of the menu
    // (above the "Selected bytes ▸" submenu) so breaking a byte range into a
    // new class is always one click away whenever bytes are selected, not
    // buried. Mirrors the node menu's top-level "Carve".
    menu.addAction(themedVsIcon(QStringLiteral(":/vsicons/symbol-structure.svg"),
                                ThemeManager::instance().current().text, 16,
                                editor->devicePixelRatioF()),
                   tr("Carve"), [this, editor]() {
        auto r = editor->byteSelectionRange();
        extractByteSelectionToNewClass(r.first, r.second);
    });
    QMenu* sub = menu.addMenu(
        tr("Selected bytes (%1)").arg(editor->byteSelectionByteCount()));
    sub->addAction(tr("Copy as hex"),           [this, editor]() { byteCopyHex(editor); });
    sub->addAction(tr("Copy as C array"),       [this, editor]() { byteCopyCArray(editor); });
    sub->addAction(tr("Copy as Python bytes"),  [this, editor]() { byteCopyPython(editor); });
    sub->addAction(tr("Edit hex…"),             [editor]() { editor->beginByteEdit(); });
    sub->addAction(tr("Zero-fill"),             [this, editor]() { byteZeroFill(editor); });
    sub->addAction(tr("Paste hex"),             [this, editor]() { bytePasteHex(editor); });
    sub->addAction(tr("Save bytes as binary…"), [this, editor]() { byteSaveAsFile(editor); });
    menu.addSeparator();
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

    // Source chooser popup
    connect(editor, &RcxEditor::sourcePopupRequested,
            this, [this, editor](QPoint globalPos) {
        showSourcePopup(editor, globalPos);
    });

    // Delete key shortcut — same op as the ribbon's Selected ▸ Delete.
    connect(editor, &RcxEditor::deleteSelectedRequested,
            this, [this]() { deleteSelection(); });

    // Ctrl+D duplicate shortcut — same op as the ribbon's Selected ▸ Duplicate.
    connect(editor, &RcxEditor::duplicateSelectedRequested,
            this, [this]() { duplicateSelection(); });

    // Real clipboard (Ctrl+C / Ctrl+X / Ctrl+V).
    // Serialize via ClipboardCodec to "application/x-RC-nodes-v1" plus a
    // plain-text dump for external pastes. Cut = copy + delete. Paste wires
    // pasted nodes into the current view-root via a single undo macro.
    auto selectedRootIds = [this]() -> QVector<uint64_t> {
        // The same row → node mapping Delete uses: member rows (an enum field's
        // members, bitfield / code rows) stand for nothing, an array-element row
        // for its Array, a footer for its container — de-duplicated and sorted
        // by offset, so copied/cut nodes serialize in struct order.
        return orderedSelectedIds(SF_ArrayElemAsArray | SF_FooterAsContainer);
    };

    connect(editor, &RcxEditor::copyNodesRequested, this, [this, selectedRootIds]() {
        auto roots = selectedRootIds();
        if (roots.isEmpty()) return;
        auto* mime = ClipboardCodec::serialize(m_doc->tree, roots);
        QApplication::clipboard()->setMimeData(mime);
        emit statusHint(QStringLiteral("Copied %1 node(s)").arg(roots.size()));
    });

    connect(editor, &RcxEditor::cutNodesRequested, this, [this, selectedRootIds]() {
        auto roots = selectedRootIds();
        if (roots.isEmpty()) return;
        auto* mime = ClipboardCodec::serialize(m_doc->tree, roots);
        QApplication::clipboard()->setMimeData(mime);
        // Delete after successful copy — matches standard cut behaviour.
        QVector<int> indices;
        for (uint64_t id : roots) {
            int idx = m_doc->tree.indexOfId(id);
            if (idx >= 0) indices.append(idx);
        }
        if (indices.size() > 1) batchRemoveNodes(indices);
        else if (indices.size() == 1) removeNode(indices.first());
        emit statusHint(QStringLiteral("Cut %1 node(s)").arg(roots.size()));
    });

    connect(editor, &RcxEditor::pasteNodesRequested, this, [this]() {
        const QMimeData* mime = QApplication::clipboard()->mimeData();
        if (!mime) return;
        auto paste = ClipboardCodec::deserialize(m_doc->tree, mime);
        if (paste.nodes.isEmpty()) {
            emit statusHint(QStringLiteral("Nothing to paste — clipboard has no RC data"));
            return;
        }

        QSet<uint64_t> rootSet;
        for (uint64_t r : paste.rootIds) rootSet.insert(r);

        // Paste-below-selection: find the anchor node with the greatest end
        // offset among the current selection, and drop the pasted roots
        // immediately after it (pushing later siblings down). This matches
        // VS-style "add field below current row" behaviour; when nothing is
        // selected we fall back to append-at-container-end.
        uint64_t targetParent = m_viewRootId;
        int anchorEnd = -1;
        for (uint64_t sid : m_selIds) {
            uint64_t nid = baseNodeIdFromSelId(sid);
            int ai = m_doc->tree.indexOfId(nid);
            if (ai < 0) continue;
            const Node& a = m_doc->tree.nodes[ai];
            int asz = (a.kind == NodeKind::Struct || a.kind == NodeKind::Array)
                ? m_doc->tree.structSpan(a.id) : a.byteSize();
            int end = a.offset + asz;
            if (end > anchorEnd) {
                anchorEnd    = end;
                targetParent = a.parentId;
            }
        }

        // Span of a to-be-pasted root, computed from paste.nodes (not yet in
        // the tree). Struct/Array roots recurse over their captured children.
        std::function<int(uint64_t)> pastedSpan = [&](uint64_t id) -> int {
            int idx = -1;
            for (int i = 0; i < paste.nodes.size(); i++)
                if (paste.nodes[i].id == id) { idx = i; break; }
            if (idx < 0) return 0;
            const Node& n = paste.nodes[idx];
            if (n.kind != NodeKind::Struct && n.kind != NodeKind::Array)
                return n.byteSize();
            int maxEnd = 0;
            for (const Node& c : paste.nodes) {
                if (c.parentId != id) continue;
                int cend = c.offset + pastedSpan(c.id);
                if (cend > maxEnd) maxEnd = cend;
            }
            return maxEnd;
        };

        // Total span the pasted roots will occupy, accounting for alignment
        // between roots. Needed so we know how far to shift existing siblings.
        int pasteTotal = 0;
        for (uint64_t r : paste.rootIds) {
            int idx = -1;
            for (int i = 0; i < paste.nodes.size(); i++)
                if (paste.nodes[i].id == r) { idx = i; break; }
            if (idx < 0) continue;
            int align = alignmentFor(paste.nodes[idx].kind);
            pasteTotal = (pasteTotal + align - 1) / align * align + pastedSpan(r);
        }

        // Shift existing siblings at/after anchorEnd down by pasteTotal so
        // the newly-inserted block can take the space. Attached to the first
        // Insert command in the macro so one undo reverses the whole paste.
        QVector<cmd::OffsetAdj> shift;
        if (anchorEnd >= 0 && pasteTotal > 0) {
            for (int si : m_doc->tree.childrenOf(targetParent)) {
                const Node& s = m_doc->tree.nodes[si];
                if (s.offset >= anchorEnd)
                    shift.append(cmd::OffsetAdj{s.id, s.offset,
                                                s.offset + pasteTotal});
            }
        }

        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Paste nodes"));
        int placedBase = anchorEnd;  // -1 ⇒ append-at-end fallback
        bool firstRoot = true;
        for (Node& n : paste.nodes) {
            if (rootSet.contains(n.id)) {
                n.parentId = targetParent;
                int align = alignmentFor(n.kind);
                if (placedBase >= 0) {
                    n.offset = (placedBase + align - 1) / align * align;
                    placedBase = n.offset + pastedSpan(n.id);
                } else {
                    // Append path: after all current siblings (legacy).
                    int maxEnd = 0;
                    for (int si : m_doc->tree.childrenOf(targetParent)) {
                        const Node& sn = m_doc->tree.nodes[si];
                        int sz = (sn.kind == NodeKind::Struct || sn.kind == NodeKind::Array)
                            ? m_doc->tree.structSpan(sn.id) : sn.byteSize();
                        int end = sn.offset + sz;
                        if (end > maxEnd) maxEnd = end;
                    }
                    n.offset = (maxEnd + align - 1) / align * align;
                }
            }
            if (firstRoot && rootSet.contains(n.id)) {
                m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{n, shift}));
                firstRoot = false;
            } else {
                m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{n, {}}));
            }
        }
        m_doc->undoStack.endMacro();
        m_suppressRefresh = false;
        refresh();
        emit statusHint(QStringLiteral("Pasted %1 node(s) %2")
                        .arg(paste.rootIds.size())
                        .arg(anchorEnd >= 0 ? QStringLiteral("below selection")
                                            : QStringLiteral("at end")));
    });

    // ── Byte-selection actions ──
    // The editor handles the keyboard event + dispatches one of these
    // signals when the user has a hex byte selection (m_byteSel) and
    // presses Ctrl+C / Ctrl+V / Del. The controller does the I/O:
    // reads via the active provider's snapshot (so values match what
    // the user sees), writes via cmd::WriteBytes for undoability.

    // The handler bodies live in named methods (byteCopyHex, bytePasteHex,
    // …) so the right-click "Selected bytes ▸" submenu invokes exactly the
    // same logic as these Ctrl+C/V/Del keyboard shortcuts.
    connect(editor, &RcxEditor::byteCopyHexRequested, this,
            [this, editor]() { byteCopyHex(editor); });
    connect(editor, &RcxEditor::byteCopyAsCArrayRequested, this,
            [this, editor]() { byteCopyCArray(editor); });
    connect(editor, &RcxEditor::byteCopyAsPythonRequested, this,
            [this, editor]() { byteCopyPython(editor); });
    connect(editor, &RcxEditor::byteSaveAsFileRequested, this,
            [this, editor]() { byteSaveAsFile(editor); });
    connect(editor, &RcxEditor::bytePasteHexRequested, this,
            [this, editor]() { bytePasteHex(editor); });
    connect(editor, &RcxEditor::byteZeroFillRequested, this,
            [this, editor]() { byteZeroFill(editor); });
    connect(editor, &RcxEditor::byteBreakIntoClassRequested, this,
            [this](uint64_t lo, uint64_t hi) {
        extractByteSelectionToNewClass(lo, hi);
    });
    // Byte selection → row selection mirror (see onByteSelectionRows).
    connect(editor, &RcxEditor::byteSelectionRowsChanged, this,
            [this](const QSet<uint64_t>& ids) { onByteSelectionRows(ids); });

    // Byte-range Enter commit. beginByteEdit narrowed the inline edit
    // to a byte range; commitInlineEdit emitted this with the parsed
    // raw bytes. We push WriteBytes directly — bypassing setNodeValue
    // which expects a full-row hex value.
    connect(editor, &RcxEditor::byteRangeCommitRequested, this,
            [this](uint64_t addr, QByteArray bytes) {
        if (!this->provider()) return;
        if (!this->provider()->isWritable() || m_readOnlyOverride || isViewingPast()) {
            emit statusHint(isViewingPast() ? viewingPastHint() : QStringLiteral("Target is read-only"));
            return;
        }
        int n = bytes.size();
        if (n <= 0) return;
        QByteArray oldBytes = this->provider()->isReadable(addr, n)
            ? this->provider()->readBytes(addr, n)
            : QByteArray(n, '\0');
        // User edit — exclude from value history (see m_userEditRanges).
        m_userEditRanges.append({addr, addr + (uint64_t)n});
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::WriteBytes{addr, oldBytes, bytes}));
        emit statusHint(QStringLiteral("Wrote %1 byte%2 at 0x%3")
            .arg(n).arg(n == 1 ? "" : "s").arg(addr, 0, 16));
    });

    // Editor status-hint forwarder.
    connect(editor, &RcxEditor::statusHintRequested, this,
            [this](const QString& text) { emit statusHint(text); });

    // Quick type change (Space, 1-5, P, F, S, U keys) — the rule lives in
    // applyQuickTypeChange so the ribbon Type panel shares it verbatim.
    connect(editor, &RcxEditor::quickTypeChangeRequested,
            this, [this](int nodeIdx, NodeKind targetKind) {
        applyQuickTypeChange(nodeIdx, targetKind);
    });

    // Left/Right arrow: cycle through same-size type variants. With a
    // multi-row selection that spans different sizes (e.g. a Hex64 + two
    // Hex32s), each selected node cycles within its OWN size class — so
    // the Hex64 walks through 8-byte variants while the Hex32s walk
    // through 4-byte ones, in one combined undo step. Previously the
    // handler computed one global target from the cursor's size and
    // dropped any selected node whose size didn't match, which surprised
    // users with mixed-size selections.
    connect(editor, &RcxEditor::cycleSameSizeTypeRequested,
            this, [this](int nodeIdx, int direction) {
        if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;

        // Per-node target picker: builds the variant list for the node's
        // own size + string/vector specialisation rules and rotates by
        // `direction`. Returns the input kind unchanged when there is no
        // alternate variant (containers, lone-of-their-size kinds).
        auto computeTarget = [direction](NodeKind kind) -> NodeKind {
            int sz = sizeForKind(kind);
            if (sz <= 0 || isContainerKind(kind)) return kind;
            bool isStr = isStringKind(kind);
            bool isVec = isVectorKind(kind);
            QVector<NodeKind> variants;
            for (const auto& m : kKindMeta) {
                if (m.size != sz || isContainerKind(m.kind)) continue;
                if (!isStr && isStringKind(m.kind)) continue;
                if (!isVec && isVectorKind(m.kind)) continue;
                variants.append(m.kind);
            }
            if (variants.size() <= 1) return kind;
            int i = variants.indexOf(kind);
            if (i < 0) return kind;
            return variants[(i + direction + variants.size()) % variants.size()];
        };

        // Group rapid ←→ presses into a single undo macro
        if (!m_cycleMacroTimer) {
            m_cycleMacroTimer = new QTimer(this);
            m_cycleMacroTimer->setSingleShot(true);
            m_cycleMacroTimer->setInterval(800);
            connect(m_cycleMacroTimer, &QTimer::timeout, this, [this]() {
                if (m_cycleMacroOpen) {
                    m_doc->undoStack.endMacro();
                    m_cycleMacroOpen = false;
                }
            });
        }
        if (!m_cycleMacroOpen) {
            m_doc->undoStack.beginMacro(QStringLiteral("Cycle type"));
            m_cycleMacroOpen = true;
        }
        m_cycleMacroTimer->start();  // restart the 800ms timer

        // Apply to ALL selected nodes — each rotates within its own size class.
        if (m_selIds.size() > 1) {
            QVector<QPair<uint64_t, NodeKind>> jobs;  // (nodeId, targetKind)
            int skipped = 0;
            for (uint64_t sid : m_selIds) {
                uint64_t nid = baseNodeIdFromSelId(sid);
                int ni = m_doc->tree.indexOfId(nid);
                if (ni < 0) { skipped++; continue; }
                NodeKind curK = m_doc->tree.nodes[ni].kind;
                NodeKind tgtK = computeTarget(curK);
                if (tgtK == curK) { skipped++; continue; }
                jobs.append({m_doc->tree.nodes[ni].id, tgtK});
            }
            if (!jobs.isEmpty()) {
                QSet<uint64_t> savedSel = m_selIds;
                m_suppressRefresh = true;
                m_doc->undoStack.beginMacro(
                    QStringLiteral("Cycle type for %1 nodes").arg(jobs.size()));
                for (const auto& job : jobs) {
                    int ix = m_doc->tree.indexOfId(job.first);
                    if (ix >= 0) changeNodeKind(ix, job.second);
                }
                m_doc->undoStack.endMacro();
                m_suppressRefresh = false;
                m_selIds = savedSel;
                refresh();
                if (skipped > 0)
                    emit statusHint(
                        QStringLiteral("Cycled %1 nodes (%2 skipped: container or only variant)")
                            .arg(jobs.size()).arg(skipped));
                int ni = m_doc->tree.indexOfId(baseNodeIdFromSelId(*m_selIds.begin()));
                if (ni >= 0) emit nodeSelected(ni);
                return;
            }
        }

        // Single-selection (or multi-selection where nothing was eligible):
        // operate on the cursor's node only.
        NodeKind cur = m_doc->tree.nodes[nodeIdx].kind;
        NodeKind target = computeTarget(cur);
        if (target == cur) {
            emit statusHint(QStringLiteral("No type variants for %1-byte fields")
                .arg(sizeForKind(cur)));
            return;
        }
        changeNodeKind(nodeIdx, target);
        nodeIdx = m_doc->tree.indexOfId(m_doc->tree.nodes[nodeIdx].id);
        if (nodeIdx >= 0) emit nodeSelected(nodeIdx);
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
            if ((isContainerKind(n.kind) || enumTypeOf(m_doc->tree, n)) && !isCollapsed(n))
                m_doc->undoStack.push(new RcxCommand(this, cmd::Collapse{n.id, false, true}));
        }
        m_doc->undoStack.endMacro();
        m_suppressRefresh = false;
        refresh();
    });
    // Ctrl+Click on type/name span: open the referenced struct in a new
    // tab. Resolves the same target as F12 but routes through MainWindow
    // via requestOpenStructInNewTab so a fresh tab gets created sharing
    // this document.
    connect(editor, &RcxEditor::openTypeInNewTabRequested, this, [this](int nodeIdx) {
        if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
        const Node& n = m_doc->tree.nodes[nodeIdx];
        uint64_t target = 0;
        if (n.refId != 0) target = n.refId;
        else if (n.kind == NodeKind::Array && n.elementKind == NodeKind::Struct
                 && n.refId != 0) target = n.refId;
        else if (n.kind == NodeKind::Struct && n.parentId != 0) target = n.id;
        if (target == 0) {
            emit statusHint(QStringLiteral("No type to open"));
            return;
        }
        emit requestOpenStructInNewTab(target);
    });

    // F12: go to definition — navigate to the type referenced by the
    // current node. Pointer.refId, Struct.refId, or Array element struct.
    connect(editor, &RcxEditor::goToDefinitionRequested, this, [this, editor](int nodeIdx) {
        uint64_t target = resolveDefinitionTarget(nodeIdx);
        if (target == 0) {
            emit statusHint(QStringLiteral("No definition to navigate to"));
            return;
        }
        // Reuse existing tab if one already views this struct, else focus
        // here. Don't open a new tab — F12 is meant to be quick nav. F12 is
        // a jump (new view root, empty trail); the trail itself grows by
        // clicking into an expansion (handleNodeClick) or sideways through
        // the bar's chevrons. History is recorded HERE, at the gesture,
        // not inside setViewRootId — load, new-tab and delete-root call
        // that too and none of them is a place the user left.
        if (target != m_viewRootId) recordNav(editor);
        setViewRootId(target);
        emit statusHint(QStringLiteral("Jumped to definition"));
    });

    // Address-bar crumb click (an ancestor crumb or a « menu pick): collapse
    // everything below that class and scroll to it (carries the crumb
    // index; no view-root change). The trail itself is grown by selection,
    // in handleNodeClick.
    connect(editor, &RcxEditor::crumbClicked, this, &RcxController::collapseToFocus);

    // The bar's base edit: Enter → rebaseTo, and the verdict goes back to
    // the bar. A success has already pushed the new state through refresh
    // (the bar stores it while its overlay is up); the verdict closes the
    // overlay and applies it. A refusal keeps the overlay open with the
    // parser's words in the field — rebaseTo has put "Base: …" on the
    // status bar too.
    connect(editor, &RcxEditor::baseCommitRequested, this, [this, editor](const QString& expr) {
        QString err;
        const bool ok = rebaseTo(expr, &err);
        if (auto* bar = editor->addressBar()) bar->baseCommitFinished(ok, err);
    });
    // A recent / bookmark / module pick from the recent cell is a rebase.
    connect(editor, &RcxEditor::recentPickRequested, this,
            [this](const QString& formula) { rebaseTo(formula); });
    // The chip's context-menu Refresh.
    connect(editor, &RcxEditor::refreshRequested, this, [this] { refresh(); });
    // Back / Forward / Up / the history menu, all from this pane: it is the
    // one scrolled on restore and the one whose first visible row anchors
    // the entry recorded. Up is the parent crumb — one undo entry, one
    // history entry, through collapseToFocus.
    connect(editor, &RcxEditor::navBackRequested,    this, [this, editor] { goBack(editor); });
    connect(editor, &RcxEditor::navForwardRequested, this, [this, editor] { goForward(editor); });
    connect(editor, &RcxEditor::navUpRequested,      this, [this, editor] { goUp(editor); });
    connect(editor, &RcxEditor::timelineBackToLiveRequested, this, [this] { returnToLive(); });
    connect(editor, &RcxEditor::timelineRecordRequested, this, [this] { timelineToggleRecording(); });
    connect(editor, &RcxEditor::historyJumpRequested, this,
            [this, editor](int delta) { jumpToHistory(delta, editor); });
    // The history menu's rows, pulled when it opens — and the label of the
    // place the user is at, its checked "you are here" row.
    editor->setAddressBarHistory([this] { return backEntries(); },
                                 [this] { return forwardEntries(); },
                                 [this] { return currentNavLabel(); });
    // The places menu's bookmarks and modules, pulled when it opens. The
    // module list is the provider's memoised one — never enumerateModules,
    // which is a snapshot syscall on a process with hundreds of DLLs.
    editor->setAddressBarProviders(
        [this] { return m_doc->tree.bookmarks; },
        [this] {
            QStringList names;
            if (this->provider())
                for (const auto& m : this->provider()->modulesCached()) names << m.name;
            return names;
        });

    // Sideways navigation. A chevron pick is one undoable switch; a root
    // pick is the F12 jump (new root, empty trail); the path edit's Enter
    // resolves the typed path and answers the bar the way the base edit is
    // answered — a success closes the overlay, a miss keeps it open with the
    // seam in markerError (statusHint has already named the segment).
    // switchSibling and navigateToDrillPath record history themselves.
    connect(editor, &RcxEditor::siblingPickRequested, this, &RcxController::switchSibling);
    // The deepest crumb renamed in place. Same command line 0's class-name
    // inline edit pushes (EditTarget::RootClassName above) — one undo entry,
    // named by the node id the crumb carries, so a drilled crumb renames the
    // class it names rather than the view root. The bar is answered the way
    // the base and path edits are: a refusal keeps the overlay open with its
    // reason where the preview goes.
    connect(editor, &RcxEditor::classRenameRequested, this,
            [this, editor](uint64_t classId, const QString& name) {
        auto answer = [editor](bool ok, const QString& err = QString()) {
            if (auto* bar = editor->addressBar()) bar->classRenameFinished(ok, err);
        };
        const QString wanted = name.trimmed();
        if (wanted.isEmpty()) { answer(false, QStringLiteral("a class needs a name")); return; }
        const int idx = m_doc->tree.indexOfId(classId);
        if (idx < 0) { answer(false, QStringLiteral("that class is gone")); return; }
        const QString had = m_doc->tree.nodes[idx].structTypeName;
        if (had != wanted)
            m_doc->undoStack.push(new RcxCommand(this, cmd::ChangeStructTypeName{classId, had, wanted}));
        answer(true);   // renaming a class to the name it has is a no-op, not a refusal
    });

    connect(editor, &RcxEditor::pathCommitRequested, this, [this, editor](const QString& path) {
        QString err;
        const bool ok = navigateToDrillPath(path, &err);
        if (auto* bar = editor->addressBar()) bar->pathCommitFinished(ok, err);
    });
    // What the chevron menus and the path edit read from the tree, pulled
    // only when a menu opens or a key is typed — never per refresh. The bar
    // has no NodeTree; these keep it that way.
    {
        AddressBar::TreeQueries q;
        q.siblingsOf   = [this](int level) { return siblingsForCrumb(level); };
        q.fieldsAtPath = [this](const QString& path) { return drillFieldsAt(path); };
        q.validatePath = [this](const QString& text) {
            QString err;
            resolveDrillPath(m_doc->tree, text, nullptr, nullptr, &err);
            return err;
        };
        editor->setAddressBarTreeQueries(std::move(q));
    }

    // Source liveness on the bar's chip. The per-refresh state push carries
    // it too, but a source that just died gets no more refresh ticks, so the
    // push alone would leave the dot green: the status signal turns it the
    // moment the status flips. Context = the editor, so a closed split pane
    // takes the connection with it.
    connect(this, &RcxController::sourceStatusChanged, editor,
            [editor](SourceStatus st) { editor->addressBar()->setLiveness(int(st)); });

    connect(editor, &RcxEditor::expandAllRequested, this, [this]() {
        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Expand all"));
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            auto& n = m_doc->tree.nodes[i];
            if ((isContainerKind(n.kind) || enumTypeOf(m_doc->tree, n)) && isCollapsed(n))
                m_doc->undoStack.push(new RcxCommand(this, cmd::Collapse{n.id, true, false}));
        }
        m_doc->undoStack.endMacro();
        m_suppressRefresh = false;
        refresh();
    });

    // Comment edit (';' key) — respects selection; shared with the ribbon's
    // Selected ▸ Comment via commentSelection.
    connect(editor, &RcxEditor::commentEditRequested,
            this, [this, editor]() { commentSelection(editor); });

    // Footer "+1024" button
    // Footer "+1" pill / Down-at-end shortcut — append one Hex64 field
    // (struct/array) or one enum member (auto-numbered) at the end.
    // Caller may pass either a struct/array/enum id (footer pill click)
    // or a leaf field id (Down-at-end emits the last visible node id);
    // we walk up the parent chain in the leaf case so the field lands in
    // the enclosing container, not gets rejected.
    connect(editor, &RcxEditor::appendSingleFieldRequested,
            this, [this](uint64_t nodeId) {
        int si = m_doc->tree.indexOfId(nodeId);
        if (si < 0) return;
        // Expanded typed-pointer footer "+1": append to the REFERENCED class,
        // not the 8-byte pointer (which made an invalid child). appendBytes /
        // trim already do this redirect; mirror it here so all the footer pills
        // agree.
        {
            const Node& nd = m_doc->tree.nodes[si];
            if ((isPointerKind(nd.kind) || isFuncPtr(nd.kind)) && nd.refId != 0
                && m_doc->tree.childrenOf(nd.id).isEmpty()) {
                int ri = m_doc->tree.indexOfId(nd.refId);
                if (ri >= 0) si = ri;
            }
        }
        // Walk up to the enclosing struct/array/enum if a leaf was passed.
        while (si >= 0) {
            const Node& n = m_doc->tree.nodes[si];
            if (n.kind == NodeKind::Struct || n.kind == NodeKind::Array
                || n.isEnum()) break;
            if (n.parentId == 0) return;
            si = m_doc->tree.indexOfId(n.parentId);
        }
        if (si < 0) return;
        uint64_t structId = m_doc->tree.nodes[si].id;
        const Node& parent = m_doc->tree.nodes[si];
        if (parent.isEnum()) {
            auto members = parent.enumMembers;
            int64_t nextVal = members.isEmpty() ? 0 : members.last().second + 1;
            auto oldMembers = members;
            members.emplaceBack(QStringLiteral("Member%1").arg(nextVal), nextVal);
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::ChangeEnumMembers{structId, oldMembers, members}));
            return;
        }
        // Array: "+1" grows arrayLen by one. Without this branch we'd
        // insert a Hex64 child into a typed array (e.g. uint8_t[7]),
        // which leaves the header label out of sync with the contents
        // and breaks the element-by-element rendering (chunk_data_tail
        // showed a single 8-byte hex blob under a "uint8_t[7]" header).
        if (parent.kind == NodeKind::Array) {
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::ChangeArrayMeta{parent.id,
                    parent.elementKind, parent.elementKind,
                    parent.arrayLen, parent.arrayLen + 1}));
            return;
        }
        // Embedded struct with refId: append to the referenced root class
        uint64_t targetId = structId;
        if (m_doc->tree.childrenOf(structId).isEmpty() && parent.refId != 0)
            targetId = parent.refId;
        // Auto-name + auto-place at the end of the parent's children. Build
        // the Node inline (rather than calling insertNode) so we can capture
        // the reserved id and move selection to the freshly-appended field
        // afterward — without that, repeated Down kept the *original* field
        // selected and visually each new line inherited a "selected"
        // appearance from leftover markers.
        int slotOffset = 0;
        for (int ci : m_doc->tree.childrenOf(targetId)) {
            const Node& sib = m_doc->tree.nodes[ci];
            int sz = (sib.kind == NodeKind::Struct || sib.kind == NodeKind::Array)
                ? m_doc->tree.structSpan(sib.id) : sib.byteSize();
            slotOffset = qMax(slotOffset, sib.offset + sz);
        }
        // Footer "+1" pill literally means +1 byte — the label drove
        // user expectations. Previous version appended a Hex64 (+8) which
        // surprised everyone. For grow-by-multiple-bytes use the +10h
        // pill which calls appendBytesRequested instead.
        int align = alignmentFor(NodeKind::Hex8);
        Node n;
        n.kind     = NodeKind::Hex8;
        n.parentId = targetId;
        n.offset   = (slotOffset + align - 1) / align * align;
        n.name     = QStringLiteral("field_%1").arg(n.offset, 4, 16, QChar('0'));
        n.id       = m_doc->tree.reserveId();
        m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{n}));

        // Move selection to the new node so a subsequent Down appends after
        // it (and so the user can immediately retype the kind via U/S/F/P).
        m_selIds.clear();
        m_selIds.insert(n.id);
        m_anchorLine = -1;
        updateCommandRow();
        applySelectionOverlays();
        emit selectionChanged(m_selIds.size());
    });

    // Footer "+10h" pill — shared with the ribbon's Add
    // panel and the Append Bytes… dialog via appendBytes.
    connect(editor, &RcxEditor::appendBytesRequested,
            this, [this](uint64_t structId, int byteCount) {
        appendBytes(structId, byteCount);
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
        QVector<uint64_t> toRemove;
        for (int ci : children) {
            const Node& n = m_doc->tree.nodes[ci];
            if (!isHexNode(n.kind)) break;
            toRemove.append(n.id);
        }
        if (toRemove.isEmpty()) return;

        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Trim %1 trailing hex nodes").arg(toRemove.size()));
        // Retyping can append padding out of offset order. Each deletion
        // shifts storage indices, so resolve the saved ID immediately before use.
        for (uint64_t id : toRemove)
            removeNode(m_doc->tree.indexOfId(id));
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

    // Enum chip clicked — pop a themed member picker at the chip position;
    // on selection, write the chosen member's value back to the field.
    // The chip carries (enumRefNodeId, currentValue) so we don't have to
    // recompute them. EnumPickerPopup mirrors the type-chooser's visual
    // conventions (custom-painted rows with accent stripe + colored pip,
    // fuzzy filter when >10 members, footer crumb with keyboard hints)
    // so the whole app reads as one design family.
    connect(editor, &RcxEditor::enumChipClicked, this,
            [this, editor](int nodeIdx, uint64_t enumRefNodeId,
                           int64_t currentValue, QPoint globalPos) {
        if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
        int enumIdx = m_doc->tree.indexOfId(enumRefNodeId);
        if (enumIdx < 0) return;
        const Node& enumNode = m_doc->tree.nodes[enumIdx];
        if (!enumNode.isEnum() || enumNode.enumMembers.isEmpty()) return;

        QVector<EnumPickerPopup::Member> members;
        members.reserve(enumNode.enumMembers.size());
        for (const auto& m : enumNode.enumMembers)
            members.append({m.first, m.second});

        const auto& t = ThemeManager::instance().current();
        auto* popup = new EnumPickerPopup(editor);
        popup->setAttribute(Qt::WA_DeleteOnClose, true);
        popup->setOnChosen([this, nodeIdx](int64_t pickVal) {
            if (nodeIdx >= m_doc->tree.nodes.size()) return;
            setNodeValue(nodeIdx, /*subLine=*/0,
                         QString::number(pickVal),
                         /*isAscii=*/false, /*resolvedAddr=*/0);
        });

        QString enumName = enumNode.name.isEmpty()
            ? QStringLiteral("(anonymous)") : enumNode.name;
        popup->show(enumName, members, currentValue, t.indDataChanged, globalPos);
    });

    // A member row under an open enum field was chosen: the field takes it.
    connect(editor, &RcxEditor::enumMemberChosen, this, [this](int nodeIdx, int64_t value) {
        if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
        setNodeValue(nodeIdx, /*subLine=*/0, QString::number(value),
                     /*isAscii=*/false, /*resolvedAddr=*/0);
    });

    // TypeHint chip clicked — commit the inferred type. One click = one
    // commit: single-kind suggestion converts the field; multi-kind
    // suggestion (e.g. float×2) splits the original hex node into N
    // contiguous fields covering the SAME byte range — no padding
    // overflow into the next sibling.
    //
    // The first changeNodeKind shrinks the node to kinds[0] and inserts
    // a hex-pad sibling at offset+sizeof(kinds[0]) covering the gap.
    // Walking by `ni + 1` is wrong: insertNode appends to the end of
    // the array, so the pad sibling lives there, not at ni+1 (where the
    // *original* next field still is — converting that one is what
    // ate the next 4 bytes in the bug report). Find the pad by
    // OFFSET inside the same parent and convert it in place.
    connect(editor, &RcxEditor::typeHintChipClicked, this,
            [this](int nodeIdx, QVector<NodeKind> kinds) {
        if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
        if (kinds.isEmpty()) return;
        uint64_t nodeId = m_doc->tree.nodes[nodeIdx].id;

        // Collect the node IDs that the click actually touched so we can
        // narrow the selection to JUST those rows afterwards. Without
        // this, a pre-existing multi-select stays around and the user
        // sees the status bar say "41/49 selected" right after a chip
        // click that only mutated 1–2 fields.
        QSet<uint64_t> touched;

        if (kinds.size() == 1) {
            changeNodeKind(nodeIdx, kinds[0]);
            touched.insert(nodeId);
        } else {
            const Node& orig = m_doc->tree.nodes[nodeIdx];
            uint64_t parentId = orig.parentId;
            int curOffset = orig.offset;
            m_doc->undoStack.beginMacro(
                QStringLiteral("Split into %1 fields").arg(kinds.size()));
            int ni = m_doc->tree.indexOfId(nodeId);
            if (ni >= 0) {
                changeNodeKind(ni, kinds[0]);
                touched.insert(nodeId);
                curOffset += sizeForKind(kinds[0]);
            }
            for (int k = 1; k < kinds.size(); ++k) {
                int padIdx = -1;
                for (int sib : m_doc->tree.childrenOf(parentId)) {
                    const Node& sn = m_doc->tree.nodes[sib];
                    if (sn.offset == curOffset && isHexNode(sn.kind)) {
                        padIdx = sib;
                        break;
                    }
                }
                if (padIdx < 0) break;
                touched.insert(m_doc->tree.nodes[padIdx].id);
                changeNodeKind(padIdx, kinds[k]);
                curOffset += sizeForKind(kinds[k]);
            }
            m_doc->undoStack.endMacro();
        }

        // Narrow selection to just the converted node(s). Mirrors the
        // single-line pattern in insertNodeBelow: clear, insert, then
        // refresh overlays + command row + emit count.
        m_selIds.clear();
        for (uint64_t id : touched) {
            // Verify still present — changeNodeKind may have replaced it.
            if (m_doc->tree.indexOfId(id) >= 0)
                m_selIds.insert(id);
        }
        m_anchorLine = -1;
        updateCommandRow();
        applySelectionOverlays();
        emit selectionChanged(m_selIds.size());
    });

    // Live expression evaluation: the address bar's base edit reads it
    // through Callbacks::evaluate for its "= 0x…" preview, and the editor's
    // value edits show the same result when the text carries an operator.
    editor->setExprEvaluator([this](const QString& text) -> QString {
        QString s = text.trimmed();
        s.remove('`');
        s.remove('\'');
        if (s.isEmpty()) return {};
        const AddressParserCallbacks cbs =
            makeAddressCallbacks(this->provider().get(), m_doc->tree.pointerSize);
        auto result = AddressParser::evaluate(s, m_doc->tree.pointerSize, &cbs);
        if (!result.ok) return {};
        return QStringLiteral("0x") + QString::number(result.value, 16).toUpper();
    });

    // Inline editing signals
    connect(editor, &RcxEditor::inlineEditCommitted,
            this, [this](int nodeIdx, int subLine, EditTarget target, const QString& text,
                         uint64_t resolvedAddr) {
        // CommandRow root-class edits have nodeIdx=-1 (the base address is
        // the bar's edit: onBaseCommit → rebaseTo, never this signal)
        if (nodeIdx < 0
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
            // An enum-typed field takes a member name (or Type::Name) as well as a number.
            QString valueText = text;
            if (nodeIdx >= 0 && nodeIdx < m_doc->tree.nodes.size()) {
                if (const Node* def = enumTypeOf(m_doc->tree, m_doc->tree.nodes[nodeIdx])) {
                    QString name = text.trimmed();
                    const int scope = name.lastIndexOf(QStringLiteral("::"));
                    if (scope >= 0) name = name.mid(scope + 2);
                    for (const auto& m : def->enumMembers) {
                        if (m.first.compare(name, Qt::CaseInsensitive) == 0) {
                            valueText = QString::number(m.second);
                            break;
                        }
                    }
                }
            }
            setNodeValue(nodeIdx, subLine, valueText, /*isAscii=*/false, resolvedAddr);
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

// One gesture, one history entry: record the place being left (only when the
// pick actually moves — re-picking the root you are on must not leave an
// entry equal to where you still are), then switch. Kept out of
// setViewRootId because load, new-tab and delete-root call THAT and must
// not record.
void RcxController::pickViewRoot(uint64_t id, RcxEditor* from) {
    if (id != m_viewRootId) recordNav(from);
    setViewRootId(id);
}

void RcxController::setViewRootId(uint64_t id) {
    if (m_viewRootId == id) return;
    m_viewRootId = id;
    ++m_capturePlanEpoch;
    m_focusPath.clear();   // new root view → fresh trail (no drill yet)
    refresh();
}

uint64_t RcxController::resolveDefinitionTarget(int nodeIdx) const {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return 0;
    return drillTargetId(m_doc->tree.nodes[nodeIdx]);
}

void RcxController::reconcileFocusPath() {
    // Walk the chain: each focus hop must still exist, be drillable, be
    // expanded, and sit inside the frame the previous hop opened. Trim at the
    // first break so a fold-margin collapse can't leave a stale trail.
    uint64_t expectedContainer = m_viewRootId;
    int valid = 0;
    for (int i = 0; i < m_focusPath.size(); ++i) {
        int pi = m_doc->tree.indexOfId(m_focusPath[i]);
        if (pi < 0) break;
        const Node& p = m_doc->tree.nodes[pi];
        if (drillTargetId(p) == 0 || isCollapsed(p)) break;
        if (expectedContainer != 0 && containerOf(m_doc->tree, p.id) != expectedContainer) break;
        expectedContainer = drillTargetId(p);  // refId class, or the embedded struct itself
        valid = i + 1;
    }
    if (valid < m_focusPath.size()) m_focusPath.resize(valid);
}

QVector<uint64_t> RcxController::focusChainTo(uint64_t pid) const {
    QVector<uint64_t> chain;
    QSet<uint64_t> seen;
    uint64_t cur = pid;
    while (cur != 0 && !seen.contains(cur)) {
        seen.insert(cur);
        chain.prepend(cur);
        uint64_t container = containerOf(m_doc->tree, cur);
        if (container == 0) return {};                  // orphan
        if (container == m_viewRootId) return chain;    // reached the view root
        if (m_viewRootId == 0) {                        // show-all: any ROOT is a base
            int ci = m_doc->tree.indexOfId(container);  // (an embedded frame is not)
            if (ci >= 0 && m_doc->tree.nodes[ci].parentId == 0) return chain;
        }
        cur = expandedHopInto(viewTree(), container);
    }
    return {};  // no expanded chain reaches the view root
}

QVector<uint64_t> RcxController::focusChainToNode(uint64_t nodeId) const {
    int idx = m_doc->tree.indexOfId(nodeId);
    if (idx < 0) return {};
    const Node& n = m_doc->tree.nodes[idx];
    // The selected node is itself an expanded typed pointer → its own chain.
    if (drillTargetId(n) != 0 && !isCollapsed(n))
        return focusChainTo(n.id);
    // Otherwise it sits inside some frame. If that frame is the view root it's
    // a top-level row (no focus → bare root crumb); else the frame is the
    // refId class of the expanded pointer that inline-rendered it, or an
    // embedded struct expanded in place — return that hop's chain, so
    // selecting inside a NewClass* adds NewClass and inside Player.stats adds
    // stats.
    uint64_t container = containerOf(m_doc->tree, nodeId);
    if (container == 0 || container == m_viewRootId) return {};
    const uint64_t hop = expandedHopInto(viewTree(), container);
    return hop ? focusChainTo(hop) : QVector<uint64_t>{};
}

void RcxController::collapseToFocus(int crumbIndex) {
    // The slot: the pane is the signal's sender (a crumb click in pane B
    // scrolls pane B only), else the primary editor.
    collapseToFocusIn(crumbIndex, gestureEditor(nullptr));
}

void RcxController::collapseToFocusIn(int crumbIndex, RcxEditor* ed) {
    if (crumbIndex < 0) return;
    // crumbIndex 0 = root class; i = the class shown by focus pointer i-1.
    // Collapse focus[crumbIndex] (the pointer that opens everything below this
    // class), then truncate the path to crumbIndex.
    const uint64_t collapsePid =
        (crumbIndex < m_focusPath.size()) ? m_focusPath[crumbIndex] : 0;
    const uint64_t scrollPid =
        (crumbIndex >= 1 && crumbIndex - 1 < m_focusPath.size())
            ? m_focusPath[crumbIndex - 1] : 0;
    // A crumb click is a navigation gesture: record the place being left
    // — but only when the trail actually shortens (a click past the end
    // moves nothing and must not leave an entry equal to here).
    if (crumbIndex < m_focusPath.size()) recordNav(ed);
    m_focusPath.resize(qMin(crumbIndex, (int)m_focusPath.size()));

    bool pushed = false;
    if (collapsePid) {
        int idx = m_doc->tree.indexOfId(collapsePid);
        if (idx >= 0 && !isCollapsed(m_doc->tree.nodes[idx])) {
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::Collapse{collapsePid, false, true}));  // auto-refresh
            pushed = true;
        }
    }
    if (!pushed) refresh();
    if (ed) ed->scrollNodeToTop(scrollPid ? scrollPid : m_viewRootId);
}

// The class crumb `level` names, walked from the view root down the focus
// path the same way addressBarState builds the crumbs — so a menu built
// here and the crumb it hangs under always agree. 0 when a hop is gone.
static uint64_t classAtLevel(const NodeTree& tree, uint64_t viewRootId,
                             const QVector<uint64_t>& focusPath, int level) {
    uint64_t container = viewRootId;
    for (int i = 0; i < level && i < focusPath.size(); ++i) {
        const int pi = tree.indexOfId(focusPath[i]);
        if (pi < 0) return 0;
        container = drillTargetId(tree.nodes[pi]);
    }
    // A show-all view (root 0) labels the first root struct; its fields are
    // the ones the root crumb's chevron lists.
    if (container == 0) {
        const int ri = detail::firstRootStructIdx(tree);
        return ri >= 0 ? tree.nodes[ri].id : 0;
    }
    return container;
}

// The first row rendered INSIDE a hop's expansion: deeper than the hop's
// own row, which its continuation rows share (same depth, same nodeId).
// -1 when the expansion is empty or the rows end. What a pointer hop's
// target address is read off (compose stamps ptrBase on every row inside
// the dereference) — for the crumbs and the sibling menus alike.
static int firstDeeperRowIn(const QVector<LineMeta>& meta, int line, uint64_t hopId) {
    const int hopDepth = meta[line].depth;
    for (int j = line + 1; j < meta.size(); ++j) {
        if (meta[j].depth > hopDepth) return j;
        if (meta[j].nodeId != hopId) break;   // left the hop's rows without going deeper
    }
    return -1;
}

QVector<SiblingEntry> RcxController::siblingsForCrumb(int level) const {
    if (level < 0 || level > m_focusPath.size()) return {};
    const NodeTree& tree = m_doc->tree;
    const uint64_t classId = classAtLevel(tree, m_viewRootId, m_focusPath, level);
    if (classId == 0) return {};
    const uint64_t current = level < m_focusPath.size() ? m_focusPath[level] : 0;
    QVector<SiblingEntry> sibs = siblingFieldsOf(tree, classId, current);

    // Where each field leads. The container is the frame the crumb at
    // `level` names — frameAddresses(), the crumbs' own data — and its rows
    // start after the hop that opened it. At the root 0 is a real address
    // (the base); deeper, 0 is "unknown", and nothing under an unknown
    // frame can be placed either.
    const auto& meta = m_lastResult.meta;
    const QVector<uint64_t> frames = frameAddresses();
    const uint64_t containerAddr = level < frames.size() ? frames[level] : 0;
    const bool containerKnown = level == 0 || containerAddr != 0;
    int cursor = 0;
    if (level > 0) {
        const QVector<int> hops = focusHopLines();
        cursor = (level - 1 < hops.size() && hops[level - 1] >= 0) ? hops[level - 1] + 1 : -1;
    }
    const Provider* prov = this->provider().get();
    const bool canRead = prov && prov->isValid();
    for (SiblingEntry& e : sibs) {
        const int ni = tree.indexOfId(e.id);
        if (ni < 0) continue;
        const Node& n = tree.nodes[ni];
        const uint64_t own = containerAddr + (n.offset > 0 ? uint64_t(n.offset) : 0);
        if (!isPointerKind(n.kind)) {
            // An embedded struct or an array dereferences nothing: it sits
            // at the container plus its offset, expanded or not.
            if (containerKnown) e.address = own;
            continue;
        }
        // An expanded pointer: compose already dereferenced it — the
        // ptrBase on the first row inside it, exactly what its crumb says.
        // The scan starts inside the container's frame, so the right
        // instance is found when one class is open under two pointers.
        if (e.expanded && cursor >= 0) {
            int line = -1;
            for (int j = cursor; j < meta.size(); ++j) {
                if (meta[j].nodeId != e.id) continue;
                if (meta[j].lineKind == LineKind::Footer || meta[j].lineKind == LineKind::CommandRow)
                    continue;
                line = j;
                break;
            }
            const int inner = line >= 0 ? firstDeeperRowIn(meta, line, e.id) : -1;
            if (inner >= 0) { e.address = meta[inner].ptrBase; continue; }
        }
        // Collapsed (or expanded onto an empty class, which renders no row
        // to read from): ONE read of the pointer value, by compose's rule —
        // a Pointer32 is four bytes, a sentinel is null, an RVA is
        // base-relative. No source, an unknown container or a failed read
        // leave it unknown. Never a module enumeration: this runs when a
        // menu opens, once per row.
        if (!canRead || !containerKnown) continue;
        uint64_t v = 0;
        if (n.kind == NodeKind::Pointer32) {
            uint32_t v32 = 0;
            if (!prov->read(own, &v32, sizeof v32)) continue;
            v = v32;
            if (v == 0xFFFFFFFFu) v = 0;
        } else {
            if (!prov->read(own, &v, sizeof v)) continue;
            if (v == UINT64_MAX) v = 0;
        }
        if (n.isRelative && v != 0) v += baseAddress();
        // A dangling pointer must say what its crumb would say: compose
        // stamps ptrBase 0 for an unreadable target, so the row does too.
        if (v != 0 && !prov->isReadable(v, 1)) v = 0;
        e.address = v;
    }
    return sibs;
}

QVector<SiblingEntry> RcxController::drillFieldsAt(const QString& path) const {
    const NodeTree& tree = m_doc->tree;
    if (path.trimmed().isEmpty()) {
        // Nothing typed yet: the first segment is a root class, so offer
        // those in the same row shape the field menu uses.
        QVector<SiblingEntry> roots;
        for (const RootEntry& r : rootClassEntries(tree)) {
            SiblingEntry e;
            e.id = r.id; e.field = r.label; e.classLabel = r.label; e.keyword = r.keyword;
            roots.push_back(e);
        }
        return roots;
    }
    uint64_t root = 0;
    QVector<uint64_t> hops;
    if (!resolveDrillPath(tree, path, &root, &hops, nullptr)) return {};
    uint64_t container = root;
    if (!hops.isEmpty()) {
        const int pi = tree.indexOfId(hops.last());
        if (pi < 0) return {};
        container = drillTargetId(tree.nodes[pi]);
    }
    return siblingFieldsOf(tree, container, 0);
}

bool RcxController::switchSibling(int level, uint64_t newPointerId) {
    if (level < 0 || level > m_focusPath.size()) return false;
    const NodeTree& tree = m_doc->tree;
    const int ni = tree.indexOfId(newPointerId);
    if (ni < 0 || drillTargetId(tree.nodes[ni]) == 0) return false;   // not a hop
    // The hop has to belong to the class the crumb at `level` names — the
    // same class whose fields the chevron menu listed. A pointer that lives
    // in some other class (a stale menu, a caller's wrong id) would be
    // spliced into a trail it is not on; reconcileFocusPath would then trim
    // it on the next refresh, after the undo entry was already pushed.
    const uint64_t classId = classAtLevel(tree, m_viewRootId, m_focusPath, level);
    if (classId == 0) return false;
    bool listed = false;
    for (const SiblingEntry& e : siblingFieldsOf(tree, classId, 0))
        if (e.id == newPointerId) { listed = true; break; }
    if (!listed) return false;
    const uint64_t oldPointer = level < m_focusPath.size() ? m_focusPath[level] : 0;
    if (oldPointer == newPointerId) return false;   // already there: nothing to undo
    // Past every refusal: the switch WILL happen, so this is the place
    // being left. The pane is read now, before anything below could
    // disturb the slot's sender.
    RcxEditor* ed = gestureEditor(nullptr);
    recordNav(ed);
    const int oi = oldPointer ? tree.indexOfId(oldPointer) : -1;
    const bool collapseOld = oi >= 0 && !isCollapsed(tree.nodes[oi]);
    const bool expandNew   = isCollapsed(tree.nodes[ni]);
    const Node& hop = tree.nodes[ni];
    const QString field = hop.name.isEmpty() ? fmt::typeNameRaw(hop.kind) : hop.name;
    // One gesture, one undo entry: the collapse and the expand travel
    // together (the "Collapse all" / "Expand all" shape). Refresh is held
    // until the focus path is set below, so the crumbs never show a
    // half-switched trail.
    if (collapseOld || expandNew) {
        const bool wasSuppressed = m_suppressRefresh;
        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Switch to %1").arg(field));
        if (collapseOld)
            m_doc->undoStack.push(new RcxCommand(this, cmd::Collapse{oldPointer, false, true}));
        if (expandNew)
            m_doc->undoStack.push(new RcxCommand(this, cmd::Collapse{newPointerId, true, false}));
        m_doc->undoStack.endMacro();
        m_suppressRefresh = wasSuppressed;
    }
    m_focusPath.resize(level);
    m_focusPath.push_back(newPointerId);
    refresh();
    if (ed) ed->scrollNodeToTop(newPointerId);
    return true;
}

bool RcxController::navigateToDrillPath(const QString& text, QString* err) {
    const NodeTree& tree = m_doc->tree;
    uint64_t root = 0;
    QVector<uint64_t> path;
    QString why;
    if (!resolveDrillPath(tree, text, &root, &path, &why)) {
        if (err) *err = why;
        emit statusHint(QStringLiteral("Path: %1").arg(why));
        return false;
    }
    // A show-all view names the first root struct in its crumb and its
    // trail text, so committing that text unchanged must stay a no-op
    // there too — only a path into ANOTHER root switches the view.
    const int firstRoot = detail::firstRootStructIdx(tree);
    const bool rootDiffers = m_viewRootId != 0
        ? root != m_viewRootId
        : (firstRoot < 0 || tree.nodes[firstRoot].id != root);
    QVector<uint64_t> toExpand;
    for (uint64_t hop : path) {
        const int pi = tree.indexOfId(hop);
        if (pi >= 0 && isCollapsed(tree.nodes[pi])) toExpand.push_back(hop);
    }
    // The place being left — only when the commit moves somewhere (the
    // trail re-committed as it stands is a no-op, and records nothing).
    RcxEditor* ed = gestureEditor(nullptr);
    if (rootDiffers || path != m_focusPath) recordNav(ed);
    // setViewRootId is the one root switch (it clears the focus path and
    // refreshes on its own — a second refresh below then shows the trail);
    // the expands are held so the crumbs never show a half-built path.
    if (rootDiffers) setViewRootId(root);
    const bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    if (!toExpand.isEmpty()) {
        m_doc->undoStack.beginMacro(QStringLiteral("Navigate to path"));
        for (uint64_t hop : toExpand)
            m_doc->undoStack.push(new RcxCommand(this, cmd::Collapse{hop, true, false}));
        m_doc->undoStack.endMacro();
    }
    m_suppressRefresh = wasSuppressed;
    m_focusPath = path;
    refresh();
    if (ed) {
        if (!path.isEmpty()) ed->scrollNodeToTop(path.last());
        else                 ed->scrollNodeToTop(m_viewRootId);
    }
    if (err) err->clear();
    return true;
}

QString RcxController::classLabelOf(uint64_t id) const {
    int idx = id ? m_doc->tree.indexOfId(id) : -1;
    if (idx >= 0) return nodeClassLabel(m_doc->tree.nodes[idx]);
    // No single view root (show-all): fall back to the first root struct name.
    const QStringList roots = rootClassNames(m_doc->tree);
    return roots.isEmpty() ? QStringLiteral("…") : roots.first();
}

// Map each focus hop to its rendered line with ONE forward scan whose
// cursor only advances: hop i sits inside hop i-1's expansion, so its row
// is below hop i-1's, and searching from there also picks the right
// instance when one class is expanded under two pointers (the same
// `parent` node renders twice; only the one under our hop counts). The
// editor's node→line index is private to it, and this runs once per
// refresh (and once per menu open) on a path that is 0-3 hops deep, so a
// scan is cheaper than plumbing an accessor; the bar's setState change
// guard makes an equal result free downstream.
//
// Show-all (m_viewRootId == 0) renders every root in order, so the scan
// starts on the first root's own rows — the root classLabelOf(0) names.
// A hop expanded under a LATER root matches there instead, and the trail
// then names the first root while pointing at the other one. That is the
// pre-existing show-all ambiguity classLabelOf(0) already has (the trail
// is click-built from one instance; only its label is guessed) and is
// left as is: a single view root has no such case.
QVector<int> RcxController::focusHopLines() const {
    const auto& meta = m_lastResult.meta;
    QVector<int> hopLine(m_focusPath.size(), -1);
    for (int i = 0, cursor = 0; i < m_focusPath.size(); ++i) {
        for (int j = cursor; j < meta.size(); ++j) {
            if (meta[j].nodeId != m_focusPath[i]) continue;
            if (meta[j].lineKind == LineKind::Footer || meta[j].lineKind == LineKind::CommandRow)
                continue;
            hopLine[i] = j;
            cursor = j + 1;
            break;
        }
        if (hopLine[i] < 0) break;   // nothing rendered for it: deeper hops are unknown too
    }
    return hopLine;
}

// The last compose already stamped every row inside a pointer expansion
// with the dereferenced target (LineMeta::ptrBase) and every row with its
// own absolute address (offsetAddr); the frames are read off those.
//
// [0] is the view root's own address: compose places a root at
// baseAddress + offset (its absOffsets seed), and the root header row is
// suppressed (the command row carries it), so it is computed, not scanned.
// [i+1] is the frame hop i opens, by the hop's KIND — not by whether
// drillTargetId returns the hop itself, which misfiled two real shapes: an
// embedded Struct field WITH a refId (the type chooser's embed-class shape,
// compose's "embedded struct with refId but no child nodes") and an Array
// of struct with a refId both drill through refId, yet neither
// dereferences anything, so compose never moves currentPtrBase for them
// and the pointer rule handed back the ENCLOSING frame's base.
//   Pointer: the target is the ptrBase compose stamped on the first row
//   rendered INSIDE it (deeper than the pointer's own row — its
//   continuation rows share its depth and its nodeId).
//   Struct (refId or not): the frame IS its row — offsetAddr.
//   Array: the first deeper row is the [0] element separator, whose
//   offsetAddr is that element's base.
// 0 when the hop has no row, the expansion is empty, or the pointer was
// null / unreadable (compose zero-fills those under a NullProvider with
// pBase = 0).
QVector<uint64_t> RcxController::frameAddresses() const {
    const NodeTree& tree = m_doc->tree;
    const auto& meta = m_lastResult.meta;
    const QVector<int> hopLine = focusHopLines();
    QVector<uint64_t> out(m_focusPath.size() + 1, 0);
    {
        const int ri = detail::classIdx(tree, m_viewRootId);
        const int off = ri >= 0 ? tree.nodes[ri].offset : 0;
        out[0] = baseAddress() + (off > 0 ? uint64_t(off) : 0);
    }
    for (int i = 0; i < m_focusPath.size(); ++i) {
        const int line = hopLine[i];
        const int pi = tree.indexOfId(m_focusPath[i]);
        if (line < 0 || pi < 0) break;
        const Node& hop = tree.nodes[pi];
        if (hop.kind == NodeKind::Struct) { out[i + 1] = meta[line].offsetAddr; continue; }
        const int inner = firstDeeperRowIn(meta, line, hop.id);
        if (inner < 0) continue;
        if (hop.kind == NodeKind::Array)      out[i + 1] = meta[inner].offsetAddr;
        else if (isPointerKind(hop.kind))     out[i + 1] = meta[inner].ptrBase;
    }
    return out;
}

AddressBarState RcxController::addressBarState() {
    reconcileFocusPath();
    const NodeTree& tree = m_doc->tree;

    // The class id a crumb names. classLabelOf(0) (show-all, no single view
    // root) names the first root struct; carry that node's id for the same
    // case so a crumb never says one class and points at another.
    auto classIdFor = [&](uint64_t container) -> uint64_t {
        if (container != 0 && tree.indexOfId(container) >= 0) return container;
        for (const Node& n : tree.nodes)
            if (n.parentId == 0 && n.kind == NodeKind::Struct) return n.id;
        return 0;
    };
    auto keywordFor = [&](uint64_t classId) -> QString {
        const int ci = classId ? tree.indexOfId(classId) : -1;
        return ci >= 0 ? tree.nodes[ci].resolvedClassKeyword() : QString();
    };

    // Per-crumb addresses come from the last compose (frameAddresses: the
    // view root first, then the frame each hop opens); the sibling menus
    // read the same vector, so a crumb and the rows under its chevron
    // never disagree about where a class sits.
    const QVector<uint64_t> frames = frameAddresses();

    // Dotted "class.field" crumbs: each crumb is the class you were IN plus the
    // field you followed OUT of it (focusPath[i] is the pointer left via);
    // the final crumb is the current class with no trailing field. Crumb i is
    // clickable → collapseToFocus(i) (go back to that class). Avoids the
    // redundant-looking "field › class" pair on auto-named pointers.
    QVector<Crumb> crumbs;
    uint64_t container = m_viewRootId;  // depth 0 container = the view root
    uint64_t address   = frames.value(0);
    for (int i = 0; i < m_focusPath.size(); ++i) {
        int pi = tree.indexOfId(m_focusPath[i]);
        if (pi < 0) break;
        const Node& p = tree.nodes[pi];
        QString field = p.name.isEmpty() ? fmt::typeNameRaw(p.kind) : p.name;
        Crumb c;
        c.label     = classLabelOf(container) + QStringLiteral(".") + field;
        c.rootId    = (uint64_t)i;
        c.classId   = classIdFor(container);
        c.pointerId = i > 0 ? m_focusPath[i - 1] : 0;
        c.address   = address;
        c.keyword   = keywordFor(c.classId);
        crumbs.push_back(c);
        // Next depth's container = the frame this hop opens: the pointer's
        // refId class, or the embedded struct itself (refId is 0 there, and
        // classLabelOf(0) would name the first root class instead).
        container = drillTargetId(p);
        address   = frames.value(i + 1);
    }
    // Current (deepest) class — `container` is the frame the last hop opens, or
    // the view root when nothing is drilled.
    {
        const int n = crumbs.size();   // == the number of hops that resolved
        Crumb c;
        c.label     = classLabelOf(container);
        c.rootId    = (uint64_t)n;
        c.classId   = classIdFor(container);
        c.pointerId = n > 0 ? m_focusPath[n - 1] : 0;
        c.address   = address;
        c.keyword   = keywordFor(c.classId);
        crumbs.push_back(c);
    }

    AddressBarState s;
    s.sourceName   = this->provider() ? this->provider()->name() : QString();
    // The saved-source kind is the provider IDENTIFIER (iconForProvider's
    // vocabulary); Provider::kind() is a display word. A live attach that
    // registered no saved source (tutorial self-attach, MCP attach) has no
    // identifier to give, and the chip falls back to the generic icon.
    s.sourceKindId = (m_activeSourceIdx >= 0 && m_activeSourceIdx < m_savedSources.size())
        ? m_savedSources[m_activeSourceIdx].kind : QString();
    s.liveness     = int(m_lastStatus);
    s.baseAddress  = baseAddress();
    s.baseFormula  = baseAddressFormula();
    s.resolvedBase = baseAddress();
    s.crumbs       = crumbs;
    s.trailPath    = trailPathText(tree, m_viewRootId, m_focusPath);
    s.viewRootId   = m_viewRootId;
    s.canBack      = canGoBack();
    s.canForward   = canGoForward();
    s.canUp        = canGoUp();
    // The timeline's buttons beside the address — only for a live source the
    // timeline can record: a static file's bar keeps every pixel for the trail.
    s.timeline  = m_timelineEnabled && m_timelineCtx != nullptr;
    s.canRecord = m_timelineCtx != nullptr;
    s.recording = timelineRecording();
    s.past      = isViewingPast();
    return s;
}

void RcxController::pushAddressBarState() {
    const AddressBarState s = addressBarState();
    for (auto* editor : m_editors)
        editor->setAddressBarState(s);
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
        m_lastValueBytes.clear();
        for (auto& lm : m_lastResult.meta)
            lm.heatLevel = 0;
        refresh();
    }
}

void RcxController::resetChangeTracking() {
    m_changedRuns.clear();
    ++m_diffGen;
    m_valueHistory.clear();
    m_lastValueAddr.clear();
    m_lastValueBytes.clear();
    m_prevPages.clear();
    m_valueTrackCooldown = 5; // suppress tracking for ~1s
    for (auto& lm : m_lastResult.meta)
        lm.heatLevel = 0;
}

void RcxController::refresh() {
    PROFILE_SCOPE("refresh");
    m_viewStale = false;
    // Bracket compose with thread-local doc pointer for type name resolution.
    // RAII guard restores the previous value on scope exit — safe against any
    // exception compose might throw.
    ComposeDocGuard composeGuard(m_doc);

    // Build symbol lookup callback. The unified NameRegistry aggregates
    // every registered NameProvider (PDB + RTTI + bookmarks + future
    // plugin sources) so a single callback labels addresses from any
    // source. We reach it through a function-pointer hook (set by the
    // GUI app in main.cpp) so this translation unit stays headless-safe
    // for test targets that don't link the names library.
    //
    // PdbNameProvider::nameFor already routes through SymbolStore's
    // binary-search reverse index AND demangles the result, so we don't
    // need a separate SymbolStore fallback — adding one would just paint
    // raw "??0?$vector@..." mangled names back over the humanised result.
    // Test builds (which don't set the hook) fall back to SymbolStore so
    // they still get symbol annotations, just without demangling.
    SymbolLookupFn symLookup;
    if (this->provider()) {
        auto* prov = this->provider().get();
        symLookup = [prov](uint64_t addr) -> QString {
            if (g_nameLookupHook) return g_nameLookupHook(addr, prov);
            return SymbolStore::instance().getSymbolForAddress(addr, prov);
        };
    }

    // Compose against the past being shown, else the snapshot, else the real
    // provider. The past is composed through TODAY's structure — that is the
    // point — but at the base address the class had THEN.
    const bool past = isViewingPast() && m_frameProv && m_frameProv->frame();
    const Provider* data = past ? static_cast<const Provider*>(m_frameProv.get())
                         : m_snapshotProv ? static_cast<const Provider*>(m_snapshotProv.get())
                                          : provider().get();
    NodeTree pastTree;
    const NodeTree* composeTree = &viewTree();
    if (past && m_timelineHub) {
        const uint64_t thenBase = m_timelineHub->baseAt(m_frameProv->frame()->timeMs,
                                                        composeTree->baseAddress);
        if (thenBase != composeTree->baseAddress) {
            pastTree = *composeTree;
            pastTree.baseAddress = thenBase;
            composeTree = &pastTree;
        }
    }
    // While capturing, note every page compose reads through the snapshot, so
    // the next ticks read exactly those — the capture set becomes what the
    // view reads, not what the range walk guessed.
    QSet<uint64_t> touched;
    const bool recordTouches = !past && m_timelineCtx && m_snapshotProv
                            && data == static_cast<const Provider*>(m_snapshotProv.get());
    if (recordTouches) m_snapshotProv->beginTouchRecording(&touched);
    if (data)
        m_lastResult = rcx::compose(*composeTree, *data, m_viewRootId, m_compactColumns, m_treeLines, m_braceWrap, m_typeHints, m_showComments, symLookup, m_showRtti, m_showEnumChips);
    else
        m_lastResult = {};
    if (recordTouches) {
        m_snapshotProv->endTouchRecording();
        for (auto it = m_touchedAge.begin(); it != m_touchedAge.end();) {
            if (touched.contains(it.key())) { it.value() = 0; ++it; }
            else if (++it.value() > kTouchedPageMaxAge) it = m_touchedAge.erase(it);
            else ++it;
        }
        for (uint64_t page : std::as_const(touched)) {
            if (m_touchedAge.contains(page)) continue;
            if (m_touchedAge.size() >= kTouchedPageCap) break;
            m_touchedAge.insert(page, 0);
        }
    }
    // The fields as laid out now: what the timeline counts a change in.
    if (!past && data && m_timelineCtx) rebuildFieldIndex();

    // Mark lines whose node data changed since last refresh. Runs and
    // lm.offsetAddr are both ABSOLUTE; subtracting the base here (as this
    // used to) meant nothing ever matched for a class not at address 0.
    // In the past, a value the frame could not supply is either a read that
    // failed THEN (a real unreadable) or bytes nobody was watching then.
    // Only the first is an error.
    if (past) {
        QString& text = m_lastResult.text;
        const QVector<int>& starts = m_lastResult.lineStarts;
        for (int i = 0; i < m_lastResult.meta.size(); ++i) {
            LineMeta& lm = m_lastResult.meta[i];
            if (lm.unreadable) continue;              // failed THEN: struck, as it was live
            const int len = rowValueBytes(*composeTree, lm);
            if (len <= 0) continue;
            bool missing = m_frameProv->notCaptured(lm.offsetAddr, len);
            const Node& n = composeTree->nodes[lm.nodeIdx];
            if (!missing && !lm.isArrayElement && n.kind == NodeKind::Pointer64 && n.ptrDepth > 0
                && isValidPrimitivePtrTarget(n.elementKind)) {
                // "-> 87" shows its target's bytes too.
                uint64_t target = m_frameProv->readU64(lm.offsetAddr);
                for (int d = 1; d < n.ptrDepth && target != 0 && !missing; ++d) {
                    missing = m_frameProv->notCaptured(target, 8);
                    target = m_frameProv->readU64(target);
                }
                if (target != 0 && !missing) {
                    Node shown;
                    shown.kind = n.elementKind;
                    shown.strLen = n.strLen;
                    missing = m_frameProv->notCaptured(target, qMax(1, shown.byteSize()));
                }
            }
            if (!missing) continue;
            lm.notCaptured = true;
            // Never a plausible-looking zero: the value reads "??" at the same
            // width, so no column moves and nobody mistakes it for data.
            if (i >= starts.size()) continue;
            const int start = starts[i];
            const int end = (i + 1 < starts.size()) ? starts[i + 1] - 1 : text.size();
            const ColumnSpan vs = valueSpanFor(lm, end - start, lm.effectiveTypeW, lm.effectiveNameW);
            if (!vs.valid) continue;
            for (int col = vs.start; col < vs.end && start + col < end; ++col)
                if (!text.at(start + col).isSpace()) text[start + col] = QLatin1Char('?');
        }
    }

    // In the past, "changed" means changed AT the record being shown.
    QVector<ChangedRun> pastRuns;
    if (past)
        for (const auto& s : m_frameProv->frame()->changedAt) pastRuns.append({s.addr, s.len});
    const QVector<ChangedRun>& changedRuns = past ? pastRuns : m_changedRuns;
    if (!changedRuns.isEmpty()) {
        // Build childMap once for structSpan lookups (avoids O(N) cache rebuilds per call)
        QHash<uint64_t, QVector<int>> childMap;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            childMap[m_doc->tree.nodes[i].parentId].append(i);

        for (auto& lm : m_lastResult.meta) {
            if (lm.nodeIdx < 0 || lm.nodeIdx >= m_doc->tree.nodes.size()) continue;
            const Node& node = m_doc->tree.nodes[lm.nodeIdx];

            if (isHexPreview(node.kind)) {
                // Per-byte tracking for hex preview nodes
                for (int b = 0; b < lm.lineByteCount; b++) {
                    if (runsOverlap(changedRuns,lm.offsetAddr + (uint64_t)b, 1)) {
                        lm.changedByteIndices.append(b);
                        lm.dataChanged = true;
                    }
                }
            } else {
                // Use structSpan for containers (byteSize returns 0 for Array-of-Struct)
                int sz = (node.kind == NodeKind::Struct || node.kind == NodeKind::Array)
                    ? m_doc->tree.structSpan(node.id, &childMap) : node.byteSize();
                if (sz > 0 && runsOverlap(changedRuns,lm.offsetAddr, (uint64_t)sz))
                    lm.dataChanged = true;
            }
        }
    }

    // The past lights exactly what changed at the record shown, at full heat,
    // so stepping change to change always shows what just moved.
    if (past)
        for (auto& lm : m_lastResult.meta)
            if (lm.dataChanged) lm.heatLevel = 3;

    // Update value history and compute heat levels
    // Only run when a live provider is attached (not for static file/buffer sources)
    {
        const Provider* prov = nullptr;
        if (m_snapshotProv && m_snapshotProv->isLive())
            prov = m_snapshotProv.get();
        else if (this->provider() && this->provider()->isValid() && this->provider()->isLive())
            prov = this->provider().get();

        // Never while showing the past: scrubbing would record history's
        // values as fresh changes.
        if (!past && m_valueTrackCooldown > 0) --m_valueTrackCooldown;
        if (!past && m_trackValues && prov && m_valueTrackCooldown <= 0) {
            for (auto& lm : m_lastResult.meta) {
                if (lm.nodeIdx < 0 || lm.nodeIdx >= m_doc->tree.nodes.size()) continue;
                if (isSyntheticLine(lm)) continue;
                // A field's line, or one of the extra rows of a multi-line
                // value (a matrix's rows 1–3): each row has its own history,
                // or only row 0 ever lit.
                const bool extraRow = lm.lineKind == LineKind::Continuation && lm.subLine > 0;
                if (lm.lineKind != LineKind::Field && !extraRow) continue;
                // Member rows (enum, bitfield, code) have no value of their own.
                if (lm.isMemberLine) continue;

                const Node& node = m_doc->tree.nodes[lm.nodeIdx];
                // Skip containers — they don't have scalar values
                if (node.kind == NodeKind::Struct || node.kind == NodeKind::Array) continue;
                // Skip FuncPtr nodes — vtable entries don't change; tracking them
                // causes false heatmap and popup fighting with the disasm popup.
                if (isFuncPtr(node.kind)) continue;

                const uint64_t key = valueHistoryKey(lm);

                // Use the absolute address from compose (correct for pointer-expanded nodes)
                uint64_t addr = lm.offsetAddr;
                int sz = node.byteSize();
                if (sz <= 0 || !prov->isReadable(addr, sz)) continue;

                QString val = fmt::readValue(node, *prov, addr, lm.subLine);
                // An enum-typed field's history reads in member names.
                if (const Node* def = enumTypeOf(m_doc->tree, node)) {
                    const QString member = fmt::enumMemberName(*def, fmt::readEnumRaw(*prov, node.kind, addr));
                    if (!member.isEmpty()) val = member;
                }
                if (!val.isEmpty()) {
                    // Clear stale history if this node's effective address changed
                    // (e.g. viewRoot switch, pointer expand/collapse, MCP restructure)
                    auto addrIt = m_lastValueAddr.find(key);
                    if (addrIt != m_lastValueAddr.end() && addrIt.value() != addr) {
                        m_valueHistory.remove(key);
                        m_lastValueBytes.remove(key);
                    }
                    m_lastValueAddr[key] = addr;

                    // Change-detection keys on the underlying RAW BYTES, not the
                    // formatted display string. Reformatting identical bytes —
                    // Hex64 "0x0" -> Pointer64 "nullptr", an endianness/RVA flag
                    // toggle, etc. — must NOT count as a value change. Keying on
                    // the string lit up the heatmap and fired the previous-values
                    // popup on those no-op reformats (user: "nullptr and 0 are the
                    // same value underneath, it's annoying").
                    //
                    // Exception: a primitive pointer that DEREFERENCES its target
                    // displays "-> <value>", so its meaningful value lives at
                    // *ptr, not in the pointer's own bytes. For those we keep
                    // string-based detection so a target-memory change still
                    // registers even when the pointer itself is fixed.
                    //
                    // This must mirror readValueImpl's deref condition EXACTLY
                    // (format.cpp Pointer64 case): only a *non-null Pointer64*
                    // with ptrDepth>0 + a valid primitive target dereferences.
                    // Pointer32 NEVER dereferences (always shows the address), and
                    // a null pointer shows "nullptr" (no deref) — both belong on
                    // the byte path. Including Pointer32 / null here would wrongly
                    // route them to string-detection and re-expose the very
                    // format-only-firing bug this guard fixes.
                    bool derefsTarget = false;
                    if (node.kind == NodeKind::Pointer64
                        && node.ptrDepth > 0 && node.refId == 0
                        && isValidPrimitivePtrTarget(node.elementKind)) {
                        derefsTarget = (prov->readU64(addr) != 0);
                    }

                    bool shouldRecord;
                    if (derefsTarget) {
                        // record()'s internal string dedup decides.
                        shouldRecord = true;
                    } else {
                        // A matrix row's value is its own four components (every
                        // row line carries the matrix's base address): another
                        // row changing must not count, even though it can change
                        // the width this row is padded to.
                        const bool matrixRow = node.kind == NodeKind::Mat4x4
                                            && lm.subLine >= 0 && lm.subLine < 4;
                        QByteArray rawBytes = matrixRow
                            ? prov->readBytes(addr + uint64_t(lm.subLine) * 16, 16)
                            : prov->readBytes(addr, sz);
                        auto bytesIt = m_lastValueBytes.find(key);
                        shouldRecord = (bytesIt == m_lastValueBytes.end()
                                        || bytesIt.value() != rawBytes);
                        if (shouldRecord)
                            m_lastValueBytes[key] = rawBytes;
                    }
                    // Don't record the user's OWN edit. If this node overlaps a
                    // range the user just wrote, the baseline was still refreshed
                    // above (byte branch) so it won't read as an external change
                    // on later ticks — but we skip adding a history entry now.
                    bool userWrote = false;
                    for (const auto& r : m_userEditRanges) {
                        if (addr < r.second && r.first < addr + (uint64_t)sz) {
                            userWrote = true;
                            break;
                        }
                    }
                    if (shouldRecord && !userWrote)
                        m_valueHistory[key].record(val);
                    lm.heatLevel = m_valueHistory[key].heatLevel();
                }
            }
        }
        // Consume the one-shot user-edit ranges so only the refresh right after
        // a user write suppresses history; later ticks see an empty list.
        m_userEditRanges.clear();
    }

    // Prune stale selections (nodes removed by undo/redo/delete)
    QSet<uint64_t> valid;
    for (uint64_t id : m_selIds) {
        uint64_t nodeId = baseNodeIdFromSelId(id);
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
    // In the past BOTH are the frame: a hover preview must not read today's
    // memory and present it as the past.
    const Provider* snapProv = past ? static_cast<const Provider*>(m_frameProv.get())
        : m_snapshotProv
        ? static_cast<const Provider*>(m_snapshotProv.get())
        : (this->provider() ? this->provider().get() : nullptr);
    const Provider* realProv = past ? static_cast<const Provider*>(m_frameProv.get())
        : this->provider() ? this->provider().get() : nullptr;

    {
        PROFILE_SCOPE("refresh.applyToEditors");
        for (auto* editor : m_editors) {
            editor->setCustomTypeNames(customTypes);
            editor->setValueHistoryRef(past ? nullptr : &m_valueHistory);
            editor->setProviderRef(snapProv, realProv, &viewTree());
            ViewState vs = editor->saveViewState();
            editor->applyDocument(m_lastResult);
            editor->restoreViewState(vs);
        }
    }
    // Text-modifying passes first (command row replaces line 0 text),
    // then overlays last so hover indicators survive the refresh.
    {
        PROFILE_SCOPE("refresh.tail");
        // Keep the grey row band (m_selIds) locked to any active byte
        // selection. The byte selection is address-based and owns the row
        // selection while active, but the prune above (and other m_selIds
        // mutations) can leave m_selIds out of sync with the freshly-composed
        // covered rows, and the editor's emit dedup (m_lastByteRows) can
        // suppress the resync. Pull the authoritative covered set straight
        // from the editor that holds the selection so the grey band always
        // matches the purple byte highlight on load/edit/undo/redo. Done
        // before updateCommandRow() so its selectionChanged emit carries the
        // corrected count too. Guarded on hasByteSelection() so non-byte
        // selection paths (clicks, type-change-clears-selection) are untouched.
        for (auto* editor : m_editors) {
            if (editor->hasByteSelection()) {
                m_selIds = editor->byteCoveredRows();
                m_anchorLine = -1;
                break;
            }
        }
        updateCommandRow();
        pushAddressBarState();
        applySelectionOverlays();
    }
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

    // Asm takes its footprint from arrayLen, like a string takes it from
    // strLen. Converting TO it is size-preserving: the window becomes exactly
    // the bytes the node already occupied, so nothing after it moves and no
    // padding is invented. (byteSize() would otherwise read a stale arrayLen
    // from whatever the node used to be — usually 0.)
    const bool toAsm = (newKind == NodeKind::Asm && node.kind != NodeKind::Asm);
    const int asmSpan = toAsm ? qMax(1, oldSize) : 0;
    if (toAsm) newSize = asmSpan;

    if (newSize > 0 && newSize < oldSize) {
        // Shrinking: insert hex padding to fill gap (no offset shift)
        int gap = oldSize - newSize;
        uint64_t parentId = node.parentId;
        int baseOffset = node.offset + newSize;

        bool wasSuppressed = m_suppressRefresh;
        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Change type"));

        // Capture the rename condition BEFORE the ChangeKind push — push()
        // synchronously executes the command (via RcxCommand::redo) which
        // mutates node.kind in place. Reading node.kind after the push
        // returns the NEW kind, so the same-row check on isHexNode(node.kind)
        // would always be false and the Rename never fired. That's why the
        // first int32 in a hex64 → int32×2 split ended up with an empty
        // name while the second (created on the same-size path below)
        // correctly got "field_<offset>".
        const QString origName = node.name;
        const int     origOffset = node.offset;
        const bool    needsRename = isHexNode(node.kind) && !isHexNode(newKind);

        // Push type change with no offset adjustments
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeKind{node.id, node.kind, newKind, {}}));

        // Hex nodes don't display names (ASCII preview instead), so the stored
        // name may be empty or stale.  Give it a sensible default.
        if (needsRename) {
            QString autoName = QStringLiteral("field_%1")
                .arg(origOffset, 4, 16, QChar('0'));
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::Rename{node.id, origName, autoName}));
        }

        // Insert hex nodes to fill the gap.
        // When shrinking between hex sizes (hex→smaller hex), use same-size
        // padding so the cycle is reversible (join consumes exact pairs).
        // For non-hex conversions, use largest-first for compactness.
        bool hexToHex = isHexNode(node.kind) && isHexNode(newKind);
        int padOffset = baseOffset;
        while (gap > 0) {
            NodeKind padKind;
            int padSize;
            if (hexToHex) {
                padKind = newKind;
                padSize = newSize;
            } else if (gap >= 8) { padKind = NodeKind::Hex64; padSize = 8; }
            else if (gap >= 4)   { padKind = NodeKind::Hex32; padSize = 4; }
            else if (gap >= 2)   { padKind = NodeKind::Hex16; padSize = 2; }
            else                 { padKind = NodeKind::Hex8;  padSize = 1; }

            insertNode(parentId, padOffset, padKind,
                       QString("pad_%1").arg(padOffset, 2, 16, QChar('0')));
            padOffset += padSize;
            gap -= padSize;
        }

        m_doc->undoStack.endMacro();
        m_suppressRefresh = wasSuppressed;
        if (!m_suppressRefresh) {
            // A shrink splits the field into the new kind + a fresh hex pad.
            // Clear the selection: leaving the shrunk node selected made a
            // single click on its type cell immediately re-open the type
            // chooser (user: "auto-highlights the two emitted nodes / triggers
            // the typechooser too easily"). The pad is brand-new and shouldn't
            // be selected either; a deliberate click re-selects when wanted.
            // Suppressed callers (batch cycle/change) manage their own
            // selection restore, so only clear on the top-level path.
            m_selIds.clear();
            emit selectionChanged(0);
            refresh();   // recompose + applySelectionOverlays(empty)
        }
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
        // One macro when the change needs more than the kind itself: the
        // rename, and asm's window + expansion.
        const bool multi = needsRename || toAsm;
        if (multi) {
            m_doc->undoStack.beginMacro(QStringLiteral("Change type"));
        }
        const NodeKind oldKind = node.kind;
        const int oldArrayLen = node.arrayLen;
        const NodeKind oldElemKind = node.elementKind;
        const bool wasCollapsed = isCollapsed(node);
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeKind{node.id, oldKind, newKind, adjs}));
        if (toAsm) {
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::ChangeArrayMeta{node.id, oldElemKind, oldElemKind,
                                     oldArrayLen, asmSpan}));
            // collapsed defaults to true (it exists for containers), so a
            // fresh asm node would show a header and no instructions — the
            // opposite of why you made it.
            if (wasCollapsed)
                m_doc->undoStack.push(new RcxCommand(this,
                    cmd::Collapse{node.id, true, false}));
        }
        if (needsRename) {
            QString autoName = QStringLiteral("field_%1")
                .arg(node.offset, 4, 16, QChar('0'));
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::Rename{node.id, node.name, autoName}));
        }
        if (multi) m_doc->undoStack.endMacro();
    }
}

void RcxController::renameNode(int nodeIdx, const QString& newName) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    auto& node = m_doc->tree.nodes[nodeIdx];
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::Rename{node.id, node.name, newName}));
}

// Extract bytes [selLo, selHi) into a new root class and embed it in
// the original parent. Works against absolute addresses (the same units
// m_byteSel uses); converts to offsets via the parent struct's base.
//
// Fully-contained typed fields (Pointer/Int/Float/etc.) are *preserved*
// in the new class — only the hex-preview rows the user dragged across
// get repacked. Selection that partially overlaps a typed field is
// still refused since splitting a typed field's bytes makes no sense.
// Container siblings (Struct/Array) are refused on any intersection
// because cloning their child subtree isn't implemented.
void RcxController::extractByteSelectionToNewClass(uint64_t selLo, uint64_t selHi) {
    if (selHi <= selLo) {
        emit statusHint(QStringLiteral("No bytes selected"));
        return;
    }

    auto& tree = m_doc->tree;
    const uint64_t base = baseAddress();
    if (selLo < base) {
        emit statusHint(QStringLiteral("Selection starts before base address"));
        return;
    }
    const int relLo = static_cast<int>(selLo - base);
    const int relHi = static_cast<int>(selHi - base);

    // Restrict consideration to nodes whose ancestor chain leads to
    // the viewed root struct. Without this, OTHER root structs in the
    // tree (vtables, enums — anything at parentId == 0 that isn't the
    // viewed root) contribute their leaves to the intersection scan
    // because their offsets numerically overlap with the selection's
    // [relLo, relHi) range — even though they live in completely
    // different address spaces. The previous version picked up a
    // VTable's FuncPtr64 children for any selection on the tutorial
    // and falsely refused with "cross-parent".
    const uint64_t viewRoot = m_viewRootId;
    auto isInView = [&tree, viewRoot](uint64_t nodeId) -> bool {
        if (!viewRoot) return true;  // no constraint when no view root
        uint64_t id = nodeId;
        while (id != 0) {
            if (id == viewRoot) return true;
            int idx = tree.indexOfId(id);
            if (idx < 0) return false;
            id = tree.nodes[idx].parentId;
        }
        return false;
    };

    // Pass 1: determine the single parent struct from the first
    // intersected field. We skip only ROOT containers (parentId==0): a
    // root struct's span covers everything and would conflate "the root
    // intersects" with "a field intersects". But an EMBEDDED struct/array
    // FIELD (parentId!=0) is a real field we want to break off — including
    // it lets a selection covering only an embedded struct find its parent.
    // Pass 2 re-checks container-vs-selection intersection explicitly.
    uint64_t parentId = 0;
    bool parentSet = false;
    auto nodeSize = [&tree](const Node& n) {
        return (n.kind == NodeKind::Struct || n.kind == NodeKind::Array)
            ? tree.structSpan(n.id) : n.byteSize();
    };
    for (int i = 0; i < tree.nodes.size(); ++i) {
        const Node& n = tree.nodes[i];
        if ((n.kind == NodeKind::Struct || n.kind == NodeKind::Array)
            && n.parentId == 0)
            continue;
        if (!isInView(n.id)) continue;
        int sz = nodeSize(n);
        if (sz <= 0) continue;
        int rowLo = n.offset, rowHi = rowLo + sz;
        if (rowHi <= relLo || rowLo >= relHi) continue;
        if (!parentSet) {
            parentId = n.parentId;
            parentSet = true;
        } else if (n.parentId != parentId) {
            emit statusHint(QStringLiteral("Selection crosses parent struct boundary"));
            return;
        }
    }
    if (!parentSet) {
        emit statusHint(QStringLiteral("No fields in selection"));
        return;
    }

    // Pass 2: collect intersected siblings of `parentId`, validate.
    QVector<int> intersected;
    for (int ci : tree.childrenOf(parentId)) {
        const Node& sib = tree.nodes[ci];
        int sz = nodeSize(sib);
        if (sz <= 0) continue;
        int rowLo = sib.offset, rowHi = rowLo + sz;
        if (rowHi <= relLo || rowLo >= relHi) continue;
        const bool fullyContained = (rowLo >= relLo && rowHi <= relHi);
        // A fully-contained Struct/Array is moved into the new class intact —
        // it's just a refId/metadata connection, so the whole hierarchy comes
        // along for free (the referenced root class is untouched). Only a node
        // that STRADDLES the boundary is a real collision we can't resolve.
        if (sib.kind == NodeKind::Struct || sib.kind == NodeKind::Array) {
            if (!fullyContained) {
                emit statusHint(QStringLiteral("Selection crosses a Struct/Array boundary — refusing break"));
                return;
            }
        } else if (!fullyContained && !isHexPreview(sib.kind)) {
            emit statusHint(QStringLiteral("Selection partially crosses a typed field — refusing"));
            return;
        }
        intersected.append(ci);
    }
    if (intersected.isEmpty()) {
        emit statusHint(QStringLiteral("No fields in selection"));
        return;
    }

    // Sort by offset so affLo/affHi are well-defined.
    std::sort(intersected.begin(), intersected.end(),
              [&tree](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });
    const int affLo = tree.nodes[intersected.first()].offset;
    const int affHi = tree.nodes[intersected.last()].offset
                    + nodeSize(tree.nodes[intersected.last()]);
    const int leftPadBytes  = relLo - affLo;
    const int rightPadBytes = affHi - relHi;
    const int extractSize   = relHi - relLo;
    Q_ASSERT(leftPadBytes  >= 0);
    Q_ASSERT(rightPadBytes >= 0);
    Q_ASSERT(extractSize   >  0);

    // Snapshot each intersected sibling's data so we can rebuild typed
    // fields in the new class after removal.
    struct Snapshot {
        Node     node;             // full node copy
        int      sz;
        bool     fullyContained;
    };
    QVector<Snapshot> snaps;
    snaps.reserve(intersected.size());
    for (int idx : intersected) {
        Snapshot s;
        s.node = tree.nodes[idx];
        s.sz   = nodeSize(s.node);
        s.fullyContained = (s.node.offset >= relLo
                            && s.node.offset + s.sz <= relHi);
        snaps.append(s);
    }

    // Greedy hex packer — fills `nBytes` at consecutive offsets with
    // the largest hex kind that fits (Hex64 → Hex8).
    auto packGreedyHex = [this](uint64_t pid, int startOffset, int nBytes) {
        int off = startOffset;
        int rem = nBytes;
        while (rem > 0) {
            NodeKind k; int sz;
            if      (rem >= 8) { k = NodeKind::Hex64; sz = 8; }
            else if (rem >= 4) { k = NodeKind::Hex32; sz = 4; }
            else if (rem >= 2) { k = NodeKind::Hex16; sz = 2; }
            else               { k = NodeKind::Hex8;  sz = 1; }
            insertNode(pid, off,  k,
                       QString("field_%1").arg(off, 4, 16, QChar('0')));
            off += sz;
            rem -= sz;
        }
    };

    // One canonical naming scheme across every class-creation path.
    const QString typeName = uniqueStructName();

    bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Extract to New Class"));

    // 1. Create the new root struct.
    Node root;
    root.kind            = NodeKind::Struct;
    root.structTypeName  = typeName;
    root.name            = QStringLiteral("instance");
    root.classKeyword    = QStringLiteral("class");
    root.parentId        = 0;
    root.offset          = 0;
    root.id              = tree.reserveId();
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{root, {}}));

    // 2. Populate the new class. Fully-contained snaps are re-inserted
    //    preserving their kind / name / comment / refId at their
    //    selection-relative offset. Gaps between them get filled with
    //    greedy hex packing. (Names `slot` / `slots` collide with Qt's
    //    `slots` macro inside QObject headers, so use `placement` here.)
    {
        struct Placement { int newOff; int sz; const Snapshot* s; };
        QVector<Placement> placements;
        for (const auto& sn : snaps) {
            if (!sn.fullyContained) continue;
            Placement p;
            p.newOff = sn.node.offset - relLo;
            p.sz     = sn.sz;
            p.s      = &sn;
            placements.append(p);
        }
        std::sort(placements.begin(), placements.end(),
                  [](const Placement& a, const Placement& b) {
                      return a.newOff < b.newOff;
                  });

        int cursor = 0;
        for (const auto& p : placements) {
            if (p.newOff > cursor)
                packGreedyHex(root.id, cursor, p.newOff - cursor);
            Node clone = p.s->node;
            clone.parentId = root.id;
            clone.offset   = p.newOff;
            clone.id       = tree.reserveId();
            m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{clone, {}}));
            cursor = p.newOff + p.sz;
        }
        if (cursor < extractSize)
            packGreedyHex(root.id, cursor, extractSize - cursor);
    }

    // 3. Remove every intersected sibling from the original parent
    //    (no offset shifts — the equal-size replacements below balance).
    QVector<uint64_t> intersectedIds;
    intersectedIds.reserve(intersected.size());
    for (int idx : intersected)
        intersectedIds.append(tree.nodes[idx].id);
    for (int i = intersectedIds.size() - 1; i >= 0; --i) {
        int curIdx = tree.indexOfId(intersectedIds[i]);
        if (curIdx < 0) continue;
        Node copy = tree.nodes[curIdx];
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::Remove{copy.id, {copy}, {}}));
    }

    // 4. Insert left pads + embedded struct + right pads at the
    //    original offsets.
    packGreedyHex(parentId, affLo, leftPadBytes);

    Node embed;
    embed.kind           = NodeKind::Struct;
    embed.parentId       = parentId;
    embed.offset         = relLo;
    embed.structTypeName = typeName;
    // Field name mirrors the type name (lowercased) — same convention
    // as a C-style "ClassFoo classfoo;" variable. Reads naturally and
    // saves the user a rename when they accept the auto-generated name.
    embed.name           = typeName.toLower();
    embed.refId          = root.id;
    embed.id             = tree.reserveId();
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{embed, {}}));

    packGreedyHex(parentId, relHi, rightPadBytes);

    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();

    emit statusHint(QStringLiteral("Extracted %1 byte%2 into %3")
        .arg(extractSize).arg(extractSize == 1 ? "" : "s").arg(typeName));
}

// True if `nodeId`'s ancestor chain reaches the current view root — i.e. the
// node is a field of the viewed class, not one shown inside an embedded class
// (whose fields belong to a different definition). Mirrors the isInView guard
// in extractByteSelectionToNewClass. With no view root (0) there is no
// constraint, so everything is "in view".
bool RcxController::nodeInView(uint64_t nodeId) const {
    if (!m_viewRootId) return true;
    uint64_t id = nodeId;
    while (id != 0) {
        if (id == m_viewRootId) return true;
        int idx = m_doc->tree.indexOfId(id);
        if (idx < 0) return false;
        id = m_doc->tree.nodes[idx].parentId;
    }
    return false;
}

// True only when `nodeId` is a DIRECT field of the view frame. The break scan
// (extractByteSelectionToNewClass) compares each field's PARENT-relative
// n.offset against a root-relative selection; those frames coincide only for a
// direct child of the viewed class. A union member (createUnion reparents it to
// offset 0) or an inline-struct field passes nodeInView() — its chain reaches
// the root — yet sits in a nested frame, so its raw offset would resolve to the
// wrong region. Breaking those is the separate "break inside a nested struct"
// feature; refuse here rather than mangle the wrong field.
bool RcxController::isDirectViewFrameChild(uint64_t nodeId) const {
    const int idx = m_doc->tree.indexOfId(nodeId);
    if (idx < 0) return false;
    const uint64_t pid = m_doc->tree.nodes[idx].parentId;
    if (m_viewRootId) return pid == m_viewRootId;
    // No explicit view root: the frame is a top-level root class, so a valid
    // direct field's parent must itself be a root (parentId == 0).
    if (pid == 0) return false;  // a root container is not itself a field
    const int pidx = m_doc->tree.indexOfId(pid);
    return pidx >= 0 && m_doc->tree.nodes[pidx].parentId == 0;
}

std::optional<QPair<uint64_t, uint64_t>>
RcxController::regionFromCurrentSelection(RcxEditor* editor) const {
    auto& tree = m_doc->tree;

    // 1. An active byte selection wins (on the passed editor, or any split).
    if (editor && editor->hasByteSelection() && editor->byteSelectionByteCount() > 0)
        return editor->byteSelectionRange();
    for (auto* ed : m_editors)
        if (ed && ed->hasByteSelection() && ed->byteSelectionByteCount() > 0)
            return ed->byteSelectionRange();

    // 2. Otherwise union the offset spans of the selected nodes. selIds are
    //    encoded (footer/array-elem/member high bits) — decode to the base
    //    node id, same as the delete path.
    bool any = false;
    int minOff = 0, maxOff = 0;
    for (uint64_t sid : m_selIds) {
        uint64_t nid = baseNodeIdFromSelId(sid);
        int idx = tree.indexOfId(nid);
        if (idx < 0) continue;
        // The node-span region is built from PARENT-relative n.offset; that
        // frame coincides with the root-relative selection only for a DIRECT
        // field of the view frame. A union member (reparented to offset 0) or an
        // inline-struct field would resolve to the wrong bytes — the exact case
        // extractByteSelectionToNewClass mangles. The context-menu break paths
        // pre-check each node, but the menu-bar / Ctrl+Shift+B "Carve"
        // action calls this directly, so guard at the shared chokepoint: refuse
        // the whole region if any contributing node is nested. (Byte selections
        // returned above are root-relative and unaffected.)
        if (!isDirectViewFrameChild(nid)) return std::nullopt;
        const Node& n = tree.nodes[idx];
        int sz = (n.kind == NodeKind::Struct || n.kind == NodeKind::Array)
                 ? tree.structSpan(n.id) : n.byteSize();
        if (sz <= 0) continue;
        const int lo = n.offset, hi = n.offset + sz;
        if (!any)      { minOff = lo; maxOff = hi; any = true; }
        else           { minOff = qMin(minOff, lo); maxOff = qMax(maxOff, hi); }
    }
    if (!any || maxOff <= minOff) return std::nullopt;
    const uint64_t base = baseAddress();
    return QPair<uint64_t, uint64_t>(base + (uint64_t)minOff, base + (uint64_t)maxOff);
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
        cmd::Collapse{node.id, isCollapsed(node), !isCollapsed(node)}));
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

bool RcxController::writeSelectedBytesToFile(uint64_t addr, int n,
                                              const QString& path,
                                              QString* err) const {
    if (n <= 0) {
        if (err) *err = QStringLiteral("No bytes to save");
        return false;
    }
    const Provider* prov = displayedProvider();   // what is on screen: recorded values too
    if (!prov) {
        if (err) *err = QStringLiteral("No active provider");
        return false;
    }
    if (!prov->isReadable(addr, n)) {
        if (err) *err = QStringLiteral("Couldn't read %1 bytes at 0x%2")
            .arg(n).arg(addr, 0, 16);
        return false;
    }
    QByteArray data = prov->readBytes(addr, n);
    if (data.size() != n) {
        if (err) *err = QStringLiteral("Short read: got %1 bytes, wanted %2")
            .arg(data.size()).arg(n);
        return false;
    }
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QStringLiteral("Couldn't open %1 for writing: %2")
            .arg(path).arg(out.errorString());
        return false;
    }
    if (out.write(data) != data.size()) {
        if (err) *err = QStringLiteral("Write failed: %1").arg(out.errorString());
        return false;
    }
    return true;
}

bool RcxController::applyCommand(const Command& command, bool isUndo,
                                  const std::shared_ptr<Provider>& writeProvider) {
    auto& tree = m_doc->tree;
    bool success = true;
    // Every command that reaches here mutates tree state in some way (the
    // exceptions — WriteBytes / ChangeBase — bump generation too because a
    // value cache keyed off (tree gen, base) needs invalidation when base
    // changes). Bump once at entry; downstream caches read it via
    // tree.generation() to decide whether to re-render.
    tree.touch();

    // Clear value history for nodes whose effective offset changed.
    // When offsets shift (insert/delete/resize), old recorded values came from
    // a different memory address, so keeping them would show false heat.
    // Also invalidates any in-flight async read so that stale snapshot data
    // from before the offset change doesn't re-introduce false heat.
    auto clearNodeHistory = [&](uint64_t id) {
        // The node's own history and each extra row's (valueHistoryKey).
        for (int row = 0; row < 8; ++row) {
            const uint64_t key = row == 0 ? id : makeMemberSelId(id, row);
            m_valueHistory.remove(key);
            m_lastValueAddr.remove(key);
            m_lastValueBytes.remove(key);
        }
    };

    auto clearHistoryForAdjs = [&](const QVector<cmd::OffsetAdj>& adjs) {
        if (adjs.isEmpty()) return;
        m_refreshGen++;  // discard in-flight async read (stale layout)
        // Build childMap once for all subtree lookups (avoids O(N²) rebuilds)
        QHash<uint64_t, QVector<int>> adjChildMap;
        bool hasContainers = false;
        for (const auto& adj : adjs) {
            clearNodeHistory(adj.nodeId);
            int ai = tree.indexOfId(adj.nodeId);
            if (ai >= 0 && (tree.nodes[ai].kind == NodeKind::Struct
                         || tree.nodes[ai].kind == NodeKind::Array))
                hasContainers = true;
        }
        // Only build childMap if any adjusted nodes are containers with children
        if (hasContainers) {
            for (int i = 0; i < tree.nodes.size(); i++)
                adjChildMap[tree.nodes[i].parentId].append(i);
        }
        for (const auto& adj : adjs) {
            int ai = tree.indexOfId(adj.nodeId);
            if (ai < 0) continue;
            const Node& n = tree.nodes[ai];
            if (n.kind != NodeKind::Struct && n.kind != NodeKind::Array)
                continue;  // leaf node — already cleared above, no descendants
            // Clear all descendants
            QVector<uint64_t> stack;
            QSet<uint64_t> visited;
            stack.append(adj.nodeId);
            visited.insert(adj.nodeId);
            while (!stack.isEmpty()) {
                uint64_t pid = stack.takeLast();
                for (int ci : adjChildMap.value(pid)) {
                    uint64_t cid = tree.nodes[ci].id;
                    if (!visited.contains(cid)) {
                        visited.insert(cid);
                        stack.append(cid);
                        clearNodeHistory(cid);
                    }
                }
            }
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
            // Bump refresh-gen to discard any in-flight async read that
            // would record the OLD-format value into the NEW node.
            m_refreshGen++;
            // Re-baseline the node's OWN value history on a kind change.
            // A type change is not a memory change, so it must not produce
            // heat or fire the previous-values popup on static data. Keeping
            // the old history was actively wrong here: when the new kind has a
            // different byte size (e.g. Hex64 -> Int32 from an int32x2 split,
            // 8 bytes -> 4), the raw-byte change-detector compares the stale
            // 8-byte sample against the new 4-byte read, always mismatches, and
            // records a spurious "change" — lighting the heatmap on a buffer
            // that never moved. Even same-size reformats (Hex64 "0x0" ->
            // Pointer64 "nullptr") would add a second distinct string entry.
            // Clearing both history + byte cache makes the field re-baseline to
            // a single value (heat 0); real subsequent byte changes re-arm heat.
            clearNodeHistory(c.nodeId);
            clearHistoryForAdjs(c.offAdjs);
        } else if constexpr (std::is_same_v<T, cmd::Rename>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].name = isUndo ? c.oldName : c.newName;
        } else if constexpr (std::is_same_v<T, cmd::Collapse>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                setCollapsed(c.nodeId, isUndo ? c.oldState : c.newState);
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
            baseAddress() = isUndo ? c.oldBase : c.newBase;
            baseAddressFormula() = isUndo ? c.oldFormula : c.newFormula;
            if (!baseAddressFormula().isEmpty()) {
                const auto callbacks = makeAddressCallbacks(provider().get(), tree.pointerSize);
                const auto result = AddressParser::evaluate(baseAddressFormula(), tree.pointerSize, &callbacks);
                if (result.ok) baseAddress() = result.value;
            }
            resetSnapshot();
        } else if constexpr (std::is_same_v<T, cmd::WriteBytes>) {
            const QByteArray& bytes = isUndo ? c.oldBytes : c.newBytes;
            // Tutorial / self-attach safety — refuse the byte write
            // even on undo/redo so a previously-queued write can't
            // execute later and stomp the editor's own memory.
            if (m_readOnlyOverride) {
                success = false;
                return;  // exits the std::visit lambda for this command
            }
            // Write through snapshot (patches pages only on success) or provider directly.
            // If write fails, the snapshot is NOT patched, so the next compose shows the
            // real unchanged value — no optimistic visual leak.
            const auto target = writeProvider ? writeProvider : this->provider();
            bool ok = target && target != this->provider()
                ? target->writeBytes(c.addr, bytes)
                : m_snapshotProv
                ? m_snapshotProv->write(c.addr, bytes.constData(), bytes.size())
                : target && target->writeBytes(c.addr, bytes);
            if (!ok) {
                qWarning() << "WriteBytes failed at address" << QString::number(c.addr, 16);
                // Signal failure so RcxCommand::redo/undo can call setObsolete(true)
                // and drop this entry from the undo stack. Otherwise a later undo
                // would write c.oldBytes over the current (still-original) memory.
                emit statusHint(QStringLiteral("Write rejected at 0x%1 — removing from history")
                                 .arg(c.addr, 0, 16));
                success = false;
            }
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
                    setCollapsed(tree.nodes[idx].id, true);
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
        } else if constexpr (std::is_same_v<T, cmd::ToggleBigEndian>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].bigEndian = isUndo ? c.oldVal : c.newVal;
        } else if constexpr (std::is_same_v<T, cmd::ToggleRelative>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].isRelative = isUndo ? c.oldVal : c.newVal;
        } else if constexpr (std::is_same_v<T, cmd::ChangeComment>) {
            int idx = tree.indexOfId(c.nodeId);
            if (idx >= 0)
                tree.nodes[idx].comment = isUndo ? c.oldComment : c.newComment;
        }
    }, command);

    // Only refresh when the op actually took effect. On WriteBytes failure
    // we skip the refresh so the UI keeps whatever the most recent
    // successful state was — a refresh here would just recompose the same
    // unchanged memory.
    if (success && !m_suppressRefresh)
        refresh();
    return success;
}

void RcxController::setNodeValue(int nodeIdx, int subLine, const QString& text,
                                  bool isAscii, uint64_t resolvedAddr) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    if (!this->provider()->isWritable()) return;
    // Tutorial / self-attach safety: writing into the editor's own
    // memory through a fully-writable provider is fatal (e.g. stomping
    // the __vptr value crashes the next virtual dispatch on this
    // RcxEditor instance). The override is set by MainWindow::selfTest.
    if (isViewingPast()) {
        emit statusHint(viewingPastHint());
        return;
    }
    if (m_readOnlyOverride) {
        // Silent no-op — UI didn't have a status channel handy and a
        // dialog would interrupt the tutorial flow. Edit commits but
        // doesn't reach the provider; refresh shows the original bytes.
        return;
    }

    const Node& node = m_doc->tree.nodes[nodeIdx];

    // Use the compose-resolved address when available (correct for pointer children).
    // Fall back to baseAddress() + computeOffset for callers that don't supply it.
    uint64_t addr;
    if (resolvedAddr != 0) {
        addr = resolvedAddr;
    } else {
        int64_t signedAddr = m_doc->tree.computeOffset(nodeIdx);
        if (signedAddr < 0) return;  // malformed tree: negative offset
        addr = baseAddress() + static_cast<uint64_t>(signedAddr);
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
        // Pass a temporary node carrying the effective kind + bigEndian so the
        // parser applies the per-node endian swap (Vec/Mat components inherit
        // the parent node's endianness by using its bigEndian flag).
        Node editNode = node;
        editNode.kind = editKind;
        newBytes = fmt::parseValue(editNode, text, &ok);
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
    if (!this->provider()->isReadable(addr, writeSize)) return;

    // Read old bytes before writing (for undo)
    QByteArray oldBytes = this->provider()->readBytes(addr, writeSize);

    // Test the write first — don't push a command that will silently fail.
    // This prevents optimistic visual updates for read-only providers.
    bool writeOk = m_snapshotProv
        ? m_snapshotProv->write(addr, newBytes.constData(), newBytes.size())
        : this->provider()->writeBytes(addr, newBytes);
    if (!writeOk) {
        qWarning() << "Write failed at address" << QString::number(addr, 16);
        refresh();  // refresh to show the real unchanged value
        return;
    }

    // Mark this as a user edit so the refresh triggered by the push below
    // doesn't record it into the node's value history (user edits aren't
    // "observed changes").
    m_userEditRanges.append({addr, addr + (uint64_t)writeSize});

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

QString RcxController::uniqueStructName(const QString& base) const {
    QSet<QString> existing;
    for (const auto& n : m_doc->tree.nodes)
        if (n.kind == NodeKind::Struct && !n.structTypeName.isEmpty())
            existing.insert(n.structTypeName);
    QString seed = base.isEmpty() ? QStringLiteral("NewClass") : base;
    if (!existing.contains(seed)) return seed;
    for (int suffix = 2; ; ++suffix) {
        QString candidate = QStringLiteral("%1_%2").arg(seed).arg(suffix);
        if (!existing.contains(candidate)) return candidate;
    }
}

uint64_t RcxController::createRootStruct(const QString& typeName,
                                         const QString& keyword, int fieldCount) {
    Node rootStruct;
    rootStruct.kind = NodeKind::Struct;
    rootStruct.name = QStringLiteral("instance");
    rootStruct.structTypeName = typeName;
    rootStruct.classKeyword = keyword;
    rootStruct.parentId = 0;
    rootStruct.offset = 0;
    rootStruct.id = m_doc->tree.reserveId();
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{rootStruct, {}}));

    const bool is32 = (m_doc->tree.pointerSize < 8);
    const NodeKind hexKind = is32 ? NodeKind::Hex32 : NodeKind::Hex64;
    const int stride = is32 ? 4 : 8;
    for (int i = 0; i < fieldCount; i++) {
        Node c;
        c.kind = hexKind;
        c.name = QStringLiteral("field_%1").arg(i * stride, 2, 16, QChar('0'));
        c.parentId = rootStruct.id;
        c.offset = i * stride;
        c.id = m_doc->tree.reserveId();
        m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{c, {}}));
    }
    return rootStruct.id;
}

void RcxController::convertToTypedPointer(uint64_t nodeId) {
    int ni = m_doc->tree.indexOfId(nodeId);
    if (ni < 0) return;
    const uint64_t oldRefId = m_doc->tree.nodes[ni].refId;
    const NodeKind ptrKind = (m_doc->tree.pointerSize >= 8)
        ? NodeKind::Pointer64 : NodeKind::Pointer32;

    // Nest-safe: convertSelectionToTypedPointers wraps several of these in
    // one outer macro, so restore the caller's suppress flag and only
    // refresh on the top-level path.
    const bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Change to ptr*"));
    if (m_doc->tree.nodes[ni].kind != ptrKind)
        changeNodeKind(ni, ptrKind);
    uint64_t newId = createRootStruct(uniqueStructName(), QStringLiteral("class"), 16);
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::ChangePointerRef{nodeId, oldRefId, newId}));
    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();
}

uint64_t RcxController::attachRttiClassToPointer(uint64_t nodeId,
                                                  const QString& baseName) {
    // Mirrors convertToTypedPointer but takes a user-provided base name
    // (the demangled RTTI class) instead of the synthetic "NewClass".
    // Per design: ALWAYS create a fresh class — even when a class with
    // the same name already exists, we suffix _2 / _3 / etc. so each
    // RTTI chip click yields a distinct, independently-editable struct.
    int ni = m_doc->tree.indexOfId(nodeId);
    if (ni < 0) return 0;
    const uint64_t oldRefId = m_doc->tree.nodes[ni].refId;
    const NodeKind ptrKind = (m_doc->tree.pointerSize >= 8)
        ? NodeKind::Pointer64 : NodeKind::Pointer32;

    const QString typeName = uniqueStructName(baseName);  // RTTI name (or NewClass)

    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(
        QStringLiteral("Attach RTTI class %1").arg(typeName));
    if (m_doc->tree.nodes[ni].kind != ptrKind)
        changeNodeKind(ni, ptrKind);
    uint64_t newId = createRootStruct(typeName, QStringLiteral("class"), 16);
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::ChangePointerRef{nodeId, oldRefId, newId}));
    m_doc->undoStack.endMacro();
    m_suppressRefresh = false;
    refresh();
    return newId;
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

//TODO-DELETE(RcxController::showHexToolbar) void RcxController::showHexToolbar(RcxEditor* editor, int nodeIdx) {
//    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
//    const auto& node = m_doc->tree.nodes[nodeIdx];
//    if (!isHexNode(node.kind)) return;
//
//    if (!m_hexToolbar) {
//        m_hexToolbar = new HexToolbarPopup(editor);
//        connect(m_hexToolbar, &HexToolbarPopup::sizeSelected,
//                this, [this](uint64_t nid, NodeKind newKind) {
//            int ni = m_doc->tree.indexOfId(nid);
//            if (ni < 0) return;
//            const auto& n = m_doc->tree.nodes[ni];
//            if (isHexNode(newKind)) {
//                if (sizeForKind(newKind) <= sizeForKind(n.kind))
//                    changeNodeKind(ni, newKind);
//                else
//                    joinHexNodes(nid, newKind);
//            } else {
//                changeNodeKind(ni, newKind);  // smart suggestion (ptr/float/utf8)
//            }
//        });
//        connect(m_hexToolbar, &HexToolbarPopup::insertAbove,
//                this, [this](uint64_t nid) {
//            int ni = m_doc->tree.indexOfId(nid);
//            if (ni >= 0) insertNodeAbove(ni, NodeKind::Hex64, QStringLiteral("field"));
//        });
//        connect(m_hexToolbar, &HexToolbarPopup::insertBelow,
//                this, [this](uint64_t nid) {
//            int ni = m_doc->tree.indexOfId(nid);
//            if (ni < 0) return;
//            const auto& n = m_doc->tree.nodes[ni];
//            insertNode(n.parentId, n.offset + sizeForKind(n.kind),
//                       NodeKind::Hex64, QStringLiteral("field"));
//        });
//        connect(m_hexToolbar, &HexToolbarPopup::joinSelected,
//                this, [this]() {
//            if (m_selIds.size() < 2) return;
//            // Find first selected hex node
//            uint64_t firstId = 0;
//            int totalBytes = 0;
//            for (uint64_t sid : m_selIds) {
//                int ni = m_doc->tree.indexOfId(sid);
//                if (ni < 0 || !isHexNode(m_doc->tree.nodes[ni].kind)) continue;
//                if (firstId == 0 || m_doc->tree.nodes[ni].offset < m_doc->tree.nodes[m_doc->tree.indexOfId(firstId)].offset)
//                    firstId = sid;
//                totalBytes += sizeForKind(m_doc->tree.nodes[ni].kind);
//            }
//            if (!firstId || totalBytes < 2) return;
//            NodeKind target = NodeKind::Hex8;
//            if      (totalBytes >= 16) target = NodeKind::Hex128;
//            else if (totalBytes >= 8)  target = NodeKind::Hex64;
//            else if (totalBytes >= 4)  target = NodeKind::Hex32;
//            else if (totalBytes >= 2)  target = NodeKind::Hex16;
//            joinHexNodes(firstId, target);
//        });
//        connect(m_hexToolbar, &HexToolbarPopup::fillToOffset,
//                this, [this](uint64_t nid, int targetOffset) {
//            int ni = m_doc->tree.indexOfId(nid);
//            if (ni < 0) return;
//            const auto& n = m_doc->tree.nodes[ni];
//            int curEnd = n.offset + sizeForKind(n.kind);
//            int gap = targetOffset - curEnd;
//            if (gap <= 0) return;
//            m_suppressRefresh = true;
//            m_doc->undoStack.beginMacro(QStringLiteral("Fill to offset 0x%1").arg(targetOffset, 0, 16));
//            int padOff = curEnd;
//            while (gap > 0) {
//                NodeKind pk; int ps;
//                if      (gap >= 16) { pk = NodeKind::Hex128; ps = 16; }
//                else if (gap >= 8)  { pk = NodeKind::Hex64;  ps = 8; }
//                else if (gap >= 4)  { pk = NodeKind::Hex32;  ps = 4; }
//                else if (gap >= 2)  { pk = NodeKind::Hex16;  ps = 2; }
//                else                { pk = NodeKind::Hex8;   ps = 1; }
//                Node pad;
//                pad.kind = pk;
//                pad.name = QStringLiteral("pad_%1").arg(padOff, 0, 16);
//                pad.parentId = n.parentId;
//                pad.offset = padOff;
//                pad.id = m_doc->tree.reserveId();
//                m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{pad, {}}));
//                padOff += ps;
//                gap -= ps;
//            }
//            m_doc->undoStack.endMacro();
//            m_suppressRefresh = false;
//            refresh();
//        });
//    }
//
//    // Build context
//    HexPopupContext ctx;
//    ctx.nodeId = node.id;
//    ctx.currentKind = node.kind;
//    int curSz = sizeForKind(node.kind);
//    bool addrOk = true;
//    uint64_t addr = m_doc->tree.absoluteAddress(nodeIdx, &addrOk);
//    ctx.data = (addrOk && this->provider())
//        ? this->provider()->readBytes(addr, curSz)
//        : QByteArray(curSz, '\0');
//
//    // Collect adjacent same-parent hex nodes
//    uint64_t parentId = node.parentId;
//    int nextOff = node.offset + curSz;
//    for (int i = nodeIdx + 1; i < m_doc->tree.nodes.size() && ctx.nexts.size() < 15; i++) {
//        const auto& sib = m_doc->tree.nodes[i];
//        if (sib.parentId != parentId) break;
//        if (sib.offset != nextOff) break;
//        if (!isHexNode(sib.kind)) break;
//        HexPopupContext::Adjacent adj;
//        adj.exists = true;
//        adj.kind = sib.kind;
//        int sibSz = sizeForKind(sib.kind);
//        bool sibOk = true;
//        uint64_t sibAddr = m_doc->tree.absoluteAddress(i, &sibOk);
//        adj.data = (sibOk && this->provider())
//            ? this->provider()->readBytes(sibAddr, sibSz)
//            : QByteArray(sibSz, '\0');
//        ctx.nexts.append(adj);
//        nextOff += sibSz;
//    }
//
//    // Smart suggestions (only when pinned — avoids overhead on every selection)
//    if (m_hexToolbar->isPinned() && this->provider()) {
//        // Pointer check: interpret bytes as uint64, check if readable address
//        if (curSz >= 8) {
//            uint64_t ptrVal = 0;
//            memcpy(&ptrVal, ctx.data.constData(), qMin(curSz, 8));
//            if (ptrVal > 0x10000 && this->provider()->isReadable(ptrVal, 1)) {
//                ctx.hasPtr = true;
//                ctx.ptrSymbol = this->provider()->getSymbol(ptrVal);
//            }
//        } else if (curSz == 4) {
//            uint32_t ptrVal = 0;
//            memcpy(&ptrVal, ctx.data.constData(), 4);
//            if (ptrVal > 0x10000 && this->provider()->isReadable(ptrVal, 1)) {
//                ctx.hasPtr = true;
//                ctx.ptrSymbol = this->provider()->getSymbol(ptrVal);
//            }
//        }
//        // Float check
//        if (curSz >= 4) {
//            float fv = 0;
//            memcpy(&fv, ctx.data.constData(), 4);
//            if (std::isfinite(fv) && std::fabs(fv) < 1e6f && fv != 0.0f
//                && std::fabs(fv) > 1e-6f) {
//                ctx.hasFloat = true;
//                ctx.floatVal = fv;
//            }
//        }
//        // String check: count leading printable ASCII bytes
//        {
//            int printable = 0;
//            for (int i = 0; i < ctx.data.size(); i++) {
//                uint8_t c = (uint8_t)ctx.data[i];
//                if (c >= 0x20 && c <= 0x7E) printable++;
//                else break;
//            }
//            if (printable >= 4) {
//                ctx.hasString = true;
//                ctx.stringPreview = QString::fromLatin1(ctx.data.constData(), printable);
//            }
//        }
//    }
//
//    // Multi-select info
//    if (m_selIds.size() > 1) {
//        // m_selIds is a QSet — collect the selected node indices and sort by
//        // offset before the contiguity scan. Iterating in hash order would
//        // make the "each node's offset == previous node's end" adjacency test
//        // spuriously fail on a genuinely contiguous selection, which wrongly
//        // disables the "merge into one typed field" affordance (hextoolbar).
//        QVector<int> sel;
//        for (uint64_t sid : m_selIds) {
//            int si = m_doc->tree.indexOfId(sid);
//            if (si >= 0) sel.append(si);
//        }
//        std::sort(sel.begin(), sel.end(), [this](int a, int b) {
//            return m_doc->tree.nodes[a].offset < m_doc->tree.nodes[b].offset;
//        });
//        int count = 0, bytes = 0;
//        bool contiguous = true;
//        NodeKind commonKind = NodeKind::Hex8;
//        int lastOff = -1;
//        uint64_t commonParent = 0;
//        for (int si : sel) {
//            if (!isHexNode(m_doc->tree.nodes[si].kind)) { contiguous = false; continue; }
//            const auto& sn = m_doc->tree.nodes[si];
//            if (count == 0) { commonKind = sn.kind; commonParent = sn.parentId; }
//            else {
//                if (sn.kind != commonKind || sn.parentId != commonParent) contiguous = false;
//                if (lastOff >= 0 && sn.offset != lastOff) contiguous = false;
//            }
//            count++;
//            bytes += sizeForKind(sn.kind);
//            lastOff = sn.offset + sizeForKind(sn.kind);
//        }
//        ctx.multiSelectCount = count;
//        ctx.multiSelectBytes = bytes;
//        ctx.multiSelectContiguous = contiguous;
//        ctx.multiSelectKind = commonKind;
//    }
//
//    m_hexToolbar->setFont(editor->scintilla()->font());
//    m_hexToolbar->setContext(ctx);
//
//    // Position below the selected line, left-aligned to the type text
//    auto* sci = editor->scintilla();
//    int line = -1;
//    for (int i = 0; i < m_lastResult.meta.size(); i++) {
//        if (m_lastResult.meta[i].nodeId == node.id) { line = i; break; }
//    }
//    if (line < 0) return;
//    const auto& lm = m_lastResult.meta[line];
//    ColumnSpan ts = typeSpanFor(lm);
//    int linePos = sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, line);
//    int typePos = linePos + (ts.start > 0 ? ts.start : 0);
//    int xPos = sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION, (uintptr_t)0, typePos);
//    int yPos = sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, (uintptr_t)0, linePos);
//    int lineH = sci->SendScintilla(QsciScintillaBase::SCI_TEXTHEIGHT, line);
//    QPoint gp = sci->viewport()->mapToGlobal(QPoint(xPos, yPos + lineH));
//    m_hexToolbar->popup(gp);
//}

void RcxController::hideHexToolbar() {
    if (m_hexToolbar && m_hexToolbar->isVisible() && !m_hexToolbar->isPinned())
        m_hexToolbar->hide();
}

void RcxController::joinHexNodes(uint64_t nodeId, NodeKind targetKind) {
    int ni = m_doc->tree.indexOfId(nodeId);
    if (ni < 0) return;
    // Save fields by value — references invalidate after tree mutations
    const int origOffset = m_doc->tree.nodes[ni].offset;
    const uint64_t origParentId = m_doc->tree.nodes[ni].parentId;
    const QString origName = m_doc->tree.nodes[ni].name;
    int curSz = sizeForKind(m_doc->tree.nodes[ni].kind);
    int tgtSz = sizeForKind(targetKind);
    if (tgtSz <= curSz) return;

    // Collect adjacent hex nodes by byte range (any hex kind)
    QVector<int> mergeIndices;
    mergeIndices.append(ni);
    int accumulated = curSz;
    int nextOff = origOffset + curSz;
    // Repeatedly scan for the node at exactly nextOff
    while (accumulated < tgtSz) {
        int found = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const auto& sib = m_doc->tree.nodes[i];
            if (sib.parentId == origParentId && sib.offset == nextOff && isHexNode(sib.kind)) {
                found = i;
                break;
            }
        }
        if (found < 0) break;
        int sibSz = sizeForKind(m_doc->tree.nodes[found].kind);
        mergeIndices.append(found);
        accumulated += sibSz;
        nextOff += sibSz;
    }
    if (accumulated < tgtSz) {
        emit statusHint(QStringLiteral("Cannot resize: need %1 bytes at +0x%2, only %3 available")
            .arg(tgtSz).arg(QString::number(origOffset, 16).toUpper()).arg(accumulated));
        return;
    }

    // Save merged node IDs before removal (for selection transfer)
    QVector<uint64_t> mergedIds;
    for (int j : mergeIndices)
        mergedIds.append(m_doc->tree.nodes[j].id);

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

    // Insert one joined node (use saved values — tree was mutated above)
    Node joined;
    joined.kind = targetKind;
    joined.name = origName;
    joined.parentId = origParentId;
    joined.offset = origOffset;
    joined.id = m_doc->tree.reserveId();
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{joined, {}}));

    // Transfer selection from merged nodes to the new joined node
    bool wasSelected = m_selIds.remove(nodeId);
    for (uint64_t mid : mergedIds)
        wasSelected |= m_selIds.remove(mid);
    if (wasSelected)
        m_selIds.insert(joined.id);

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
    if (!this->provider() || !this->provider()->isWritable()) return;
    if (isViewingPast() || m_readOnlyOverride) {
        emit statusHint(isViewingPast() ? viewingPastHint() : QStringLiteral("Target is read-only"));
        return;
    }

    const auto& bm = node.bitfieldMembers[memberIdx];
    int64_t signedOff = m_doc->tree.computeOffset(ni);
    if (signedOff < 0) return;
    uint64_t addr = baseAddress() + static_cast<uint64_t>(signedOff);
    int containerSize = sizeForKind(node.elementKind);
    if (containerSize <= 0) containerSize = 4;

    QByteArray oldBytes(containerSize, 0);
    this->provider()->read(addr, oldBytes.data(), containerSize);

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
    if (!this->provider() || !this->provider()->isWritable()) return;
    if (isViewingPast() || m_readOnlyOverride) {
        emit statusHint(isViewingPast() ? viewingPastHint() : QStringLiteral("Target is read-only"));
        return;
    }

    const auto& bm = node.bitfieldMembers[memberIdx];
    int64_t signedOff = m_doc->tree.computeOffset(ni);
    if (signedOff < 0) return;
    uint64_t addr = baseAddress() + static_cast<uint64_t>(signedOff);
    int containerSize = sizeForKind(node.elementKind);
    if (containerSize <= 0) containerSize = 4;

    // Read current value
    uint64_t curVal = fmt::extractBits(*this->provider(), addr, node.elementKind,
                                       bm.bitOffset, bm.bitWidth);
    uint64_t maxVal = (bm.bitWidth >= 64) ? UINT64_MAX : ((1ULL << bm.bitWidth) - 1);

    auto inputOpt = ThemedInputDialog::getText(nullptr,
        QStringLiteral("Edit Bitfield Value"),
        QStringLiteral("%1 (%2 bits, max %3):")
            .arg(bm.name).arg(bm.bitWidth).arg(maxVal),
        QString::number(curVal));
    if (!inputOpt || inputOpt->isEmpty()) return;
    const QString input = *inputOpt;

    // Parse value (support hex with 0x prefix)
    bool ok = false;
    uint64_t newVal;
    if (input.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        newVal = input.mid(2).toULongLong(&ok, 16);
    else
        newVal = input.toULongLong(&ok, 10);
    if (!ok) return;
    newVal &= maxVal;

    QByteArray oldBytes(containerSize, 0);
    this->provider()->read(addr, oldBytes.data(), containerSize);

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

void RcxController::appendBytesDialog(QWidget* parent, uint64_t targetId) {
    auto inputOpt = ThemedInputDialog::getText(parent,
        QStringLiteral("Append Bytes"),
        QStringLiteral("Byte count (decimal or 0x hex):"),
        QStringLiteral("128"));
    if (!inputOpt || inputOpt->trimmed().isEmpty()) return;
    QString trimmed = inputOpt->trimmed();
    bool ok = false;
    int byteCount = 0;
    if (trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        byteCount = trimmed.mid(2).toInt(&ok, 16);
    else
        byteCount = trimmed.toInt(&ok, 10);
    if (!ok || byteCount <= 0) return;
    appendBytes(targetId, byteCount);
}

// Helper: create a prev ← center → next button row for a context menu.
// The row stays open — user can click ← or → multiple times to cycle.
// Labels update after each click to reflect the new position.
static QWidgetAction* makeCycleRow(QMenu* menu,
                                    const QVector<NodeKind>& variants, int startIdx,
                                    const QString& centerLabel,
                                    std::function<void(NodeKind)> onSelect) {
    const auto& theme = ThemeManager::instance().current();
    QSettings s("RC", "RC");
    QFont font(s.value("font", "JetBrains Mono").toString(), 10);
    font.setFixedPitch(true);
    QString css = QStringLiteral(
        "QPushButton { background: transparent; color: %1;"
        " border: none; padding: 3px 6px; border-radius: 2px; }"
        "QPushButton:hover { background: %2; color: %3; }")
        .arg(theme.textDim.name(), theme.hover.name(), theme.text.name());
    auto kn = [](NodeKind k) {
        auto* m = kindMeta(k); return m ? QString::fromLatin1(m->typeName) : QStringLiteral("?");
    };

    auto* row = new QWidget;
    auto* hl = new QHBoxLayout(row);
    hl->setContentsMargins(8, 2, 8, 2);
    hl->setSpacing(0);

    auto* prev = new QPushButton(row);
    prev->setFont(font); prev->setCursor(Qt::PointingHandCursor); prev->setStyleSheet(css);
    hl->addWidget(prev);

    auto* label = new QLabel(row);
    label->setFont(font); label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(theme.textMuted.name()));
    hl->addWidget(label);

    auto* next = new QPushButton(row);
    next->setFont(font); next->setCursor(Qt::PointingHandCursor); next->setStyleSheet(css);
    hl->addWidget(next);

    // Shared mutable index — updated on each click, labels refresh
    auto idx = std::make_shared<int>(startIdx);
    int N = variants.size();
    auto refresh = [=]() {
        int pi = (*idx - 1 + N) % N;
        int ni = (*idx + 1) % N;
        prev->setText(QStringLiteral("\u2190 %1").arg(kn(variants[pi])));
        label->setText(QStringLiteral(" %1 ").arg(centerLabel));
        next->setText(QStringLiteral("%1 \u2192").arg(kn(variants[ni])));
    };
    refresh();

    auto* wa = new QWidgetAction(menu);
    wa->setDefaultWidget(row);

    // Don't close menu — user can click repeatedly to cycle
    QObject::connect(prev, &QPushButton::clicked, menu, [=]() {
        *idx = (*idx - 1 + N) % N;
        onSelect(variants[*idx]);
        refresh();
    });
    QObject::connect(next, &QPushButton::clicked, menu, [=]() {
        *idx = (*idx + 1) % N;
        onSelect(variants[*idx]);
        refresh();
    });
    return wa;
}

void RcxController::showContextMenu(RcxEditor* editor, int line, int nodeIdx,
                                     int subLine, const QPoint& globalPos) {
    // Menu icons must be INKED for the theme. The raw :/vsicons SVGs carry VS
    // Code's #C5C5C5, which reads correctly on a dark menu and washes out to a
    // pale grey on a light one — the item TEXT comes from the palette and is
    // black, so the icons beside it looked disabled. themedVsIcon swaps that
    // grey for the theme's text colour and caches on (path, tint, size, dpr).
    // A context menu is rebuilt on every right-click, so this picks up a theme
    // switch for free — unlike the menu bar, which needs retintMenuIcons.
    const QColor menuInk = ThemeManager::instance().current().text;
    const qreal menuDpr = editor ? editor->devicePixelRatioF()
                                 : qApp->devicePixelRatio();
    auto icon = [menuInk, menuDpr](const char* name) {
        return themedVsIcon(QStringLiteral(":/vsicons/%1").arg(name), menuInk, 16, menuDpr);
    };
    // Destructive rows are red — theme.markerPtr, the same token the dialogs
    // use for a destructive button. The icon is tinted here; the LABEL is
    // painted by MenuBarStyle, which reads this property off the action
    // (QStyleOptionMenuItem carries no QAction of its own).
    const QColor destructiveInk = ThemeManager::instance().current().markerPtr;
    auto destructiveIcon = [destructiveInk, menuDpr](const char* name) {
        return themedVsIcon(QStringLiteral(":/vsicons/%1").arg(name),
                            destructiveInk, 16, menuDpr);
    };
    auto markDestructive = [](QAction* a) {
        if (a) a->setProperty("rcxDestructive", true);
        return a;
    };

    const bool hasNode = nodeIdx >= 0 && nodeIdx < m_doc->tree.nodes.size();

    // Selection policy
    if (hasNode) {
        // Use the SAME encoded id the click path stores (selIdForLine):
        // array-element / member / footer rows carry encoding bits in
        // m_selIds, so a raw-nodeId membership test would treat an
        // already-selected such row as "outside the selection" and reset
        // it — wrongly dropping an active byte selection (and its submenu)
        // when you right-click the very rows it covers. Match handleNodeClick.
        uint64_t clickedId = (line >= 0 && line < m_lastResult.meta.size())
            ? selIdForLine(m_lastResult.meta[line])
            : m_doc->tree.nodes[nodeIdx].id;
        if (!m_selIds.contains(clickedId)) {
            // Right-clicking a row outside the current selection moves the
            // selection here and drops any active byte selection (and its
            // submenu) so the two stay coherent. clearByteSelection emits
            // byteSelectionRowsChanged(empty) → onByteSelectionRows clears
            // m_selIds, so do it before installing the clicked id.
            if (editor) editor->clearByteSelection();
            m_selIds.clear();
            m_selIds.insert(clickedId);
            m_anchorLine = line;
            applySelectionOverlays();
            // Right-click moved the selection here — emit so the status-bar
            // "N nodes selected" count tracks it. Every other selection-mutation
            // path emits selectionChanged (most via updateCommandRow); this
            // direct branch did not, leaving the count stale on the pre-click
            // value until the next operation.
            emit selectionChanged(m_selIds.size());
        }
    }

    // Multi-select batch actions
    if (hasNode && m_selIds.size() > 1) {
        QMenu menu;
        addByteSubmenu(menu, editor);  // "Selected bytes (N) ▸" at top, if any

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

        // ── Delete ──
        // Above Carve, matching the single-node menu: the common destructive
        // action first, the rare structural one after it.
        markDestructive(menu.addAction(destructiveIcon("trash.svg"),
                                       QString("Delete %1 nodes").arg(count),
                                       [this, collectIndices]() {
            batchRemoveNodes(collectIndices());
        }));
        menu.addSeparator();

        // Break the multi-node selection into a new class. Only when there's no
        // byte selection — addByteSubmenu already added the top-level action in
        // that case. regionFromCurrentSelection unions the selected rows' spans.
        if (!editor || !editor->hasByteSelection()) {
            menu.addAction(icon("symbol-structure.svg"), "Carve", [this, editor]() {
                // Refuse if any selected row is inside an embedded class (its
                // offset is relative to that class's def → wrong region).
                for (uint64_t sid : m_selIds) {
                    uint64_t nid = baseNodeIdFromSelId(sid);
                    if (!nodeInView(nid)) {
                        emit statusHint(QStringLiteral(
                            "Can't break a selection that includes a field inside "
                            "an embedded class."));
                        return;
                    }
                    if (!isDirectViewFrameChild(nid)) {
                        emit statusHint(QStringLiteral(
                            "Can't break a selection that includes a field nested "
                            "in a union or inline struct."));
                        return;
                    }
                }
                auto region = regionFromCurrentSelection(editor);
                if (region) extractByteSelectionToNewClass(region->first, region->second);
            });
            menu.addSeparator();
        }

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

        // "Next Type →" for multi-select (same size, filtered variants)
        if (allSame) {
            int sz = sizeForKind(commonKind);
            if (sz > 0 && !isCodeKind(commonKind)) {
                bool curStr = isStringKind(commonKind);
                bool curVec = isVectorKind(commonKind);
                QVector<NodeKind> variants;
                for (const auto& m : kKindMeta) {
                    if (m.size != sz || isContainerKind(m.kind)) continue;
                    if (!curStr && isStringKind(m.kind)) continue;
                    if (!curVec && isVectorKind(m.kind)) continue;
                    // Never: asm's table size is 1 but its real footprint is
                    // arrayLen, so it does not belong in a by-size cycle in
                    // either direction.
                    if (isCodeKind(m.kind)) continue;
                    variants.append(m.kind);
                }
                int ci = variants.indexOf(commonKind);
                if (ci >= 0 && variants.size() > 1) {
                    menu.addAction(makeCycleRow(&menu, variants, ci,
                        QStringLiteral("\u2190\u2192"),
                        [this, collectIndices](NodeKind k) { batchChangeKind(collectIndices(), k); }));
                }
                // Resize row for multi-select hex nodes (no hex128)
                if (isHexNode(commonKind) && commonKind != NodeKind::Hex128) {
                    static constexpr NodeKind hexCycle[] = {
                        NodeKind::Hex8, NodeKind::Hex16, NodeKind::Hex32,
                        NodeKind::Hex64 };
                    int hi = -1;
                    for (int i = 0; i < 4; i++) if (hexCycle[i] == commonKind) { hi = i; break; }
                    if (hi >= 0) {
                        QVector<NodeKind> hv = {NodeKind::Hex8, NodeKind::Hex16, NodeKind::Hex32,
                                                 NodeKind::Hex64};
                        menu.addAction(makeCycleRow(&menu, hv, hi,
                            QStringLiteral("Spc"),
                            [this, collectIndices](NodeKind k) { batchChangeKind(collectIndices(), k); }));
                    }
                }
                menu.addSeparator();
            }
        }

        menu.addAction(icon("symbol-structure.svg"), QString("Change type of %1 nodes...").arg(count),
                       [this, ids, collectIndices]() {
            QStringList types;
            for (const auto& e : kKindMeta) types << e.name;
            auto sel = ThemedInputDialog::getItem(nullptr,
                QStringLiteral("Change Type"),
                QStringLiteral("New type:"),
                types, 0);
            if (sel)
                batchChangeKind(collectIndices(), kindFromString(*sel));
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

        // Comment is always available — discoverability matters even when
        // showComments is off (users can author comments before turning the
        // display on). Action signature stays the same; the rendered chip
        // simply won't appear until the toggle flips.
        menu.addAction(icon("edit.svg"), QString("&Comment %1 nodes\t;").arg(count), [this, ids]() {
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

        menu.addSeparator();

        QMenu* copyMenu = menu.addMenu(icon("clippy.svg"), "Copy");
        copyMenu->addAction(icon("link.svg"), "Copy &Address", [this, ids]() {
            // m_selIds is a QSet — iterate into (offset, text) pairs and sort
            // by offset so the copied list is in struct order, not hash order.
            QVector<QPair<int64_t, QString>> rows;
            for (uint64_t id : ids) {
                int ni = m_doc->tree.indexOfId(id);
                if (ni < 0) continue;
                int64_t off = m_doc->tree.computeOffset(ni);
                if (off < 0) continue;
                uint64_t addr = baseAddress() + static_cast<uint64_t>(off);
                rows.append({off, QStringLiteral("0x") + QString::number(addr, 16).toUpper()});
            }
            std::sort(rows.begin(), rows.end(),
                      [](const QPair<int64_t, QString>& a,
                         const QPair<int64_t, QString>& b) { return a.first < b.first; });
            QStringList addrs;
            for (const auto& r : rows) addrs << r.second;
            QApplication::clipboard()->setText(addrs.join('\n'));
        });
        copyMenu->addSeparator();
        if (editor) {
            copyMenu->addAction("Copy Line", [editor, line]() {
                const QString text = editor->lineTextForCopy(line).trimmed();
                if (!text.isEmpty())
                    QApplication::clipboard()->setText(text);
            });
        }
        copyMenu->addAction("Copy All as Text", [editor]() {
            if (editor) QApplication::clipboard()->setText(editor->textWithMargins());
        });
        {
            QSet<uint64_t> capIds = ids;
            auto lastResult = m_lastResult;
            copyMenu->addAction(tr("Copy Selected as Text (%1 row%2)")
                                    .arg(ids.size()).arg(ids.size() == 1 ? "" : "s"),
                                [editor, capIds, lastResult]() {
                // Each row loses its fold column and trailing blanks, then the
                // indent every copied row shares is cut once. Trimming rows one
                // by one shifted each by a different amount, so the tree's
                // columns no longer lined up in the paste.
                struct Row { QString margin, body; };
                QVector<Row> rows;
                int common = -1;
                for (int i = 0; i < lastResult.meta.size(); ++i) {
                    const auto& lm = lastResult.meta[i];
                    if (lm.nodeIdx < 0) continue;
                    if (!capIds.contains(selIdForLine(lm))) continue;
                    QString body = editor ? editor->lineTextForCopy(i) : QString();
                    body = body.mid(LineGeometry::forLine(lm).prefixWidth);
                    int end = body.size();
                    while (end > 0 && body.at(end - 1).isSpace()) --end;
                    body.truncate(end);
                    if (!body.isEmpty()) {
                        int lead = 0;
                        while (lead < body.size() && body.at(lead) == QLatin1Char(' ')) ++lead;
                        common = common < 0 ? lead : qMin(common, lead);
                    }
                    rows.append({lm.offsetText, body});
                }
                if (rows.isEmpty()) return;
                QStringList lines;
                for (const Row& r : rows) lines.append(r.margin + r.body.mid(qMax(0, common)));
                QApplication::clipboard()->setText(lines.join('\n'));
            });
        }

        // Save selected nodes' raw bytes as a binary file (parallel to
        // the byte-selection menu's same action). For multi-row, the
        // saved blob is the concatenation of each selected node's bytes
        // in ascending-offset order — non-contiguous rows pack tightly,
        // gaps between them are dropped (the user picked specific rows;
        // an unselected gap shouldn't bleed into the file).
        menu.addAction(icon("save.svg"),
                       QString("Save %1 nodes as binary file…").arg(count),
                       [this, ids]() {
            // Resolve node indices + absolute addr/size, then sort by offset.
            struct Span { uint64_t addr; int sz; };
            QVector<Span> spans;
            for (uint64_t id : ids) {
                int ni = m_doc->tree.indexOfId(id);
                if (ni < 0) continue;
                const Node& n = m_doc->tree.nodes[ni];
                int sz = (n.kind == NodeKind::Struct || n.kind == NodeKind::Array)
                    ? m_doc->tree.structSpan(n.id) : n.byteSize();
                if (sz <= 0) continue;
                int64_t off = m_doc->tree.computeOffset(ni);
                if (off < 0) continue;
                spans.append({baseAddress() + uint64_t(off), sz});
            }
            if (spans.isEmpty()) {
                emit statusHint(QStringLiteral("No readable bytes in selection"));
                return;
            }
            std::sort(spans.begin(), spans.end(),
                      [](const Span& a, const Span& b) { return a.addr < b.addr; });
            int64_t total = 0;
            for (const auto& s : spans) total += s.sz;
            if (total > (1 << 27)) {  // 128 MB sanity cap
                emit statusHint(QStringLiteral("Selection too large to save"));
                return;
            }
            QString defaultName = QStringLiteral("nodes_%1_%2.bin")
                .arg(spans.first().addr, 0, 16).arg(total);
            QString path = QFileDialog::getSaveFileName(
                qobject_cast<QWidget*>(parent()),
                QStringLiteral("Save Selected Nodes"),
                defaultName,
                QStringLiteral("Binary (*.bin);;All Files (*)"));
            if (path.isEmpty()) return;

            const Provider* prov = displayedProvider();   // what is on screen: recorded values too
            if (!prov) {
                emit statusHint(QStringLiteral("No active provider"));
                return;
            }
            QByteArray data;
            data.reserve(int(total));
            for (const auto& s : spans) {
                if (prov->isReadable(s.addr, s.sz))
                    data.append(prov->readBytes(s.addr, s.sz));
                else
                    data.append(QByteArray(s.sz, '\0'));  // unreadable → zero-fill
            }
            QFile out(path);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                emit statusHint(QStringLiteral("Couldn't open %1").arg(out.errorString()));
                return;
            }
            if (out.write(data) != data.size()) {
                emit statusHint(QStringLiteral("Write failed: %1").arg(out.errorString()));
                return;
            }
            emit statusHint(QStringLiteral("Saved %1 byte%2 to %3")
                .arg(data.size()).arg(data.size() == 1 ? "" : "s")
                .arg(QFileInfo(path).fileName()));
        });

        menu.addSeparator();
        menu.addAction(tr("Clear selection"), [this, editor]() {
            if (editor) editor->clearByteSelection();
            clearSelection();
        });

        emit contextMenuAboutToShow(&menu, line);
        menu.exec(globalPos);
        return;
    }

    QMenu menu;
    addByteSubmenu(menu, editor);  // "Selected bytes (N) ▸" at top, if any

    // ── Node-specific actions (only when clicking on a node) ──
    if (hasNode) {
        const Node& node = m_doc->tree.nodes[nodeIdx];
        if (isPointerKind(node.kind) && node.refId != 0) {
            auto* beside = menu.addAction(icon("split-vertical.svg"), tr("Open Beside"),
                [this, editor, line] { openPointerBeside(editor, line); });
            beside->setObjectName(QStringLiteral("openInstanceBeside"));
            menu.addSeparator();
        }
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
            menu.addAction(icon("clippy.svg"), "Copy &Path", [this, nodeId]() {
                QString p = m_doc->tree.fieldPath(nodeId);
                if (p.isEmpty()) {
                    emit statusHint(QStringLiteral("Field has no path (unnamed)"));
                    return;
                }
                QApplication::clipboard()->setText(p);
                emit statusHint(QStringLiteral("Copied path: %1").arg(p));
            });
            menu.addSeparator();
            markDestructive(menu.addAction(destructiveIcon("trash.svg"), "&Delete",
                                           [this, nodeId]() {
                int ni = m_doc->tree.indexOfId(nodeId);
                if (ni >= 0) removeNode(ni);
            }));
            menu.addSeparator();
            // Fall through to always-available actions
        } else {

        // ── Delete ──
        // Above Carve deliberately: it is what people open this menu for,
        // and it used to sit at the very bottom under every type conversion
        // while the far rarer, far more specialised Carve held the top.
        markDestructive(menu.addAction(destructiveIcon("trash.svg"), "&Delete\tDelete",
                                       [this, nodeId, editor]() {
            int ni = m_doc->tree.indexOfId(nodeId);
            if (ni < 0) return;
            // A delete is a structural change: drop any active byte selection
            // so it doesn't re-paint onto the node that shifts up (see
            // batchRemoveNodes). No-op when nothing is byte-selected.
            if (editor) editor->clearByteSelection();
            removeNode(ni);
        }));
        menu.addSeparator();

        // ── New Class / Ptr to New Class (promoted near top) ──
        if (node.kind != NodeKind::Struct && node.kind != NodeKind::Array) {
            int nodeSz = node.byteSize();
            // "Carve" — extract the active selection (byte range or
            // selected nodes), or failing that this node's own bytes, off
            // into a new embedded class sized EXACTLY to the region, moving
            // any fully-contained structs/typed fields in intact. Replaces
            // the old fixed-64-byte "New Class"; the legacy "make a class of
            // sample hex" behavior lives on in "Ptr to New Class" below.
            // With a byte selection active, the promoted top-level "Break into
            // Class" (see addByteSubmenu) already covers it — skip this
            // node-level duplicate; keep it for the no-byte-selection cases
            // (the clicked node's own span, or multi-node selections).
            if (!editor || !editor->hasByteSelection())
            menu.addAction(icon("symbol-structure.svg"), "Carve", [this, editor, nodeId]() {
                int ni = m_doc->tree.indexOfId(nodeId);
                if (ni < 0) return;
                // A field shown INSIDE an embedded class stores its offset
                // relative to that class's OWN definition, so breaking it would
                // resolve to the wrong region in the viewed class and wrap the
                // wrong field. Refuse rather than do the wrong thing — breaking
                // inside an embedded class proper is a separate feature.
                if (!nodeInView(nodeId)) {
                    emit statusHint(QStringLiteral(
                        "Can't break a field inside an embedded class — open that "
                        "class as its own view first."));
                    return;
                }
                // A union member / inline-struct field reaches the root (passes
                // nodeInView) but its offset is relative to its own container,
                // so the break scan would resolve it to the wrong region.
                if (!isDirectViewFrameChild(nodeId)) {
                    emit statusHint(QStringLiteral(
                        "Can't break a field nested in a union or inline struct — "
                        "break it from that struct's own view."));
                    return;
                }
                auto region = regionFromCurrentSelection(editor);
                if (!region) {
                    // No selection — break just the clicked node's own span.
                    const Node& n = m_doc->tree.nodes[ni];
                    int sz = (n.kind == NodeKind::Struct || n.kind == NodeKind::Array)
                             ? m_doc->tree.structSpan(n.id) : n.byteSize();
                    if (sz <= 0) return;
                    const uint64_t lo = baseAddress() + (uint64_t)n.offset;
                    region = QPair<uint64_t, uint64_t>(lo, lo + (uint64_t)sz);
                }
                extractByteSelectionToNewClass(region->first, region->second);
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
            const LineChip* tipChip = findChip(lm, ChipKind::TypeHint);
            if (tipChip && !tipChip->typeHintKinds.isEmpty()) {
                const QVector<NodeKind>& tipKinds = tipChip->typeHintKinds;
                NodeKind suggested = tipKinds[0];
                if (tipKinds.size() == 1) {
                    auto* m = kindMeta(suggested);
                    QString label = QStringLiteral("Convert to %1").arg(QString::fromLatin1(m->typeName));
                    menu.addAction(label, [this, nodeId, suggested]() {
                        int ni = m_doc->tree.indexOfId(nodeId);
                        if (ni >= 0) changeNodeKind(ni, suggested);
                    });
                } else {
                    auto* m = kindMeta(tipKinds[0]);
                    QString label = QStringLiteral("Split into %1\u00D7%2")
                        .arg(QString::fromLatin1(m->typeName))
                        .arg(tipKinds.size());
                    menu.addAction(label, [this, nodeId, kinds = tipKinds]() {
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

        // "← prev | current (N/M) | next →" type cycling row
        {
            int sz = sizeForKind(node.kind);
            if (sz > 0 && !isCodeKind(node.kind)) {
                bool curStr = isStringKind(node.kind);
                bool curVec = isVectorKind(node.kind);
                QVector<NodeKind> variants;
                for (const auto& m : kKindMeta) {
                    if (m.size != sz || isContainerKind(m.kind)) continue;
                    if (!curStr && isStringKind(m.kind)) continue;
                    if (!curVec && isVectorKind(m.kind)) continue;
                    // Never: asm's table size is 1 but its real footprint is
                    // arrayLen, so it does not belong in a by-size cycle in
                    // either direction.
                    if (isCodeKind(m.kind)) continue;
                    variants.append(m.kind);
                }
                int ci = variants.indexOf(node.kind);
                if (ci >= 0 && variants.size() > 1) {
                    auto kn = [](NodeKind k) {
                        auto* m = kindMeta(k); return m ? QString::fromLatin1(m->typeName) : QStringLiteral("?");
                    };
                    menu.addAction(makeCycleRow(&menu, variants, ci,
                        QStringLiteral("\u2190\u2192"),
                        [this, nodeId](NodeKind k) {
                            int ni = m_doc->tree.indexOfId(nodeId);
                            if (ni >= 0) changeNodeKind(ni, k);
                        }));
                    addedQuickConvert = true;
                }
            }
        }

        // Hex resize row: ← smaller | Spc | larger → (no hex128)
        if (isHexNode(node.kind) && node.kind != NodeKind::Hex128) {
            static constexpr NodeKind hexCycle[] = {
                NodeKind::Hex8, NodeKind::Hex16, NodeKind::Hex32,
                NodeKind::Hex64 };
            int hi = -1;
            for (int i = 0; i < 4; i++) if (hexCycle[i] == node.kind) { hi = i; break; }
            if (hi >= 0) {
                QVector<NodeKind> hv = {NodeKind::Hex8, NodeKind::Hex16, NodeKind::Hex32,
                                         NodeKind::Hex64};
                int nodeOff = node.offset;
                uint64_t nodePid = node.parentId;
                menu.addAction(makeCycleRow(&menu, hv, hi,
                    QStringLiteral("Spc"),
                    [this, nodeOff, nodePid](NodeKind k) {
                        // Find the current hex node at this offset (ID may have changed after join)
                        uint64_t nid = 0;
                        for (const auto& n : m_doc->tree.nodes)
                            if (n.parentId == nodePid && n.offset == nodeOff && isHexNode(n.kind))
                                { nid = n.id; break; }
                        if (nid == 0) return;
                        int ni = m_doc->tree.indexOfId(nid);
                        if (ni < 0) return;
                        int curSz = sizeForKind(m_doc->tree.nodes[ni].kind);
                        int tgtSz = sizeForKind(k);
                        if (tgtSz > curSz)
                            joinHexNodes(nid, k);
                        else if (tgtSz < curSz)
                            changeNodeKind(ni, k);
                    }));
                addedQuickConvert = true;
            }
        }

        if (addedQuickConvert)
            menu.addSeparator();

        // ── Hex byte / ASCII inline editing ──
        if (isHexNode(node.kind) && this->provider()->isWritable()) {
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
                          && this->provider()->isWritable();
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

        // Comment is always available — see batch path for rationale.
        menu.addAction(icon("edit.svg"), "&Comment\t;", [editor, line]() {
            editor->beginInlineEdit(EditTarget::Comment, line);
        });

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
            // The fnptr/ptr conversions above add actions but didn't mark the
            // menu non-empty; without this the Convert submenu gets disabled
            // (the `if (!hasConvert)` guard below) for a node whose ONLY
            // available conversion is one of these (e.g. FuncPtr64 -> ptr64).
            if (node.kind == NodeKind::Hex64 || node.kind == NodeKind::Pointer64 ||
                node.kind == NodeKind::Hex32 || node.kind == NodeKind::Pointer32 ||
                node.kind == NodeKind::FuncPtr64 || node.kind == NodeKind::FuncPtr32)
                hasConvert = true;
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

        // ── Big-endian toggle (only kinds the formatter actually swaps) ──
        {
            if (isEndianSwappable(node.kind)) {
                bool cur = node.bigEndian;
                QAction* act = menu.addAction("Big &endian", [this, nodeId, cur]() {
                    int ni = m_doc->tree.indexOfId(nodeId);
                    if (ni < 0) return;
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::ToggleBigEndian{nodeId, cur, !cur}));
                });
                act->setCheckable(true);
                act->setChecked(cur);
            }
        }

        // ── Structure ► submenu (only when relevant) ──
        {
            auto* structMenu = menu.addMenu("Structure");
            bool hasStructAction = false;

            if (node.kind == NodeKind::Struct || node.kind == NodeKind::Array) {
                structMenu->addAction(icon("diff-added.svg"), "Add &Child", [this, nodeId]() {
                    insertNode(nodeId, 0, NodeKind::Hex64, "newField");
                });
                if (isCollapsed(node)) {
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

            // An enum-typed field folds open to its members like a struct.
            if (enumTypeOf(m_doc->tree, node)) {
                const bool collapsed = isCollapsed(node);
                structMenu->addAction(icon(collapsed ? "expand-all.svg" : "collapse-all.svg"),
                                      collapsed ? "&Expand" : "&Collapse", [this, nodeId]() {
                    int ni = m_doc->tree.indexOfId(nodeId);
                    if (ni >= 0) toggleCollapse(ni);
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

        // ── Duplicate ── (Delete is promoted above Carve, near the top)
        menu.addAction(icon("files.svg"), "D&uplicate\tCtrl+D", [this, nodeId]() {
            int ni = m_doc->tree.indexOfId(nodeId);
            if (ni >= 0) duplicateNode(ni);
        });


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

    // ── Timeline: when did this change? ──
    // Scoped to the selection, like the View ▸ Timeline actions. The
    // shortcut is shown as a hint only — it is registered once, on the menu.
    if (hasNode && m_timelineCtx && !m_timelineCtx->model().isEmpty()) {
        QString of = timelineScopeLabel();
        of.replace(QLatin1Char('&'), QStringLiteral("&&"));
        if (!of.isEmpty()) of.prepend(QStringLiteral(" of "));
        menu.addAction(QStringLiteral("Previous Change%1\tCtrl+,").arg(of),
                       [this]() { stepTimelineChange(-1); });
        QAction* next = menu.addAction(QStringLiteral("Next Change%1\tCtrl+.").arg(of),
                                       [this]() { stepTimelineChange(+1); });
        next->setEnabled(isViewingPast());
        menu.addAction(QStringLiteral("Show in Timeline"), [this]() { revealSelectionInTimeline(); });
        menu.addSeparator();
    }

    // ── Bookmark this address (user-defined symbol via bookmark) ──
    if (hasNode) {
        uint64_t labelNodeId = m_doc->tree.nodes[nodeIdx].id;
        const LineMeta* bookmarkLine = editor->metaForLine(line);
        const uint64_t bookmarkAddress = bookmarkLine ? bookmarkLine->offsetAddr : baseAddress();
        menu.addAction(icon("symbol-key.svg"), "Bookmark this address...",
                       [this, labelNodeId, bookmarkAddress]() {
            int ni = m_doc->tree.indexOfId(labelNodeId);
            if (ni < 0) return;
            int64_t off = m_doc->tree.computeOffset(ni);
            if (off < 0) return;
            uint64_t addr = bookmarkAddress;
            auto entered = ThemedInputDialog::getText(nullptr,
                QStringLiteral("Bookmark this address"),
                QStringLiteral("Symbol name for 0x%1").arg(addr, 0, 16),
                {}, QStringLiteral("symbol name"));
            if (!entered || entered->trimmed().isEmpty()) return;
            QString formula;
            if (!baseAddressFormula().isEmpty() && off >= 0)
                formula = QStringLiteral("(%1)+0x%2")
                    .arg(baseAddressFormula())
                    .arg((qulonglong)off, 0, 16);
            else
                formula = QStringLiteral("0x%1").arg(addr, 0, 16);
            addBookmark(entered->trimmed(), addressExpression(addr, formula));
            if (g_namesChangedHook) g_namesChangedHook();
        });
        menu.addSeparator();
    }

    // ── Copy ──
    QMenu* copyMenu = menu.addMenu(icon("clippy.svg"), "Copy");
    if (hasNode) {
        uint64_t copyNodeId = m_doc->tree.nodes[nodeIdx].id;
        copyMenu->addAction(icon("link.svg"), "Copy &Address\tCtrl+Shift+C", [this, copyNodeId]() {
            int ni = m_doc->tree.indexOfId(copyNodeId);
            if (ni < 0) return;
            int64_t off = m_doc->tree.computeOffset(ni);
            if (off < 0) return;
            uint64_t addr = baseAddress() + static_cast<uint64_t>(off);
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
    copyMenu->addAction("Copy Line", [editor, line]() {
        const QString text = editor->lineTextForCopy(line).trimmed();
        if (!text.isEmpty())
            QApplication::clipboard()->setText(text);
    });
    copyMenu->addAction("Copy All as Text", [editor]() {
        QApplication::clipboard()->setText(editor->textWithMargins());
    });


    // ── Tracking (always available at bottom) ──
    {
        auto* trackMenu = menu.addMenu("Tracking");
        auto* act = trackMenu->addAction("Track Value Changes");
        act->setCheckable(true);
        act->setChecked(m_trackValues);
        connect(act, &QAction::toggled, this, &RcxController::setTrackValues);
        trackMenu->addAction("Clear All History", [this]() {
            resetChangeTracking();
            refresh();
            for (auto* ed : m_editors) ed->dismissHistoryPopup();
        });
    }

    // ── Kernel paging menu items ──
    if (this->provider() && this->provider()->hasKernelPaging()) {
        menu.addSeparator();
        auto* kernelMenu = menu.addMenu(icon("symbol-key.svg"), "Kernel");

        // Show Physical Address — translate the node's VA to physical
        if (hasNode) {
            int64_t nodeOff = m_doc->tree.computeOffset(nodeIdx);
            uint64_t nodeAddr = (nodeOff >= 0)
                ? baseAddress() + static_cast<uint64_t>(nodeOff) : 0;
            kernelMenu->addAction("Show Physical Address", [this, nodeAddr, &menu]() {
                auto result = this->provider()->translateAddress(nodeAddr);
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
                    ThemedMessageBox::info(
                        qobject_cast<QWidget*>(parent()),
                        QStringLiteral("Physical Address"), msg);
                } else {
                    ThemedMessageBox::warn(
                        qobject_cast<QWidget*>(parent()),
                        QStringLiteral("Translation Failed"),
                        QStringLiteral("Address 0x%1 isn't mapped in the current page table.")
                            .arg(nodeAddr, 16, 16, QChar('0')));
                }
            });
        }

        // Browse Page Tables — open PML4 in a new physical tab
        kernelMenu->addAction("Browse Page Tables", [this]() {
            uint64_t cr3 = this->provider()->getCr3();
            if (cr3 == 0) {
                ThemedMessageBox::warn(qobject_cast<QWidget*>(parent()),
                    QStringLiteral("Page Table Unavailable"),
                    QStringLiteral("Couldn't read CR3 from the kernel provider."));
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
                        uint64_t nodeAddr = baseAddress()
                            + static_cast<uint64_t>(nodeOff);
                        kernelMenu->addAction("Follow Physical Frame",
                            [this, nodeAddr, bitOff, bitWid]() {
                            uint64_t pteValue = 0;
                            if (!this->provider()->read(nodeAddr, &pteValue, 8)) {
                                ThemedMessageBox::warn(qobject_cast<QWidget*>(parent()),
                                    QStringLiteral("PTE Read Failed"),
                                    QStringLiteral("Couldn't read the page-table entry at 0x%1.")
                                        .arg(nodeAddr, 0, 16));
                                return;
                            }
                            uint64_t mask = (1ULL << bitWid) - 1;
                            uint64_t frame = ((pteValue >> bitOff) & mask) << bitOff;
                            if (frame == 0) {
                                ThemedMessageBox::warn(qobject_cast<QWidget*>(parent()),
                                    QStringLiteral("Frame Not Present"),
                                    QStringLiteral("The physical frame is zero. The page is likely "
                                                   "paged out or marked not present."));
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

    // Bottom "Clear selection" — clears the byte selection and the row
    // selection together. Only offered when there's something to clear.
    if ((editor && editor->hasByteSelection()) || !m_selIds.isEmpty()) {
        if (!menu.isEmpty()) menu.addSeparator();
        menu.addAction(tr("Clear selection"), [this, editor]() {
            if (editor) editor->clearByteSelection();
            clearSelection();
        });
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

    // Clear selection before delete (prevents stale highlight on shifted lines).
    // Also clear any active byte selection: it's address-based and survives a
    // refresh by design, but a delete is a structural change — leaving it set
    // would re-paint its bytes onto whatever node shifts up into that address
    // range (user-reported "deleted the end, now the top row shows selected
    // bytes"). clearByteSelection is a no-op when nothing is selected.
    for (auto* ed : m_editors)
        if (ed) ed->clearByteSelection();
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

// Replace a contiguous run of sibling fields with a single asm node covering
// exactly their bytes. `ordered` is in ascending offset order (batchChangeKind
// sorts it). Modelled on joinHexNodes, which does the same remove-many /
// insert-one dance for hex resizing.
void RcxController::mergeToCodeWindow(const QVector<uint64_t>& ordered) {
    if (ordered.size() < 2) return;

    // Every node must be a plain field under one parent. A container would
    // take its children with it, and this is not the place to decide what
    // that means — Carve is.
    const int firstIdx = m_doc->tree.indexOfId(ordered.first());
    if (firstIdx < 0) return;
    const uint64_t parentId = m_doc->tree.nodes[firstIdx].parentId;
    const int startOffset   = m_doc->tree.nodes[firstIdx].offset;
    const QString name      = m_doc->tree.nodes[firstIdx].name;

    int expected = startOffset;
    for (uint64_t id : ordered) {
        const int i = m_doc->tree.indexOfId(id);
        if (i < 0) return;
        const Node& n = m_doc->tree.nodes[i];
        if (isContainerKind(n.kind)) {
            emit statusHint(QStringLiteral(
                "Can't disassemble a selection containing a struct or array — "
                "select plain fields."));
            return;
        }
        if (n.parentId != parentId) {
            emit statusHint(QStringLiteral(
                "Can't disassemble a selection spanning two classes."));
            return;
        }
        // Contiguous: no holes, no overlaps. A byte selection is contiguous by
        // construction; a ctrl-clicked scatter is not, and silently swallowing
        // the bytes between would be the wrong kind of helpful.
        if (n.offset != expected) {
            emit statusHint(QStringLiteral(
                "Can't disassemble a broken selection — the fields must be "
                "back to back."));
            return;
        }
        expected += n.byteSize();
    }
    const int span = expected - startOffset;
    if (span <= 0) return;

    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Disassemble %1 bytes").arg(span));

    // Remove in reverse so the indices stay valid as the tree shrinks.
    for (int j = ordered.size() - 1; j >= 0; j--) {
        const int idx = m_doc->tree.indexOfId(ordered[j]);
        if (idx < 0) continue;
        QVector<Node> subtree{ m_doc->tree.nodes[idx] };
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::Remove{m_doc->tree.nodes[idx].id, subtree, {}}));
    }

    Node code;
    code.kind      = NodeKind::Asm;
    code.name      = name;
    code.parentId  = parentId;
    code.offset    = startOffset;
    code.arrayLen  = span;      // the declared window — see Node::byteSize
    code.collapsed = false;     // a collapsed code node shows nothing at all
    code.id        = m_doc->tree.reserveId();
    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{code, {}}));

    // The merged rows are gone; carry the selection to what replaced them.
    bool wasSelected = false;
    for (uint64_t id : ordered) wasSelected |= m_selIds.remove(id);
    if (wasSelected) m_selIds.insert(code.id);

    m_doc->undoStack.endMacro();
    m_suppressRefresh = false;
    refresh();
    emit selectionChanged(m_selIds.size());
}

void RcxController::batchChangeKind(const QVector<int>& nodeIndices, NodeKind newKind) {
    QSet<uint64_t> idSet;
    for (int idx : nodeIndices) {
        if (idx >= 0 && idx < m_doc->tree.nodes.size())
            idSet.insert(m_doc->tree.nodes[idx].id);
    }
    idSet = m_doc->tree.normalizePreferDescendants(idSet);
    if (idSet.isEmpty()) return;

    // Apply in ascending offset order (ties by id). Iterating the QSet
    // directly would push the ChangeKind/pad commands in hash order — the
    // user-visible sequence inside the macro (and the pad names) must be
    // deterministic. Sorted from a snapshot of the indices; the loop still
    // re-resolves each id because a shrink inserts pads and shifts indices.
    QVector<uint64_t> ordered(idSet.begin(), idSet.end());
    std::sort(ordered.begin(), ordered.end(), [this](uint64_t a, uint64_t b) {
        const int ia = m_doc->tree.indexOfId(a), ib = m_doc->tree.indexOfId(b);
        const int64_t oa = ia >= 0 ? m_doc->tree.computeOffset(ia) : 0;
        const int64_t ob = ib >= 0 ? m_doc->tree.computeOffset(ib) : 0;
        return oa != ob ? oa < ob : a < b;
    });

    // ── Code is a WINDOW, so a multi-row selection merges into ONE node ──
    //
    // Every other kind converts row by row: eight selected bytes become eight
    // uint8s, which is what you asked for. Machine code is different — an
    // instruction is 1..15 bytes and pays no attention to where the fields it
    // sits in happen to start, so converting per row gives a column of
    // `asm[1]` nodes each holding one instruction-fragment, and any
    // instruction spanning a field boundary is cut in half and rendered as a
    // `db` byte. The selection is a byte RANGE; the node has to be one too.
    if (isCodeKind(newKind) && ordered.size() > 1) {
        mergeToCodeWindow(ordered);
        return;
    }

    // Preserve selection across batch change so user can keep pressing ←→
    QSet<uint64_t> savedSel = m_selIds;

    bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QString("Change type of %1 nodes").arg(idSet.size()));
    for (uint64_t id : ordered) {
        int idx = m_doc->tree.indexOfId(id);
        if (idx >= 0) changeNodeKind(idx, newKind);
    }
    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;

    // Restore selection (node IDs are preserved across kind changes)
    m_selIds = savedSel;
    if (!m_suppressRefresh) refresh();
}

// ── Selection-level operations (ribbon / key / menu shared) ──

QVector<uint64_t> RcxController::orderedSelectedIds(unsigned flags) const {
    const auto& tree = m_doc->tree;
    QSet<uint64_t> seen;
    QVector<uint64_t> out;
    for (uint64_t sid : m_selIds) {
        const SelKind sk = selKindOf(sid);
        if (sk == SelKind::Member) continue;
        if (sk == SelKind::Footer && !(flags & SF_FooterAsContainer)) continue;
        if (sk == SelKind::ArrayElem && !(flags & SF_ArrayElemAsArray)) continue;
        const uint64_t nid = baseNodeIdFromSelId(sid);
        const int idx = tree.indexOfId(nid);
        if (idx < 0 || seen.contains(nid)) continue;
        const Node& n = tree.nodes[idx];
        if ((flags & SF_SkipRoots) && n.parentId == 0) continue;
        if ((flags & SF_SkipContainers) && isContainerKind(n.kind)) continue;
        seen.insert(nid);
        out.append(nid);
    }
    // QSet order is hash order — sort so every consumer mutates in a
    // deterministic, user-visible top-to-bottom sequence.
    std::sort(out.begin(), out.end(), [&tree](uint64_t a, uint64_t b) {
        const int64_t oa = tree.computeOffset(tree.indexOfId(a));
        const int64_t ob = tree.computeOffset(tree.indexOfId(b));
        return oa != ob ? oa < ob : a < b;
    });
    return out;
}

void RcxController::quickTypeChangeSingle(int nodeIdx, NodeKind targetKind) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;
    const auto& node = m_doc->tree.nodes[nodeIdx];
    if (isHexNode(targetKind) && isHexNode(node.kind)) {
        int curSz = sizeForKind(node.kind);
        int tgtSz = sizeForKind(targetKind);
        if (tgtSz < curSz) {
            // Shrink: changeNodeKind inserts hex padding for freed bytes.
            // These padding nodes can be joined back later (reversible cycle).
            changeNodeKind(nodeIdx, targetKind);
        } else if (tgtSz > curSz) {
            // Grow: consume adjacent hex nodes to fill the target size.
            joinHexNodes(node.id, targetKind);
        }
    } else {
        changeNodeKind(nodeIdx, targetKind);
    }
    // Re-emit so status bar updates with new type. A join removes nodes,
    // so the index may have gone out of range — re-resolve by id only
    // while it is still a valid slot.
    if (nodeIdx >= m_doc->tree.nodes.size()) return;
    nodeIdx = m_doc->tree.indexOfId(m_doc->tree.nodes[nodeIdx].id);
    if (nodeIdx >= 0) emit nodeSelected(nodeIdx);
}

void RcxController::applyQuickTypeChange(int nodeIdx, NodeKind targetKind) {
    if (nodeIdx < 0 || nodeIdx >= m_doc->tree.nodes.size()) return;

    // Apply to ALL selected nodes when multi-selected
    if (m_selIds.size() > 1) {
        QVector<int> indices;
        for (uint64_t sid : m_selIds) {
            uint64_t nid = baseNodeIdFromSelId(sid);
            int ni = m_doc->tree.indexOfId(nid);
            if (ni >= 0) indices.append(ni);
        }
        if (indices.size() > 1) {
            batchChangeKind(indices, targetKind);
            int ni = m_doc->tree.indexOfId(baseNodeIdFromSelId(*m_selIds.begin()));
            if (ni >= 0) emit nodeSelected(ni);
            return;
        }
    }

    quickTypeChangeSingle(nodeIdx, targetKind);
}

// ── Retype an exact byte RANGE ──
//
// A byte selection says which BYTES you mean, not which fields happen to
// contain them. Converting the covering fields instead is what produced the
// mess this exists to fix: select three bytes inside a hex64, ask for hex8,
// and you got ONE hex8 plus a seven-byte pad ladder, because the whole field
// was retyped and the leftovers filled in. Select seven hex64s and ask for
// bool and you got seven bools and twenty-one pad nodes.
//
// What you asked for is the bytes: N nodes of `kind` laid end to end across
// exactly the selected range. The fields the range cuts through are rebuilt
// around it — whatever of them falls outside the selection comes back as
// padding, so no byte is lost and nothing after the range moves.
//
// Deliberately NOT an array: `kind[N]` is one node with one type, and the
// point of doing this is to look at the bytes individually.
bool RcxController::retypeByteRange(uint64_t selLo, uint64_t selHi, NodeKind kind) {
    auto& tree = m_doc->tree;
    if (selHi <= selLo) return false;
    if (isContainerKind(kind) || isStringKind(kind)) return false;
    // Machine code is a window over the whole range, not one node per
    // element — see mergeToCodeWindow for the same reasoning on rows.
    const bool asWindow = isCodeKind(kind);
    const int elemSz = asWindow ? 1 : sizeForKind(kind);
    if (elemSz <= 0) return false;

    const uint64_t base = baseAddress();
    if (selLo < base) {
        emit statusHint(QStringLiteral("Selection starts before the base address"));
        return false;
    }
    const int relLo = static_cast<int>(selLo - base);
    const int relHi = static_cast<int>(selHi - base);

    // Every leaf the range touches, fully or partially. Containers are
    // refused: rebuilding one means deciding what happens to its children,
    // which is Carve's job, not this one.
    uint64_t parentId = 0;
    bool parentSet = false;
    QVector<uint64_t> covered;
    int coveredLo = INT_MAX, coveredHi = INT_MIN;
    for (int i = 0; i < tree.nodes.size(); ++i) {
        const Node& n = tree.nodes[i];
        if (n.parentId == 0) continue;                  // root containers
        const int lo = n.offset;
        const int hi = n.offset + n.byteSize();
        if (hi <= relLo || lo >= relHi) continue;        // no overlap
        if (isContainerKind(n.kind)) {
            emit statusHint(QStringLiteral(
                "Selection crosses a struct or array — use Carve for that."));
            return false;
        }
        if (!parentSet) { parentId = n.parentId; parentSet = true; }
        else if (n.parentId != parentId) {
            emit statusHint(QStringLiteral("Selection crosses a class boundary."));
            return false;
        }
        covered.append(n.id);
        coveredLo = qMin(coveredLo, lo);
        coveredHi = qMax(coveredHi, hi);
    }
    if (covered.isEmpty()) {
        emit statusHint(QStringLiteral("No fields in the selection"));
        return false;
    }

    // How many whole elements fit. A 1-byte type always divides; a 7-byte
    // range asked to hold int32s gets one, and the odd 3 bytes come back as
    // padding rather than being silently rounded away.
    const int rangeLen = relHi - relLo;
    const int count = rangeLen / elemSz;
    if (count < 1) {
        emit statusHint(QStringLiteral("%1 needs %2 bytes; only %3 selected")
                            .arg(QString::fromLatin1(kindMeta(kind)->typeName))
                            .arg(elemSz).arg(rangeLen));
        return false;
    }

    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(
        QStringLiteral("Change %1 bytes to %2")
            .arg(rangeLen).arg(QString::fromLatin1(kindMeta(kind)->typeName)));

    // Out with the fields the range touched (reverse, so indices stay valid).
    std::sort(covered.begin(), covered.end(), [&tree](uint64_t a, uint64_t b) {
        const int ia = tree.indexOfId(a), ib = tree.indexOfId(b);
        return (ia >= 0 ? tree.nodes[ia].offset : 0) < (ib >= 0 ? tree.nodes[ib].offset : 0);
    });
    for (int j = covered.size() - 1; j >= 0; --j) {
        const int idx = tree.indexOfId(covered[j]);
        if (idx < 0) continue;
        QVector<Node> subtree{ tree.nodes[idx] };
        m_doc->undoStack.push(new RcxCommand(this, cmd::Remove{covered[j], subtree, {}}));
    }

    // Largest-first hex, the same ladder the shrink path lays down.
    auto pad = [this, parentId](int from, int to) {
        while (from < to) {
            const int gap = to - from;
            NodeKind k; int sz;
            if      (gap >= 8) { k = NodeKind::Hex64; sz = 8; }
            else if (gap >= 4) { k = NodeKind::Hex32; sz = 4; }
            else if (gap >= 2) { k = NodeKind::Hex16; sz = 2; }
            else               { k = NodeKind::Hex8;  sz = 1; }
            insertNode(parentId, from, k,
                       QStringLiteral("pad_%1").arg(from, 2, 16, QChar('0')));
            from += sz;
        }
    };

    pad(coveredLo, relLo);                       // before the selection
    if (asWindow) {
        Node code;
        code.kind      = kind;
        code.name      = QStringLiteral("field_%1").arg(relLo, 4, 16, QChar('0'));
        code.parentId  = parentId;
        code.offset    = relLo;
        code.arrayLen  = rangeLen;   // the declared window
        code.collapsed = false;      // a collapsed code node shows nothing
        code.id        = tree.reserveId();
        m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{code, {}}));
    } else {
        for (int i = 0; i < count; ++i) {
            const int off = relLo + i * elemSz;
            insertNode(parentId, off, kind,
                       QStringLiteral("field_%1").arg(off, 4, 16, QChar('0')));
        }
    }
    pad(relLo + count * elemSz, coveredHi);      // the odd tail, and after

    m_doc->undoStack.endMacro();
    m_suppressRefresh = false;

    // The old rows are gone, so any selection pointing at them is stale.
    m_selIds.clear();
    for (int i = 0; i < tree.nodes.size(); ++i) {
        const Node& n = tree.nodes[i];
        if (n.parentId == parentId && n.kind == kind
            && n.offset >= relLo && n.offset < relLo + count * elemSz)
            m_selIds.insert(n.id);
    }
    refresh();
    emit selectionChanged(m_selIds.size());
    return true;
}

void RcxController::retypeSelection(NodeKind kind) {
    // A byte selection is the more specific instruction: it names the BYTES,
    // so the type lands on exactly those and the fields around them are
    // rebuilt. Without this the covering ROWS were retyped instead, which is
    // why three selected bytes produced one field plus a pad ladder.
    // Strings and containers have no byte-range meaning (a string's footprint
    // is strLen, a container's is its children), so those fall through to the
    // row path unchanged.
    const bool rangeApplies = !isContainerKind(kind) && !isStringKind(kind);
    if (rangeApplies) {
        for (RcxEditor* ed : m_editors) {
            if (!ed || !ed->hasByteSelection()) continue;
            const auto r = ed->byteSelectionRange();
            if (retypeByteRange(r.first, r.second, kind))
                ed->clearByteSelection();   // those bytes are fields now
            // Either way this was the instruction: a refusal has already
            // explained itself, and falling through to the row path would do
            // something different from what was asked.
            return;
        }
    }

    const QVector<uint64_t> ids = orderedSelectedIds(SF_SkipRoots | SF_SkipContainers);
    if (ids.isEmpty()) {
        emit statusHint(QStringLiteral("Select a field to change its type"));
        return;
    }
    if (ids.size() == 1) {
        quickTypeChangeSingle(m_doc->tree.indexOfId(ids.first()), kind);
        return;
    }
    QVector<int> indices;
    for (uint64_t id : ids) {
        int idx = m_doc->tree.indexOfId(id);
        if (idx >= 0) indices.append(idx);
    }
    batchChangeKind(indices, kind);
    int ni = m_doc->tree.indexOfId(ids.first());
    if (ni >= 0) emit nodeSelected(ni);
}

void RcxController::appendBytes(uint64_t targetId, int byteCount) {
    if (byteCount <= 0) return;
    if (targetId == 0) {
        // Ribbon / menu callers don't know the target: the viewed class,
        // else the first root struct of the document.
        targetId = m_viewRootId;
        if (targetId == 0 || m_doc->tree.indexOfId(targetId) < 0) {
            targetId = 0;
            for (const auto& n : m_doc->tree.nodes)
                if (n.kind == NodeKind::Struct && n.parentId == 0) { targetId = n.id; break; }
        }
    }
    int si = m_doc->tree.indexOfId(targetId);
    if (si < 0) return;

    // Enums and bitfields hold MEMBERS, not raw bytes: a Hex64 child under
    // an enum / bitfield would render as garbage and never round-trip. The
    // footer "+1 / +10" pills grow those via the enum-member path instead.
    auto takesMembersNotBytes = [](const Node& n) { return n.isEnum() || n.isBitfield(); };
    auto refuse = [this](const Node& n) {
        emit statusHint(QStringLiteral("Append: %1 takes members, not raw bytes")
                            .arg(n.isEnum() ? QStringLiteral("an enum")
                                            : QStringLiteral("a bitfield")));
    };
    if (takesMembersNotBytes(m_doc->tree.nodes[si])) { refuse(m_doc->tree.nodes[si]); return; }

    // Array: grow arrayLen by enough elements to cover `byteCount`
    // (rounded up). Inserting raw Hex64/Hex8 children here would
    // produce the same out-of-sync header bug as the +1 path.
    if (m_doc->tree.nodes[si].kind == NodeKind::Array) {
        const Node& arr = m_doc->tree.nodes[si];
        int elemSize = qMax(1, sizeForKind(arr.elementKind));
        int addCount = (byteCount + elemSize - 1) / elemSize;
        if (addCount <= 0) return;
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeArrayMeta{arr.id,
                arr.elementKind, arr.elementKind,
                arr.arrayLen, arr.arrayLen + addCount}));
        return;
    }

    // Struct path: append raw Hex64 + Hex8 padding. If the struct
    // is an embedded reference (refId != 0), redirect to the
    // referenced root class so the change persists across uses.
    uint64_t appendTo = targetId;
    if (m_doc->tree.childrenOf(targetId).isEmpty()
        && m_doc->tree.nodes[si].refId != 0)
        appendTo = m_doc->tree.nodes[si].refId;
    {
        // The redirect target can be an enum class too (typed enum field).
        const int ti = m_doc->tree.indexOfId(appendTo);
        if (ti < 0) return;
        if (takesMembersNotBytes(m_doc->tree.nodes[ti])) { refuse(m_doc->tree.nodes[ti]); return; }
    }
    int hex64Count = byteCount / 8;
    int remainBytes = byteCount % 8;
    const bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Append %1 bytes").arg(byteCount));
    for (int i = 0; i < hex64Count; i++)
        insertNode(appendTo, -1, NodeKind::Hex64,
                   QStringLiteral("field_%1").arg(i));
    for (int i = 0; i < remainBytes; i++)
        insertNode(appendTo, -1, NodeKind::Hex8,
                   QStringLiteral("field_%1").arg(hex64Count + i));
    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();
}

void RcxController::insertBytesAbove(uint64_t anchorNodeId, int byteCount) {
    if (byteCount <= 0) return;
    const int ai = m_doc->tree.indexOfId(anchorNodeId);
    if (ai < 0) return;
    if (m_doc->tree.nodes[ai].parentId == 0) {
        // A root class has no siblings to shift — nothing sensible to
        // insert "above"; the ribbon routes footer-only selections to
        // appendBytes instead.
        emit statusHint(QStringLiteral("Insert: select a field first"));
        return;
    }
    const Node& anchor = m_doc->tree.nodes[ai];
    const QString anchorName = anchor.name.isEmpty()
        ? QStringLiteral("field_%1").arg(anchor.offset, 4, 16, QChar('0'))
        : anchor.name;
    const int hex64Count = byteCount / 8;
    const int remainBytes = byteCount % 8;

    const bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Insert %1 bytes above %2")
                                    .arg(byteCount).arg(anchorName));
    // Each block lands at the anchor's CURRENT offset: insertNodeAbove shifts
    // the anchor (and everything after it) down by the block size, so the
    // blocks come out ascending and contiguous. The anchor index is
    // re-resolved every step because each Insert changes the node order.
    auto insertBlock = [this, anchorNodeId](NodeKind kind) -> bool {
        const int idx = m_doc->tree.indexOfId(anchorNodeId);
        if (idx < 0) return false;
        const int off = m_doc->tree.nodes[idx].offset;
        insertNodeAbove(idx, kind,
                        QStringLiteral("field_%1").arg(off, 4, 16, QChar('0')));
        return true;
    };
    for (int i = 0; i < hex64Count; ++i)
        if (!insertBlock(NodeKind::Hex64)) break;
    for (int i = 0; i < remainBytes; ++i)
        if (!insertBlock(NodeKind::Hex8)) break;
    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();
}

void RcxController::deleteSelection() {
    // Member rows never delete anything; an array-element row stands for
    // its whole Array and a footer `}` row for its container — exactly what
    // the Delete key did before the ribbon existed (it decoded every selId
    // to its base node). The footer is the only selectable row a root class
    // composes (its header is the command row).
    const QVector<uint64_t> ids = orderedSelectedIds(SF_ArrayElemAsArray | SF_FooterAsContainer);
    if (ids.isEmpty()) {
        emit statusHint(QStringLiteral("Delete: select a field first"));
        return;
    }
    // A lone selected root struct: the Delete key removed it before the
    // ribbon existed (plain removeNode); keep that reachable but take the
    // dedicated path, which also clears every refId that points at the
    // class and re-targets the view root — a plain Remove would leave
    // typed pointers dangling.
    if (ids.size() == 1) {
        const int idx = m_doc->tree.indexOfId(ids.first());
        if (idx >= 0 && m_doc->tree.nodes[idx].parentId == 0) {
            if (m_doc->tree.nodes[idx].kind == NodeKind::Struct)
                deleteRootStruct(ids.first());
            else
                removeNode(idx);
            return;
        }
    }
    QVector<int> indices;
    for (uint64_t id : ids) {
        int idx = m_doc->tree.indexOfId(id);
        if (idx >= 0) indices.append(idx);
    }
    if (indices.size() > 1)
        batchRemoveNodes(indices);
    else if (indices.size() == 1)
        removeNode(indices.first());
}

void RcxController::duplicateSelection() {
    const QVector<uint64_t> ids = orderedSelectedIds(SF_SkipRoots | SF_SkipContainers);
    if (ids.isEmpty()) {
        emit statusHint(QStringLiteral("Duplicate: select a field (classes and arrays can't be duplicated)"));
        return;
    }
    if (ids.size() == 1) {
        duplicateNode(m_doc->tree.indexOfId(ids.first()));
        return;
    }
    // Ascending offset: each duplicate lands right after its source and
    // shifts the rest down, so the copies interleave (a, a_copy, b, b_copy).
    const bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Duplicate %1 nodes").arg(ids.size()));
    for (uint64_t id : ids) {
        int idx = m_doc->tree.indexOfId(id);
        if (idx >= 0) duplicateNode(idx);
    }
    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();
}

bool RcxController::fillSelectionBytes(ByteFill fill) {
    if (!this->provider()) return false;
    if (!this->provider()->isWritable() || m_readOnlyOverride || isViewingPast()) {
        emit statusHint(isViewingPast() ? viewingPastHint() : QStringLiteral("Target is read-only"));
        return false;
    }
    auto region = regionFromCurrentSelection(primaryEditor());
    if (!region) {
        emit statusHint(QStringLiteral("Fill: select bytes or fields first"));
        return false;
    }
    const uint64_t lo = region->first;
    const int n = static_cast<int>(region->second - region->first);
    if (n <= 0 || n > 65536) {
        if (n > 65536)
            emit statusHint(QStringLiteral("Fill: selection too large (max 64 KiB)"));
        return false;
    }
    QByteArray oldBytes = this->provider()->isReadable(lo, n)
        ? this->provider()->readBytes(lo, n)
        : QByteArray(n, '\0');
    QByteArray newBytes(n, fill == ByteFill::FF ? char(0xFF) : '\0');
    if (fill == ByteFill::Random) {
        auto* rng = QRandomGenerator::global();
        for (int i = 0; i < n; ++i)
            newBytes[i] = static_cast<char>(rng->bounded(256));
    }
    // User edit — exclude from value history (see m_userEditRanges).
    m_userEditRanges.append({lo, lo + static_cast<uint64_t>(n)});
    m_doc->undoStack.push(new RcxCommand(this,
        cmd::WriteBytes{lo, oldBytes, newBytes}));
    const char* what = fill == ByteFill::Zero ? "Zero-filled"
                     : fill == ByteFill::FF   ? "FF-filled" : "Randomized";
    emit statusHint(QStringLiteral("%1 %2 byte%3 at 0x%4")
        .arg(QLatin1String(what)).arg(n).arg(n == 1 ? "" : "s").arg(lo, 0, 16));
    return true;
}

void RcxController::toggleBigEndianSelection() {
    // isEndianSwappable (controller.h) is the single source of truth shared
    // with the context menu's "Big endian" item and the ribbon predicate.
    QVector<uint64_t> ids;
    for (uint64_t id : orderedSelectedIds(SF_SkipContainers)) {
        int idx = m_doc->tree.indexOfId(id);
        if (idx >= 0 && isEndianSwappable(m_doc->tree.nodes[idx].kind)) ids.append(id);
    }
    if (ids.isEmpty()) {
        emit statusHint(QStringLiteral("Swap: select a 16/32/64-bit field first"));
        return;
    }
    const bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    if (ids.size() > 1)
        m_doc->undoStack.beginMacro(QStringLiteral("Toggle big endian on %1 nodes").arg(ids.size()));
    for (uint64_t id : ids) {
        int idx = m_doc->tree.indexOfId(id);
        if (idx < 0) continue;
        const bool cur = m_doc->tree.nodes[idx].bigEndian;
        m_doc->undoStack.push(new RcxCommand(this, cmd::ToggleBigEndian{id, cur, !cur}));
    }
    if (ids.size() > 1)
        m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();
}

bool RcxController::makeArrayFromSelection() {
    const QVector<uint64_t> ids = orderedSelectedIds(SF_SkipRoots | SF_SkipContainers);
    if (ids.isEmpty()) {
        emit statusHint(QStringLiteral("Array: select a field first"));
        return false;
    }
    auto& tree = m_doc->tree;

    // Validate the shape: every node the same non-string kind, the same
    // parent, and byte-contiguous in offset order (strings have a strLen
    // footprint that sizeForKind can't express as an element).
    const int firstIdx = tree.indexOfId(ids.first());
    const NodeKind kind = tree.nodes[firstIdx].kind;
    const uint64_t parentId = tree.nodes[firstIdx].parentId;
    if (isStringKind(kind)) {
        emit statusHint(QStringLiteral("Array: strings can't be array elements"));
        return false;
    }
    int lastEnd = tree.nodes[firstIdx].offset;
    for (uint64_t id : ids) {
        const Node& n = tree.nodes[tree.indexOfId(id)];
        if (n.kind != kind || n.parentId != parentId || n.offset != lastEnd) {
            emit statusHint(QStringLiteral(
                "Array: select one field, or contiguous fields of the same type"));
            return false;
        }
        lastEnd = n.offset + n.byteSize();
    }

    const bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    if (ids.size() == 1) {
        // One leaf → Array[1] of its own kind, same footprint.
        m_doc->undoStack.beginMacro(QStringLiteral("Change to array"));
    } else {
        // N leaves → drop 2..N (raw Remove, no offset shift — the array
        // re-occupies exactly their bytes, like joinHexNodes) and retype
        // the first to Array[N].
        m_doc->undoStack.beginMacro(QStringLiteral("Group %1 into array").arg(ids.size()));
        for (int j = ids.size() - 1; j >= 1; --j) {
            int idx = tree.indexOfId(ids[j]);
            if (idx < 0) continue;
            QVector<Node> subtree;
            subtree.append(tree.nodes[idx]);
            m_doc->undoStack.push(new RcxCommand(this,
                cmd::Remove{ids[j], subtree, {}}));
        }
    }
    int idx = tree.indexOfId(ids.first());
    if (idx >= 0) changeNodeKind(idx, NodeKind::Array);
    idx = tree.indexOfId(ids.first());
    if (idx >= 0) {
        const Node& n = tree.nodes[idx];
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeArrayMeta{ids.first(), n.elementKind, kind,
                                 n.arrayLen, static_cast<int>(ids.size())}));
    }
    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();
    return true;
}

void RcxController::convertSelectionToTypedPointers() {
    QVector<uint64_t> eligible;
    for (uint64_t id : orderedSelectedIds(SF_SkipRoots | SF_SkipContainers)) {
        const Node& n = m_doc->tree.nodes[m_doc->tree.indexOfId(id)];
        const int sz = n.byteSize();
        if ((sz == 4 || sz == 8) && n.refId == 0) eligible.append(id);
    }
    if (eligible.isEmpty()) {
        emit statusHint(QStringLiteral("Ptr→Class: select a 4- or 8-byte field first"));
        return;
    }
    if (eligible.size() == 1) {
        convertToTypedPointer(eligible.first());
        return;
    }
    const bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Change %1 fields to ptr*").arg(eligible.size()));
    for (uint64_t id : eligible)
        convertToTypedPointer(id);   // nest-safe: honours the suppress flag
    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    if (!m_suppressRefresh) refresh();
}

void RcxController::commentSelection(RcxEditor* editor) {
    if (!m_showComments) return;
    if (!editor) editor = primaryEditor();
    QSet<uint64_t> ids = m_selIds;
    // Strip footer/array/member bits to get real node IDs
    QSet<uint64_t> nodeIds;
    for (uint64_t id : ids) {
        uint64_t nid = baseNodeIdFromSelId(id);
        if (m_doc->tree.indexOfId(nid) >= 0)
            nodeIds.insert(nid);
    }

    if (nodeIds.size() <= 1) {
        if (!editor) return;
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
}

// Project a range's endpoints to siblings in the displayed tree. Using the
// display ancestry also handles virtual pointer children and array elements.
// `anchors`, when given, receives where each selected id was taken from — the
// rows inside the range, so a node shown elsewhere too is marked here.
static QSet<uint64_t> nodeRangeSelection(const QVector<LineMeta>& meta,
                                       RcxEditor* source, int anchor, int target,
                                       QHash<uint64_t, SelectionAnchor>* anchors = nullptr) {
    auto selectable = [&](int line) {
        const auto& lm = meta[line];
        return lm.nodeId != 0 && lm.nodeId != kCommandRowId && !lm.isContinuation
            && (lm.lineKind == LineKind::Field || lm.lineKind == LineKind::Header)
            && (!source || source->scintilla()->SendScintilla(
                QsciScintillaBase::SCI_GETLINEVISIBLE, (unsigned long)line));
    };
    auto pathTo = [&](int line) {
        QVector<int> path;
        if (line < 0 || line >= meta.size()) return path;
        if (meta[line].lineKind == LineKind::Footer) {
            for (int i = line - 1; i >= 0; --i) {
                if (meta[i].nodeId == meta[line].nodeId && selectable(i)) {
                    line = i;
                    break;
                }
            }
        }
        while (line >= 0 && !selectable(line)) --line;
        if (line < 0) return path;
        path.append(line);
        int depth = meta[line].depth;
        for (int i = line - 1; i >= 0; --i) {
            if (meta[i].depth < depth && meta[i].foldHead && selectable(i)) {
                path.prepend(i);
                depth = meta[i].depth;
            }
        }
        return path;
    };
    const auto a = pathTo(anchor);
    const auto b = pathTo(target);
    if (a.isEmpty() || b.isEmpty()) return {};
    int common = 0;
    while (common < a.size() && common < b.size() && a[common] == b[common])
        ++common;
    if (common == a.size() || common == b.size()) {
        const int parent = common == a.size() ? a.back() : b.back();
        if (anchors) anchors->insert(selIdForLine(meta[parent]), anchorForLine(meta, parent));
        return {selIdForLine(meta[parent])};
    }
    const int from = qMin(a[common], b[common]);
    const int to = qMax(a[common], b[common]);
    QSet<uint64_t> selected;
    for (int i = from; i <= to; ++i) {
        if (selectable(i) && meta[i].depth == meta[from].depth) {
            const uint64_t sel = selIdForLine(meta[i]);
            if (anchors && !selected.contains(sel)) anchors->insert(sel, anchorForLine(meta, i));
            selected.insert(sel);
        }
    }
    return selected;
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
        return selIdForLine(m_lastResult.meta[ln]);
    };

    // Escape / deselect: nodeId=0 means clear selection
    if (nodeId == 0) {
        clearSelection();
        return;
    }

    // Reject a STALE click whose node was already deleted. A click carries
    // (line, nodeId) captured at click time; a deferred/queued nodeClicked
    // can fire AFTER a delete shifted the rows (the clicked node is one of
    // the deleted ones). effectiveId() derives the selection from the row at
    // `line`, so such a stale click would otherwise select whatever node
    // shifted up into that line — the reported "delete auto-selects the node
    // now in that position" bug. Existing nodes fall through unchanged.
    if (m_doc->tree.indexOfId(nodeId) < 0) return;

    uint64_t selId = effectiveId(line, nodeId);
    // Where the click landed: a node shown in several places is marked only here.
    const SelectionAnchor here = anchorForLine(m_lastResult.meta, line);

    if (!ctrl && !shift) {
        m_selIds.clear();
        m_selIds.insert(selId);
        m_selAnchors.clear();
        m_selAnchors.insert(selId, here);
        m_anchorLine = line;
    } else if (ctrl && !shift) {
        // Ctrl+click on the marked row unselects it; on another place the
        // same node is shown, the mark moves there.
        if (m_selIds.contains(selId) && m_selAnchors.value(selId, here) == here) {
            m_selIds.remove(selId);
            m_selAnchors.remove(selId);
        } else {
            m_selIds.insert(selId);
            m_selAnchors.insert(selId, here);
        }
        m_anchorLine = line;
    } else if (shift && !ctrl) {
        if (m_anchorLine < 0) {
            m_selIds.clear();
            m_selIds.insert(selId);
            m_selAnchors.clear();
            m_selAnchors.insert(selId, here);
            m_anchorLine = line;
        } else {
            m_selAnchors.clear();
            m_selIds = nodeRangeSelection(m_lastResult.meta, source, m_anchorLine, line, &m_selAnchors);
        }
    } else { // Ctrl+Shift
        if (m_anchorLine < 0) {
            m_selIds.insert(selId);
            m_selAnchors.insert(selId, here);
            m_anchorLine = line;
        } else {
            QHash<uint64_t, SelectionAnchor> ranged;
            m_selIds.unite(nodeRangeSelection(m_lastResult.meta, source, m_anchorLine, line, &ranged));
            m_selAnchors.insert(ranged);
        }
    }

    updateCommandRow();
    applySelectionOverlays();

    // The trail follows the selection: the chain of expanded typed pointers
    // containing the clicked node. Selecting inside a NewClass* expansion adds
    // it; a top-level row clears back to the root crumb. Pushed only when the
    // scope actually changed — most clicks land within the same path (the
    // bar's setState would early-return on the equal state anyway).
    QVector<uint64_t> newFocus = focusChainToNode(nodeId);
    if (newFocus != m_focusPath) {
        m_focusPath = std::move(newFocus);
        pushAddressBarState();
    }

    if (m_selIds.size() == 1) {
        uint64_t sid = *m_selIds.begin();
        // Strip footer/array/member bits for node lookup
        int idx = m_doc->tree.indexOfId(baseNodeIdFromSelId(sid));
        if (idx >= 0) emit nodeSelected(idx);
    }
}

void RcxController::clearSelection() {
    m_selIds.clear();
    m_selAnchors.clear();
    m_anchorLine = -1;
    bool hadFocus = !m_focusPath.isEmpty();
    m_focusPath.clear();   // the trail back to the bare root crumb
    updateCommandRow();
    applySelectionOverlays();
    if (hadFocus) pushAddressBarState();
}

void RcxController::applySelectionOverlays() {
    // Anchors of ids no longer selected (a delete, an undo, a replaced set)
    // would only mislead the next time that id is selected some other way.
    for (auto it = m_selAnchors.begin(); it != m_selAnchors.end();)
        it = m_selIds.contains(it.key()) ? std::next(it) : m_selAnchors.erase(it);
    for (auto* editor : m_editors)
        editor->applySelectionOverlay(m_selIds, m_selAnchors);
}


void RcxController::updateCommandRow() {
    // Line 0 is the class header alone: "[▸] struct Name {". The source
    // control and the base address both moved to the address bar (the
    // provider's name, liveness, the chooser popup, the base edit and its
    // formula display all live there; pushAddressBarState carries them).

    // Root class keyword + name (uses current view root)
    QString keyword, className;
    bool haveRoot = false;
    auto takeRoot = [&](const Node& n) {
        keyword = n.resolvedClassKeyword();
        className = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
        if (className.isEmpty()) className = QStringLiteral("Untitled");
        haveRoot = true;
    };
    if (m_viewRootId != 0) {
        int vi = m_doc->tree.indexOfId(m_viewRootId);
        if (vi >= 0) takeRoot(m_doc->tree.nodes[vi]);
    }
    if (!haveRoot) {
        // Fallback: find first root struct
        for (const auto& n : m_doc->tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct) { takeRoot(n); break; }
        }
    }
    if (!haveRoot) {
        // No struct nodes at all in the tree → blank project. Show
        // "Untitled" instead of the old "NoName" placeholder so the
        // command row doesn't look like a programmer-grade default
        // leaked into the UI.
        keyword = QStringLiteral("struct");
        className = QStringLiteral("Untitled");
    }

    const QString combined = buildCommandRowText(keyword, className, m_braceWrap);
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

SourceChooserPopup* RcxController::ensureSourcePopup(RcxEditor* editor) {
    if (!m_cachedSourcePopup) {
        m_cachedSourcePopup = new SourceChooserPopup(editor);
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                m_cachedSourcePopup, &SourceChooserPopup::applyTheme);
        m_cachedSourcePopup->warmUp();
    }
    m_cachedSourcePopup->disconnect(this);
    return m_cachedSourcePopup;
}

void RcxController::showSourcePopup(RcxEditor* editor, QPoint globalPos) {
    // Toggle: dismiss if already visible
    if (m_cachedSourcePopup && m_cachedSourcePopup->isVisible()) {
        m_cachedSourcePopup->hide();
        return;
    }
    auto* popup = ensureSourcePopup(editor);

    // Build entries from saved sources + registered providers. Wrapped in a
    // lambda so the per-source delete can rebuild the list in place without
    // closing the popup.
    auto buildEntries = [this]() -> QVector<SourceEntry> {
    QVector<SourceEntry> entries;

    // Section: bound sources (if any)
    if (!m_savedSources.isEmpty()) {
        SourceEntry hdr;
        hdr.entryKind = SourceEntry::SectionHeader;
        hdr.displayName = QStringLiteral("Connected");
        hdr.enabled = false;
        entries.append(hdr);

        for (int i = 0; i < m_savedSources.size(); i++) {
            const auto& ss = m_savedSources[i];
            SourceEntry e;
            e.entryKind = SourceEntry::SavedSource;
            e.displayName = ss.displayName;
            e.providerIdentifier = ss.kind;
            e.providerTarget = ss.providerTarget;
            e.filePath = ss.filePath;
            e.savedIndex = i;
            e.isActive = (i == m_activeSourceIdx);
            e.kindLabel = kindLabelFor(ss.kind);
            e.iconPath = iconForProvider(ss.kind);

            // Extract PID from providerTarget "1234:processname"
            if (!ss.providerTarget.isEmpty() && ss.providerTarget.contains(':'))
                e.pid = ss.providerTarget.section(':', 0, 0);

            // Base address
            if (!ss.baseAddressFormula.isEmpty())
                e.baseAddress = ss.baseAddressFormula;
            else if (ss.baseAddress != 0)
                e.baseAddress = QStringLiteral("0x") +
                    QString::number(ss.baseAddress, 16).toUpper();

            // Architecture (from active provider if this is the active source)
            if (e.isActive && this->provider())
                e.arch = (m_doc->tree.pointerSize >= 8)
                    ? QStringLiteral("x64") : QStringLiteral("x86");

            entries.append(e);
        }
    }

    // Section: available providers
    {
        SourceEntry hdr;
        hdr.entryKind = SourceEntry::SectionHeader;
        hdr.displayName = QStringLiteral("Add Source");
        hdr.enabled = false;
        entries.append(hdr);

        // File provider (always available — built into the binary, no
        // plugin DLL). Set kindLabel + a "built-in" subtext so the row
        // has the same two-line shape as plugin-loaded providers
        // (Process / Kernel / etc) below it.
        {
            SourceEntry e;
            e.entryKind = SourceEntry::ProviderAction;
            e.displayName = QStringLiteral("Open File");
            e.providerIdentifier = QStringLiteral("File");
            e.kindLabel = kindLabelFor(QStringLiteral("File"));
            e.dllFileName = QStringLiteral("built-in");
            e.iconPath = iconForProvider(QStringLiteral("File"));
            entries.append(e);
        }

        // Registered plugin providers
        const auto& providers = ProviderRegistry::instance().providers();
        for (const auto& prov : providers) {
            SourceEntry e;
            e.entryKind = SourceEntry::ProviderAction;
            e.displayName = prov.name;
            e.providerIdentifier = prov.name;
            e.dllFileName = prov.dllFileName;
            e.kindLabel = kindLabelFor(prov.identifier);
            e.iconPath = iconForProvider(prov.identifier);
            entries.append(e);
        }
    }

    // Clear All action — always present so the user has a visible
    // "reset" affordance even before any sources are saved.
    {
        SourceEntry e;
        e.entryKind = SourceEntry::ClearAction;
        e.displayName = QStringLiteral("Clear All");
        e.iconPath = QStringLiteral(":/vsicons/clear-all.svg");
        e.enabled = !m_savedSources.isEmpty();
        entries.append(e);
    }

    return entries;
    };

    // Configure and show popup
    QSettings settings("RC", "RC");
    QString fontName = settings.value("font", "JetBrains Mono").toString();
    QFont font(fontName, 12);
    font.setFixedPitch(true);
    auto* sci = editor->scintilla();
    int zoom = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETZOOM);
    font.setPointSize(font.pointSize() + zoom);

    font.setPointSize(font.pointSize() - 1);  // slightly smaller than editor
    popup->setFont(font);
    popup->applyTheme(ThemeManager::instance().current());
    popup->setSources(buildEntries());

    connect(popup, &SourceChooserPopup::sourceSelected,
            this, [this](int idx) { switchToSavedSource(idx); });
    connect(popup, &SourceChooserPopup::providerSelected,
            this, [this](const QString& id) { selectSource(id); });
    connect(popup, &SourceChooserPopup::removeRequested,
            this, [this, popup, buildEntries](int savedIdx) {
                removeSavedSource(savedIdx);
                popup->setSources(buildEntries());  // refresh in place, stay open
            });
    connect(popup, &SourceChooserPopup::clearRequested,
            this, [this]() { clearSources(); });

    // The bar's top edge goes along with the anchor (its bottom edge under
    // the chip): a popup that has to flip above the bar then ends one device
    // row above it instead of covering the bar and the chip. QPointer: the
    // bar goes with its pane, and the popup can outlive both.
    QPointer<AddressBar> bar = editor->addressBar();
    popup->popup(globalPos, bar ? bar->mapToGlobal(QPoint(0, 0)).y() : -1);

    // The bar's chip stays t.hover while the popup is up; its hide — a
    // pick, Esc, a click elsewhere — emits dismissed(), which clears it.
    // ensureSourcePopup's disconnect(this) drops this with the rest before
    // the next open re-makes it.
    if (bar) {
        bar->setSourceMenuOpen(true);
        connect(popup, &SourceChooserPopup::dismissed, this,
                [bar]() { if (bar) bar->setSourceMenuOpen(false); });
    }

    // Deferred liveness probe for saved sources
    QTimer::singleShot(0, this, [this]() {
        if (!m_cachedSourcePopup || !m_cachedSourcePopup->isVisible()) return;
        QVector<bool> alive;
        for (int i = 0; i < m_savedSources.size(); i++) {
            const auto& ss = m_savedSources[i];
            if (i == m_activeSourceIdx) {
                // Active source: check the current provider
                alive.append(this->provider() && this->provider()->isValid());
            } else if (ss.kind == QStringLiteral("File")) {
                alive.append(QFile::exists(ss.filePath));
            } else {
                // For non-active process sources, assume alive
                // (probing would require creating a temporary provider)
                alive.append(true);
            }
        }
        if (m_cachedSourcePopup)
            m_cachedSourcePopup->setLivenessResults(alive);
    });
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
    QSettings settings("RC", "RC");
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
    popup->setRecentTypes(m_recentTypeNames);

    connect(popup, &TypeSelectorPopup::typeSelected,
            this, [this, mode, nodeIdx](const TypeEntry& entry, const QString& fullText) {
        applyTypePopupResult(mode, nodeIdx, entry, fullText);
    });
    connect(popup, &TypeSelectorPopup::createNewTypeRequested,
            this, [this, mode, nodeIdx](int modifierId, int arrayCount,
                                        const QString& name, const QString& keyword) {
        bool wasSuppressed = m_suppressRefresh;
        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Create new type"));

        // name empty → uniqueStructName() falls back to "NewClass"; a typed
        // name is de-duplicated (Foo → Foo_2) the same way. keyword carries
        // struct vs class through to the new root.
        const QString typeName = uniqueStructName(name);
        uint64_t newId = createRootStruct(typeName, keyword, 8);

        m_doc->undoStack.endMacro();
        m_suppressRefresh = wasSuppressed;

        TypeEntry newEntry;
        newEntry.entryKind = TypeEntry::Composite;
        newEntry.structId  = newId;

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
                e.kindGroup     = kindGroupFor(m.kind);
                entries.append(e);
                // For Pointer32 / Pointer64, append an RVA variant
                // immediately after so users can pick "Pointer32 (RVA)"
                // from the same list — no modifier checkbox needed.
                if (m.kind == NodeKind::Pointer32 || m.kind == NodeKind::Pointer64) {
                    TypeEntry rva = e;
                    rva.isRelative = true;
                    rva.displayName = QString::fromLatin1(m.typeName)
                                    + QStringLiteral(" (RVA)");
                    entries.append(rva);
                }
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
                int maxAlign = 1;
                for (int i = 0; i < kids.size(); i++) {
                    const Node& child = m_doc->tree.nodes[kids[i]];
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
                e.fieldCount = kids.size();
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
                // An enum-typed int reads as its enum: that composite is current.
                if (!(node->kind == NodeKind::Struct && node->refId != 0)
                    && !enumTypeOf(m_doc->tree, *node)) {
                    // For pointer kinds, the catalog now ships two
                    // entries per width (absolute + RVA). Match the
                    // variant that mirrors node->isRelative so opening
                    // the chooser on an existing RVA pointer pre-
                    // selects the "(RVA)" entry.
                    for (auto& e : entries) {
                        if (e.entryKind != TypeEntry::Primitive) continue;
                        if (e.primitiveKind != node->kind) continue;
                        if (isPtr && e.isRelative != node->isRelative) continue;
                        currentEntry = e;
                        hasCurrent = true;
                        break;
                    }
                }
            }
            addComposites([&](const Node& n, const TypeEntry& e) {
                if (isTypedPtr && n.refId == e.structId) return true;
                if (isArray && n.elementKind == NodeKind::Struct && n.refId == e.structId) return true;
                if (!isPtr && !isArray && n.kind == NodeKind::Struct && n.refId == e.structId) return true;
                if (!isPtr && !isArray && enumTypeOf(m_doc->tree, n) && n.refId == e.structId) return true;
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

        // Deduplicate by name — shared between cross-doc and common-types blocks
        QSet<QString> localNames;

        // Add types from other open documents
        if (m_projectDocs) {
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

        // Add built-in common types (reuse localNames from cross-doc block above)
        if (mode != TypePopupMode::Root) {
            if (!m_projectDocs) {
                // No cross-doc block ran — build the name set now
                for (const auto& e : entries)
                    if (e.entryKind == TypeEntry::Composite)
                        localNames.insert(e.displayName);
            }
            for (int ci = 0; ci < kCommonTypeCount; ci++) {
                const auto& ct = kCommonTypes[ci];
                QString name = QString::fromLatin1(ct.name);
                if (localNames.contains(name)) continue;
                TypeEntry e;
                e.entryKind    = TypeEntry::Composite;
                e.structId     = 0;
                e.displayName  = name;
                e.classKeyword = QString::fromLatin1(ct.classKeyword);
                e.category     = TypeEntry::CatType;
                e.sizeBytes    = ct.totalSize;
                e.fieldCount   = ct.fieldCount;
                int maxAlign = 1;
                for (int fi = 0; fi < ct.fieldCount; fi++)
                    maxAlign = qMax(maxAlign, alignmentFor(ct.fields[fi].kind));
                e.alignment    = maxAlign;
                e.kindGroup    = QStringLiteral("Common");
                for (int fi = 0; fi < qMin(ct.fieldCount, 6); fi++) {
                    const auto& f = ct.fields[fi];
                    auto* km = kindMeta(f.kind);
                    QString tn = km ? QString::fromLatin1(km->typeName) : QStringLiteral("?");
                    e.fieldSummary << QStringLiteral("0x%1: %2 %3")
                        .arg(f.offset, 2, 16, QChar('0'))
                        .arg(tn, QString::fromLatin1(f.name));
                }
                entries.append(e);
            }
        }

        popup->setTypes(entries, hasCurrent ? &currentEntry : nullptr);
    });
}

void RcxController::pushRecentType(const QString& displayName) {
    if (displayName.isEmpty()) return;
    m_recentTypeNames.removeAll(displayName);
    m_recentTypeNames.prepend(displayName);
    while (m_recentTypeNames.size() > 8) m_recentTypeNames.removeLast();
}

void RcxController::applyTypePopupResult(TypePopupMode mode, int nodeIdx,
                                         const TypeEntry& entry, const QString& fullText) {
    // Resolve external types: structId==0 means from another document, import first
    TypeEntry resolved = entry;
    if (resolved.entryKind == TypeEntry::Composite && resolved.structId == 0
        && !resolved.displayName.isEmpty()) {
        resolved.structId = findOrCreateStructByName(resolved.displayName);
    }

    // Track for the popup's "Recent" section. Done early so any return path
    // below still records the user's selection.
    pushRecentType(resolved.displayName);

    if (mode == TypePopupMode::Root) {
        // The line-0 class chooser is a root pick, the same gesture as the
        // bar's root.chev: the place left is recorded here, at the gesture
        // (setViewRootId never records — load, new-tab and delete-root
        // call it too), and only when the root actually changes.
        if (resolved.entryKind == TypeEntry::Composite)
            pickViewRoot(resolved.structId);
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
                // Picking a plain type on an enum field makes it that plain type:
                // the enum link goes too, in the same undo step.
                const bool wasEnum = enumTypeOf(m_doc->tree, m_doc->tree.nodes[nodeIdx]) != nullptr;
                const bool refreshWasSuppressed = m_suppressRefresh;
                if (wasEnum) {
                    m_suppressRefresh = true;
                    m_doc->undoStack.beginMacro(QStringLiteral("Change type"));
                }
                if (resolved.primitiveKind != nodeKind)
                    changeNodeKind(nodeIdx, resolved.primitiveKind);
                if (wasEnum) {
                    int idx = m_doc->tree.indexOfId(nodeId);   // changeNodeKind may insert pads
                    if (idx >= 0 && m_doc->tree.nodes[idx].refId != 0)
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ChangePointerRef{nodeId, m_doc->tree.nodes[idx].refId, 0}));
                    m_doc->undoStack.endMacro();
                    m_suppressRefresh = refreshWasSuppressed;
                    if (!m_suppressRefresh) refresh();
                }
                // Apply RVA flag from the catalog entry. The catalog
                // ships two pointer entries per width — "Pointer32" and
                // "Pointer32 (RVA)" — that differ only in isRelative.
                // Honour the pick explicitly so switching from RVA to
                // absolute clears the flag, and vice versa.
                if (isPointerKind(resolved.primitiveKind)) {
                    int idx = m_doc->tree.indexOfId(nodeId);
                    if (idx >= 0 && m_doc->tree.nodes[idx].isRelative != resolved.isRelative) {
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ToggleRelative{nodeId,
                                m_doc->tree.nodes[idx].isRelative,
                                resolved.isRelative}));
                    }
                    // Hint about the second step: the user just picked
                    // a void pointer kind (and possibly RVA). Without
                    // a refId the row will read as "void*" / "void* rva"
                    // and won't expand. Tell them how to wire a target.
                    if (idx >= 0 && m_doc->tree.nodes[idx].refId == 0) {
                        emit statusHint(QStringLiteral(
                            "Pointer set. Re-open the type chooser and "
                            "pick a struct to set the target."));
                    }
                }
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

            } else if (isPointerKind(nodeKind)) {
                // Composite picked on an existing Pointer node — set
                // refId (the pointer's struct target) without changing
                // the node's kind, ptrDepth, or isRelative. This is
                // the second step of the "Pointer32 (RVA) → MyStruct"
                // workflow: user picks ptr32 (RVA) first to set kind
                // + isRelative, then reopens the chooser and picks the
                // target struct here. Preserving kind/isRelative means
                // they don't get clobbered back to inline Struct.
                int idx = m_doc->tree.indexOfId(nodeId);
                if (idx >= 0 && m_doc->tree.nodes[idx].refId != resolved.structId) {
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::ChangePointerRef{nodeId,
                            m_doc->tree.nodes[idx].refId,
                            resolved.structId}));
                }
            } else if (const int enumIdx = m_doc->tree.indexOfId(resolved.structId);
                       enumIdx >= 0 && m_doc->tree.nodes[enumIdx].isEnum()) {
                // An enum is not a struct: the field stays an int (UInt32 unless it
                // already is one) and points at the enum, so it reads as that enum.
                const bool isInt = nodeKind == NodeKind::UInt8 || nodeKind == NodeKind::UInt16
                    || nodeKind == NodeKind::UInt32 || nodeKind == NodeKind::UInt64
                    || nodeKind == NodeKind::Int8 || nodeKind == NodeKind::Int16
                    || nodeKind == NodeKind::Int32 || nodeKind == NodeKind::Int64;
                if (!isInt)
                    changeNodeKind(nodeIdx, NodeKind::UInt32);
                int idx = m_doc->tree.indexOfId(nodeId);
                if (idx >= 0 && m_doc->tree.nodes[idx].refId != resolved.structId)
                    m_doc->undoStack.push(new RcxCommand(this,
                        cmd::ChangePointerRef{nodeId, m_doc->tree.nodes[idx].refId, resolved.structId}));
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
        // Only runs for Struct/Array targets: changeNodeKind() forces newSize=0
        // for those kinds (see controller.cpp ~L1237) so its shrink/grow path
        // never fires, and this block is responsible for shifting siblings.
        // For primitives/pointers, changeNodeKind() already handled siblings
        // (padding on shrink, offset shift on grow) — running this block for
        // those cases would double-shift and break offsets of fields below.
        {
            int ni = m_doc->tree.indexOfId(nodeId);
            if (ni >= 0 && (m_doc->tree.nodes[ni].kind == NodeKind::Struct
                            || m_doc->tree.nodes[ni].kind == NodeKind::Array)) {
                const Node& updatedNode = m_doc->tree.nodes[ni];
                int newEffectiveSize = updatedNode.byteSize();
                if (newEffectiveSize == 0 && updatedNode.kind == NodeKind::Struct)
                    newEffectiveSize = m_doc->tree.structSpan(nodeId);
                // Array-of-Struct: byteSize() and structSpan() both return 0
                // because sizeForKind(Struct)==0. Compute from refId span × arrayLen.
                // Use int64_t to prevent overflow with large arrays.
                if (newEffectiveSize == 0 && updatedNode.kind == NodeKind::Array
                    && updatedNode.elementKind == NodeKind::Struct && updatedNode.refId != 0) {
                    int elemSpan = m_doc->tree.structSpan(updatedNode.refId);
                    int64_t product = (int64_t)elemSpan * updatedNode.arrayLen;
                    newEffectiveSize = (int)qMin(product, (int64_t)INT_MAX);
                }
                if (newEffectiveSize == 0 && updatedNode.kind == NodeKind::Array
                    && updatedNode.elementKind != NodeKind::Struct) {
                    int64_t product = (int64_t)sizeForKind(updatedNode.elementKind) * updatedNode.arrayLen;
                    newEffectiveSize = (int)qMin(product, (int64_t)INT_MAX);
                }
                int sizeDelta = newEffectiveSize - oldEffectiveSize;
                if (sizeDelta != 0 && oldEffectiveSize > 0) {
                    int oldEnd = nodeOffset + oldEffectiveSize;
                    auto siblings = m_doc->tree.childrenOf(parentId);
                    bool wasSuppressed2 = m_suppressRefresh;
                    m_suppressRefresh = true;
                    m_doc->undoStack.beginMacro(QStringLiteral("Adjust sibling offsets"));
                    for (int si : siblings) {
                        const auto& sib = m_doc->tree.nodes[si];
                        if (sib.id == nodeId) continue;
                        if (sib.offset >= oldEnd) {
                            m_doc->undoStack.push(new RcxCommand(this,
                                cmd::ChangeOffset{sib.id, sib.offset,
                                                  sib.offset + sizeDelta}));
                        }
                    }
                    m_doc->undoStack.endMacro();
                    m_suppressRefresh = wasSuppressed2;
                    if (!m_suppressRefresh) refresh();
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

uint64_t RcxController::findOrCreateStructByName(const QString& typeName, int depth) {
    if (depth > 8) return 0;  // prevent runaway recursion on cyclic type graphs

    // Check if it already exists locally
    for (const auto& n : m_doc->tree.nodes) {
        if (n.parentId == 0 && n.kind == NodeKind::Struct
            && (n.structTypeName == typeName || (n.structTypeName.isEmpty() && n.name == typeName)))
            return n.id;
    }

    bool wasSuppressed = m_suppressRefresh;
    m_suppressRefresh = true;
    m_doc->undoStack.beginMacro(QStringLiteral("Import type"));

    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = typeName;
    root.name = QStringLiteral("instance");
    root.parentId = 0;
    root.offset = 0;
    root.id = m_doc->tree.reserveId();

    // Check if this is a built-in common type with a predefined layout
    const CommonType* ct = findCommonType(typeName);
    if (ct) {
        root.classKeyword = QString::fromLatin1(ct->classKeyword);
        m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{root}));

        for (int i = 0; i < ct->fieldCount; i++) {
            const auto& f = ct->fields[i];
            Node child;
            child.kind = f.kind;
            child.name = QString::fromLatin1(f.name);
            child.parentId = root.id;
            child.offset = f.offset;
            child.id = m_doc->tree.reserveId();

            if (f.ptrTarget && f.ptrTarget[0] != '\0'
                && (f.kind == NodeKind::Pointer64 || f.kind == NodeKind::Pointer32)) {
                QString targetName = QString::fromLatin1(f.ptrTarget);
                if (targetName != typeName) {
                    m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{child}));
                    uint64_t targetId = findOrCreateStructByName(targetName, depth + 1);
                    if (targetId != 0) {
                        m_doc->undoStack.push(new RcxCommand(this,
                            cmd::ChangePointerRef{child.id, 0, targetId}));
                    }
                    continue;
                }
            }
            m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{child}));
        }
    } else {
        // Unknown type: create with default hex64 fields
        m_doc->undoStack.push(new RcxCommand(this, cmd::Insert{root}));
        for (int i = 0; i < 8; i++)
            insertNode(root.id, i * 8, NodeKind::Hex64,
                       QString("field_%1").arg(i * 8, 2, 16, QChar('0')));
    }

    m_doc->undoStack.endMacro();
    m_suppressRefresh = wasSuppressed;
    return root.id;
}

void RcxController::attachViaPlugin(const QString& providerIdentifier, const QString& target,
                                    bool registerAsSavedSource) {
    const auto* info = ProviderRegistry::instance().findProvider(providerIdentifier);
    if (!info || !info->plugin) {
        ThemedMessageBox::warn(qobject_cast<QWidget*>(parent()),
            QStringLiteral("Provider Unavailable"),
            QStringLiteral("Provider \"%1\" isn't registered. Make sure the plugin is loaded.")
                .arg(providerIdentifier));
        return;
    }

    QString errorMsg;
    auto provider = info->plugin->createProvider(target, &errorMsg);
    if (!provider) {
        if (!errorMsg.isEmpty())
            ThemedMessageBox::warn(qobject_cast<QWidget*>(parent()),
                QStringLiteral("Couldn't Attach"), errorMsg);
        return;
    }

    if ((m_instance || m_doc->controllers.size() > 1)
        && provider->isLive() && provider->pointerSize() != m_doc->tree.pointerSize) {
        emit statusHint(QStringLiteral("Source pointer size differs from the shared class definition"));
        return;
    }
    if (!m_instance && m_doc->controllers.size() == 1) m_doc->undoStack.clear();
    this->provider() = std::move(provider);
    if (!m_instance) m_doc->dataPath.clear();
    // Don't overwrite baseAddress — caller (e.g. selfTest) already set it.
    // User-initiated source switches go through selectSource() which does update it.

    // Adopt the provider's pointer size for this document
    if (!m_instance) m_doc->tree.pointerSize = this->provider()->pointerSize();

    // Re-evaluate stored formula against the new provider
    if (!baseAddressFormula().isEmpty()) {
        int ptrSz = m_doc->tree.pointerSize;
        const AddressParserCallbacks cbs =
            makeAddressCallbacks(this->provider().get(), ptrSz);
        auto result = AddressParser::evaluate(baseAddressFormula(), ptrSz, &cbs);
        if (result.ok)
            baseAddress() = result.value;
    }

    resetSnapshot();

    // Optional: register the attach as a saved-source entry so the
    // source-picker dropdown surfaces it. Mirrors what selectSource
    // does on UI-initiated attaches — dedup on (kind, providerTarget)
    // so a repeat attach of the same target reuses the existing slot
    // instead of growing the list.
    if (registerAsSavedSource) {
        int existingIdx = -1;
        for (int i = 0; i < m_savedSources.size(); ++i) {
            if (m_savedSources[i].kind == providerIdentifier
                && m_savedSources[i].providerTarget == target) {
                existingIdx = i;
                break;
            }
        }
        if (existingIdx >= 0) {
            m_activeSourceIdx = existingIdx;
            m_savedSources[existingIdx].baseAddress = baseAddress();
        } else {
            SavedSourceEntry entry;
            entry.kind             = providerIdentifier;
            entry.displayName      = this->provider() ? this->provider()->name()
                                                    : providerIdentifier;
            entry.providerTarget   = target;
            entry.baseAddress      = baseAddress();
            m_savedSources.append(entry);
            m_activeSourceIdx = m_savedSources.size() - 1;
        }
    }

    emit m_doc->documentChanged();
    refresh();
}

void RcxController::switchToSavedSource(int idx) {
    if (idx < 0 || idx >= m_savedSources.size()) return;
    if (idx == m_activeSourceIdx) return;
    // A source switch is a navigation gesture (the base comes with the
    // source, so the view lands somewhere else). Silent during load and
    // during a Back/Forward restore — recordNav checks both.
    recordNav();

    // Save current source's base address before switching
    if (m_activeSourceIdx >= 0 && m_activeSourceIdx < m_savedSources.size()) {
        m_savedSources[m_activeSourceIdx].baseAddress = baseAddress();
        m_savedSources[m_activeSourceIdx].baseAddressFormula = baseAddressFormula();
    }

    const auto& entry = m_savedSources[idx];

    if (entry.kind == QStringLiteral("File")) {
        if (!loadSourceFile(entry.filePath)) return;
        m_activeSourceIdx = idx;
        baseAddress() = entry.baseAddress;
        baseAddressFormula() = entry.baseAddressFormula;
        // Drop the prior source's snapshot/pages/value-history. loadData() only
        // swaps the document's provider; without this, refresh() still composes
        // against the stale m_snapshotProv (controller.cpp:1959) and renders the
        // previous (possibly live process) bytes over the file. The provider
        // paths reset via attachViaPlugin; the File path must do it itself.
        resetSnapshot();
        refresh();
    } else if (!entry.providerTarget.isEmpty()) {
        m_activeSourceIdx = idx;
        // Plugin-based provider (e.g. "processmemory" with target "pid:name")
        // Restore formula before attach so it can be re-evaluated against the new provider
        baseAddressFormula() = entry.baseAddressFormula;
        attachViaPlugin(entry.kind, entry.providerTarget);
        // Restore saved base address — always override with saved value on source switch
        if (entry.baseAddressFormula.isEmpty())
            baseAddress() = entry.baseAddress;
    }
    // Notify listeners that the active source changed — used by the
    // doc tab's source-icon to swap to the new provider's icon.
    emit m_doc->documentChanged();
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
                m_savedSources[m_activeSourceIdx].baseAddress = baseAddress();

            if (!loadSourceFile(path)) return;

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
                baseAddress() = m_savedSources[existingIdx].baseAddress;
            } else {
                SavedSourceEntry entry;
                entry.kind = QStringLiteral("File");
                entry.displayName = QFileInfo(path).fileName();
                entry.filePath = path;
                entry.baseAddress = baseAddress();
                m_savedSources.append(entry);
                m_activeSourceIdx = m_savedSources.size() - 1;
            }
            // Notify after m_activeSourceIdx is set so listeners
            // (doc tab source-icon, etc) see the new state.
            emit m_doc->documentChanged();
            // Drop the prior source's snapshot before composing the file (else
            // refresh() renders the stale snapshot — see switchToSavedSource).
            resetSnapshot();
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
                    if ((m_instance || m_doc->controllers.size() > 1)
                        && provider->isLive() && provider->pointerSize() != m_doc->tree.pointerSize) {
                        emit statusHint(QStringLiteral("Source pointer size differs from the shared class definition"));
                        return;
                    }
                    if (m_activeSourceIdx >= 0 && m_activeSourceIdx < m_savedSources.size())
                        m_savedSources[m_activeSourceIdx].baseAddress = baseAddress();

                    uint64_t newBase = provider->base();
                    QString displayName = provider->name();
                    if (!m_instance && m_doc->controllers.size() == 1) m_doc->undoStack.clear();
                    this->provider() = std::move(provider);
                    if (!m_instance) m_doc->dataPath.clear();
                    if (!m_instance) m_doc->tree.pointerSize = this->provider()->pointerSize();

                    // Re-evaluate formula if present (mirrors attachViaPlugin)
                    if (!baseAddressFormula().isEmpty()) {
                        int ptrSz = m_doc->tree.pointerSize;
                        const AddressParserCallbacks cbs =
                            makeAddressCallbacks(this->provider().get(), ptrSz);
                        auto result = AddressParser::evaluate(
                            baseAddressFormula(), ptrSz, &cbs);
                        if (result.ok)
                            baseAddress() = result.value;
                    } else {
                        // Adopt the new provider's base when this target
                        // hasn't been seen before. The old test ("base ==
                        // 0x00400000") only caught the fresh-project case
                        // and missed:
                        //   * the "New Class" self-attach, which leaves
                        //     baseAddress pointing at a heap pointer in
                        //     Reclass.exe (m_ownedBuffer) — that pointer
                        //     reads as unmapped 00s in any other target;
                        //   * any other prior attach where the user
                        //     never set a custom base.
                        // Saved sources we've seen before take their own
                        // saved baseAddress in the existingIdx branch
                        // below, so this only fires for genuinely new
                        // attaches.
                        QString identifier = providerInfo->identifier;
                        bool isExisting = false;
                        for (const auto& s : m_savedSources) {
                            if (s.kind == identifier && s.providerTarget == target) {
                                isExisting = true;
                                break;
                            }
                        }
                        if (!isExisting && newBase != 0)
                            baseAddress() = newBase;
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
                        m_savedSources[existingIdx].baseAddress = baseAddress();
                    } else {
                        SavedSourceEntry entry;
                        entry.kind = identifier;
                        entry.displayName = displayName;
                        entry.providerTarget = target;
                        entry.baseAddress = baseAddress();
                        m_savedSources.append(entry);
                        m_activeSourceIdx = m_savedSources.size() - 1;
                    }
                    // Re-emit AFTER m_activeSourceIdx is set so the
                    // doc tab's source-icon reads the new index, not
                    // the stale one from the earlier emit at 5258.
                    emit m_doc->documentChanged();
                    refresh();
                } else if (!errorMsg.isEmpty()) {
                    ThemedMessageBox::warn(qobject_cast<QWidget*>(parent()),
                        QStringLiteral("Couldn't Attach"), errorMsg);
                }
            }
        }
    }
}

void RcxController::clearSources() {
    m_savedSources.clear();
    m_nav.forgetAllSources();   // every recorded source index just went stale
    m_activeSourceIdx = -1;
    this->provider() = std::make_shared<NullProvider>();
    if (!m_instance) m_doc->dataPath.clear();
    resetSnapshot();
    refresh();
}

void RcxController::removeSavedSource(int idx) {
    if (idx < 0 || idx >= m_savedSources.size()) return;
    const bool wasActive = (idx == m_activeSourceIdx);
    m_savedSources.removeAt(idx);

    // Keep m_activeSourceIdx pointing at the same entry it did before the
    // removal shifted everything after `idx` down by one — and the history
    // entries too, which hold the same raw indices (an old entry would
    // otherwise restore whichever source slid into the removed slot).
    if (wasActive)            m_activeSourceIdx = -1;
    else if (m_activeSourceIdx > idx) m_activeSourceIdx--;
    m_nav.forgetSource(idx);

    if (wasActive) {
        // Removing the connected source detaches the view — same as Clear All
        // but leaving the remaining saved sources intact (no auto-activate, so
        // it behaves like "clicking out of" the source rather than surprising
        // the user by jumping to a different process/file).
        this->provider() = std::make_shared<NullProvider>();
        if (!m_instance) m_doc->dataPath.clear();
        resetSnapshot();
    }
    refresh();
    emit m_doc->documentChanged();
}

void RcxController::copySavedSources(const QVector<SavedSourceEntry>& sources, int activeIdx) {
    m_savedSources = sources;
    m_activeSourceIdx = activeIdx;
    // Notify so the new tab's source icon repaints to reflect the copied
    // active source (mirrors removeSavedSource). Without this, a tab opened
    // into an existing project (project_new, forceFreshDoc=false) keeps the
    // placeholder plug icon for a disconnected saved source until an unrelated
    // reconcile — the tab icon refresh is wired to documentChanged (the heavy
    // rebuild handler is deferred + guarded, so emitting here is safe).
    emit m_doc->documentChanged();
}

// ── Auto-refresh ──

void RcxController::setRefreshInterval(int ms) {
    // The Options dialog feeds the user's chosen "snappy" rate here.
    // Treat that as the base — the adaptive loop still backs off when
    // pages stop changing or when the window loses focus, just relative
    // to this new floor instead of the kDefaultRefreshMs constant.
    m_refreshIntervalBaseMs = qMax(1, ms);
    m_refreshIntervalMaxMs  = qMax(m_refreshIntervalBaseMs, 1500);
    m_refreshIntervalBlurMs = qMax(m_refreshIntervalBaseMs, 1500);
    applyAdaptiveInterval();
}

void RcxController::applyAdaptiveInterval() {
    if (!m_refreshTimer) return;
    int target;
    if (!m_windowVisible) {
        // Stop the timer entirely while minimized — nothing on screen
        // to update, no reason to syscall. Resume on setWindowState
        // restoring visibility.
        if (!timelineCapturing()) {
            if (m_refreshTimer->isActive()) m_refreshTimer->stop();
            return;
        }
        // …unless the timeline is capturing: playing the target with Reclass
        // minimised and rewinding afterwards is the point. Reads continue a
        // little slower; nothing is composed until the window is back.
        target = qMax(m_refreshIntervalBaseMs, 250);
    } else if (!m_windowFocused) {
        target = timelineCapturing() ? m_refreshIntervalBaseMs : m_refreshIntervalBlurMs;
    } else if (m_idleTicks >= kIdleBackoffTicks) {
        // Geometric backoff once the struct has been quiet for a while:
        // base × 2^(idleTicks / threshold), capped. e.g. base=200, after
        // 8 idle ticks → 400, after 16 → 800, after 24 → 1500 (capped).
        int factor = 1 << qMin(4, (m_idleTicks - kIdleBackoffTicks) / kIdleBackoffTicks + 1);
        // Capturing caps the backoff lower: a change after a quiet spell is
        // stamped within half a second, not a second and a half.
        const int cap = timelineCapturing() ? qMax(m_refreshIntervalBaseMs, 500)
                                            : m_refreshIntervalMaxMs;
        target = qMin(cap, m_refreshIntervalBaseMs * factor);
    } else {
        target = m_refreshIntervalBaseMs;
    }
    if (m_refreshTimer->interval() != target)
        m_refreshTimer->setInterval(target);
    if (!m_refreshTimer->isActive())
        m_refreshTimer->start();
}

void RcxController::setWindowState(bool focused, bool visible) {
    bool focusGained = (focused && !m_windowFocused);
    m_windowFocused = focused;
    m_windowVisible = visible;
    // Coming back into focus → snap back to base rate immediately so
    // the user sees current values, not a stale snapshot from the
    // last backed-off tick.
    if (focusGained) m_idleTicks = 0;
    applyAdaptiveInterval();
    if (m_windowVisible && m_viewStale) refresh();
}

void RcxController::setCompactColumns(bool v) {
    m_compactColumns = v;
    refresh();
}

void RcxController::setTreeLines(bool v) {
    m_treeLines = v;
    refresh();
}

void RcxController::setTreeColumns(bool v) {
    // Paint-only: the text doesn't change, so no recompose.
    m_treeColumns = v;
    for (RcxEditor* editor : m_editors)
        editor->setTreeColumns(v);
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

void RcxController::setShowRtti(bool v) {
    m_showRtti = v;
    refresh();
}

void RcxController::setShowEnumChips(bool v) {
    m_showEnumChips = v;
    refresh();
}

void RcxController::setupAutoRefresh() {
    int ms = QSettings("RC", "RC").value("refreshMs", kDefaultRefreshMs).toInt();
    m_refreshIntervalBaseMs = qMax(1, ms);
    m_refreshIntervalMaxMs  = qMax(m_refreshIntervalBaseMs, 1500);
    m_refreshIntervalBlurMs = qMax(m_refreshIntervalBaseMs, 1500);
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(m_refreshIntervalBaseMs);
    connect(m_refreshTimer, &QTimer::timeout, this, &RcxController::onRefreshTick);
    m_refreshTimer->start();

    m_refreshWatcher = new QFutureWatcher<ReadBatch>(this);
    connect(m_refreshWatcher, &QFutureWatcher<ReadBatch>::finished,
            this, &RcxController::onReadComplete);
}

std::shared_ptr<const RcxController::CapturePlan> RcxController::capturePlanFor(uint64_t rootId) {
    const NodeTree& tree = viewTree();
    const quint64 key = (tree.generation() * 0x9E3779B97F4A7C15ull)
                      ^ (m_capturePlanEpoch * 0xC2B2AE3D27D4EB4Full) ^ rootId;
    if (m_capturePlan && m_capturePlanKey == key) return m_capturePlan;

    auto plan = std::make_shared<CapturePlan>();
    QHash<uint64_t, QVector<int>> childMap;
    for (int i = 0; i < tree.nodes.size(); ++i) childMap[tree.nodes[i].parentId].append(i);

    // Which containers have a pointer anywhere beneath them: an array of
    // structs with none is skipped instead of walked element by element.
    QHash<uint64_t, bool> hasPtrMemo;
    std::function<bool(uint64_t, int)> hasPointers = [&](uint64_t id, int depth) -> bool {
        auto it = hasPtrMemo.constFind(id);
        if (it != hasPtrMemo.constEnd()) return it.value();
        if (depth > 32) return false;
        hasPtrMemo.insert(id, false);   // cycle guard
        bool found = false;
        for (int ci : childMap.value(id)) {
            const Node& n = tree.nodes[ci];
            if (n.kind == NodeKind::Pointer32 || n.kind == NodeKind::Pointer64) { found = true; break; }
            if ((n.kind == NodeKind::Struct || n.kind == NodeKind::Array)
                && (hasPointers(n.id, depth + 1) || (n.refId && hasPointers(n.refId, depth + 1)))) {
                found = true;
                break;
            }
        }
        hasPtrMemo.insert(id, found);
        return found;
    };
    auto spanOf = [&](uint64_t id) -> int {
        const int idx = tree.indexOfId(id);
        if (idx >= 0 && isPointerKind(tree.nodes[idx].kind)) {
            // Materialised pointer children: the target frame is their extent.
            int64_t end = 0;
            for (int ci : childMap.value(id)) {
                const Node& c = tree.nodes[ci];
                const int sz = (c.kind == NodeKind::Struct || c.kind == NodeKind::Array)
                             ? tree.structSpan(c.id, &childMap) : c.byteSize();
                end = qMax<int64_t>(end, int64_t(c.offset) + sz);
            }
            return int(qMin<int64_t>(end, INT_MAX));
        }
        return tree.structSpan(id, &childMap);
    };

    QHash<uint64_t, int> planIndex;
    int totalPtrs = 0;
    std::function<int(uint64_t)> compile;
    std::function<void(PlanStruct&, uint64_t, int64_t, int)> walk =
        [&](PlanStruct& ps, uint64_t parent, int64_t baseOff, int depth) {
        if (depth > 32 || totalPtrs >= kMaxPlanPointers) return;
        for (int ci : childMap.value(parent)) {
            const Node& n = tree.nodes[ci];
            const int64_t off = baseOff + n.offset;
            if (n.kind == NodeKind::Pointer32 || n.kind == NodeKind::Pointer64) {
                const bool materialised = childMap.contains(n.id);
                PlanPtr pp;
                pp.offset = off;
                pp.size = (n.kind == NodeKind::Pointer32) ? 4 : 8;
                if (n.kind == NodeKind::Pointer64 && n.ptrDepth > 0 && n.refId == 0 && !materialised
                    && isValidPrimitivePtrTarget(n.elementKind)) {
                    // A primitive pointer shows "-> value", collapsed or not
                    // (format.cpp): its target is part of what the view reads.
                    pp.derefs = uint8_t(qBound(0, n.ptrDepth - 1, 8));
                    Node target;
                    target.kind = n.elementKind;
                    target.strLen = n.strLen;
                    pp.targetLen = qMax(1, target.byteSize());
                } else if (!isCollapsed(n) && (materialised || n.refId != 0)) {
                    pp.derefs = uint8_t(qBound(0, n.ptrDepth, 8));
                    pp.relative = n.isRelative;
                    pp.targetStruct = compile(materialised ? n.id : n.refId);
                } else {
                    continue;
                }
                ps.ptrs.append(pp);
                if (++totalPtrs >= kMaxPlanPointers) return;
            } else if (n.kind == NodeKind::Struct || n.kind == NodeKind::Array) {
                const bool ownChildren = childMap.contains(n.id);
                const uint64_t frame = ownChildren ? n.id : n.refId;
                if (!frame || !hasPointers(frame, 0)) continue;
                if (n.kind == NodeKind::Array && !ownChildren) {
                    if (n.elementKind != NodeKind::Struct) continue;
                    const int stride = qMax(1, tree.structSpan(n.refId, &childMap));
                    const int count = qMin(n.arrayLen, kMaxPlanArrayElems);
                    for (int i = 0; i < count && totalPtrs < kMaxPlanPointers; ++i)
                        walk(ps, n.refId, off + int64_t(i) * stride, depth + 1);
                } else {
                    walk(ps, frame, off, depth + 1);
                }
            }
        }
    };
    compile = [&](uint64_t containerId) -> int {
        auto it = planIndex.constFind(containerId);
        if (it != planIndex.constEnd()) return it.value();
        const int idx = plan->structs.size();
        plan->structs.append(PlanStruct{});
        planIndex.insert(containerId, idx);
        PlanStruct ps;
        ps.span = spanOf(containerId);
        walk(ps, containerId, 0, 0);
        plan->structs[idx] = std::move(ps);
        return idx;
    };
    plan->root = compile(rootId);
    m_capturePlan = plan;
    m_capturePlanKey = key;
    return m_capturePlan;
}

// Recursively collect memory ranges for a struct and its pointer targets.
// memBase is the absolute address where this struct's data lives.
void RcxController::collectPointerRanges(
        uint64_t structId, uint64_t memBase,
        int depth, int maxDepth,
        QSet<QPair<uint64_t,uint64_t>>& visited,
        QVector<QPair<uint64_t,int>>& ranges,
        int64_t& budget) const
{
    if (depth >= maxDepth) return;
    if (budget <= 0) return;  // exhausted byte budget — bail
    QPair<uint64_t,uint64_t> key{structId, memBase};
    if (visited.contains(key)) return;
    visited.insert(key);

    int span = m_doc->tree.structSpan(structId);
    if (span <= 0) return;
    ranges.emplaceBack(memBase, span);
    budget -= span;
    if (budget <= 0) return;

    if (!m_snapshotProv) return;

    // Walk children looking for non-collapsed pointers
    QVector<int> children = m_doc->tree.childrenOf(structId);
    for (int ci : children) {
        if (budget <= 0) break;
        const Node& child = m_doc->tree.nodes[ci];
        if (child.kind != NodeKind::Pointer32 && child.kind != NodeKind::Pointer64)
            continue;
        if (isCollapsed(child) || child.refId == 0) continue;

        uint64_t ptrAddr = memBase + child.offset;
        int ptrSize = child.byteSize();
        if (!m_snapshotProv->isReadable(ptrAddr, ptrSize)) continue;

        uint64_t ptrVal = (child.kind == NodeKind::Pointer32)
            ? (uint64_t)m_snapshotProv->readU32(ptrAddr)
            : m_snapshotProv->readU64(ptrAddr);
        if (ptrVal == 0 || ptrVal == UINT64_MAX) continue;

        uint64_t pBase = ptrVal;
        collectPointerRanges(child.refId, pBase, depth + 1, maxDepth,
                             visited, ranges, budget);
    }

    // Embedded struct references (struct node with refId but no own children)
    int idx = m_doc->tree.indexOfId(structId);
    if (idx >= 0) {
        const Node& sn = m_doc->tree.nodes[idx];
        if (sn.kind == NodeKind::Struct && sn.refId != 0 && children.isEmpty())
            collectPointerRanges(sn.refId, memBase, depth, maxDepth,
                                 visited, ranges, budget);
    }
}

void RcxController::onRefreshTick() {
    // Liveness-flip detection runs BEFORE the early-returns below: when
    // a process exits its provider transitions to !isValid(), and we
    // also want to report that case to the UI so the tab icon dims.
    bool nowLive = (this->provider() && this->provider()->isValid());
    if (nowLive != m_lastLive) {
        m_lastLive = nowLive;
        emit sourceLivenessChanged(nowLive);
    }

    // Tri-state source status for the status-bar badge (transition-only). Runs
    // before the early-returns so it tracks even when the source isn't live.
    SourceStatus status;
    if (!nowLive)
        // No valid provider bound: None only when there's also no saved source;
        // otherwise a saved source that has disconnected. (Liveness must win over
        // the saved-index check — a plugin attach with registerAsSavedSource=false
        // leaves m_activeSourceIdx == -1 yet is genuinely live: the tutorial
        // self-attach, kernel/physical provider tab, and the MCP attach tool.
        // Keying None on m_activeSourceIdx<0 mislabelled those live, polled
        // processes as None → the chip masked them to a neutral "Static" dot.)
        status = (m_activeSourceIdx < 0) ? SourceStatus::None
                                         : SourceStatus::Disconnected;
    else if (!this->provider()->isLive())          status = SourceStatus::Static;
    else if (!m_lastReadOk)                       status = SourceStatus::Stale;
    else                                          status = SourceStatus::Live;
    if (status != m_lastStatus) {
        m_lastStatus = status;
        emit sourceStatusChanged(status);
    }

    if (m_readInFlight) return;
    if (!this->provider() || !this->provider()->isLive()) return;
    if (m_suppressRefresh) return;
    for (auto* editor : m_editors)
        if (editor->isEditing()) return;

    ++m_tickCount;

    int extent = computeDataExtent();
    if (extent <= 0) return;

    // Collect all needed ranges: main struct + pointer targets (absolute addresses)
    QVector<QPair<uint64_t,int>> ranges;
    ranges.emplaceBack(baseAddress(), extent);

    // Pointer targets are followed on the worker, from the bytes it has just
    // read, so a pointer and its target always come from the same tick (the
    // old walk read pointer values from the previous snapshot, a tick late,
    // and missed pointers inside nested structs and arrays). The plan is the
    // view's pointer graph, flattened here while the tree is safe to touch.
    // The 64 MB budget still clips a cyclic or pathological graph.
    uint64_t planRootId = m_viewRootId;
    if (planRootId == 0 && !m_doc->tree.nodes.isEmpty())
        planRootId = m_doc->tree.nodes[0].id;
    const std::shared_ptr<const CapturePlan> plan = capturePlanFor(planRootId);
    const int64_t pointerBudget = kPointerSnapshotByteBudget - extent;
    const bool planHasPointers = plan && plan->root >= 0 && !plan->structs[plan->root].ptrs.isEmpty();

    // Pages compose actually read on its last passes — RTTI vtables, deref
    // targets, pointers inside nested structs the walk above does not follow.
    // Reading them here keeps them out of the UI thread's fall-through reads
    // and puts them in the timeline, where a past frame needs them.
    for (auto it = m_touchedAge.constBegin(); it != m_touchedAge.constEnd(); ++it)
        ranges.emplaceBack(it.key(), 4096);

    // ── Speedup 1: viewport-bounded re-read ──
    // The first refresh on a fresh attach reads the whole extent so the
    // snapshot has every page. Subsequent refreshes only re-fetch pages
    // that intersect the visible viewport (plus a 2-page overscan in
    // each direction to keep small scrolls instant). Pages outside the
    // viewport keep their previous snapshot bytes; when the user scrolls
    // them in, the next tick refreshes them.
    std::optional<QPair<uint64_t, uint64_t>> viewport;
    bool firstSnapshot = !m_snapshotProv || m_prevPages.isEmpty();
    if (!firstSnapshot) viewport = viewportAddressRange();

    constexpr uint64_t kPageSize = 4096;
    constexpr uint64_t kPageMask = ~(kPageSize - 1);
    constexpr uint64_t kOverscanPages = 2;

    // Build the set of pages we actually need this tick — and, for the
    // timeline, the set this view is WATCHING, before any page is skipped.
    QSet<uint64_t> requestPages;
    QVector<uint64_t> intendedPages;
    bool skippedStable = false;
    for (const auto& r : ranges) {
        uint64_t pageStart = r.first & kPageMask;
        uint64_t end = r.first + r.second;
        uint64_t pageEnd = (end + kPageSize - 1) & kPageMask;
        for (uint64_t p = pageStart; p < pageEnd; p += kPageSize) {
            intendedPages.append(p);
            // Speedup 4: never re-read pages we've classified as
            // permanent (read-only module memory).
            if (m_snapshotProv && m_snapshotProv->isPermanent(p)) continue;

            if (viewport) {
                // Restrict main-struct reads to the viewport-overscan
                // window. Pointer-target ranges (depth > 0 in
                // collectPointerRanges) aren't position-bound to the
                // viewport — but they're typically tiny and few.
                uint64_t lo = (viewport->first  > kOverscanPages * kPageSize)
                            ? (viewport->first - kOverscanPages * kPageSize) & kPageMask
                            : 0;
                uint64_t hi = ((viewport->second + kOverscanPages * kPageSize)
                              + kPageSize - 1) & kPageMask;
                bool inViewport = (p >= lo && p < hi);
                bool isMainRange = (r.first == baseAddress());
                if (isMainRange && !inViewport) {
                    // Speedup 2: stable backstage page → re-read at half rate.
                    int stab = m_pageStability.value(p, 0);
                    bool isStable = (stab >= kStabilityThreshold);
                    if (isStable && (m_tickCount & 1ULL)) { skippedStable = true; continue; }
                }
            }
            requestPages.insert(p);
        }
    }

    if (requestPages.isEmpty() && !planHasPointers) {
        // Nothing to read this tick (everything is stable + off-screen,
        // or every page is permanent). Treat as zero-change for the
        // adaptive backoff so the timer can widen — but don't recompose,
        // the previous snapshot is already on screen.
        ++m_idleTicks;
        applyAdaptiveInterval();
        return;
    }

    m_readInFlight = true;
    m_readGen = m_refreshGen;

    m_inflightPartial = skippedStable;
    m_readStartMs = tl::CaptureClock::nowMs();

    auto prov = this->provider();
    QVector<uint64_t> pageList(requestPages.constBegin(), requestPages.constEnd());
    // The worker diffs against the pages as they stood at dispatch. Copying
    // the map is O(1) (implicitly shared): anything the UI thread replaces
    // while the read is in flight detaches rather than changing under it.
    const PageMap baseline = m_prevPages;
    const uint64_t diffGen = m_diffGen;
    const QSet<uint64_t> permanent = m_snapshotProv ? m_snapshotProv->permanentPages() : QSet<uint64_t>{};
    const uint64_t mainBase = baseAddress();
    const uint64_t relBase = viewTree().baseAddress;
    m_refreshWatcher->setFuture(QtConcurrent::run(
        [prov, pageList, baseline, diffGen, permanent, intendedPages, plan, mainBase, relBase, pointerBudget]() -> ReadBatch {
        ReadBatch batch;
        batch.diffGen = diffGen;
        const int pageSize = static_cast<int>(kPageSize);
        QHash<uint64_t, QByteArray> fresh;    // every page read OK this tick
        QSet<uint64_t> refused;

        auto readPage = [&](uint64_t p) {
            QByteArray page(pageSize, Qt::Uninitialized);
            // read(), not readBytes(): readBytes zero-fills a refused read
            // and discards the verdict, so unmapped memory used to merge
            // into the snapshot as real zero bytes and never showed as
            // unreadable. Deliberately one page per call: the process
            // plugins report a PARTIAL multi-page read as success with the
            // tail zero-filled, which would bring that bug straight back.
            if (!prov->read(p, page.data(), pageSize)) {
                batch.unreadable.append(p);
                refused.insert(p);
                return;
            }
            if (p == 0)
                batch.page0AllZero = std::all_of(page.cbegin(), page.cend(),
                                                 [](char c) { return c == 0; });
            auto old = baseline.constFind(p);
            if (old == baseline.constEnd() || old->size() != pageSize) {
                batch.firstSeen.append(p);
                batch.pages.insert(p, page);
            } else if (diffPageRuns(batch.runs, p, old->constData(),
                                    page.constData(), pageSize)) {
                batch.changed.append(p);
                batch.pages.insert(p, page);
            } else {
                batch.unchanged.append(p);
            }
            fresh.insert(p, page);
        };
        for (uint64_t p : pageList) readPage(p);

        // ── Pointer targets, from the bytes this tick just read ──
        const QSet<uint64_t> watched(intendedPages.cbegin(), intendedPages.cend());
        QSet<uint64_t> targets;
        auto bytesOf = [&](uint64_t page) -> const QByteArray* {
            auto it = fresh.constFind(page);
            if (it != fresh.constEnd()) return &it.value();
            if (refused.contains(page)) return nullptr;
            // Skipped this tick (stable backstage, or permanent module
            // memory): its last bytes still stand.
            if (watched.contains(page) || permanent.contains(page)) {
                auto b = baseline.constFind(page);
                if (b != baseline.constEnd() && b->size() == pageSize) return &b.value();
            }
            readPage(page);
            it = fresh.constFind(page);
            return it == fresh.constEnd() ? nullptr : &it.value();
        };
        auto readAt = [&](uint64_t addr, char* out, int len) -> bool {
            uint64_t cur = addr;
            int remaining = len;
            while (remaining > 0) {
                const uint64_t page = cur & kPageMask;
                const int off = int(cur - page);
                const int chunk = qMin(remaining, pageSize - off);
                targets.insert(page);
                const QByteArray* b = bytesOf(page);
                if (!b) return false;
                std::memcpy(out, b->constData() + off, size_t(chunk));
                out += chunk;
                cur += uint64_t(chunk);
                remaining -= chunk;
                if (cur == 0 && remaining > 0) return false;
            }
            return true;
        };
        auto touch = [&](uint64_t addr, int64_t len) {
            if (len <= 0) return;
            const uint64_t last = (addr > UINT64_MAX - uint64_t(len - 1)) ? UINT64_MAX : addr + uint64_t(len - 1);
            for (uint64_t page = addr & kPageMask;; page += kPageSize) {
                targets.insert(page);
                bytesOf(page);
                if (page + (kPageSize - 1) >= last) break;
            }
        };
        if (plan && plan->root >= 0) {
            int64_t budget = pointerBudget;
            QSet<QPair<int, uint64_t>> visited;
            std::function<void(int, uint64_t, int)> resolve = [&](int si, uint64_t base, int depth) {
                if (depth > 99 || budget <= 0) return;
                const QPair<int, uint64_t> key(si, base);
                if (visited.contains(key)) return;
                visited.insert(key);
                for (const PlanPtr& pp : plan->structs[si].ptrs) {
                    if (budget <= 0) return;
                    auto isNull = [&pp](uint64_t x) {
                        return x == 0 || x == UINT64_MAX || (pp.size == 4 && x == 0xFFFFFFFFull);
                    };
                    uint64_t v = 0;
                    if (!readAt(base + uint64_t(pp.offset), reinterpret_cast<char*>(&v), pp.size)) continue;
                    if (isNull(v)) continue;
                    if (pp.relative) v += relBase;
                    for (int d = 0; d < pp.derefs && v; ++d) {
                        uint64_t next = 0;
                        if (!readAt(v, reinterpret_cast<char*>(&next), pp.size) || isNull(next)) { v = 0; break; }
                        v = next;
                    }
                    if (!v) continue;
                    if (pp.targetStruct >= 0) {
                        const int span = plan->structs[pp.targetStruct].span;
                        if (span <= 0) continue;
                        budget -= span;
                        touch(v, span);
                        resolve(pp.targetStruct, v, depth + 1);
                    } else if (pp.targetLen > 0) {
                        budget -= pp.targetLen;
                        touch(v, pp.targetLen);
                    }
                }
            };
            resolve(plan->root, mainBase, 0);
        }

        // What this tick watched: the main range, touched pages and every
        // pointer target — the timeline's coverage.
        batch.intended = intendedPages;
        for (uint64_t p : std::as_const(targets)) batch.intended.append(p);
        std::sort(batch.intended.begin(), batch.intended.end());
        batch.intended.erase(std::unique(batch.intended.begin(), batch.intended.end()), batch.intended.end());
        normalizeRuns(batch.runs);
        return batch;
    }));
}

void RcxController::onReadComplete() {
    m_readInFlight = false;

    if (m_readGen != m_refreshGen) return;

    ReadBatch batch;
    try {
        batch = m_refreshWatcher->result();
    } catch (const std::exception& e) {
        qWarning() << "[Refresh] async read threw:" << e.what();
        m_lastReadOk = false;
        return;
    } catch (...) {
        qWarning() << "[Refresh] async read threw unknown exception";
        m_lastReadOk = false;
        return;
    }

    // All-zero guard: if page 0 is all zeros and we already have data, discard
    if (!m_prevPages.isEmpty() && batch.page0AllZero) {
        if (!m_loggedAllZeroPage0) {
            qDebug() << "[Refresh] discarding all-zero page-0, keeping stale snapshot (further occurrences silenced)";
            m_loggedAllZeroPage0 = true;
        }
        m_lastReadOk = false;
        return;
    }
    // First successful non-all-zero refresh — re-arm the log latch so a
    // future all-zero burst gets a single line again.
    m_loggedAllZeroPage0 = false;
    // Live but every page refused: that is a failing read, not a live one.
    const bool anyReadOk = !batch.firstSeen.isEmpty() || !batch.changed.isEmpty()
                        || !batch.unchanged.isEmpty();
    m_lastReadOk = anyReadOk || batch.unreadable.isEmpty();

    // A resetChangeTracking() while the read was in flight threw away the
    // baseline the worker diffed against, so its "changed" is relative to
    // bytes the user just asked us to forget. Keep the bytes, drop the verdict.
    const bool staleBaseline = (batch.diffGen != m_diffGen);
    bool firstSnapshot = m_prevPages.isEmpty();
    bool anyChanged = false;
    m_changedRuns.clear();
    if (!staleBaseline) {
        m_changedRuns = batch.runs;
        anyChanged = !batch.changed.isEmpty();
    }

    // Per-page stability. First sight isn't a value mutation, so it neither
    // counts as a change nor as quiet; a refused page stays hot so it is
    // retried at full rate.
    for (uint64_t p : batch.firstSeen)  m_pageStability[p] = 0;
    for (uint64_t p : batch.changed)    m_pageStability[p] = 0;
    for (uint64_t p : batch.unreadable) m_pageStability[p] = 0;
    for (uint64_t p : batch.unchanged)
        m_pageStability[p] = qMin(kStabilityThreshold + 16, m_pageStability.value(p, 0) + 1);

    // Adaptive: count consecutive ticks with zero observed change.
    if (anyChanged) {
        m_idleTicks = 0;
    } else if (!firstSnapshot) {
        ++m_idleTicks;
    }
    applyAdaptiveInterval();

    int mainExtent = computeDataExtent();

    // A page turning unreadable, or readable again, changes what is on
    // screen (the unreadable strike) even when no byte value moved.
    bool readabilityChanged = false;
    for (uint64_t p : batch.unreadable)
        if (!m_snapshotProv || !m_snapshotProv->isFailed(p)) { readabilityChanged = true; break; }
    if (!readabilityChanged && m_snapshotProv)
        for (uint64_t p : batch.firstSeen)
            if (m_snapshotProv->isFailed(p)) { readabilityChanged = true; break; }

    // Merge only what is new or changed. Pages we skipped this tick
    // (backstage / permanent) keep their previous bytes, and so do pages
    // that read back identical — re-inserting an equal page every tick
    // swapped a fresh buffer into both maps for nothing.
    for (auto it = batch.pages.constBegin(); it != batch.pages.constEnd(); ++it)
        m_prevPages.insert(it.key(), it.value());
    for (uint64_t p : batch.unreadable)
        m_prevPages.remove(p);

    if (m_snapshotProv) {
        m_snapshotProv->mergePages(batch.pages, mainExtent);
    } else {
        m_snapshotProv = std::make_unique<SnapshotProvider>(
            this->provider(), batch.pages, mainExtent);
    }
    m_snapshotProv->markFailed(batch.unreadable);

    // Speedup 4: classify newly-fetched pages as permanent if they fall
    // in a read-only module section — module memory doesn't change at
    // runtime, so we can skip them on every subsequent tick. Must run
    // after m_snapshotProv exists, otherwise the helper early-returns.
    classifyPermanentPages(batch.pages);

    // Hand the tick to the timeline before composing: refresh() consumes the
    // user-edit ranges the timeline needs to tell your writes from the target's.
    feedTimeline(batch);

    // Compose only when something actually changed (or this is the
    // first snapshot — there's nothing on screen yet). Not while the view
    // shows the past — reads (and a recording) continue underneath, the past
    // does not change — and not while minimised: one compose on restore
    // instead.
    if (anyChanged || firstSnapshot || readabilityChanged) {
        if (!m_windowVisible)
            m_viewStale = true;
        else if (!isViewingPast())
            refresh();
    }
    m_changedRuns.clear();
}

// ── Speedup 1: viewport address range ──
std::optional<QPair<uint64_t, uint64_t>>
RcxController::viewportAddressRange() const {
    if (m_editors.isEmpty()) return std::nullopt;
    bool any = false;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (auto* editor : m_editors) {
        if (!editor || !editor->scintilla()) continue;
        auto* sci = editor->scintilla();
        int firstLine = sci->firstVisibleLine();
        int onScreen  = (int)sci->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN);
        // Scintilla counts visual (display) lines; metaForLine takes a
        // document line. SCI_DOCLINEFROMVISIBLE bridges the two when
        // wrap is on. (Our editor uses no wrap by default but the call
        // is safe either way.)
        for (int v = firstLine; v < firstLine + onScreen; ++v) {
            int docLine = (int)sci->SendScintilla(QsciScintillaBase::SCI_DOCLINEFROMVISIBLE, v);
            const LineMeta* lm = editor->metaForLine(docLine);
            if (!lm) continue;
            uint64_t addr = lm->offsetAddr;
            if (addr == 0) continue;  // synthetic / commandRow / no address
            if (addr < lo) lo = addr;
            // Conservative high-water — assume each visible line spans
            // up to 16 bytes (largest hex preview). Overscan in the
            // caller widens this further.
            uint64_t hiCandidate = addr + 16;
            if (hiCandidate > hi) hi = hiCandidate;
            any = true;
        }
    }
    if (!any) return std::nullopt;
    return QPair<uint64_t, uint64_t>{lo, hi};
}

// ── Speedup 4: classify pages whose region is read-only module memory ──
void RcxController::classifyPermanentPages(const PageMap& fresh) {
    if (!m_snapshotProv || !this->provider()) return;
    // enumerateRegions() is a FULL VirtualQueryEx sweep of the target's address
    // space — 10s of ms on a process with thousands of mappings (a big game like
    // DayZ). The old code here re-ran it on EVERY refresh tick on the main thread
    // (the "already-cached" comment was wrong: ProcessMemoryProvider re-sweeps on
    // every call, and we ask this->provider(), not a cached wrapper), which is what
    // made module-heavy targets barely usable. Cache the list and refresh it at
    // most every kRegionRefreshTicks ticks: the executable module regions we mark
    // permanent are stable, so a slightly stale list only means a just-loaded
    // module's pages take an extra cycle to be marked permanent — never a
    // correctness issue.
    if (!m_classifyRegionsValid
        || (m_tickCount - m_classifyRegionsTick) >= (uint64_t)kRegionRefreshTicks) {
        m_classifyRegions = this->provider()->enumerateRegions();
        m_classifyRegionsValid = true;
        m_classifyRegionsTick = m_tickCount;
    }
    const auto& regions = m_classifyRegions;
    if (regions.isEmpty()) return;
    constexpr uint64_t kPageSize = 4096;
    for (auto it = fresh.constBegin(); it != fresh.constEnd(); ++it) {
        uint64_t pageAddr = it.key();
        if (m_snapshotProv->isPermanent(pageAddr)) continue;
        for (const auto& r : regions) {
            if (r.moduleName.isEmpty()) continue;
            if (pageAddr < r.base) continue;
            if (pageAddr + kPageSize > r.base + r.size) continue;
            if (!r.executable) continue;
            m_snapshotProv->markPermanent(pageAddr);
            break;
        }
    }
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

    int provSize = this->provider()->size();
    if (provSize > 0) return provSize;
    return 0;
}

void RcxController::resetSnapshot() {
    m_refreshGen++;
    m_readInFlight = false;
    m_snapshotProv.reset();
    m_prevPages.clear();
    m_changedRuns.clear();
    ++m_diffGen;
    // Touched pages are addresses in the old address space / at the old base.
    m_touchedAge.clear();
    m_valueHistory.clear();
    m_lastValueAddr.clear();
    m_lastValueBytes.clear();
    // Speedup-related state — module identity and page stability are
    // both per-attach. Switching processes (resetProvider →
    // resetSnapshot) must drop these or stale data leaks across.
    m_pageStability.clear();
    m_classifyRegions.clear();
    m_classifyRegionsValid = false;
    m_classifyRegionsTick = 0;
    m_idleTicks = 0;
    m_tickCount = 0;
    // Every rebase, attach and source switch lands here. The timeline's
    // history is NOT reset with the snapshot: it re-binds (same process →
    // same history, with a base-address epoch; another process → a new one).
    bindTimeline();
    applyAdaptiveInterval();  // restart timer if it was paused
}

// ─────────────────────────────────────────────────────────────────────────
//  Class timeline
// ─────────────────────────────────────────────────────────────────────────

std::shared_ptr<tl::TimelineHub> RcxController::ensureTimelineHub() {
    if (m_instance) {
        if (!m_ownTimelineHub) {
            m_ownTimelineHub = std::make_shared<tl::TimelineHub>();
            QPointer<RcxController> self(this);
            m_ownTimelineHub->onChanged = [self]() { if (self) self->scheduleTimelineChanged(); };
        }
        return m_ownTimelineHub;
    }
    if (!m_doc) return {};
    if (!m_doc->timeline) {
        m_doc->timeline = std::make_shared<tl::TimelineHub>();
        QPointer<RcxDocument> doc(m_doc.data());
        m_doc->timeline->onChanged = [doc]() { if (doc) emit doc->timelineStateChanged(); };
    }
    return m_doc->timeline;
}

void RcxController::bindTimeline() {
    m_timelineHub = ensureTimelineHub();
    const std::shared_ptr<Provider>& prov = provider();
    const int64_t now = tl::CaptureClock::nowMs();

    std::shared_ptr<tl::CaptureContext> ctx;
    if (m_timelineEnabled && prov && prov->isLive())
        ctx = tl::TimelineService::instance().contextFor(prov);

    if (ctx != m_timelineCtx) {
        // Another source: the past being shown belongs to the old history.
        if (isViewingPast()) leavePast();
        detachTimeline();
        m_timelineCtx = ctx;
        m_timelineCoverage.clear();
        m_timelineSeeded = false;
        // Another history: count its fields afresh on the next compose.
        m_classChanges.clear();
        ++m_classChangesVersion;
        ++m_classChangesQuery;
        m_fieldLayoutKey = 0;
        if (m_timelineCtx) {
            m_timelineProducer = m_timelineCtx->addProducer();
            m_timelineCtx->setWantSpans(true);
            QPointer<RcxController> self(this);
            m_timelineListener = m_timelineCtx->addListener([self](const tl::CommitBatch& b) {
                if (!self) return;
                self->onTimelineCommit(b);
                self->scheduleTimelineChanged();
            });
        }
    }

    if (m_timelineHub) {
        if (m_timelineCtx) {
            const auto& svc = tl::TimelineService::instance();
            tl::TimelineHub::Budgets b;
            b.rollingBytes = svc.rollingBudgetPerContext();
            b.rollingWindowMs = svc.budgets().rollingWindowMs;
            b.recordingBytes = svc.budgets().recordRamBytes;
            b.recordingDiskBytes = svc.budgets().recordDiskBytes;
            m_timelineHub->setBudgets(b);
            m_timelineHub->bindSource(m_timelineCtx, prov->name(), now);
            const uint64_t base = baseAddress();
            if (m_timelineHub->baseAt(INT64_MAX, base) != base)
                m_timelineHub->noteEvent(tl::TimelineEventKind::Rebase,
                    QStringLiteral("0x%1").arg(base, 0, 16), now);
            m_timelineHub->noteBase(base, now);
        } else if (!m_timelineEnabled) {
            // Switched off: nothing is kept, not even what was captured.
            m_timelineHub->clearHistory();
        } else if (m_timelineHub->context() && (!prov || !prov->isLive())) {
            m_timelineHub->unbindSource(prov ? prov->name() : QString(), now);
        }
    }
    applyAdaptiveInterval();
    scheduleTimelineChanged();
}

void RcxController::detachTimeline() {
    if (!m_timelineCtx) return;
    m_timelineCtx->removeListener(m_timelineListener);
    m_timelineCtx->cancelFrames(m_timelineRequester);
    // Its watched pages leave coverage: a coverage-only record, so the
    // history says "not watched from here", never "unchanged".
    m_timelineCtx->removeProducer(m_timelineProducer, tl::CaptureClock::nowMs());
    m_timelineCtx.reset();
    m_timelineListener = 0;
    m_timelineProducer = 0;
}

void RcxController::feedTimeline(const ReadBatch& batch) {
    if (!m_timelineCtx) return;
    // Nothing is kept until Record is pressed, and nothing after Stop.
    if (!timelineRecording()) {
        leaveTimelineCoverage();
        return;
    }
    tl::TickInput in;
    in.producer = m_timelineProducer;
    in.readStartMs = m_readStartMs;
    in.timeMs = tl::CaptureClock::nowMs();
    in.pages = batch.pages;                 // shared buffers, not copies
    in.unreadable = batch.unreadable;
    in.partialSampling = m_inflightPartial;
    const bool seed = !m_timelineSeeded;
    const bool coverageChanged = seed || batch.intended != m_timelineCoverage;
    if (coverageChanged) {
        // A page entering coverage starts with no bytes in the store — every
        // page on a recording's first tick; later a pointer target or touched
        // page coming back — and an unchanged page is not in this tick's
        // read, so hand over the snapshot's copy (this tick already merged).
        // Both page lists are sorted and unique.
        auto covIt = m_timelineCoverage.cbegin();
        const auto covEnd = m_timelineCoverage.cend();
        for (uint64_t page : batch.intended) {
            if (!seed) {
                while (covIt != covEnd && *covIt < page) ++covIt;
                if (covIt != covEnd && *covIt == page) continue;   // already covered
            }
            if (in.pages.contains(page)) continue;
            auto it = m_prevPages.constFind(page);
            if (it != m_prevPages.constEnd()) in.pages.insert(page, it.value());
        }
    }
    if (coverageChanged) in.coverage = batch.intended;
    for (const auto& r : m_userEditRanges)
        if (r.second > r.first) in.userEdits.append({r.first, r.second - r.first});
    if (in.pages.isEmpty() && in.unreadable.isEmpty() && !in.coverage) return;   // idle
    const bool accepted = m_timelineCtx->append(std::move(in)) == tl::AppendStatus::Accepted;
    if (accepted) {
        if (coverageChanged) m_timelineCoverage = batch.intended;
        m_timelineSeeded = true;
    }
}

void RcxController::leaveTimelineCoverage() {
    if (!m_timelineCtx || !m_timelineSeeded) return;
    // A coverage-only record: from here this tab's pages read "not recorded"
    // in the history, never "unchanged". Another tab recording the same
    // pages keeps its own coverage.
    tl::TickInput in;
    in.producer = m_timelineProducer;
    in.timeMs = tl::CaptureClock::nowMs();
    in.readStartMs = in.timeMs;
    in.coverage = QVector<uint64_t>{};
    m_timelineCtx->append(std::move(in));
    m_timelineCoverage.clear();
    m_timelineSeeded = false;
}

void RcxController::scheduleTimelineChanged() {
    if (m_timelineChangedPending) return;
    m_timelineChangedPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_timelineChangedPending = false;
        // The address bar's buttons read the same state as the strip.
        if (m_doc) pushAddressBarState();
        emit timelineChanged();
    });
}

tl::TimelineHub* RcxController::timelineHub() const { return m_timelineHub.get(); }
tl::CaptureContext* RcxController::timelineContext() const { return m_timelineCtx.get(); }

uint64_t RcxController::timelineClassId() const {
    if (m_viewRootId) return m_viewRootId;
    for (const Node& n : m_doc->tree.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) return n.id;
    return 0;
}

int64_t RcxController::pastTimeMs() const {
    // The moment asked for, remembered when it was chosen: the record behind
    // it may since have aged out of the model, and the moment itself may sit
    // between two records.
    return isViewingPast() ? m_pastViewMs : 0;
}

bool RcxController::timelineRecording() const {
    return m_timelineCtx && m_timelineHub
        && m_timelineHub->mode(timelineClassId()) == tl::CaptureMode::Recording;
}

QString RcxController::viewingPastHint() const {
    return QStringLiteral("Looking back at a recording: values are read-only. Back to live (Ctrl+End) to edit.");
}

void RcxController::viewTimelineAt(int64_t timeMs) {
    if (!m_timelineCtx || m_timelineCtx->model().isEmpty()) return;
    const tl::TimelineModel& m = m_timelineCtx->model();
    tl::RecordId r = m.recordAtOrBefore(timeMs);
    if (!m_timelineHub) {
        if (r == tl::kNoRecord) r = m.firstRecord();
        viewTimelineRecord(r);
        return;
    }
    // Only this class's own recording: nothing from before its first Record
    // or a Clear, and nothing another class or tab recorded meanwhile.
    const uint64_t cls = timelineClassId();
    const tl::RecordId first = m_timelineHub->firstVisibleRecord(cls);
    if (first == tl::kNoRecord) return;
    // A moment inside a not-recorded stretch (after Stop) shows the last
    // recorded moment before it.
    for (int guard = 0; guard < 64 && r != tl::kNoRecord && r >= first
                        && m_timelineHub->isHiddenFor(cls, m.timeOf(r)); ++guard) {
        const int64_t at = m.timeOf(r);
        const tl::Gap* gap = nullptr;
        for (const tl::Gap& g : m_timelineHub->track(cls).pauses)
            if (g.contains(at)) { gap = &g; break; }
        if (!gap) { r = first; break; }   // before the floor or the first Record
        r = m.recordAtOrBefore(gap->startMs - 1);
    }
    if (r == tl::kNoRecord || r < first) r = first;
    // The playhead stays where it was asked for — inside a not-recorded
    // stretch included, where the frame shown is the last one before it.
    viewTimelineRecordAt(r, timeMs);
}

void RcxController::viewTimelineRecord(tl::RecordId record) {
    if (!m_timelineCtx || !m_timelineCtx->model().contains(record)) return;
    viewTimelineRecordAt(record, m_timelineCtx->model().timeOf(record));
}

void RcxController::viewTimelineRecordAt(tl::RecordId record, int64_t viewMs) {
    if (!m_timelineCtx || !m_timelineCtx->model().contains(record)) return;
    m_pastRecordMs = m_timelineCtx->model().timeOf(record);
    m_pastViewMs = viewMs;
    // Scrubbing between two records keeps the same frame but moves the
    // playhead, so this must still tell the UI — hence before the early exit.
    if (m_pastRecord == record && m_frameProv) {
        scheduleTimelineChanged();
        return;
    }
    m_pastRecord = record;
    if (!m_frameProv)
        m_frameProv = std::make_unique<tl::TimelineFrameProvider>(provider(), nullptr, computeDataExtent());
    QPointer<RcxController> self(this);
    m_timelineCtx->requestFrame(m_timelineRequester, record, {}, this,
        [self](tl::FramePtr frame) { if (self) self->onPastFrame(std::move(frame)); });
    scheduleTimelineChanged();
}

void RcxController::onPastFrame(std::shared_ptr<const tl::Frame> frame) {
    if (!isViewingPast() || !m_frameProv || !frame) return;
    // A frame computed for a moment since replaced keeps the current one.
    if (frame->record != m_pastRecord) return;
    m_frameProv->setFrame(std::move(frame));
    m_frameProv->setExtent(computeDataExtent());
    refresh();
}

void RcxController::leavePast() {
    if (!isViewingPast()) return;
    m_pastRecord = tl::kNoRecord;
    if (m_timelineCtx) m_timelineCtx->cancelFrames(m_timelineRequester);
    m_frameProv.reset();
    refresh();
    scheduleTimelineChanged();
}

void RcxController::returnToLive() {
    leavePast();
}

void RcxController::timelineToggleRecording() {
    if (!m_timelineHub) return;
    m_timelineHub->toggleRecording(timelineClassId(), tl::CaptureClock::nowMs());
    // Stopped: this tab's pages leave the recording now, not on the next read.
    if (!timelineRecording()) leaveTimelineCoverage();
    // Recording reads on while unfocused or minimised; stopped, it need not.
    applyAdaptiveInterval();
    scheduleTimelineChanged();
}

void RcxController::timelineReset() {
    if (!m_timelineHub) return;
    if (isViewingPast()) leavePast();
    m_timelineHub->reset(timelineClassId(), tl::CaptureClock::nowMs());
    scheduleTimelineChanged();
}

void RcxController::beginTimelineScrub() {
    m_scrubSnapshot = ScrubSnapshot{true, m_pastRecord, m_pastRecordMs, m_pastViewMs};
}

void RcxController::cancelTimelineScrub() {
    const ScrubSnapshot s = m_scrubSnapshot;
    m_scrubSnapshot = ScrubSnapshot{};
    if (!s.active) return;
    if (s.record == tl::kNoRecord) {
        // Out of the past: live again.
        leavePast();
        return;
    }
    // The same record, at the time it was chosen (it may have aged out since).
    if (!m_timelineCtx || !m_timelineCtx->model().contains(s.record)) return;
    viewTimelineRecordAt(s.record, s.viewMs);
    m_pastRecordMs = s.recordMs;
    scheduleTimelineChanged();
}

bool RcxController::stepTimelineChange(int dir) {
    if (!m_timelineCtx || m_timelineCtx->model().isEmpty() || dir == 0) return false;
    const tl::TimelineModel& m = m_timelineCtx->model();
    const uint64_t cls = timelineClassId();
    tl::RecordId first = m_timelineHub ? m_timelineHub->firstVisibleRecord(cls) : m.firstRecord();
    if (first == tl::kNoRecord) return false;
    auto hidden = [&](tl::RecordId r) {
        return m_timelineHub && m_timelineHub->isHiddenFor(cls, m.timeOf(r));
    };
    if (timelineCountsFields()) {
        // Change = a field of this class changed; with rows selected, one of
        // THEM changed — "when did health last move?".
        const QVector<tl::FieldSpan> scope = timelineSelectionScope();
        const QVector<tl::FieldSpan>* sc = scope.isEmpty() ? nullptr : &scope;
        if (dir < 0) {
            tl::RecordId r = isViewingPast() ? m_pastRecord : m.lastRecord() + 1;
            while ((r = m_classChanges.previous(r, sc, first)) != tl::kNoRecord)
                if (m.contains(r) && !hidden(r)) { viewTimelineRecord(r); return true; }
            emit statusHint(sc ? QStringLiteral("No earlier change of %1 was captured.").arg(timelineScopeLabel())
                               : QStringLiteral("No earlier change was captured."));
            return false;
        }
        if (!isViewingPast()) return false;
        tl::RecordId r = m_pastRecord;
        while ((r = m_classChanges.next(r, sc)) != tl::kNoRecord)
            if (m.contains(r) && !hidden(r)) { viewTimelineRecord(r); return true; }
        returnToLive();   // past the newest change is now
        return true;
    }
    auto eligible = [&](tl::RecordId r) { return m.changedBytesOf(r) > 0 && !hidden(r); };
    if (dir < 0) {
        tl::RecordId r = isViewingPast() ? m_pastRecord : m.lastRecord() + 1;
        while (r > first) {
            --r;
            if (eligible(r)) { viewTimelineRecord(r); return true; }
        }
        return false;
    }
    if (!isViewingPast()) return false;
    for (tl::RecordId r = m_pastRecord + 1; r <= m.lastRecord(); ++r)
        if (eligible(r)) { viewTimelineRecord(r); return true; }
    returnToLive();   // past the newest change is now
    return true;
}

void RcxController::setTimelineBackgroundCapture(bool on) {
    m_timelineBackgroundCapture = on;
    applyAdaptiveInterval();
}

void RcxController::setTimelineEnabled(bool on) {
    if (m_timelineEnabled == on) return;
    m_timelineEnabled = on;
    // Its buttons go with it: a recorded moment left on screen would have no
    // way back to live.
    if (!on && isViewingPast()) returnToLive();
    bindTimeline();
}

const Provider* RcxController::displayedProvider() const {
    if (isViewingPast() && m_frameProv && m_frameProv->frame()) return m_frameProv.get();
    if (m_snapshotProv) return m_snapshotProv.get();
    return provider() ? provider().get() : nullptr;
}

// The bytes a composed row's value shows; 0 for a row with no value of its
// own (an open struct's header, a footer).
int RcxController::rowValueBytes(const NodeTree& tree, const LineMeta& lm) {
    if (lm.lineKind != LineKind::Field || lm.nodeIdx < 0 || lm.nodeIdx >= tree.nodes.size()) return 0;
    const Node& n = tree.nodes[lm.nodeIdx];
    if (lm.isArrayElement) return qMax(0, sizeForKind(lm.elementKind));
    if (n.kind == NodeKind::Struct && n.classKeyword != QStringLiteral("bitfield")) return 0;
    return qMax(0, n.byteSize());
}

QString RcxController::fieldDisplayName(uint64_t selId) const {
    const NodeTree& tree = viewTree();
    const int idx = tree.indexOfId(baseNodeIdFromSelId(selId));
    if (idx < 0) return QStringLiteral("?");
    const Node& n = tree.nodes[idx];
    QString name = n.name.isEmpty() ? n.structTypeName : n.name;
    if (selId & kArrayElemBit) name += QStringLiteral("[%1]").arg(arrayElemIdxFromSelId(selId));
    return name;
}

void RcxController::rebuildFieldIndex() {
    const NodeTree& tree = viewTree();
    const uint64_t cls = timelineClassId();
    // A recount is for the LAYOUT changing (retype, resize, fold, another
    // class); the base moving or a pointer re-pointing only moves the spans.
    const quint64 layoutKey = (tree.generation() * 0x9E3779B97F4A7C15ull)
                            ^ (m_capturePlanEpoch * 0xC2B2AE3D27D4EB4Full) ^ (cls + 1);
    const uint64_t base = baseAddress();
    const bool layoutChanged = layoutKey != m_fieldLayoutKey;
    const bool pointersMove = m_capturePlan && m_capturePlan->root >= 0
                           && !m_capturePlan->structs[m_capturePlan->root].ptrs.isEmpty();
    if (!layoutChanged && base == m_fieldIndexBase && !pointersMove) return;

    QHash<uint64_t, QVector<int>> childMap;
    bool childMapBuilt = false;
    QVector<tl::FieldSpan> fields;
    fields.reserve(m_lastResult.meta.size());
    for (const LineMeta& lm : m_lastResult.meta) {
        if (lm.nodeIdx < 0 || lm.nodeIdx >= tree.nodes.size()) continue;
        if (lm.lineKind == LineKind::Field) {
            const int len = rowValueBytes(tree, lm);
            if (len > 0) fields.append({lm.offsetAddr, uint32_t(len), selIdForLine(lm)});
            continue;
        }
        if (lm.lineKind != LineKind::Header) continue;
        const Node& n = tree.nodes[lm.nodeIdx];
        int len = 0;
        if (isPointerKind(n.kind)) {
            len = n.byteSize();                    // an open pointer's own bytes
        } else if (lm.foldCollapsed) {
            // A folded struct or array is one field: anything inside it.
            if (!childMapBuilt) {
                for (int i = 0; i < tree.nodes.size(); ++i) childMap[tree.nodes[i].parentId].append(i);
                childMapBuilt = true;
            }
            len = tree.structSpan(n.id, &childMap);
            if (len <= 0 && n.kind == NodeKind::Array && n.refId)
                len = int(qMin<int64_t>(INT_MAX, int64_t(tree.structSpan(n.refId, &childMap)) * n.arrayLen));
        }
        if (len > 0) fields.append({lm.offsetAddr, uint32_t(len), lm.nodeId});
    }
    m_fieldIndex.build(std::move(fields));
    m_fieldIndexBase = base;
    ++m_fieldIndexGeneration;
    if (layoutChanged) {
        m_fieldLayoutKey = layoutKey;
        recountClassChanges();
    }
}

void RcxController::countRecord(tl::ClassChangeSeries& series, tl::RecordId r, int64_t tMs,
                                const QVector<tl::ChangedSpan>& spans, QVector<int>& hits) const {
    if (spans.isEmpty()) return;
    // A record from before a rebase is counted where the class was then.
    const uint64_t base = baseAddress();
    const uint64_t thenBase = m_timelineHub ? m_timelineHub->baseAt(tMs, base) : base;
    if (thenBase != base) {
        QVector<tl::ChangedSpan> moved = spans;
        for (tl::ChangedSpan& s : moved) s.addr += base - thenBase;
        m_fieldIndex.hits(moved, hits);
    } else {
        m_fieldIndex.hits(spans, hits);
    }
    if (hits.isEmpty()) return;
    QVector<uint64_t> ids;
    ids.reserve(qMin(hits.size(), tl::ClassChangeSeries::kMaxIdsPerRecord));
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int h : std::as_const(hits)) {
        const tl::FieldSpan& f = m_fieldIndex.at(h);
        if (ids.size() < tl::ClassChangeSeries::kMaxIdsPerRecord) ids.append(f.id);
        lo = qMin(lo, f.addr);
        hi = qMax(hi, tl::FieldIndex::endOf(f.addr, f.len));
    }
    series.append(r, tMs, hits.size(), ids, lo, hi);
}

void RcxController::onTimelineCommit(const tl::CommitBatch& b) {
    bool changed = false;
    if (b.firstRetained != tl::kNoRecord && !m_classChanges.isEmpty()) {
        const int before = m_classChanges.size();
        m_classChanges.dropBefore(b.firstRetained);
        changed = m_classChanges.size() != before;
    }
    if (!m_fieldIndex.isEmpty() && !b.spans.isEmpty()) {
        QVector<int> hits;
        int j = 0;
        for (const auto& rs : b.spans) {
            while (j < b.appended.size() && b.appended[j].rec < rs.first) ++j;
            const int64_t t = (j < b.appended.size() && b.appended[j].rec == rs.first)
                            ? b.appended[j].timeMs
                            : (m_timelineCtx ? m_timelineCtx->model().timeOf(rs.first) : 0);
            const int size = m_classChanges.size();
            countRecord(m_classChanges, rs.first, t, rs.second, hits);
            changed |= m_classChanges.size() != size;
        }
    }
    if (changed) ++m_classChangesVersion;
    // A moment on screen that ages out of the store stays on screen: the
    // frame holds its pages, and pastTimeMs() reads the time cached when it
    // was chosen. Re-pinning to the oldest record would creep the view
    // forward on every trim.
}

void RcxController::recountClassChanges() {
    const int query = ++m_classChangesQuery;
    // The series is NOT cleared here. The refill below is queued on the
    // maintenance lane, behind every capture tick, so clearing up front blanked
    // the graph on any relayout — a fold, a tree edit, a view-root change — and
    // read as the timeline reloading. The callback swaps the rebuilt series in
    // atomically; until it lands the old counts keep drawing.
    if (!m_timelineCtx || m_timelineCtx->model().isEmpty() || m_fieldIndex.isEmpty()) {
        // Nothing to count against: here the old counts WOULD be a lie.
        m_classChanges.clear();
        ++m_classChangesVersion;
        scheduleTimelineChanged();
        return;
    }
    const tl::TimelineModel& m = m_timelineCtx->model();
    const tl::RecordId hi = m.lastRecord();
    tl::RecordId lo = m.firstRecord();
    constexpr tl::RecordId kMaxRecount = 200000;   // an 11-hour recording at 5 Hz
    if (hi - lo + 1 > kMaxRecount) lo = hi - kMaxRecount + 1;
    // Commits landing meanwhile are counted live into m_classChanges; the
    // recount goes in front of them when it arrives.
    QPointer<RcxController> self(this);
    m_timelineCtx->querySpans(lo, hi, this, [self, query](tl::CaptureContext::SpanList list) {
        if (!self || query != self->m_classChangesQuery || !self->m_timelineCtx) return;
        const tl::TimelineModel& model = self->m_timelineCtx->model();
        tl::ClassChangeSeries recount;
        QVector<int> hits;
        for (const auto& rs : list)
            if (model.contains(rs.first))
                self->countRecord(recount, rs.first, model.timeOf(rs.first), rs.second, hits);
        recount.appendNewer(self->m_classChanges);
        self->m_classChanges = std::move(recount);
        ++self->m_classChangesVersion;
        self->scheduleTimelineChanged();
    });
}

QVector<tl::FieldSpan> RcxController::timelineSelectionScope() const {
    if (m_selIds.isEmpty() || m_fieldIndex.isEmpty()) return {};
    const quint64 key = qHash(m_selIds) ^ (m_fieldIndexGeneration * 0x9E3779B97F4A7C15ull);
    if (key == m_scopeKey && m_scopeValid) return m_scope;
    QVector<tl::FieldSpan> scope = m_fieldIndex.spansOf(m_selIds);
    // An open struct or array row: every field laid out inside it.
    const NodeTree& tree = viewTree();
    for (uint64_t sel : m_selIds) {
        if (sel & (kFooterIdBit | kArrayElemBit | kMemberBit)) continue;
        const int idx = tree.indexOfId(sel);
        if (idx < 0 || !isContainerKind(tree.nodes[idx].kind)) continue;
        const int64_t off = tree.computeOffset(idx);
        const int span = tree.structSpan(sel);
        if (off < 0 || span <= 0) continue;
        const uint64_t lo = baseAddress() + uint64_t(off);
        const uint64_t hi = tl::FieldIndex::endOf(lo, uint64_t(span));
        for (const tl::FieldSpan& f : m_fieldIndex.fields())
            if (f.addr >= lo && tl::FieldIndex::endOf(f.addr, f.len) <= hi) scope.append(f);
    }
    m_scope = scope;
    m_scopeKey = key;
    m_scopeValid = true;
    return scope;
}

QString RcxController::timelineScopeLabel() const {
    if (timelineSelectionScope().isEmpty()) return {};
    if (m_selIds.size() == 1)
        return QStringLiteral("'%1'").arg(fieldDisplayName(*m_selIds.constBegin()));
    return QStringLiteral("%1 fields").arg(m_selIds.size());
}

QString RcxController::describeTimelineAt(int64_t timeMs) const {
    if (!m_timelineCtx) return {};
    const tl::TimelineModel& m = m_timelineCtx->model();
    const tl::RecordId r = m.recordAtOrBefore(timeMs);
    if (r == tl::kNoRecord) return QStringLiteral("Before capture began");
    // Parked between records: what is shown is the last capture before this
    // moment, and "nothing changed" would hide how far back that was.
    const int64_t age = timeMs - m.timeOf(r);
    if (age > 1000) {
        const double secs = age / 1000.0;
        return QStringLiteral("Unchanged — last change %1s earlier")
            .arg(secs < 10 ? QString::number(secs, 'f', 1)
                           : QString::number(qint64(secs + 0.5)));
    }
    if (!timelineCountsFields()) {
        const uint32_t bytes = m.changedBytesOf(r);
        return bytes ? QStringLiteral("%1 bytes changed").arg(bytes) : QStringLiteral("Nothing changed");
    }
    const int count = m_classChanges.countOf(r);
    if (count == 0)
        return m.changedBytesOf(r) ? QStringLiteral("Nothing in this class changed")
                                   : QStringLiteral("Nothing changed");
    QString text = count == 1 ? QStringLiteral("1 field changed")
                              : QStringLiteral("%1 fields changed").arg(count);
    const QVector<uint64_t> ids = m_classChanges.idsOf(r);
    QStringList names;
    for (uint64_t id : ids) {
        if (names.size() == 4) break;
        names << fieldDisplayName(id);
    }
    if (!names.isEmpty())
        text += QStringLiteral(": ") + names.join(QStringLiteral(", "))
              + (count > names.size() ? QStringLiteral(", …") : QString());
    return text;
}

void RcxController::revealSelectionInTimeline() {
    if (!m_timelineCtx || m_timelineCtx->model().isEmpty()) {
        emit statusHint(QStringLiteral("Nothing captured yet — the timeline records live sources."));
        return;
    }
    const QVector<tl::FieldSpan> scope = timelineSelectionScope();
    const tl::TimelineModel& m = m_timelineCtx->model();
    const QVector<int64_t> times = m_classChanges.times(INT64_MIN, INT64_MAX,
                                                        scope.isEmpty() ? nullptr : &scope, INT_MAX);
    if (times.isEmpty()) {
        emit statusHint(scope.isEmpty() ? QStringLiteral("No change of this class was captured.")
                                        : QStringLiteral("No change of %1 was captured.").arg(timelineScopeLabel()));
        emit timelineRevealRequested(m.timeOf(m.firstRecord()), tl::CaptureClock::nowMs());
        return;
    }
    const int64_t pad = qMax<int64_t>(500, (times.last() - times.first()) / 20);
    emit timelineRevealRequested(times.first() - pad, times.last() + pad);
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

bool RcxController::navigateToFormula(const QString& formula, QString* errOut) {
    // Kept for its callers (Goto dialog, bookmarks dock). The work is
    // rebaseTo, so a Goto is undoable and lands in the recent list like an
    // inline base edit does.
    return rebaseTo(formula, errOut);
}

bool RcxController::rebaseTo(const QString& expr, QString* err, bool recordHistory) {
    QString s = expr.trimmed();
    s.remove('`');          // WinDbg backtick separators (e.g. 7ff6`6cce0000)
    s.remove('\n');
    s.remove('\r');
    auto refuse = [&](const QString& why) {
        if (err) *err = why;
        emit statusHint(QStringLiteral("Base: ") + why);
        return false;
    };
    if (s.isEmpty()) return refuse(QStringLiteral("empty formula"));

    const AddressParserCallbacks cbs =
        makeAddressCallbacks(this->provider().get(), m_doc->tree.pointerSize);
    const auto result = AddressParser::evaluate(s, m_doc->tree.pointerSize, &cbs);
    if (!result.ok) return refuse(result.error);

    // Preserve the typed expression as the formula unless it is a bare
    // hex/decimal literal that round-trips identically through the canonical
    // "0xHEX" display — a literal formula would only shadow the number.
    const QString newFormula = isBareAddressLiteral(s) ? QString() : s;
    const uint64_t oldBase = baseAddress();
    const QString oldFormula = baseAddressFormula();
    if (result.value != oldBase || newFormula != oldFormula) {
        // The place being left, recorded only on the path that moves: the
        // same-base branch below changes nothing and leaves no entry.
        if (recordHistory) recordNav();
        // Values read at the new base are not "changes": arm the tracking
        // cooldown (what the scanner's Set-as-Base did by hand) before the
        // command's apply resets the snapshot and recomposes.
        resetChangeTracking();
        m_doc->undoStack.push(new RcxCommand(this,
            cmd::ChangeBase{oldBase, result.value, oldFormula, newFormula}));
        // The command's apply recomposed but announced nothing (undo-stack
        // pushes never emit documentChanged). The bookmarks dock, the doc-tab
        // source icon and the MCP bridge listen to documentChanged — and so
        // does this controller (→ refresh), which makes this the canonical
        // post-rebase refresh for every entry point, the pair the old
        // navigateToFormula fired. Emitted once; the inline edit's own
        // trailing refresh is skipped on success.
        emit m_doc->documentChanged();
    } else {
        // Same base, same formula: nothing to record or announce. The inline
        // edit still needs its typed text replaced by the canonical row.
        refresh();
    }
    // m_focusPath is deliberately kept: the expanded chain is still on
    // screen, and reconcileFocusPath keeps it structurally honest.
    GotoAddressDialog::pushRecent(s);
    return true;
}

// ── Navigation history ──

RcxEditor* RcxController::gestureEditor(RcxEditor* from) const {
    if (from) return from;
    // Inside a slot, sender() is the pane whose signal is being handled
    // (a direct call from that slot still sees it); outside one it is null.
    if (auto* ed = qobject_cast<RcxEditor*>(sender())) return ed;
    return primaryEditor();
}

QVector<NavEntry> RcxController::backEntries() const {
    QVector<NavEntry> out;
    for (const NavEntry& e : m_nav.backEntries())
        if (navEntryValid(m_doc->tree, e)) out.push_back(e);
    return out;
}

QVector<NavEntry> RcxController::forwardEntries() const {
    QVector<NavEntry> out;
    for (const NavEntry& e : m_nav.forwardEntries())
        if (navEntryValid(m_doc->tree, e)) out.push_back(e);
    return out;
}

bool RcxController::canGoBack() const {
    // Validity-aware, so a Back cell is never lit for entries a delete
    // made unrestorable (back() would discard them and do nothing).
    for (const NavEntry& e : m_nav.backEntries())
        if (navEntryValid(m_doc->tree, e)) return true;
    return false;
}

bool RcxController::canGoForward() const {
    for (const NavEntry& e : m_nav.forwardEntries())
        if (navEntryValid(m_doc->tree, e)) return true;
    return false;
}

NavEntry RcxController::currentNavEntry(RcxEditor* from) const {
    const NodeTree& tree = m_doc->tree;
    NavEntry e;
    e.viewRootId      = m_viewRootId;
    e.focusPath       = m_focusPath;
    e.baseAddress     = baseAddress();
    e.baseFormula     = baseAddressFormula();
    e.activeSourceIdx = m_activeSourceIdx;
    // The anchor: the node on the pane's first visible row, so a restore
    // scrolls back to what was on screen and not merely to the class
    // header. The meta index IS the visible line (compose emits only
    // visible rows — scrollNodeToTop relies on the same fact).
    if (!from) from = primaryEditor();
    if (from && from->scintilla()) {
        const int first = (int)from->scintilla()->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
        if (first >= 0 && first < m_lastResult.meta.size())
            e.anchorNodeId = m_lastResult.meta[first].nodeId;
    }
    e.label = currentNavLabel();
    return e;
}

QString RcxController::currentNavLabel() const {
    const NodeTree& tree = m_doc->tree;
    return trailPathText(tree, m_viewRootId, m_focusPath)
         + QStringLiteral("  @ 0x") + QString::number(baseAddress(), 16).toUpper();
}

void RcxController::recordNav(RcxEditor* from) {
    // A restore's own source switch and the constructor's auto-attach go
    // through the same gestures; neither is a place the user left.
    if (m_navRestoring || m_loading) return;
    m_nav.push(currentNavEntry(gestureEditor(from)));
    emit historyChanged(canGoBack(), canGoForward());
}

void RcxController::goBack(RcxEditor* from) {
    jumpToHistory(-1, from);
}

void RcxController::goForward(RcxEditor* from) {
    jumpToHistory(+1, from);
}

void RcxController::goUp(RcxEditor* from) {
    if (m_focusPath.isEmpty()) return;
    collapseToFocusIn(m_focusPath.size() - 1, gestureEditor(from));
}

void RcxController::jumpToHistory(int delta, RcxEditor* from) {
    if (delta == 0) return;
    from = gestureEditor(from);
    auto valid = [this](const NavEntry& e) { return navEntryValid(m_doc->tree, e); };
    // Step the stacks one entry at a time — each step moves the place
    // being left onto the other stack, exactly as single Back presses
    // would — but restore only where the walk ends: one refresh, and at
    // most one "Reopen path" macro instead of one per intermediate place.
    NavEntry cur = currentNavEntry(from);
    std::optional<NavEntry> target;
    for (int i = 0; i < qAbs(delta); ++i) {
        std::optional<NavEntry> e = delta < 0 ? m_nav.back(cur, valid) : m_nav.forward(cur, valid);
        if (!e) break;
        target = e;
        cur = *e;
    }
    if (target) {
        restoreNav(*target, from);
        return;
    }
    // Nothing restorable: the walk may still have discarded stale entries,
    // so the cells re-read the flags without waiting for a refresh tick.
    emit historyChanged(canGoBack(), canGoForward());
    pushAddressBarState();
}

void RcxController::restoreNav(const NavEntry& e, RcxEditor* from) {
    m_navRestoring = true;

    // 1. The source. switchToSavedSource brings that entry's base with it;
    //    step 3 then writes the entry's own base over that. Degrade when
    //    the saved entry is gone, its file vanished, or the attach was
    //    refused (attachViaPlugin leaves the provider untouched then, so
    //    the index it already moved is put back): the place is restored
    //    under the source the user has, with a hint saying so.
    if (e.activeSourceIdx >= 0 && e.activeSourceIdx != m_activeSourceIdx) {
        bool ok = e.activeSourceIdx < m_savedSources.size();
        QString name = ok ? m_savedSources[e.activeSourceIdx].displayName : QString();
        if (ok) {
            const SavedSourceEntry& se = m_savedSources[e.activeSourceIdx];
            if (se.kind == QStringLiteral("File") && !se.filePath.isEmpty()
                && !QFileInfo::exists(se.filePath))
                ok = false;
        }
        if (ok) {
            const int prevIdx = m_activeSourceIdx;
            const Provider* prevProv = this->provider().get();
            switchToSavedSource(e.activeSourceIdx);
            if (this->provider().get() == prevProv) {   // refused: nothing was swapped
                m_activeSourceIdx = prevIdx;
                ok = false;
            }
        }
        if (!ok)
            emit statusHint(QStringLiteral("Source %1 is not available — kept the current one")
                                .arg(name.isEmpty() ? QStringLiteral("#%1").arg(e.activeSourceIdx) : name));
    } else if (e.activeSourceIdx == kNavSourceRemoved) {
        // The saved source this place was read from was removed since (or
        // every source cleared) — NavHistory::forgetSource marked it. The
        // place is still restored, under the source the user has now.
        emit statusHint(QStringLiteral("The source this place was read from was removed — kept the current one"));
    }

    // 2. The view root — directly, never through setViewRootId (which
    //    would refresh mid-restore and which callers must not record).
    if (e.viewRootId != m_viewRootId) {
        m_viewRootId = e.viewRootId;
        m_focusPath.clear();
    }

    // 3. Base + formula: a RAW restore (decided), not a cmd::ChangeBase —
    //    the undo entry the original rebase pushed stays where it is, so
    //    Ctrl+Z after a Back still undoes THAT rebase. Mirrors what the
    //    command's apply does besides the assignment: values read at the
    //    restored base are not "changes", and the snapshot is of the old
    //    base's pages.
    NodeTree& tree = m_doc->tree;
    uint64_t restoredBase = e.baseAddress;
    if (!e.baseFormula.isEmpty()) {
        const auto callbacks = makeAddressCallbacks(provider().get(), tree.pointerSize);
        const auto result = AddressParser::evaluate(e.baseFormula, tree.pointerSize, &callbacks);
        if (result.ok) restoredBase = result.value;
    }
    if (restoredBase != baseAddress() || e.baseFormula != baseAddressFormula()) {
        baseAddress() = restoredBase;
        baseAddressFormula() = e.baseFormula;
        resetChangeTracking();
        resetSnapshot();
    }

    // 4. The focus path. A hop the user collapsed since (a crumb click, Up,
    //    a fold click) is document state: reopening it goes through the
    //    undo stack — every collapsed hop in ONE macro, and no macro at all
    //    when the path is already open (the common case: Back after F12).
    QVector<uint64_t> toExpand;
    for (uint64_t hop : e.focusPath) {
        const int pi = tree.indexOfId(hop);
        if (pi >= 0 && isCollapsed(tree.nodes[pi])) toExpand.push_back(hop);
    }
    if (!toExpand.isEmpty()) {
        const bool wasSuppressed = m_suppressRefresh;
        m_suppressRefresh = true;
        m_doc->undoStack.beginMacro(QStringLiteral("Reopen path"));
        for (uint64_t hop : toExpand)
            m_doc->undoStack.push(new RcxCommand(this, cmd::Collapse{hop, true, false}));
        m_doc->undoStack.endMacro();
        m_suppressRefresh = wasSuppressed;
    }
    m_focusPath = e.focusPath;
    reconcileFocusPath();

    // 5. One announcement, and it IS the refresh: documentChanged is
    //    connected to refresh() (the constructor), which composes the
    //    restored place and pushes the bar state with the new flags; the
    //    docks, the tab icon and the MCP bridge listen too. No refresh()
    //    call after it — that composed every Back twice (rebaseTo relies
    //    on the same emit as its one refresh).
    emit m_doc->documentChanged();

    // 6. Scroll the pane the gesture came from back to what it showed:
    //    the anchored row if that node still exists, else the deepest hop
    //    (the class the trail ends in), else the root.
    if (from) {
        uint64_t anchor = e.anchorNodeId;
        if (anchor == 0 || tree.indexOfId(anchor) < 0)
            anchor = m_focusPath.isEmpty() ? m_viewRootId : m_focusPath.last();
        if (anchor) from->scrollNodeToTop(anchor);
    }

    m_navRestoring = false;
    emit historyChanged(canGoBack(), canGoForward());
}

void RcxController::addBookmark(const QString& name, const QString& formula) {
    Bookmark b;
    b.name = name.trimmed();
    b.addressFormula = formula.trimmed();
    if (b.name.isEmpty() || b.addressFormula.isEmpty()) return;
    if (isBareAddressLiteral(b.addressFormula)) {
        const auto callbacks = makeAddressCallbacks(provider().get(), m_doc->tree.pointerSize);
        const auto result = AddressParser::evaluate(b.addressFormula, m_doc->tree.pointerSize, &callbacks);
        if (result.ok) b.addressFormula = addressExpression(result.value);
    }
    m_doc->tree.bookmarks.append(b);
    m_doc->modified = true;
    emit m_doc->documentChanged();
}

void RcxController::removeBookmark(int idx) {
    auto& bms = m_doc->tree.bookmarks;
    if (idx < 0 || idx >= bms.size()) return;
    bms.remove(idx);
    m_doc->modified = true;
    emit m_doc->documentChanged();
}

} // namespace rcx
