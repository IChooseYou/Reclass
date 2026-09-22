#include "capture_context.h"

#include <QMutex>
#include <atomic>

namespace rcx::tl {

struct CaptureContext::Core {
    explicit Core(StoreConfig cfg) : store(std::move(cfg)) {}

    // Strand-only state.
    PageStore       store;
    RetentionPolicy retention;
    bool            wantSpans = false;
    RecordId        lastFirstRetained = kNoRecord;
    int             lastGapCount = 0;
    int64_t         lastGapEnd = -2;
    struct Cursor { QVector<uint64_t> wanted; FramePtr frame; };
    QHash<int, Cursor> cursors;
    SpillFactory    spillFactory;
    bool            spillOpened = false;

    // Disk only while a recording asks for it; its files open on first need.
    void applySpill() {
        if (retention.spill && !spillOpened && spillFactory) {
            spillOpened = true;
            std::shared_ptr<SpillFile> chunks = spillFactory('c');
            std::shared_ptr<SpillFile> blobs = spillFactory('b');
            if (chunks && blobs) store.setSpill(std::move(chunks), std::move(blobs), retention.blobRamBytes);
        }
        store.setSpillActive(retention.spill);
    }

    // Crossing threads.
    std::atomic<int>    pendingTicks{0};
    std::atomic<qint64> pendingBytes{0};
    std::atomic<int>    maxPendingTicks{64};
    std::atomic<qint64> maxPendingBytes{256LL << 20};

    QMutex frameMutex;
    struct FrameSlot {
        quint64 seq = 0;
        RecordId r = kNoRecord;
        QVector<uint64_t> wanted;
        QPointer<QObject> guard;
        FrameCallback cb;
    };
    QHash<int, FrameSlot> frameSlots;   // guarded by frameMutex
    quint64 frameSeq = 0;               // guarded by frameMutex

