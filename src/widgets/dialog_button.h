#pragma once

#include <QPushButton>
#include <QIcon>
#include "themes/thememanager.h"

namespace rcx {

// Unified push-button for use inside dialogs. All variants share a
// 30 px fixed height + square corners + hairline-border, outline-only
// resting state — the visual identity comes from text color and the
// :default focus border, not from a filled accent. Earlier rev had
// Primary filled in indHoverSpan (purple) which competed loudly with
// Destructive's amber fill; both have been muted to outlines.
//
// Variants:
//   Primary     — text in theme.text, border in theme.borderFocused;
//                 default-focus border flags it as the action target
//                 (OK / Save / Apply / Open).
//   Secondary   — text in theme.textDim, border in theme.border. The
//                 "back out" affordance (Cancel / Close / neutral
//                 toggles).
//   Destructive — text + border in theme.markerPtr (the conventional
//                 warning red, not amber). Delete / Discard / Unload —
//                 anything irreversible.
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
