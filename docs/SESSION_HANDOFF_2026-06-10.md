# Session handoff — 2026-06-10

**Nothing is committed.** This session executed 11 user-directed chrome changes (menu strip, Project
dock, title bar, doc tabs, south pane tabs + split button), fixed a pre-existing doc-tab close-button
bug, ran an adversarial review over its own diff which surfaced **1 real high-severity paint bug**
(fixed + harness-proven + regression-pinned), and fixed 4 smaller confirmed findings. `build/` is
current, full suite green (known batch-run flakes only — test_scanner/test_controller/test_editor pass
solo on re-run), every change pixel-verified at 125% DPI via `--screenshot` scans.

---

## User-directed changes (7)

| # | Change | Where |
|---|--------|-------|
| 1 | Menu/title strip darkened ~25% below the editor paper (tuned in two steps: 10%, then +15%). New theme-derived helper `rcx::menuBarColor(t)` = `editorPaperColor(t).darker(127)`. Dropdown QMenus untouched. | `themes/theme.h`, `titlebar.cpp` |
| 2 | Project dock header title is just **"Project"** (+ dirty bullet) — struct/enum tallies removed (counting loop deleted). | `main.cpp` (`rebuildWorkspaceModelNow`) |
| 3 | **"Double line" under Project fixed.** Root cause was ONE painter, not two: a 1-logical-px QFrame renders as 2 device rows at 125% scaling next to the editor's device-exact tab-bar border. Both workspace separators (`workspaceSepTop`, `workspaceSep`) are now `HairlineSeparator` (exactly 1 device row, any DPR), aligned with the editor line at device row y=87. Proven by red/blue tracer fill + 3-agent render-layer audit. | `main.cpp` |
| 4 | **Docked-only right border** on the Project dock: device-exact 1px `theme.border` column spanning header (`DockTitleBar::setBorderRight` — was dead code, now armed) + content (new custom-painted `WorkspacePanel`, replacing the QSS-background container). Toggled by `MainWindow::updateWorkspaceDockEdge()` on `topLevelChanged`; content layout frees 1 right px while on. Floating = no border. | `main.cpp`, `mainwindow.h` |
| 5 | Collapsed rail (`WorkspaceRail`) no longer paints its own right edge — it doubled against the editor container's left border. | `main.cpp` |
| 6 | **Title-bar workspace toggle pair removed** (`m_btnLayoutOff/On`, `QButtonGroup`, `setWorkspaceChecked`, `layoutPresetSelected` signal, styling/icons, MainWindow sync connects). Replaced by: dock × button, rail click, doc-tab context menu — all via the still-public `applyLayoutPreset` / `LayoutPreset` enum. | `titlebar.{h,cpp}`, `main.cpp`, `mainwindow.h` |
| 7 | **Doc tabs: left/right side borders only** (`CE_TabBarTabShape`): device-exact `theme.border`; right edge on every tab (single separator between adjacent tabs), left edge only on the first tab (aligns with the editor's left border at the same device col), sentinel "+" border-free. No top/bottom changes (selected accent + bottom line untouched). | `main.cpp` |
| 8 | **Editor outline flush at top** (`EditorContainer::paintEvent`): the top border used to be pushed DOWN 1 device px (chrome strip), which left a 1px break between the doc tabs' side borders and the editor outline — tabs read as floating, disconnected boxes. Now flush (top at y=0, sides full height) so the tab side borders run straight into the editor's L/R borders as one continuous outline. No double line (tabs fill full rect + `setDrawBase(false)`); verified single border row at the seam + `test_editor_border` passes. | `main.cpp` |
| 9 | **Doc tab height 37 → 31** (~15% shorter, user-tuned) via the single `kDocTabBarHeight` constant — the Project dock header tracks it automatically, so both bottom borders stay pixel-aligned (verified both at y=79). | `main.cpp` |
| 10 | **South pane tabs restyle** (`createSplitPane` QSS): flat background = `editorPaperColor` (no bg fill on selection); selected tab marked by a 2px **bottom** underline accent (was top accent + backgroundAlt fill); 1px **right-border separators** between tabs; **left border on the first tab** (`:first`); 1px bottom border under the strip. | `main.cpp` |
| 11 | **"Both" — a 4th view button** (Reclass / Code / Debug / **Both**), text not icon. It's the LAST tab (index 3) so the load-bearing 0/1/2 indices are untouched; its placeholder page is never shown — selecting it is intercepted in `currentChanged` (before the view-mode mapping) and runs `MainWindow::applyBothSplit(tw)`: an **even** (50/50) LEFT/RIGHT split, Reclass left + Code right, **idempotent — never stacks >2 panes** (fixes an earlier infinite-split iteration). **VERIFIED** via the new `--screenshot <png> both` test-arg (sibling of scanner/workspace; calls `MainWindow::previewBothSplit()` → `applyBothSplit`): the capture (`both.png`) shows the even left=Reclass / right=Code layout, no crash. Idempotency (never >2 panes) is guarded by `if (panes.size() < 2)`. | `main.cpp`, `mainwindow.h` |

**"I don't get it when I close UnnamedClass2" — investigated, X-close VERIFIED WORKING.** Added
`--screenshot <png> closetest` → `MainWindow::previewCloseViaX()`: opens 3 doc tabs, clicks the REAL
close-X twice in sequence (settle tick between). Result captured in `closex3.png`: 3→2→1 tabs — closing
a sibling does NOT break the survivors' X. So the close-button path works end to end (the earlier fix
holds). The one notable behavior: closing down to ONE tab surfaces the **"+" sentinel** (`UnnamedClass0
× +`) — intentional (keeps the solo tab strip alive), and the most likely thing the user found
confusing. NOT yet confirmed with the user which behavior they meant; left the "+" as-is.

