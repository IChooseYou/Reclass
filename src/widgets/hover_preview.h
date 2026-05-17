#pragma once

// Pluggable hover-preview registry.
//
// The editor builds a HoverPopupHost (file-local class in editor.cpp)
// that shows one of N preview "views" of the row the cursor sits on.
// Adding a new view (matrix-preview, vector-angle, color-swatch, …)
// means writing ONE HoverPreview subclass and calling
// HoverPreviewRegistry::add() — no other editor.cpp changes.
//
// All previews share the same dwell timing, the same anchor logic, the
// same QSettings persistence of "which view was last picked for this
// node-kind", and the same Tab / Shift+Tab keyboard cycling, all owned
// by the host. Concrete previews only need to answer two questions:
//
//   1. eligible(lm, node, ctx) — does this view make sense for that row?
//   2. widget(lm, node, ctx, parent) — produce a QWidget to display.

#include <QFont>
#include <QHash>
#include <QString>
#include <QVector>
#include <memory>

#include "core.h"

class QWidget;

namespace rcx {

struct Theme;
class  Provider;

// Bag of read-only references a HoverPreview can consult to decide
// eligibility + build its widget. Lifetime is the single hover tick.
struct HoverContext {
    QFont          editorFont;
    const Theme*   theme           = nullptr;
    // dataProvider == the active snapshot provider (or the real provider
    // if no snapshot). Use for reading the value AT the row's address.
    const Provider* dataProvider   = nullptr;
    // codeProvider == ALWAYS the real provider (never snapshot). Use
    // when you need to read code/data at an arbitrary process address
    // that won't be in the snapshot page table (function bodies,
    // pointer targets outside the captured pages, etc.).
    const Provider* codeProvider   = nullptr;
    const NodeTree* tree           = nullptr;
    // Per-nodeId value-history map. nullptr-safe; previews must guard.
    const QHash<uint64_t, ValueHistory>* history = nullptr;
};

class HoverPreview {
public:
    virtual ~HoverPreview() = default;

    // Stable identifier — used as QSettings key for "which preview was
    // last shown for this node-kind". Don't change once shipped.
    virtual QString id() const = 0;

    // Short label shown in the popup's title area (e.g. "Hex Dump").
    virtual QString tabLabel() const = 0;

    // Cheap (no-allocation, no provider reads) eligibility check —
    // runs on every hover tick. Heavy work belongs in widget().
    virtual bool eligible(const LineMeta& lm,
                          const Node&     node,
                          const HoverContext& ctx) const = 0;

    // Build / refresh the preview's content widget. Called only when
    // the preview becomes the active view in the host. Implementations
    // may cache and reuse the widget across calls; the host owns
    // parenting once the widget is attached.
    //
    // Return nullptr to signal "I'm eligible but produced no content
    // this tick" (e.g. read failed) — the host will hide.
    virtual QWidget* widget(const LineMeta& lm,
                            const Node&     node,
                            const HoverContext& ctx,
                            QWidget*        parent) = 0;
};

class HoverPreviewRegistry {
public:
    // Take ownership. Registration order is the default tie-breaker
    // when multiple previews are eligible and no QSettings preference
    // has been recorded yet.
    void add(std::unique_ptr<HoverPreview> p) {
        m_all.push_back(std::move(p));
    }

    // Returns raw pointers to previews whose eligible() returned true,
    // in registration order. Pointers are stable for the registry's
    // lifetime. Callers must NOT delete.
    QVector<HoverPreview*> eligibleFor(const LineMeta&  lm,
                                       const Node&      node,
                                       const HoverContext& ctx) const {
        QVector<HoverPreview*> out;
        out.reserve(int(m_all.size()));
        for (const auto& p : m_all) {
            if (p && p->eligible(lm, node, ctx))
                out.push_back(p.get());
        }
        return out;
    }

    int size() const { return int(m_all.size()); }

private:
    std::vector<std::unique_ptr<HoverPreview>> m_all;
};

} // namespace rcx
