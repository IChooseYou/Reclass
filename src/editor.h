#pragma once
#include "core.h"
#include "providerregistry.h"
#include "themes/theme.h"
#include <QWidget>
#include <QSet>
#include <QPoint>
#include <QHash>
#include <QVariantAnimation>
#include <functional>
#include <memory>

class QLabel;
class QLineEdit;
class QsciScintilla;
class QsciLexerCPP;

namespace rcx {

class HoverPreviewRegistry;  // src/widgets/hover_preview.h

class RcxEditor : public QWidget {
    Q_OBJECT
public:
    explicit RcxEditor(QWidget* parent = nullptr);
    ~RcxEditor() override;

    void applyDocument(const ComposeResult& result);

    ViewState saveViewState() const;
    void restoreViewState(const ViewState& vs);

    QsciScintilla* scintilla() const { return m_sci; }
    // m_historyPopup is the inline-edit value-history popup (with "Set"
    // buttons); m_popupHost is the unified hover preview host. Tests
    // and external observers query whichever is relevant via the
    // accessors below.
    QWidget* historyPopup() const { return m_historyPopup; }
    QWidget* hoverPopup() const;          // returns m_popupHost
    QString  hoverPopupActiveId() const;  // active preview's id (empty if host not visible)
    const LineMeta* metaForLine(int line) const;
    int currentNodeIndex() const;
    void scrollToNodeId(uint64_t nodeId);
    void smoothScrollToNodeId(uint64_t nodeId);
    void setFocusNode(uint64_t nodeId);
    void clearFocusNode();
    bool isFocusGlowActive() const { return m_focusNodeId != 0; }
    void setPresentationMode(bool on) { m_presentationMode = on; }
    void setHoverEffects(bool on);
    bool hoverEffects() const { return m_hoverEffects; }
    void showFindBar();
    void dismissHistoryPopup();
    void dismissAllPopups();

    // ── Column span computation ──
    static ColumnSpan typeSpan(const LineMeta& lm, int typeW = kColType);
    static ColumnSpan nameSpan(const LineMeta& lm, int typeW = kColType, int nameW = kColName);
    static ColumnSpan valueSpan(const LineMeta& lm, int lineLength, int typeW = kColType, int nameW = kColName);

    // ── Multi-selection ──
    QSet<int> selectedNodeIndices() const;

    // ── Inline editing ──
    bool isEditing() const { return m_editState.active; }
    int editSpanStart() const { return m_editState.spanStart; }
    int editEnd() const;  // public accessor for editEndCol()
    bool beginInlineEdit(EditTarget target, int line = -1, int col = -1);
    void cancelInlineEdit();
    void setHexEditPending(bool v) { m_hexEditPending = v; }
    void setStaticCompletions(const QStringList& words) { m_staticCompletions = words; }

    void applySelectionOverlay(const QSet<uint64_t>& selIds);
    void setCommandRowText(const QString& line);
    void setEditorFont(const QString& fontName);
    static void setGlobalFontName(const QString& fontName);
    static QString globalFontName();
    void applyTheme(const Theme& theme);

    // Custom type names (struct types from the tree) shown in type picker + lexer GlobalClass coloring
    QString textWithMargins() const;
    void setCustomTypeNames(const QStringList& names);
    void setValueHistoryRef(const QHash<uint64_t, ValueHistory>* ref) { m_valueHistory = ref; }
    void setExprEvaluator(std::function<QString(const QString&)> fn) { m_exprEvaluator = std::move(fn); }
    void setProviderRef(const Provider* prov, const Provider* realProv, const NodeTree* tree) {
        m_disasmProvider = prov; m_disasmRealProv = realProv; m_disasmTree = tree;
    }

    void setRelativeOffsets(bool rel) { m_relativeOffsets = rel; reformatMargins(); }

