#include "tl_store.h"
#include "tl_varint.h"
#include "tl_xorrun.h"

#include <QtCore/qbytearray.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace rcx::tl {

namespace {

// Record layout inside a chunk (all multi-byte integers are varints):
//   u8 flags · opCount · changedBytes · ops…
// Op:  pageIdDelta (zigzag, vs the previous op's page) · u8 kind|userEdit<<3 · payload
constexpr uint8_t kKindMask = 0x07;
constexpr uint8_t kUserEditBit = 0x08;

bool isXor(OpKind k) { return k == OpKind::XorRuns || k == OpKind::XorDense; }

const QByteArray& zeroPage() {
    static const QByteArray z(int(kPageSize), '\0');
    return z;
}

} // namespace

PageStore::PageStore(StoreConfig cfg) : m_cfg(std::move(cfg)) {
    if (!m_cfg.hash)
        m_cfg.hash = [](const char* d, size_t n) { return hashBytes(d, n); };
    if (m_cfg.chunkRawBytes < 256) m_cfg.chunkRawBytes = 256;
    m_blobs.append(Blob{});   // id 0: the zero page
}

// ─────────────────────────────────────────────────────────────────────────
//  Pages, anchors, blobs
// ─────────────────────────────────────────────────────────────────────────

PageId PageStore::pageIdFor(uint64_t pageAddr) {
    auto it = m_pageIds.constFind(pageAddr);
    if (it != m_pageIds.constEnd()) return it.value();
    const PageId id = PageId(m_slots.size());
    PageSlot s;
    s.addr = pageAddr;
    m_slots.append(std::move(s));
    m_pageIds.insert(pageAddr, id);
    return id;
}

int PageStore::anchorAtOrBefore(const PageSlot& s, RecordId r) const {
    // Last anchor with rec <= r.
    auto it = std::upper_bound(s.anchors.cbegin(), s.anchors.cend(), r,
        [](RecordId v, const Anchor& a) { return v < a.rec; });
    return int(it - s.anchors.cbegin()) - 1;
}

BlobId PageStore::acquireBlob(const QByteArray& bytes) {
    if (isZeroBytes(bytes.constData(), size_t(bytes.size()))) return kZeroBlob;
    const Hash128 h = m_cfg.hash(bytes.constData(), size_t(bytes.size()));
    // A hash is an index, not a proof: share an image only if the bytes match.
    for (auto it = m_blobIndex.constFind(h); it != m_blobIndex.cend() && it.key() == h; ++it) {
        if (blobMatches(it.value(), bytes)) { ++m_blobs[int(it.value())].refs; return it.value(); }
    }
    BlobId id;
    if (!m_freeBlobs.isEmpty()) {
        id = m_freeBlobs.takeLast();
    } else {
        id = BlobId(m_blobs.size());
        m_blobs.append(Blob{});
    }
    Blob& b = m_blobs[int(id)];
    b.hash = h;
    b.refs = 1;
    b.disk = {};
    m_blobIndex.insert(h, id);
    // A recording past its RAM share writes new images out; RAM keeps the index.
    if (m_spillActive && !m_spillBlobs->failed() && m_blobBytes + bytes.size() > m_blobRamBytes) {
        const SpillLocation loc = m_spillBlobs->write(bytes);
        if (loc.isValid()) {
            b.disk = loc;
            b.checksum = checksum32(bytes.constData(), size_t(bytes.size()));
            b.data = QByteArray();
            m_blobDiskBytes += loc.size;
            ++m_spilledBlobs;
            return id;
        }
    }
    b.data = bytes;          // shared with the caller's buffer (copy-on-write)
    m_blobBytes += bytes.size();
    return id;
}

bool PageStore::blobMatches(BlobId id, const QByteArray& bytes) const {
    const Blob& b = m_blobs[int(id)];
    if (!b.disk.isValid()) return b.data == bytes;
    bool ok = false;
    const QByteArray stored = blobData(id, &ok);
    return ok && stored == bytes;
}

void PageStore::retainBlob(BlobId id) {
    if (id != kZeroBlob) ++m_blobs[int(id)].refs;
}

void PageStore::releaseBlob(BlobId id) {
    if (id == kZeroBlob) return;
    Blob& b = m_blobs[int(id)];
    if (b.refs == 0) return;
    if (--b.refs == 0) {
        m_blobIndex.remove(b.hash, id);
        if (b.disk.isValid()) {
            if (m_spillBlobs) m_spillBlobs->release(b.disk);
            m_blobDiskBytes -= b.disk.size;
            --m_spilledBlobs;
            b.disk = {};
            for (int k = m_blobCache.size() - 1; k >= 0; --k)
                if (m_blobCache[k].first == id) m_blobCache.removeAt(k);
        } else {
            m_blobBytes -= b.data.size();
        }
        b.data = QByteArray();
        m_freeBlobs.append(id);
    }
}

QByteArray PageStore::blobData(BlobId id, bool* ok) const {
    if (ok) *ok = true;
    if (id == kZeroBlob) return zeroPage();
    const Blob& b = m_blobs[int(id)];
    if (!b.disk.isValid()) return b.data;
    for (const auto& e : m_blobCache)
        if (e.first == id) return e.second;
    QByteArray bytes;
    if (!m_spillBlobs || !m_spillBlobs->read(b.disk, bytes)
        || checksum32(bytes.constData(), size_t(bytes.size())) != b.checksum) {
        if (ok) *ok = false;
        return {};
    }
    if (m_blobCache.size() >= 64) m_blobCache.removeFirst();
    m_blobCache.append({id, bytes});
    return bytes;
}

