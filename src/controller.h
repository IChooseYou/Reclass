#pragma once
#include "core.h"
#include "editor.h"
#include "providers/snapshot_provider.h"
#include <QObject>
#include <QUndoStack>
#include <QUndoCommand>
#include <QTimer>
#include <QFutureWatcher>
#include <QPointer>
#include <QPair>
#include <QJsonArray>
#include <memory>
#include <optional>

namespace rcx {

class RcxController;
class TypeSelectorPopup;
class SourceChooserPopup;
class HexToolbarPopup;
struct TypeEntry;
enum class TypePopupMode;

// ── Document ──

class RcxDocument : public QObject {
    Q_OBJECT
public:
    explicit RcxDocument(QObject* parent = nullptr);

    NodeTree                   tree;
    std::shared_ptr<Provider>  provider;
    QUndoStack                 undoStack;
    QString                    filePath;
    QString                    dataPath;
    bool                       modified = false;
    QHash<NodeKind, QString>   typeAliases;

    // Owned RW buffer for self-attached "New Class" projects. The
    // processmemory provider reads/writes via the OS's RPM/WPM APIs
    // which take absolute virtual addresses — pointing baseAddress at
    // this buffer's data() means the user lands on guaranteed-writable
    // memory in our own process. std::unique_ptr<uint8_t[]> rather
    // than QByteArray to dodge the CoW pointer-stability hazard; size
    // is fixed at allocation time. nullptr when the document has a
    // real source attached (file / external process).
    std::unique_ptr<uint8_t[]> m_ownedBuffer;
    size_t                     m_ownedBufferSize = 0;
    // Saved-source entries deserialized from the .rcx file at load
    // time. Held as raw JSON until a controller attaches and lifts
    // them into its own QVector<SavedSourceEntry> — keeps RcxDocument
    // free of the controller's struct dependency. Examples like
    // png.rcx use this to ship a sibling sample binary that the
    // controller auto-attaches on first load.
    QJsonArray                 pendingSavedSources;
    // Count of sibling-overlaps detected by tree.findOverlaps() during the
    // most recent load(). Surfaced to the user via a controller statusHint
    // post-load so it's actually visible (the per-pair details are also
    // logged via qWarning for console post-mortem). Zero on clean trees /
    // freshly-created docs.
    int                        m_loadOverlapCount = 0;

    QString resolveTypeName(NodeKind kind) const {
        auto it = typeAliases.find(kind);
        if (it != typeAliases.end() && !it.value().isEmpty())
            return it.value();
        auto* m = kindMeta(kind);
        return m ? QString::fromLatin1(m->typeName) : QStringLiteral("???");
    }

    ComposeResult compose(uint64_t viewRootId = 0, bool compactColumns = false,
                          bool treeLines = false, bool braceWrap = false,
                          bool typeHints = false, bool showComments = true,
                          SymbolLookupFn symbolLookup = {}) const;
    bool save(const QString& path);
    bool load(const QString& path);
    void loadData(const QString& binaryPath);
    void loadData(const QByteArray& data);

signals:
    void documentChanged();
};

// ── Undo command ──

class RcxCommand : public QUndoCommand {
public:
    RcxCommand(RcxController* ctrl, Command cmd);
    void undo() override;
    void redo() override;
private:
    RcxController* m_ctrl;
    Command m_cmd;
};

// ── Saved source entry ──

struct SavedSourceEntry {
    QString kind;          // "File" or provider identifier (e.g. "processmemory")
    QString displayName;   // filename or process name
    QString filePath;      // for File sources
    QString providerTarget; // for plugin providers (e.g. "pid:name")
    uint64_t baseAddress = 0;
    QString baseAddressFormula;
};

// ── Controller ──

class RcxController : public QObject {
    Q_OBJECT
public:
    explicit RcxController(RcxDocument* doc, QWidget* parent = nullptr);
    ~RcxController() override;

    RcxEditor* primaryEditor() const;
    RcxEditor* addSplitEditor(QWidget* parent = nullptr);
    void removeSplitEditor(RcxEditor* editor);
    QList<RcxEditor*> editors() const { return m_editors; }

