#pragma once
#include <QColor>
#include <QString>
#include <QJsonObject>

namespace rcx {

struct Theme {
    QString name;

    // ── Chrome ──
    QColor background;      // editor bg, margin bg, window
    QColor backgroundAlt;   // panels, tab selected, tooltips
    QColor surface;         // alternateBase
    QColor border;          // separators, menu borders
    QColor borderFocused;   // window border when focused
    QColor button;          // button bg

    // ── Text ──
    QColor text;            // primary text, caret, identifiers
    QColor textDim;         // margin fg, status bar
    QColor textMuted;       // inactive tab, disabled menu
    QColor textFaint;       // margin dim, hex dim

    // ── Interactive ──
    QColor hover;           // row hover, tab hover, menu hover
    QColor selected;        // row selection highlight
    QColor selection;       // text selection background

    // ── Syntax ──
    QColor syntaxKeyword;
    QColor syntaxNumber;
    QColor syntaxString;
    QColor syntaxComment;
    QColor syntaxPreproc;
    QColor syntaxType;      // custom types / GlobalClass

    // ── Indicators ──
    QColor indHoverSpan;    // hover link text
    QColor indCmdPill;      // command row pill bg
    QColor indDataChanged;  // changed data values (legacy, fallback for old themes)
    QColor indHeatCold;     // heatmap level 1 (changed once)
    QColor indHeatWarm;     // heatmap level 2 (moderate changes)
    QColor indHeatHot;      // heatmap level 3 (frequent changes)
    QColor indHintGreen;    // comment/hint text

    // ── Markers ──
    QColor markerPtr;       // null pointer
    QColor markerCycle;     // cycle detection
    QColor markerError;     // error row bg

    QJsonObject toJson() const;
    static Theme fromJson(const QJsonObject& obj);
};

// ── Shared field metadata (serialization + editor UI) ──

struct ThemeFieldMeta {
    const char*    key;     // JSON key
    const char*    label;   // display label
    const char*    group;   // section group name
    QColor Theme::*ptr;
};

extern const ThemeFieldMeta kThemeFields[];
extern const int kThemeFieldCount;

} // namespace rcx
