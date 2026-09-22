# Plan — Full "Break into Class" for nested/embedded fields (task **B**)

**Status:** design/analysis only — *not implemented*. The safe interim (refuse with a hint) is live.
This doc gives the decision basis + a code-grounded implementation path so the greenlight is a quick, informed pick. Nothing here changed the data model.

Researched 2026-06-08 (two read-only code audits). All file:line refs verified against the current tree.

---

## 1. Where we are

A field that is a **direct child of the viewed class** breaks correctly. A field nested one frame deeper is **refused** by `isDirectViewFrameChild` (`controller.cpp:2572`) on the node + multi-node menu paths, with a clear hint. That guard is the safe interim — see [[reference_break_scan_parent_relative_frame]].

## 2. Root cause (why nested breaks are hard)

`extractByteSelectionToNewClass` (`controller.cpp:2287`) converts the selection to **root-relative** byte offsets (`relLo = selLo - tree.baseAddress`, `:2299`) and then, in Pass 1/Pass 2, compares them against each field's **parent-relative `n.offset`** (`:2345-2346`, `:2366-2367`) and does all pad/rebase math in those mixed frames (`:2394-2399`, `:2481`, `:2522-2537`). The two frames coincide **only when the field's parent frame origin == root origin** — i.e. the field is a direct child of the view root. For any deeper frame the math silently resolves to the **wrong byte region / wraps the wrong field** (the exact corruption the guard prevents).

## 3. The three nesting cases (frame mechanics)

Let `R` = viewed root (`m_viewRootId`), origin `tree.baseAddress`.

| Case | parentId chain | `n.offset` is relative to | Caught by |
|------|----------------|---------------------------|-----------|
| **(a) Union member** | `member → union → R` | the **union** frame; `groupIntoUnion` resets `member.offset = 0` (`controller.cpp:2810-2811`), union sits at `union.offset` | `nodeInView` ✓ passes, `isDirectViewFrameChild` ✗ refuses |
| **(b) Inline (non-refId) struct field** | `field → innerStruct → R` | the **innerStruct** frame (origin `base + innerStruct.offset`) | same as (a) |
| **(c) refId-embedded class field** | `child → D → 0` (separate def root `D`, **never reaches R**) | `D`'s own frame | `nodeInView` ✗ refuses (different reason) |

**Critical for (c):** an embedded class shown inline renders the **referenced def `D`'s children directly** — the rows carry `D`'s child ids, NOT local copies (`compose.cpp:918-921, 936, 955`). `materializeRefChildren` (`controller.cpp:2896`) only clones-on-demand and is a no-op if children exist. So **breaking on un-materialized embedded rows edits the SHARED def `D`** → changes every other embedding of that class.

## 4. The fix: make the break frame-aware

Replace the implicit "container origin == root origin" assumption with an explicit `frameOff` term. Algorithm:

1. **Identify the container** `C` = the selected field's immediate parent (`union` / `innerStruct` / the embedding node `E`).
2. **`frameOff = computeOffset(indexOfId(C))`** (`core.h:734` — sums `n.offset` up the chain to root; this is the proven correct primitive).
3. **Translate the selection into C-local offsets:** `localLo = relLo - frameOff; localHi = relHi - frameOff`.
4. **Run the existing intersection/extraction restricted to `tree.childrenOf(C)`**, comparing `sib.offset` against `localLo/localHi` (now same frame), parenting the new embedded replacement under `C` at `embed.offset = localLo`, and rebasing child clone offsets as `sn.node.offset - localLo`.

The whole correctness rests on applying `frameOff` **consistently** at every site that currently uses raw `relLo/relHi` vs `n.offset` (§6). Missing it at even one (e.g. leaving `embed.offset = relLo`) reintroduces the wrong-field corruption.

For case (c), step 1 must first `materializeRefChildren(indexOfId(E))` so the rows become real children of `E` (turning it into a case-(b) inline struct) — UNLESS the user intends a shared-def edit (see §5).

## 5. ⚠ The decision you need to make — shared-definition semantics (case c)

When the field lives in a **refId-embedded class**, breaking it is inherently a question of *which* definition changes:

