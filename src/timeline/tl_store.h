#pragma once

// ── PageStore: the timeline's byte history for one address space ──
//
// Single-threaded by design: everything here runs on one strand (a serial
// lane on the timeline pool, or the calling thread in tests), so there are
// no locks inside. What crosses threads are VALUES — TickInput in, Frames
// out — never references into the store.
//
// The model, in one paragraph. Each 4 KiB page has a HEAD (its bytes as of
// the latest record) and a list of ANCHORS: a stored image (or a bare state,
// for NotCovered / Unreadable) valid "as of the end of record N". Between
// anchors a page changes only through XOR deltas recorded in RECORDS, and a
// record exists only for a tick where something changed. To know a page at
// record r: take its last anchor at or before r, then XOR in that page's
// deltas up to r. Anchors are written only on first sight, on a state
// change, or after kAnchorMaxOps / kAnchorMaxBytes of deltas, which bounds
// any one page's replay and keeps images a fraction of the delta bytes —
// a static page of a huge structure is stored exactly once.
//
// Records live in CHUNKS (append-only, sealed and compressed at capacity).
// Retention drops whole oldest chunks and re-anchors the pages they touched.

#include "tl_types.h"
#include "tl_hash.h"
#include "tl_pyramid.h"
#include "spill_store.h"

#include <QByteArray>
#include <QHash>
#include <QMultiHash>
#include <QPair>
#include <QSet>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

namespace rcx::tl {

// One tick's worth of observations from one producer (a controller's
// refresh loop).
struct TickInput {
    int64_t timeMs = 0;        // capture clock, milliseconds, monotonic
    int64_t readStartMs = 0;   // when the read began (stale-sample rule)
    int     producer = 0;      // which read loop took these samples

    // Page-aligned address → kPageSize bytes. Normally only pages that are
    // new or changed; identical pages are cheap to pass and store nothing.
    QHash<uint64_t, QByteArray> pages;
    QVector<uint64_t> unreadable;            // pages whose read was refused

    // The producer's full intended page set — sent only when it changed.
    // Pages sampled without ever being declared are covered implicitly.
    std::optional<QVector<uint64_t>> coverage;

    QVector<AddrRange> userEdits;            // bytes the user just wrote
    bool partialSampling = false;            // some stable pages were skipped
};

// An absolute span of changed bytes.
struct ChangedSpan {
    uint64_t addr = 0;
    uint32_t len = 0;
};

struct Gap {
    int64_t   startMs = 0;
    int64_t   endMs = -1;     // < 0: still open
    GapReason reason = GapReason::Paused;
    bool isOpen() const { return endMs < 0; }
    bool contains(int64_t t) const { return t >= startMs && (isOpen() || t < endMs); }
};

// A reconstructed moment. Immutable once handed out and self-contained: it
// stays valid after the store trims or is destroyed.
struct Frame {
    RecordId record = kNoRecord;
    int64_t  timeMs = 0;
    QHash<uint64_t, QByteArray> pages;   // Valid pages only (shared, copy-on-write)
    QHash<uint64_t, PageState>  states;  // every page that was asked for
    QVector<ChangedSpan> changedAt;      // bytes that changed AT this record (when asked)

    PageState stateOf(uint64_t addr) const {
        return states.value(addr & kPageMask, PageState::NotCovered);
    }
};
using FramePtr = std::shared_ptr<const Frame>;

struct StoreConfig {
    // Test seam: force hash collisions to prove images are compared, not
    // trusted. Defaults to hashBytes.
    std::function<Hash128(const char*, size_t)> hash;
    int  chunkRawBytes = kChunkRawBytes;   // tests shrink this to exercise sealing
    bool compressSealed = true;
    int  decodeCacheChunks = 8;
};

struct StoreStats {
    RecordId firstRecord = kNoRecord;
    RecordId lastRecord = kNoRecord;
    int      records = 0;
    int      chunks = 0;
    int      pages = 0;
    int      blobs = 0;
    int      anchors = 0;
    qint64   chunkBytes = 0;   // stored (compressed) + open raw
    qint64   blobBytes = 0;
    qint64   indexBytes = 0;
    qint64   totalBytes() const { return chunkBytes + blobBytes + indexBytes; }
    int      lostChunks = 0;
    // A recording's history on disk (not part of totalBytes, which is RAM).
    qint64   diskBytes = 0;
    int      spilledChunks = 0;
    int      spilledBlobs = 0;
    bool     diskDegraded = false;   // a spill write failed: RAM only from then on
    // Instrumentation for the replay bound.
    mutable qint64 replayedOps = 0;
};

class PageStore {
public:
    explicit PageStore(StoreConfig cfg = {});
    PageStore(const PageStore&) = delete;
    PageStore& operator=(const PageStore&) = delete;

