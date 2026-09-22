# Finding a view matrix in a live process via MCP

How to locate a 4×4 view/camera matrix in a (multi-GB) target process using the Reclass MCP
scanner tools. Validated end-to-end against a live moving target (2026-06-04).

## Why this works

A game holds **many** approximate 4×4 float matrices (view, projection, world, bone, …); you can't
pick *the* view matrix out by static bytes. Two facts make it findable:

1. **Encoding signature** — an affine view/world matrix has a homogeneous `(0,0,0,1)` vector at float
   indices `{3,7,11,15}` (both DX row-major and GL column-major), and a rotation block of small,
   plausible floats. `scanner.find_matrix` scores 64-byte windows for exactly this.
2. **Dynamic behaviour** — the *live* view matrix is the one that **changes every frame as the camera
   moves**. Static world matrices and vertex data don't. `scanner.rescan condition=changed` after a
   camera move isolates it.

## Recommended workflow (structure-first)

1. **Attach:** `source.switch { pid: <game> }`. (Optionally `source.modules` to get the main module's
   data range.)
2. **Generate candidates:** `scanner.find_matrix { requireOrthonormal: true, maxCandidates: 64 }`
   — one pass; returns a few ranked candidates (score 0-100) with the 16 floats shown. Defaults skip
   system modules and scan writable data. On a huge process, restrict with
   `regions: [["0x<lo>","0x<hi>"]]` (e.g. main-module `.data` + heap) for speed.
3. **Confirm the live one:** move/rotate the camera in-game, then
   `scanner.rescan { condition: "changed" }`. Candidates that updated survive; static ones drop.
   Repeat once or twice — the set collapses to the matrix that moves every frame.
4. **Inspect / apply:** `scanner.results { floatWindow: 16 }` to eyeball the 64-byte block; then apply
   a `Mat4x4` node at the address (`tree.apply`). `analysis.pointer_chain` can recover a static path.

### Tuning `find_matrix`
- `requireOrthonormal: true` — strict (true rotation only); rejects scale/skew and overlap windows.
  Use it first; relax to `false` if the engine uses a non-orthonormal view matrix.
- `magMax` — raise above the default `1e7` if the camera translation is large world-space coords.
- `affineEps` / `orthoEps` — loosen if the matrix is stored with reduced precision.
- `minScore` — default 60 requires the full `(0,0,0,1)`; lower to surface near-misses.

## Alternative: value-capture workflow (no structure assumption)

If `find_matrix` comes up empty (unusual matrix layout), fall back to the classic Cheat-Engine loop:

1. `scanner.scan { valueType: "float", condition: "unknown" }` — captures every aligned float
   (bound the magnitude with `condition: "between", value, value2` to cut the set).
2. Move the camera → `scanner.rescan { condition: "changed" }`. Repeat. The surviving floats are the
   ones that move every frame — the matrix components (and other camera-coupled values).
3. `scanner.results { floatWindow: 16 }` to find a contiguous Mat4×4 among the survivors.

Every scan response carries `scanId`, `total`, and `capped`; `scanner.rescan`/`scanner.results` take
`offset`/`limit` for paging. Pass `scanId` to `rescan` to reject a stale set.

## Notes
- `find_matrix` results carry a 64-byte window; a follow-up `rescan changed` re-reads and compares all
  64 bytes, so a **translation-only** camera move (rotation unchanged) is still detected.
- Long first scans block with a hard timeout (default ~120 s) and return a partial set flagged
  `capped`; constrain `regions` / set `skipSystemModules` to keep the first capture bounded.
