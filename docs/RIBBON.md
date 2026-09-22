# The ribbon (Home | Modify)

A ReClassEx-style tabbed ribbon sits between the title/menu bar and the
document tabs. **Home** is what you do to a *project* (open, save, new class,
attach a source, open a panel); **Modify** is what you do to the *selection*
(grow the class, retype fields, restructure them). Home is first and is the
first-run tab; a persisted `ribbonTab` still wins.

```
 Home   Modify                                                                                                 ⌃
 ────── ━━━━━━───────────────────────────────────────────────────────────────────────────────────────────────────
     Add   │   Insert  │ 64  32  16  8  │  Float   │Ptr  │ Str  │ Other     │            Selection
  +4  +1K  │  ⤵4  ⤵1K  │ H64 H32 H16 H8 │ D  V2 M4 │ PTR │ STR  │ B         │   ≡    ✕ Delete    000 ⟲ Big endian
  +8  +2K  │  ⤵8  ⤵2K  │ I64 I32 I16 I8 │ F  V3    │ FN* │ WSTR │ ⌸ Custom… │        ⧉ Duplicate FFF ⌸ Ptr → Class
  +64      │  ⤵64      │ U64 U32 U16 U8 │    V4    │     │      │           │ Carve  ≡ Comment   ??? [] Array
 ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
```

Flat: no panel boxes or caption bands — one 1-device-px divider per panel gap,
a dim 9 pt caption **above** each panel (or per column *group* on Type), a
hairline above and below the body. The caption heads its list rather than
trailing it: "64" / "Selection" name what follows, and the colon was pointing
the wrong way while the items sat above it (user, 2026-09-08: "`:` denotes the
list is below … i think all the texts should be moved up"). The active tab is a
2-device-row `textDim` underline on the strip's hairline. The `⌃` at the right
end of the tab row collapses the body.

## Pieces

| File | Role |
|---|---|
| `src/pixelglyphs.h` | Departure Mono (a real pixel face) + the fixed hand-drawn bitmaps, painted in whole **device** pixels and alpha-thresholded (crisp at 100/125/150/200 %). |
| `src/ribbon_icons.h` | Glyph / composite / Codicon icon builders with theme-token family colours and a contrast guard; dpr-keyed cache. |
| `src/ribbon_spec.h` | The data model **and** the tables (`defaultRibbonSpec()`, header-only): tabs → panels → items. Also `ribbonSpecForId()` — the single source of every command's label + tooltip, read by both the widget and `RibbonActions`. |
| `src/ribbon.h/.cpp` | `rcx::RibbonBar` — one custom-painted widget: tab row + collapse chevron, flat panels with dim captions, 3-row small-button columns, large buttons, label compaction, `…` overflow menu, minimise. `Qt::NoFocus`. Also `rcx::ribbonStateFromSettings()`. |
| `src/paintutil.h` | Device-pixel-exact 1-row / 1-col edge fills plus the N-row `fillBottomDeviceRowsOfRect` used for the accent underlines, `kGutter = 8` and `kUnderlineRows = 2`. |
| `src/ribbon_actions.h/.cpp` | `rcx::RibbonActions` — one `QAction` per Modify button wired to `RcxController` selection ops; enabled-state follows the active controller's signals. Labels/tooltips come from `ribbonSpecForId()`. |
| `src/titlebar.cpp` | `TitleBarWidget::setQuickActions()` — the Undo / Redo pair in the title strip (there is no Edit panel on the ribbon). |
| `src/main.cpp` `createRibbon()` | Hosts the ribbon in a chrome-less `QToolBar` (`RibbonHost`), binds Home-tab buttons to the existing menu `QAction`s, owns `applyRibbonState()`, theme hook. |
| `tools/ribbon_render.cpp` | Harness: `ribbon_render <prefix> [theme] [labels 0\|1\|2]` renders both tabs at 760/1080/1350/1920 px (1080 = the 125 % user window) plus a collapsed shot, and prints `natural=` / `overflow=[]` / item rects — the width budget check for any change to a glyph cell. |
| `tests/test_pixel_glyphs.cpp`, `tests/test_ribbon_layout.cpp`, `tests/test_ribbon_actions.cpp`, `tests/test_ribbon_mainwindow.cpp`, `tests/test_ribbon_ids.cpp` | Crispness/contrast, layout/overflow/tone/collapse, controller-op, QMainWindow-placement + settings migration + quick access, and id-parity regression tests. |

## Layout

**Home** — panel order **File · Source · Class · View** (natural ≈ 852 px at 1080). Source precedes Class because the session's true first click is choosing what memory to look at: the start page already preloads a class, and `project_new` self-attaches to a scratch buffer.

| Panel | Large | Small column |
|---|---|---|
| **File** (neverHide) | Open, Save | **Export ▾** (pops the Export menu), Close |
| **Class** (neverHide) | **New Class** (teal) | New Struct, New Enum (teal) |
| **Source** | **Source ▾** (pops the Data Source menu) | Refresh, Go to Address |
| **View** (id `panels`) | Scanner · Symbols · RTTI (one column — all three open a window onto the current target) ‖ Bookmarks · Console · Split Below (the workspace column). All **Small**: Scanner and Symbols were Large until 2026-09-06, which gave the tab's most expendable panel a third of its emphasis for two plain dock toggles. Caption is "View", not "Panels", because Split Below and Console open no panel — the caption was the defect, not the items. |

The three "creates a type" buttons carry the editor's class colour; every other
Home icon is `GF::Plain`. Size is the hierarchy, hue appears once.

**Modify** — panel order **Add · Insert · Type · Selection** (natural ≈ 910 px at 1080). Type moved left of Selection: it is the panel clicked hundreds of times per class, and with Add/Insert permanently icon-only its origin is now the same at every width. Hide order Selection → Insert → Add,
Type `neverHide`):

