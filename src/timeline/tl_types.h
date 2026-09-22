#pragma once

// ── Timeline storage: shared vocabulary ──
//
// The timeline records the bytes a class's view reads, over time, so the
// view can be rewound and re-rendered through the CURRENT structure. It
// stores bytes, never rendered text: retyping a field reinterprets the
// whole history for free.
//
// Everything in src/timeline/ is QtCore-only so its tests run headless.
// The storage engine never holds a Provider — only bytes — so a plugin
// unloading can never leave it with a dangling vtable.

#include <QtGlobal>
#include <cstdint>

namespace rcx::tl {

// One page is the unit of identity, deduplication and coverage — the same
// 4 KiB the refresh loop reads. The unit of CHANGE is a sub-page XOR run.
inline constexpr uint32_t kPageSize = 4096;
inline constexpr uint64_t kPageMask = ~uint64_t(kPageSize - 1);

// Record ids are dense and monotonic within one capture context. A record
// exists only for a tick where something changed — an idle tick costs
// nothing — so ids count changes, not ticks.
using RecordId = uint32_t;
inline constexpr RecordId kNoRecord = 0xFFFFFFFFu;

using PageId = uint32_t;           // dense, assigned on first sight
using BlobId = uint32_t;           // content-addressed page image
inline constexpr BlobId kZeroBlob = 0;   // the all-zero page, never stored

// Where a page stands at a given record.
enum class PageState : uint8_t {
    NotCovered,     // not in anyone's capture set then — never show stale bytes
    NotYetSampled,  // covered, but no read has landed yet
    Valid,          // bytes are known
    Unreadable,     // covered and read, but the read was refused
    Lost,           // stored, but the block holding it failed its checksum
};

// Why a stretch of time has no observations. Written only on transitions,
// so "captured, nothing changed" (no gap) and "not watched" (a gap) never
// look alike.
enum class GapReason : uint8_t {
    Paused,         // the user paused capture
    Suspended,      // the refresh timer stopped (e.g. minimised, capture off)
    SourceLost,     // the process went away
    Dropped,        // backpressure: the engine could not keep up
};

enum class CaptureMode : uint8_t {
    Rolling,        // automatic: keep the recent past, bounded, RAM only
    Recording,      // explicit: keep everything, may spill to disk
    Paused,
};

// Delta ops, one per changed page inside a record.
enum class OpKind : uint8_t {
    XorRuns          = 0,   // sparse XOR runs vs the previous version
    XorDense         = 1,   // 4096 XOR bytes (when runs would be no smaller)
    Enter            = 2,   // page entered coverage (content comes from its anchor)
    Leave            = 3,   // page left coverage
    BecameUnreadable = 4,
    BecameValid      = 5,   // content comes from its anchor
};

// Record flags.
enum RecordFlag : uint8_t {
    RF_CoverageOnly    = 1 << 0,   // no byte changed; coverage or readability did
    RF_HasUnreadable   = 1 << 1,
    RF_AfterGap        = 1 << 2,   // first record after a gap interval
    RF_UserEdit        = 1 << 3,   // at least one op overlaps the user's own write
    RF_PartialSampling = 1 << 4,   // the tick skipped some stable pages
};

// Anchor policy: a page image is stored again only when its deltas since the
// last image reach either cap, bounding reconstruction of any one page to
// this much replay and keeping images to ≤ 25 % of that page's delta bytes.
inline constexpr int      kAnchorMaxOps   = 2048;
inline constexpr uint32_t kAnchorMaxBytes = 16 * 1024;

// Records are appended into fixed-capacity raw chunks; a chunk is the unit
// of sealing, compression and (later) spilling to disk.
inline constexpr int kChunkRawBytes = 256 * 1024;

// Two runs closer than this are encoded as one (the equal bytes between
// them cost less than a second run header).
inline constexpr int kRunMergeGap = 3;

struct AddrRange {
    uint64_t addr = 0;
    uint64_t len  = 0;
};

} // namespace rcx::tl
