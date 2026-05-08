#pragma once

#include <QPushButton>
#include <QIcon>
#include "themes/thememanager.h"

namespace rcx {

// Unified push-button for use inside dialogs. Mirrors the visual
// language of ScanButton (the existing best-in-class button in the
// scanner panel) so that — should we ever decide to unify — the two
// classes already match: 28 px fixed height, 14 px icons, hairline
// border, optional 2 px accent stripe on top.
//
// Variants:
//   Primary     — accent stripe in theme.indHoverSpan (the default
//                 "go ahead" affordance: OK, Save, Apply, Open).
//   Secondary   — no accent stripe (Cancel, Close, neutral toggles).
//   Destructive — accent stripe in theme.indHeatHot, hover with red
//                 tint (Delete, Discard, Unload — irreversible work).
//
// Theme-aware: subscribes to ThemeManager::themeChanged and
// re-applies its stylesheet automatically. Callers don't need to
// remember to push theme updates.
class DialogButton : public QPushButton {
    Q_OBJECT
public:
    enum Variant { Primary, Secondary, Destructive };

    explicit DialogButton(const QString& label,
                          Variant v = Secondary,
                          QWidget* parent = nullptr);
    explicit DialogButton(const QIcon& icon, const QString& label,
                          Variant v = Secondary,
                          QWidget* parent = nullptr);

    void setVariant(Variant v);
    Variant variant() const { return m_variant; }

    void applyTheme();

private:
    Variant m_variant;
    void init();
};

} // namespace rcx