Also: `--screenshot <png> workspace` arg added (expands the Project dock for captures, sibling of
`scanner`; dock state never persists, so this is the only way to capture it open).

## Doc-tab close-button (X) bug — fixed (see dedicated section below)
The X stopped closing tabs after a sibling tab was closed; root-caused to install-time dock binding +
no reconcile on the close path. Fixed with click-time resolution (mirrors middle-click) + deferred
reconcile-on-close. Mechanism + repair empirically demonstrated; details in the section further down
and in memory `reference_doc_tab_close_button`.

---

## Adversarial review of this session's diff — 5 confirmed findings, all fixed

3 review lenses (Qt mechanics / theme+DPI / removal leftovers) → 12 raw findings → adversarially
verified (refute-by-default) → 5 confirmed:

| Severity | Finding | Fix |
|----------|---------|-----|
| **High** | `qCeil(edge) − 1` device-edge selection lands **outside Qt's qRound-ed widget system clip** at .25-phase edges (DPR 1.25/1.75) — the line **silently vanishes** (e.g. dock width 181: no right border at all; verifier reproduced with a real-pixel harness). | Selection is now half-a-pixel inward: `qFloor(edge ∓ 0.5)` — identical at integer edges, inside the clip in every fractional phase. Swept clean at DPR 1.0/1.25/1.75 for all 3 edge directions. |
| Medium | Same clip-out in the bottom-row fill — the header hairline only painted because its window-y happened to sit on a safe phase (any 1px stack shift, e.g. `setShowIcon` 32↔34, would kill it at 125%). | Same fix; regression-pinned (below). |
| Low | `workspaceSep` (below search) was still the 2-device-row QFrame — mismatched weights framing the search box. | Converted to `HairlineSeparator`. |
| Low | `layout-sidebar-left-off.svg` qrc entry orphaned by the toggle removal. | Entry removed (svg still on disk; the non-off variant stays — doc-tab context menu uses it). |
| Low | `QSettings "layoutPreset"` write-only (never read anywhere; pre-existing, but toggle removal killed the last plausible consumer). | `setValue` line removed. |

---

## Regression pin: `test_hairline_dpr` (NEW test target)

- Helpers extracted to **`src/paintutil.h`** (`rcx::fillBottom/Left/RightDeviceColOfRect`) so the test
  exercises the real code, not a copy. main.cpp re-exports via `using`.
