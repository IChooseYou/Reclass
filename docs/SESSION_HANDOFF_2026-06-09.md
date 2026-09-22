# Session handoff — 2026-06-09

**Nothing is committed.** This session ran a thorough bug sweep over the large in-flight uncommitted
tree (the `modulesCached`/RTTI + UI/UX feature from the 2026-06-08 handoff) and fixed 7 real bugs, added
3 regression tests, and deferred 1 minor bug. `build/` is current, full suite **57/57 green**, app
smoke-tested with no regression. Builds on top of — and shares the commit with — the prior in-flight work.

---

## Bugs fixed (7) + 1 deferred

Found via 4 parallel adversarial review waves covering **every substantive changed file** (broad diff →
diffutil/value-render, source-lifecycle, ABI/provider-cache → editor selection/overlay/popup, UI panels,
dock/rail/console → rtti/profiler/processpicker).

| # | Severity | Bug | File(s) | Fix |
|---|----------|-----|---------|-----|
| 1 | **High (corruption)** | "Break into Class" via the **Edit menu / Ctrl+Shift+B** was unguarded — `regionFromCurrentSelection` unions PARENT-relative `n.offset` for node selections, so a union member / inline-struct field resolved to the wrong bytes (the context-menu paths guard with `isDirectViewFrameChild`; the menu-bar entry did not). | `controller.cpp` | Pushed the guard into the shared chokepoint (node branch of `regionFromCurrentSelection`) so all callers are covered. **Test added.** |
| 2 | **High (corruption)** | Switching a live process → **File** source skipped `resetSnapshot()`, so `refresh()` (controller.cpp:1959, ungated) kept composing the **stale process snapshot** — the file showed the old process's bytes and never recovered (non-live provider's tick early-returns). 2 independent reviewers confirmed. | `controller.cpp` (`switchToSavedSource`, `selectSource` File branches) | `resetSnapshot()` added to both File paths (matches every other switch path). |
| 3 | Medium | A **live** provider attached with `registerAsSavedSource=false` (tutorial self-attach, kernel/physical tab, MCP attach) left `m_activeSourceIdx == -1` → status keyed `None` → chip masked it to a neutral **"Static"** dot, mislabelling a live polled process. | `controller.cpp` (`onRefreshTick`) | Reordered: liveness wins over the saved-index check. **Verified end-to-end** — chip now renders the green "Live" dot. |
| 4 | Medium | The Workspace **empty-state overlay** (`EmptyHintTreeView` "No types yet") could **never appear** — `buildProjectExplorer` always appended an "ALL TYPES" section header → `rowCount >= 1` → the overlay's `rowCount>0 return` guard always skipped. | `workspace_model.h` | Header now emitted only when there are types. **Test added** (new `test_workspace`). |
| 5 | Efficiency | Code pane re-themed its lexer **every refresh tick** even on a render cache-hit (~270 Scintilla style calls/tick). | `main.cpp` (`updateRenderedView`) | Gated `applyCodeLexer` on `!cacheHit` (theme switches re-theme via `applyTheme`). |
| 6 | Minor (leak) | `QsciScintilla::setLexer` orphans the prior lexer (parented, accumulates per format-class swap). | `code_highlight.h` | `deleteLater()` the old lexer at all 3 swap sites. |
| 7 | Low | UTF8/UTF16 string nodes were struck **"unreadable"** when their declared display width (`strLen`, default 64) overshot the mapped page — despite the displayed NUL-terminated text being readable. | `compose.cpp` | Probe only the first element for string kinds. **Test added.** |

