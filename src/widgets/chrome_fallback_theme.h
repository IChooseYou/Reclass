#pragma once

// Colours for custom-painted chrome when ThemeManager has nothing loaded
// (tests, render harnesses run from a directory without themes/). The same
// VS2022 Dark values every strip falls back to, so they agree even in a bare
// target. One copy, shared by the address bar and the timeline strip.

#include "themes/theme.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace rcx {

inline Theme chromeFallbackTheme() {
    static const char* kJson = R"({
        "name": "VS2022 Dark", "background": "#181818", "backgroundAlt": "#2d2d30",
        "surface": "#333337", "border": "#3f3f46", "borderFocused": "#b180d7",
        "button": "#3f3f46", "text": "#dcdcdc", "textDim": "#858585",
        "textMuted": "#636369", "textFaint": "#585862", "hover": "#242427",
        "selected": "#2c2c31", "selection": "#264f78", "syntaxKeyword": "#569cd6",
        "syntaxNumber": "#b5cea8", "syntaxString": "#d69d85", "syntaxComment": "#57a64a",
        "syntaxPreproc": "#9b9b9b", "syntaxType": "#4ec9b0", "indHoverSpan": "#b180d7",
        "indCmdPill": "#2d2d30", "indDataChanged": "#8fbc7a", "indHintGreen": "#5a8248",
        "indRttiHint": "#D7BA7D", "markerPtr": "#f44747", "markerCycle": "#e5a00d",
        "markerError": "#7a2e2e", "focusGlow": "#E5A00D" })";
    return Theme::fromJson(QJsonDocument::fromJson(kJson).object());
}

} // namespace rcx
