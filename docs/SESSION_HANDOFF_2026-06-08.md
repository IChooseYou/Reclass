# Session handoff — 2026-06-08

**Nothing is committed.** This session fixed (1) a crash and (2) the DayZ-attach lag, both
interwoven with your large in-flight uncommitted `modulesCached`/RTTI feature (`compose.cpp`,
`rtti.cpp`, `controller.cpp`, `main.cpp`, `provider.h`, …). Committing was deliberately **not** done
autonomously — see "Open decisions" for why and how to land it. `build/` is current, ABI-consistent,
and runs cleanly.

---

## The crash

Reported repro: click a process source in the picker → `0xC0000005` access violation,
`lock add dword ptr [rdx],1` (a refcount bump) on a near-null pointer inside `rttiForVtable`.
Full minidump: `build/reclass_crash_20260608_091351.dmp`.

**Root cause — fragile-base-class ABI break against a stale plugin DLL.** Diagnosed from the dump
(CDB `.ecxr;k`, then MinGW `nm`/`addr2line` on `0x140000000 + RVA` — see the new
`reference_mingw_dump_symbolication` memory):

- The crashing object had a vtable into `libProcessMemoryPlugin.dll` with garbage members; the
  faulting value was a corrupt `QVector<Provider::ModuleEntry>` (`d = 0x854`).
- Stack: `SourceChooserPopup::providerSelected` → `RcxController::selectSource` → `refresh()` →
  `compose()` → `composeParent` → `composeLeaf` → `rttiForVtable()` → `prov.modulesCached()`.
- Timeline: your `provider.h` change at **08:16** added two data members to base `Provider`
  (`m_moduleCache`, `m_moduleCacheValid`), growing `sizeof(Provider)` 8→40. `Reclass.exe` was rebuilt
  at **08:20** (new layout) but `libProcessMemoryPlugin.dll` was last built at **06:40** (old layout,
  from a partial `--target Reclass` build). `modulesCached()` then read the new base members where
  the stale plugin stored `ProcessMemoryProvider`'s own data → garbage `QVector` → AV on first touch.

---

## Fix (3 layers)

| File(s) | Change |
|---------|--------|
| `CMakeLists.txt` | `add_dependencies(Reclass <5 provider plugins>)` — building the app now ALWAYS rebuilds the plugins, so a partial `--target Reclass` build can never leave an ABI-stale plugin behind (the exact cause). |
| `src/iplugin.h`, `src/pluginmanager.cpp`, 5× `*Plugin.cpp` | **Provider ABI guard.** Each provider plugin exports `RcxPluginAbiToken()` = `(RCX_PROVIDER_ABI_VERSION << 32) \| sizeof(rcx::Provider)`. `PluginManager::LoadPlugin` (after construction, gated on `Type()==ProviderPlugin`) rejects a missing/mismatched token gracefully with a "rebuild the plugin" log and loads the rest — turning a silent memory-corruption crash into a diagnosable refusal. Low 32 bits auto-catch data-member changes; high 32 bits (hand-bump on Provider **vtable** changes) catch what `sizeof` can't. |
| `src/providers/provider.h` | Hazard comment documenting the fragile base class next to the cache members. |

**Files touched (9):** clean (only my changes) — `iplugin.h`, `pluginmanager.cpp`, `KernelMemoryPlugin.cpp`,
`RcNetCompatPlugin.cpp`, `RemoteProcessMemoryPlugin.cpp`, `WinDbgMemoryPlugin.cpp`. Mixed with your
pre-existing WIP — `CMakeLists.txt`, `provider.h`, `ProcessMemoryPlugin.cpp`.

---

## Verification status

- **Full build ×2** (388 targets) — both `exit 0`, no errors/undefined refs (re-run after the token change).
- **Deterministic test sweep** — 54/56 pass. The 2 (`test_controller`, `test_scanner`) are the documented
  flakes (`reference_ci_flake_cursor_hover`); re-ran each serially ×2 → clean pass (rc=0). My changes can't
  touch controller/scanner logic.
- **ABI guard, end-to-end** (`QT_FORCE_STDERR_LOGGING=1 Reclass.exe --screenshot`):
  - positive → `Loaded 5 plugin(s)`, no crash (token `0x100000028` = ver 1, size 40, host↔plugin agree);
  - negative (deliberately wrong token, reverted) → rejected + `Loaded 4`, no crash.