    // ── Ingest ──
    // Returns the record created, or kNoRecord when nothing changed — an
    // idle tick costs no bytes.
    RecordId ingest(const TickInput& in);
    // A producer went away: its coverage leaves (a coverage-only record).
    RecordId removeProducer(int producer, int64_t timeMs);

    void beginGap(int64_t timeMs, GapReason reason);
    void endGap(int64_t timeMs);
    const QVector<Gap>& gaps() const { return m_gaps; }
    std::optional<Gap> gapAt(int64_t timeMs) const;

    // ── Index ──
    bool     isEmpty() const { return m_times.isEmpty(); }
    RecordId firstRecord() const { return isEmpty() ? kNoRecord : m_base; }
    RecordId lastRecord() const { return isEmpty() ? kNoRecord : m_next - 1; }
    bool     contains(RecordId r) const { return !isEmpty() && r >= m_base && r < m_next; }
    int64_t  timeOf(RecordId r) const;
    RecordId recordAtOrBefore(int64_t timeMs) const;
    uint32_t changedBytesOf(RecordId r) const;
    uint8_t  flagsOf(RecordId r) const;
    const MaxSumPyramid& changedBytesPyramid() const { return m_pyramid; }

    // ── Reads ──
    FramePtr frameAt(RecordId r, const QVector<uint64_t>& wantedPages) const;
    // Move a frame to record r, touching only what changed in between. XOR
    // deltas commute, so the same records step the frame either direction.
    FramePtr advance(const FramePtr& from, RecordId r) const;
    PageState pageAt(uint64_t pageAddr, RecordId r, QByteArray* bytes = nullptr) const;
    // Every page someone was watching at record r (state other than
    // NotCovered), sorted. A past frame asks for exactly these, which
    // includes the pointer targets that were being followed at the time.
    QVector<uint64_t> coveredPagesAt(RecordId r) const;
    // Byte spans that changed AT record r (normalized, absolute).
    QVector<ChangedSpan> changedSpansAt(RecordId r) const;
    // Records in [lo, hi] where any byte of [addr, addr+len) changed.
    QVector<RecordId> changeRecordsForRange(uint64_t addr, uint64_t len,
                                            RecordId lo, RecordId hi) const;

    // ── Retention ──
    // Drop every whole chunk that lies entirely before `floor`. Returns the
    // first record still retained.
    RecordId trimBefore(RecordId floor);
    // Drop oldest chunks while over `budgetBytes`, or while older than
    // `nowMs - windowMs` (windowMs <= 0: no time limit). Records at or after
    // `protectFrom` are never dropped for age, only for bytes.
    // A recording first moves sealed chunks to disk (when spilling) before
    // anything is dropped for RAM; `diskBudgetBytes` (> 0) caps what is on
    // disk the same way — oldest first.
    void enforceRetention(qint64 budgetBytes, int64_t nowMs, int64_t windowMs,
                          RecordId protectFrom = kNoRecord, qint64 diskBudgetBytes = 0);

    // ── Disk ──
    // Set once. While active, sealed chunks are written to `chunks`, and page
    // images past `blobRamBytes` of RAM to `blobs`; what is on disk stays
    // readable after deactivation. A failed write keeps the bytes in RAM.
    void setSpill(std::shared_ptr<SpillFile> chunks, std::shared_ptr<SpillFile> blobs,
                  qint64 blobRamBytes);
    void setSpillActive(bool on) { m_spillActive = on && m_spillChunks && m_spillBlobs; }
    bool hasSpill() const { return m_spillChunks != nullptr; }
    bool spillActive() const { return m_spillActive; }

    StoreStats stats() const;

