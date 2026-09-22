# TypeSelectorPopup (typechooser) audit

Exhaustive issue audit of `src/typeselectorpopup.cpp` / `.h` (+ consumer
`controller.cpp`). Produced by an 8-way parallel review (4 line-range
correctness sweeps + 4 cross-cutting lenses: keyboard/focus, lifetime/signals,
filter/sort, state/mode). "✓N" = independently flagged by N reviewers.

Status legend: **[FIXED]** implemented + verified (`test_type_selector` 32/0,
`test_tooltip_flicker` 8/0, `test_controller` 56/0) · **[DECIDE]** needs a
product/UX decision · **[VERIFY]** needs interactive GUI verification
(`SKIP_GUI_INTEGRATION` disables the relevant tests) · **[API]** removal of
declared/public surface — confirm intent first.

---

## Fixed this pass (11)

1. **[FIXED] Array toggle with empty/zero count committed a *plain* (non-array) type.** ✓4
   `acceptIndex` ~1801 dropped the `[]` when the count edit was empty; `setModifier(3,0)`
   bypassed the `QIntValidator`. Now: array toggle always emits an array (empty/invalid → `[1]`);
   `setModifier` clamps `qMax(1,arrayCount)`; create path forces ≥1.
2. **[FIXED] `TypeEntry` dangling ref across `typeSelected`.** ✓lifetime
   `acceptIndex` emitted a `const&` into `m_filteredTypes` before `hide()`; the slot can
   re-enter `applyFilter` and clear the vector. Now copies by value and `hide()`s before `emit`.
4. **[FIXED] Accept during loading emitted a stale entry.** ✓state
   Skeleton rows exist in the model while `m_filteredTypes` holds the previous open's data.
   Added `if (m_loading) return;` at the top of `acceptIndex`.
5. **[FIXED] `RcxTooltip` leak in the delegate.** ✓lifetime
   `new RcxTooltip(nullptr)` (null parent, never deleted). Routed through `sharedRcxTooltip()`
   in both `helpEvent` and `hideTip`; removed the leaking member.
6. **[FIXED] `parseTypeSpec` base-name corruption.** ✓agent-1
   `int***` left a stray `*` in the base name; array count not trimmed; no upper bound.
   Now chops every trailing `*` (depth capped at 2), trims the count, clamps to `1<<20`.
   (`int32_t[0]` → arrayCount 0 preserved per existing test.)
15. **[FIXED] Unescaped `displayName` in detail-pane/preview HTML.** ✓2
    Names with `<`, `>`, `&` corrupted the markup. `toHtmlEscaped()` at all 6 sites
    (preview footer/label, detail header, C-decl `tn`, Select/Pointer/Array action links).
16/26. **[FIXED] Chip "none" kept a nondeterministic group.** ✓3
    Iterated a `QHash`; now keeps "Hex" deterministically.
20. **[FIXED] `all`/`none` chips fired N refilters.** ✓2
    Bulk `setChecked` triggered `toggled`→`applyFilter` per chip. Now wrapped in
    `QSignalBlocker` + a single `refilter()`.
22. **[FIXED] Ctrl+F hijacked any Ctrl chord.** ✓agent-4
    `(modifiers() & ControlModifier)` → exact `== Qt::ControlModifier`.
- **[FIXED] Size-bar int overflow** (~352): `sizeBytes * m_barW / m_maxSz` now 64-bit.
- **[FIXED] Nondeterministic sort ties** : all 8 `std::sort` → `std::stable_sort`.

### Rejected after checking
- **`qobject_cast` for the delegate downcasts** — `TypeSelectorDelegate` has **no `Q_OBJECT`**
  (private `.cpp` class, see comment ~1957), so `qobject_cast` cannot work. `static_cast` is
  correct here. The audit suggestion does not apply.

---

## Fixed after the first pass

