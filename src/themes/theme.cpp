#include "theme.h"
#include <QtGlobal>
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
    {"indRttiHint",   "RTTI Hint",      "Indicators",  &Theme::indRttiHint},
    {"markerPtr",     "Pointer",        "Markers",     &Theme::markerPtr},
    {"markerCycle",   "Cycle",          "Markers",     &Theme::markerCycle},
    {"markerError",   "Error",          "Markers",     &Theme::markerError},
    {"focusGlow",     "Focus Glow",     "Presentation", &Theme::focusGlow},
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
    // Derive heat colors as an amber gradient — never red. Red reads as
    // "error" and makes a quietly-mutating value feel alarming when
    // it's just doing its job. Amber/orange is the "warm activity"
    // hue: the eye picks up movement without firing the threat
    // response. Old palette had Hot = markerPtr (saturated red), which
    // was unreadable on the live struct view where dozens of heated
    // rows might be on screen at once.
    //   Cold (1 unique value seen recently)  → faint gold, just a wash
    //   Warm (2–4)                            → clear amber
    //   Hot  (5+)                             → saturated amber (no red)
    auto lerpRgb = [](const QColor& a, const QColor& b, double f) {
        return QColor(qBound(0, a.red()   + int((b.red()   - a.red())   * f), 255),
                      qBound(0, a.green() + int((b.green() - a.green()) * f), 255),
                      qBound(0, a.blue()  + int((b.blue()  - a.blue())  * f), 255));
    };
    QColor dim = t.textDim.isValid() ? t.textDim : QColor(133, 133, 133);
    if (!t.indHeatCold.isValid())
        t.indHeatCold = lerpRgb(dim, QColor(220, 180, 120), 0.35);
    if (!t.indHeatWarm.isValid())
        t.indHeatWarm = QColor(225, 170, 90);   // clear amber
    if (!t.indHeatHot.isValid())
        t.indHeatHot = QColor(232, 165, 92);    // saturated amber, not red

    if (!t.focusGlow.isValid())
        t.focusGlow = t.borderFocused.isValid() ? t.borderFocused : QColor("#4fc3f7");

    // RTTI hint defaults to a warm amber so it visibly contrasts the green
    // type-inference hint when both fire on the same line. Themes can override
    // via the JSON key; this only fires for themes that don't ship the field.
    if (!t.indRttiHint.isValid())
        t.indRttiHint = QColor("#d19a66");

    // Ensure hover is visually distinct from background
    if (t.hover.isValid() && t.background.isValid()) {
        int dist = qAbs(t.hover.red() - t.background.red())
                 + qAbs(t.hover.green() - t.background.green())
                 + qAbs(t.hover.blue() - t.background.blue());
        if (dist < 20)
            t.hover = t.background.lighter(130);
    }
    return t;
}

} // namespace rcx