void PageStore::setAnchor(PageSlot& s, RecordId rec, PageState st, const QByteArray* bytes) {
    Anchor a;
    a.rec = rec;
    a.state = st;
    a.blob = (st == PageState::Valid && bytes) ? acquireBlob(*bytes) : kZeroBlob;
    // Two anchor events in one record (e.g. Enter then its first sample):
    // the later one describes the end of the record, so it replaces.
    if (!s.anchors.isEmpty() && s.anchors.last().rec == rec) {
        releaseBlob(s.anchors.last().blob);
        s.anchors.last() = a;
    } else {
        s.anchors.append(a);
    }
    s.opsSinceAnchor = 0;
    s.bytesSinceAnchor = 0;
}

// ─────────────────────────────────────────────────────────────────────────
//  Ingest
// ─────────────────────────────────────────────────────────────────────────

RecordId PageStore::ingest(const TickInput& in) {
    const RecordId rec = m_next;
    QVector<PendingOp> ops;
    QHash<PageId, int> opIndex;   // page → index in ops, for this record
    uint32_t changedBytes = 0;
    bool anyXor = false, anyUnreadable = false, anyUserEdit = false;

    auto pushStateOp = [&](PageId id, OpKind kind) {
        auto it = opIndex.constFind(id);
        if (it != opIndex.constEnd()) {
            PendingOp& op = ops[it.value()];
            // Enter stays Enter whatever the page's first observation is.
            if (op.kind != OpKind::Enter) op.kind = kind;
            op.payload.clear();
            op.mask = ~uint64_t(0);
            return;
        }
        PendingOp op;
        op.page = id;
        op.kind = kind;
        op.mask = ~uint64_t(0);
        opIndex.insert(id, ops.size());
        ops.append(std::move(op));
    };

    auto overlapsUserEdit = [&](uint64_t pageAddr) {
        for (const AddrRange& r : in.userEdits) {
            if (r.len == 0) continue;
            const uint64_t last = r.addr + (r.len - 1);
            if (r.addr <= pageAddr + (kPageSize - 1) && last >= pageAddr) return true;
        }
        return false;
    };

    auto enter = [&](uint64_t pageAddr) {
        const PageId id = pageIdFor(pageAddr);
        PageSlot& s = m_slots[int(id)];
        if (s.coverageRefs++ > 0) return;
        s.state = PageState::NotYetSampled;
        s.head = QByteArray();
        setAnchor(s, rec, PageState::NotYetSampled, nullptr);
        pushStateOp(id, OpKind::Enter);
    };
    auto leave = [&](uint64_t pageAddr) {
        auto it = m_pageIds.constFind(pageAddr);
        if (it == m_pageIds.constEnd()) return;
        PageSlot& s = m_slots[int(it.value())];
        if (s.coverageRefs <= 0 || --s.coverageRefs > 0) return;
        s.state = PageState::NotCovered;
        s.head = QByteArray();
        setAnchor(s, rec, PageState::NotCovered, nullptr);
        pushStateOp(it.value(), OpKind::Leave);
    };

    // 1. Coverage.
    QSet<uint64_t>& cov = m_coverage[in.producer];
    if (in.coverage) {
        QSet<uint64_t> next;
        next.reserve(in.coverage->size());
        for (uint64_t p : *in.coverage) next.insert(p & kPageMask);
        QVector<uint64_t> leaving, entering;
        for (uint64_t p : std::as_const(cov)) if (!next.contains(p)) leaving.append(p);
        for (uint64_t p : std::as_const(next)) if (!cov.contains(p)) entering.append(p);
        std::sort(leaving.begin(), leaving.end());
        std::sort(entering.begin(), entering.end());
        for (uint64_t p : leaving) leave(p);
        for (uint64_t p : entering) enter(p);
        cov = std::move(next);
    }
    auto implicitlyCover = [&](uint64_t pageAddr) {
        if (!cov.contains(pageAddr)) { cov.insert(pageAddr); enter(pageAddr); }
    };

    // 2. Refused reads.
    QVector<uint64_t> unreadable;
    unreadable.reserve(in.unreadable.size());
    for (uint64_t p : in.unreadable) unreadable.append(p & kPageMask);
    std::sort(unreadable.begin(), unreadable.end());
    unreadable.erase(std::unique(unreadable.begin(), unreadable.end()), unreadable.end());
    for (uint64_t p : unreadable) {
        implicitlyCover(p);
        const PageId id = m_pageIds.value(p);
        PageSlot& s = m_slots[int(id)];
        if (in.readStartMs < s.lastSampleMs) continue;
        s.lastSampleMs = in.readStartMs;
        if (s.state == PageState::Unreadable) continue;
        s.state = PageState::Unreadable;
        s.head = QByteArray();
        setAnchor(s, rec, PageState::Unreadable, nullptr);
        pushStateOp(id, OpKind::BecameUnreadable);
        anyUnreadable = true;
    }

    // 3. Samples, in address order so ops come out in a stable order.
    QVector<uint64_t> sampled;
    sampled.reserve(in.pages.size());
    for (auto it = in.pages.constBegin(); it != in.pages.constEnd(); ++it)
        if (it.value().size() == int(kPageSize)) sampled.append(it.key() & kPageMask);
    std::sort(sampled.begin(), sampled.end());
    for (uint64_t p : sampled) {
        const QByteArray& bytes = in.pages.value(p);
        implicitlyCover(p);
        const PageId id = m_pageIds.value(p);
        PageSlot& s = m_slots[int(id)];
        // A slower producer's older read must not revert a fresher sample.
        if (in.readStartMs < s.lastSampleMs) continue;
        s.lastSampleMs = in.readStartMs;

        if (s.state == PageState::Valid) {
            if (s.head.constData() == bytes.constData() || s.head == bytes) continue;
            PendingOp op;
            op.page = id;
            XorStats st;
            op.kind = encodeXor(s.head.constData(), bytes.constData(), int(kPageSize),
                                op.payload, &st);
            {
                ByteReader r(op.payload.constData(), op.payload.size());
                QVector<QPair<int, int>> touched;
                xorTouchedRanges(op.kind, r, int(kPageSize), touched);
                for (const auto& t : touched) op.mask |= bucketMaskForRange(t.first, t.second);
            }
            op.userEdit = overlapsUserEdit(p);
            anyUserEdit = anyUserEdit || op.userEdit;
            changedBytes += st.changedBytes;
            anyXor = true;
            const uint32_t payloadBytes = uint32_t(op.payload.size());
            opIndex.insert(id, ops.size());
            ops.append(std::move(op));

            s.head = bytes;
            Anchor& a = s.anchors.last();
            if (a.firstOp == kNoRecord) a.firstOp = rec;
            a.lastOp = rec;
            ++s.opsSinceAnchor;
            s.bytesSinceAnchor += payloadBytes;
            if (s.opsSinceAnchor >= kAnchorMaxOps || s.bytesSinceAnchor >= kAnchorMaxBytes)
                setAnchor(s, rec, PageState::Valid, &s.head);
        } else {
            // First sight, or readable again: the content is an image.
            s.state = PageState::Valid;
            s.head = bytes;
            setAnchor(s, rec, PageState::Valid, &s.head);
            pushStateOp(id, OpKind::BecameValid);
        }
    }

    if (ops.isEmpty()) return kNoRecord;

    uint8_t flags = 0;
    if (!anyXor) flags |= RF_CoverageOnly;
    if (anyUnreadable) flags |= RF_HasUnreadable;
    if (m_afterGap) { flags |= RF_AfterGap; m_afterGap = false; }
    if (anyUserEdit) flags |= RF_UserEdit;
    if (in.partialSampling) flags |= RF_PartialSampling;

    int64_t t = in.timeMs;
    if (!m_times.isEmpty() && t < m_times.last()) t = m_times.last();
    writeRecord(rec, flags, changedBytes, ops, t);
    ++m_next;
    return rec;
}

