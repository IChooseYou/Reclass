# The address bar

The 26-px strip between the document tabs and the command row (Scintilla
line 0). It is Explorer's bar for a class view: it answers **where am I
reading from** (the source chip), **where in it** (the base address and the
drill trail) and **where can I go** (Back / Forward / history / Up, a chevron
per level, recent places), and lets you act on each. One per split pane; the
controller pushes one `AddressBarState` per refresh and every pane shows the
same trail (only the pane a gesture came from scrolls).

```
 ← → ˅ ↑ │ ▣● RC.exe ˅ <RC.exe>+0x1234 = 0x7FF6DEAD1234  « QWidget.d_ptr › QObjectPrivate.parent › QObjectData ›            ˅
 ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
  back fwd hist up   src        src.chev base                          overflow crumb:2  chev:2 crumb:3  chev:3  crumb:4  chev:4  space recent
```

Paper, not chrome: the band is `editorPaperColor` (it sits inside the
document column), the seam under it one device row of `containerBorderColor`,
hover `t.hover`, a mouse-down flash `pressedFill(t)` (a cell with its menu up stays `t.hover`), tones walk `text > textDim >
textMuted > textFaint`, and there is **no accent anywhere on the bar** —
purple is spent on document tabs and real selection, not on a navigation aid.
The first ink (the Back glyph) sits on `kGutter`, the same device column every
other strip starts on.

## Pieces

| File | Role |
|---|---|
| `src/widgets/address_bar.h` | `rcx::AddressBar` — ONE custom-painted, header-only widget on the RibbonBar template: lazily recomputed `Layout` of string-addressed cells, hover / pressed / menu-open / disabled / keyboard-focused states, the overflow rule (six fit steps, then the five narrow-pane steps), tooltips, context menus, the three edit scopes over one hidden `QLineEdit`, every dropdown as a transient `QMenu`. No `Q_OBJECT`: outbound events are a `Callbacks` struct of `std::function` that `RcxEditor` bridges to signals. `Qt::NoFocus` at rest. |
| `src/widgets/address_bar_model.h` | Core-only model: `AddressBarState` (the value type pushed per refresh, `operator==` guards the relayout), `siblingFieldsOf` / `rootClassEntries` / `trailPathText` / `resolveDrillPath` over a `NodeTree`, and `AddressBarTreeQueries` (what the menus and the path edit pull on demand). `SiblingEntry::address` (where a field leads) is left 0 here — the model has no memory; the controller fills it. |
| `src/nav_history.h` | `NavEntry` (a PLACE: view root, trail, base + formula, saved-source index, scroll anchor, label) and `NavHistory` (push dedupes the head, truncates forward, cap 50, `back()` / `forward()` skip stale entries, `forgetSource` / `forgetAllSources` follow the saved-source list). |
| `src/address_callbacks.h` | `makeAddressCallbacks(Provider*, ptrSize)` — the one `AddressParserCallbacks` block (module lookup, memory read, kernel paging) every evaluator shares. |
| `src/controller.cpp` | `addressBarState()` / `pushAddressBarState()` (over `focusHopLines` / `frameAddresses`, the compose-side lookups the crumbs and the sibling menus share), `siblingsForCrumb` (the model's list plus where each field leads), `rebaseTo`, `switchSibling`, `navigateToDrillPath`, `collapseToFocus`, `recordNav` / `goBack` / `goForward` / `goUp` / `jumpToHistory` / `restoreNav`, `currentNavLabel` (the history menu's "you are here" row), `containerOf` and the focus-path reconcile. |
| `src/editor.cpp` | Owns the bar (layout item 0 above `m_sci`), bridges `Callbacks` to signals, routes the shortcuts through its KeyPress switch. Line 0 under it is the class header alone (see below). |
| `src/paintutil.h` | `kGutter`, the device-exact edge fills (seam, divider, focus ring), `drawPixmapSnapped`, `pressedFill`. |
| `tools/address_bar_render.cpp` | Harness: `address_bar_render <prefix> [theme]` — 1 / 3 / 6-crumb bars at 240 / 300 / 480 / 760 / 1080 / 1920 px, one sheet per width, every cell's rect + dpr on stdout (UTF-8, so `«` and `…` survive a redirect). |
| `tools/editor_render.cpp drill` | The in-context render (F06 in the book): a real controller + editor, the trail grown by a click, the bar grabbed 4× to `bc_bar_4x.png` and the chip's five liveness states to `bc_bar_states_4x.png`. |
| `tests/test_breadcrumb.cpp`, `tests/test_address_bar_model.cpp` | Controller + widget (geometry, pixels, menus, edits, history, keyboard, split panes, the narrow-pane invariant, and the focus ring at 100 / 125 % — `testKeyboardFocusRingPixels` / `testKeyboardFocusRingIsOneDeviceRowAt125Percent`), and the pure model + `NavHistory`. |

