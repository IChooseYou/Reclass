#include "dialog_button.h"
#include <QStringLiteral>

namespace rcx {

DialogButton::DialogButton(const QString& label, Variant v, QWidget* parent)
    : QPushButton(label, parent), m_variant(v) {
    init();
}

DialogButton::DialogButton(const QIcon& icon, const QString& label,
                           Variant v, QWidget* parent)
    : QPushButton(icon, label, parent), m_variant(v) {
    init();
}

void DialogButton::init() {
    setIconSize(QSize(14, 14));
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(30);
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this](const Theme&){ applyTheme(); });
}

void DialogButton::setVariant(Variant v) {
    m_variant = v;
    applyTheme();
}

// Choose readable foreground (black or white) for a filled background.
// Pure-luminance heuristic — the indHoverSpan accent can land anywhere
// on the theme dial (deep blue, warm orange, etc.) so per-theme
// branching would multiply with no payoff.
static QString contrastingFg(const QColor& bg) {
    // Relative luminance per WCAG (simplified — gamma omitted for cost).
    double L = (0.299 * bg.red() + 0.587 * bg.green() + 0.114 * bg.blue()) / 255.0;
    return L > 0.55 ? QStringLiteral("#000000") : QStringLiteral("#ffffff");
}

void DialogButton::applyTheme() {
    const auto& t = ThemeManager::instance().current();
    // The three variants should be visually distinct AT REST, not only on
    // hover. Earlier all three started flat-transparent and only their
    // hover tints differed — meaning the user couldn't tell Cancel from
    // Discard without mousing over both. The redesign:
    //
    //   Primary     → filled accent. The obvious "this is the action you
    //                 want" target. Auto-contrast foreground (black on
    //                 light accents, white on dark accents).
    //   Secondary   → outline-only with neutral border. The "back out"
    //                 affordance, low-key but readable.
    //   Destructive → red border + red text at rest, flips to filled
    //                 red on hover — visible WARNING even before hover.
    //                 The earlier rgba(...,90) wash read as a dirty
    //                 smudge; this is a clean red identity.
    QString bg, fg, border, hoverBg, hoverFg, pressedBg, focusBorder;
    switch (m_variant) {
    case Primary: {
        QColor accent = t.indHoverSpan;
        bg          = accent.name();
        fg          = contrastingFg(accent);
        border      = accent.name();
        hoverBg     = accent.lighter(115).name();
        hoverFg     = fg;
        pressedBg   = accent.darker(112).name();
        focusBorder = accent.lighter(125).name();
        break;
    }
    case Secondary:
        bg          = QStringLiteral("transparent");
        fg          = t.text.name();
        border      = t.border.name();
        hoverBg     = t.hover.name();
        hoverFg     = t.text.name();
        pressedBg   = t.hover.darker(112).name();
        focusBorder = t.borderFocused.name();
        break;
    case Destructive: {
        QColor warn = t.indHeatHot;
        bg          = QStringLiteral("transparent");
        fg          = warn.name();
        border      = warn.name();
        hoverBg     = warn.name();
        hoverFg     = contrastingFg(warn);
        pressedBg   = warn.darker(115).name();
        focusBorder = warn.lighter(125).name();
        break;
    }
    }
    // Stylesheet notes:
    //   • Wider min-width (88) lets "Save changes" sit on one line
    //     without truncation; "OK" / "Cancel" still look balanced.
    //   • 4 px radius reads as a button rather than a flat rectangle
    //     without going full-pill.
    //   • Vertical padding 5 px keeps text optically centered in 30 px
    //     height (was 2 px which sat the text high).
    //   • Disabled is unified across variants — text muted, no fill.
    //   • :focus combines a stronger border with an outline offset
    //     equivalent (via a 2 px outer ring drawn by tightening padding
    //     and bumping border width) — keyboard users can see which
    //     button Enter will hit.
    setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 4px;"
        "  padding: 5px 16px;"
        "  min-width: 88px;"
        "  font-weight: 500;"
        "}"
        "QPushButton:hover {"
        "  background: %4;"
        "  color: %5;"
        "  border-color: %4;"
        "}"
        "QPushButton:pressed {"
        "  background: %6;"
        "  color: %5;"
        "  border-color: %6;"
        "}"
        "QPushButton:focus {"
        "  border: 1px solid %7;"
        "  padding: 5px 16px;"
        "}"
        "QPushButton:default {"
        "  border-width: 1px;"
        "}"
        "QPushButton:disabled {"
        "  background: transparent;"
        "  color: %8;"
        "  border-color: %9;"
        "}")
        .arg(bg, fg, border,
             hoverBg, hoverFg, pressedBg,
             focusBorder,
             t.textMuted.name(), t.border.name()));
}

} // namespace rcx