RecordId PageStore::removeProducer(int producer, int64_t timeMs) {
    if (!m_coverage.contains(producer)) return kNoRecord;
    TickInput in;
    in.producer = producer;
    in.timeMs = timeMs;
    in.readStartMs = INT64_MIN;
    in.coverage = QVector<uint64_t>{};
    const RecordId r = ingest(in);
    m_coverage.remove(producer);
    return r;
}

void PageStore::beginGap(int64_t timeMs, GapReason reason) {
    if (!m_gaps.isEmpty() && m_gaps.last().isOpen()) return;
    Gap g;
    g.startMs = timeMs;
    g.reason = reason;
    m_gaps.append(g);
    m_afterGap = true;
}

void PageStore::endGap(int64_t timeMs) {
    if (m_gaps.isEmpty() || !m_gaps.last().isOpen()) return;
    m_gaps.last().endMs = std::max(timeMs, m_gaps.last().startMs);
}

std::optional<Gap> PageStore::gapAt(int64_t timeMs) const {
    auto it = std::upper_bound(m_gaps.cbegin(), m_gaps.cend(), timeMs,
        [](int64_t t, const Gap& g) { return t < g.startMs; });
    if (it == m_gaps.cbegin()) return std::nullopt;
    const Gap& g = *(it - 1);
    if (g.contains(timeMs)) return g;
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────
//  Chunks
// ─────────────────────────────────────────────────────────────────────────

void PageStore::writeRecord(RecordId rec, uint8_t flags, uint32_t changedBytes,
                            QVector<PendingOp>& ops, int64_t timeMs) {
    std::sort(ops.begin(), ops.end(),
              [](const PendingOp& a, const PendingOp& b) { return a.page < b.page; });

    QByteArray body;
    body.append(char(flags));
    putVarU32(body, uint32_t(ops.size()));
    putVarU32(body, changedBytes);
    int64_t prevPage = 0;
    for (const PendingOp& op : ops) {
        putVarS64(body, int64_t(op.page) - prevPage);
        prevPage = int64_t(op.page);
        body.append(char(uint8_t(op.kind) | (op.userEdit ? kUserEditBit : 0)));
        if (isXor(op.kind)) body.append(op.payload);
    }

    if (m_chunks.isEmpty() || m_chunks.last().sealed
        || (m_chunks.last().count > 0
            && m_chunks.last().raw.size() + body.size() > m_cfg.chunkRawBytes)) {
        if (!m_chunks.isEmpty() && !m_chunks.last().sealed) sealOpenChunk();
        Chunk c;
        c.first = rec;
        c.raw.reserve(m_cfg.chunkRawBytes);
        m_chunks.append(std::move(c));
    }
    Chunk& c = m_chunks.last();
    m_offsets.append(uint32_t(c.raw.size()));
    c.raw.append(body);
    ++c.count;
    for (const PendingOp& op : ops) c.pageMasks[op.page] |= op.mask;

    m_times.append(timeMs);
    m_changed.append(changedBytes);
    m_flags.append(flags);
    m_pyramid.append(changedBytes);
}

void PageStore::sealOpenChunk() {
    if (m_chunks.isEmpty() || m_chunks.last().sealed) return;
    Chunk& c = m_chunks.last();
    c.rawSize = c.raw.size();
    if (m_cfg.compressSealed) {
        QByteArray z = qCompress(c.raw, 1);
        if (z.size() < c.raw.size() - c.raw.size() / 10) {
            c.stored = std::move(z);
            c.compressed = true;
        }
    }
    if (!c.compressed) {
        c.stored = c.raw;
        c.stored.squeeze();
    }
    c.checksum = checksum32(c.stored.constData(), size_t(c.stored.size()));
    c.raw = QByteArray();
    c.sealed = true;
    if (m_spillActive) spillChunk(c);
}

void PageStore::spillChunk(Chunk& c) {
    if (!m_spillChunks || m_spillChunks->failed() || !c.sealed || c.disk.isValid() || c.stored.isEmpty())
        return;
    const SpillLocation loc = m_spillChunks->write(c.stored);
    if (!loc.isValid()) return;            // the disk refused: it stays in RAM
    c.disk = loc;
    m_chunkDiskBytes += loc.size;
    c.stored = QByteArray();
}

void PageStore::spillSealedChunks(qint64 budgetBytes) {
    qint64 ram = stats().totalBytes();
    for (Chunk& c : m_chunks) {
        if (ram <= budgetBytes || !m_spillChunks || m_spillChunks->failed()) return;
        if (!c.sealed || c.disk.isValid()) continue;
        const qint64 held = c.stored.size();
        spillChunk(c);
        if (c.disk.isValid()) ram -= held;
    }
}

void PageStore::setSpill(std::shared_ptr<SpillFile> chunks, std::shared_ptr<SpillFile> blobs,
                         qint64 blobRamBytes) {
    if (m_spillChunks) return;             // what is on disk is addressed in these files
    m_spillChunks = std::move(chunks);
    m_spillBlobs = std::move(blobs);
    m_blobRamBytes = std::max<qint64>(0, blobRamBytes);
}

int PageStore::chunkIndexOf(RecordId r) const {
    auto it = std::upper_bound(m_chunks.cbegin(), m_chunks.cend(), r,
        [](RecordId v, const Chunk& c) { return v < c.first; });
    return int(it - m_chunks.cbegin()) - 1;
}

const QByteArray* PageStore::chunkBytes(int ci) const {
    const Chunk& c = m_chunks[ci];
    if (!c.sealed) return &c.raw;
    if (c.lost) return nullptr;
    for (auto& e : m_decodeCache)
        if (e.first == c.first) return &e.second;
    QByteArray fromDisk;
    const QByteArray* stored = &c.stored;
    if (c.disk.isValid()) {
        if (!m_spillChunks || !m_spillChunks->read(c.disk, fromDisk)) {
            c.lost = true;
            return nullptr;
        }
        stored = &fromDisk;
    }
    // Verify BEFORE decoding: a flipped byte marks the chunk lost instead of
    // handing garbage to the decompressor.
    if (checksum32(stored->constData(), size_t(stored->size())) != c.checksum) {
        c.lost = true;
        return nullptr;
    }
    QByteArray raw = c.compressed ? qUncompress(*stored) : *stored;
    if (raw.size() != c.rawSize) { c.lost = true; return nullptr; }
    if (m_decodeCache.size() >= std::max(1, m_cfg.decodeCacheChunks))
        m_decodeCache.removeFirst();
    m_decodeCache.append({c.first, std::move(raw)});
    return &m_decodeCache.last().second;
}

template <typename Fn>
QSet<PageId> PageStore::forEachOp(RecordId lo, RecordId hi, const QSet<PageId>& pages,
                                  Fn&& fn) const {
    QSet<PageId> lost;
    if (isEmpty() || pages.isEmpty() || lo > hi) return lost;
    lo = std::max(lo, m_base);
    hi = std::min(hi, m_next - 1);
    if (lo > hi) return lost;
    for (int ci = std::max(0, chunkIndexOf(lo)); ci < m_chunks.size(); ++ci) {
        const Chunk& c = m_chunks[ci];
        if (c.count == 0) continue;
        if (c.first > hi) break;
        if (c.last() < lo) continue;
        bool relevant = false;
        if (pages.size() <= c.pageMasks.size()) {
            for (PageId p : pages) if (c.pageMasks.contains(p)) { relevant = true; break; }
        } else {
            for (auto it = c.pageMasks.constBegin(); it != c.pageMasks.constEnd(); ++it)
                if (pages.contains(it.key())) { relevant = true; break; }
        }
        if (!relevant) continue;
        const QByteArray* bytes = chunkBytes(ci);
        if (!bytes) {
            for (auto it = c.pageMasks.constBegin(); it != c.pageMasks.constEnd(); ++it)
                if (pages.contains(it.key())) lost.insert(it.key());
            continue;
        }
        const RecordId from = std::max(lo, c.first);
        const RecordId to = std::min(hi, c.last());
        for (RecordId rec = from; rec <= to; ++rec) {
            const uint32_t off = m_offsets[int(rec - m_base)];
            ByteReader r(bytes->constData() + off, bytes->size() - int(off));
            uint8_t flags;
            uint32_t opCount, changed;
            if (!r.getU8(flags) || !r.getVarU32(opCount) || !r.getVarU32(changed)) break;
            int64_t page = 0;
            for (uint32_t i = 0; i < opCount; ++i) {
                int64_t delta;
                uint8_t kindByte;
                if (!r.getVarS64(delta) || !r.getU8(kindByte)) break;
                page += delta;
                const OpKind kind = OpKind(kindByte & kKindMask);
                const PageId pid = PageId(page);
                if (isXor(kind)) {
                    if (pages.contains(pid)) {
                        ByteReader payload(r.pos(), r.remaining());
                        const ByteReader before = payload;
                        fn(rec, pid, kind, payload);
                        // Advance the outer reader past the payload.
                        ByteReader skipper = before;
                        if (!skipXor(kind, skipper, int(kPageSize))) break;
                        r.skip(int(skipper.pos() - before.pos()));
                    } else if (!skipXor(kind, r, int(kPageSize))) {
                        break;
                    }
                } else if (pages.contains(pid)) {
                    ByteReader empty;
                    fn(rec, pid, kind, empty);
                }
            }
        }
    }
    return lost;
}

// ─────────────────────────────────────────────────────────────────────────
//  Reads
// ─────────────────────────────────────────────────────────────────────────

int64_t PageStore::timeOf(RecordId r) const {
    if (!contains(r)) return 0;
    return m_times[int(r - m_base)];
}

RecordId PageStore::recordAtOrBefore(int64_t timeMs) const {
    if (isEmpty()) return kNoRecord;
    auto it = std::upper_bound(m_times.cbegin(), m_times.cend(), timeMs);
    if (it == m_times.cbegin()) return kNoRecord;
    return m_base + RecordId(it - m_times.cbegin()) - 1;
}

uint32_t PageStore::changedBytesOf(RecordId r) const {
    return contains(r) ? m_changed[int(r - m_base)] : 0;
}

uint8_t PageStore::flagsOf(RecordId r) const {
    return contains(r) ? m_flags[int(r - m_base)] : 0;
}

void PageStore::replay(QVector<Need>& needs, Frame& f) const {
    if (needs.isEmpty()) return;
    QHash<PageId, int> byPage;
    QSet<PageId> pages;
    RecordId lo = kNoRecord, hi = 0;
    for (int i = 0; i < needs.size(); ++i) {
        byPage.insert(needs[i].id, i);
        pages.insert(needs[i].id);
        lo = std::min(lo, needs[i].from);
        hi = std::max(hi, needs[i].to);
    }
    const QSet<PageId> lost = forEachOp(lo, hi, pages,
        [&](RecordId rec, PageId pid, OpKind kind, ByteReader& payload) {
            const Need& n = needs[byPage.value(pid)];
            if (rec < n.from || rec > n.to || !isXor(kind)) return;
            QByteArray& bytes = f.pages[n.addr];
            if (bytes.size() != int(kPageSize)) return;
            applyXor(kind, payload, bytes.data(), int(kPageSize));
            ++m_instr.replayedOps;
        });
    for (PageId pid : lost) {
        const Need& n = needs[byPage.value(pid)];
        f.pages.remove(n.addr);
        f.states[n.addr] = PageState::Lost;
    }
}

FramePtr PageStore::frameAt(RecordId r, const QVector<uint64_t>& wantedPages) const {
    auto f = std::make_shared<Frame>();
    f->record = r;
    f->timeMs = timeOf(r);
    QVector<Need> needs;
    for (uint64_t want : wantedPages) {
        const uint64_t page = want & kPageMask;
        if (f->states.contains(page)) continue;
        auto idIt = m_pageIds.constFind(page);
        if (!contains(r) || idIt == m_pageIds.constEnd()) {
            f->states.insert(page, PageState::NotCovered);
            continue;
        }
        const PageSlot& s = m_slots[int(idIt.value())];
        const int ai = anchorAtOrBefore(s, r);
        if (ai < 0) {
            f->states.insert(page, PageState::NotCovered);
            continue;
        }
        const Anchor& a = s.anchors[ai];
        f->states.insert(page, a.state);
        if (a.state != PageState::Valid) continue;
        bool imageOk = true;
        QByteArray image = blobData(a.blob, &imageOk);
        if (!imageOk) {
            f->states.insert(page, PageState::Lost);
            continue;
        }
        f->pages.insert(page, image);
        if (a.firstOp != kNoRecord && a.firstOp <= r)
            needs.append({idIt.value(), page, a.firstOp, std::min(a.lastOp, r)});
    }
    replay(needs, *f);
    return f;
}

FramePtr PageStore::advance(const FramePtr& from, RecordId r) const {
    if (!from) return nullptr;
    if (from->record == r) return from;
    const QVector<uint64_t> pagesWanted = from->states.keys();
    if (!contains(r) || !contains(from->record)) return frameAt(r, pagesWanted);

    const RecordId lo = std::min(from->record, r) + 1;
    const RecordId hi = std::max(from->record, r);
    // Past a few MiB of records a fresh reconstruction from anchors is cheaper.
    int ci0 = chunkIndexOf(lo), ci1 = chunkIndexOf(hi);
    qint64 span = 0;
    for (int ci = std::max(0, ci0); ci <= ci1 && ci < m_chunks.size(); ++ci)
        span += m_chunks[ci].sealed ? m_chunks[ci].rawSize : m_chunks[ci].raw.size();
    if (span > 8LL * 1024 * 1024) return frameAt(r, pagesWanted);

    auto f = std::make_shared<Frame>(*from);
    f->record = r;
    f->timeMs = timeOf(r);
    QSet<PageId> pages;
    QHash<PageId, uint64_t> addrOf;
    for (uint64_t page : pagesWanted) {
        auto it = m_pageIds.constFind(page);
        if (it == m_pageIds.constEnd()) continue;
        pages.insert(it.value());
        addrOf.insert(it.value(), page);
    }
    QSet<PageId> rebuild;
    const QSet<PageId> lost = forEachOp(lo, hi, pages,
        [&](RecordId, PageId pid, OpKind kind, ByteReader& payload) {
            if (!isXor(kind)) { rebuild.insert(pid); return; }
            if (rebuild.contains(pid)) return;
            const uint64_t page = addrOf.value(pid);
            if (f->states.value(page) != PageState::Valid) { rebuild.insert(pid); return; }
            QByteArray& bytes = f->pages[page];
            applyXor(kind, payload, bytes.data(), int(kPageSize));
            ++m_instr.replayedOps;
        });
    rebuild.unite(lost);
    if (!rebuild.isEmpty()) {
        QVector<uint64_t> again;
        for (PageId pid : rebuild) again.append(addrOf.value(pid));
        FramePtr fresh = frameAt(r, again);
        for (uint64_t page : again) {
            f->pages.remove(page);
            f->states[page] = fresh->stateOf(page);
            auto pit = fresh->pages.constFind(page);
            if (pit != fresh->pages.constEnd()) f->pages.insert(page, pit.value());
        }
    }
    return f;
}

PageState PageStore::pageAt(uint64_t pageAddr, RecordId r, QByteArray* bytes) const {
    const FramePtr f = frameAt(r, {pageAddr});
    if (bytes) *bytes = f->pages.value(pageAddr & kPageMask);
    return f->stateOf(pageAddr);
}

QVector<uint64_t> PageStore::coveredPagesAt(RecordId r) const {
    QVector<uint64_t> out;
    if (!contains(r)) return out;
    for (const PageSlot& s : m_slots) {
        const int ai = anchorAtOrBefore(s, r);
        if (ai >= 0 && s.anchors[ai].state != PageState::NotCovered) out.append(s.addr);
    }
    std::sort(out.begin(), out.end());
    return out;
}

QVector<ChangedSpan> PageStore::changedSpansAt(RecordId r) const {
    QVector<ChangedSpan> out;
    if (!contains(r)) return out;
    int ci = chunkIndexOf(r);
    if (ci < 0) return out;
    const QByteArray* bytes = chunkBytes(ci);
    if (!bytes) return out;
    ByteReader rd(bytes->constData() + m_offsets[int(r - m_base)],
                  bytes->size() - int(m_offsets[int(r - m_base)]));
    uint8_t flags;
    uint32_t opCount, changed;
    if (!rd.getU8(flags) || !rd.getVarU32(opCount) || !rd.getVarU32(changed)) return out;
    int64_t page = 0;
    for (uint32_t i = 0; i < opCount; ++i) {
        int64_t delta;
        uint8_t kindByte;
        if (!rd.getVarS64(delta) || !rd.getU8(kindByte)) break;
        page += delta;
        const OpKind kind = OpKind(kindByte & kKindMask);
        if (!isXor(kind)) continue;
        if (page < 0 || page >= m_slots.size()) break;
        const uint64_t base = m_slots[int(page)].addr;
        QVector<QPair<int, int>> touched;
        if (!xorTouchedRanges(kind, rd, int(kPageSize), touched)) break;
        for (const auto& t : touched)
            out.append({base + uint64_t(t.first), uint32_t(t.second)});
    }
    std::sort(out.begin(), out.end(),
              [](const ChangedSpan& a, const ChangedSpan& b) { return a.addr < b.addr; });
    // Join spans that touch across a page boundary.
    QVector<ChangedSpan> merged;
    for (const ChangedSpan& s : out) {
        if (!merged.isEmpty()) {
            ChangedSpan& m = merged.last();
            const uint64_t mLast = m.addr + (m.len - 1);
            if (s.addr <= mLast + 1) {
                const uint64_t sLast = s.addr + (s.len - 1);
                if (sLast > mLast) m.len = uint32_t(sLast - m.addr + 1);
                continue;
            }
        }
        merged.append(s);
    }
    return merged;
}

QVector<RecordId> PageStore::changeRecordsForRange(uint64_t addr, uint64_t len,
                                                   RecordId lo, RecordId hi) const {
    QVector<RecordId> out;
    if (len == 0 || isEmpty()) return out;
    lo = std::max(lo, m_base);
    hi = std::min(hi, m_next - 1);
    if (lo > hi) return out;
    const uint64_t last = (addr > UINT64_MAX - (len - 1)) ? UINT64_MAX : addr + (len - 1);
    for (uint64_t page = addr & kPageMask;; page += kPageSize) {
        auto idIt = m_pageIds.constFind(page);
        if (idIt != m_pageIds.constEnd()) {
            const PageSlot& s = m_slots[int(idIt.value())];
            const int inStart = int(std::max(addr, page) - page);
            const int inEnd = int(std::min<uint64_t>(last, page + (kPageSize - 1)) - page);
            // Only the stretches between anchors that saw deltas can match.
            RecordId wLo = kNoRecord, wHi = 0;
            for (const Anchor& a : s.anchors) {
                if (a.firstOp == kNoRecord) continue;
                if (a.firstOp > hi || a.lastOp < lo) continue;
                wLo = std::min(wLo, std::max(a.firstOp, lo));
                wHi = std::max(wHi, std::min(a.lastOp, hi));
            }
            if (wLo != kNoRecord) {
                const PageId pid = idIt.value();
                forEachOp(wLo, wHi, QSet<PageId>{pid},
                    [&](RecordId rec, PageId, OpKind kind, ByteReader& payload) {
                        if (!isXor(kind)) return;
                        QVector<QPair<int, int>> touched;
                        if (!xorTouchedRanges(kind, payload, int(kPageSize), touched)) return;
                        for (const auto& t : touched) {
                            if (t.first <= inEnd && t.first + t.second - 1 >= inStart) {
                                out.append(rec);
                                break;
                            }
                        }
                    });
            }
        }
        if (page + (kPageSize - 1) >= last) break;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ─────────────────────────────────────────────────────────────────────────
//  Retention
// ─────────────────────────────────────────────────────────────────────────

RecordId PageStore::trimBefore(RecordId floor) {
    // Only whole chunks go, and never the open one.
    int dropChunks = 0;
    while (dropChunks < m_chunks.size() - 1
           && m_chunks[dropChunks].sealed
           && m_chunks[dropChunks].last() < floor)
        ++dropChunks;
    if (dropChunks == 0) return firstRecord();

    const RecordId newBase = m_chunks[dropChunks].first;
    const RecordId baseRec = newBase - 1;   // re-anchor "as of the end of" this record

    // Every page whose history starts before the new base gets one anchor at
    // baseRec describing its state then. Pages with deltas before the new
    // base need their bytes replayed first — while those chunks still exist.
    QVector<Need> needs;
    QHash<PageId, int> anchorIdx;
    for (int id = 0; id < m_slots.size(); ++id) {
        const PageSlot& s = m_slots[id];
        const int ai = anchorAtOrBefore(s, baseRec);
        if (ai < 0) continue;
        anchorIdx.insert(PageId(id), ai);
        const Anchor& a = s.anchors[ai];
        if (a.state == PageState::Valid && a.firstOp != kNoRecord && a.firstOp <= baseRec)
            needs.append({PageId(id), s.addr, a.firstOp, std::min(a.lastOp, baseRec)});
    }
    Frame replayed;
    for (int i = needs.size() - 1; i >= 0; --i) {
        const Need& n = needs[i];
        const Anchor& a = m_slots[int(n.id)].anchors[anchorIdx.value(n.id)];
        bool imageOk = true;
        QByteArray image = blobData(a.blob, &imageOk);
        if (!imageOk) {
            replayed.states.insert(n.addr, PageState::Lost);
            needs.removeAt(i);
            continue;
        }
        replayed.pages.insert(n.addr, image);
    }
    replay(needs, replayed);

    for (auto it = anchorIdx.constBegin(); it != anchorIdx.constEnd(); ++it) {
        PageSlot& s = m_slots[int(it.key())];
        const int ai = it.value();
        const Anchor old = s.anchors[ai];
        Anchor base;
        base.rec = baseRec;
        base.state = old.state;
        if (replayed.states.value(s.addr) == PageState::Lost) {
            base.state = PageState::Lost;
        } else if (old.state == PageState::Valid) {
            auto pit = replayed.pages.constFind(s.addr);
            if (pit != replayed.pages.constEnd()) {
                base.blob = acquireBlob(pit.value());
            } else {
                base.blob = old.blob;
                retainBlob(base.blob);
            }
        }
        // Deltas of the old stretch that fall after the new base still apply.
        if (old.lastOp != kNoRecord && old.lastOp >= newBase) {
            base.firstOp = std::max(old.firstOp, newBase);
            base.lastOp = old.lastOp;
        }
        for (int k = 0; k <= ai; ++k) releaseBlob(s.anchors[k].blob);
        s.anchors.remove(0, ai + 1);
        s.anchors.prepend(base);
    }

    int dropRecords = 0;
    for (int ci = 0; ci < dropChunks; ++ci) {
        dropRecords += m_chunks[ci].count;
        if (m_chunks[ci].disk.isValid()) {
            if (m_spillChunks) m_spillChunks->release(m_chunks[ci].disk);
            m_chunkDiskBytes -= m_chunks[ci].disk.size;
        }
        for (int k = m_decodeCache.size() - 1; k >= 0; --k)
            if (m_decodeCache[k].first == m_chunks[ci].first) m_decodeCache.removeAt(k);
    }
    m_chunks.remove(0, dropChunks);
    m_times.remove(0, dropRecords);
    m_offsets.remove(0, dropRecords);
    m_changed.remove(0, dropRecords);
    m_flags.remove(0, dropRecords);
    m_pyramid.dropFront(dropRecords);
    m_base = newBase;

    // Gaps that ended before the oldest retained moment are history too.
    if (!m_times.isEmpty()) {
        const int64_t oldest = m_times.first();
        while (!m_gaps.isEmpty() && !m_gaps.first().isOpen() && m_gaps.first().endMs <= oldest)
            m_gaps.removeFirst();
    }
    return m_base;
}

void PageStore::enforceRetention(qint64 budgetBytes, int64_t nowMs, int64_t windowMs,
                                 RecordId protectFrom, qint64 diskBudgetBytes) {
    // A recording moves sealed history to disk before anything is forgotten.
    if (m_spillActive && budgetBytes > 0 && stats().totalBytes() > budgetBytes)
        spillSealedChunks(budgetBytes);
    for (;;) {
        if (m_chunks.size() < 2) {
            // Only the open chunk: seal it so it can be dropped next time.
            if (!m_chunks.isEmpty() && budgetBytes > 0 && stats().totalBytes() > budgetBytes
                && m_chunks.last().count > 0)
                sealOpenChunk();
            return;
        }
        if (!m_chunks.first().sealed) return;
        // Copy what we need: trimBefore() erases the chunk.
        const RecordId oldestFirst = m_chunks.first().first;
        const RecordId oldestLast = m_chunks.first().last();
        const StoreStats st = stats();
        const bool overBudget = (budgetBytes > 0 && st.totalBytes() > budgetBytes)
                             || (diskBudgetBytes > 0 && st.diskBytes > diskBudgetBytes);
        const bool tooOld = windowMs > 0
            && (protectFrom == kNoRecord || oldestLast < protectFrom)
            && timeOf(oldestLast) < nowMs - windowMs;
        if (!overBudget && !tooOld) return;
        trimBefore(oldestLast + 1);
        if (m_chunks.isEmpty() || m_chunks.first().first == oldestFirst) return;   // nothing went
    }
}

bool PageStore::corruptSealedChunkForTest(int chunkIndex) {
    if (chunkIndex < 0 || chunkIndex >= m_chunks.size()) return false;
    Chunk& c = m_chunks[chunkIndex];
    if (!c.sealed || c.stored.isEmpty()) return false;
    c.stored[c.stored.size() / 2] = char(c.stored[c.stored.size() / 2] ^ 0x5A);
    for (int k = m_decodeCache.size() - 1; k >= 0; --k)
        if (m_decodeCache[k].first == c.first) m_decodeCache.removeAt(k);
    return true;
}

bool PageStore::corruptSpilledChunkForTest(int chunkIndex) {
    if (chunkIndex < 0 || chunkIndex >= m_chunks.size() || !m_spillChunks) return false;
    const Chunk& c = m_chunks[chunkIndex];
    if (!c.disk.isValid()) return false;
    QFile file(m_spillChunks->segmentPath(c.disk.segment));
    if (!file.open(QIODevice::ReadWrite)) return false;
    const qint64 at = c.disk.offset + c.disk.size / 2;
    if (!file.seek(at)) return false;
    QByteArray one = file.read(1);
    if (one.size() != 1 || !file.seek(at)) return false;
    one[0] = char(one[0] ^ 0x5A);
    if (file.write(one) != 1) return false;
    file.close();
    for (int k = m_decodeCache.size() - 1; k >= 0; --k)
        if (m_decodeCache[k].first == c.first) m_decodeCache.removeAt(k);
    return true;
}

StoreStats PageStore::stats() const {
    StoreStats s;
    s.firstRecord = firstRecord();
    s.lastRecord = lastRecord();
    s.records = m_times.size();
    s.chunks = m_chunks.size();
    s.pages = m_slots.size();
    s.blobs = int(m_blobs.size() - 1 - m_freeBlobs.size());
    for (const Chunk& c : m_chunks) {
        s.chunkBytes += c.sealed ? c.stored.size() : c.raw.size();
        s.indexBytes += qint64(c.pageMasks.size()) * 16;
        if (c.lost) ++s.lostChunks;
        if (c.disk.isValid()) ++s.spilledChunks;
    }
    s.diskBytes = m_chunkDiskBytes + m_blobDiskBytes;
    s.spilledBlobs = m_spilledBlobs;
    s.diskDegraded = (m_spillChunks && m_spillChunks->failed()) || (m_spillBlobs && m_spillBlobs->failed());
    for (const PageSlot& ps : m_slots) s.anchors += ps.anchors.size();
    s.blobBytes = m_blobBytes;
    s.indexBytes += qint64(m_times.size()) * (8 + 4 + 4 + 1)
                  + qint64(s.anchors) * 20
                  + qint64(m_slots.size()) * 64;
    s.replayedOps = m_instr.replayedOps;
    return s;
}

} // namespace rcx::tl