## Item ids

Every cell is addressed by a string id — the namespace `itemRect(id)`,
`itemIdAt(pos)`, `focusId()` and the harness speak. Left to right:

| Id | Cell | Width | Click |
|---|---|---|---|
| `back` `fwd` | Back / Forward, `arrow-left/right.svg` 14 px | 22 | one history step; right-click or press-and-hold on `back` opens the history list (so it stays reachable when `hist` is dropped). The overflow rule drops Forward at step 7d and Back — with the divider — last, at 7e, so the history list keeps an on-bar route until the very end; Alt+Left / Right still work |
| `hist` | `chevron-down.svg` 12 px | 14 | the history menu, Explorer's list: Back entries nearest first, then the place you are at — checked, disabled, worded as `currentNavLabel()` (`Trail.path  @ 0x…`, exactly what the next `recordNav` would store) — then Forward entries nearest first. No separator: the checked row is the divider |
| `up` | `arrow-up.svg` | 22 | the parent crumb (`collapseToFocus` on the deepest hop) |
| — | field divider: one device column, `containerBorderColor`, inset 4 px | 6 + 1 + 6 | everything right of it is "the field" |
| `src` | the source chip: 16-px `iconForProvider` icon (0.40 opacity when disconnected), a 6-px liveness dot at its bottom-right, the provider name (`textDim`, hover `text`); empty provider = `plug.svg` + "Select source". Icon-only in a narrow pane (step 7a): the name lives in the tooltip, `sourceDisplayText()` is empty | 4 + 16 + 4 + text + 2 (icon-only: 4 + 16 + 2) | the source chooser, anchored under the chip; the chip stays `t.hover` while it is up |
| `src.chev` | `chevron-down` `textFaint` | 14 | same as `src` (one hover group, one keyboard stop) |
| `base` | the formula if set else `0x…`, then `baseSepGap()` (one space, as geometry) and a `textMuted` `= 0x…` suffix (what the formula resolves to; only beside a formula — an equation, evenly spaced, and NOT an arrow: the bar's own Back and Forward are arrows); in a narrow pane (step 7b) the bare `0x…` the base resolves to, elided to 60 px | 6 + text + gap + suffix + 6 | the base edit — always on the FULL formula |
| `overflow` | `«` in `textDim`, present only when crumbs are folded | 18 | menu of the hidden crumbs, root first → `onCrumb(i)` |
| `crumb:<i>` | one crumb: `Class.field` for an ancestor, bare `Class` for the deepest | 6 + text + 6 | ancestor → collapse below + scroll (one undo entry, one history entry); the deepest is a FIELD on its own class name (no fill, IBeam) — click or F2 renames it in place, and its context menu carries "Scroll to top" (what the click used to do) and "Other classes…" (the line-0 chooser) |
| `chev:<i>` | after crumb *i*, same › → ˅ flip | 14 | menu of the drillable fields of the class at crumb *i*, the trail's hop checked, each row saying where its field leads (`@ 0x…` in the row's tab column when known, always in its tooltip); the trailing one drills further |
| `space` | the stretch, IBeam cursor | rest | the path edit |
| `recent` | `chevron-down` `textFaint`, pinned at `width − 6 − 16` | 16 | the places menu: Recent (`GotoAddressDialog::loadRecent`), Bookmarks, `<module>` names, then "Go to address… Ctrl+G", "Clear recent" → `rebaseTo` |

Liveness dot: `Live = indHintGreen`, `Stale = focusGlow`, `Disconnected =
markerError`, `Static` / none = no dot (the status chip's mapping, existing
tokens only).

## Tones and states

