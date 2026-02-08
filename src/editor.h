#pragma once
#include "core.h"
#include <QWidget>
#include <QSet>
#include <QPoint>

class QsciScintilla;
class QsciLexerCPP;

namespace rcx {

struct SavedSourceDisplay {
    QString text;
    bool active = false;
};

class RcxEditor : public QWidget {
    Q_OBJECT
public:
    explicit RcxEditor(QWidget* parent = nullptr);
    ~RcxEditor() override;

    void applyDocument(const ComposeResult& result);

    ViewState saveViewState() const;
    void restoreViewState(const ViewState& vs);

    QsciScintilla* scintilla() const { return m_sci; }
    const LineMeta* metaForLine(int line) const;
    int currentNodeIndex() const;
    void scrollToNodeId(uint64_t nodeId);

    // ── Column span computation ──
    static ColumnSpan typeSpan(const LineMeta& lm, int typeW = kColType);
    static ColumnSpan nameSpan(const LineMeta& lm, int typeW = kColType, int nameW = kColName);
    static ColumnSpan valueSpan(const LineMeta& lm, int lineLength, int typeW = kColType, int nameW = kColName);

    // ── Multi-selection ──
    QSet<int> selectedNodeIndices() const;

    // ── Inline editing ──
    bool isEditing() const { return m_editState.active; }
    bool beginInlineEdit(EditTarget target, int line = -1);
    void cancelInlineEdit();

    void applySelectionOverlay(const QSet<uint64_t>& selIds);
    void setCommandRowText(const QString& line);
    void setEditorFont(const QString& fontName);
    static void setGlobalFontName(const QString& fontName);

    // Custom type names (struct types from the tree) shown in type picker + lexer GlobalClass coloring
    void setCustomTypeNames(const QStringList& names);

    // Saved sources for quick-switch in source picker
    void setSavedSources(const QVector<SavedSourceDisplay>& sources) { m_savedSourceDisplay = sources; }

signals:
    void marginClicked(int margin, int line, Qt::KeyboardModifiers mods);
    void contextMenuRequested(int line, int nodeIdx, int subLine, QPoint globalPos);
    void nodeClicked(int line, uint64_t nodeId, Qt::KeyboardModifiers mods);
    void inlineEditCommitted(int nodeIdx, int subLine,
                             EditTarget target, const QString& text);
    void inlineEditCancelled();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    QsciScintilla*    m_sci    = nullptr;
    QsciLexerCPP*     m_lexer  = nullptr;
    QVector<LineMeta> m_meta;
    LayoutInfo        m_layout;  // cached from ComposeResult

    int m_marginStyleBase = -1;
    int m_hintLine = -1;

    // ── Hover cursor + highlight ──
    QPoint m_lastHoverPos;
    bool   m_hoverInside = false;
    uint64_t m_hoveredNodeId = 0;
    int      m_hoveredLine = -1;
    QSet<uint64_t> m_currentSelIds;
    QVector<int> m_hoverSpanLines;  // Lines with hover span indicators
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
    };
    InlineEditState m_editState;

    // ── Tab cycling state ──
    EditTarget m_lastTabTarget = EditTarget::Value;

    // ── Custom type names for type picker ──
    QStringList m_customTypeNames;

    // ── Saved sources for quick-switch ──
    QVector<SavedSourceDisplay> m_savedSourceDisplay;

    // ── Reentrancy guards ──
    bool m_clampingSelection = false;
    bool m_updatingComment = false;

    void setupScintilla();
    void setupLexer();
    void setupMargins();
    void setupFolding();
    void setupMarkers();
    void allocateMarginStyles();

    void applyMarginText(const QVector<LineMeta>& meta);
    void applyMarkers(const QVector<LineMeta>& meta);
    void applyFoldLevels(const QVector<LineMeta>& meta);
    void applyHexDimming(const QVector<LineMeta>& meta);
    void applyDataChangedHighlight(const QVector<LineMeta>& meta);
    void applyBaseAddressColoring(const QVector<LineMeta>& meta);
    void applyCommandRowPills();

    void commitInlineEdit();
    int  editEndCol() const;
    bool handleNormalKey(QKeyEvent* ke);
    bool handleEditKey(QKeyEvent* ke);
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