- Regression: `test_provider/compose/rtti/rtti_hint/core` green.

## Memory written
- `reference_provider_fragile_base_abi`, `reference_mingw_dump_symbolication` (both indexed in `MEMORY.md`,
  under new "Plugins" section + "Crash Debugging").

---

## Performance — "DayZ_x64.exe barely usable"

Separate from the crash. After attaching to DayZ the view was barely usable. **Root cause was a
per-refresh-tick full address-space sweep**, not RTTI (RTTI was already off in your registry).

| File(s) | Change | Effect |
|---------|--------|--------|
| `controller.cpp`, `controller.h` | **Cache `enumerateRegions()` in `classifyPermanentPages`.** It was calling `provider->enumerateRegions()` — a full `VirtualQueryEx` sweep of DayZ's entire address space — **every refresh tick**. Now tick-gated: re-enumerated only every `kRegionRefreshTicks` (64) ticks, stored in `m_classifyRegions`, invalidated in `resetSnapshot()`. | **The main win.** Kills the per-tick syscall storm that scaled with DayZ's huge region count. |
| `main.cpp`, `controller.h` | **Auto-RTTI default OFF**, gated behind View → *"Auto-detect RTTI … (scans vtables, can lag)"*. `m_showRtti = false`; `createTab` reads `showRttiChips` (default false). Tutorial tab stays ON intentionally. | Stops per-pointer vtable RTTI walks unless the user opts in. |
| `compose.cpp` | **Gate `inferTypes`** behind default-off `state.typeHints` (`if (state.typeHints && isHexNode(...))`); was an ungated per-hex-node scan every compose. | Removes a per-node cost from every refresh. |

**Hot-path audit (this session, confirming no residual per-tick big-process cost):**
- `enumerateRegions` — now cached (above).
- `enumerateModules` — not called while RTTI is off; `modulesCached()` is lazy/cached anyway.
- `inferTypes` — gated off.
- `getSymbol(pv)` per pointer — `SymbolStore::getSymbolForAddress` early-outs `{}` when no symbols are
  loaded (`m_modules.isEmpty()`), so a bare attach pays nothing; only a linear scan + binary search when
  the user has explicitly loaded PDBs.
- Remaining DayZ-scaling cost is the **one-time** `EnumProcessModulesEx` + per-module info in the
  `ProcessMemoryProvider` constructor (at attach), not per-tick — acceptable.

**Verification:** build `exit 0`, `Loaded 5 plugin(s)`, `test_refresh_speedups`/`test_compose`/`test_core`/
`test_rtti_hint` green; `test_controller`/`test_scanner` are the documented `reference_ci_flake_cursor_hover`
flakes (serial re-run clean). **Awaiting your DayZ re-test** — if residual lag remains, `Reclass.exe --profile`
→ attach → View → Performance Profiler will name the hot `PROFILE_SCOPE`.

---

## More this session (all uncommitted, all verified — full deterministic suite green)

UI/UX + perf work layered on top of the crash + DayZ fixes. Memory entries written for the durable ones.