| | |
|---|---|
| Crumbs | a lone crumb is `textDim` regular (it repeats the doc tab); from depth 2 the ancestors are `textDim` and the deepest is `text` DemiBold — the single weight step on this surface. |
| Rest | nothing filled. Nav glyphs, chevrons and the chip's chevron are `textDim` / `textFaint`. |
| Hover | `t.hover` fill under the cell, tone → `text`; a chevron flips › → ˅. The chip and its chevron light as one rect. |
| Pressed | `pressedFill(t)` for the mouse-down flash only. |
| Menu open | `t.hover` for as long as the menu is up (`aboutToHide` releases it; the source chooser's `dismissed`). A hung dropdown reads as the hover it grew from, not as a selection: on tw.json `pressedFill` is `t.selected` (button == background), which painted the chip as a pale-blue selected box for the life of the popup. |
| Disabled | `setOpacity(0.40)` around the whole cell — never `textDim × 0.4`. Back / Forward / Up / history follow the controller's `canBack` / `canForward` / `canUp`; a disabled cell is laid out (the field never shifts when history appears) and its click is ignored. |
| Keyboard focus | one device-exact 1-px ring in `borderFocused` (the four edge fills — a `QPen` rect is two rows at 125 %). |
| Editing | the overlay is up; the seam row under the field turns `borderFocused` while the text parses / resolves and `markerError` while it does not (or the controller refused it). |
| Tooltips | the widget's `toolTip` mirrors the hovered cell and the app's `GlobalTooltipBridge` shows it; `QEvent::ToolTip` is never handled here and nothing is dismissed on a hover change (the ribbon rule). Per crumb: `QWidgetPrivate  @ 0x7FF6…` from `LineMeta::ptrBase`, or `@ (unreadable)`. A sibling-menu row carries the same words as its `QAction::toolTip` (`vptr  →  QWidgetPrivate  @ 0x20`); the bridge resolves a `QMenu`'s action tips itself, so the menu needs no `setToolTipsVisible`. |

## Overflow

Applied in `relayout()` in this order until the strip fits; each knob turns
only as far as the current overshoot needs (a 30-px squeeze costs the base
three characters, not half of it).

1. **The base** — middle-elide the formula by the overshoot (floor 90 px of text); when the floor cannot absorb it, drop the `→ 0x…` suffix and give the formula back what that freed.
2. **The source name** — the same way, floor 80 px.
3. **Fold crumbs into `«`** from the ROOT, one at a time — never the deepest.
4. **Middle-elide the deepest** to 60 px.
5. **Drop `up` and `hist`** (below ~420 px).
6. **Drop the trailing chevron.**
7. **Narrow panes** — the six steps above bottom out near 395 px with a
   process source and a formula base. Below that the field keeps only what
   names the place, in this order:
   - 7a. the chip goes **icon-only**: the icon, its liveness dot and
     `src.chev` stay, the name moves to the tooltip;
   - 7b. the base shows its **bare literal** — the `0x…` the formula
     resolves to (or the literal itself), middle-elided to 60 px;
   - 7c. Forward is dropped (24 px). Back stays: its right-click /
     press-and-hold is the history list's only on-bar route once `hist`
     is gone;
   - 7e. last resort: Back and the divider go; the chip's icon takes the
     gutter. Alt+Left / Alt+Right and the mouse buttons still navigate.

Folding is width-driven, not a count: the 6-crumb trail in the harness folds
at 1080 and 760 px too, the 3-crumb one first near 480. **Invariant:** the
deepest crumb (≥ 60 px, middle-elided) and `recent` are never laid out past
the right edge — `relayout()` asserts it (debug) and
`test_breadcrumb::testNarrowPanesKeepTheDeepestCrumbAndRecent` pins it at 480
/ 300 / 240 px, at the floor, and at every px between the floor and 300
(Forward always goes before Back). The floor is `AddressBar::kNarrowFloorW`
(228 px: the chip cell's 4-px left edge (its icon ink lands on kGutter), the icon 22, its chevron 14 plus 4 px of pad, the
bare base 72, `«` 18, the deepest crumb 72, `recent` 16 and the 6-px margin);
narrower than that nothing can hold the two and the layout is best effort.
The deepest crumb is never
dropped at any width, because it is the one thing the bar exists to name.
Every elision is DISPLAY only — `state()` carries the full strings and the
base edit always opens on the full formula (the command row used to feed its
ellipsis to the parser and silently no-op).

## Edits

Three scopes, one hidden `QLineEdit` in `PanelSearchField`'s interior; the bar
paints the focus seam under it. Enter is the only commit; Esc and losing
focus restore (the Explorer / Goto-dialog rule). While an overlay is up the
layout is frozen against STATE — a live tick's state push is stored and its
relayout waits for the edit to end — and the cells it covers stop answering
to hover. A pane RESIZE is not a state push: the cells re-lay out at the new
width and the overlay follows them (`resizeEvent` → `followEditGeometry`,
the same `editGeometryFor` rule a fresh open uses), the "after" paper is
recomputed, and the text, caret and selection are left exactly as the user
had them; Esc still restores.

### The overlay IS the cell

Clicking into a segment draws the ring and selects the value. It moves
**nothing** — the standard the editor's own inline edit sets, which replaces a
span inside the Scintilla buffer and so cannot move anything by construction.
Four rules get an overlaid `QLineEdit` to the same place, and
`testOpeningAnEditMovesNothingOutsideItsSegment` is the one that actually
proves it: the bar grabbed at rest and mid-edit, every differing device column
counted, all of them required to fall inside the segment.

1. **Same first pixel.** `syncEditTextMargins` MEASURES the inset a
   `QLineEdit` keeps for itself (our margins at zero, the caret at position 0,
   `cursorRect().left()` — hence `address_bar_detail::EditField`, a two-line
   subclass that makes the protected `cursorRect` reachable) and sets text
   margins so the left inset is exactly `kBasePad`, the same the cell paints
   at. Assuming Qt's number instead of measuring it is a 2-px shove.
2. **Same width.** `editFitWidth` = `kBasePad + advance(FULL text) +
   baseSepGap()`, which for a cell's own text is that cell's width. There is
   deliberately **no minimum** — a one-character value gets a one-character
   field, because that is what the cell is. (`kEditMinW` is not a width floor;
   it is the narrow-STRIP fallback, plus a real floor for the Path scope,
   which stands in for the whole trail rather than for one cell.)
3. **Same face.** `editFont(scope)` returns the cell's own, `deepestFont()`
   included — the deepest crumb is DemiBold from depth 2, and a field that
   opened in the regular weight would re-set the text lighter and narrower.
4. **Same suffix column.** `baseSepGap()` — one space wide — is the gap the
   cell leaves before its `= 0x…`, and it is GEOMETRY rather than a space in
   the string because it doubles as the room the caret needs. The field ends
   in it, so `m_editRect.right() + 1` is exactly where the resting suffix
   stands, and that is where `paintEditChrome` starts the preview.

Growth is for text you **add**. Within one edit session the width is
**monotonic**: `placeEdit` records `m_editGrownW` and `editWidthFor` reads it
back as a floor, so the field grows as you type and never shrinks back — a box
that reflowed under the caret, dragging the preview with every character,
would be its own kind of annoying. The right bound is `editLimit()` less
`kEditPreviewMinW`, so growth can never squeeze out the preview; past that the
`QLineEdit` scrolls internally.

**What an overlay covers** is the cell too, not the strip: the field, the cell
it replaces, and the preview's run (`placeEdit` → `editCellRect` +
`editPreviewRight`). Papering out to `recent` was right when the field was a
180-px slab that half-overlapped the crumbs and left their tails showing;
blanking the trail because you clicked the address is the same sin as shoving
it. **Path is the exception** — it stands in for the whole trail, so it covers
the whole trail, and it is the scope whose covered cells go hover-quiet.