    void convertRootKeyword(const QString& newKeyword);
    void changeNodeKind(int nodeIdx, NodeKind newKind);
    void renameNode(int nodeIdx, const QString& newName);
    void insertNode(uint64_t parentId, int offset, NodeKind kind, const QString& name);
    void insertNodeAbove(int beforeIdx, NodeKind kind, const QString& name);
    void removeNode(int nodeIdx);
    // Extract bytes [selLo, selHi) into a new root class, inserting an
    // embedded Struct field at selLo in the original parent. Splits any
    // partially-overlapped hex siblings into left / right hex pads.
    // Refuses if the range crosses parents or non-hex / container
    // siblings. Address-space inputs (not byte offsets).
    void extractByteSelectionToNewClass(uint64_t selLo, uint64_t selHi);
    void toggleCollapse(int nodeIdx);
    void materializeRefChildren(int nodeIdx);
    void setNodeValue(int nodeIdx, int subLine, const QString& text,
                      bool isAscii = false, uint64_t resolvedAddr = 0);
    void duplicateNode(int nodeIdx);
    void convertToTypedPointer(uint64_t nodeId);
    // Attach a (possibly new) class to the clicked-on node based on an
    // RTTI demangled name. Always creates a NEW root struct named
    // baseName, appending _N suffix when the name collides — multiple
    // RTTI overlay clicks on different fields each get their own class
    // even when the type names match. Pointer kind + refId are set in
    // one undo macro. Returns the resulting struct id.
    uint64_t attachRttiClassToPointer(uint64_t nodeId, const QString& baseName);
    void splitHexNode(uint64_t nodeId);
    void toggleBitfieldBit(uint64_t nodeId, int memberIdx);
    void editBitfieldValue(uint64_t nodeId, int memberIdx);
    void showContextMenu(RcxEditor* editor, int line, int nodeIdx, int subLine, const QPoint& globalPos);
    void batchRemoveNodes(const QVector<int>& nodeIndices);
    void batchChangeKind(const QVector<int>& nodeIndices, NodeKind newKind);
    void deleteRootStruct(uint64_t structId);
    void groupIntoUnion(const QSet<uint64_t>& nodeIds);
    void dissolveUnion(uint64_t unionId);

    // Write a span of bytes from the active provider to a binary file
    // at `path`. Returns true on success; on failure writes a one-line
    // reason into *err. Used by the byte-selection "Save as binary
    // file" action — split out as a public static so tests can drive
    // it without going through a QFileDialog. Reads the bytes via the
    // controller's active provider (snapshot wins over real when both
    // are present, matching the copy paths).
    bool writeSelectedBytesToFile(uint64_t addr, int n,
                                  const QString& path,
                                  QString* err = nullptr) const;

    // Applies a command variant. Returns false if the underlying operation
    // rejected the change (e.g. provider write failed). Callers — primarily
    // RcxCommand::undo/redo — use this to mark the command obsolete so the
    // undo stack stays consistent with the actual data.
    bool applyCommand(const Command& cmd, bool isUndo);
    void refresh();
    void applyTypePopupResult(TypePopupMode mode, int nodeIdx, const TypeEntry& entry, const QString& fullText);
    uint64_t findOrCreateStructByName(const QString& typeName, int depth = 0);

    // Selection
    void handleNodeClick(RcxEditor* source, int line, uint64_t nodeId,
                         Qt::KeyboardModifiers mods);
    void clearSelection();
    void applySelectionOverlays();
    QSet<uint64_t> selectedIds() const { return m_selIds; }

    void setViewRootId(uint64_t id);
    uint64_t viewRootId() const { return m_viewRootId; }
    void scrollToNodeId(uint64_t nodeId);

    // Bookmarks
    bool navigateToFormula(const QString& formula, QString* errOut = nullptr);
    void addBookmark(const QString& name, const QString& formula);
    void removeBookmark(int idx);

    RcxDocument* document() const { return m_doc; }
    void setEditorFont(const QString& fontName);
    void setRefreshInterval(int ms);
    void setCompactColumns(bool v);
    void setTreeLines(bool v);
    void setBraceWrap(bool v);
    void setTypeHints(bool v);
    bool typeHints() const { return m_typeHints; }
    void setShowComments(bool v);
    bool showComments() const { return m_showComments; }
    void setShowRtti(bool v);
    bool showRtti() const { return m_showRtti; }
    void setShowEnumChips(bool v);
    bool showEnumChips() const { return m_showEnumChips; }
    // Read-only override: force value/byte writes to no-op even when the
    // underlying provider reports isWritable(). Used by the live-self
    // tutorial — writing into the editor's own memory (e.g. setting the
    // __vptr value to 1) crashes the next virtual dispatch.
    void setReadOnlyOverride(bool v) { m_readOnlyOverride = v; }
    bool readOnlyOverride() const { return m_readOnlyOverride; }
    void resetProvider();