    // Saved sources for quick-switch in source picker
    void setSavedSources(const QVector<SavedSourceDisplay>& sources) { m_savedSourceDisplay = sources; }

signals:
    void marginClicked(int margin, int line, Qt::KeyboardModifiers mods);
    void contextMenuRequested(int line, int nodeIdx, int subLine, QPoint globalPos);
    void keywordConvertRequested(const QString& newKeyword);
    void nodeClicked(int line, uint64_t nodeId, Qt::KeyboardModifiers mods);
    void inlineEditCommitted(int nodeIdx, int subLine,
                             EditTarget target, const QString& text,
                             uint64_t resolvedAddr = 0);
    void inlineEditCancelled();
    void typeSelectorRequested();
    void typePickerRequested(EditTarget target, int nodeIdx, QPoint globalPos);
    void sourcePopupRequested(QPoint globalPos);
    void insertAboveRequested(int nodeIdx, NodeKind kind);
    void commentEditRequested();
    void relativeOffsetsChanged(bool relative);
    // Tail-chip click signals.
    //
    // RTTI chip click — extended to carry the demangled class name and
    // the hosting node id so MainWindow can attachRttiClassToPointer()
    // directly (auto-create class + wire pointer). vtableAddr is kept
    // for the "Open RTTI Browser" menu path which still uses it.
    void rttiChipClicked(uint64_t vtableAddr, QString className,
                          uint64_t hostNodeId);
    // Null-vtable RTTI chip click — fires when the user clicks the
    // "(Name class…)" CTA chip on a pointer field with value 0x00.
    // MainWindow opens inline rename on the current tab's root struct.
    void rttiNullChipClicked();
    void enumChipClicked(int nodeIdx, uint64_t enumRefNodeId, int64_t currentValue,
                         QPoint globalPos);
    // TypeHint chip clicked — caller (controller) converts the hex node
    // to the inferred kind(s). One click commits the inference; if the
    // suggestion is a multi-kind split (e.g. int32×2) one click splits
    // the whole run.
    void typeHintChipClicked(int nodeIdx, QVector<NodeKind> inferredKinds);
    void appendBytesRequested(uint64_t structId, int byteCount);
    void trimHexRequested(uint64_t structId);
    void appendEnumMembersRequested(uint64_t enumId, int count);
    // Single-field append from footer "+Field" pill. Adds one Hex64 field
    // (or one enum member, when the parent is an enum) at the end.
    void appendSingleFieldRequested(uint64_t structId);
    void deleteSelectedRequested();
    void duplicateSelectedRequested();
    // Real clipboard (rcx-clipboard/v1 MIME + plaintext fallback). Controller
    // handles the actual serialization/paste + undo; editor just signals.
    void copyNodesRequested();
    void cutNodesRequested();
    void pasteNodesRequested();
    // Fires after applyDocument() has pushed new text into Scintilla. Carries
    // the composed text so a minimap (or any passive mirror) can copy it
    // without polling.
    void documentApplied(const QString& text);
    void quickTypeChangeRequested(int nodeIdx, NodeKind targetKind);
    void cycleSameSizeTypeRequested(int nodeIdx, int direction);  // -1=prev, +1=next
    void moveNodeRequested(int nodeIdx, int direction);  // -1=up, +1=down
    void collapseAllRequested();
    void expandAllRequested();
    // F12: navigate to the type definition referenced by the current node
    // (Pointer.refId, Struct.refId, or Array element struct). Controller
    // resolves and opens / focuses the target.
    void goToDefinitionRequested(int nodeIdx);
    // Ctrl+Click on a navigable type span: open the referenced struct in
    // a NEW tab (sharing the same document). Controller resolves and asks
    // MainWindow to spawn the tab.
    void openTypeInNewTabRequested(int nodeIdx);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    QsciScintilla*    m_sci    = nullptr;
    QsciLexerCPP*     m_lexer  = nullptr;
    QVector<LineMeta> m_meta;
    LayoutInfo        m_layout;  // cached from ComposeResult
    // Previous-frame text used by applyDocument's diff-and-patch path so
    // we can avoid the full Scintilla setText() (~659 µs) on the common
    // append/edit case.
    QString           m_prevText;
    QVector<LineMeta> m_prevMeta;
    // Tracks whether the last applyDocument took the patch path (cheap, no
    // marker loss outside the patched range) or the full-replace path
    // (which wipes all markers). applySelectionOverlay uses this together
    // with set-equality to decide whether it can skip its rebuild on a
    // refresh tick that didn't actually change anything visible.
    bool              m_lastApplyWasPatch = false;
    // Skip-on-no-change caches for the refresh-tail path.
    QString           m_lastCommandRowText;

    // ── Toggle: absolute vs relative offset margin
    bool m_relativeOffsets = true;

    int m_marginStyleBase = -1;
    int m_hintLine = -1;

    // ── Hover cursor + highlight ──
    QPoint m_lastHoverPos;
    bool   m_hoverInside = false;
    // ── Chip hover / pressed state ──
    // Tracks the clickable chip the cursor is currently over, so the
    // pill can light up (hover) or sink (pressed) like a button. Held
    // in screen-line + doc-column space so a single chip span can be
    // identified across scroll. -1 line == nothing hovered.
    int  m_chipHoverLine     = -1;
    int  m_chipHoverStartCol = -1;
    int  m_chipHoverEndCol   = -1;
    bool m_chipPressed       = false;
    uint64_t m_hoveredNodeId = 0;
    int      m_hoveredLine = -1;
    uint64_t m_prevHoveredNodeId = 0;  // for incremental marker update
    int      m_prevHoveredLine = -1;   // for incremental marker update
    QSet<uint64_t> m_currentSelIds;
    QVector<int> m_hoverSpanLines;  // Lines with hover span indicators
    // ── nodeId → display-line index (built in applyDocument) ──
    QHash<uint64_t, QVector<int>> m_nodeLineIndex;
    // ── Drag selection ──
    bool m_dragging = false;
    bool m_dragStarted = false;   // true once drag threshold exceeded
    int  m_dragLastLine = -1;
    QPoint m_dragStartPos;        // viewport coords at press
    Qt::KeyboardModifiers m_dragInitMods = Qt::NoModifier;