    // A commit worth sending home? Only when something the UI mirrors moved:
    // idle ticks must not wake the UI thread.
    CommitBatch makeBatch(RecordId appended, bool force) {
        CommitBatch b;
        const RecordId first = store.firstRecord();
        const int gapCount = store.gaps().size();
        const int64_t gapEnd = gapCount ? store.gaps().last().endMs : -2;
        const bool changed = appended != kNoRecord || first != lastFirstRetained
                          || gapCount != lastGapCount || gapEnd != lastGapEnd;
        if (!changed && !force) return b;
        if (appended != kNoRecord) {
            b.appended.append({appended, store.timeOf(appended),
                               store.changedBytesOf(appended), store.flagsOf(appended)});
            if (wantSpans) b.spans.append({appended, store.changedSpansAt(appended)});
        }
        b.firstRetained = first;
        b.gaps = store.gaps();
        b.stats = store.stats();
        lastFirstRetained = first;
        lastGapCount = gapCount;
        lastGapEnd = gapEnd;
        return b;
    }
};

namespace {
bool batchIsEmpty(const CommitBatch& b) {
    return b.appended.isEmpty() && b.firstRetained == kNoRecord && b.gaps.isEmpty()
        && b.stats.records == 0 && b.stats.chunks == 0;
}
} // namespace

CaptureContext::CaptureContext(std::shared_ptr<IExecutor> exec, StoreConfig cfg)
    : m_exec(std::move(exec))
    , m_core(std::make_shared<Core>(std::move(cfg))) {}

CaptureContext::~CaptureContext() {
    // Outstanding frame requests are addressed to objects that may outlive
    // us; drop them. Queued jobs keep the core alive; their deliveries die
    // with m_mailbox.
    QMutexLocker lock(&m_core->frameMutex);
    m_core->frameSlots.clear();
}

void CaptureContext::post(std::function<void(Core&)> job, Lane lane, bool commitAfter) {
    std::shared_ptr<Core> core = m_core;
    std::shared_ptr<IExecutor> exec = m_exec;
    QPointer<QObject> guard(&m_mailbox);
    CaptureContext* self = this;
    m_exec->post([core, exec, guard, self, job = std::move(job), commitAfter]() {
        job(*core);
        if (!commitAfter) return;
        CommitBatch b = core->makeBatch(kNoRecord, false);
        if (batchIsEmpty(b)) return;
        exec->deliver(guard, [self, b]() { self->onCommit(b); });
    }, lane);
}

AppendStatus CaptureContext::append(TickInput in) {
    qint64 bytes = 0;
    for (auto it = in.pages.constBegin(); it != in.pages.constEnd(); ++it) bytes += it.value().size();
    Core& c = *m_core;
    if (c.pendingTicks.load() >= c.maxPendingTicks.load()
        || c.pendingBytes.load() + bytes > c.maxPendingBytes.load()) {
        if (!m_inDropGap) {
            m_inDropGap = true;
            beginGap(in.timeMs, GapReason::Dropped);
        }
        return AppendStatus::Dropped;
    }
    if (m_inDropGap) {
        m_inDropGap = false;
        endGap(in.timeMs);
    }
    c.pendingTicks.fetch_add(1);
    c.pendingBytes.fetch_add(bytes);

    std::shared_ptr<Core> core = m_core;
    std::shared_ptr<IExecutor> exec = m_exec;
    QPointer<QObject> guard(&m_mailbox);
    CaptureContext* self = this;
    m_exec->post([core, exec, guard, self, in = std::move(in), bytes]() {
        const RecordId r = core->store.ingest(in);
        const RetentionPolicy& p = core->retention;
        core->store.enforceRetention(p.budgetBytes, in.timeMs, p.windowMs, p.protectFrom,
                                     p.spill ? p.diskBudgetBytes : 0);
        CommitBatch b = core->makeBatch(r, false);
        core->pendingTicks.fetch_sub(1);
        core->pendingBytes.fetch_sub(bytes);
        if (batchIsEmpty(b)) return;
        exec->deliver(guard, [self, b]() { self->onCommit(b); });
    }, Lane::Ingest);
    return AppendStatus::Accepted;
}

void CaptureContext::beginGap(int64_t nowMs, GapReason reason) {
    post([nowMs, reason](Core& c) { c.store.beginGap(nowMs, reason); }, Lane::Ingest, true);
}

void CaptureContext::endGap(int64_t nowMs) {
    post([nowMs](Core& c) { c.store.endGap(nowMs); }, Lane::Ingest, true);
}

void CaptureContext::removeProducer(int producer, int64_t nowMs) {
    std::shared_ptr<Core> core = m_core;
    std::shared_ptr<IExecutor> exec = m_exec;
    QPointer<QObject> guard(&m_mailbox);
    CaptureContext* self = this;
    m_exec->post([core, exec, guard, self, producer, nowMs]() {
        const RecordId r = core->store.removeProducer(producer, nowMs);
        CommitBatch b = core->makeBatch(r, false);
        if (batchIsEmpty(b)) return;
        exec->deliver(guard, [self, b]() { self->onCommit(b); });
    }, Lane::Ingest);
}

void CaptureContext::setRetention(const RetentionPolicy& policy) {
    post([policy](Core& c) { c.retention = policy; c.applySpill(); }, Lane::Ingest, false);
}

void CaptureContext::setRetentionFor(const void* owner, const RetentionPolicy& policy) {
    m_policies.insert(owner, policy);
    applyPolicies();
}

void CaptureContext::clearRetentionFor(const void* owner) {
    if (m_policies.remove(owner)) applyPolicies();
}

void CaptureContext::applyPolicies() {
    if (m_policies.isEmpty()) return;
    // <= 0 is "no limit", which outranks any limit.
    auto most = [](qint64 a, qint64 b) -> qint64 { return (a <= 0 || b <= 0) ? 0 : std::max(a, b); };
    RetentionPolicy p;
    bool first = true;
    bool spill = false;
    qint64 disk = 0;
    for (const RetentionPolicy& q : std::as_const(m_policies)) {
        if (first) {
            p = q;
            first = false;
        } else {
            p.budgetBytes = most(p.budgetBytes, q.budgetBytes);
            p.windowMs = most(p.windowMs, q.windowMs);
            p.protectFrom = std::min(p.protectFrom, q.protectFrom);
            p.blobRamBytes = std::max(p.blobRamBytes, q.blobRamBytes);
        }
        // A disk budget only means something for a policy that spills.
        if (q.spill) {
            disk = spill ? most(disk, q.diskBudgetBytes) : q.diskBudgetBytes;
            spill = true;
        }
    }
    p.spill = spill;
    p.diskBudgetBytes = spill ? disk : 0;
    setRetention(p);
}

void CaptureContext::setSpillFactory(SpillFactory factory) {
    post([factory = std::move(factory)](Core& c) { c.spillFactory = factory; c.applySpill(); },
         Lane::Ingest, false);
}

void CaptureContext::setWantSpans(bool on) {
    post([on](Core& c) { c.wantSpans = on; }, Lane::Ingest, false);
}

void CaptureContext::setBackpressureLimits(int maxPendingTicks, qint64 maxPendingBytes) {
    m_core->maxPendingTicks.store(maxPendingTicks);
    m_core->maxPendingBytes.store(maxPendingBytes);
}

void CaptureContext::requestFrame(int requester, RecordId r, QVector<uint64_t> wantedPages,
                                  QObject* ctx, FrameCallback cb) {
    std::shared_ptr<Core> core = m_core;
    quint64 seq;
    {
        QMutexLocker lock(&core->frameMutex);
        Core::FrameSlot& slot = core->frameSlots[requester];
        slot.seq = ++core->frameSeq;
        slot.r = r;
        slot.wanted = std::move(wantedPages);
        slot.guard = QPointer<QObject>(ctx);
        slot.cb = std::move(cb);
        seq = slot.seq;
    }
    std::shared_ptr<IExecutor> exec = m_exec;
    m_exec->post([core, exec, requester, seq]() {
        Core::FrameSlot slot;
        {
            QMutexLocker lock(&core->frameMutex);
            auto it = core->frameSlots.find(requester);
            if (it == core->frameSlots.end() || it->seq != seq) return;   // superseded
            slot = *it;
            core->frameSlots.erase(it);
        }
        if (slot.wanted.isEmpty()) slot.wanted = core->store.coveredPagesAt(slot.r);
        FramePtr stepped;
        auto cur = core->cursors.constFind(requester);
        if (cur != core->cursors.constEnd() && cur->frame && cur->wanted == slot.wanted)
            stepped = core->store.advance(cur->frame, slot.r);
        else
            stepped = core->store.frameAt(slot.r, slot.wanted);
        auto withSpans = std::make_shared<Frame>(*stepped);
        withSpans->changedAt = core->store.changedSpansAt(slot.r);
        FramePtr frame = withSpans;
        core->cursors.insert(requester, Core::Cursor{slot.wanted, frame});
        FrameCallback cb = slot.cb;
        exec->deliver(slot.guard, [cb, frame]() { if (cb) cb(frame); });
    }, Lane::Interactive);
}

void CaptureContext::cancelFrames(int requester) {
    {
        QMutexLocker lock(&m_core->frameMutex);
        m_core->frameSlots.remove(requester);
    }
    post([requester](Core& c) { c.cursors.remove(requester); }, Lane::Interactive, false);
}

void CaptureContext::queryChangeRecords(uint64_t addr, uint64_t len, RecordId lo, RecordId hi,
                                        QObject* ctx, std::function<void(QVector<RecordId>)> cb) {
    std::shared_ptr<Core> core = m_core;
    std::shared_ptr<IExecutor> exec = m_exec;
    QPointer<QObject> guard(ctx);
    m_exec->post([core, exec, guard, addr, len, lo, hi, cb]() {
        QVector<RecordId> out = core->store.changeRecordsForRange(addr, len, lo, hi);
        exec->deliver(guard, [cb, out]() { if (cb) cb(out); });
    }, Lane::Maintenance);
}

void CaptureContext::querySpans(RecordId lo, RecordId hi, QObject* ctx,
                                std::function<void(SpanList)> cb) {
    std::shared_ptr<Core> core = m_core;
    std::shared_ptr<IExecutor> exec = m_exec;
    QPointer<QObject> guard(ctx);
    m_exec->post([core, exec, guard, lo, hi, cb]() {
        SpanList out;
        const PageStore& s = core->store;
        if (!s.isEmpty()) {
            const RecordId from = std::max(lo, s.firstRecord());
            const RecordId to = std::min(hi, s.lastRecord());
            for (RecordId r = from; r <= to && r != kNoRecord; ++r) {
                if (s.changedBytesOf(r) == 0) continue;
                out.append({r, s.changedSpansAt(r)});
                if (r == to) break;
            }
        }
        exec->deliver(guard, [cb, out]() { if (cb) cb(out); });
    }, Lane::Maintenance);
}

int CaptureContext::addListener(Listener fn) {
    const int id = m_nextListener++;
    m_listeners.insert(id, std::move(fn));
    return id;
}

void CaptureContext::removeListener(int id) {
    m_listeners.remove(id);
}

void CaptureContext::onCommit(const CommitBatch& b) {
    m_model.apply(b);
    // Copy: a listener may add or remove listeners.
    const auto listeners = m_listeners;
    for (auto it = listeners.constBegin(); it != listeners.constEnd(); ++it)
        if (it.value()) it.value()(b);
}

} // namespace rcx::tl