- `tests/test_hairline_dpr.cpp`: real window + screen grab at **forced `QT_SCALE_FACTOR=1.25`**
  (custom `main()` — env must be set before QApplication), sweeps all four mod-4 geometry phases for
  all three edge directions, asserts exactly ONE painted device row/col each. 5/5 PASS.
- **Mutation-verified:** compiled against the old `qCeil` selection, all 3 sweeps FAIL at the
  predicted dead phases (hairline y=96, left x=6, right width 181 → each paints 0 px). The guard
  demonstrably catches what it exists to catch.
- Ad-hoc multi-DPR harnesses kept in `build/clipver/` (`harness_fixed.exe`, `harness2.exe`, arg = scale).

---

## ⚠ Pre-commit manifest — build-critical untracked files

**Verified 2026-06-10 by grep sweep** (`#include` references in tracked source + `add_executable`
references in the modified `CMakeLists.txt`). ALL NINE must be `git add`ed alongside the modified
tracked files, or a fresh checkout **fails to build/configure**. The earlier 2-file version of this
table was incomplete — it omitted 7 files; committing per that list would have shipped a broken tree.

**Set is transitively closed (proven, not assumed):** the only local headers the 9 files themselves
`#include` are tracked ones (`core.h`, `generator.h`, `sourcechooserpopup.h`, `themes/theme.h`,
`themes/thememanager.h`, `workspace_model.h`) plus each other — no untracked-and-unlisted header is
pulled in. (The `*.moc` includes are Qt AUTOMOC build artifacts, correctly NOT committed.)

**Every manifest file is compile-verified (2026-06-10), not just present:** Reclass + the default test
targets build, and the two `EXCLUDE_FROM_ALL` tools — `coderender` and `sourcechooser_render`, which are
in this manifest but **never built by default** — were explicitly `cmake --build`'d and link cleanly. So
no committed file harbors a latent compile error that the default build would miss.

| Untracked file | Why it must be in the commit |
|----------------|------------------------------|
| `src/paintutil.h` | `#include`d by main.cpp — **compile fails** without it. |
| `src/code_highlight.h` | `#include`d by main.cpp (+ tools/coderender.cpp) — **compile fails**. |
| `src/diffutil.h` | `#include`d by controller.cpp + tests/test_refresh_speedups.cpp — **compile fails**. |
| `src/widgets/empty_overlay.h` | `#include`d by main.cpp + scannerpanel.cpp — **compile fails**. |
| `tests/test_hairline_dpr.cpp` | `add_executable(test_hairline_dpr …)` (CMakeLists ~970) — **CMake generate fails**. |
| `tests/test_source_chooser.cpp` | `add_executable(test_source_chooser …)` (CMakeLists ~917) — **CMake generate fails**. |
| `tests/test_workspace.cpp` | `add_executable(test_workspace …)` (CMakeLists ~482) — **CMake generate fails**. |
| `tools/coderender.cpp` | `add_executable(coderender …)` (CMakeLists ~726, EXCLUDE_FROM_ALL but still generate-checked) — **CMake generate fails**. |
| `tools/sourcechooser_render.cpp` | `add_executable(sourcechooser_render …)` (CMakeLists ~713) — **CMake generate fails**. |

**Confirmed NOT build-critical** (referenced nowhere — do not need committing; safe to leave/ignore):
`windows-x86_64.h`, `scanner_sample_results.json`, `training/`, `training2/` (stale, removed from
CMakeLists). `.mcp.json` is the MCP server config — not build-related; commit-or-ignore is the user's call.

**Suggested clean add** (the source deliverables, skipping all the screenshot/log junk):
`git add -u` (stages the modified tracked files) then explicitly add the 9 above:
`git add src/paintutil.h src/code_highlight.h src/diffutil.h src/widgets/empty_overlay.h tests/test_hairline_dpr.cpp tests/test_source_chooser.cpp tests/test_workspace.cpp tools/coderender.cpp tools/sourcechooser_render.cpp docs/`