| Scope | Opened by | Text | Grammar | Commit |
|---|---|---|---|---|
| **Base** | click `base`, F2 on `base` in keyboard mode, "Edit address" in its context menu | the FULL formula (never the elided display), selected, in a field fitted to it | while it is open a PRIVATE `RcxTooltip` shows the grammar (`fmt::baseAddressHelpTitle` / `Body`, a monospace table of the five forms plus the operators) — private because `GlobalTooltipBridge` clears the shared one on every `KeyPress`, and re-armed by a 5-s timer against `RcxTooltip`'s own 20-s expiry; anything `AddressParser` takes: `0x1000`, `<mod.exe>+0x40`, `[<mod.exe>+0x10]*2`. Live: `fmt::validateBaseAddress` + the controller's evaluator drive the seam colour and, for a FORMULA only, a `= 0x…` preview — a literal evaluates to itself, and `0x1000 = 0x1000` is the same number twice appearing out of nowhere the moment you click in; Down / `▾` opens a fuzzy-filtered menu of recents, bookmarks and `Provider::modulesCached()` as `<mod.exe>` | `onBaseCommit` → `rebaseTo`: strip backticks / CRLF → evaluate → bare-literal rule → `cmd::ChangeBase` only on change → `GotoAddressDialog::pushRecent`. Invalid input is REFUSED: the overlay stays, the seam goes `markerError`, the status bar says `Base: <error>`. |
| **Path** | click `space`, Alt+D, Ctrl+L, F2 on an ANCESTOR crumb in keyboard mode | the dotted trail (`trailPathText`): `RcxEditor.vptr.parent`, from the base's right edge to `recent` | `Root.field.field…` — the first segment names a top-level struct by type name or name, every later one a drillable field of the class the segments before it reach (a pointer's `refId` class or an embedded struct). Whitespace, a trailing dot and doubled dots are tolerated. Down completes the last segment from that class's drillable fields. | `onPathCommit` → `navigateToDrillPath`: one macro "Navigate to path" (expand every collapsed hop), `focusPath` = the resolved hops, one history entry. An unknown segment is refused with the seam in `markerError` and a hint naming it (`No field 'nope' in RcxEditor`). |
| **Class** | click the DEEPEST crumb, F2 on it in keyboard mode, "Rename class" in its context menu | that crumb's label, which is a bare class name (ancestors read `Class.field`, which is a path segment and not a name) — selected, in a field over the crumb itself | a name. The one rule is that it is not empty, the same rule line 0's class-name edit enforces; nothing stricter, because a bar that refused what the editor accepts would be the odd one out | `onClassRename(Crumb::classId, name)` → `cmd::ChangeStructTypeName` — the class the CRUMB names, not the view root, so a drilled crumb renames the drilled class. One undo entry; renaming a class to the name it already has closes the overlay and pushes nothing. |

