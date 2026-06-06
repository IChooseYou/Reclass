// Offscreen render harness for the custom window title bar. Builds the bar,
// themes it (re-tinting the chrome SVG icons), optionally enables the app-icon
// mode (which bumps the bar to 34px), and grabs it to a PNG — works under
// `-platform offscreen`, so the chrome-button icon vertical centering can be
// eyeballed deterministically. The qrc is linked (see CMake) so :/vsicons load.
//
// Usage: titlebar_render <out.png> [icon]
#include <QApplication>
#include "titlebar.h"
#include "themes/thememanager.h"

using namespace rcx;

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    TitleBarWidget bar;
    bar.applyTheme(ThemeManager::instance().current());
    if (argc > 2 && QString::fromLocal8Bit(argv[2]) == QStringLiteral("icon"))
        bar.setShowIcon(true);

    bar.resize(760, bar.sizeHint().height());
    bar.show();
    app.processEvents();
    app.processEvents();

    const QString out = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                    : QStringLiteral("titlebar_render.png");
    bar.grab().save(out);
    return 0;
}
