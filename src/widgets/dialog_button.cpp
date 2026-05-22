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

void DialogButton::applyTheme() {
    const auto& t = ThemeManager::instance().current();
    // All variants are outline-only at rest — no filled accent. Earlier
    // rev had Primary filled in indHoverSpan (purple) while Destructive
    // used indHeatHot (amber); the two competed and read as garish in a
    // small dialog. New language:
    //
    //   Primary     → text in t.text, border in t.borderFocused.
    //                 The default-button highlight (Qt's :default
    //                 selector) is what flags it as the action target —
    //                 no big fill needed.
    //   Secondary   → text in t.textDim, border in t.border. The "back
    //                 out" affordance, lowest-key.
    //   Destructive → text + border in markerPtr (the conventional red
    //                 warning hue, not the amber indHeatHot which read
    //                 as "warm hint" not "irreversible action").
    //
    // All three flip to a soft `t.hover` fill on mouseover so the
    // hit-target is unambiguous without committing to a color identity
    // at rest. No rounded corners — the rest of the app reads square.
    QString bg = QStringLiteral("transparent");
    QString hoverFill = t.hover.name();
    QString hoverFg, fg, border, focusBorder;
    switch (m_variant) {
    case Primary:
        fg          = t.text.name();
        border      = t.borderFocused.name();
        hoverFg     = t.text.name();
        focusBorder = t.borderFocused.name();
        break;
    case Secondary:
        fg          = t.textDim.name();
        border      = t.border.name();
        hoverFg     = t.text.name();
        focusBorder = t.borderFocused.name();
        break;
    case Destructive: {
        QColor warn = t.markerPtr.isValid() ? t.markerPtr : t.indHeatHot;
        fg          = warn.name();
        border      = warn.name();
        hoverFg     = warn.name();
        focusBorder = warn.name();
        break;
    }
    }
    setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 0px;"
        "  padding: 5px 16px;"
        "  min-width: 88px;"
        "  font-weight: 500;"
        "}"
        "QPushButton:hover {"
        "  background: %4;"
        "  color: %5;"
        "}"
        "QPushButton:pressed {"
        "  background: %6;"
        "  color: %5;"
        "}"
        "QPushButton:focus {"
        "  border: 1px solid %7;"
        "  padding: 5px 16px;"
        "}"
        "QPushButton:default {"
        "  border: 1px solid %7;"
        "}"
        "QPushButton:disabled {"
        "  background: transparent;"
        "  color: %8;"
        "  border-color: %9;"
        "}")
        .arg(bg, fg, border,
             hoverFill, hoverFg, t.hover.darker(115).name(),
             focusBorder,
             t.textMuted.name(), t.border.name()));
}

} // namespace rcx
