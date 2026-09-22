#include "timeline_service.h"

#include <QDir>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>

namespace rcx::tl {

namespace {
bool sameOwner(const std::weak_ptr<void>& a, const std::shared_ptr<void>& b) {
    return !a.owner_before(b) && !b.owner_before(a);
}
} // namespace

TimelineService& TimelineService::instance() {
    static TimelineService s;
    return s;
}

TimelineService::TimelineService() {
    // Two threads: ingest for several live sources plus a scrub never
    // starve each other, and capture never competes with the UI.
    m_pool.setMaxThreadCount(2);
    m_pool.setThreadPriority(QThread::LowPriority);
    m_pool.setExpiryTimeout(30'000);
}

std::shared_ptr<IExecutor> TimelineService::makeExecutor() {
    if (m_factoryForTest) return m_factoryForTest();
    return std::make_shared<PoolStrand>(&m_pool);
}

void TimelineService::purge() const {
    for (int i = m_entries.size() - 1; i >= 0; --i)
        if (m_entries[i].identity.expired() || m_entries[i].context.expired())
            m_entries.removeAt(i);
}

std::shared_ptr<CaptureContext> TimelineService::contextFor(const std::shared_ptr<void>& identity) {
    if (!identity || m_shutdown) return nullptr;
    purge();
    for (const Entry& e : m_entries) {
        if (!sameOwner(e.identity, identity)) continue;
        if (auto ctx = e.context.lock()) return ctx;
    }
    std::shared_ptr<IExecutor> exec = makeExecutor();
    auto ctx = std::make_shared<CaptureContext>(exec);
    Entry e;
    e.identity = identity;
    e.context = ctx;
    e.strand = std::dynamic_pointer_cast<PoolStrand>(exec);
    m_entries.append(e);
    const quint32 stream = ++m_nextStream;
    ctx->setSpillFactory([this, stream](char kind) { return openSpill(stream, kind); });
    return ctx;
}

std::shared_ptr<SpillFile> TimelineService::openSpill(quint32 stream, char kind) {
    const TimelineBudgets b = budgets();
    const QString root = spillRoot();
    QMutexLocker lock(&m_spillMutex);
    if (m_shutdown) return nullptr;
    if (!m_spillDir) m_spillDir = SpillDir::create(root);
    if (!m_spillDir) return nullptr;
    return std::make_shared<SpillFile>(m_spillDir, stream, kind, SpillFile::kDefaultSegmentBytes,
                                       b.minFreeDiskBytes);
}

QString TimelineService::spillRoot() const {
    if (!m_spillRootForTest.isEmpty()) return m_spillRootForTest;
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return base.isEmpty() ? QString() : QDir(base).filePath(QStringLiteral("timeline"));
}

QString TimelineService::spillSessionPath() const {
    QMutexLocker lock(&m_spillMutex);
    return m_spillDir ? m_spillDir->path() : QString();
}

void TimelineService::removeStaleSpillLater() {
    const QString root = spillRoot();
    QString keep;
    {
        QMutexLocker lock(&m_spillMutex);
        if (m_spillDir) keep = m_spillDir->name();
    }
    // A live session holds its LOCK and a just-created one is young, so
    // neither can be swept even if it appears meanwhile.
    m_pool.start([root, keep]() { SpillDir::removeStaleSessions(root, 3600'000, keep); });
}

int TimelineService::liveContexts() const {
    purge();
    return m_entries.size();
}

TimelineBudgets TimelineService::budgets() const {
    if (m_haveTestBudgets) return m_testBudgets;
    QSettings s(QStringLiteral("RC"), QStringLiteral("RC"));
    TimelineBudgets b;
    b.rollingWindowMs = qMax<int64_t>(1, s.value(QStringLiteral("timeline/rollingMinutes"), 30).toLongLong()) * 60'000;
    b.rollingPerContextBytes = qMax<qint64>(4, s.value(QStringLiteral("timeline/rollingMiB"), 64).toLongLong()) << 20;
    b.rollingGlobalBytes = qMax<qint64>(16, s.value(QStringLiteral("timeline/rollingGlobalMiB"), 256).toLongLong()) << 20;
    b.recordRamBytes = qMax<qint64>(16, s.value(QStringLiteral("timeline/recordRamMiB"), 256).toLongLong()) << 20;
    b.recordDiskBytes = qMax<qint64>(64, s.value(QStringLiteral("timeline/recordDiskMiB"), 16384).toLongLong()) << 20;
    b.minFreeDiskBytes = qMax<qint64>(0, s.value(QStringLiteral("timeline/minFreeDiskMiB"), 4096).toLongLong()) << 20;
    b.captureWhenMinimized = s.value(QStringLiteral("timeline/captureWhenMinimized"), true).toBool();
    return b;
}

qint64 TimelineService::rollingBudgetPerContext() const {
    const TimelineBudgets b = budgets();
    const int n = qMax(1, liveContexts());
    return qMin(b.rollingPerContextBytes, b.rollingGlobalBytes / n);
}

void TimelineService::shutdown(int timeoutMs) {
    {
        QMutexLocker lock(&m_spillMutex);
        m_shutdown = true;
    }
    QElapsedTimer t;
    t.start();
    for (const Entry& e : std::as_const(m_entries)) {
        if (auto strand = e.strand.lock()) {
            const int left = int(qMax<qint64>(0, timeoutMs - t.elapsed()));
            strand->waitIdle(left);
        }
    }
    m_pool.waitForDone(int(qMax<qint64>(0, timeoutMs - t.elapsed())));
    QMutexLocker lock(&m_spillMutex);
    m_spillDir.reset();
}

} // namespace rcx::tl