    // ── Deferred click (protects multi-select on double-click) ──
    uint64_t m_pendingClickNodeId = 0;
    int      m_pendingClickLine = -1;
    Qt::KeyboardModifiers m_pendingClickMods = Qt::NoModifier;

    // ── Inline edit state ──
    struct InlineEditState {
        bool       active    = false;
        int        line      = -1;
        int        nodeIdx   = -1;
        int        subLine   = 0;
        EditTarget target    = EditTarget::Name;
        int        spanStart = 0;
        int        linelenAfterReplace = 0;
        QString    original;
        long       posStart  = 0;   // Scintilla position of edit start
        long       posEnd    = 0;   // Scintilla position of edit end
        NodeKind   editKind = NodeKind::Int32;
        int        commentCol = -1;  // fixed comment column (stored at edit start)
        bool       lastValidationOk = true;  // track state to avoid redundant updates
        bool       hexOverwrite = false;  // true for hex-byte / ASCII-preview fixed-length editing
        // Trailing padding written by beginInlineEdit so the comment area
        // exists on a line that compose left short. Tracked here so
        // endInlineEdit can strip it back off — leaving Scintilla in sync
        // with m_prevText. Without this, the next applyDocument's diff
        // computes against a stale prefix and corrupts an unrelated line.
        long       padBytes  = 0;     // utf-8 bytes appended to the line
        long       padPos    = 0;     // byte position right before padding
        // For Comment-edit's trim-then-placeholder dance: bytes of trailing
        // whitespace begin trimmed away. End restores them so the line ends
        // up byte-identical to its pre-begin shape.
        int        padRestoreSpaces = 0;
    };
    InlineEditState m_editState;
    QStringList m_staticCompletions;  // autocomplete words for StaticExpr editing

    // ── Tab cycling state ──
    EditTarget m_lastTabTarget = EditTarget::Value;

    // ── Custom type names for type picker ──
    QStringList m_customTypeNames;

    // ── Saved sources for quick-switch ──
    QVector<SavedSourceDisplay> m_savedSourceDisplay;

    // ── Value history ref (owned by controller) ──
    const QHash<uint64_t, ValueHistory>* m_valueHistory = nullptr;
    std::function<QString(const QString&)> m_exprEvaluator;
    QLabel* m_exprResultLabel = nullptr;
    QWidget* m_historyPopup = nullptr;  // inline-edit ValueHistoryPopup (file-local in editor.cpp)
    // The unified hover preview host (HoverPopupHost, file-local in
    // editor.cpp) replaces the old m_disasmPopup + m_structPreviewPopup
    // pair. It hosts whichever HoverPreview is eligible for the row
    // under the cursor — value history, hex dump, disasm, struct
    // target, future matrix/vector/color, etc. — and the user cycles
    // through them with Tab / Shift+Tab.
    QWidget* m_popupHost = nullptr;
    // The registry owning the HoverPreview subclasses. Filled once in
    // setupScintilla. New preview kinds drop in as one registry->add()
    // call there. Stored as unique_ptr<HoverPreviewRegistry> in the
    // impl; the header keeps it forward-declared.
    std::unique_ptr<HoverPreviewRegistry> m_previewRegistry;
    QWidget* m_arrowTooltip = nullptr;       // RcxTooltip (arrow callout)
    const Provider* m_disasmProvider = nullptr;   // snapshot or real — for reading tree data
    const Provider* m_disasmRealProv = nullptr;   // real process provider — for reading code at arbitrary addresses
    const NodeTree* m_disasmTree = nullptr;

    // ── Find bar ──
    QWidget*   m_findBarContainer = nullptr;
    QLineEdit* m_findBar = nullptr;
    long       m_findPos = 0;
    void hideFindBar();

    // ── Hex inline edit ──
    bool m_hexEditPending = false;  // set by context menu before calling beginInlineEdit

    // ── Reentrancy guards ──
    bool m_applyingDocument = false;
    bool m_clampingSelection = false;
    bool m_updatingComment = false;

    // ── Hover effects toggle ──
    bool m_hoverEffects = true;

