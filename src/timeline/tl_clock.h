#pragma once

// One monotonic clock for every capture in the process.
//
// Every tab's read loop stamps its samples with this clock, so records from
// different producers of the same address space order correctly, and a
// timeline never jumps when the wall clock is adjusted. Wall time is only
// for labels ("12:04:31.2"): convert with toEpochMs at display time.

#include <QDateTime>
#include <QElapsedTimer>
#include <atomic>
#include <cstdint>

namespace rcx::tl {

class CaptureClock {
public:
    static int64_t nowMs() {
        const int64_t forced = testNow().load(std::memory_order_relaxed);
        return forced >= 0 ? forced : timer().elapsed();
    }
    static int64_t toEpochMs(int64_t captureMs) { return epochAtZero() + captureMs; }

    // Tests drive time explicitly. A negative value restores the real clock.
    static void setTestNow(int64_t ms) { testNow().store(ms, std::memory_order_relaxed); }

private:
    static QElapsedTimer& timer() {
        static QElapsedTimer t = [] { QElapsedTimer e; e.start(); return e; }();
        return t;
    }
    static int64_t epochAtZero() {
        static const int64_t e = QDateTime::currentMSecsSinceEpoch() - timer().elapsed();
        return e;
    }
    static std::atomic<int64_t>& testNow() {
        static std::atomic<int64_t> v{-1};
        return v;
    }
};

} // namespace rcx::tl