    // Test hook: flip one stored byte of a sealed chunk, as disk or memory
    // corruption would. Returns false if there is no such sealed chunk.
    bool corruptSealedChunkForTest(int chunkIndex);
    // The same for a chunk that is on disk: the byte is flipped in its file.
    bool corruptSpilledChunkForTest(int chunkIndex);

private:
    struct Anchor {
        RecordId  rec = 0;                // state as of the END of this record
        PageState state = PageState::NotCovered;
        BlobId    blob = kZeroBlob;       // meaningful when state == Valid
        RecordId  firstOp = kNoRecord;    // first XOR delta after this anchor
        RecordId  lastOp = kNoRecord;     // last XOR delta before the next anchor
    };
    struct PageSlot {
        uint64_t  addr = 0;
        QByteArray head;                  // bytes after the latest record (Valid only)
        PageState state = PageState::NotCovered;
        QVector<Anchor> anchors;
        int64_t   lastSampleMs = INT64_MIN;
        int       opsSinceAnchor = 0;
        uint32_t  bytesSinceAnchor = 0;
        int       coverageRefs = 0;
    };
    struct Blob {
        Hash128    hash;
        QByteArray data;                  // empty when on disk
        uint32_t   refs = 0;
        SpillLocation disk;
        uint32_t   checksum = 0;          // of the bytes on disk
    };
    struct Chunk {
        RecordId   first = 0;
        int        count = 0;
        QByteArray raw;                   // open chunk; empty once sealed
        QByteArray stored;                // sealed bytes (maybe compressed); empty when on disk
        SpillLocation disk;
        bool       sealed = false;
        bool       compressed = false;
        int        rawSize = 0;
        uint32_t   checksum = 0;
        mutable bool lost = false;
        QHash<PageId, uint64_t> pageMasks;  // pages with ops here → touched buckets
        RecordId last() const { return first + RecordId(count) - 1; }
    };
    struct PendingOp {
        PageId     page = 0;
        OpKind     kind = OpKind::XorRuns;
        bool       userEdit = false;
        QByteArray payload;
        uint64_t   mask = 0;
    };
    struct Need {
        PageId   id = 0;
        uint64_t addr = 0;
        RecordId from = 0;
        RecordId to = 0;
    };

    // Pages / anchors / blobs
    PageId   pageIdFor(uint64_t pageAddr);
    int      anchorAtOrBefore(const PageSlot& s, RecordId r) const;
    void     setAnchor(PageSlot& s, RecordId rec, PageState st, const QByteArray* bytes);
    BlobId   acquireBlob(const QByteArray& bytes);
    void     retainBlob(BlobId id);
    void     releaseBlob(BlobId id);
    // The image's bytes; `ok` false (and empty bytes) when its copy on disk
    // cannot be read back intact.
    QByteArray blobData(BlobId id, bool* ok = nullptr) const;
    bool     blobMatches(BlobId id, const QByteArray& bytes) const;

    // Chunks / records
    int   chunkIndexOf(RecordId r) const;
    const QByteArray* chunkBytes(int ci) const;   // nullptr: lost
    void  sealOpenChunk();
    void  spillChunk(Chunk& c);
    void  spillSealedChunks(qint64 budgetBytes);
    void  writeRecord(RecordId rec, uint8_t flags, uint32_t changedBytes,
                      QVector<PendingOp>& ops, int64_t timeMs);
    // Walk the records in [lo, hi] of the chunks that may hold ops for the
    // given pages; fn(rec, pageId, kind, payloadReader) for each such op.
    // Returns the pages whose data could not be read (lost chunk).
    template <typename Fn>
    QSet<PageId> forEachOp(RecordId lo, RecordId hi, const QSet<PageId>& pages, Fn&& fn) const;
    void  replay(QVector<Need>& needs, Frame& f) const;

    StoreConfig m_cfg;

    QVector<PageSlot> m_slots;
    QHash<uint64_t, PageId> m_pageIds;
    QHash<int, QSet<uint64_t>> m_coverage;   // producer → declared pages

    QVector<Blob> m_blobs;                   // [0] = the zero page, never stored
    QVector<BlobId> m_freeBlobs;
    QMultiHash<Hash128, BlobId> m_blobIndex;
    qint64 m_blobBytes = 0;

    QVector<Chunk> m_chunks;
    RecordId m_base = 0;                     // first retained record
    RecordId m_next = 0;                     // id the next record will get
    QVector<int64_t>  m_times;               // indexed by rec - m_base
    QVector<uint32_t> m_offsets;             // byte offset of the record in its chunk
    QVector<uint32_t> m_changed;
    QVector<uint8_t>  m_flags;
    MaxSumPyramid     m_pyramid;             // changed bytes per record

    QVector<Gap> m_gaps;
    bool m_afterGap = false;

    mutable QVector<QPair<RecordId, QByteArray>> m_decodeCache;   // chunk first → raw
    mutable StoreStats m_instr;

    std::shared_ptr<SpillFile> m_spillChunks;
    std::shared_ptr<SpillFile> m_spillBlobs;
    qint64 m_blobRamBytes = 0;
    bool   m_spillActive = false;
    qint64 m_chunkDiskBytes = 0;
    qint64 m_blobDiskBytes = 0;
    int    m_spilledBlobs = 0;
    mutable QVector<QPair<BlobId, QByteArray>> m_blobCache;       // images read back from disk
};

} // namespace rcx::tl