| Area | Change | Files |
|------|--------|-------|
| **Console flash** | App is now **GUI-subsystem** (`WIN32_EXECUTABLE`) → no console window/flash on launch; console is on-demand via `AllocConsole` behind View ▸ Show Console; stderr still reaches terminals/pipes (so `--screenshot`/`--profile` unaffected). Don't revert to console-subsystem. **Two safety hardenings** (review-caught + audit): `freopen` skips a stream already redirected to a pipe/file (so a redirected `--profile` dump isn't stolen into the console); the AllocConsole'd console's **[X] is disabled** (closing it would `CTRL_CLOSE_EVENT`-terminate the app). | `CMakeLists.txt`, `main.cpp` · memory `project_gui_subsystem_console` |
| **Startup** | Bookmarks dock content **deferred off the ctor** — `MainWindow::ctor` 327ms → ~97ms (the cost was the first stylesheet'd-widget QSS polish at `setWidget`). Added headless `--profile --screenshot` profiler dump (`Profiler::dumpToStderr`). | `main.cpp`, `mainwindow.h`, `profiler.{h,cpp}` · memory `reference_startup_profiling_qss_coldstart` |
| **Break into Class** | Promoted to the **top** of the context menu for **byte** selections (out of the `Selected bytes ▸` submenu) AND **multi-node** selections; node "Break Class" suppressed during a byte selection. **Embedded-field guard:** `nodeInView()` refuses to break a field shown *inside* an embedded class (its offset is relative to that class's def → would mangle the wrong outer field) with a clear hint. | `controller.cpp`, `controller.h` + `test_context_menu` (placement, `nodeInView`, multi-node union tests) |
| **QSet-order scrambles (×3)** | Sort by offset before user-visible output: **Copy Address**, **Ctrl+C Copy Nodes** (`selectedRootIds`), and the **contiguous-merge** detection (hex toolbar). | `controller.cpp` |
| **"(Name class…)" CTA** | Gated on `Provider::isLive()` so the null-pointer CTA never sprouts on a flat **file** source. | `compose.cpp` + `test_overlay_null_rtti`/`test_overlay_widget` (live provider) ; `test_chips` opts into `typeHints` (now default-off) |
| **Workspace rail** | Thin fixed-width left **dock** that reserves a column when the workspace is hidden — discoverable handle to re-open it; reactive "Show/Hide Workspace" on the document-tab right-click. Chevron + centered "PROJECT" label. | `main.cpp` (`WorkspaceRail`), `mainwindow.h` |
| **Panel colors** | **All four side panels** — Workspace, Bookmarks, Symbols (`UnifiedSymbolPanel` list+search), Scanner (`ScannerPanel` results table) — now use the **editor "paper"** surface (`#151515`) for their content, instead of the lighter chrome `background` (`#181818`) that read as jarring beside the editor. Interactive chrome (chips, sort/scan buttons, footers, tab bars, headers) deliberately kept. Single-source `rcx::editorPaperColor(theme)` in `theme.h`. **Also consolidated** every editor-paper surface (the ~10 inline `theme.background.darker(115)`/`background` paper computations — Code/Debug Scintilla views, syntax styles, margins, code lexer, **minimap**) onto the same helper — DRY + a **light-theme bug fix** (those views used raw `darker(115)`, which the editor.cpp comment notes produces "dirty khaki" on light themes; now they correctly go white like the main editor). **Zero change on dark themes.** | `theme.h`, `editor.cpp`, `main.cpp`, `workspace_model.h`, `widgets/unified_symbol_panel.h`, `scannerpanel.cpp`, `code_highlight.h` |

**Still gated on you:** rail look tweak (chevron/label placement); **`B`** — full support for breaking a field *inside* an embedded class (the `nodeInView` guard above is the safe interim; B would resolve the embedded offset and operate within that class's def — note it modifies a *shared* class definition); **`new-class`** reset affordance for source switches.

---

## Adversarial review (round 2) — 1 bug found + fixed, 2 LOWs deferred

A second-opinion review pass (two parallel reviewers over the break logic and the console/rail/bookmarks lifecycle) surfaced one real defect, now fixed, plus two low-impact findings left documented.

**FIXED — break-into-class corrupted nested fields.** `extractByteSelectionToNewClass` does its intersection scan in **parent-relative `n.offset`** coordinates (`controller.cpp:2345`, `rowLo = n.offset`) while the selection arrives as **root-relative** addresses. Those frames coincide only for a *direct* child of the viewed class. A **union member** (`createUnion` reparents it to `offset 0`) or an **inline-struct field** still reaches the root, so the existing `nodeInView` guard let it through, yet its raw offset resolved to the wrong region → silent wrong-field extraction. Fix mirrors the existing "refuse rather than mangle" philosophy: new **`isDirectViewFrameChild()`** guard (`controller.{h,cpp}`) rejects nested fields in both the single-node "Break Class" and multi-node "Break into Class" paths with a clear hint ("break it from that struct's own view"). Pinned by `test_context_menu::testDirectViewFrameChild_RejectsNestedField` (37/37 green). The *byte-selection* path shares the same parent-relative-frame limitation for nested fields — that, plus actually *making* nested breaks work, is the gated **`B`** feature; this fix is the safe interim refusal.

**LOW-1 (deferred) — stale `stdout`/`stderr` FILE* after hiding the console.** `rcxSetConsoleVisible(false)` calls `FreeConsole()` without re-pointing the streams an earlier show had `freopen`'d onto `CONOUT$`; later `fprintf(stderr,…)` is silently dropped until a re-show. Practical impact ~nil: only affects output that was *non-redirected* (redirected streams are never freopen'd, so they survive), i.e. output that already had nowhere visible to go in the GUI-subsystem default; minidump unaffected; re-show recovers. A correct fix must stay redirect-aware (don't clobber a `2>log` stream with NUL) — left out to avoid re-touching the hardened console path for near-zero benefit.

**LOW-2 (deferred) — rail shows alongside a tabified workspace.** The rail keys on `m_workspaceDock visibilityChanged(false)`, which Qt also fires when the dock becomes the *inactive tab* of a tab group → the 22px rail column appears next to a tab that still contains Project. Cosmetic; clicking the rail recovers. Already in the rail's documented v1 out-of-scope ("floating/right-docked workspace … not handled here").

---

## Adversarial review (round 3) — core WIP, clean

Three parallel reviewers over the in-flight `modulesCached`/RTTI + perf feature (the bulk of the uncommitted tree). **No fresh correctness defects.** Two latent/pre-existing notes, neither introduced this session, both harmless today — documented, not patched (would mean changing your WIP on paths the reviewers confirm are currently dead).

- **Region-enum cache (`m_classifyRegions`) — sound.** Verified `resetSnapshot()` clears the vector + tick counter, and *every* live-provider source change routes through it (`attachViaPlugin`/`selectSource`/`clearSources`/`removeSavedSource`); first-tick re-enumerates before use (no 64-tick mis-classify window); `MemoryRegion` is a pure value type (no dangle on provider swap); cache only touched on the main thread. **Latent note:** the *File* branch of `switchToSavedSource` (`controller.cpp:6455-6459`) doesn't call `resetSnapshot()`, so it leaves the prior process's regions cached — but `classifyPermanentPages` sits behind the `isLive()` gate and `BufferProvider::isLive()==false`, so a file source never reads them. Becomes a real bug only if a future file-backed provider returns `isLive()==true`.
- **`modulesCached()` / RTTI hot path — sound.** Cache is per-provider-instance; re-attach/source-switch recreate the `SnapshotProvider` with a fresh cache; `enumerateModules()` is no longer per-pointer (O(1) cached); RTTI-off short-circuits before any read; half-open `[base,base+size)` bounds match across `rtti.cpp`/`compose.cpp`; ABI members default-initialized; empty/zero-module target is UB-free. **Latent note:** `invalidateModuleCache()` is **dead code** (zero callers) and is on the wrong layer — compose runs against `SnapshotProvider`, which doesn't forward it; it would only help the "DLL loaded mid-session, no re-attach" case, which `ProcessMemoryProvider` (ctor-only `cacheModules()`) already didn't handle pre-cache. Either wire it through `SnapshotProvider` + a module-change trigger, or delete it — your call.
- **`compose.cpp` gating — correct.** `inferTypes` short-circuits on `state.typeHints` first (zero leaked cost when off; byte-identical when on); both null-pointer "(Name class…)" CTA sites gate the **null branch only** on `isLive()` — the resolved-RTTI display is NOT gated (the exact regression a prior iteration had is confirmed absent); `isLive()` is a cached bool (no per-node syscall); file/Null providers never sprout the CTA, live ones do, `SnapshotProvider` delegates correctly.

**Net:** the uncommitted tree has now been adversarially reviewed end-to-end (crash/ABI + perf in round 1, break logic + console/rail in round 2, core RTTI/cache/compose in round 3, source-picker lifecycle in round 4). One real bug found and fixed across all rounds (the nested-field break, above); everything else verified or documented as latent. It's in a landable state.

## Adversarial review (round 4) — source-picker lifecycle, clean

The crash-adjacent source-attach UI (`sourcechooserpopup.{cpp,h}`, `processpicker.cpp`) — newly adding a per-row × / Delete-key source delete that detaches the active source — reviewed for use-after-free / dangling / state bugs. **No defects.** Verified: `m_doc->provider` is a `std::shared_ptr<Provider>`, so swapping to `NullProvider` on delete can't free a provider another ref still holds; `removeSavedSource`/`clearSources` detach to NullProvider + `resetSnapshot()` *before* `refresh()`; in-flight `QtConcurrent` reads capture the provider **by shared_ptr copy** and are gated by an `m_refreshGen` generation counter (`resetSnapshot` bumps it → stale results discarded in `onReadComplete`); ×-button and Delete-key both route through the one safe `removeRequested(savedIndex)` handler reading the index fresh from `m_filteredEntries`; the ×-click event filter returns `true` so no stray row-activation fires; `ensureSourcePopup` `disconnect(this)`s before reconnecting so a reused popup can't stack duplicate handlers. `processpicker.cpp` is pure button QSS (out of lifecycle scope). Every uncommitted source file is now reviewed.

---

## Post-review UI work (user re-engaged on the Project panel)

After the review rounds, the user reported two Project-panel issues; both fixed in `main.cpp` (pixel-verified via `--screenshot` scans):

- **Header height misalignment** ("project title bar … 1-2-3px too short"). The Project dock's `DockTitleBar` (hardcoded 36px) didn't match the editor doc-tab height (`MenuBarStyle CT_TabBarTab` = 37px), so the separator line under "Project" sat 2px above the line under the editor tab. Fix: single shared `static constexpr int kDocTabBarHeight = 37` (file scope, above `class MenuBarStyle`); both `MenuBarStyle::sizeFromContents` and `createWorkspaceDock` read it. Header = `kDocTabBarHeight` (NOT +1 — the QTabBar's extra render pixel and the left dock starting 1px lower cancel out). Verified: both separator lines at y=87, both panels' content at y=88. Memory: `reference_project_header_tab_height_alignment`.
- **"Why project still light"** — measured: the workspace **content** was already `#151515` = the editor paper exactly; only the header strip is `#181818` (= editor tab-bar chrome, intentional). The "light" impression was the misaligned header. Hardened anyway: `createWorkspaceDock`'s **construction-time** defaults (container bg, tree stylesheet + `QPalette::Base`, search box) now use `rcx::editorPaperColor(t)` — matching the runtime `applyTheme` values — so the panel is dark from the first paint, not just after a theme reapply.
- **Rail look (#5, taste — applied, awaiting confirmation):** `WorkspaceRail` widened 22→32px with a bigger chevron + 13px "PROJECT" label for discoverability (the rail dock sizes to the widget). Comparison image sent to the user; revert/tweak trivially if they prefer.

Regression: `test_doc_tab_chrome`/`test_dock_resize`/`test_dock_size_tip`/`test_tab_source_icon` green; full suite 56/56 green with all of the above in place.

## ⚠ Pre-commit file manifest (verified 2026-06-08, round-4 sweep)

**Four NEW untracked files are integral to the commit — a `git commit -a` (tracked-modified only) would OMIT them and break the build / fail CMake configure.** When landing, `git add` these explicitly:

| Untracked file | Why it must be in the commit |
|----------------|------------------------------|
| `src/code_highlight.h` | `#include`d by the modified, tracked `src/main.cpp` (editor-paper + per-language lexer consolidation) — **build breaks** without it. |
| `tests/test_source_chooser.cpp` | `add_executable` + `add_test` in the modified `CMakeLists.txt:909-918` — **CMake configure fails** (source missing). |
| `tools/sourcechooser_render.cpp` | `add_executable(... EXCLUDE_FROM_ALL ...)` in modified `CMakeLists.txt:705`. |
| `tools/coderender.cpp` | `add_executable(... EXCLUDE_FROM_ALL ...)` in modified `CMakeLists.txt:718`. |

So the commit = `git diff --name-only` (39 tracked-modified files as of the latest edits — self-updating, just run it) **＋ those 4 untracked**. Everything else untracked is debris or your own out-of-band tooling (`tools/*.json`, `tools/*.py`, `training/`, `training2/` — left alone; not in the stated debris categories).

**Cleanup preview** (what `clean` would remove — all untracked, none tracked): 79 images (`*.png`/`*.bmp`), 156 loose `*.txt` logs, `THEIA_*.dmp`, and root build debris `CMakeCache.txt` / `CMakeFiles/` / `Testing/` / `build2/` / `build2_clean/`; plus obvious junk `drag_debug.log`, `testInlineEditReEntry`, `test_editor_border.out`, empty `Comments`, empty `Type`. **Uncertain — your call:** `windows-x86_64.h` (1.3 MB C header, not in your stated debris types) and `scanner_sample_results.json`. KEEP `build/`, `docs/`, all `src/`/`tests/`/`tools/` source, the 4 commit files above.

## Ready-to-use commit message (for the "commit it all as one" path)

If you choose the single-commit path, this message covers the interwoven tree (adjust scope/split as you like):

```
fix(rtti): cache module enum + guard plugin ABI; perf, console, break & source UX

Land the in-flight modulesCached/RTTI feature together with the crash fix,
the DayZ-attach perf work, and this session's UI/UX changes (interwoven —
no clean subset commits in isolation).

Crash / ABI
- Diagnose the process-source-attach 0xC0000005 in rttiForVtable as a
  fragile-base-class ABI break against a stale plugin DLL (Provider grew
  8->40 bytes; a partial --target Reclass build left an old-layout plugin).
- add_dependencies(Reclass <5 provider plugins>) so the app can't be built
  without rebuilding the plugins.
- Provider ABI guard: each provider plugin exports RcxPluginAbiToken()
  (version<<32 | sizeof(Provider)); PluginManager rejects a mismatch with a
  clear "rebuild the plugin" log instead of corrupting memory.

Performance (DayZ-scale)
- Cache enumerateRegions() in classifyPermanentPages (tick-gated, 64 ticks,
  invalidated in resetSnapshot) — kills a per-tick VirtualQueryEx storm.
- Provider::modulesCached() — per-instance memo so rttiForVtable stops
  re-running enumerateModules() per pointer (961 -> 1 calls).
- Auto-RTTI default OFF (View toggle); gate inferTypes behind typeHints.

UI / UX
- GUI-subsystem app (WIN32_EXECUTABLE): no console flash; on-demand console
  via View > Show Console (redirect-aware freopen; [X] disabled).
- Defer bookmarks-dock content off the ctor (327ms -> ~97ms).
- Break into Class: promoted to top-level for byte + multi-node selections;
  nodeInView guard refuses embedded-class fields; isDirectViewFrameChild
  guard refuses union/inline-nested fields (would extract the wrong region).
- Sort QSet-ordered output by offset (Copy Address, Ctrl+C, contiguous merge).
- Gate the null-pointer "(Name class...)" CTA on Provider::isLive() so it
  never sprouts on a flat file source.
- Workspace rail: thin reserved-column dock to re-open a hidden workspace.
- Editor "paper" surface unified via rcx::editorPaperColor across all side
  panels + code/debug/minimap views (fixes a light-theme khaki bug).
- Source chooser: Connected/Add Source labels + per-row delete.
- Project dock header height tied to the editor doc-tab height via a shared
  kDocTabBarHeight constant so their separator lines align to the pixel; the
  workspace panel uses the editor "paper" surface from the first paint (no
  light flash); the collapsed workspace rail widened (22->32px) + bigger
  chevron/label for discoverability.
```

(Authored IChooseYou <ichooseyou@users.noreply.github.com>, no co-authors. Include the 4 untracked files from the manifest above.)

## Open decisions (need your one-word steer — NOT done autonomously)

| Item | The fork |
|------|----------|
| **Commit structure** | Both fixes are interwoven with your uncommitted `modulesCached` feature. The crash fix is mostly 9 clean files (ABI guard) but its `provider.h`/`CMakeLists.txt`/`ProcessMemoryPlugin.cpp` edits sit on your WIP; the **perf fixes live entirely inside WIP files** (`controller.cpp/.h`, `main.cpp`, `compose.cpp`), so there is no clean subset to commit alone — splitting them out would land cache members without their callers. The only coherent commit bundles the whole in-flight feature + both fixes — your call. Say "branch + commit it all as one" (or name the split you want) and I'll execute (as IChooseYou, no co-authors). |
| **Debug-artifact cleanup** | `git status` is buried under `*.png`/`*.dmp`/`*.txt`, root `CMakeCache.txt`/`CMakeFiles/`, `build2/`, `tmp_*.txt`. Yours, not mine — say "clean" and I'll remove the debris (keeping `build/`, `docs/`, source). |
| **ABI guard scope** | Currently guards `rcx::Provider` only. `IPlugin`/`IProviderPlugin` are also plugin-shared bases; a vtable change there has the same hazard. Left out as over-engineering past this incident — flag if you want it covered too. |

Pick one (or point me at something new) and I'll execute immediately.
