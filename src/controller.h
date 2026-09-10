#pragma once
#include "core.h"
#include "editor.h"
#include "nav_history.h"
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

// Kinds whose value the formatter byte-swaps when Node::bigEndian is set
// (format.cpp readValueImpl): every multi-byte hex / integer / float. The
// 8-bit kinds, Bool, pointers, strings, vectors and containers are never
// swapped, so "Big endian" / the ribbon's Swap must not offer them. Listed
// explicitly on purpose — a `k >= Int16 && k <= UInt128` range test
// silently included UInt8 (the enum runs Int8..Int128, UInt8..UInt128).
// Single source of truth for the context menu item, toggleBigEndianSelection
// and RibbonActions' predicate.
inline constexpr bool isEndianSwappable(NodeKind k) {
    switch (k) {
    case NodeKind::Hex16:   case NodeKind::Hex32:   case NodeKind::Hex64:   case NodeKind::Hex128:
    case NodeKind::Int16:   case NodeKind::Int32:   case NodeKind::Int64:   case NodeKind::Int128:
    case NodeKind::UInt16:  case NodeKind::UInt32:  case NodeKind::UInt64:  case NodeKind::UInt128:
    case NodeKind::Float16: case NodeKind::Float:   case NodeKind::Double:
        return true;
    default:
        return false;
    }
}

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
    // Break bytes [selLo, selHi) into a new root class, inserting an
    // embedded Struct field at selLo in the original parent. Splits any
    // partially-overlapped hex siblings into left / right hex pads.
    // Fully-contained Struct/Array/typed siblings are moved into the new
    // class INTACT (preserving refId/metadata — the referenced hierarchy
    // is untouched). Refuses only when the range crosses parents or
    // straddles a non-hex / container sibling. Address-space inputs.
    void extractByteSelectionToNewClass(uint64_t selLo, uint64_t selHi);
    // Compute the break region [lo, hi) (absolute addresses) from the
    // current selection: the active byte selection if one exists on `editor`,
    // otherwise the combined offset span of the selected nodes (m_selIds).
    // Returns nullopt when nothing usable is selected. Shared by the node
    // "Carve" action, shared by the node menu, the Edit menu and the ribbon.
    std::optional<QPair<uint64_t, uint64_t>>
    regionFromCurrentSelection(RcxEditor* editor) const;
    // True if nodeId is a field of the viewed class (its ancestor chain reaches
    // m_viewRootId), false if it's shown inside an embedded class. Guards the
    // break ops from resolving an embedded field's relative offset wrongly.
    bool nodeInView(uint64_t nodeId) const;
    // Stricter than nodeInView: true only if `nodeId` is a DIRECT field of the
    // view frame. Union members and inline-struct fields pass nodeInView (their
    // chain reaches the root) but store offsets relative to their own container,
    // so the parent-relative break scan would resolve them to the wrong region.
    bool isDirectViewFrameChild(uint64_t nodeId) const;
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
    // Change the kind of every listed node in ONE undo macro, applied in
    // ascending offset order (QSet iteration order is hash order — never
    // let it leak into a user-visible mutation sequence). Restores m_selIds.
    void batchChangeKind(const QVector<int>& nodeIndices, NodeKind newKind);
    // Replace a contiguous run of sibling fields with ONE asm node spanning
    // exactly their bytes. Machine code is a byte window, not a per-field
    // type: converting row by row leaves a column of asm[1] nodes and cuts
    // every instruction that crosses a field boundary in half.
    void mergeToCodeWindow(const QVector<uint64_t>& orderedIds);
    void deleteRootStruct(uint64_t structId);

    // ── Selection-level operations ──
    // Public entry points shared by the editor's key handlers, the ribbon
    // (RibbonActions), menus and MCP. Each is a complete user-level op:
    // one undo step (macro where several commands are pushed), refresh
    // suppressed for the duration, ids re-resolved to indices inside loops
    // (pad insertion / removal shifts indices), ordered work sorted by
    // offset. Every method reads m_selIds through baseNodeIdFromSelId /
    // selKindOf so footer / member / array-element rows are classified
    // consistently.
    //
    // The quick-type-change rule (Space / 1-5 / P / F / S / U keys and the
    // ribbon Type panel): multi-selection → batchChangeKind over every
    // selected node; single hex → smaller hex → changeNodeKind (pads the
    // freed bytes); hex → bigger hex → joinHexNodes (consumes following
    // hex siblings); anything else → changeNodeKind. Emits nodeSelected so
    // the status bar picks up the new type.
    void applyQuickTypeChange(int nodeIdx, NodeKind kind);
    // Retype the selected leaf nodes (plain rows only; footer / member /
    // array-element rows, root structs and containers are skipped). One
    // node → applyQuickTypeChange; several → batchChangeKind in offset order.
    void retypeSelection(NodeKind kind);
    // Lay N nodes of `kind` end to end across exactly [selLo, selHi), rebuilding
    // the fields the range cuts through and padding whatever of them falls
    // outside it. A byte selection names BYTES, not the fields containing them.
    // Returns false (with a status hint) if the range cannot be retyped.
    bool retypeByteRange(uint64_t selLo, uint64_t selHi, NodeKind kind);
    // Append `byteCount` raw bytes to the end of a container: Array → grow
    // arrayLen; embedded struct with refId → redirect to the referenced
    // root class; Struct → byteCount/8 × Hex64 + byteCount%8 × Hex8 in one
    // macro "Append N bytes". targetId 0 = the view root, else the first
    // root struct. Enums and bitfields take members, not raw bytes — those
    // targets are refused with a statusHint (the footer "+10" pill grows an
    // enum via appendEnumMembersRequested instead). Body of the footer
    // "+10h/+100h/+1000h" pills.
    void appendBytes(uint64_t targetId, int byteCount);
    // Insert `byteCount` raw bytes ABOVE `anchorNodeId` (byteCount/8 × Hex64
    // then the remainder as Hex8, ascending, each landing at the anchor's
    // CURRENT offset so the anchor and everything below it shift down by
    // byteCount in total). One macro "Insert N bytes above <name>". No-op
    // for roots (nothing to shift) and unknown ids.
    void insertBytesAbove(uint64_t anchorNodeId, int byteCount);
    // Delete the selected nodes. Member rows never delete anything; an
    // array-element row stands for its whole Array and a footer `}` row for
    // its container — a root class has no other selectable row, and the
    // Delete key removed a root selected that way before the ribbon existed.
    // A LONE root goes through deleteRootStruct (clears refIds that point at
    // it, re-targets the view root); otherwise several → batchRemoveNodes
    // (one macro), one → removeNode. Nothing deletable → statusHint.
    void deleteSelection();
    // Duplicate every selected leaf in ONE macro "Duplicate N nodes"
    // (duplicateNode per node, ascending offset). Containers can't be
    // duplicated (duplicateNode refuses them); nothing eligible → statusHint.
    void duplicateSelection();
    // Overwrite the bytes of the current region — the active byte selection,
    // else the span of the selected nodes (regionFromCurrentSelection) — with
    // a constant / random pattern via one cmd::WriteBytes (undoable; kept
    // out of value history like every user edit). Returns false when the
    // provider is read-only, no region is selected, or the region exceeds
    // 64 KiB.
    enum class ByteFill { Zero, FF, Random };
    bool fillSelectionBytes(ByteFill fill);
    // Flip the display-side big-endian flag of every selected scalar
    // (Hex16+, Int16+, UInt16+, Float16/Float/Double) in one macro.
    void toggleBigEndianSelection();
    // Turn the selection into an Array node. One leaf → Array[1] of its
    // kind (macro "Change to array"); N contiguous same-kind siblings →
    // remove 2..N and retype the first to Array[N] (macro "Group N into
    // array"). Returns false (with a statusHint) when the selection is not
    // one of those shapes.
    bool makeArrayFromSelection();
    // convertToTypedPointer for every selected 4/8-byte leaf that has no
    // refId yet — one macro "Change to ptr*" when several.
    void convertSelectionToTypedPointers();
    // The ';' key: edit the selected node's comment inline (single / empty
    // selection) or prompt once and apply to all (multi-selection, one
    // macro). No-op while comments are hidden (setShowComments(false)).
    void commentSelection(RcxEditor* editor);
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

    // Mirror an editor's byte selection into the row selection. The set is
    // the encoded selIds of every hex row the byte selection covers (empty
    // = clear). Driven by RcxEditor::byteSelectionRowsChanged so the grey
    // M_SELECTED rows track the byte selection.
    void onByteSelectionRows(const QSet<uint64_t>& selIds);

    // ── Byte-selection op handlers ──
    // One implementation shared by the Ctrl+C/V/Del keyboard shortcuts
    // (wired in connectEditor) and the right-click "Selected bytes ▸"
    // submenu (built in showContextMenu). Each reads the live byte range
    // from editor->byteSelection() and performs provider I/O.
    void byteCopyHex(RcxEditor* editor);
    void byteCopyCArray(RcxEditor* editor);
    void byteCopyPython(RcxEditor* editor);
    void byteSaveAsFile(RcxEditor* editor);
    void bytePasteHex(RcxEditor* editor);
    void byteZeroFill(RcxEditor* editor);
    QByteArray readSelectionBytes(RcxEditor* editor);
    // Append the "Selected bytes (N) ▸" submenu to the top of a node menu
    // when `editor` has an active byte selection. No-op otherwise.
    void addByteSubmenu(QMenu& menu, RcxEditor* editor);

    void setViewRootId(uint64_t id);
    // Switching the view root AS A GESTURE: records the place being left,
    // then switches. setViewRootId alone never records — load, new-tab and
    // delete-root call it too — so every deliberate pick has to pair the two,
    // and this is the one place that pairing lives. The type chooser's Root
    // mode is its only caller in the app; the address bar had a second,
    // worse copy of the same menu until the chooser took the job outright.
    void pickViewRoot(uint64_t id, RcxEditor* from = nullptr);
    uint64_t viewRootId() const { return m_viewRootId; }
    void scrollToNodeId(uint64_t nodeId);

    // ── Drill-down trail (inline-expansion focus path) ──
    // The trail the address bar renders is CLICK-driven: handleNodeClick sets
    // the focus path to the chain of expanded typed pointers containing the
    // clicked node, so selecting a typed pointer (or any row inside its
    // inline expansion) adds it to the trail. No row affordance / follow-arrow.
    // Crumb clicked (crumbIndex: 0 = root class, i = the class shown by focus
    // pointer i-1): collapse everything below that class and scroll to it.
    // The view root never changes.
    void collapseToFocus(int crumbIndex);
    const QVector<uint64_t>& focusPath() const { return m_focusPath; }  // test accessor
    // The focus-path chain that a selection on `nodeId` produces (the expanded
    // typed pointers from the view root down to the one containing it). Public
    // for tests; handleNodeClick assigns its result to m_focusPath.
    QVector<uint64_t> focusChainToNode(uint64_t nodeId) const;

    // ── Sideways navigation (the address bar's chevrons and its path edit) ──
    // The drillable fields of the class at crumb `level` (0 = the view root,
    // i = the class focus hop i-1 opens), the hop the trail goes through
    // flagged current; level == focusPath().size() is the deepest class and
    // nothing is current. What a chev:<level> menu lists. Each entry's
    // address says where the field leads: an expanded hop from the last
    // compose (the ptrBase stamped inside it — the same data the crumbs
    // read), a collapsed pointer from ONE provider read at the container
    // plus its offset (only with a valid provider, never a module
    // enumeration), an embedded struct / array from its own row. Called
    // when a menu opens, never per refresh.
    QVector<SiblingEntry> siblingsForCrumb(int level) const;
    // The drillable fields of the class a dotted path lands in ("Player" →
    // Player's fields, "Player.stats" → Stats's); an empty path lists the
    // root classes in the same shape (field = class label), and a path that
    // does not resolve lists nothing. What the path edit's Down completes
    // from.
    QVector<SiblingEntry> drillFieldsAt(const QString& path) const;
    // Explorer's sideways jump: make `newPointerId` the hop at `level` —
    // collapse the hop that was there (if any and expanded) and expand the
    // new one (if collapsed) in ONE undo macro "Switch to <field>", set the
    // focus path to focusPath[0..level) + newPointerId, refresh and scroll
    // the new hop's row to the top of the sender pane. level ==
    // focusPath().size() appends (the deepest crumb's "drill further").
    // Choosing the hop already there is a no-op: no undo entry. True when
    // the switch happened; false (nothing pushed, path untouched) for a bad
    // level, a non-hop, or an id that is not a hop of the class at `level`.
    bool switchSibling(int level, uint64_t newPointerId);
    // The path edit's Enter: resolve "RcxEditor.vptr.parent"
    // (resolveDrillPath), switch the view root if the path names another
    // one, expand every collapsed hop in ONE undo macro "Navigate to path",
    // set the focus path, refresh and scroll the deepest hop to the top.
    // On a miss: *err names the segment, statusHint carries it, nothing
    // changes, false.
    bool navigateToDrillPath(const QString& text, QString* err = nullptr);

    // Rebase the document to `expr` (anything AddressParser understands —
    // "0x7FF6...", "<game.exe>+0x40", "[ntdll!Ldr]"). THE base-address
    // mutation: the inline command-row edit, Goto, the bookmarks dock, the
    // scanner's Set-as-Base and MCP change_base all come through here, so
    // every rebase is one undoable cmd::ChangeBase (pushed only when base or
    // formula actually change), lands in the Goto recent list, arms the
    // value-tracking cooldown, and keeps m_focusPath. A bare hex/decimal
    // literal clears the formula; anything else is kept as the formula. On
    // failure: *err (parser message), statusHint("Base: …"), false, nothing
    // pushed. recordHistory=false skips the NavHistory entry (nothing uses
    // it today: a Back/Forward restore writes the base directly, never
    // through here).
    bool rebaseTo(const QString& expr, QString* err = nullptr, bool recordHistory = true);
    // Bookmarks / Goto — a wrapper over rebaseTo, kept for its callers.
    bool navigateToFormula(const QString& formula, QString* errOut = nullptr);
    void addBookmark(const QString& name, const QString& formula);
    void removeBookmark(int idx);

    // ── Navigation history (the address bar's Back / Forward / Up) ──
    // Explorer's model, not the undo model: an entry is a PLACE (view root,
    // drill path, base, active source), recorded at the navigation gestures
    // — the F12 jump, a crumb click, every rebase, a source switch, a
    // chevron / root pick, a path edit — and NOWHERE else (never inside
    // setViewRootId, which load / new-tab / delete-root also call). A
    // restore writes the place back directly: no undo entry, no history
    // entry — except that a collapsed hop on the restored path is re-opened
    // through ONE undo macro ("Reopen path"), because Node::collapsed is
    // document state and reopening it must stay undoable. Undo / redo never
    // touch history. One gesture = at most one undo entry + one history
    // entry. Entries whose root or hops were deleted are skipped on the way
    // back, so canGoBack() only says yes when something restorable is there.
    bool canGoBack() const;
    bool canGoForward() const;
    bool canGoUp() const { return !m_focusPath.isEmpty(); }
    // `from` is the pane the gesture came from: it is the one scrolled on
    // restore and the one whose first visible row anchors the entry. Null
    // = the signal's sender pane, else the primary editor.
    void goBack(RcxEditor* from = nullptr);
    void goForward(RcxEditor* from = nullptr);
    // Up one level = the parent crumb: collapseToFocus on the deepest hop,
    // which records its own history entry (it is a crumb click).
    void goUp(RcxEditor* from = nullptr);
    // The source chooser the address bar's chip opens, for other surfaces
    // (the ribbon's Source button) so there is one picker, not two.
    void openSourceChooser(RcxEditor* editor, QPoint globalPos) { showSourcePopup(editor, globalPos); }
    // The history menu's pick: negative = back |delta| entries, positive =
    // forward. Steps the stacks entry by entry (so they stay exactly what a
    // sequence of single steps would leave) but restores only the final
    // place — one refresh, at most one "Reopen path" macro.
    void jumpToHistory(int delta, RcxEditor* from = nullptr);
    // The stacks as NavHistory keeps them, oldest first (the menu reverses),
    // with the stale entries left out — a deleted root or hop that back()
    // / forward() would discard on the way. So a row's position in the
    // history menu equals the number of restorable steps its pick asks
    // for; unfiltered, a pick beyond a stale row landed one place further.
    QVector<NavEntry> backEntries() const;
    QVector<NavEntry> forwardEntries() const;
    // The place the controller is at right now, anchored on `from`'s first
    // visible row. What recordNav pushes and what a step hands to
    // NavHistory as the place being left.
    NavEntry currentNavEntry(RcxEditor* from = nullptr) const;
    // The label currentNavEntry would record now — "Trail.path  @ 0x…" —
    // without the scroll anchor. The history menu's checked "you are
    // here" row, so it reads exactly like the entries around it.
    QString currentNavLabel() const;

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
    void removeSavedSource(int idx);
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
//TODO-DELETE(dataExtent)     int  dataExtent() const { return computeDataExtent(); }
    // Refresh-speedup observability (test-only).
    int  refreshIntervalMs()  const { return m_refreshTimer ? m_refreshTimer->interval() : 0; }
    bool refreshTimerActive() const { return m_refreshTimer && m_refreshTimer->isActive(); }