    // ── Hover dwell for preview popups ──
    // Value-history / disasm / struct-preview popups wait this long
    // before appearing so casual mouse-overs don't keep flashing them
    // open. The arrow tooltip (RcxTooltip) on command-row spans is
    // intentionally NOT gated — it's a discoverability cue that needs
    // to feel instant. Timer restarts whenever the (nodeId, line)
    // hover target changes; popup blocks read m_hoverDwellElapsed
    // before showing.
    QTimer*  m_hoverDwellTimer = nullptr;
    bool     m_hoverDwellElapsed = false;
    uint64_t m_dwellNodeId = 0;
    int      m_dwellLine = -1;

    // ── Presentation mode (smooth scroll + focus glow) ──
    bool m_presentationMode = false;
    QVariantAnimation* m_scrollAnim = nullptr;
    bool m_scrollAnimActive = false;
    QTimer* m_focusGlowTimer = nullptr;
    uint64_t m_focusNodeId = 0;
    int m_glowPhase = 0;
    QColor m_focusGlowColor;  // cached from theme
    QColor m_glowDim;         // pre-blended dim pulse (opaque)
    QColor m_glowBright;      // pre-blended bright pulse (opaque)

    void setupScintilla();
    void setupLexer();
    void setupMargins();
    void setupFolding();
    void setupMarkers();
    void allocateMarginStyles();

    // Optional [first, last] line range. When set (>=0), the per-line pass
    // operates only on those lines; markers/indicators on lines outside
    // are assumed to have survived the most recent SCI_REPLACETARGET.
    // When unset (-1), full-doc pass — used by the fullReplace path.
    void applyLineAttributes(const QVector<LineMeta>& meta, int firstLine = -1, int lastLine = -1);
    void reformatMargins(int firstLine = -1, int lastLine = -1);
    void applyHexDimming(const QVector<LineMeta>& meta, int firstLine = -1, int lastLine = -1);
    void applyHeatmapHighlight(const QVector<LineMeta>& meta, const QVector<QString>& lineTexts,
                                int firstLine = -1, int lastLine = -1);
    void applySymbolColoring(const QVector<LineMeta>& meta, const QVector<QString>& lineTexts,
                              int firstLine = -1, int lastLine = -1);
    void applyBaseAddressColoring(const QVector<LineMeta>& meta);
    void applyCommandRowPills();

    void commitInlineEdit();
    void updateExprResultPopup();
    int  editEndCol() const;
    bool handleNormalKey(QKeyEvent* ke);
    bool handleEditKey(QKeyEvent* ke);
    bool handleHexEditKey(QKeyEvent* ke);
    void showTypeAutocomplete();
    void showSourcePicker();
    void showTypeListFiltered(const QString& filter);
    void updateTypeListFilter();
    void showPointerTargetPicker();
    void showPointerTargetListFiltered(const QString& filter);
    void updatePointerTargetFilter();
    void paintEditableSpans(int line);
    void updateEditableIndicators(int line);
    void applyHoverCursor();
    void applyHoverHighlight();
    // Resolve the index in `eligible` of the preview the user last
    // picked for `kind` (QSettings persistence). Falls back to 0 when
    // no preference is recorded or the recorded id is gone.
    int  pickLastUsedPreviewIdx(const QVector<class HoverPreview*>& eligible,
                                NodeKind kind) const;
    // Chip button-state visuals — repaint the hover/pressed pill overlay
    // for the chip the cursor currently maps to. Idempotent; called from
    // MouseMove, MouseButtonPress, MouseButtonRelease, Leave.
    struct HitInfo;
    void updateChipHover(const HitInfo& h);
    void clearChipButtonState();
    void applyChipButtonOverlay();
    void validateEditLive();
    void setEditComment(const QString& comment);
    void clampEditSelection();

    // ── Refactored helpers ──
    struct HitInfo { int line = -1; int col = -1; uint64_t nodeId = 0; bool inFoldCol = false; };
    HitInfo hitTest(const QPoint& viewportPos) const;

    struct EndEditInfo { int nodeIdx; int subLine; EditTarget target; };
    EndEditInfo endInlineEdit();

    struct NormalizedSpan { int start = 0; int end = 0; bool valid = false; };
    NormalizedSpan normalizeSpan(const ColumnSpan& raw, const QString& lineText,
                                 EditTarget target, bool skipPrefixes) const;

    // ── Indicator helpers (dedupe + UTF-8 safe) ──
    void clearIndicatorLine(int indic, int line);
    void fillIndicatorCols(int indic, int line, int colA, int colB);
    bool resolvedSpanFor(int line, EditTarget t, NormalizedSpan& out,
                         QString* lineTextOut = nullptr) const;
};

} // namespace rcx
