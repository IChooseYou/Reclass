# Session handoff — 2026-06-16

**Committed and pushed to `origin/main` as IChooseYou** (code-only, no co-authors): `61aaf0a` (the fixes
below) + `9f39e36` (CI fix). **CI is fully green** — run 27638958087 published release `Snapshot 16-06-2026`
with all 4 platform artifacts. This session fixed 4 editor bugs (1 user-reported, 1 prior-session in-flight,
2 found-and-fixed via an adversarial review of the selection/overlay/refresh subsystem) + 1 CI build break,
added 1 regression test proven to catch its bug, and **surfaced a 7-bug "stale chrome on raise/show" cluster
for your triage** (below — best fixed as one helper, not 7 edits). Local full suite **57/58 green** (the 1 is
an unrelated environmental `test_scanner` self-attach flake — see below). Docs (this file, etc.) remain
uncommitted by design (code-only).

---

## Bugs fixed (5)

| # | Severity | Bug | File(s) | Fix |
|---|----------|-----|---------|-----|
| 1 | Medium (user-reported) | **Byte-selection grey row band desyncs from the purple highlight after a refresh/undo.** A byte-boundary selection spanning 3 hex rows kept its purple `IND_BYTE_SEL` on all rows after Ctrl+Z, but the grey `M_SELECTED`/`M_ACCENT` band dropped the bottom (partial) row; status bar stayed stale at "3 nodes selected" (`build/issue.png`). Root cause: the editor's `m_lastByteRows` is a per-editor **emit-dedup cache**, NOT a sync guarantee with the controller's `m_selIds`. When `refresh()` prunes/mutates `m_selIds` (controller.cpp ~2098) while the byte selection's covered set is unchanged, the resync emit at `editor.cpp` ~3494 (`covered != m_lastByteRows`) is suppressed → `onByteSelectionRows` never runs → the band paints from the stale `m_selIds`. (Note: `applyByteSelectionOverlay` runs at editor.cpp:2945, after `m_applyingDocument=false`, so the `!m_applyingDocument` half of that guard is effectively dead — the dedup is the real gate.) | `editor.h`, `editor.cpp`, `controller.cpp` | Controller now **pulls** the truth each refresh instead of relying on the emit: `RcxEditor::byteCoveredRows()` returns `m_byteCoveredRows` (stored unconditionally every `applyByteSelectionOverlay` call); `refresh()`'s tail reconciles `m_selIds = editor->byteCoveredRows()` when `hasByteSelection()`, before `updateCommandRow()` (so its `selectionChanged` emit carries the corrected count too) and `applySelectionOverlays()`. Guarded on `hasByteSelection()` so non-byte selection paths are untouched. **Test added & proven to fail without the fix.** Diagnosed via a 5-agent read-only investigation workflow + manual cross-check. |
| 2 | Low (prior-session in-flight) | **Status-bar source chip didn't update on doc-tab switch.** The chip/title/bookmarks refreshed only via each doc dock's `visibilityChanged(true)`, which doesn't reliably fire on a tabified-dock tab switch. | `main.cpp` (`QApplication::focusChanged` hook ~1271) | When focus lands in a different doc dock, walk up to its `QDockWidget`, update `m_activeDocDock`, and refresh title/source-chip/bookmarks (reuses the same calls the `visibilityChanged` path uses). Outside the fragile tab-bar reconcile path. **Caveat:** fires when focus enters the new tab's editor; a pure tab-bar click without touching the editor may still lag — the complete fix would hook the doc tab-bar `currentChanged` (in the reconcile path), left for your go-ahead. |
| 3 | Low | **Right-click status-count staleness.** `showContextMenu` (controller.cpp:4089-4101): right-clicking a row *outside* the current selection moves the selection (`m_selIds.clear(); insert(clickedId); applySelectionOverlays()`) but — unlike `handleNodeClick`, which routes through `updateCommandRow()` — never `emit selectionChanged(...)`, so "N nodes selected" lingered on the pre-right-click count. Found by the investigation workflow's status-source agent. | `controller.cpp` | Added `emit selectionChanged(m_selIds.size());` after the overlay paint, matching every other selection-mutation path. **No dedicated unit test** — the selection mutation sits at the top of `showContextMenu` before the blocking menu `exec()` (4480), which the existing `test_context_menu` suite never invokes; verified instead by build + `test_context_menu` (38) + `test_controller` (60) green. Applied under the standing autonomous directive after surfacing it; trivially reversible (one line) if you'd rather it stayed deferred. |
| 4 | Low | **`copySavedSources` mutates source state without notifying.** `copySavedSources` (controller.cpp:6744) set `m_savedSources`/`m_activeSourceIdx` + `pushSavedSourcesToEditors()` but — unlike sibling `removeSavedSource` (6741) — never `emit documentChanged()`. The tab-bar source icon is refreshed only via `documentChanged → refreshDocTabSourceIcon` (main.cpp:3741). Its only caller is `project_new` (forceFreshDoc=false: add a class to an existing project in a new tab); `createTab` set the icon to `plug.svg` *before* `copySavedSources` populated the sources, so a **disconnected** saved source kept the plug icon until an unrelated reconcile (live sources self-heal via `sourceLivenessChanged`). Found by the review's controller-mutations agent (repro corrected by the verifier). | `controller.cpp` | Added `emit m_doc->documentChanged();` at the end of `copySavedSources`, matching `removeSavedSource`. Safe to emit mid-tab-creation: of the 3 `documentChanged` handlers, two are lightweight (icon/bookmarks refresh; MCP notify) and the heavy one (rebuild workspace/symbols, main.cpp:3994) is `QTimer::singleShot(0)`-deferred + `QPointer`/`m_tabs`-guarded. Verified by build + `test_tab_source_icon` (7) + `test_source_management` (18) + `test_controller` (60) green. |
| 5 | **High (CI red)** | **`findChildren<ResizeEdge*>` failed to build on Qt 6.8 (CI).** `ResizeEdge` (frameless-resize `QWidget`, `main.cpp:2010`) had no `Q_OBJECT`; Qt 6.8 added a `HasQ_OBJECT_Macro` static_assert to `findChildren<T>` that 6.5.2 (local) lacks — so it built locally but failed `main.cpp.o` on all CI platforms (the prior `be9c2b6` commit was already red for this). | `main.cpp` (commit `9f39e36`) | Added `Q_OBJECT` to `ResizeEdge` (MOC already wired via `#include "main.moc"`). Audited every other `findChildren` — all target standard Qt classes. CI verified fully green (run 27638958087, release published). Memory: `reference_qt68_ci_findchildren`. |

