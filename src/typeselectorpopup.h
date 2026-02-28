#pragma once
#include <QFrame>
#include <QFont>
#include <QVector>
#include <QString>
#include <QStringList>
#include <cstdint>
#include "core.h"

class QLineEdit;
class QListView;
class QStringListModel;
class QLabel;
class QToolButton;
class QButtonGroup;
class QWidget;

namespace rcx {

struct Theme;

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
    QString     classKeyword;                    // "struct", "class", "enum" (Composite only)
    bool        enabled       = true;            // false = grayed out (visible but not selectable)
    int         sizeBytes     = 0;               // size in bytes (for display)
    int         alignment     = 0;               // natural alignment in bytes
    int         fieldCount    = 0;               // child field count (composite only)
    QStringList fieldSummary;                     // first ~6 fields: "0x00: float x"
};

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
    void setTypes(const QVector<TypeEntry>& types, const TypeEntry* current = nullptr);
    void popup(const QPoint& globalPos);

    /// Show popup instantly with skeleton placeholders; call setTypes() to fill content.
    void popupLoading(const QPoint& globalPos);

    /// Force native window creation to avoid cold-start delay.
    void warmUp();

signals:
    void typeSelected(const TypeEntry& entry, const QString& fullText);
    void createNewTypeRequested();
    void saveRequested();
    void dismissed();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QLabel*           m_titleLabel   = nullptr;
    QToolButton*      m_escLabel     = nullptr;
    QToolButton*      m_createBtn    = nullptr;
    QToolButton*      m_saveBtn      = nullptr;
    QLineEdit*        m_filterEdit   = nullptr;
    QListView*        m_listView     = nullptr;
    QStringListModel* m_model        = nullptr;

    // Modifier toggles
    QWidget*          m_modRow       = nullptr;
    QToolButton*      m_btnPtr       = nullptr;
    QToolButton*      m_btnDblPtr    = nullptr;
    QToolButton*      m_btnArray     = nullptr;
    QLineEdit*        m_arrayCountEdit = nullptr;
    QButtonGroup*     m_modGroup     = nullptr;

    // Category filter checkboxes
    QWidget*          m_chipRow      = nullptr;
    QToolButton*      m_chipPrim     = nullptr;
    QToolButton*      m_chipTypes    = nullptr;
    QToolButton*      m_chipEnums    = nullptr;
    QLabel*           m_statusLabel  = nullptr;

    QVector<TypeEntry> m_allTypes;
    QVector<TypeEntry> m_filteredTypes;
    QVector<QVector<int>> m_matchPositions;
    TypeEntry          m_currentEntry;
    bool               m_hasCurrent = false;
    TypePopupMode      m_mode = TypePopupMode::FieldType;
    int                m_currentNodeSize = 0;
    int                m_pointerSize = 8;
    bool               m_loading = false;
    QFont              m_font;

    void applyFilter(const QString& text);
    void updateModifierPreview();
    void acceptCurrent();
    void acceptIndex(int row);
    int  nextSelectableRow(int from, int direction) const;
};

} // namespace rcx