- **(A) MUTATE-SHARED** — edit the referenced def `D` directly → propagates to **every** embedding of that class. **Primitive already exists**: the footer `+1` / `Append` / `Trim` handlers already redirect to `parent.refId` when the embed has no own children (`controller.cpp:1324-1327, 1387-1390, 1416-1421`). Essentially free.
- **(B) UNSHARE-FIRST** — clone `D` into a new unique root class, repoint **this** embedding's `refId` to the clone, then break within the clone → only this instance changes. Template = `convertToTypedPointer` (`controller.cpp:3342-3416`: unique-name loop, create root, `cmd::Insert` children, `cmd::ChangePointerRef`, all under one undo macro). Caveat: `materializeRefChildren` clones **one level only** (`controller.cpp:2910-2921`); a true deep copy needs a recursive walk over `subtreeIndices(D)` (`core.h:682-710`) with parentId remapping — **that recursive-clone primitive does not exist yet** and would be the main new code.

**Recommended default (data-safe, low-surprise):** compute the **reference count** first (one pass counting `n.refId == defId` across embed/pointer/array forms — the loop already exists at `controller.cpp:2717-2722` and `5855-5857`):
- **refcount == 1 → MUTATE-SHARED silently** (no other view to protect; unsharing would just litter the doc with a duplicate type).
- **refcount > 1 → UNSHARE-FIRST by default**, with a one-line undoable notice ("class used in N places — broke a private copy `<Name>_2`"), optionally offering "apply to all uses instead" (= mutate-shared) as a secondary action.

Everything is wrapped in a single `beginMacro`/`endMacro` so either choice is fully reversible.

**This refcount-based default is my recommendation — but cases (a)/(b) (union member, inline struct) have NO sharing and need no decision; they just need the §4 frame translation.** So B can ship in phases (below) and the shared-def decision only blocks case (c).

## 6. Touch-points (functions/lines a full implementation edits)

- `extractByteSelectionToNewClass` `controller.cpp:2287` — add a `containerId`/`frameOff` param; rebase `relLo/relHi` (`:2299`), Pass 1 parent-pick (`:2337-2358`), Pass 2 sibling scan (`:2362-2383`), pad math (`:2394-2399`), child rebase (`:2481`), embed insert (`:2522-2537`).
- `regionFromCurrentSelection` `controller.cpp:2584` — currently unions raw `n.offset` + baseAddress (`:2609, 2615`); must return container-frame-aware coords or the container id.
- Relax/redirect guards: node "Break Class" (`controller.cpp:4552-4586`), multi-node (`controller.cpp:4065-4086`).
- Case (c): invoke `materializeRefChildren` (`controller.cpp:2896`) or the new recursive-clone for unshare.
- `isDirectViewFrameChild`/`nodeInView` (`controller.cpp:2552, 2572`) — become "which frame" classifiers, not pure refuse-guards.

## 7. ✅ VERIFIED safe — the byte-selection path is unguarded but does NOT corrupt

The promoted top-level **byte-selection** "Break into Class" in `addByteSubmenu` (`controller.cpp:~649-653`) calls `extractByteSelectionToNewClass` with **no `isDirectViewFrameChild` guard** — only the node/multi-node menu paths got the interim guard.

**Resolved (test added 2026-06-08):** the byte path's selection comes from `byteSelectionRange()` = actual **root-relative rendered addresses**, so a partial selection inside a nested frame **straddles the container node** at the R frame and hits the existing Pass-2 "crosses a Struct/Array boundary" refusal — it never mis-extracts a wrong field via `base + n.offset` (the node-path bug). A full-container selection breaks the container intact (correct). Pinned by `test_context_menu::testByteBreak_NestedInlineStructPartial_RefusesNotCorrupt` (confirms refuse-not-corrupt; 38/38 green). **No urgent fix needed.** When B lands the frame-aware translation, nested byte selections route through the new logic anyway.

## 8. Suggested phasing

1. **Verify §7** (test the unguarded byte path) — cheap, removes uncertainty.
2. **Phase 1 — cases (a)+(b)** (union member, inline struct): pure §4 frame translation, **no sharing decision**. Lowest risk, covers the common "break a field in a union/inline struct" case.
3. **Phase 2 — case (c)** (refId-embedded): needs the §5 decision + (for unshare) the recursive-clone primitive. Higher risk (touches shared defs); ship behind the refcount default.

Each phase is independently testable; the §4 translation must be regression-tested against the existing direct-child break (must stay byte-identical) and the wrong-field hazard (§2).

## 9. Open question for you (the only true blocker)

For **case (c)** only: confirm the **refcount-based default** (refcount==1 → mutate-shared, refcount>1 → unshare-first + notice), or pick a fixed policy (always-unshare / always-mutate / always-prompt). Cases (a)/(b) need no decision — say "do B phase 1" and I can implement union/inline-struct breaks without touching shared defs.
