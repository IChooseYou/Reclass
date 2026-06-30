#pragma once
#include <QFrame>
#include <QFont>
#include <QVector>
#include <QHash>
#include <QString>
#include <QStringList>
#include <cstdint>
#include "core.h"

class QLineEdit;
class QListView;
class QStringListModel;
class QLabel;
class QAbstractButton;
class QToolButton;
class QButtonGroup;
class QWidget;

namespace rcx {

struct Theme;
class DialogButton;

// ── Popup mode ──

enum class TypePopupMode { Root, FieldType, ArrayElement, PointerTarget };

// ── Type entry (explicit discriminant — no sentinel IDs) ──

struct TypeEntry {
    enum Kind { Primitive, Composite, Section };
    enum Category { CatPrimitive, CatType, CatEnum };

    Kind        entryKind     = Primitive;
    Category    category      = CatPrimitive;
    NodeKind    primitiveKind = NodeKind::Hex8;  // valid when entryKind==Primitive
    uint64_t    structId      = 0;               // valid when entryKind==Composite
    QString     displayName;
    // RVA variant of a pointer entry. When true, picking this entry
    // sets node.isRelative=true so the pointer dereferences as
    // (parentBase + value) instead of value-as-absolute-address.
    // Surfaced as a separate entry like "Pointer32 (RVA)" rather
    // than a modifier toggle so it's discoverable from the same flow
    // as Pointer32 / FuncPtr32 — no new UI surface.
    bool        isRelative    = false;
    QString     classKeyword;                    // "struct", "class", "enum" (Composite only)
    bool        enabled       = true;            // false = grayed out (visible but not selectable)
    int         sizeBytes     = 0;               // size in bytes (for display)
    int         alignment     = 0;               // natural alignment in bytes
    int         fieldCount    = 0;               // child field count (composite only)
    QStringList fieldSummary;                     // first ~6 fields: "0x00: float x"

    // Kind-group for visual grouping + coloring (Hex/Int/UInt/Float/Ptr/Vec/Str/Ctr)
    QString     kindGroup;

    // Synthetic, selectable "Show all types / Show common only" row appended
    // to the bottom of the unfiltered grouped view. Activating it toggles
    // m_showAllTypes and re-filters instead of emitting a type selection.
    bool        isExpandToggle = false;

    // Synthetic, selectable "＋ Create class/struct" action row. Activating it
    // creates a brand-new composite (named createName, or auto-named when
    // empty) and applies it to the field in one step via
    // createNewTypeRequested(modifier, count, createName, createKeyword) —
    // no separate "+ New" trip. Appears in the filtered view when the typed
    // base is a fresh identifier (B1) and as pinned rows unfiltered (B5).
    bool        isCreateNew    = false;
    QString     createName;                      // typed identifier; empty = auto-name
    QString     createKeyword;                   // "" = struct, "class" = class

    // Collapsible section state (Section rows only). A collapsible section
    // header shows a ▸/▾ chevron + member count and toggles its members'
    // visibility on click / Enter. Common primitive groups default to
    // expanded; "Your classes" and the std-lib group default to collapsed so
    // a large SDK never floods the list or buries the common primitives.
    bool        sectionCollapsible = false;
    bool        sectionExpanded    = true;
    QString     sectionKey;                      // stable collapse-state key
};

// Kind-group string for a NodeKind (Hex/Int/UInt/Float/Ptr/Vec/Str/Ctr)
QString kindGroupFor(NodeKind k);
// Per-group accent color (returns theme-derived color for each kind group)
QColor kindGroupColor(const QString& group);
// Dimmed variant (for size bar background)
QColor kindGroupDimColor(const QString& group);

// ── Parsed type spec (shared between popup filter and inline edit) ──

struct TypeSpec {
    QString baseName;
    bool    isPointer  = false;
    int     ptrDepth   = 0;       // 1 = *, 2 = ** (only meaningful when isPointer)
    int     arrayCount = 0;       // 0 = not array
};

TypeSpec parseTypeSpec(const QString& text);

// ── Popup widget ──

class TypeSelectorPopup : public QFrame {
    Q_OBJECT
public:
    explicit TypeSelectorPopup(QWidget* parent = nullptr);

    void setFont(const QFont& font);
    void setTitle(const QString& title);
    void setMode(TypePopupMode mode);
    void applyTheme(const Theme& theme);
    void setCurrentNodeSize(int bytes);
    void setPointerSize(int bytes);
    void setModifier(int modId, int arrayCount = 0);
    // Legacy modifier id (0=plain, 1=*, 2=**, 3=[]) from the segmented control.
    int  currentModId() const;
    void setTypes(const QVector<TypeEntry>& types, const TypeEntry* current = nullptr);
    // Most-recent-first list of type display names. Surfaces a "Recent"
    // pseudo-section at the top of group view when non-empty.
    void setRecentTypes(const QStringList& names) { m_recentNames = names; }
    void popup(const QPoint& globalPos);

    /// Show popup instantly with skeleton placeholders; call setTypes() to fill content.
    void popupLoading(const QPoint& globalPos);

    /// Force native window creation to avoid cold-start delay.
    void warmUp();

    /// Test accessor: the current filtered/sectioned row model (read-only).
    const QVector<TypeEntry>& filteredTypes() const { return m_filteredTypes; }

    /// Test/harness hook: force the simple/full view and re-render, the same
    /// way the bottom "Show all types" row does — so render harnesses can grab
    /// the full (modifier-visible) layout deterministically.
    void setShowAllTypesForTest(bool all);

