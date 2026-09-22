#pragma once

// ── TimelineService: the process-wide half of the timeline ──
//
// Owns the private worker pool, hands out one CaptureContext per data source
// (so every tab on the same process shares one history), and reads the
// budgets from settings. It never holds a Provider: a source is identified by
// a weak reference to whatever object the caller says stands for it.

#include "capture_context.h"
#include "executor.h"
#include "spill_store.h"

#include <QMutex>
#include <QThreadPool>
#include <QVector>
#include <functional>
#include <memory>

namespace rcx::tl {

struct TimelineBudgets {
    int64_t rollingWindowMs        = 30LL * 60 * 1000;
    qint64  rollingPerContextBytes = 64LL << 20;
    qint64  rollingGlobalBytes     = 256LL << 20;
    qint64  recordRamBytes         = 256LL << 20;
    qint64  recordDiskBytes        = 16LL << 30;
    qint64  minFreeDiskBytes       = 4LL << 30;   // below it a recording stays in RAM
    bool    captureWhenMinimized   = true;
};

class TimelineService {
public:
    static TimelineService& instance();

    // One context per source identity, alive while anyone holds it.
    std::shared_ptr<CaptureContext> contextFor(const std::shared_ptr<void>& identity);
    int liveContexts() const;

    // Budgets from QSettings("RC","RC") timeline/*.
    TimelineBudgets budgets() const;
    void setBudgetsForTest(const TimelineBudgets& b) { m_testBudgets = b; m_haveTestBudgets = true; }
    // The rolling budget one context gets: its own cap, but never more than a
    // fair share of the global cap.
    qint64 rollingBudgetPerContext() const;

    // Tests run everything inline and deterministically.
    void setExecutorFactoryForTest(std::function<std::shared_ptr<IExecutor>()> factory) {
        m_factoryForTest = std::move(factory);
    }

    // App quit: stop taking work and give queued jobs a bounded time. The
    // session's spill directory goes with the last file still using it.
    void shutdown(int timeoutMs);

    // Recordings spill under <CacheLocation>/timeline; tests point it elsewhere.
    QString spillRoot() const;
    void setSpillRootForTest(const QString& root) { m_spillRootForTest = root; }
    // This session's spill directory once something has spilled, else empty.
    QString spillSessionPath() const;
    // Startup maintenance, on the pool: remove what crashed sessions left.
    void removeStaleSpillLater();

private:
    TimelineService();
    std::shared_ptr<IExecutor> makeExecutor();
    void purge() const;
    std::shared_ptr<SpillFile> openSpill(quint32 stream, char kind);

    QThreadPool m_pool;
    struct Entry {
        std::weak_ptr<void> identity;
        std::weak_ptr<CaptureContext> context;
        std::weak_ptr<PoolStrand> strand;
    };
    mutable QVector<Entry> m_entries;
    std::function<std::shared_ptr<IExecutor>()> m_factoryForTest;
    TimelineBudgets m_testBudgets;
    bool m_haveTestBudgets = false;
    bool m_shutdown = false;          // written under m_spillMutex

    mutable QMutex m_spillMutex;      // spill files open on pool threads
    std::shared_ptr<SpillDir> m_spillDir;
    QString m_spillRootForTest;
    quint32 m_nextStream = 0;
};

} // namespace rcx::tl