//TODO-DELETE(idleTicks)     int  idleTicks()          const { return m_idleTicks; }
    int  pageStability(uint64_t pageAddr) const { return m_pageStability.value(pageAddr & ~uint64_t(4095), 0); }
    const SnapshotProvider* snapshotProv() const { return m_snapshotProv.get(); }

    // Tri-state status of the active data source (status-bar badge): None = no
    // source; Static = a non-live file source (fully read); Live = reading;
    // Stale = live but the last read failed / was rejected; Disconnected = the
    // provider went invalid (process exited, file vanished).
    enum class SourceStatus { None, Static, Live, Stale, Disconnected };
    SourceStatus sourceStatus() const { return m_lastStatus; }

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
    // Tri-state source status (transition-only) for the status-bar badge.
    void sourceStatusChanged(SourceStatus status);
    // The Back / Forward stacks changed (an entry recorded, a restore, a
    // stale entry discarded). The bars get the flags through the state push
    // on the refresh that follows; this is for a menu that wants them
    // without a refresh.
    void historyChanged(bool canBack, bool canForward);

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
    bool               m_showRtti = false;         // auto-RTTI scan default OFF (expensive on module-heavy targets; View menu opts in)
    bool               m_showEnumChips = true;     // chip toggle, default ON
    bool               m_readOnlyOverride = false; // tutorial safety; see header
    uint64_t           m_viewRootId = 0;
    QVector<uint64_t>  m_focusPath;    // the address bar's trail: chain of
                                       // expanded typed-pointer ids from the
                                       // root class down to the deepest
                                       // drilled one.

    // ── Navigation history ──
    NavHistory         m_nav;
    bool               m_navRestoring = false;  // a restore in progress: its
                                                // source switch must not record
    bool               m_loading = false;       // the constructor's auto-attach
                                                // of the first saved source
                                                // is not a gesture
    // Record the place being LEFT. Called BEFORE the state change at each
    // gesture, and only when that gesture will actually move (a no-op
    // gesture must not leave an entry equal to where the user still is).
    // Silent while loading or restoring.
    void recordNav(RcxEditor* from = nullptr);
    // Write `e` back: source (degrading to the current one when the saved
    // entry is gone or does not connect), view root, base + formula (raw —
    // no cmd::ChangeBase), the focus path (re-expanding collapsed hops in
    // one undo macro), then one refresh and a scroll on `from`.
    void restoreNav(const NavEntry& e, RcxEditor* from);
    // The pane a gesture came from: the signal's sender when the call is
    // inside a slot, else the primary editor.
    RcxEditor* gestureEditor(RcxEditor* from) const;
    // collapseToFocus's body with the pane spelled out (collapseToFocus
    // itself reads sender(), which goUp cannot rely on).
    void collapseToFocusIn(int crumbIndex, RcxEditor* ed);

    // Drill target for a node — the struct id following it navigates to
    // (drillTargetId on the resolved node). 0 = nothing to follow.
    uint64_t resolveDefinitionTarget(int nodeIdx) const;
    // The container notion every focus-path consumer shares — containerOf()
    // and expandedHopInto() — lives in core.h as free functions over the
    // NodeTree so the address bar model can use it without a controller.
    // Reconstruct the chain of expanded hops from the view root down to
    // `pid` (inclusive) by following containerOf/expandedHopInto — so
    // ancestors expanded via the fold margin (which never grew m_focusPath)
    // are still represented. Empty if no expanded chain reaches the view root
    // (orphan / cycle).
    QVector<uint64_t> focusChainTo(uint64_t pid) const;
    // Trim m_focusPath at the first entry that is gone, no longer a drillable
    // pointer, collapsed, or whose container breaks the chain (e.g. the user
    // fold-collapsed an ancestor). Keeps the trail honest each refresh.
    void reconcileFocusPath();
    // Display label for a class id (structTypeName / name / fallback). For
    // id 0 / show-all, the first root struct name.
    QString classLabelOf(uint64_t id) const;
    // The address bar's snapshot: source name / kind / liveness, base
    // address + formula, and the dotted "class.field" Crumb list built from
    // m_focusPath — with each crumb's class id, entry hop, address and
    // keyword. Reconciles the focus path first.
    AddressBarState addressBarState();
    // Push that snapshot to every editor's bar (each bar early-returns on an
    // equal state, so the refresh tail calls this unconditionally).
    void pushAddressBarState();
    // The compose-side lookups the crumbs and the sibling menus share, over
    // m_lastResult.meta (see addressBarState for the scan rule):
    //   focusHopLines  — the rendered line of each focus hop, -1 when it
    //                    (and so every deeper hop) has no row;
    //   frameAddresses — the absolute address of the frame each crumb
    //                    names: [0] the view root, [i+1] what hop i opens;
    //                    0 = unknown / unreadable. Size focusPath.size()+1.
    QVector<int>      focusHopLines() const;
    QVector<uint64_t> frameAddresses() const;

    // ── Class creation (one canonical scheme, shared by every creator) ──
    // Unique struct type name: `base`, else `base_2`, `base_3`, …
    QString uniqueStructName(const QString& base = QStringLiteral("NewClass")) const;
    // Create a root struct (parentId 0) named `typeName` with `keyword`
    // ("class"/"struct"/…, empty = struct) and `fieldCount` arch-sized hex
    // children; pushes the Insert commands onto the CURRENT undo macro and
    // returns the new struct id. Caller wraps in beginMacro + m_suppressRefresh.
    uint64_t createRootStruct(const QString& typeName, const QString& keyword,
                              int fieldCount);

    // ── Saved sources for quick-switch ──
    QVector<SavedSourceEntry> m_savedSources;
    int m_activeSourceIdx = -1;
    bool m_lastLive = false;  // last-seen provider->isValid(); diff for sourceLivenessChanged emit
    bool m_lastReadOk = true;  // last refresh produced a usable (non-rejected) read
    SourceStatus m_lastStatus = SourceStatus::None;  // diff for sourceStatusChanged emit

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
    // Latches the "discarding all-zero page-0" debug log so a sustained
    // unreadable-page condition doesn't flood the console at refresh
    // tick rate. Cleared the first time we get a real read through.
    bool            m_loggedAllZeroPage0 = false;
    QSet<int64_t>   m_changedOffsets;
    QHash<uint64_t, ValueHistory> m_valueHistory;
    QHash<uint64_t, uint64_t> m_lastValueAddr;  // nodeId → last offsetAddr used for value recording
    // nodeId → raw bytes of the last sampled value. Change-detection keys on
    // this (not the formatted display string) so a no-op reformat of identical
    // bytes — Hex64 "0x0" → Pointer64 "nullptr", endianness/RVA toggle — doesn't
    // register as a value change and spuriously light the heatmap / fire the
    // previous-values popup.
    QHash<uint64_t, QByteArray> m_lastValueBytes;
    // Absolute [lo,hi) byte ranges the USER just wrote (inline value edit or
    // hex/ascii byte edit). Consumed by the very next value-history pass: nodes
    // overlapping these ranges have their baseline updated but are NOT recorded
    // into value history — the user's own edit must not appear as a tracked
    // change. Cleared each refresh after the value-history pass.
    QVector<QPair<uint64_t, uint64_t>> m_userEditRanges;
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

    // Cached region list for classifyPermanentPages (Speedup 4). enumerateRegions()
    // is a full VirtualQueryEx sweep of the target's address space (10s of ms on a
    // big game like DayZ) and was re-run on EVERY refresh tick, which made
    // module-heavy targets stutter ("barely usable"). The executable module
    // regions we classify as permanent are stable, so refresh the list at most
    // every kRegionRefreshTicks ticks. Reset on resetSnapshot (per-attach).
    QVector<MemoryRegion> m_classifyRegions;
    bool                  m_classifyRegionsValid = false;
    uint64_t              m_classifyRegionsTick = 0;
    static constexpr int  kRegionRefreshTicks = 64;

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
    // The single-node half of applyQuickTypeChange (hex shrink → pads, hex
    // grow → joinHexNodes, else changeNodeKind; emits nodeSelected).
    // retypeSelection calls this directly for a lone leaf so a stray footer /
    // array-element row in m_selIds can never re-route it into the batch path.
    void quickTypeChangeSingle(int nodeIdx, NodeKind kind);
    // m_selIds decoded to existing base node ids, deduped, ascending by
    // root-relative offset (ties by id). Only PLAIN rows contribute by
    // default; SF_ArrayElemAsArray lets an array-element row stand for its
    // Array node; SF_FooterAsContainer lets a footer `}` row stand for its
    // container (the ONLY selectable row a root class has — its header is
    // the command row); SF_SkipRoots drops parentId == 0 nodes;
    // SF_SkipContainers drops Struct / Array nodes. Member rows never
    // contribute.
    enum SelFilter : unsigned {
        SF_None = 0, SF_ArrayElemAsArray = 1, SF_SkipRoots = 2, SF_SkipContainers = 4,
        SF_FooterAsContainer = 8
    };
    QVector<uint64_t> orderedSelectedIds(unsigned flags) const;
    // Lift doc->pendingSavedSources (populated by RcxDocument::load
    // from the .rcx's "savedSources" array) into m_savedSources and
    // auto-activate the first one. Called once from the constructor.
    void ingestPendingSavedSources();
    void appendBytesDialog(QWidget* parent, uint64_t targetId);
    void handleMarginClick(RcxEditor* editor, int margin, int line, Qt::KeyboardModifiers mods);
    void updateCommandRow();
    void switchToSavedSource(int idx);
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