    /// One-time per-process primer: absorbs ~300ms DLL/style/font init cost.
    /// Call early (e.g. from main() or MainWindow constructor) so the first
    /// user-visible popup open is fast on all platforms.
    static void preload();

signals:
    void typeSelected(const TypeEntry& entry, const QString& fullText);
    // name = typed/desired type name ("" → controller auto-names);
    // keyword = "" struct / "class" class. Threaded so the chooser can
    // create+apply a named class in one step (B1/B2/B5).
    void createNewTypeRequested(int modifierId, int arrayCount,
                                const QString& name, const QString& keyword);
    void saveRequested();
    void dismissed();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel*           m_titleLabel   = nullptr;
    QToolButton*      m_escLabel     = nullptr;
    DialogButton*     m_createBtn    = nullptr;
    DialogButton*     m_saveBtn      = nullptr;
    QLineEdit*        m_filterEdit   = nullptr;
    QListView*        m_listView     = nullptr;
    QStringListModel* m_model        = nullptr;

    // Modifier: a joined segmented "Apply as: Value | Pointer | Array" control
    // (exclusive, default Value). Pointer reveals a ×2 depth toggle; Array
    // reveals the count edit. currentModId() maps the segment+depth back to the
    // legacy modId (0=plain, 1=*, 2=**, 3=[]) used by accept/preview/apply.
    QWidget*          m_modRow       = nullptr;
    QLabel*           m_modLabel     = nullptr;
    QToolButton*      m_segValue     = nullptr;
    QToolButton*      m_segPointer   = nullptr;
    QToolButton*      m_segArray     = nullptr;
    QToolButton*      m_ptrDouble    = nullptr;
    QLineEdit*        m_arrayCountEdit = nullptr;
    QButtonGroup*     m_modGroup     = nullptr;  // ids 0=Value 1=Pointer 2=Array

    // Sort toolbar row (group/name/size + density/detail toggles). Stored as a
    // member so simple mode can hide the whole strip in one call.
    QWidget*          m_sortRow      = nullptr;

    // Kind-group filter chips (Hex, Int, UInt, Float, Ptr, Vec, Str, Ctr)
    QWidget*          m_chipRow      = nullptr;
    QHash<QString, QAbstractButton*> m_groupChips;
    QLabel*           m_statusLabel  = nullptr;

    QLabel*           m_footerLabel  = nullptr;

    // Detail pane (togglable right panel)
    QWidget*          m_detailPane   = nullptr;
    QLabel*           m_detailContent = nullptr;
    QToolButton*      m_detailBtn    = nullptr;  // toggle button in sort toolbar
    bool              m_showDetail   = false;
    bool              m_compact      = false;
    // When false (default), the unfiltered grouped view shows only the
    // common primitive set (isCommonKind) + user structs; the std-lib
    // "Common Types" section and long-tail primitives are hidden behind a
    // bottom "Show all types" toggle row. Typing a filter always searches
    // every type regardless. Persists for the cached popup's lifetime.
    bool              m_showAllTypes = false;

    // Per-section expand/collapse state, keyed by section key (kindGroup).
    // Absent key → the section's built-in default (common groups expanded,
    // "Your classes" / std-lib collapsed). m_showAllTypes, when set by the
    // test/harness hook, force-expands every section.
    QHash<QString, bool> m_sectionExpanded;

    QVector<TypeEntry> m_allTypes;
    QVector<TypeEntry> m_filteredTypes;
    QStringList        m_recentNames;  // most-recent-first; emitted as top section
    QVector<QVector<int>> m_matchPositions;
    TypeEntry          m_currentEntry;
    bool               m_hasCurrent = false;
    TypePopupMode      m_mode = TypePopupMode::FieldType;
    int                m_currentNodeSize = 0;
    int                m_pointerSize = 8;
    bool               m_loading = false;
    // True once the user committed a pick this open (typeSelected /
    // createNewTypeRequested), so hideEvent doesn't ALSO emit dismissed() —
    // dismissed means "closed without choosing". Reset on each popup().
    bool               m_accepted = false;
    QFont              m_font;
    int                m_cachedMaxNameLen = 0; // longest displayName length (chars)

    // Sort toolbar state
    enum SortMode { SortGroup, SortName, SortSize, SortAlign };
    SortMode           m_sortMode = SortGroup;
    int                m_sortDir  = 1;  // 1=ascending, -1=descending
    QVector<QToolButton*> m_sortBtns;

    void applyFilter(const QString& text);
    // Show/hide the chip row, sort toolbar, and modifier row based on
    // m_showAllTypes: simple (Common) mode leaves just the filter + a minimal
    // common-types list; All mode reveals the full chrome. Also syncs the
    // prominent [Common|All] toggle's checked state.
    void updateModeChrome();
    // Size the popup to (w,h) and place it at globalPos, shifting the origin
    // back onto the screen (rather than shrinking to a sliver / negative size)
    // when it would overflow the right/bottom edge. Shared by popup() and
    // popupLoading() so their geometry can't diverge. Shows + focuses the popup.
    void placeOnScreen(const QPoint& globalPos, int w, int h);
    // Apply/hide the ×2 depth toggle + array count edit for the active segment.
    // focusCount=true (a user segment click) also focuses the count edit; the
    // text/programmatic paths pass false so they don't steal filter focus.
    void syncModifierExtras(bool focusCount = false);
    void updateModifierPreview();
    void updateDetailPane();
    void acceptCurrent();
    void acceptIndex(int row);
    int  nextSelectableRow(int from, int direction) const;
};

} // namespace rcx