    // MCP bridge accessors
    void setSuppressRefresh(bool v) { m_suppressRefresh = v; }
    // Attach a provider via plugin. When `registerAsSavedSource` is true
    // (default: false), also push the attach onto the saved-sources list
    // so it appears in the source-picker dropdown — useful for the
    // self-attached "New Class" flow where the user expects the chosen
    // source to be discoverable. MCP / requestOpenProviderTab callers
    // leave it false to keep the saved-source list as a UI-owned thing.
    void attachViaPlugin(const QString& providerIdentifier, const QString& target,
                         bool registerAsSavedSource = false);
    const QVector<SavedSourceEntry>& savedSources() const { return m_savedSources; }
    int activeSourceIndex() const { return m_activeSourceIdx; }
    void switchSource(int idx) { switchToSavedSource(idx); }
    void clearSources();
    void selectSource(const QString& text);
    void copySavedSources(const QVector<SavedSourceEntry>& sources, int activeIdx);

    // Value tracking toggle (per-tab, off by default)
    bool trackValues() const { return m_trackValues; }
    void setTrackValues(bool on);
    void resetChangeTracking();

    // Cross-tab type visibility: point at the project's full document list
    void setProjectDocuments(QVector<RcxDocument*>* docs) { m_projectDocs = docs; }

    // Test accessors
    const QHash<uint64_t, ValueHistory>& valueHistory() const { return m_valueHistory; }
    const ComposeResult& lastResult() const { return m_lastResult; }
    int  dataExtent() const { return computeDataExtent(); }
    // Refresh-speedup observability (test-only).
    int  refreshIntervalMs()  const { return m_refreshTimer ? m_refreshTimer->interval() : 0; }
    bool refreshTimerActive() const { return m_refreshTimer && m_refreshTimer->isActive(); }
    int  idleTicks()          const { return m_idleTicks; }
    int  pageStability(uint64_t pageAddr) const { return m_pageStability.value(pageAddr & ~uint64_t(4095), 0); }
    const SnapshotProvider* snapshotProv() const { return m_snapshotProv.get(); }

signals:
    void nodeSelected(int nodeIdx);
    void selectionChanged(int count);
    void statusHint(const QString& text);  // brief status bar message
    void contextMenuAboutToShow(QMenu* menu, int line);
    void requestOpenProviderTab(const QString& pluginId, const QString& target,
                                const QString& title);
    // Ctrl+Click on a typed pointer or struct field — open the target struct
    // in a new tab sharing this document. MainWindow calls createTab(doc)
    // and setViewRootId(structId) on the new tab.
    void requestOpenStructInNewTab(uint64_t structId);
    // Active provider's isValid() flipped — used by the dock tab's
    // source-status icon to dim/restore in real time when a process
    // exits, a file vanishes, etc. Fires on transition only, not every
    // refresh tick.
    void sourceLivenessChanged(bool live);

private:
    RcxDocument*       m_doc;
    QList<RcxEditor*>  m_editors;
    ComposeResult      m_lastResult;
    QSet<uint64_t>     m_selIds;
    int                m_anchorLine = -1;
    bool               m_suppressRefresh = false;
    bool               m_compactColumns = false;
    bool               m_treeLines = false;
    bool               m_braceWrap = false;
    bool               m_typeHints = false;
    bool               m_showComments = false;
    bool               m_showRtti = true;          // chip toggle, default ON (current behavior)
    bool               m_showEnumChips = true;     // chip toggle, default ON
    bool               m_readOnlyOverride = false; // tutorial safety; see header
    uint64_t           m_viewRootId = 0;

    // ── Saved sources for quick-switch ──
    QVector<SavedSourceEntry> m_savedSources;
    int m_activeSourceIdx = -1;
    bool m_lastLive = false;  // last-seen provider->isValid(); diff for sourceLivenessChanged emit

    // ── Cached type selector popup (avoids ~350ms cold-start on first show) ──
    QPointer<TypeSelectorPopup> m_cachedPopup;
    int m_typePopupGen = 0;  // generation counter for deferred content loading
    // Recently-used type display names (most recent first, capped at 8).
    // Pushed in applyTypePopupResult so primitive + composite selections are
    // both tracked. Surfaced as a "Recent" pseudo-section in the popup.
    QStringList m_recentTypeNames;
    void pushRecentType(const QString& displayName);

    // ── Cached source chooser popup ──
    QPointer<SourceChooserPopup> m_cachedSourcePopup;
    void showSourcePopup(RcxEditor* editor, QPoint globalPos);
    SourceChooserPopup* ensureSourcePopup(RcxEditor* editor);

    // ── Hex toolbar popup (auto-shows on hex node selection) ──
    QPointer<HexToolbarPopup> m_hexToolbar;
    void showHexToolbar(RcxEditor* editor, int nodeIdx);
    void hideHexToolbar();
public:
    void joinHexNodes(uint64_t nodeId, NodeKind targetKind);
private:

    // ── Auto-refresh state ──
    using PageMap = QHash<uint64_t, QByteArray>;
    QTimer*         m_refreshTimer = nullptr;
    QFutureWatcher<PageMap>* m_refreshWatcher = nullptr;
    std::unique_ptr<SnapshotProvider> m_snapshotProv;
    PageMap         m_prevPages;
    QSet<int64_t>   m_changedOffsets;
    QHash<uint64_t, ValueHistory> m_valueHistory;
    QHash<uint64_t, uint64_t> m_lastValueAddr;  // nodeId → last offsetAddr used for value recording
    bool            m_trackValues = true;
    int             m_valueTrackCooldown = 0; // suppress value recording for N refresh cycles after clear
    uint64_t        m_refreshGen = 0;
    uint64_t        m_readGen = 0;
    bool            m_readInFlight = false;

    // ── Refresh speedups (memory-source-only optimizations) ──
    // Per-page stability counter: increments every tick a page's bytes
    // didn't change, resets to 0 on byte change. Pages stable for >=
    // kStabilityThreshold ticks get re-read at half rate (every other
    // tick) so an idle struct stops syscalling at the full cadence.
    QHash<uint64_t, int> m_pageStability;
    static constexpr int kStabilityThreshold = 5;

    // Tick counter — used to halve the read cadence for stable pages.
    uint64_t m_tickCount = 0;

    // Adaptive refresh: counts back-to-back ticks where no page bytes
    // changed. After kIdleBackoffTicks of these we widen the timer
    // interval geometrically up to m_refreshIntervalMaxMs. Reset to
    // m_refreshIntervalBaseMs (snappy) on the next observed change or
    // window focus-in.
    int m_idleTicks = 0;
    int m_refreshIntervalBaseMs = 200;   // snappy default, focused
    int m_refreshIntervalMaxMs  = 1500;  // backed-off cap
    int m_refreshIntervalBlurMs = 1500;  // unfocused cap (set on focusOut)
    static constexpr int kIdleBackoffTicks = 8;

    // Window state — drives focus-/minimize-aware throttling. Wired
    // from QGuiApplication::applicationStateChanged.
    bool m_windowFocused = true;
    bool m_windowVisible = true;

    QVector<RcxDocument*>* m_projectDocs = nullptr;

    // ── Undo grouping for rapid ←→ cycling ──
    QTimer*  m_cycleMacroTimer = nullptr;
    bool     m_cycleMacroOpen = false;

    void connectEditor(RcxEditor* editor);
    // Lift doc->pendingSavedSources (populated by RcxDocument::load
    // from the .rcx's "savedSources" array) into m_savedSources and
    // auto-activate the first one. Called once from the constructor.
    void ingestPendingSavedSources();
    void appendBytesDialog(QWidget* parent, uint64_t targetId);
    void handleMarginClick(RcxEditor* editor, int margin, int line, Qt::KeyboardModifiers mods);
    void updateCommandRow();
    void switchToSavedSource(int idx);
    void pushSavedSourcesToEditors();
    void showTypePopup(RcxEditor* editor, TypePopupMode mode, int nodeIdx, QPoint globalPos);
    TypeSelectorPopup* ensurePopup(RcxEditor* editor);

    // ── Auto-refresh methods ──
    void setupAutoRefresh();
    void onRefreshTick();
    void onReadComplete();
    int  computeDataExtent() const;
    // Byte range covered by visible lines across all attached editors,
    // expressed as [absStart, absEnd). Returns std::nullopt when no
    // editor is showing anything yet (during construction). Used by
    // onRefreshTick to skip pages that nobody is looking at — the
    // snapshot retains stale bytes for those, refresh resumes when
    // the user scrolls them back into view.
    std::optional<QPair<uint64_t, uint64_t>> viewportAddressRange() const;
    // Re-classify pages just merged into the snapshot — pages wholly
    // contained within an executable region get marked permanent so
    // future ticks skip them entirely.
    void classifyPermanentPages(const PageMap& fresh);
    // Adaptive interval helpers — separate from setRefreshInterval so
    // user-level changes (Options dialog) can update the *base* without
    // fighting the per-tick adaptive logic.
    void applyAdaptiveInterval();

public:
    // Driven by MainWindow on QGuiApplication::applicationStateChanged
    // and on its own changeEvent (minimize/restore). Public so the
    // wiring lives in one place instead of every controller subscribing
    // to a global singleton.
    void setWindowState(bool focused, bool visible);

private:
    void resetSnapshot();
    void collectPointerRanges(uint64_t structId, uint64_t memBase,
                              int depth, int maxDepth,
                              QSet<QPair<uint64_t,uint64_t>>& visited,
                              QVector<QPair<uint64_t,int>>& ranges,
                              int64_t& budget) const;
    static constexpr int64_t kPointerSnapshotByteBudget = 64 * 1024 * 1024; // 64 MB
};

} // namespace rcx