## The command row under it

Scintilla line 0 is the class header alone:

```
[▸] struct Player {
```

The chevron opens the view chooser (`typeSelectorRequested`), the class
name is an inline rename (`EditTarget::RootClassName`), the keyword converts
through its right-click menu, and the brace is furniture. The keyword is
whatever `Node::resolvedClassKeyword()` prints for the root — `struct`,
`class`, `enum` or `union` — and `commandRowRootStart` accepts exactly those
four, glued to the chevron span's end (a union root once printed but never
parsed: no type span, no name span, nothing to rename until it was
converted). The right-click menu offers `struct` → Class, `class` → Struct,
`union` → Struct or Class (`convertRootKeyword` refuses only `enum`, which
offers nothing); the keyword is all a conversion changes. The source label
and the base address that used to sit between the chevron and the keyword
are the bar's cells now: line 0 scrolls away with the document, the bar
never does, so the two navigation surfaces no longer hide each other, and
there is one base edit, not two. `buildCommandRowText` (core.h) is the one
builder — `compose.cpp`'s placeholder and `RcxController::updateCommandRow`
both produce that shape — and `commandRowChevronSpan`,
`commandRowRootTypeSpan`, `commandRowRootNameSpan` are the only parsers
left; `EditTarget::BaseAddress` went with the cell. The MCP `status.set`
tool may overwrite the row with `[▸] [Claude: text]`: the text is inert (no
parser anchors on it, nothing is tinted editable, a click starts nothing),
and the tool strips newlines and the old `▾` glyph so a status can never
grow a control of its own.

## Sideways: the chevron menus

`chev:<i>` lists **all** drillable siblings of the class at crumb *i*
(`siblingFieldsOf`: `childrenOf` filtered by `drillTargetId`, stable-sorted by
offset, union members in declaration order), expanded or not, the trail's hop
checked. A pick is a **switch**, not a jump between things already open:
`switchSibling(level, id)` collapses the current hop and expands the chosen one
in ONE undoable macro ("Switch to <field>"), sets `focusPath = prefix + id`,
records one history entry and scrolls the new class header up. Picking the hop
already there is a no-op (no undo entry, no history entry). The chevron after
the deepest crumb lists that class's fields with nothing checked: drill
further.

The bar has **no class dropdown of its own**. It carried one until 2026-09-08
— `root.chev`, a chevron between the chip and the address whose menu was a
flat, unsorted, uncapped, filter-less `QMenu` of every root class. On a
project with thousands of classes that is a wall, not a chooser, and it was a
second and worse copy of the one line 0's `[▸]` chevron opens
(`TypePopupMode::Root`, with a filter box). Its position made it worse still:
a chevron in the run of separators, opening something that was neither the
source before it nor the address after it. The chooser is reached from line 0
and, when line 0 has scrolled away, from the deepest crumb's context menu
("Other classes…"); `RcxController::pickViewRoot` is the one place the
record-then-switch pair for that gesture lives.

Every row also says **where its field leads** (`SiblingEntry::address`,
filled by `RcxController::siblingsForCrumb` when the menu opens, never per
refresh): an EXPANDED pointer from the last compose — the `ptrBase` stamped
on the first row inside it, the same `frameAddresses()` data its crumb would
show; a COLLAPSED pointer (or one expanded onto an empty class, which renders
no row) from ONE provider read at the container's frame plus the field's
offset, by compose's rule (a `Pointer32` reads four bytes, a sentinel is
null, an RVA is base-relative) and only with a valid provider — never a
module enumeration, at most one read per row; an embedded struct or array
from its own row (container + offset). 0 = unknown: no source, a null or
unreadable pointer, or a container frame the crumbs could not place. The
bar prints it as `@ 0x…` after a tab in the row text —
`siblingActionRowText` — which `QMenu` lays out right-aligned in its
shortcut column for free (the places menu's `Go to address…\tCtrl+G` already
uses it). It is NOT dimmed: the style paints that column in the item's own
pen and dimming it would need a `QStyle` of the bar's own, which is not
worth a suffix. An unknown address is left to the tooltip (`@ (unreadable)`),
so a menu under a dead source is not a column of that word. The path edit's
completion menu (`drillFieldsAt`) carries no addresses and keeps the plain
row text.

## History versus undo