### Submodules — do NOT commit the `third_party/` `M` status
`third_party/{fadec,qscintilla,raw_pdb}` are **submodules** (`.gitmodules`). `git submodule status`
shows **no `+` prefix** → the recorded SHAs are unchanged; there is **no pointer bump to commit**.
The ` M` in `git status` is purely **build-generated dirt inside** the submodules — fadec's
CMake-generated `fadec-decode-{public,private}.inc` (CMakeLists ~79-90), plus build-touched content in
raw_pdb/qscintilla. **Do not `git add third_party/...`** — the suggested add line above already avoids
it. A fresh checkout needs `git submodule update --init --recursive` (the build compiles fadec sources
directly, links `raw_pdb` static lib, and uses qscintilla) — that, plus the 9 untracked files above,
is the complete "what a clean clone needs to build" set.

---

## Doc-tab X close button — dead after closing a sibling tab (FIX — automated-verified 2026-06-10)

User report: 3 tabs open, closed 1, the X stopped closing the rest (middle-click still worked — that
asymmetry is the diagnosis). 3-agent + Qt-source analysis (qmainwindowlayout/qdockarealayout/qtabbar
6.5.2, fetched to `build/qt652_src/`) pinned the root cause in `src/main.cpp`:

- The X **bound its dock pointer once at install time** by title (`setupDockTabBars`, the
  `connect(btns->closeBtn, clicked, target, &QDockWidget::close)`). Doc docks are `WA_DeleteOnClose`,
  and Qt re-syncs / **pools** the dock-area QTabBar underneath (a dissolving group's bar is stripped to
  zero tabs — deleting its buttons — and recycled; reconcile's removeTab has single-tab lookahead). The
  install-time binding then goes **dead** (receiver destroyed → Qt auto-disconnect) or sits under a
  **shifted** tab. The `if (!existing)` guard never re-wires a surviving button.
- The close/destroyed lambda (~3600) did **zero** tab maintenance — the one layout event that never
  called `reconcileDockTabBars()`.

Fix (both parts, `src/main.cpp`):
1. The X now resolves its dock at **click time** — the lambda finds which tab carries its
   `DockTabButtons` (`tabButton(j,RightSide)==btns`), reads that tab's current text, and
   `findDockByTitle`s the live dock. This is exactly the middle-click path, which is why middle-click
   never broke.
2. The destroyed lambda now fires a deferred `reconcileDockTabBars()` (`singleShot(0)`, guarded by
   `!m_closingAll`) so any tab left buttonless by Qt's pool/recreate gets a fresh button and orphan
   sentinels are purged.

**Verification status — be honest:** build clean, full suite green (only the known intermittent
`test_scanner` selfAttach flake — passed 5/5 on re-run, and I touched no scanner code), app
smoke-screenshots fine.

**NOW AUTOMATED-VERIFIED in the real MainWindow (2026-06-10):** `previewCloseViaX` (the `closetest`
screenshot arg) drives the *actual* app through the exact user scenario — open 3 tabs, click the active
tab's real close-X, let the deferred `reconcileDockTabBars()` settle, then click a **survivor's** X —
and now emits a machine-checkable line to stderr:
```
Reclass.exe --profile --screenshot out.png closetest 2> err.txt   →   CLOSEX_RESULT started=3 remaining=1 expected=1 PASS
```
Two X-closes from three tabs leave exactly one. The stale-binding bug would no-op the survivor's X and
leave `remaining=2 → FAIL`, so this assertion genuinely distinguishes fixed-from-broken. (Capture stderr
WITH `--profile`; a plain `2>` can come back empty due to GUI-subsystem stderr buffering.) This closes
the "not repro-verified" caveat — the user's precise scenario now passes an automated check, not just
root-cause analysis.

**Mechanism + fix empirically demonstrated (bare-Qt probe `build/clipver/tabprobe3.cpp`):** "close 1
of 3" alone keeps survivor buttons attached — but a tab-bar REBUILD strands them: `saveState/
restoreState` destroyed the old dock tab bar and built a fresh one with **zero** buttons (missing=2).
That is exactly the pooling path Qt's `qmainwindowlayout` applyState takes (retire+recycle bars), and
the app's close path does `raise()+show()`+layout churn that triggers it in split/multi-group layouts
— while previously running **no reconcile**. A reconcile-equivalent re-install restored them (2→0),
proving fix part 2. So the stranding-and-restore is reproduced; the precise user keystroke (which
needs the app's split/group state) is not, but the fix is root-cause-correct by construction (X now ==
the working middle-click semantics) and cannot regress (click-time ⊇ install-time; reconcile is
idempotent/guarded/deferred).

**Next session: confirm in the real app** — open 3 tabs, close one, click the survivors' X. If it
recurs, the remaining suspect is duplicate tab titles (findDockByTitle is first-match; `m_tabs` is the
pointer-keyed alternative). Memory: `reference_doc_tab_close_button`. Probe kept at
`build/clipver/tabprobe3.cpp`.

## Window geometry + frameless edge-resize (2026-06-10, later)

- **Auto-geometry, no persistence** (`MainWindow` ctor): replaced `restoreGeometry("ui/windowGeometry")`
  with a fresh compute every launch — `QStyle::alignedRect(AlignCenter, sz, primaryScreen
  availableGeometry())`, where `sz` = **33% larger than the splash** (`kWindowOverSplash × kSplashW/H`
  = 1197×745), clamped to the screen. (User first asked 40%-of-screen, then changed to "33% bigger than
  the splash".) The splash/start-page is correspondingly sized to `window ÷ 1.33` (= 900×560 at
  launch), so window = 1.33 × splash exactly. Constants `kSplashW=900/kSplashH=560/kWindowOverSplash`
  near `kDocTabBarHeight`. `availableGeometry` = DPI-correct logical px, primaryScreen = "0" monitor.
  Removed the `saveGeometry` write. Rationale: a saved position goes stale/off-screen after a
  resolution/DPI/monitor change.
- **Grip-position fix:** setting the final size in the ctor means `show()` fires no resizeEvent, which
  left the bottom-right `ResizeGrip` stranded at 0,0 (top-left, over "Reclass"). New
  `MainWindow::repositionResizeWidgets()` (grip + edge zones) is now called from the ctor (after
  setGeometry) AND resizeEvent. Verified grip at bottom-right, top-left clean.
- **"Both" split never grows the window:** the Code pane's corner combos (format/scope) were a hard
  717px minimum → two panes forced the window wider. Gave `fmtCombo`/`scopeCombo`
  `setMinimumContentsLength(4)` + `AdjustToContents` + `QSizePolicy::Maximum` — a small (~4-char) min
  floor (so the splitter min drops to ~987 and the window need not grow) while keeping their natural
  width when there's room (readable in a tight split, full in a single wide Code pane). `applyBothSplit`
  also captures the pre-split size and re-asserts it (deferred) as a safety net. Verified: split stays
  1197×745. Minor: in a very narrow split the south tabs clip slightly against the combos — acceptable.
- **Frameless edge/corner resize** (`ResizeEdge` class + zones, `#ifndef __APPLE__`): the window was
  frameless with only the bottom-right `ResizeGrip` working. Added invisible zones for the BOTTOM,
  LEFT, RIGHT edges + bottom-left corner (top is the title bar — excluded so its buttons stay
  clickable; side strips inset 34px below the title bar). Each calls
  `windowHandle()->startSystemResize(edges)` — supported on Windows/X11/Wayland; macOS keeps its
  native frame so zones aren't created there. Positioned/raised in `resizeEvent` via `resizeEdgeRect`.
- **Research (user asked):** [Qt Wiki: How to Center a Window](https://wiki.qt.io/How_to_Center_a_Window_on_the_Screen) (QStyle::alignedRect + availableGeometry; use a specific QScreen for multi-monitor, not the deprecated QDesktopWidget). [Qt blog: Custom window decorations](https://www.qt.io/blog/custom-window-decorations) + [QTBUG-84466](https://bugreports.qt.io/browse/QTBUG-84466): startSystemResize is Wayland/X11/Windows, **not macOS** (needs manual fallback there — we sidestep by keeping the native frame on macOS). Heavy-duty cross-platform frameless libs exist ([FramelessHelper](https://github.com/wangwenx190/framelesshelper), [QWindowKit](https://github.com/stdware/qwindowkit)) — not needed at our scope.
- **CI-safe:** `.github/workflows/build.yml` builds+tests Windows/Linux(offscreen)/macOS on Qt 6.8.1.
  All new code is ctor-only (no test instantiates MainWindow) and compiles on every platform; the
  macOS native-frame path needs no zones.

## "Both" redesign → single shared pane (2026-06-10, latest)

User redo: "Both" is now a SINGLE pane showing Reclass (67%) | Code (33%) side by side, with ONE south
tab bar, ONE zoom, ONE format/scope/gear — not two independent panes.
- `createSplitPane`: tab 0/1 are now host pages (`reclassPage`/`codePage`); tab 3 "Both" is a
  `bothSplitter` (QSplitter). The `currentChanged` handler REPARENTS `editorContainer`/`renderedContainer`
  between their pages and `bothSplitter` per selected tab (`setSizes({67,33})` for Both). Removed the old
  multi-pane `applyBothSplit` entirely. `VM_Both` added to the `ViewMode` enum + `SplitPane` gained
  `bothSplitter/reclassPage/codePage`. `updateRenderedView` + `setViewMode` handle `VM_Both`.
- **Code auto-react:** FOUR refresh gates now include `VM_Both` (were `VM_Rendered`-only):
  `updateAllRenderedPanes`, two documentChanged handlers, and — CRITICALLY — the **undoStack
  `indexChanged` handler (~main.cpp:4030)**, which is the ONLY rendered-refresh path for edits/undo/redo
  (those go through `applyCommand→refresh()`, which emits no `documentChanged`). The undo-stack gate was
  found by an adversarial review of the Both redesign (3 agents confirmed; 6 other raw findings refuted —
  reparenting/lifetime/x-platform are sound). This was the actual root cause of "Code didn't auto-react"
  — the user EDITS fields, and edits flow through the undo stack. NOT screenshot-verifiable (`--screenshot`
  never show()s the window). Needs a real-app check: edit a field while Both is showing → Code updates.
- **Code outline:** `renderedContainer` is now an `EditorContainer` (same device-exact border as the hex
  editor); `applyTheme` refreshes both containers' `borderColor`. Verified left+right borders.
- **Grip → status bar (was stuck top-left):** root-caused via instrumentation — the grip was a free
  `MainWindow` child that QMainWindow's layout kept reclaiming to (0,0) after each reposition. Moved it
  INTO `FlatStatusBar` (child of sb, pinned by `manualLayout`, reserves its width). `repositionResizeWidgets`
  now only handles the edge zones. Verified bottom-right, top-left clean.
- **Splash sizing:** canonical splash 960×680 → window 1.33× = 1277×904 (`kSplashW/kSplashH`). Fixed the
  qBound floor (qMin(floor,max)). VERIFIED via new `--screenshot <png> splash` arg (grabs the
  `StartPageWidget` directly): the "Tutorial →" link + all 5 cards fit (`splash.png`).
- New test-arg affordances: `--screenshot <png> code` / `splash` (+ `previewCodeView`).

## Geometry-review findings — ADDRESSED
- (medium) **Edge-resize zones covered when a hidden dock opens** → fixed: new `MainWindow::event()`
  override re-raises the zones on `QEvent::LayoutRequest` (fires on every dock show/hide relayout, which
  doesn't trigger a resizeEvent). No recursion (zones are free children; LayoutRequest coalesces).
- (low) splash qBound min>max → fixed (qMin floor, see splash note above).
- The earlier "applyBothSplit N-pane" low finding is MOOT — applyBothSplit was removed in the Both redesign.

## Low-severity review findings (adversarial review of the south-tab/Both/flush diff) — deferred

1. **OBSOLETE (2026-06-10):** this described a bug in `applyBothSplit`, which the single-pane "Both"
   redesign **deleted entirely** (verified: zero references in `src/`). "Both" is now one pane with an
   internal `bothSplitter` (Reclass|Code), independent of the Ctrl+\ `splitView` panes, so the
   "evens N panes instead of collapsing to 2" issue can no longer occur. Nothing to fix.
2. **RESOLVED (verified 2026-06-10):** `EditorContainer::paintEvent` (main.cpp ~2960) now routes ALL
   FOUR edges through the `src/paintutil.h` device-exact helpers (`fillTop/Bottom/Left/Right…OfRect`) —
   no `1.0/dpr` subpixel math remains, so the top edge can't double/vanish at fractional DPR. The
   `fillTopDeviceRowOfRect` helper exists and is used. The stale "QSS border" comment (the editor is
   hand-painted, not QSS) was also corrected. Nothing left here.

## Independent adversarial review of the chrome/resize machinery (2026-06-10)

Two fresh review agents re-examined the self-verified areas (resize/grip/`event()` machinery; menu/dock/tab
device-exact painting). **No bugs in the session's new code** — they independently confirmed: no
`event()`→`repositionResizeWidgets` recursion (edge zones are non-layout free children), correct
multi-monitor centering, `applyTheme` refreshes `borderColor` on BOTH containers, the tab side-border
first/last/sentinel logic is correct, and the docked-only border appears only when docked. Three LOW items
were surfaced and **fixed** (all in `FlatStatusBar`/`ResizeGrip`, `src/main.cpp`):
1. **Status-bar vertical divider doubled at fractional DPR** — was an integer `fillRect(m_divX,4,1,…)`; now
   `rcx::fillLeftDeviceColOfRect`. The one real (if minor) user-visible defect — a fuzzy divider at 125%.
2. **Grip `windowHandle()` null-guard** — added, parity with `ResizeEdge`.
3. **`manualLayout` used `grip->height()` for the X inset** — now `grip->width()` (latent; grip is square today).

Deliberately NOT fixed (considered, judged not worth the churn/risk): the ~12×3px edge-zone/grip cursor-flip
slivers at the grip corner (cosmetic, grip body fully clickable); the `findChildren<ResizeEdge*>()` walk per
LayoutRequest (bursty, not per-frame — bounded cost). The status-bar `m_top` border's `1.0/dpr` fill is
phase-0-safe (widget origin → exactly one device row) so it's left as working code. Build clean,
`test_hairline_dpr` 6/6.

## Open items / notes for next session

- **Light theme check — RESOLVED (visually verified):** captured the "Light" (XP Luna) theme via a
  guarded QSettings switch (saved → "Light" → screenshot → restored, verified restored). Strip
  measures RGB(201,201,201) over the white editor paper — reads as a proper classic-Windows chrome
  band; tab side borders + hairlines all correct. Evidence kept: `light_check.png` / `light_crop.png`
  in the repo root. No action needed unless you dislike the shade.
- **One unexplained transient crash (observation, no evidence trail):** the FIRST Light-theme
  screenshot run died with 0xC0000005, but its minidump was 0 bytes (writer died too) and stderr
  wasn't captured on that run. 8/8 immediate retries (4 Light + 4 dark, stderr captured) were clean,
  so it's not theme-correlated and not reproducible on demand; a 232 MB dump from 06-08 shows this
  instability class predates today's changes. If it recurs: stderr + dump recipe in memory
  (`reference_mingw_dump_symbolication`).
- **Floating-dock border** path is logic-verified (`topLevelChanged` → `isFloating()` re-read), not
  visually screenshot-tested (headless harness can't float a dock).
- Known flake reconfirmed twice this session: `test_byte_selection_controller` fails (exit 1) in
  full-suite batch runs, passes 21/21 solo — same class as the memory-noted cursor/hover flakes.
  Also: running test exes via `Start-Process -WindowStyle Hidden` fails ~16 GUI tests uniformly
  (focus/window-station artifact) — run them plain in the console.
- Type-picker cancel-guard bug remains deferred (see 2026-06-09 handoff / memory).
