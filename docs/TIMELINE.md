# Class timelines

Press **Record** and a class on a live source keeps every change from then on.
The strip along the bottom of a document tab shows when its data changed; drag
on it to look back, and the view renders the bytes of that moment through the
**current** structure — so retyping a field reinterprets the whole recording.

Nothing is captured, and the timeline does not move, until Record is pressed.
View ▸ Timeline ▸ Enable Timeline (on by default) switches the feature off: the
strip and the buttons hide, and anything recorded is freed.

## What the user gets

| Control | Where | Does |
|---|---|---|
| **Back to live** (green ▶) | beside the address — only while looking back at a recording; View ▸ Timeline, `Ctrl+End`, F5 | Leaves the recording and shows live values; the graph follows again. Otherwise values are always live: there is no pausing them |
| **Record** / **Stop** (red ● / red ■) | beside the address, `F9` | Record keeps every change of this class from now on, within the memory limit. Stop keeps what was recorded until Clear; a later Record carries on keeping it. Recording continues while looking back |
| Look back | drag on the graph | Show that moment — it stops exactly where you release it, between changes included (Ctrl snaps to a change); what you see is the state as of the last capture at or before it; release at the right edge is back to live; Esc or a right-press mid-drag puts back exactly what was shown |
| Zoom / pan | wheel / Shift+wheel, double-click fits | While live, zooming changes the span and keeps following; in the past it zooms about the pointer |
| Timeline menu | the ⋯ button at the strip's right end, or right-click the strip | Previous / Next Change, Show Whole Recording, Clear Recording…, Hide Timeline (hides the strip only — recordings and the buttons beside the address stay; View ▸ Timeline ▸ Show Timeline brings it back) |
| Previous / next change | `Ctrl+,` / `Ctrl+.`, the timeline and node menus | A recorded change of the class — or, with rows selected, of **those fields** ("Previous Change of 'health'"). Past the newest change you are live again |
| Show in Timeline | node menu | Frames the selected fields' recorded changes on the strip |
| Clear Recording… | timeline menu, View ▸ Timeline | Discard this class's recording (confirms when there is one); a running recording carries on from now |

Each button is named for what a click does, glyph and word together; only a
very narrow pane shows the glyph alone (the tooltip keeps the word) before
Record, then both, give way. The glyph carries the colour on the chrome's
straight hover fill: no coloured boxes, no pulsing. **No time is written
anywhere** — not beside the buttons, not on the graph; hovering the graph
shows when a moment was.

A class's history is its own: it starts at that class's first Record, so a
class never recorded has none — even while another class, or another tab on
the same process, is recording — and Clear leaves nothing to scrub back to.
Hubs sharing one source each state a retention policy and the source keeps
what the most demanding one needs, so one tab can never trim another's
recording.

Before anything is recorded the strip says *Press Record to capture changes
over time*; recording with nothing changed yet, *Recording — changes show up
here as they happen*. While not recording the strip does not slide: its right
edge stays where the recording ended.

While looking back: values are read-only (writes are refused with a status
hint), structure edits are allowed, recording continues underneath, bytes
that changed at the shown record are lit hot, and bytes nobody was watching
then read `??` in faint ink — never a plausible-looking zero, never the red
unreadable strike (nothing failed).

The strip is 43 px with four lanes, top to bottom:

- **Beads** — records where a field *started* changing (one the record before
  did not change, `ClassChangeSeries::onsets`): a bounce, a restart, a flag
  flipping stands out from the steady churn of a moving object. A bigger bead
  for three or more fields.
- **The envelope** — how many **fields of this class** one record changed
  (square-root scaled; a vector moving is three changes, a double one —
  however many bytes), drawn in the document's number ink as one antialiased
  shape joining record to record, with a crisp edge over a fill that fades
  toward the baseline. Records are far sparser than device columns, so columns
  alone drew a comb of 1-px ticks; two records join when they are neighbours
  on screen or at most 1.2 s apart, so a quiet stretch between bursts drops to
  the baseline, and never across a not-recorded stretch. It sits on a faint band marking
  what was recorded.
