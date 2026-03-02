#pragma once

#include <QWidget>

namespace rcx {

struct Theme;

// Apply macOS native title bar color to match the theme.
// No-op on non-macOS platforms (implementation is platform-specific).
void applyMacTitleBarTheme(QWidget* window, const Theme& theme);

} // namespace rcx