| Panel | Contents |
|---|---|
| **Add** / **Insert** | 4 · 8 · 64 ‖ 1024 · 2048. The strip shows the count alone (`shortLabel`); the `QAction` keeps "Add 4". |
| **Selection** (id `selected`) | **Carve** (LARGE, teal — the panel's entry point) ‖ Delete (red) · Duplicate · Comment ‖ `000` `FFF` `???` (icon-only) ‖ **Big endian** (checkable) · Ptr → Class · Array. One panel, ordered by what the button does: entry point · edit in place · fill · reshape. The separate **Structure** panel was merged in on 2026-09-06 — it split one question ("I selected some bytes, now what?") across two captions. **RTTI moved OUT** to Home ▸ Panels: it opens a browser window rather than changing the selection. |
| **Type** | **SIZE-major**: columns are widths, rows are readings — `64` `32` `16` `8` each holding Hex over Int over UInt, then `Float` (D F ‖ V2 V3 V4 ‖ M4), `Ptr`, `Str`, `Other` (Bool, Custom…). Kind-major made row 1 read "H64 I64 U64 H8" — the row's meaning broke at the fourth column — and disagreed with the keyboard, where 1-4 pick a WIDTH and S/U/F reinterpret at the existing size. Cheaper too: `64` fits its column free where `Byte` inflated one by 13 px. |

## Action ids

Every button is addressable by id (`RibbonBar::action(id)`, `RibbonActions::action(id)`):

- `add.4 add.8 add.64 add.1024 add.2048` — `RcxController::appendBytes(viewRoot, n)`
- `insert.4 … insert.2048` — `insertBytesAbove(lowest-offset selected node, n)`
- `sel.delete sel.duplicate sel.comment` — `deleteSelection` / `duplicateSelection` / `commentSelection`
- `sel.zero sel.ff sel.random` — `fillSelectionBytes(Zero|FF|Random)`
- `sel.swap` — **Big endian**, checkable: `toggleBigEndianSelection`, checked when every swappable leaf in the selection is already big-endian (`SelectionSummary::allBigEndian`)
- `home.panels.rtti` — no op of its own; `createRibbon()` forwards `triggered` to Tools ▸ RTTI Browser. Lives on **Home ▸ Panels** (it opens a window) but RibbonActions keeps the id so it keeps its predicate: enabled = live && exactly one selected pointer-sized field
- `type.hex64 … type.hex8`, `type.int64 … type.int8`, `type.uint64 … type.uint8`, `type.double type.float type.bool`, `type.vec2 type.vec3 type.vec4 type.mat4x4`, `type.pointer type.funcptr` (64/32-bit from `tree.pointerSize`), `type.utf8 type.utf16` — `retypeSelection(kind)`; `type.ptrclass` — `convertSelectionToTypedPointers`; `type.class` — **Carve** (Large); `type.array` — `makeArrayFromSelection`; `type.custom` — the inline type editor
- Menu-backed (the same `QAction` objects as the menus): `home.file.open/save/close`, `home.class.newclass/newstruct/newenum`, `home.source.refresh/goto`, `home.panels.scanner/symbols/bookmarks/console/split`
- Ribbon-only: `home.source.attach` pops the Data Source menu under the button; `home.file.code` pops a two-item scope menu
  (**This Class** / **All Classes**) and switches the ACTIVE pane to its Code view — it does not write a file, File ▸ Export still does
  that. Its labels come from `codeScopeName()`, the same strings the pane's scope combo shows, so the two cannot drift. Both items carry a `menu` ▾.
- **Quick access, no ribbon button**: `edit.undo` / `edit.redo` — owned by `RibbonActions` (they own the undo-stack predicates) and shown as two 28×32 buttons in the title strip. `tests/test_ribbon_ids.cpp` lists them as the documented exception to "every RibbonActions id has a button".

`type.*` actions carry `data() = int(NodeKind)`, `add.*`/`insert.*` the byte count.

## Metrics and visual policy

All logical px at 10 pt JetBrains Mono (`fm.height()` 17); "device" = physical px.

| | |
|---|---|
| Tab row | `fm.height()+8` = **25**; `background` (the ribbon's own header, *not* `menuBarColor` — that belongs to the title strip alone); one `border` hairline on its last device row. Tabs from `x = kGutter`, `advance + 2·16` wide, `kTabGap = 8` apart — deliberately airier than the strip gutter, they read as words rather than buttons. Text inactive `textDim` / hover `text` / active `text`; **active = 2 device rows of `textDim` on the tab's bottom edge — NEUTRAL, not the accent**. Three stacked tab rows all underlined in purple made the accent meaningless; these tabs switch which *toolbar* you see (navigation, not selection), so the word carries the state and the rule only anchors it. No fill, no box, in any state. |
| Collapse chevron | 22 × tabRowH at `width() − 6 − 22`. `chevron-up.svg` expanded / `chevron-down.svg` minimized; rest `textDim`, hover `hover` fill + `text` — painted exactly like the `…` item. Hit-tested before the tabs; tooltip "Collapse/Expand the ribbon (Ctrl+F1)". |
| Body | `background`, read TOP-DOWN: padTop 2 + caption **12** + captionGap 7 + 3 rows × `rowH = max(18, fm.height()+1)` + hairline 1 = **76** → **101** total (minimized: 26). The caption band moved from the bottom to the top on 2026-09-08; the SUM is the invariant — the doc tabs and everything under them are positioned off `tabRowH + bodyHeight`. |
| Panels | width = max(columns, caption + 2), columns centred when the caption wins. `kPanelGap 14`; **one 1-device-px `border` divider at `A.right()+8`** (= 1 + kPanelGap/2; `rect.right()` is inclusive), 2 px clear of both hairlines; none before the first panel; one before the `…` item. `‖` family separators: rows only, `kSepGap 7`; `kColGap 2`. Family separators are painted a step SOFTER than panel dividers (border blended 35 % toward the ribbon ground, floored through the caption contrast ladder): Modify draws 3 dividers and up to 8 separators, and at one ink they were peer rules that hid the panel boundaries. Caption: 9 pt, `textDim` through the ladder, centred in the 12-px rect at the TOP of the body (`captionTop = tabRowH + padTop`, items start `captionH + captionGap` below it). A panel with `groupCaption` items draws one caption per column group instead, widening the group's last column when the caption needs it. A group spans from its first column's left edge to its **last column's right edge** (an `endsCaptionGroup` column terminates the run). |
| Small item | `colW × 18`; icon cell 16 tall, **width per icon kind** (`ribbonIconCellWidth`: TypeGlyph / FillSquares = `8·max(2, len)` → 16 / 24 / 32, unlabelled Add/Insert = 32, else 16) at `(x+3, y+1)`; label from `x+3+cell+4` to `right−6`; width `3 + cell + 4 + text + 6`, icon-only `cell + 6`, `+10` for a `menu` ▾. |
| Large item | `clamp(text + 8, 48, 80)` (`+10` for a ▾) wide, 54 tall; **icon centred in rows 1–2, label on row 3** — the same line as the third small button of the neighbouring column. Every Large column of a panel takes the panel's widest Large width. Label `ElideRight`. |
| Pixel glyphs | Departure Mono at **one DEVICE size per DPR**, from a measured table (`pixelFontSizeDev`): **14** px at 100 / 125 %, **22** at 150 / 200 %. The rule is UNIFORM STEM WIDTH after the alpha threshold, not "a whole multiple of the 11 px design size" — 12 / 16 / 18 / 20 px mix 1-px and 2-px stems and look broken, while 13 / 14 / 15 / 22 do not. 14 is the step up from 11 the user asked for on 2026-09-08 (+25 % cap height, 8 → 10) and it costs the layout nothing: `pixelLabelCellWidth` already reserves the widest LOGICAL advance across every shipped DPI, and the 150 % tier (22 device ÷ 1.5) was already wider than 14 device ÷ 1.0. The fixed BITMAPS (plus, hook arrow, fill squares, ▾) keep their own scale, `s = round(1.6·dpr)` → 2 / 2 / 2 / 3. |
| Square QMenu / QAction icons | `wide = false` redraws the same label inside a **16×16 logical** cell (QMenu clamps an action icon to `PM_SmallIconSize`, and a 24- / 32-wide ribbon pixmap handed to it would only be smooth-scaled into blur). The strip's size does not fit that cell — “H64” is 27 device px of advance against a 20 device px cell at 125 % — so the square path has **its own size**, `pixelFontSizeSquareDev`: the largest clean size at which a **3-character** reference fits, capped there so a 1-character label cannot balloon out of step with its neighbours, and stepped down further only for a label longer than the reference. Shipped: **8 / 8 / 11 / 14** device px for 3 characters and under, **6 / 8 / 8 / 11** for the one 4-character label (`WSTR`), at 100 / 125 / 150 / 200 %. Before 2026-09-08 this path drew the STRIP's font in the square cell and simply clipped it (`(20 − 27) / 2 = −3`), losing both edges of every 3-character label, while the header comment claimed it was shrunk to fit. |
| Codicons | rendered **only on integer multiples of the 16-unit grid**: small `16·round(dpr)` device px centred in the cell when it fits, else the cell with AA; large `16·max(2, round(1.5·dpr))` → 32 device at 125 / 150 %, 48 at 200 %. Every referenced SVG must be a 16-unit viewBox (guarded by `test_pixel_glyphs`). `RibbonIconSpec::mirrorH` flips (redo = mirrored `discard`). |
| States | paint order: opacity → fill → icon + label → underline. **Rest: `text`** for Plain icons and labels — the ribbon is the actionable surface, and dimming it at rest put the toolbar below the document in the hierarchy. Hover: `hover` fill only (the ink does not change). Pressed: `button` fill (`selected` when `button == background`). Checked: `indHoverSpan` + 2 device rows underline. Disabled: `setOpacity(0.40)` of the *same* ink before anything (never `textDim × 0.4`, which lands under 2 : 1). `RibbonItemSpec::destructive` (Delete): the icon is `markerPtr` in every state, the label turns red only under the pointer. |
| Tones | `ribbonToneColour` is a **ladder**: the asked-for tone → `textDim` → `text`, first at ≥ 3 : 1 wins. (The old cliff jumped straight to `text`, and vs.json's `textMuted` misses the guard by 0.02 — captions rendered at full label brightness.) Purple appears only on the checked state — the active *tab* is neutral `textDim`, so the ribbon spends the accent budget once, not twice; red only on Delete. |
| Families | Hex = `text`; **Signed and Unsigned both `syntaxNumber`** (the I / U letter carries the sign, which frees `markerPtr` to mean only "destructive"); Float = `syntaxKeyword`; Text = `syntaxString`; Pointer = `syntaxType`; Bits = `syntaxNumber`; Plain = `text`/the state tone. Pixel-text rasterisers demand **4.5 : 1** (`kRibbonPixelInkContrast`); Codicons keep 3.0. |
| Compaction | stage 1 (Auto only) drops labels by `labelDrop`, higher first, equal values together: Modify **Selection 10 only** (Add / Insert are icon-only in every mode, and `glyphLabels` Type is skipped); Home View 30 · Source 10 · File 0 · Class 0. A `glyphLabels` panel is glyph-only in **every** mode, so "All labels" is an honest promise; `keepLabel` items (Custom…, and the three reshape commands) keep their word regardless — so at 760 px Modify gives up the Selection panel to the `…` menu rather than showing mute glyphs; at the 1080-px window nothing hides. Stage 2 hides by `hideOrder` (lower first, `neverHide` respected): Modify Selection 0 · Insert 10 · Add 20 (Type never); Home View 0 · Source 20 (File, Class never). Hidden panels sit behind a single middle-row 22×18 `…` item: rest `textDim`, hover `hover` fill + `text`, and **`hover` for as long as its menu is up** — the address bar's rule for a cell with a dropdown hung under it, never `pressedFill` (on tw.json that is `selected`, which boxed the item in pale blue for the life of the menu). It opens on press, so it has no mouse-down flash. |

## `RibbonItemSpec` fields

Beyond `id / label / tooltip / size / icon / columnBreakBefore / separatorBefore /
iconOnly / destructive / data`:

| Field | Meaning |
|---|---|
| `bool menu` | The button opens a menu rather than acting: an 8-px ▾ in the state tone, cell width `+10`. Small items put it at the right edge; Large items put it beside the row-3 label. |
| `QString shortLabel` | The text painted on the strip when the long label doesn't earn its width ("Add 4" → "4"). The `QAction` keeps `label`, so menus and tooltips stay unambiguous. |
| `bool keepLabel` | Survives a panel-wide label drop (and a `glyphLabels` panel). |
| `QString groupCaption` | Starts a caption group at this item's column, spanning until the next `groupCaption` **or an `endsCaptionGroup` column**. A panel with any group caption does not draw its own. The caption rect ends at the last column's right edge, not at the following gap. |
| `bool endsCaptionGroup` | Closes the open caption group *before* this column without opening one, so a trailing family-less column (`Custom…`) is not swallowed by the last group. Without it `Str` was painted over `Custom…`. |

## Adding a button

1. Add a `RibbonItemSpec` to the right panel in `defaultRibbonSpec()`
   (`src/ribbon_spec.h`): id, label, tooltip, size, icon, column breaks. **This
   is the only place the words live** — `RibbonActions::add(id, data)` reads
   them back.
2. If it is a Modify-tab op, create the `QAction` in
   `RibbonActions::buildActions()` and add its enabled predicate to
   `refreshEnabled()`; the controller op should be a public `RcxController`
   method (macro + `m_suppressRefresh` idiom, ids re-resolved inside loops).
3. If it reuses a menu action, capture the action in `createMenus()` and
   `bind()` it in `createRibbon()`.
4. Run `test_ribbon_layout`, `test_pixel_glyphs`, `test_ribbon_ids`,
   `test_ribbon_actions`, `test_ribbon_mainwindow`, and `ribbon_render` on the
   hidden desktop (`tools/run_tests_hidden.py`) — then look at the PNGs at 125 %.

## Settings (QSettings "RC")

| Key | Meaning |
|---|---|
| `ribbonState` | **0 Full · 1 Collapsed · 2 Hidden** — the ONE key behind View ▸ Ribbon, the tab-row chevron, the tab double-click and Ctrl+F1. `rcx::ribbonStateFromSettings()` migrates the legacy `showRibbon` + `ribbonMinimized` pair on the first read and deletes them **in that same branch only** (a later read never writes), so no second persisted state survives. `MainWindow::applyRibbonState()` is the only writer. |
| `ribbonLabels` | 0 Auto (default) · 1 All labels · 2 Icons only — View ▸ Ribbon ▸ … |
| `ribbonTab` | `home` default. |

**View ▸ Ribbon** is one submenu: radio *Full / Collapsed / Hidden*, a
separator, *Toggle Ribbon* (**Ctrl+F1**), a separator, and the three Labels
radios. Right-clicking the strip pops the same submenu.

Ctrl+F1 never puts the ribbon *into* Hidden — that state has no on-screen
chevron, so entering it stays a deliberate menu choice. Anything that is not
Full maps back to Full, which also makes Ctrl+F1 the keyboard way out of
Hidden. **Collapsed** is "tabs only"; clicking a tab expands the ribbon and
persists Full (there is no one-shot temporary reveal — the tab double-click is
the secondary collapse gesture).

## Notes

- The ribbon never takes focus and never registers shortcuts; the editor owns the plain keys (Delete, Insert, 1–5, P/F/S/U). Shortcuts show in tooltips only, in one format: `<what it does> — Ctrl+D`, and a spec tooltip that already names its shortcut (`Open a project (Ctrl+O)`) does not get it twice.
- **Tooltip source.** `tooltipFor()` prefers a bound external `QAction`'s tooltip *only when it is a real one*. A menu action that never set one hands back Qt's echo of its menu text (`&Close Project` → `Close Project`), which is a different name from the one painted on the strip; that case falls through to `ribbonSpecForId()`, so ribbon = menu = tooltip stays true on the Home tab too.
- Family colours come from theme tokens, contrast-guarded per theme; Codicons are tinted with a SourceIn fill so baked-colour icons follow the theme too. Plain icons (Codicon, FillSquares, Add/Insert) take the item's state tone through `RibbonIconOptions::plainInk`.
- `DockOverlay::contentRect()` skips the ribbon host so drag-drop zones never cover it. Ctrl+Click on the ribbon reports region `ribbon` with the button id.
- `RCX_RIBBON_DEBUG=1 RC.exe --screenshot out.png [home|modify|collapsed]` prints the live ribbon geometry (window/host/ribbon rects, natural width, overflowed panels, font, dpr) to stderr and forces the captured tab / collapsed state (the user's `ribbonTab` / `ribbonState` are restored before exit). Note `--screenshot` PNGs are device pixels: a 1350-px capture at 125 % is a 1080-logical-px window.
- Left out for now: Bits/bitfield, Show/Hide (no hidden nodes), VTable/PCHAR/PWCHAR kinds, 128-bit / half-float types (use Custom…).