- **The baseline** — solid where recorded, dotted where not.
- **The selection lane** — short bars in the selection colour when the
  selected rows changed.

The graph frames the **whole recording** from the left edge
(`timeline::fitLeftWindow`, a dense ms-per-column ladder so little paper is
wasted); it grows into room kept on the right — the same room after Stop, so
Stop rescales nothing — and the scale steps now and then instead of every
tick. Releasing a drag at the end of the recording (not the graph's edge)
goes back to live; panning a frame that already shows everything does
nothing. The beads and the selection lane are per-column queries
(`ClassChangeSeries::onsetMax` over a per-record onset pyramid counted once
at append, `matchColumns`), so they cover the whole recording with no cap and
are drawn into the cached graph image. Zooming in follows now at that span;
zooming out past the recording, double-click, or "Show Whole Recording" frames
all of it again. Hovering draws a hairline with a dot on the envelope and the
readout names the fields: "2 fields changed: health, ammo". Looking back, the
amber playhead carries a small handle and what was recorded after the moment
shown is faded. Until the view has composed once the byte counts of the source
stand in.
Three baselines say what a stretch of time is: **solid** — recorded, nothing
changed; **dotted** — not recorded (after Stop, minimised, detached,
dropped); **none** — cleared, or before the first Record.

Value heat (the warm/hot rows) is separate from the timeline and follows the
live view; each row of a multi-line value — a matrix's four rows — has its own
value history (`valueHistoryKey`), so a row lights when it changes.

## Architecture

```
TimelineService (singleton)  private QThreadPool(2, low priority) · one CaptureContext per live Provider
CaptureContext               PageStore on a serial strand (PoolStrand) · UI-side TimelineModel mirror
TimelineHub                  per document (instance tabs: per controller) · per-class mode, pins, floors,
                             not-recorded intervals · events · base-address epochs
RcxController                read loop → Producer::append while recording · Live | Past view ·
                             TimelineFrameProvider
TimelineStrip                per tab, bottom · header-only, no Q_OBJECT · math in timeline_strip_model.h
```

`src/timeline/` is QtCore only and never holds a `Provider` — only bytes — so
it is tested headless and a plugin unloading cannot leave it with a dangling
vtable. `TimelineFrameProvider` is the one piece that is a `Provider` (a
subclass: `Provider` is a fragile plugin ABI).

### The store (`tl_store.h`)

- Unit: the 4 KiB page the refresh loop already reads.
- A **record** exists only for a tick where something changed; an idle tick
  costs 0 bytes and no job. Ops per changed page: sparse **XOR runs** (a 4-byte
  field change is ~10 bytes), dense XOR, or payload-free
  `Enter / Leave / BecameUnreadable / BecameValid`. XOR is self-inverse, so the
  same records step a frame forward or back.
- Each page has **anchors**: an image (or bare state) valid as of the end of a
  record. An image is stored on first sight, on becoming valid, or after
  `kAnchorMaxOps` / `kAnchorMaxBytes` of deltas — which bounds any page's
  replay and stores a static page of a huge structure exactly once.
- Images are content-addressed (128-bit hash), refcounted, **compared before
  sharing**; the zero page is never stored.
- Records live in 256 KiB chunks, sealed and `qCompress`ed (level 1) at
  capacity, checksum-verified before decoding — a corrupt block turns the
  pages that need it `Lost`, never garbage.
- Coverage is part of history: a page nobody was watching at a record is
  `NotCovered` then, never stale bytes from another time.
- Retention drops whole oldest chunks and re-anchors the pages they touched.
  A recording's pin — kept through Stop until Clear — protects it from
  ageing; the recording byte budgets still hold.

### Recordings on disk (`spill_store.h`)

While any class has a recording (running or stopped), the context's policy
asks to spill: sealed chunks are written out as they seal, page images once
the RAM share for images (`recordRamMiB / 4`, at most 64 MiB) is used, and RAM
keeps only the index, the open chunk and a small read cache. Over the RAM
budget, sealed chunks still in RAM move to disk before anything is dropped;
the disk budget (`recordDiskMiB`) drops the oldest history like any other
budget.

```
<CacheLocation>/timeline/s<pid>-<startMsHex>/LOCK
<CacheLocation>/timeline/s<pid>-<startMsHex>/c<stream>-<segment>.rtc   chunks
<CacheLocation>/timeline/s<pid>-<startMsHex>/b<stream>-<segment>.rtb   page images
```

- Append-only 64 MiB segments with a 16-byte header (`RTLF`, version, kind,
  session tag, stream); the store keeps each entry's location, size and
  checksum and verifies every read — a damaged entry is `Lost` data, never
  garbage. A segment with nothing live left is deleted (retried on Windows
  while a handle lingers).
- A failed write, or free space below `minFreeDiskMiB`, stops spilling for
  that context (`diskDegraded`): the recording carries on in RAM under the
  RAM budget; nothing already on disk is lost.
- Session only: the directory is created when a recording first starts
  (segment files only on the first write), removed when its last file closes
  after `TimelineService::shutdown` at exit, and a crashed session's directory
  is swept by a later launch (~5 s after start) — its `QLockFile` can be taken
  once the owner is gone; a directory with no LOCK goes after an hour. Links
  are never followed.

### Capture (`controller.cpp`)

- The worker reads each page with `read()` — **one page per call** (the
  process plugins report a partial multi-page read as success with the tail
  zero-filled) — diffs against the baseline taken at dispatch, and returns
  only new or changed pages, plus refused pages as `unreadable`.
- **A pointer and its target are one moment.** The view's pointer graph is
  flattened on the UI thread into a `CapturePlan` (rebuilt only when the
  tree generation, the view root or a fold changes): per struct, its pointers
  — offset, size, extra dereferences, relative — and what they lead to,
  through embedded structs, arrays of structs, materialised pointer children
  and primitive pointers that display their target. The worker reads the
  struct pages, takes pointer values **from the bytes it just read**, and
  reads the targets in the same tick (64 MiB budget, cycle-safe).
- Pages compose actually read (RTTI vtables and the like) are recorded and
  read on the next ticks as a supplement.
- `onReadComplete` feeds the tick to the timeline **before** composing — only
  while the class shown is recording. The first recorded tick hands over
  every watched page (the snapshot's copy of pages that did not change), so a
  recording starts complete; after that only changed pages, with the watched
  page set sent when it changes, and the user's own write ranges. Stop (or
  switching to a class that is not recording) sends a coverage-only record:
  from there the history reads "not recorded".
- Rebase and source switch do not lose history: `resetSnapshot` re-binds
  (same process → same history, a base epoch and a Rebase event).
- While recording, the timer keeps running when unfocused and minimised
  (`timeline/captureWhenMinimized`, default on); nothing composes until the
  window is back. Not recording, the old throttles apply.

### Live, always (`controller.cpp`)

Values on screen are always live; the only thing that stops them is looking
back at a recording. There is no Pause (it was built, and removed: a second
way to stop the view only muddled "live" with "recording"). Copying bytes,
saving a selection and Open Beside read what is on screen
(`displayedProvider`): the recorded moment while looking back, else the
snapshot. Every value write is refused while looking back, bitfield Toggle
Bit / Edit Value included; switching the timeline off goes back to live.

### The past (`frame_provider.h`)

`compose()` renders whatever `Provider` it is given, so the past is a provider
swap. `TimelineFrameProvider` serves a frame's pages and **never reads live
memory**: an uncaptured page is zeros plus `read() == false`. Only a page
whose read *failed* then is not readable (compose strikes it, as it did
live); an uncaptured page stays "readable" so a pointer's target is still
laid out at its address, and the controller marks every value touching it
`notCaptured` (`??`). It reports `isWritable() == false` (which alone disables
inline value edits) and `isLive() == true` (so null-pointer chips look as
they did). A frame is composed at the base address the class had then.

### Fields changed (`class_changes.h`)

Counting happens on the UI thread, where the layout is. Every live compose
refreshes a `FieldIndex` — the composed rows as absolute byte spans (leaf
values, array elements, open pointers' own bytes, a folded struct as one
field), keyed by the row's selection id. Each commit carries the changed
spans of its records (`setWantSpans`), and a `ClassChangeSeries` keeps, only
for records that touched a field, the count, up to 32 ids and the byte hull,
over a max/sum pyramid. When the layout changes (retype, resize, fold,
another view root) the retained history is recounted from the store's spans
off the UI thread; records from before a rebase are counted where the class
was then. Selection-scoped stepping matches ids, and the hull for records
that changed more fields than they list.

## Settings (`QSettings("RC","RC")`)

| Key | Default |
|---|---|
| `timeline/enabled` | true |
| `timeline/recordRamMiB` | 256 |
| `timeline/recordDiskMiB` | 16384 |
| `timeline/minFreeDiskMiB` | 4096 |
| `timeline/captureWhenMinimized` | true |
| `timeline/stripVisible` | true (View ▸ Timeline ▸ Show Timeline; hiding keeps recordings) |

`timeline/rollingMinutes`, `rollingMiB` and `rollingGlobalMiB` are still read
as the store's budgets for a context with no recording, which now only holds
what a coverage-only record needs.

## Tests and harness

| Target | Pins |
|---|---|
| `test_timeline` | varints, XOR deltas both ways, pyramid, **model fuzz** (every record reconstructs exactly, in any order, after trims), idle ticks store nothing, replay bound, image sharing by content, coverage, refused reads, stale samples, change queries, retention, gaps, corrupt chunks; the facade (latest-wins frames, dead addressees, backpressure gaps, serial strand); the hub (Stop keeps the pin and opens a not-recorded interval, Record closes it, a class sees nothing before its own first Record, hubs sharing a context merge their retention); the frame provider never reading live; field index hits; change series; spill round trips, corrupt spill files, degraded writes, session directories, disk budgets |
| `test_timeline_strip_model` | time ladder, no shimmer while following, zoom keeps the anchor, pan, snap, domain hysteresis, sqrt heights, regions, the readout's formatters, the graph + one square button layout |
| `test_timeline_strip` | nothing moves between states, only the graph and one button, latest-wins scrub with a final on release, live edge, right-press cancel, returning live follows again, zooming while live keeps following, the reduced menu, "Press Record" when empty and no words on a graph with data, amber playhead, no accent colours, the overflow tooltip, selected-field ticks, Show in Timeline framing |
| `test_breadcrumb` (timeline) | the buttons beside the address say what they do (Record / Stop; Back to live only while looking back), green triangle / red dot and square, pinned right, clicks, glyph-only then dropped in narrow panes, never past the edge, and no state change ever moves another cell at any width |
| `test_timeline_capture` | end to end through the controller: **nothing kept until Record**, the first record holds what was already on screen, nothing after Stop; the past equals what was shown and **reads no live memory**, rebase keeps history at the old address, writes refused in the past, the view stays live while recording, minimised capture, stepping, detach, `??` for unwatched bytes and pointer targets, a moved pointer and its new target in one record, changes counted in fields, stepping scoped to the selected field, Show in Timeline, every matrix row's heat; switching the timeline off goes back to live, a cleared recording cannot be scrubbed back, a class never recorded has no history while another records |
| `test_refresh_speedups` | changed-byte highlight at a non-zero base, refused pages unreadable, unchanged pages keep their buffer, run diff vs naive |
| `timeline_render` | every state at 240…1920 px — run hidden, both themes, 1.0 and 1.25 |
