#include "macos_titlebar.h"
#include "themes/theme.h"

#import <Cocoa/Cocoa.h>
#include <QColor>
#include <QWidget>

namespace rcx {

static NSColor* toNSColor(const QColor& color) {
    return [NSColor colorWithCalibratedRed:color.redF()
                                     green:color.greenF()
                                      blue:color.blueF()
                                     alpha:color.alphaF()];
}

void applyMacTitleBarTheme(QWidget* window, const Theme& theme) {
    if (!window) return;

    // Ensure native window is created.
    window->winId();

    auto* nsView = reinterpret_cast<NSView*>(window->winId());
    if (!nsView) return;

    NSWindow* nsWindow = [nsView window];
    if (!nsWindow) return;

    // Keep native traffic lights while tinting the title bar to the theme.
    // Match the title text contrast by selecting the appropriate system appearance.
    const qreal luminance =
        0.2126 * theme.background.redF() +
        0.7152 * theme.background.greenF() +
        0.0722 * theme.background.blueF();
    const bool isLight = luminance >= 0.5;
    [nsWindow setAppearance:[NSAppearance appearanceNamed:
        (isLight ? NSAppearanceNameAqua : NSAppearanceNameDarkAqua)]];
    [nsWindow setTitlebarAppearsTransparent:YES];
    [nsWindow setTitleVisibility:NSWindowTitleVisible];
    [nsWindow setBackgroundColor:toNSColor(theme.background)];
}

} // namespace rcx