### Deferred (1) — needs your steer
**Type-picker suppression guard swallows the click after a picker CANCEL.** `m_suppressTypePickerForNode`
(editor.cpp:5306) is armed when the picker *opens*, but only cleared by a real selection change or by
consuming the suppressed click. Cancel (Esc/click-away) or picking the same type changes nothing → guard
stays armed → next type-cell click is eaten (user must click twice). The correct fix arms on the actual
type *change* (which happens in the controller via `typePickerRequested`; the editor only learns via the
next refresh), so it needs controller→editor plumbing into **memory-flagged, user-validated** guard logic
(`feedback_value_popup_unification`). Left for your UX call — minor nit (one extra click after a rare
action). Documented in memory `project_deferred_typepicker_cancel_guard`.

---

## Regression tests added (3)

| Test | File | Pins |
|------|------|------|
| `testDirectViewFrameChild_RejectsNestedField` (extended) | `tests/test_context_menu.cpp` | Bug 1 — `regionFromCurrentSelection` refuses a nested-node selection at the shared chokepoint. |
| `testUnreadableStringWidthDoesNotFalseStrike` | `tests/test_compose.cpp` | Bug 7 — a wide-`strLen` string over a short buffer is NOT struck if its start is mapped. |
| `testEmptyProjectHasNoRows` + 2 more | **`tests/test_workspace.cpp` (NEW target)** | Bug 4 — empty project → 0 rows; struct → header+row. |

**Bugs 2 & 3 are verified-by-review only (not unit-tested).** Confirmed (by reading the actual pipeline,
not assumed): both live in the **private, timer-driven, QtConcurrent-async** `onRefreshTick`/`onReadComplete`
path. A deterministic test needs either a `friend`/visibility change on `controller.h` or event-loop/timer
waiting in the already-flaky `test_controller` — an invasive/flaky addition left for your call. Bug 3 is
visually verified (green "Live" dot); Bug 2 is code-verified (the fix matches all other switch paths).

---

## ⚠ Pre-commit manifest update

The 2026-06-08 manifest listed 4 integral untracked files. **This session adds one more:**

| Untracked file | Why it must be in the commit |
|----------------|------------------------------|
| `tests/test_workspace.cpp` | `add_executable(test_workspace …)` + `add_test` in the modified `CMakeLists.txt` — **CMake configure fails** without it. |

So the commit = tracked-modified ＋ the 2026-06-08 manifest's 4 untracked (`src/code_highlight.h`,
`tests/test_source_chooser.cpp`, `tools/sourcechooser_render.cpp`, `tools/coderender.cpp`) ＋
**`tests/test_workspace.cpp`**. (`src/diffutil.h` is also untracked-but-integral, included by the WIP.)

**Files changed this session:** `src/core.h`, `src/compose.cpp`, `src/controller.cpp`, `src/editor.cpp`,
`src/main.cpp`, `src/scannerpanel.cpp`, `src/workspace_model.h`, `src/code_highlight.h` (untracked),
`src/widgets/empty_overlay.h` (new untracked), `tests/test_compose.cpp`, `tests/test_context_menu.cpp`,
`tests/test_workspace.cpp` (new), `CMakeLists.txt`.

---

## Code-quality cleanups applied