---

## Regression test added (1)

| Test | File | Pins |
|------|------|------|
| `testRefreshReconcilesRowBandToByteSelection` | `tests/test_byte_selection_controller.cpp` | Bug 1 — sets a 2-row byte selection, empties `m_selIds` via `clearSelection()` (leaves `m_byteSel`/`m_lastByteRows` intact so the emit would dedup), then `refresh()` and asserts the band is restored to the covered rows. **Verified to FAIL** with the reconcile disabled (`selectedIds()` empty instead of `{2,3}`), passes with it. |

Memory written: `reference_byteband_selids_reconcile` (indexed in `MEMORY.md`).

---

## Surfaced for your triage — "stale chrome on raise/show" cluster (7 bugs, NOT applied)

The same review's main-window agent found **7 sites** that share one anti-pattern: they switch the active doc
via `dock->raise(); dock->show(); m_activeDocDock = X;` **without** refreshing chrome (window title / source
chip / scanner title / bookmarks). Each is **layout-conditional** (real, but mostly self-heals in the common
tabbed case; genuinely persists in floating/side-by-side-split layouts, or when `visibilityChanged` skips a
tab switch — which the code's own comment at main.cpp:1271 admits it does). Adversarially verified; severities
were **narrowed** from the raw candidates.

| Site (main.cpp) | Trigger | Severity |
|-----------------|---------|----------|
| **3950** | Ctrl+Click a struct (`requestOpenStructInNewTab`) that already has a tab → reuse it | medium |
| **7766** (+ siblings **7613**, **7834**) | Find-field dialog: activate a result in another tab | medium |
| **8571** | Workspace tree **double-click** a child member (nested class) | medium |
| **8595** | Workspace tree **double-click** a root struct with an existing tab | medium |
| **8640** | Workspace tree **single-click** (peek) a child member | medium |
| **8662** | Workspace tree **single-click** (peek) a root struct | low |
| **8352** | Workspace context menu "Open in Current Tab" (only the **window title** is stale — doc/provider unchanged, so chip/bookmarks are fine) | low |

**Why my fix #2 doesn't cover these:** the `focusChanged` backstop (main.cpp:1271) is *defeated two ways* here —
(a) focus stays in the workspace tree / find dialog, never entering the doc dock; and (b) these sites **pre-set
`m_activeDocDock = X`**, so when focus *does* later land in X my guard `if (m_activeDocDock != dk)` is already
false and the refresh is skipped.

**Recommended fix (one helper, not 7 scattered edits):** add
`void MainWindow::setActiveDocDock(QDockWidget* dock)` that does `m_activeDocDock = dock; updateWindowTitle();
refreshBookmarksDock();` (`updateWindowTitle()` already chains `updateScannerTitle()` + `updateSourceChip()`),
and route all of the above sites (plus the existing direct assignments) through it. The 8352 case only needs
`updateWindowTitle()`. **Not applied** — it's a 7-site change in the `main.cpp` active-doc/chrome area and a
design choice (centralize active-doc switching) that's yours to make, not an unsupervised sweep. The find-dialog
sites (7766/7613/7834) also have a focus-revert wrinkle: after `dlg.accept()` focus returns to the *old* dock and
`focusChanged` (main.cpp:1271) can revert `m_activeDocDock` back — so those need `setFocus` on the target dock or
a guarded assignment, not just an inline refresh. **Verifiable when authorized:** `tests/test_doc_tab_chrome.cpp`
already drives a real `MainWindow` (`QTest::qWaitForWindowExposed` + real tabify/click), so a regression test
asserting `win->windowTitle()` / the source chip after an active-doc switch is feasible there (real-window
QtTest, not headless-unit). Full per-site repros + evidence are in the review workflow result
(`tasks/wa2n0xvci.output`).

---

## Test note: `test_scanner` self-attach failure (NOT a regression)

`TestScanner::selfAttach_findMutateRevalidate` failed 5/5 this session ("scan didn't return the planted heap
address"). Confirmed **environmental, not anyone's code change**: `git diff --name-only HEAD` shows no
scan-engine / provider / `test_scanner.cpp` file is modified vs HEAD — the entire scan subsystem and its test
are byte-for-byte committed state. This session's changes only execute under `hasByteSelection()`, never in a
scanner test. It's the documented self-attach flake (memory `reference_ci_flake_cursor_hover`), currently
hard-failing on this machine/session. Per standing guidance, not chased. All other 56 suites green.

---

## State / next steps

- **Committed + pushed** (code-only, IChooseYou, no co-authors): `61aaf0a` (bugs 1-4) + `9f39e36` (bug 5 / CI).
  **CI green**, release `Snapshot 16-06-2026` published. Docs left uncommitted by design. Bugs 3 & 4 were
  applied under the standing autonomous directive (one-line, pattern-matched, suite-verified) — revert if
  you'd rather either had waited.
- Awaiting your call on: (a) the **`setActiveDocDock` helper** to fix the 7-bug stale-chrome cluster above
  (verifiable via `test_doc_tab_chrome`'s real-window harness), (b) the tab-bar `currentChanged` follow-up for
  the source chip (the pure-tab-click lag caveat on bug 2).
- GUI smoke check worth doing: byte-select across several rows, edit + Ctrl+Z → bottom row's grey band now
  tracks the purple highlight; switch doc tabs → source chip updates.
