#pragma once

// Where timeline work runs, and how its results come home.
//
// All work on one capture context runs on a STRAND: jobs execute one at a
// time, in order, on a private low-priority thread pool — never Qt's global
// pool, which the refresh reads and the scanner already use. The store
// therefore needs no locks. Results travel back as values, delivered on the
// UI thread, and only if the object they are addressed to still exists.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QWaitCondition>
#include <deque>
#include <functional>
#include <memory>

namespace rcx::tl {

enum class Lane {
    Interactive,   // frames for a scrub: latest-wins, served first
    Ingest,        // ticks, gaps, retention — strictly in order
    Maintenance,   // history-wide queries and rebuilds
};

class IExecutor {
public:
    virtual ~IExecutor() = default;
    virtual void post(std::function<void()> fn, Lane lane = Lane::Ingest) = 0;
    // Run fn on the UI thread if `guard` is still alive by then. `guard`
    // must have been taken on the UI thread; it is only ever read there.
    virtual void deliver(const QPointer<QObject>& guard, std::function<void()> fn) = 0;
};

// Tests: everything runs immediately, on the calling thread, in call order.
class InlineExecutor final : public IExecutor {
public:
    void post(std::function<void()> fn, Lane) override { if (fn) fn(); }
    void deliver(const QPointer<QObject>& guard, std::function<void()> fn) override {
        if (guard && fn) fn();
    }
};

// Production: a serial lane over a thread pool.
class PoolStrand final : public IExecutor, public std::enable_shared_from_this<PoolStrand> {
public:
    explicit PoolStrand(QThreadPool* pool) : m_pool(pool) {}

    void post(std::function<void()> fn, Lane lane = Lane::Ingest) override {
        if (!fn) return;
        bool schedule = false;
        {
            QMutexLocker lock(&m_mutex);
            queueFor(lane).push_back(std::move(fn));
            if (!m_running) { m_running = true; schedule = true; }
        }
        if (schedule) scheduleDrain();
    }

    void deliver(const QPointer<QObject>& guard, std::function<void()> fn) override {
        auto* app = QCoreApplication::instance();
        if (!app || !fn) return;
        QMetaObject::invokeMethod(app, [guard, fn = std::move(fn)]() {
            if (guard) fn();
        }, Qt::QueuedConnection);
    }

    // Shutdown only: wait until nothing is queued or running.
    bool waitIdle(int timeoutMs) {
        QMutexLocker lock(&m_mutex);
        QElapsedTimer t;
        t.start();
        while (m_running) {
            const qint64 left = timeoutMs - t.elapsed();
            if (left <= 0) return false;
            m_idle.wait(&m_mutex, QDeadlineTimer(left));
        }
        return true;
    }

private:
    std::deque<std::function<void()>>& queueFor(Lane lane) {
        switch (lane) {
        case Lane::Interactive: return m_interactive;
        case Lane::Maintenance: return m_maintenance;
        case Lane::Ingest:      break;
        }
        return m_ingest;
    }

    void scheduleDrain() {
        std::shared_ptr<PoolStrand> self = shared_from_this();
        m_pool->start([self]() { self->drain(); });
    }

    void drain() {
        QElapsedTimer slice;
        slice.start();
        for (;;) {
            std::function<void()> job;
            {
                QMutexLocker lock(&m_mutex);
                auto* q = !m_interactive.empty() ? &m_interactive
                        : !m_ingest.empty()      ? &m_ingest
                        : !m_maintenance.empty() ? &m_maintenance
                                                 : nullptr;
                if (!q) {
                    m_running = false;
                    m_idle.wakeAll();
                    return;
                }
                job = std::move(q->front());
                q->pop_front();
            }
            job();
            // Fairness: after a slice, give the pool thread back and requeue
            // this strand behind other contexts' strands.
            if (slice.elapsed() >= 8) {
                scheduleDrain();
                return;
            }
        }
    }

    QThreadPool* m_pool;
    QMutex m_mutex;
    QWaitCondition m_idle;
    std::deque<std::function<void()>> m_interactive, m_ingest, m_maintenance;
    bool m_running = false;
};

} // namespace rcx::tl
