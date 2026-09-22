#pragma once

// ── CaptureContext: one address space's byte history, used from the UI ──
//
// One context per live data source (a Provider instance). Every tab reading
// that source is a PRODUCER feeding it, and every class those tabs show is a
// view over it — so pages several classes share are stored once, and two
// tabs on one process do not keep two histories.
//
// The store itself runs on a strand. This facade is what the UI thread
// touches: appending a tick is O(1) (pages are implicitly shared, never
// copied), frames and queries are asynchronous and addressed to a QObject,
// and a result whose addressee has died is simply dropped. Destroying the
// context never blocks: queued jobs hold the store alive and it dies on the
// pool thread after the last one.

#include "executor.h"
#include "spill_store.h"
#include "tl_model.h"
#include "tl_store.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVector>
#include <functional>
#include <memory>

namespace rcx::tl {

enum class AppendStatus {
    Accepted,
    Dropped,     // backpressure: recorded as a Dropped gap, never silently
};

struct RetentionPolicy {
    qint64   budgetBytes = 64LL << 20;          // <= 0: unlimited
    int64_t  windowMs = 30LL * 60 * 1000;       // <= 0: no age limit
    RecordId protectFrom = kNoRecord;           // a Recording pin: never dropped for age
    bool     spill = false;                     // a Recording: sealed history goes to disk
    qint64   diskBudgetBytes = 0;               // <= 0: unlimited
    qint64   blobRamBytes = 64LL << 20;         // page images held in RAM before they spill too
};

class CaptureContext {
public:
    explicit CaptureContext(std::shared_ptr<IExecutor> exec, StoreConfig cfg = {});
    ~CaptureContext();
    CaptureContext(const CaptureContext&) = delete;
    CaptureContext& operator=(const CaptureContext&) = delete;

    // ── Producers ──
    int  addProducer() { return m_nextProducer++; }
    AppendStatus append(TickInput in);
    void beginGap(int64_t nowMs, GapReason reason);
    void endGap(int64_t nowMs);
    void removeProducer(int producer, int64_t nowMs);

    // ── Policy ──
    void setRetention(const RetentionPolicy& policy);
    // Several hubs can share one context (a document's tabs and an instance
    // tab on the same process): each states its own policy, and the context
    // keeps what the most demanding one needs — the oldest pin, disk if any
    // of them records, the largest budgets. `owner` only identifies the hub.
    void setRetentionFor(const void* owner, const RetentionPolicy& policy);
    void clearRetentionFor(const void* owner);
private:
    QHash<const void*, RetentionPolicy> m_policies;
    void applyPolicies();
public:
    // Where a recording's spill files come from ('c' chunks, 'b' images).
    // Called on the strand, once, the first time a policy asks to spill.
    using SpillFactory = std::function<std::shared_ptr<SpillFile>(char kind)>;
    void setSpillFactory(SpillFactory factory);
    void setWantSpans(bool on);
    void setBackpressureLimits(int maxPendingTicks, qint64 maxPendingBytes);

    // ── Reads (asynchronous; `ctx` must be a UI-thread object) ──
    using FrameCallback = std::function<void(FramePtr)>;
    // Latest wins per requester: only the newest outstanding request of a
    // requester is ever computed. Nearby requests step the previous frame
    // instead of rebuilding it. Empty `wantedPages` means every page that
    // was covered at `r`. The frame carries the spans that changed at `r`.
    void requestFrame(int requester, RecordId r, QVector<uint64_t> wantedPages,
                      QObject* ctx, FrameCallback cb);
    void cancelFrames(int requester);
    void queryChangeRecords(uint64_t addr, uint64_t len, RecordId lo, RecordId hi,
                            QObject* ctx, std::function<void(QVector<RecordId>)> cb);
    using SpanList = QVector<QPair<RecordId, QVector<ChangedSpan>>>;
    void querySpans(RecordId lo, RecordId hi, QObject* ctx, std::function<void(SpanList)> cb);

    // ── UI-side index ──
    const TimelineModel& model() const { return m_model; }
    using Listener = std::function<void(const CommitBatch&)>;
    int  addListener(Listener fn);
    void removeListener(int id);

private:
    struct Core;
    void post(std::function<void(Core&)> job, Lane lane, bool commitAfter);
    void onCommit(const CommitBatch& b);

    std::shared_ptr<IExecutor> m_exec;
    std::shared_ptr<Core>      m_core;
    QObject                    m_mailbox;   // guards deliveries to this context
    TimelineModel              m_model;
    QHash<int, Listener>       m_listeners;
    int  m_nextListener = 1;
    int  m_nextProducer = 1;
    bool m_inDropGap = false;
};

} // namespace rcx::tl