Back / Forward are Explorer's, not the undo stack's. An entry is a **place**
(view root, trail, base + formula, saved-source index) plus a scroll anchor
and a label; it is recorded at the gesture that leaves it — F12, `rebaseTo`,
a source switch, a sibling or root pick (`pickViewRoot`, which the line-0
class chooser calls), a path edit, a crumb click, Up — never inside
`setViewRootId` (load, new tab and delete-root call that too), never during a
document load or a restore.

- **Raw restore.** `restoreNav` writes `m_viewRootId`, `m_focusPath`,
  `tree.baseAddress` / `baseAddressFormula` and the active source directly;
  no `cmd::ChangeBase` is pushed. The one thing that goes through the undo
  stack is a "Reopen path" macro when a hop on the restored trail was
  collapsed since (and none at all when the path is still open — the common
  case: Back after F12). Undo / redo never move history.
- **One gesture = at most one undo entry + at most one history entry.** A
  Back / Forward = zero of both (plus the optional reopen macro). Pushing the
  place already on top is a no-op, so a gesture that lands where it started
  (rebase to the same address, re-picking the current root) never grows the
  stack.
- **Ctrl+Z after Back still undoes the original rebase** — the entry that
  rebase pushed is exactly where it was.
- **Stale entries** (a deleted root, a removed hop) are skipped by `back()` /
  `forward()` and left out of `backEntries()` / `forwardEntries()`, so a
  history-menu row's position equals the number of steps its pick walks.
  `canBack` / `canForward` are validity-aware: a cell is never lit for an
  entry that cannot be restored.
- **Saved sources.** An entry's `activeSourceIdx` is a raw index into the
  controller's saved-source list; `removeSavedSource` and `clearSources`
  mirror themselves into the history (`forgetSource` shifts later indices
  down and marks the removed one `kNavSourceRemoved`; `forgetAllSources`
  marks them all). A restore whose source is gone — removed, its file
  vanished, or the attach refused — keeps the current source, restores the
  rest of the place and says so on the status bar.
- **A Back across a File-source switch wipes the undo stack.** Switching to a
  File source is `RcxDocument::loadData`, which clears the undo stack — the
  pre-existing behaviour of every source switch, not something the history
  adds. The history itself survives.
- **Per document, not per pane.** The trail and the history are
  controller-global: both panes of a split show the same state and only the
  pane the gesture came from scrolls (its first visible row is the anchor the
  entry records).
- **The menu marks the current place.** Between the Back rows and the
  Forward rows the history menu lists where you are — `currentNavLabel()`,
  the label `recordNav` would store next — checked and disabled (data 0;
  it never triggers). It is there whenever either stack has a restorable
  entry, and it moves with every step: after Back, the place just left is
  the first Forward row under it. Explorer's dropdown, with the check mark
  as the divider and no separators.

## Shortcuts

Routed through `RcxEditor::eventFilter`'s KeyPress switch (where F2 / F12
live), so the bar itself registers no shortcuts and never takes focus at
rest. During a Scintilla inline edit the arrows keep the caret and navigation
waits for Enter / Esc.

