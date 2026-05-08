#include "themed_dialog.h"

namespace rcx {

ThemedDialog::ThemedDialog(QWidget* parent) : QDialog(parent) {
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this](const Theme&){ applyTheme(); });
}

void ThemedDialog::applyTheme() {
    // Mirror main.cpp's applyGlobalTheme so dialogs inherit the same
    // palette as the main window. If the global palette already
    // matches (the usual case), this is a no-op cost; if a parent
    // overrode part of the palette, we restore the canonical mapping.
    const auto& t = ThemeManager::instance().current();
    QPalette pal = palette();
    pal.setColor(QPalette::Window,          t.background);
    pal.setColor(QPalette::WindowText,      t.text);
    pal.setColor(QPalette::Base,            t.background);
    pal.setColor(QPalette::AlternateBase,   t.surface);
    pal.setColor(QPalette::Text,            t.text);
    pal.setColor(QPalette::Button,          t.button);
    pal.setColor(QPalette::ButtonText,      t.text);
    pal.setColor(QPalette::Highlight,       t.selected);
    pal.setColor(QPalette::HighlightedText, t.text);
    pal.setColor(QPalette::ToolTipBase,     t.backgroundAlt);
    pal.setColor(QPalette::ToolTipText,     t.text);
    pal.setColor(QPalette::Mid,             t.hover);
    pal.setColor(QPalette::Dark,            t.border);
    pal.setColor(QPalette::Light,           t.textFaint);
    pal.setColor(QPalette::Link,            t.indHoverSpan);

    pal.setColor(QPalette::Disabled, QPalette::WindowText,      t.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::Text,            t.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText,      t.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, t.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::Light,           t.background);
    setPalette(pal);
    setAutoFillBackground(true);
}

QHBoxLayout* ThemedDialog::makeButtonRow(
        std::initializer_list<QPushButton*> buttons) {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 8, 0, 0);
    row->setSpacing(8);
    row->addStretch(1);
    for (auto* b : buttons) row->addWidget(b);
    return row;
}

} // namespace rcx