3. **[FIXED] Selection reset to row 0 on every `applyFilter` → Enter accepted the WRONG type.** ✓3
   `applyFilter` now captures the selected entry's identity (displayName + kind + structId/primitiveKind/
   isRelative) at the top and restores it after the rebuild, falling back to the first selectable row only
   if it didn't survive the filter. `setTypes` clears the current index before its `applyFilter` so nothing
   carries across opens (its own current-type pre-select still wins). Verified by a new deterministic
   headless regression test `testSelectionSurvivesRefilter` (model-state only, not GUI-flaky):
   `test_type_selector` 33/0, `test_controller` 56/0.
   *Deferred (separate, subjective):* arrow-key wrap-around behavior (#21).
28. **[FIXED] `dismissed()` fired even on a successful pick.** ✓state
    `hideEvent` emitted `dismissed()` unconditionally, so it fired right after `typeSelected`/
    `createNewTypeRequested`. Added an `m_accepted` flag (set on accept/create, reset on each
    `popup`/`popupLoading`); `hideEvent` now emits `dismissed()` only when no pick was made.
    Verified by `testDismissedNotEmittedOnPick`. (No current consumer connects `dismissed`, so this
    is contract-hardening for future use.)

## Needs your decision (UX policy) — NOT auto-applied

Each item below now carries a code-grounded **Recommendation** so the call is a quick yes/no — say
which option you want and it gets applied (none are implemented yet).

10. **[DECIDE] Sort toolbar is ignored while a text filter is active; `SortGroup` ignores `m_sortDir` (arrow lies).** ✓filter
    Apply the chosen sort as a secondary key within filtered results, or grey the toolbar while filtering?
    *Code:* `applyFilter` filter path (`typeselectorpopup.cpp` ~1640) sorts `scored` by fuzzy score only;
    `m_sortMode`/`m_sortDir` are unused while `filterBase` is non-empty. Sort buttons live in `m_sortBtns`.
    **→ Recommendation: grey/disable the toolbar while filtering.** Fuzzy-relevance order is what you want
    mid-search, so re-sorting fuzzy hits by name/size fights the search; disabling the buttons (loop
    `m_sortBtns`, `setEnabled(filterBase.isEmpty())`) is honest and ~3 lines. (Re-enable on clear.) Cheapest,
    least-surprising, no behaviour change to the un-filtered view.
19. **[DECIDE] Chips don't reset across opens (sticky); current type can be silently hidden by an off chip.** ✓state
    Reset chips to all-on each open, or keep sticky and auto-enable the current entry's chip?
    *Code:* chips are `setChecked(true)` only at construction (~537) and never touched in `setTypes`, so
    they persist. `setTypes` already knows the current entry (`m_currentEntry`/`m_hasCurrent`).
    **→ Recommendation: keep sticky, but auto-enable the current entry's group chip on open.** In `setTypes`,
    when `m_hasCurrent`, compute its `kindGroup` (same `kindGroupFor`/`"Ctr"` rule) and
    `m_groupChips.value(group)->setChecked(true)` (under a `QSignalBlocker`). Preserves the user's deliberate
    filter while guaranteeing the type they're editing is never hidden — strictly better than a full reset,
    which discards their filter every open.
21. **[DECIDE] First Down-from-filter doesn't advance** (uses `cur.row()` not `+1`, `typeselectorpopup.cpp` ~1944). ✓3
    One reviewer argued the current "enter list at current selection" behavior is actually desirable.
    *Code:* filter-edit Down does `startRow = cur.isValid() ? cur.row() : 0; nextSelectableRow(startRow,1)`,
    so it lands ON the current selection (the in-list Down at ~1974 uses `cur.row()+1`).
    **→ Recommendation: WONTFIX (keep current).** Landing on the current selection when you arrow down out of
    the filter is the conventional, useful behavior (you immediately see/act on the pre-selected type, then
    arrow further). Changing it to skip the selection would be the surprising option. Leave as-is unless you
    specifically want first-Down to skip.

## Fixed in the second pass (6, objective/self-contained — 2026-06-05)

- **[FIXED] Match-highlight base derived from `fullText.lastIndexOf(" - ")` instead of `entry.displayName`**
  (~281). The model string is `displayName + " - <size>B"`; `lastIndexOf(" - ")` mis-split any zero-size
  entry whose `displayName` itself contains `" - "`, throwing off the fuzzy-highlight char offsets (which
  index into `displayName`). Now takes the base straight from `entry->displayName` when the entry is
  available; falls back to the old parse only when it isn't.
- **[FIXED] `font.pointSize()` returns -1 for pixel-sized fonts → chip/sub/section fonts snap to 7pt.**
  Added `resolvedPointSize(const QFont&)` (resolves via `QFontInfo` when `pointSize()<0`) and routed all
  five derivation sites through it (delegate small font ~142, title/small/chip fonts ~1004/1012/1022,
  detail-pane `pt` ~1368).
- **[FIXED] Section pip color derived from the label's first word** (~1681). `title.left(indexOf(' '))`
  produced `"Pointer"/"String"/"Type"` — none of which `kindGroupColor()` knows (`Ptr`/`Str`/`Ctr`), so
  those pips fell to the neutral default. `appendSection` now takes the exact group key; Recent passes
  `"Common"` (neutral grey, intentional for the heterogeneous section).
- **[FIXED] Delegate `setFilteredTypes`/`setMatchPositions` set *after* `setStringList`.** On a fresh
  open the synchronous repaint from `setStringList` ran with the delegate's pointers still unset →
  rows painted with no entry/highlight data for one frame. Moved the delegate pointer assignment to
  *before* `setStringList` (the vectors are fully populated by then).
- **[FIXED] Negative/zero popup size near screen edges; `popup`/`popupLoading` size math diverged.**
  Both computed height as `avail.bottom() - globalPos.y()`, going zero/negative near the bottom/right
  edge (degenerate 1px window). Extracted a shared `placeOnScreen(globalPos, w, h)` that clamps to a
  usable minimum and *shifts the origin* back onto the screen instead of shrinking. Both entry points
  now share it, so their geometry can't diverge.
- **[FIXED] `applyFilter` mutated `m_allTypes` (kindGroup auto-assign) — side effect in a query method,
  3 duplicate sites.** Moved the normalization to a single loop in `setTypes` (the sole owner of the
  type list), so the filter path now reads `t.kindGroup` directly and never writes. The three in-filter
  auto-assign blocks are gone; loop vars are now `const auto&`.

Verified: `Reclass` builds clean; `test_type_selector` 37/0 (19 GUI-skipped), `test_controller` 56/0,
`test_tooltip_flicker` 8/0. Three of these fixes have dedicated regression tests, each proven to fail
when the bug is reintroduced:
- `testPopupStaysOnScreenAtEdge` — opens at the screen's bottom-right corner, asserts a non-degenerate
  on-screen popup (fails `degenerate popup size 1x1` with the old shrink math).
- `testSectionKindGroupKeysAreValid` — asserts every section header carries a real group key, not the
  label's first word (fails `section 'Type' has bogus pip key 'Type'` with the old derivation).
- `testDerivedFontsDontSnapToFloorWithPixelFont` — sets a 40px (pixel-sized) base font and asserts the
  derived chip fonts resolve above 7pt via `QFontInfo` (fails `chip font resolved to 7pt` with the old
  `font.pointSize()` path). NB: `font().pointSize()` reports -1 in pixel mode, so the test reads through
  `QFontInfo` to get a concrete size in either mode.
The remaining two (match-highlight base, delegate-pointer ordering) are rendering-only fixes covered by
no-regression of the full suite; `applyFilter` no-mutate is a cleanliness refactor with no observable
behavior change to assert. (A read-only `filteredTypes()` test accessor was added to the public API to
enable the section-key test.)

### Re-examined, NOT a bug (left as-is)
- **`m_currentNodeSize`/`m_pointerSize` not reset per open** — the sole consumer (`controller.cpp`
  ~5453-5454) calls `setCurrentNodeSize`/`setPointerSize` on *every* open, *before* `popupLoading`.
  Resetting inside `popup()`/`popupLoading()` would wipe those just-set values → a regression. No safe
  reset hook exists and there's no leaking path today, so this is a non-issue in the current code.

## Lower-value / confirm-intent (still open)

- **[API] Dead surface:** `saveRequested` signal (declared, never emitted), `SortAlign` enum
  (no button creates it), `m_footerLabel` (always null, guarded everywhere). Remove only if not future API.
- **[DECIDE] `eventFilter` only on `m_filterEdit`+`m_listView`** — Esc/Enter/Ctrl+F dead when focus is
  on the array-count edit / modifier buttons / chips; the `[]` toggle traps the keyboard there. ✓keyboard
  Needs a focus model (override `keyPressEvent` on the frame, or install the filter on more widgets).
- **[DECIDE] No keyboard path to `*`/`**`/`[]` modifiers**; filter text isn't run through `parseTypeSpec`,
  so typing `Foo*` / `int[10]` matches nothing. ✓2
- Chip badge counts computed pre-chip-filter (disagree with status label, ~1586). ✓filter
  *(borderline [DECIDE]: the badge arguably SHOULD show "how many appear if you enable this chip" =
  pre-chip-filter count, so the disagreement with the post-filter status label may be intentional.)*
- "Recent" section respects chip filtering (recent picks vanish on chip toggle, ~1653). ✓filter
- No focus restoration to the caller on dismiss (Windows focus quirk risk). ✓keyboard
- RVA `isRelative`: lost when a `*` modifier is toggled on a Ptr entry; pre-select picks the wrong RVA variant. ✓state
  *(NOT touched autonomously — sits in the RVA-pointer-display area of recent commits a6c29de/ae6e3b6.)*
- Array modifier is a dead control in `ArrayElement` mode (controller ignores `arrayCount` there). ✓state
- `runPrimerOnce` calls `processEvents()` (re-entrancy during preload). ✓lifetime

---

*Generated 2026-06-04. First-pass fixes 1/2/3/4/5/6/15/16/20/22/28 + size-bar + stable_sort, plus new
regression tests `testSelectionSurvivesRefilter` and `testDismissedNotEmittedOnPick`. Second pass
(2026-06-05): match-highlight base, `resolvedPointSize`, section pip key, delegate-pointer ordering,
`placeOnScreen`, `applyFilter` no-mutate. All in the working tree (uncommitted). See
`git diff src/typeselectorpopup.{h,cpp}`.*
