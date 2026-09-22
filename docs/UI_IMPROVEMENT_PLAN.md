# Reclass 2027 — UI Improvement Plan
## Based on Refactoring UI principles + Exhaustive Color Audit

---

## ⚠ VERIFICATION (2026-06-05) — most color bugs already fixed; plan is largely stale

Re-checked every color collision below against the CURRENT theme JSONs
(`src/themes/defaults/*.json`). A prior theme pass already resolved most of Part 2/Part 4-P0. Confirmed
current state (visual changes are NOT auto-applied — they're yours to approve):

**reclass_dark.json — only 2 literal dups remain:**
- ✅ STILL REAL: `selected` `#2a2d2e` **==** `surface` `#2a2d2e` (distance 0) — a selected row is the same
  color as an alternating/surface row, so selection is hard to see. *Fix is a design choice* (the Part-5
  proposal suggests a blue `selected` `#1d3b5e`; VS Code uses a blue selection).
- ✅ STILL REAL: `indCmdPill` `#2a2a2a` **==** `hover` `#2a2a2a` (distance 0) — command pill bg == hover.
  (Part-5 proposes `indCmdPill: backgroundAlt #252526`.)
- ❌ STALE/FIXED: `selection` is now `#264f78` (a distinct blue), no longer ≈ `hover`.
- ❌ N/A: `indHeatCold`/`indHeatWarm`/`indHeatHot` fields no longer exist in the JSON (the heat colors
  the audit referenced are gone/derived elsewhere) — those rows are moot.

**mid.json — FIXED:** `hover` `#181E2A`, `backgroundAlt` `#121720`, `selected` `#1A2D4A` are all distinct
now; the "triple collision" no longer exists.

**warm.json — FIXED:** the WCAG contrast failures are gone — `syntaxString` `#6B3B21`→`#C0825A`,
`syntaxComment` `#464646`→`#8A8878`, `indHintGreen`→`#688A58`, `markerPtr`→`#B85A42`. (Minor new
observation, not in the original audit: warm `surface`==`backgroundAlt`==`indCmdPill` are all `#2a2a2a`.)

**Net:** Part 4 "P0 — Critical (Color Bugs)" is ~80% already done. The only remaining objective dark-theme
dups are the two above; everything else in this doc is subjective hierarchy/typography/empty-state polish
awaiting your direction.

---

## PART 1: SCREENSHOT ANALYSIS (withproject.png)

### What I See
- Dark theme memory structure editor with a 3-panel layout
- Left: Project tree sidebar ("Project" dock) with classes/structs/enums
- Center: "RxcEditor" tab showing hex memory view (offsets +0 to +78, all zeroed)
- Right: "Unnamed" tab (empty/newly created)
- Menu bar: Reclass | File | Edit | View | Tools | Plugins | Help
- Status bar: "Reclass" / "C/C++" view toggle tabs

### Visual Hierarchy Issues (Refactoring UI Ch. 2: "Hierarchy is Everything")

1. **Everything competes equally for attention.** The hex data, the offsets, the type labels ("hex64"), the dot preview ("........"), and the tab headers all sit at roughly the same visual weight. There's no clear focal point. Per the book: "When everything in an interface is competing for attention, it feels noisy and chaotic."

2. **The sidebar and content area have no visual separation.** Both the project tree and the editor use nearly identical background colors (#1e1e1e vs #252526 — only 7 units apart). The book says: "Emphasize by de-emphasizing" — the sidebar should recede so the main content takes focus.

3. **Tab system lacks active/inactive hierarchy.** The "RxcEditor" and "Unnamed" tabs at the top have very similar treatments. The active tab doesn't stand out enough. Per the book: "Primary actions should be obvious."

4. **Status bar view toggle buttons blend into the chrome.** "Reclass" and "C/C++" at the bottom are functional tabs but look like passive labels.

5. **The empty state (Unnamed tab) is a missed opportunity.** Per the book: "Don't overlook empty states." The blank dark area could include a subtle prompt like "Start by defining a struct" or a quick-action button.

---

## PART 2: EXHAUSTIVE COLOR CONSISTENCY AUDIT

### A. Hardcoded Colors That Bypass the Theme System

These colors are NOT driven by the theme JSON and will look wrong when users switch themes:

| File | Line | Hardcoded Value | Used For | Should Be |
|------|------|----------------|----------|-----------|
| `themeeditor.cpp` | 18 | `#888` | Section label text | `theme.textDim` |
| `themeeditor.cpp` | 19 | `#444` | Section border | `theme.border` |
| `themeeditor.cpp` | 67 | `#666` | File info label text | `theme.textMuted` |
| `themeeditor.cpp` | 112 | `#aaa` | Hex color label text | `theme.textDim` |
| `themeeditor.cpp` | 179 | `#555` | Swatch button border | `theme.border` |
| `editor.cpp` | ~869 | `#ffffff` | Error marker foreground | New theme field or derive from `theme.text` |
| `main.cpp` | 858 | `RGB(255,200,80)` | Shimmer fallback bright | Already falls back to `indHoverSpan` but literal remains |
| `titlebar.cpp` | 101 | `#c42b1c` | Close button hover (red) | New theme field `chromeCloseHover` or keep as semantic constant |

### B. Near-Identical / Ambiguous Color Values in `reclass_dark.json`

Colors that are so close they fail to create visual distinction:

| Color A | Value | Color B | Value | RGB Distance | Problem |
|---------|-------|---------|-------|-------------|---------|
| `hover` | `#2a2a2a` | `background` | `#1e1e1e` | 36 | Just barely above the 20-unit auto-fix threshold; hover is nearly invisible |
| `selected` | `#2a2d2e` | `surface` | `#2a2d2e` | **0** | Identical! Selection and alternateBase are the same color |
| `selection` | `#2b2b2b` | `hover` | `#2a2a2a` | 3 | Text selection is indistinguishable from row hover |
| `indCmdPill` | `#2a2a2a` | `hover` | `#2a2a2a` | **0** | Identical — cmd pill background = hover |
| `indHeatCold` | `#D4A945` | `indHoverSpan` | `#E6B450` | 29 | Both yellowish-gold, heat indicator looks same as hover links |
| `indHeatWarm` | `#E6B450` | `indHoverSpan` | `#E6B450` | **0** | Identical — warm heat = hover span |

### C. Cross-Theme Consistency Issues

**Warm theme — dangerously low contrast:**
| Color | Hex | Background | Contrast Issue |
|-------|-----|-----------|----------------|
| `syntaxString` | `#6B3B21` | `#212121` | Dark brown on dark gray — ~2.1:1 ratio, fails WCAG AA |
| `syntaxComment` | `#464646` | `#212121` | ~2.0:1 ratio, nearly invisible |
| `indHintGreen` | `#464646` | `#212121` | Same as syntaxComment — hint text invisible |
| `markerPtr` | `#6B3B21` | `#212121` | Pointer markers use syntaxString — barely visible |

**Mid theme — selected/hover confusion:**
| Color | Value | Problem |
|-------|-------|---------|
| `hover` | `#121720` | Same as `backgroundAlt` (#121720) — hover indistinguishable from panel bg |
| `selected` | `#121720` | Same as `hover` AND `backgroundAlt` — three roles, one color |

### D. Missing Theme Fields for Full Coverage

Currently hardcoded behaviors that should be themeable:
1. **Error text foreground** — hardcoded `#ffffff`, should be a theme field
2. **Close button hover** — hardcoded `#c42b1c`, standard Windows red but should be overridable
3. **Scrollbar colors** — not in theme, inherits from Qt Fusion defaults
4. **Tooltip border radius** — structural, but color is themed
5. **Find bar placeholder text** — inherits from Qt, not explicitly themed

---

## PART 3: IMPROVEMENTS BY REFACTORING UI PRINCIPLE

### 1. Hierarchy & Spacing (Ch. 2 + Ch. 3)

**Problem:** The hex editor view treats offsets, types, dot-preview, and hex bytes with equal visual weight.

**Recommendations:**
- **De-emphasize offsets (`+0`, `+8`, etc.)** — Use `textFaint` instead of `textDim`. These are reference guides, not primary data.
- **De-emphasize type labels (`hex64`)** — Use `textDim`. The data type is supporting info.
- **De-emphasize dot preview (`........`)** — Already using textFaint which is correct.
- **Emphasize hex data bytes** — The actual `00 00 00 00...` should be the brightest (`text`). When values change, the heat colors will pop against the dimmer surroundings.
- **Add more spacing between offset groups** — Currently every row is evenly spaced. Consider grouping by 16-byte boundaries with a slightly larger gap, per the book: "Avoid ambiguous spacing."

### 2. Color System (Ch. 5: "Working with Color")

**"You need more colors than you think" + "Define your shades up front":**

The current theme has 31 fields which is good, but several values collapse into each other. Recommendations:

a) **Fix the `selected` / `surface` / `hover` / `selection` collision:**
```json
{
  "hover":     "#262626",   // was #2a2a2a — move CLOSER to bg, subtle
  "selected":  "#264f78",   // use a blue tint for selection (like VS Code)
  "selection": "#264f78",   // text selection should match row selection
  "surface":   "#252526"    // keep as-is for alternating rows
}
```
Using a **tinted selection color** (blue) instead of a slightly-different gray follows the book's advice: "Don't rely on color alone" — but in this case, adding color to selection HELPS distinguish it from hover, which is the point.

b) **Increase hover contrast:**
Bump the auto-fix threshold from 20 to 30 RGB distance, or manually set hover to a visually distinct value. A 36-unit distance is not enough — aim for at least 50.

c) **Differentiate heat indicator colors:**
```
indHeatCold: use a desaturated blue-green (#5BA3A3) — "cold" should LOOK cold
indHeatWarm: keep amber (#E6B450)
indHeatHot:  keep red (#f44747)
```
This creates a temperature-based color progression that's semantically clear AND visually distinct from hover span links.