**(1) Empty-state overlay → single source of truth.** The review (finding E#1) flagged that the
centered two-line empty-state painter was duplicated byte-for-byte in `main.cpp` (`paintEmptyOverlay`,
used by the Workspace/Bookmarks empty-hint views) and `scannerpanel.cpp` (`EmptyResultsTable::paintEvent`).
Hoisted to a new shared header **`src/widgets/empty_overlay.h`** (`namespace rcx`, matching the `widgets/`
convention); both sites now call `rcx::paintEmptyOverlay`. **Behavior-identical** — verified the two impls
were equivalent (the scanner's hint is always non-empty, so `main.cpp`'s `if (!hint.isEmpty())` guard is a
no-op for it). Build clean, suite 57/57, app smoke-tested. **NOTE:** `widgets/empty_overlay.h` is a new
untracked file `#include`d by `main.cpp` + `scannerpanel.cpp` — **must be in the commit** (build breaks
without it), like `code_highlight.h`/`diffutil.h`.

**(2) selId decode → single source of truth.** The review (finding E#4) flagged that there was a selId
*encoder* (`selIdForLine`) but no *decoder* — the mask `sid & ~(kFooterIdBit | kArrayElemBit |
kArrayElemMask | kMemberBit | kMemberSubMask)` was hand-copied at **17 sites** across `controller.cpp`
(13), `main.cpp` (3), `editor.cpp` (1), so adding a future selId flag bit would silently corrupt every
un-updated copy. Added `rcx::baseNodeIdFromSelId()` to `core.h` (next to `selIdForLine`) and routed all
17 sites through it. Pure dedup — **behavior-identical** (same bitmask), verified by the full suite
(57/57; any mask error breaks the selection tests). Matches the codebase's existing single-source-of-truth
pattern (`selIdForLine`, `editorPaperColor`, `resolvedPointSize`).

---

## Verification

- **Full build** clean (Reclass + 5 plugins + all tests).
- **Full deterministic suite 57/57** (was 56; +1 `test_workspace`). The lone parallel-sweep `test_controller`
  failure is the documented `reference_ci_flake_cursor_hover` flake — passed 2/2 serially.
- **Canonical `ctest -j1` 57/57 clean** (the CI path). `test_workspace` is registered (Test #10), so CI
  runs it. One transient `test_byte_selection_controller` flake appeared in the first full sequential run
  (Test #33) but did NOT reproduce — passed 8/8 on isolated re-runs and the full `ctest` re-ran 57/57 clean.
  It's the documented cross-test-state flake (`reference_ci_flake_cursor_hover`, refined 2026-06-09); the
  selId-decode refactor is deterministic/behavior-identical and cannot cause intermittency.
- **Visual smoke test** (`--screenshot`): editor renders identically to baseline.
- **Bug 3 end-to-end:** status chip dot is green (Live) for the self-attach tab.
- **Independent adversarial review** of all 8 of this session's changes (a fresh reviewer, not the author):
  **zero bugs found** — each verified correct incl. edge cases (string-kind probe, the nullopt guard, both
  resetSnapshot placements, all 5 status cases, the lexer-gating + leak fix, all 17 selId substitutions,
  the empty-header guard). The one edge note (a 0-byte file → Disconnected) is pre-existing, not a regression.

---

## Open decisions (NOT done autonomously — yours to steer)

| Item | Note |
|------|------|
| **Deferred Bug** (type-picker cancel) | UX call in validated guard logic — see above / memory. |
| **Bugs 2 & 3 regression tests** | Need a `friend`/visibility seam on `controller.h` or async-pump tests in the flaky `test_controller`. |
| **Cleanup/altitude dedups** (from the prior review) | `paintEmptyOverlay` — **DONE** (see "Code-quality cleanups applied"). Remaining, held because they shift behavior: **(a) `themeCppFamilyLexer` reuse** in editor.cpp — investigated 2026-06-09: it is NOT a pure dedup. editor.cpp maps `KeywordSet2 → syntaxKeyword` (2415) while the shared helper maps it `→ syntaxType`, and the helper themes `VerbatimString`/`CommentLineDoc` the editor omits. Moot today (the editor's plain `QsciLexerCPP` has no KeywordSet2 words and the hex view has no verbatim strings / `///`), but routing changes the semantic intent and could affect user comment content. Decide: keep KeywordSet2 as keyword-colored, or adopt the helper's type-colored mapping? **(b) break-guard consolidation** — touches corruption-sensitive break logic. |
| **UI improvement work** ("the ui shit") | Its framing ("must vibe, don't jam it in") requires your taste judgment. Bug 4 (empty-state) was the one objective piece and is fixed. |
| **Commit structure + debris cleanup** | Per the 2026-06-08 standing decisions — still yours. |

## Env note
`/tmp` (Temp) drive is **full (0 free)** and **E: is at ~4.4 GB** — an agent's shell hit ENOSPC during the
session. Worth clearing before the next big build.