| Key | Does |
|---|---|
| Alt+D, Ctrl+L | open the path edit (Explorer's and the browser's key; the scanner's Ctrl+L is a `WidgetWithChildrenShortcut` on its own panel) |
| Alt+Left / Alt+Right | Back / Forward |
| Alt+Up | Up one level (the parent crumb); Alt+Down is nobody's |
| XButton1 / XButton2 (mouse Back / Forward) | Back / Forward; consumed always, inert during an inline edit |
| F6 | keyboard mode: focus into the bar, starting at the deepest crumb |
| in keyboard mode: Left / Right, Tab / Shift+Tab, Home / End | walk the enabled cells (the chip is one stop, `space` is none) |
| Enter, Space | synthesise a press + release at the cell's centre (the ordinary click path, menus included) |
| Down | open the focused cell's menu when it has one |
| F2 | the base edit on `base`, the path edit on a crumb |
| Esc | back to the document (`scintilla()->setFocus()`, the bar returns to `NoFocus`) |
| in an edit: Enter / Esc / Down | commit / restore / the completion or places menu |
| Ctrl+G | the Goto dialog (main window); also the places menu's "Go to address…" |

A real mouse press anywhere on the bar, or focus leaving it for anything but
a popup, ends keyboard mode.

## Context menus

| On | Items |
|---|---|
| a crumb | Copy path (`crumbPathText(i)`, the dotted path down to it) · Copy address · Copy class name |
| `base` | Copy address · Copy formula · Edit address · Go to address… Ctrl+G |
| the chip | Copy source name · Change source… · Refresh F5 |
| `back` | the history list (the narrow-width fallback for `hist`) |

## Test surface and harness

- `itemRect(id)` / `itemIdAt(pos)` — geometry by id; null when not laid out.
  `segments()` — the rendered trail (`«` first when folded, then the full
  labels); `overflowMenuLabels()`, `sourceDisplayText()`,
  `baseDisplayText()` (the ELIDED text, against `state()`'s full strings),
  `sourceIconRect()`, `livenessDotRect()`, `crumbPathText(i)`.
- `state()`, `stateApplyCount()` — the equal-state early return.
- `isEditing()` / `isBaseEditing()` / `isPathEditing()`, `editText()`,
  `editRect()`, `editCoveredRect()`, `editTextValid()`, `editPreviewText()`;
  `baseCommitFinished` / `pathCommitFinished` / `classRenameFinished` are
  the controller's verdicts; `isClassNameEditing()` and `editHelpVisible()`
  are the class-rename and grammar-help hooks.
- `enterKeyboardMode()`, `inKeyboardMode()`, `focusId()`, `traversalIds()`;
  `setHoldDelayMs()`, `historyMenuOpenCount()` for the press-and-hold.
- Menus are `QMenu` children named `rcxAddressBarHistoryMenu`,
  `rcxAddressBarSiblingMenu`, `rcxAddressBarCellMenu` (the context menu — a
  `popup()` like the rest, never `exec()`), …; on the hidden desktop a popup is closed at
  the first event pump, so a test opens it with `QTest::mousePress`, reads
  and triggers it synchronously, then releases.
- Pixels: `grab()` at real width, `countColour` over `devRect`; the seam is
  the bottom device row, the hover fill `t.hover`, and there must be no
  `indHoverSpan` pixel anywhere on the bar.
- Line 0: `test_command_row` parses `buildCommandRowText`'s output back
  through the real span functions (`row_unionKeyword` for the fourth
  keyword); `test_breadcrumb` checks a live controller's line 0 equals it,
  stays byte-identical across a rebase, and names a union root with an
  editable name (`testLineZeroNamesAUnionRoot`); `test_editor` covers the
  row's tones, the root-name edit's keystroke rules, the inert MCP status
  row and the union root's rename (`testCommandRowUnionRootNameEditable`).
- The finishing set: `testHistoryMenuMarksTheCurrentPlace` (the checked
  row's text equals `currentNavLabel()` and moves with Back),
  `testSiblingRowsSayWhereEachPointerLeads` (a `BufferProvider` holding
  pointer values → `siblingsForCrumb(0)` addresses equal them, collapsed
  by a read and expanded by compose alike; a `NullProvider` → 0 and
  tooltip-only), `testEditOverlayFollowsAResize` (both scopes track the
  base cell / path span at 1000 / 500 / 800 px with the text and selection
  intact; Esc restores).
- Harness: build `address_bar_render` (EXCLUDE_FROM_ALL), run it on the
  hidden desktop (`tools/run_tests_hidden.py`'s `run_hidden()`; set
  `QT_SCALE_FACTOR=1.25` in the driver's environment for the HiDPI pass) as
  `address_bar_render <prefix> [tw|vs|<path>.json]`, then read
  `<prefix>_<w>.png` — three bars (1, 3, 6 crumbs) stacked with magenta gaps
  — and crop by the printed rects (logical × dpr, plus the row's device y).

## The source chooser

`SourceChooserPopup` (`src/sourcechooserpopup.cpp`) is the chip's dropdown
and the reference for the popup family rule, which lives in the comment
above its `paintEvent`: ONE surface (`theme.background`, the ground
`MenuBarStyle::PE_FrameMenu` gives the bar's own `QMenu`s) inside ONE
device-exact 1-px `theme.border` frame painted with `paintutil`'s four edge
fills, the layout inset 1 logical px so no child — the filter field, the
list viewport, its scrollbar, a delegate fill, the accent — ever touches the
frame; square corners, `Qt::NoDropShadowWindowHint`; the filter is the same
surface plus one device-exact seam underneath (`containerBorderColor` at
rest, `borderFocused` while focused — `PanelSearchField`'s grammar on the
popup ground); section captions follow `section_header.h` (8-pt regular
`textDim`, hairline under the row); the Clear All divider and the footer
seam are `containerBorderColor` too, so the frame is the only full
`theme.border` line. Rows: tinted `themedVsIcon` at `textDim` (0.40 when the
source exited), name `text` (bold when active), row 2 as plain tokens
`Process  PID n  x64  active` — the word "active" is the popup's one accent;
no side bar, no pills, no stale-row tint. The scrollbar is styled locally (6
px, track = the surface, handle `textFaint` at 45 % over it) so the app-wide
`QScrollBar` rule never reaches it, and the popup's height comes from the
real row hints capped by the screen under the anchor (flipping above when
there is more room there), so a scrollbar appears only when the list truly
cannot fit. The anchor is the bar's bottom edge under the chip: the popup's
top frame row is the device row after the bar's seam row at any scale. When
the room below is too small the popup flips above the bar and ends one
device row above the bar's TOP edge — `popup(anchor, anchorTop)`, the
controller passing `bar->mapToGlobal(QPoint(0, 0)).y()` — never on the
anchor, which would cover the bar and the chip. A per-row × delete rebuilds
the list while the popup is up; `setSources()` then re-fits the height in
place — the edge on the bar stays: the top edge under it, the bottom edge
when the popup is flipped above it — so no bare ground opens above the
footer and a flipped popup never detaches from the bar. The section
hairlines and the Clear All divider run under the scroll track to the
right frame (`SeamScrollBar`), and a disabled Clear All never lights
`t.hover`.

**Seams reach the frame because the popup paints them.** The layout's
1-logical inset is not device-exact any more than a 1-logical line is: at
125 % it is 1.25 device px, so at half the widths the children end one
device column short of the right frame (and at 150 % and above they always
do), and a seam painted only by the field, the delegate or the scrollbar
stopped with a notch of ground before the frame. So every popup paints
every interior seam itself, across its whole width and before the frame —
the field's row in the field's own (focus-aware) colour, the section
hairlines and dividers from the same rects it hands its `SeamScrollBar`
(viewport-relative; the viewport's offset is whole logical px, so the
device row picked is the delegate's), the footer's row — and repaints on
every scroll. The children paint the same device row over their share; the
popup's fill is what reaches the frame, and the frame, painted last, is
what ends it. A row marker inside the popup (the type chooser's kind
stripe on the current row, the enum picker's per-row stripe) ends at
`kGutter`, never at the row's x=0 against the frame column.

**The family today.** The rule's followers are `TypeSelectorPopup`,
`EnumPickerPopup` and `HexToolbarPopup`, all built from the shared pieces
in `src/widgets/popup_chrome.h` — `PopupFilterField` (the field with its
seam), `popupScrollBarQss` (the local 6-px scrollbar), `SeamScrollBar` (a
scrollbar that carries a list's section hairlines and dividers across its
track) — and `paintutil.h`'s `fillDeviceFrameOfRect`; the ribbon's `…`
overflow item follows the bar's menu-open rule (`t.hover`, never
`pressedFill`) while its menu is up. The one exception is the editor's
`HoverPopupHost`: a hover popup is transient like `RcxTooltip`, so it keeps
the tooltip surface (`backgroundAlt`) — but its frame and its title seam
are the same device-exact rows, not a `QFrame::Box`. The bar's own `QMenu`s
get the surface and the frame from `MenuBarStyle::PE_FrameMenu`.

Pinned by `test_source_chooser` (the frame on exactly one device row /
column per side, the surface strips, the accent budget, the sizing and the
scroll case, at dpr 1.0 and 1.25 under tw and vs; the seams under the
track and to the frame at every width phase, the disabled Clear All, the
in-place re-fit under and above the bar, and the flip above the bar),
`test_type_selector` (the same frame / surface / field probes for the type
chooser under tw and vs, the seams to the frame at every width phase, a
selected header's seam, the current row's stripe inside the gutter, the
banner's elision, the enum picker's left strip, the hex toolbar's
device-exact outlines, focus ring, pin glyph and accent budget, and the
detail pane's seam-coloured dividers),
`test_ribbon_layout` (`overflowMenuOpenIsHover`) and `test_breadcrumb`
(`testMenuOpenCellIsHoverNotSelected`,
`testSourceChooserHangsOnTheSeamAtBothScales`). Harness:
`sourcechooser_render <prefix> [tw vs ...] [many]` (EXCLUDE_FROM_ALL; run on
the hidden desktop) grabs one PNG per theme and prints the popup size, the
dpr and every child / row rect for cropping.

## Rules

- No new theme tokens; no accent on the bar; no `border-radius`, no QSS
  border — every line is a device-exact fill.
- `setState()` early-returns on `operator==`: a live tick with nothing new
  costs nothing. Never tear down and rebuild children — there is exactly one
  (the edit overlay), hidden at rest.
- Every dropdown is built per open and freed on `aboutToHide`; the cell keeps
  its hover fill until then. The source chooser is the one popup the bar does
  not own (`setSourceMenuOpen` mirrors it).
- The bar has no `NodeTree`: what its menus and the path edit need arrives
  as `AddressBarTreeQueries`, pulled when a menu opens or a key is typed —
  never per refresh. Likewise the module list is `Provider::modulesCached()`,
  never the enumerating syscall, and a sibling row's address costs at most
  one provider read, at menu-open time, only for a pointer compose has not
  already dereferenced.
- History is recorded at the gesture, in the controller; the widget only
  asks (`onBack`, `onHistoryJump(delta)` — stepped, not indexed).
