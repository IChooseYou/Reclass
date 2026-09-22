#pragma once
#include <QColor>
#include <QString>
#include <QJsonObject>

namespace rcx {

struct Theme {
    QString name;
    // Optional editor font override. When set (non-empty), switching
    // to this theme forces the editor font to this family — useful for
    // themes designed around a specific typeface (e.g. the XP Luna
    // theme reads best in IBM Plex Mono). Empty = follow the user's
    // saved font preference.
    QString font;

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
    QColor indRttiHint;     // RTTI vtable name hint (distinct from indHintGreen)

    // ── Markers ──
    QColor markerPtr;       // null pointer
    QColor markerCycle;     // cycle detection
    QColor markerError;     // error row bg

    // ── Presentation ──
    QColor focusGlow;       // MCP focus pulse (warm amber)

    QJsonObject toJson() const;
    static Theme fromJson(const QJsonObject& obj);
};

// The editor "paper" colour: intentionally a touch darker than the chrome
// `background` on dark themes (pure white on light themes), giving the editor
// body visual depth. Panels that should read as the SAME surface as the editor
// (e.g. the Project workspace tree) must use this — not raw `background` — or
// they look jarringly lighter. Single source of truth for both editor.cpp and
// the workspace styling in main.cpp.
inline QColor editorPaperColor(const Theme& t) {
    return (t.background.lightnessF() > 0.78)
        ? QColor(QStringLiteral("#FFFFFF"))
        : t.background.darker(115);
}

// The main-menu / title-bar strip: ~25% darker than the editor paper
// (user-tuned: 10%, then another 15%) so the chrome reads as a clearly
// distinct band instead of bleeding into the document.
inline QColor menuBarColor(const Theme& t) {
    return editorPaperColor(t).darker(127);
}

// Seam colour for every 1-px line that touches the document: the editor /
// Code container outline, the doc-tab and address-bar bottoms, the pane-tab
// and status-bar tops, dock-header bottoms. At full theme.border those lines
// compete with the strip hairlines and the panes read as a stack of parallel
// lines; blended 35% toward the editor paper they read as seams between
// surfaces. DERIVED, not a new token: it tracks the theme automatically.
// Strong theme.border is reserved for the seams BETWEEN chrome strips (title
// bottom, ribbon tab-row bottom, ribbon body bottom).
inline QColor containerBorderColor(const Theme& t) {
    constexpr qreal k = 0.35;  // 0 = full border, 1 = gone
    const QColor a = t.border;
    const QColor b = editorPaperColor(t);
    return QColor::fromRgbF(a.redF()   + (b.redF()   - a.redF())   * k,
                            a.greenF() + (b.greenF() - a.greenF()) * k,
                            a.blueF()  + (b.blueF()  - a.blueF())  * k,
                            a.alphaF());
}

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
