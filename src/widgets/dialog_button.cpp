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
    // Match ScanButton's fixed 28 px height so dialog buttons line up
    // pixel-for-pixel with the scanner toolbar — a future unification
    // is then mechanical.
    setFixedHeight(28);
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
    // No top-emphasis stripe — variants are distinguished by hover
    // tint and focus border only. The earlier 2 px accent line read
    // as visual noise on small dialogs ("very gross") and offered no
    // affordance the focus border doesn't already give.
    QString fg, hoverBg, pressedBg, focusBorder;
    switch (m_variant) {
    case Primary:
        fg          = t.text.name();
        hoverBg     = t.hover.name();
        pressedBg   = t.hover.darker(115).name();
        focusBorder = t.indHoverSpan.name();
        break;
    case Secondary:
        fg          = t.text.name();
        hoverBg     = t.hover.name();
        pressedBg   = t.hover.darker(115).name();
        focusBorder = t.border.name();
        break;
    case Destructive:
        fg = t.text.name();
        // Subtle red wash on hover instead of full bright red — keeps
        // the button readable while signalling "this can't be undone."
        hoverBg = QStringLiteral("rgba(%1, %2, %3, 90)")
                      .arg(t.indHeatHot.red())
                      .arg(t.indHeatHot.green())
                      .arg(t.indHeatHot.blue());
        pressedBg = QStringLiteral("rgba(%1, %2, %3, 140)")
                        .arg(t.indHeatHot.red())
                        .arg(t.indHeatHot.green())
                        .arg(t.indHeatHot.blue());
        focusBorder = t.indHeatHot.name();
        break;
    }
    setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color:%1;"
        "  border: 1px solid %2;"
        "  border-radius: 2px; padding: 2px 14px; min-width: 72px; }"
        "QPushButton:hover { background:%3; }"
        "QPushButton:pressed { background:%4; }"
        "QPushButton:disabled { color:%5; }"
        "QPushButton:focus { border-color:%6; }")
        .arg(fg, t.border.name(),
             hoverBg, pressedBg, t.textMuted.name(),
             focusBorder));
}

} // namespace rcx
