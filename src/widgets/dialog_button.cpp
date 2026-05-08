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
    QString fg, accent, hoverBg, pressedBg;
    switch (m_variant) {
    case Primary:
        fg        = t.text.name();
        accent    = t.indHoverSpan.name();
        hoverBg   = t.hover.name();
        pressedBg = t.hover.darker(115).name();
        break;
    case Secondary:
        fg        = t.text.name();
        accent    = QStringLiteral("transparent");
        hoverBg   = t.hover.name();
        pressedBg = t.hover.darker(115).name();
        break;
    case Destructive:
        fg     = t.text.name();
        accent = t.indHeatHot.name();
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
        break;
    }
    setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color:%1;"
        "  border: 1px solid %2; border-top: 2px solid %3;"
        "  border-radius: 2px; padding: 2px 14px; min-width: 72px; }"
        "QPushButton:hover { background:%4; }"
        "QPushButton:pressed { background:%5; }"
        "QPushButton:disabled { color:%6; border-top-color:%2; }"
        "QPushButton:focus { border-color:%7; }")
        .arg(fg, t.border.name(), accent,
             hoverBg, pressedBg, t.textMuted.name(),
             t.indHoverSpan.name()));
}

} // namespace rcx