d) **Fix Warm theme contrast:**
Every syntax color must achieve at least 4.5:1 contrast ratio against the background (WCAG AA). Current `syntaxString` (#6B3B21) against `#212121` is ~2.1:1.
```
syntaxString:  #B87040  (was #6B3B21)
syntaxComment: #686858  (was #464646)
indHintGreen:  #5A7A50  (was #464646)
```

### 3. Typography & Text Hierarchy (Ch. 4: "Designing Text")

**Recommendations:**
- **Tab titles** — The MDI tabs ("RxcEditor", "Unnamed") use the same font size as code. Consider making them slightly smaller or using a different weight to differentiate chrome from content.
- **Status bar text** — Currently uses `textDim`. Good. The view toggle labels "Reclass" / "C/C++" could benefit from being slightly bolder when active.
- **Sidebar "Project" header** — Consider de-emphasizing it per the book: "Section titles act more like labels than headings."

### 4. Borders & Separation (Ch. 7: "Use fewer borders")

**Current state:** The app uses 1px borders between:
- Title bar and content (bottom border on titlebar)
- Sidebar and content (dock separator = 1px)
- Content and status bar (top hairline on status bar)
- Tab dividers

**Recommendations:**
- The sidebar could use a **background color difference** instead of a border. Set the project dock background to `backgroundAlt` to create natural separation without a line.
- The **status bar top border** is good — it's a necessary separator. Keep it.
- Consider **removing the border between MDI tabs** and using background color alone to distinguish active/inactive.

### 5. Interactive States (Ch. 6: "Semantics are secondary")

**Close button hover:**
Currently `#c42b1c` hardcoded. This is standard Windows behavior and feels correct. However, it should be themeable for non-Windows platforms or custom themes.

**Menu hover:**
Uses `QPalette::Mid` = `theme.hover`. The selected menu item text switches to `theme.indHoverSpan` (gold). This is good — it creates a clear active state. However, the hover background is very subtle.

**Tree item hover/selection:**
Both use `theme.hover`. Selection and hover should be visually different. Consider using the tinted selection color for tree selection too.

### 6. Depth & Shadows (Ch. 6: "Creating Depth")

The app is intentionally flat (appropriate for a developer tool). However:
- **Dropdown menus** could benefit from a subtle box-shadow to feel elevated above the editor content. Currently, the OS drop shadow handles this (Fusion PE_FrameMenu is killed), which is fine.
- **Tooltip popups** (ValueHistoryPopup, DisasmPopup) use `backgroundAlt` with a `border` stroke. Consider a very subtle shadow or slightly more border contrast for popups that overlay content.

### 7. Empty States (Ch. 7: "Don't overlook empty states")

The "Unnamed" tab shows an empty dark area with:
```
[>] 'Reclass.exe'  0x400000  class Unnamed {
  +0    hex64    ........  00 00 00 00 00 00 00 00
  ...
};
```

When a project is newly created, consider:
- A subtle helper text: "Right-click to add fields, or attach to a process" in `textMuted`
- Or even just an icon + brief instruction in the center when the struct is empty

### 8. Finishing Touches (Ch. 7)

**Accent borders:**
The status bar view tabs use `indHoverSpan` (gold) as an accent line on the active tab. This is excellent — it follows the book's advice to "add color with accent borders."

**Consider extending this pattern to:**
- The active MDI tab (top accent line in the tab strip)
- The focused dock panel (sidebar) border
- The find bar when focused (border-bottom accent)

---

## PART 4: PRIORITIZED ACTION ITEMS

### P0 — Critical (Color Bugs)
1. Fix `selected` = `surface` collision in `reclass_dark.json` (selection is invisible)
2. Fix `selection` ≈ `hover` collision (text selection indistinguishable from hover)
3. Fix Warm theme contrast failures (`syntaxString`, `syntaxComment`, `indHintGreen`)
4. Fix Mid theme `hover` = `backgroundAlt` = `selected` triple collision

### P1 — Important (Consistency)
5. Replace all hardcoded colors in `themeeditor.cpp` with theme values
6. Make error marker foreground (`#ffffff`) a theme-derived value
7. Differentiate `indHeatCold` / `indHeatWarm` / `indHoverSpan` visually
8. Increase hover-vs-background contrast threshold from 20 to 40

### P2 — Enhancement (Visual Hierarchy)
9. De-emphasize offsets and type labels in hex editor view
10. Add tinted (blue) selection color instead of gray
11. Add accent line to active MDI tab
12. Improve empty state for new projects

### P3 — Polish (Finishing Touches)
13. Add close button hover color to theme system
14. Sidebar background differentiation (use backgroundAlt)
15. Subtle depth for tooltips/popups (box-shadow or stronger border)
16. Font weight/size hierarchy for sidebar vs content

---

## PART 5: PROPOSED THEME COLOR CHANGES (reclass_dark.json)

```json
{
  "name": "Reclass Dark",
  "background":    "#1e1e1e",
  "backgroundAlt": "#252526",
  "surface":       "#2a2d2e",
  "border":        "#3c3c3c",
  "borderFocused": "#888888",
  "button":        "#333333",

  "text":          "#d4d4d4",
  "textDim":       "#858585",
  "textMuted":     "#585858",
  "textFaint":     "#404040",

  "hover":         "#262628",
  "selected":      "#1d3b5e",
  "selection":     "#264f78",

  "syntaxKeyword": "#569cd6",
  "syntaxNumber":  "#b5cea8",
  "syntaxString":  "#ce9178",
  "syntaxComment": "#6a9955",
  "syntaxPreproc": "#c586c0",
  "syntaxType":    "#4EC9B0",

  "indHoverSpan":  "#E6B450",
  "indCmdPill":    "#252526",
  "indDataChanged":"#8fbc7a",
  "indHeatCold":   "#5BA3A3",
  "indHeatWarm":   "#E6B450",
  "indHeatHot":    "#f44747",
  "indHintGreen":  "#5a8248",

  "markerPtr":     "#f44747",
  "markerCycle":   "#e5a00d",
  "markerError":   "#7a2e2e"
}
```

**Key changes from current:**
- `textFaint`: `#505050` → `#404040` (push further from textMuted for clearer hierarchy)
- `hover`: `#2a2a2a` → `#262628` (slight blue tint, still subtle but distinguishable)
- `selected`: `#2a2d2e` → `#1d3b5e` (blue tinted, no longer identical to surface)
- `selection`: `#2b2b2b` → `#264f78` (VS Code-style blue selection, matches VS2022 theme)
- `indCmdPill`: `#2a2a2a` → `#252526` (use backgroundAlt instead of duplicating hover)
- `indHeatCold`: `#D4A945` → `#5BA3A3` (cold = teal/cool color, semantically correct)

---

*Generated from exhaustive codebase audit covering all .cpp, .h, .json, and .qrc files in the Reclass 2027 source tree.*
