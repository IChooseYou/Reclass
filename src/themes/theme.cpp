#include "theme.h"
#include <type_traits>

namespace rcx {

// ── Shared field metadata (serialization + editor UI) ──

const ThemeFieldMeta kThemeFields[] = {
    {"background",    "Background",     "Chrome",      &Theme::background},
    {"backgroundAlt", "Background Alt", "Chrome",      &Theme::backgroundAlt},
    {"surface",       "Surface",        "Chrome",      &Theme::surface},
    {"border",        "Border",         "Chrome",      &Theme::border},
    {"borderFocused", "Border Focused", "Chrome",      &Theme::borderFocused},
    {"button",        "Button",         "Chrome",      &Theme::button},
    {"text",          "Text",           "Text",        &Theme::text},
    {"textDim",       "Text Dim",       "Text",        &Theme::textDim},
    {"textMuted",     "Text Muted",     "Text",        &Theme::textMuted},
    {"textFaint",     "Text Faint",     "Text",        &Theme::textFaint},
    {"hover",         "Hover",          "Interactive",  &Theme::hover},
    {"selected",      "Selected",       "Interactive",  &Theme::selected},
    {"selection",     "Selection",      "Interactive",  &Theme::selection},
    {"syntaxKeyword", "Keyword",        "Syntax",      &Theme::syntaxKeyword},
    {"syntaxNumber",  "Number",         "Syntax",      &Theme::syntaxNumber},
    {"syntaxString",  "String",         "Syntax",      &Theme::syntaxString},
    {"syntaxComment", "Comment",        "Syntax",      &Theme::syntaxComment},
    {"syntaxPreproc", "Preprocessor",   "Syntax",      &Theme::syntaxPreproc},
    {"syntaxType",    "Type",           "Syntax",      &Theme::syntaxType},
    {"indHoverSpan",  "Hover Span",     "Indicators",  &Theme::indHoverSpan},
    {"indCmdPill",    "Cmd Pill",       "Indicators",  &Theme::indCmdPill},
    {"indDataChanged","Data Changed",   "Indicators",  &Theme::indDataChanged},
    {"indHeatCold",   "Heat Cold",      "Indicators",  &Theme::indHeatCold},
    {"indHeatWarm",   "Heat Warm",      "Indicators",  &Theme::indHeatWarm},
    {"indHeatHot",    "Heat Hot",       "Indicators",  &Theme::indHeatHot},
    {"indHintGreen",  "Hint Green",     "Indicators",  &Theme::indHintGreen},
    {"markerPtr",     "Pointer",        "Markers",     &Theme::markerPtr},
    {"markerCycle",   "Cycle",          "Markers",     &Theme::markerCycle},
    {"markerError",   "Error",          "Markers",     &Theme::markerError},
};
const int kThemeFieldCount = static_cast<int>(std::extent_v<decltype(kThemeFields)>);

QJsonObject Theme::toJson() const {
    QJsonObject o;
    o["name"] = name;
    for (int i = 0; i < kThemeFieldCount; i++)
        o[kThemeFields[i].key] = (this->*kThemeFields[i].ptr).name();
    return o;
}

Theme Theme::fromJson(const QJsonObject& o) {
    Theme t;
    t.name = o["name"].toString("Untitled");
    for (int i = 0; i < kThemeFieldCount; i++) {
        if (o.contains(kThemeFields[i].key))
            t.*kThemeFields[i].ptr = QColor(o[kThemeFields[i].key].toString());
    }
    // Derive heat colors from the theme's own palette when keys are absent
    // cold = keyword blue, warm = hover/string amber, hot = marker red
    if (!t.indHeatCold.isValid())
        t.indHeatCold = t.syntaxKeyword;
    if (!t.indHeatWarm.isValid())
        t.indHeatWarm = t.indHoverSpan.isValid() ? t.indHoverSpan : t.syntaxString;
    if (!t.indHeatHot.isValid())
        t.indHeatHot = t.markerPtr;
    return t;
}

} // namespace rcx
