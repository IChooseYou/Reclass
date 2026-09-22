// The timeline's byte store, tested against a model that cannot be wrong.
//
// The store is clever — XOR deltas, per-page anchors, deduplicated images,
// sealed and compressed chunks, retention that re-anchors — so the main
// test does not reason about any of that. It drives a simulated address
// space tick by tick, keeps a plain copy of every page's state after every
// record, and demands that any record reconstructs to exactly that copy, in
// any order, stepped either direction, after trims. The targeted tests below
// it pin the individual promises the controller and the UI rely on.

#include <QtTest/QTest>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <random>

#include <QCoreApplication>
#include <QThread>
#include <atomic>

#include "timeline/tl_varint.h"
#include "timeline/tl_xorrun.h"
#include "timeline/tl_pyramid.h"
#include "timeline/tl_store.h"
#include "timeline/tl_model.h"
#include "timeline/capture_context.h"
#include "timeline/timeline_service.h"
#include "timeline/timeline_hub.h"
#include "timeline/frame_provider.h"
#include "timeline/class_changes.h"
#include "timeline/spill_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using namespace rcx::tl;

namespace {

// Holds every job until the test says run — the only way to observe
// ordering, latest-wins and backpressure deterministically.
class ManualExecutor final : public IExecutor {
public:
    struct Job { std::function<void()> fn; Lane lane; };
    QVector<Job> jobs;
    QVector<std::function<void()>> deliveries;

    void post(std::function<void()> fn, Lane lane) override { jobs.append({std::move(fn), lane}); }
    void deliver(const QPointer<QObject>& guard, std::function<void()> fn) override {
        deliveries.append([guard, fn]() { if (guard && fn) fn(); });
    }
    int runAll() {
        int n = 0;
        while (!jobs.isEmpty() || !deliveries.isEmpty()) {
            while (!jobs.isEmpty()) { Job j = jobs.takeFirst(); j.fn(); ++n; }
            while (!deliveries.isEmpty()) { auto d = deliveries.takeFirst(); d(); }
        }
        return n;
    }
};

struct ModelPage {
    PageState  state = PageState::NotCovered;
    QByteArray bytes;
};
using ModelState = QHash<uint64_t, ModelPage>;

bool samePage(const ModelPage& a, const ModelPage& b) {
    if (a.state != b.state) return false;
    return a.state != PageState::Valid || a.bytes == b.bytes;
}

bool sameModel(const ModelState& a, const ModelState& b, const QVector<uint64_t>& addrs) {
    for (uint64_t p : addrs)
        if (!samePage(a.value(p), b.value(p))) return false;
    return true;
}

const char* stateName(PageState s) {
    switch (s) {
    case PageState::NotCovered:    return "NotCovered";
    case PageState::NotYetSampled: return "NotYetSampled";
    case PageState::Valid:         return "Valid";
    case PageState::Unreadable:    return "Unreadable";
    case PageState::Lost:          return "Lost";
    }
    return "?";
}

// Empty string when the frame matches the model for every address.
QString frameMismatch(const Frame& f, const ModelState& m, const QVector<uint64_t>& addrs) {
    for (uint64_t p : addrs) {
        const ModelPage want = m.value(p);
        const PageState got = f.stateOf(p);
        if (got != want.state)
            return QStringLiteral("page %1 at record %2: state %3, expected %4")
                .arg(p, 0, 16).arg(f.record).arg(stateName(got)).arg(stateName(want.state));
        if (want.state == PageState::Valid && f.pages.value(p) != want.bytes)
            return QStringLiteral("page %1 at record %2: bytes differ").arg(p, 0, 16).arg(f.record);
    }
    return {};
}

QByteArray randomPage(std::mt19937& rng) {
    QByteArray b(int(kPageSize), '\0');
    // Mostly structured data: runs of zeros and small values, like real memory.
    for (int i = 0; i < b.size(); ++i)
        b[i] = (rng() % 3 == 0) ? char(rng() & 0xFF) : char(0);
    return b;
}

// A simulated address space and a single producer reading it.
struct Sim {
    std::mt19937 rng;
    QVector<uint64_t> addrs;
    QHash<uint64_t, QByteArray> memory;
    QSet<uint64_t> covered;
    QSet<uint64_t> refusing;
    ModelState model;
    int64_t now = 10'000;

    explicit Sim(uint32_t seed) : rng(seed) {
        addrs = { 0x0, 0x1000, 0x2000, 0x7FF6DEAD0000ull, 0x7FF6DEAD1000ull,
                  0x7FF6DEAD2000ull, 0x10000000ull, 0x10003000ull,
                  0xFFFFFFFFFFFFE000ull, 0xFFFFFFFFFFFFF000ull };
        for (uint64_t a : addrs) memory.insert(a, randomPage(rng));
        for (int i = 0; i < 6; ++i) covered.insert(addrs[i]);
    }

    void mutateMemory() {
        for (uint64_t a : addrs) {
            if (rng() % 4) continue;
            QByteArray& b = memory[a];
            const int kind = int(rng() % 4);
            if (kind == 0) {                      // one field
                const int off = int(rng() % (kPageSize - 8));
                for (int i = 0; i < 4; ++i) b[off + i] = char(rng() & 0xFF);
            } else if (kind == 1) {               // a few scattered bytes
                for (int i = 0; i < 6; ++i) b[int(rng() % kPageSize)] = char(rng() & 0xFF);
            } else if (kind == 2) {               // a long run
                const int off = int(rng() % 3000);
                const int len = 200 + int(rng() % 800);
                for (int i = 0; i < len; ++i) b[off + i] = char(rng() & 0xFF);
            } else {                              // the whole page
                b = randomPage(rng);
            }
        }
    }

    bool declared = false;

    TickInput step(bool& expectChange, bool allowCoverage = true) {
        now += 100;
        const ModelState before = model;
        TickInput in;
        in.timeMs = now;
        in.readStartMs = now;

        mutateMemory();

        // A producer declares what it intends to read on its first tick, as
        // the controller does: those pages are covered, just not sampled yet.
        if (!declared) {
            declared = true;
            QVector<uint64_t> list(covered.begin(), covered.end());
            std::sort(list.begin(), list.end());
            in.coverage = list;
            for (uint64_t a : list) model[a] = ModelPage{PageState::NotYetSampled, {}};
        } else if (allowCoverage && rng() % 10 == 0) {
            const bool removeOne = !covered.isEmpty() && (rng() % 2 || covered.size() == addrs.size());
            if (removeOne) {
                QVector<uint64_t> c(covered.begin(), covered.end());
                std::sort(c.begin(), c.end());
                const uint64_t gone = c[int(rng() % c.size())];
                covered.remove(gone);
                model[gone] = ModelPage{};
            } else {
                QVector<uint64_t> free;
                for (uint64_t a : addrs) if (!covered.contains(a)) free.append(a);
                const uint64_t fresh = free[int(rng() % free.size())];
                covered.insert(fresh);
                model[fresh] = ModelPage{PageState::NotYetSampled, {}};
            }
            QVector<uint64_t> list(covered.begin(), covered.end());
            std::sort(list.begin(), list.end());
            in.coverage = list;
        }

        if (rng() % 15 == 0) {
            const uint64_t a = addrs[int(rng() % addrs.size())];
            if (refusing.contains(a)) refusing.remove(a); else refusing.insert(a);
        }

        QVector<uint64_t> list(covered.begin(), covered.end());
        std::sort(list.begin(), list.end());
        for (uint64_t a : list) {
            if (rng() % 10 >= 7) continue;       // this page skipped this tick
            if (refusing.contains(a)) {
                in.unreadable.append(a);
                model[a] = ModelPage{PageState::Unreadable, {}};
            } else {
                in.pages.insert(a, memory.value(a));
                model[a] = ModelPage{PageState::Valid, memory.value(a)};
            }
        }
        expectChange = !sameModel(before, model, addrs);
        return in;
    }
};

// Run a simulation into `store`, returning the model after every record.
QHash<RecordId, ModelState> runSim(PageStore& store, Sim& sim, int ticks) {
    QHash<RecordId, ModelState> snaps;
    for (int t = 0; t < ticks; ++t) {
        bool expect = false;
        const TickInput in = sim.step(expect);
        const RecordId r = store.ingest(in);
        if ((r != kNoRecord) != expect)
            qFatal("tick %d: store %s a record but the model %s",
                   t, r != kNoRecord ? "wrote" : "skipped",
                   expect ? "changed" : "did not change");
        if (r != kNoRecord) snaps.insert(r, sim.model);
    }
    return snaps;
}

TickInput tickWith(int64_t t, std::initializer_list<std::pair<uint64_t, QByteArray>> pages) {
    TickInput in;
    in.timeMs = t;
    in.readStartMs = t;
    for (const auto& p : pages) in.pages.insert(p.first, p.second);
    return in;
}

} // namespace

class TestTimeline : public QObject {
    Q_OBJECT
private slots:

    // ── Encoding primitives ──

    void varintRoundTripsAndRejectsTruncation() {
        const uint64_t values[] = { 0, 1, 127, 128, 16383, 16384, 0xFFFFFFFFull,
                                    0x100000000ull, UINT64_MAX };
        QByteArray buf;
        for (uint64_t v : values) putVarU64(buf, v);
        putVarS64(buf, -1);
        putVarS64(buf, INT64_MIN);
        putVarS64(buf, INT64_MAX);
        ByteReader r(buf.constData(), buf.size());
        for (uint64_t v : values) {
            uint64_t got;
            QVERIFY(r.getVarU64(got));
            QCOMPARE(got, v);
            QCOMPARE(varU64Size(v), (v < 128) ? 1 : varU64Size(v));
        }
        int64_t s;
        QVERIFY(r.getVarS64(s)); QCOMPARE(s, int64_t(-1));
        QVERIFY(r.getVarS64(s)); QCOMPARE(s, INT64_MIN);
        QVERIFY(r.getVarS64(s)); QCOMPARE(s, INT64_MAX);
        QVERIFY(r.atEnd());

        // Truncated in the middle of a varint: the reader fails, stays failed.
        QByteArray cut;
        putVarU64(cut, UINT64_MAX);
        cut.chop(1);
        ByteReader t(cut.constData(), cut.size());
        uint64_t dummy;
        QVERIFY(!t.getVarU64(dummy));
        QVERIFY(!t.ok());
        uint8_t b;
        QVERIFY(!t.getU8(b));

        // A u32 read of a value that does not fit fails rather than truncating.
        QByteArray big;
        putVarU64(big, 0x1FFFFFFFFull);
        ByteReader u(big.constData(), big.size());
        uint32_t u32;
        QVERIFY(!u.getVarU32(u32));
    }

    // XOR deltas: the same payload steps a page forward AND back, the
    // touched ranges are exactly the differing bytes, and a delta is never
    // bigger than the page.
    void xorDeltaStepsBothWaysAndReportsExactRanges() {
        std::mt19937 rng(0xFEEDu);
        for (int round = 0; round < 400; ++round) {
            QByteArray oldP = randomPage(rng);
            QByteArray newP = oldP;
            const int mode = round % 4;
            if (mode == 0) {
                for (int i = 0; i < 3; ++i) newP[int(rng() % kPageSize)] = char(rng() & 0xFF);
            } else if (mode == 1) {
                const int off = int(rng() % 4000);
                for (int i = 0; i < 4 && off + i < int(kPageSize); ++i) newP[off + i] = char(~newP[off + i]);
            } else if (mode == 2) {
                for (int i = 0; i < int(kPageSize); ++i) newP[i] = char(rng() & 0xFF);
            } else {
                // Changes exactly 2 apart: the equal byte between must be merged.
                newP[100] = char(~newP[100]);
                newP[102] = char(~newP[102]);
            }

            QByteArray payload;
            XorStats st;
            const OpKind kind = encodeXor(oldP.constData(), newP.constData(), int(kPageSize), payload, &st);
            QVERIFY(payload.size() <= int(kPageSize) + 8);

            uint32_t naiveChanged = 0;
            QSet<int> naive;
            for (int i = 0; i < int(kPageSize); ++i)
                if (oldP[i] != newP[i]) { ++naiveChanged; naive.insert(i); }
            QCOMPARE(st.changedBytes, naiveChanged);

            QByteArray fwd = oldP;
            ByteReader r1(payload.constData(), payload.size());
            QVERIFY(applyXor(kind, r1, fwd.data(), int(kPageSize)));
            QCOMPARE(fwd, newP);
            QVERIFY(r1.atEnd());

            QByteArray back = newP;
            ByteReader r2(payload.constData(), payload.size());
            QVERIFY(applyXor(kind, r2, back.data(), int(kPageSize)));
            QCOMPARE(back, oldP);

            QVector<QPair<int, int>> touched;
            ByteReader r3(payload.constData(), payload.size());
            QVERIFY(xorTouchedRanges(kind, r3, int(kPageSize), touched));
            QSet<int> covered;
            for (const auto& t : touched)
                for (int i = t.first; i < t.first + t.second; ++i) covered.insert(i);
            // Every changed byte is covered, and each range starts and ends
            // on a changed byte (merge-gap zeros only ever sit inside).
            for (int i : naive) QVERIFY(covered.contains(i));
            for (const auto& t : touched) {
                QVERIFY(naive.contains(t.first));
                QVERIFY(naive.contains(t.first + t.second - 1));
            }
            if (mode == 3) {
                QCOMPARE(touched.size(), 1);
                QCOMPARE(touched[0], qMakePair(100, 3));
            }
        }
    }

    void xorPayloadRejectsRunsThatLeaveThePage() {
        QByteArray bad;
        putVarU32(bad, 1);        // one run
        putVarU32(bad, 4090);     // gap
        putVarU32(bad, 16);       // len → past the end
        bad.append(QByteArray(16, 'x'));
        QByteArray page(int(kPageSize), '\0');
        ByteReader r(bad.constData(), bad.size());
        QVERIFY(!applyXor(OpKind::XorRuns, r, page.data(), int(kPageSize)));
    }

    void pyramidMatchesNaive() {
        std::mt19937 rng(0xABCDu);
        MaxSumPyramid p;
        QVector<uint32_t> values;
        for (int n = 0; n < 5000; ++n) {
            const uint32_t v = (rng() % 5 == 0) ? rng() % 100000 : rng() % 10;
            p.append(v);
            values.append(v);
            if (n % 97 != 0) continue;
            for (int q = 0; q < 20; ++q) {
                int lo = int(rng() % values.size());
                int hi = lo + int(rng() % (values.size() - lo + 1));
                uint32_t mx = 0;
                uint64_t sum = 0;
                for (int i = lo; i < hi; ++i) { mx = std::max(mx, values[i]); sum += values[i]; }
                const auto a = p.query(lo, hi);
                QCOMPARE(a.max, mx);
                QCOMPARE(a.sum, sum);
                QCOMPARE(a.count, hi - lo);
            }
        }
        p.dropFront(1234);
        values.remove(0, 1234);
        QCOMPARE(p.size(), values.size());
        const auto all = p.query(0, p.size());
        uint64_t sum = 0;
        for (uint32_t v : values) sum += v;
        QCOMPARE(all.sum, sum);
    }

    // ── The store against the model ──

    // THE correctness test. Random mutations, coverage entering and
    // leaving, reads refused and recovering, tiny chunks so sealing and
    // compression happen constantly — and every record must reconstruct to
    // the model exactly, in random order.
    void everyRecordReconstructsToTheModel() {
        for (uint32_t seed : {1u, 2u, 3u, 0xC0DEu}) {
            StoreConfig cfg;
            cfg.chunkRawBytes = 3000;
            PageStore store(cfg);
            Sim sim(seed);
            const auto snaps = runSim(store, sim, 1500);
            QVERIFY2(snaps.size() > 100, "the simulation barely changed anything");
            QVERIFY(store.stats().chunks > 3);

            QVector<RecordId> order = snaps.keys();
            std::shuffle(order.begin(), order.end(), std::mt19937(seed));
            for (RecordId r : order) {
                const FramePtr f = store.frameAt(r, sim.addrs);
                const QString mismatch = frameMismatch(*f, snaps.value(r), sim.addrs);
                QVERIFY2(mismatch.isEmpty(), qPrintable(QStringLiteral("seed %1: %2").arg(seed).arg(mismatch)));
            }
        }
    }

    // Stepping a frame (either direction, near or far) lands on exactly what
    // a fresh reconstruction gives.
    void advanceLandsWhereFrameAtDoes() {
        StoreConfig cfg;
        cfg.chunkRawBytes = 4000;
        PageStore store(cfg);
        Sim sim(77);
        const auto snaps = runSim(store, sim, 1200);
        QVector<RecordId> ids = snaps.keys();
        std::sort(ids.begin(), ids.end());
        std::mt19937 rng(5);
        FramePtr cursor = store.frameAt(ids.first(), sim.addrs);
        for (int step = 0; step < 400; ++step) {
            RecordId target;
            if (step % 3 == 0) {
                target = ids[int(rng() % ids.size())];                        // jump
            } else {
                const int at = int(std::lower_bound(ids.begin(), ids.end(), cursor->record) - ids.begin());
                const int next = qBound(0, at + (rng() % 2 ? 1 : -1) * int(1 + rng() % 3), int(ids.size()) - 1);
                target = ids[next];                                            // ±few
            }
            cursor = store.advance(cursor, target);
            QCOMPARE(cursor->record, target);
            const QString mismatch = frameMismatch(*cursor, snaps.value(target), sim.addrs);
            QVERIFY2(mismatch.isEmpty(), qPrintable(mismatch));
        }
    }

    // User decision: a tick where nothing changed stores nothing at all.
    void identicalTicksStoreNothing() {
        PageStore store;
        std::mt19937 rng(9);
        const QByteArray page = randomPage(rng);
        QCOMPARE(store.ingest(tickWith(100, {{0x5000, page}})), RecordId(0));
        const StoreStats before = store.stats();
        for (int i = 0; i < 1000; ++i) {
            // A fresh buffer with the same bytes, as a real read returns.
            QByteArray copy(page.constData(), page.size());
            QCOMPARE(store.ingest(tickWith(200 + i * 100, {{0x5000, copy}})), kNoRecord);
        }
        const StoreStats after = store.stats();
        QCOMPARE(after.records, 1);
        QCOMPARE(after.totalBytes(), before.totalBytes());
        QCOMPARE(after.blobs, before.blobs);
    }

    // A page changing on every tick still reconstructs from a bounded
    // number of deltas: anchors cap the replay.
    void replayIsBoundedByTheAnchorCap() {
        PageStore store;
        std::mt19937 rng(21);
        QByteArray page = randomPage(rng);
        ModelState last;
        for (int t = 0; t < 7000; ++t) {
            page[int(rng() % kPageSize)] = char(rng() & 0xFF);
            page[int(rng() % kPageSize)] = char(rng() | 1);
            store.ingest(tickWith(1000 + t * 50, {{0x9000, page}}));
        }
        const qint64 before = store.stats().replayedOps;
        const FramePtr f = store.frameAt(store.lastRecord(), {0x9000});
        const qint64 replayed = store.stats().replayedOps - before;
        QCOMPARE(f->pages.value(0x9000), page);
        QVERIFY2(replayed <= kAnchorMaxOps,
                 qPrintable(QStringLiteral("replayed %1 ops for one page").arg(replayed)));
        // And somewhere in the middle of the history, too.
        const qint64 before2 = store.stats().replayedOps;
        store.frameAt(store.firstRecord() + 3333, {0x9000});
        QVERIFY(store.stats().replayedOps - before2 <= kAnchorMaxOps);
    }

    // Identical images are stored once; a hash collision never shares
    // images whose bytes differ.
    void imagesAreSharedByContentNotByHash() {
        std::mt19937 rng(33);
        const QByteArray a = randomPage(rng), b = randomPage(rng);
        {
            PageStore store;
            store.ingest(tickWith(100, {{0x1000, a}, {0x2000, a}, {0x3000, a}}));
            QCOMPARE(store.stats().blobs, 1);
        }
        StoreConfig cfg;
        cfg.hash = [](const char*, size_t) { return Hash128{42, 42}; };   // everything collides
        PageStore store(cfg);
        const RecordId r = store.ingest(tickWith(100, {{0x1000, a}, {0x2000, b}, {0x3000, a}}));
        QCOMPARE(store.stats().blobs, 2);
        const FramePtr f = store.frameAt(r, {0x1000, 0x2000, 0x3000});
        QCOMPARE(f->pages.value(0x1000), a);
        QCOMPARE(f->pages.value(0x2000), b);
        QCOMPARE(f->pages.value(0x3000), a);
        // The zero page is never stored at all.
        PageStore z;
        z.ingest(tickWith(100, {{0x1000, QByteArray(int(kPageSize), '\0')}}));
        QCOMPARE(z.stats().blobs, 0);
        QCOMPARE(z.frameAt(0, {0x1000})->pages.value(0x1000), QByteArray(int(kPageSize), '\0'));
    }

    // Coverage decides what a frame may claim: a page nobody was watching
    // is NotCovered — never the last bytes it happened to have.
    void coverageEntersAndLeaves() {
        PageStore store;
        std::mt19937 rng(44);
        const QByteArray p = randomPage(rng);

        TickInput declare;
        declare.timeMs = declare.readStartMs = 100;
        declare.coverage = QVector<uint64_t>{0x1000, 0x2000};
        const RecordId r0 = store.ingest(declare);
        QVERIFY(r0 != kNoRecord);

        const RecordId r1 = store.ingest(tickWith(200, {{0x1000, p}}));
        TickInput shrink;
        shrink.timeMs = shrink.readStartMs = 300;
        shrink.coverage = QVector<uint64_t>{0x1000};
        const RecordId r2 = store.ingest(shrink);
        QVERIFY(r1 != kNoRecord && r2 != kNoRecord);
        QVERIFY(store.flagsOf(r2) & RF_CoverageOnly);

        QCOMPARE(store.pageAt(0x2000, r0), PageState::NotYetSampled);
        QCOMPARE(store.pageAt(0x1000, r1), PageState::Valid);
        QCOMPARE(store.pageAt(0x2000, r1), PageState::NotYetSampled);
        QCOMPARE(store.pageAt(0x2000, r2), PageState::NotCovered);
        QCOMPARE(store.pageAt(0x7000, r2), PageState::NotCovered);   // never seen

        const RecordId r3 = store.removeProducer(0, 400);
        QVERIFY(r3 != kNoRecord);
        QCOMPARE(store.pageAt(0x1000, r3), PageState::NotCovered);
        QByteArray bytes;
        QCOMPARE(store.pageAt(0x1000, r2, &bytes), PageState::Valid);
        QCOMPARE(bytes, p);
    }

    void refusedReadsAreAStateNotZeros() {
        PageStore store;
        std::mt19937 rng(55);
        const QByteArray a = randomPage(rng), b = randomPage(rng);
        const RecordId r0 = store.ingest(tickWith(100, {{0x4000, a}}));

        TickInput refuse;
        refuse.timeMs = refuse.readStartMs = 200;
        refuse.unreadable = {0x4000};
        const RecordId r1 = store.ingest(refuse);
        QVERIFY(r1 != kNoRecord);
        QVERIFY(store.flagsOf(r1) & RF_HasUnreadable);
        refuse.timeMs = refuse.readStartMs = 300;
        QCOMPARE(store.ingest(refuse), kNoRecord);      // still refused: no change

        const RecordId r2 = store.ingest(tickWith(400, {{0x4000, b}}));
        QByteArray bytes;
        QCOMPARE(store.pageAt(0x4000, r0, &bytes), PageState::Valid);
        QCOMPARE(bytes, a);
        QCOMPARE(store.pageAt(0x4000, r1), PageState::Unreadable);
        QCOMPARE(store.pageAt(0x4000, r2, &bytes), PageState::Valid);
        QCOMPARE(bytes, b);
    }

    // Two read loops sampling the same page: an older read that lands late
    // must not revert the fresher one.
    void aStaleSampleCannotRevertAFresherOne() {
        PageStore store;
        std::mt19937 rng(66);
        const QByteArray fresh = randomPage(rng), stale = randomPage(rng);
        TickInput a = tickWith(500, {{0x8000, fresh}});
        a.producer = 1;
        a.readStartMs = 480;
        const RecordId r0 = store.ingest(a);
        TickInput b = tickWith(510, {{0x8000, stale}});
        b.producer = 2;
        b.readStartMs = 450;           // began before producer 1's read
        QCOMPARE(store.ingest(b), kNoRecord);
        QByteArray bytes;
        store.pageAt(0x8000, r0, &bytes);
        QCOMPARE(bytes, fresh);
    }

    // What changed at a record, and when a byte range changed — the two
    // questions behind change highlighting in the past and "previous change
    // of this field".
    void changeQueriesAreExact() {
        PageStore store;
        QByteArray p1(int(kPageSize), '\x11'), p2(int(kPageSize), '\x22');
        const RecordId r0 = store.ingest(tickWith(100, {{0x10000, p1}, {0x11000, p2}}));

        p1[10] = 'a'; p1[11] = 'b'; p1[12] = 'c'; p1[13] = 'd';
        const RecordId r1 = store.ingest(tickWith(200, {{0x10000, p1}}));

        p1[4000] = 'x';
        p1[4095] = 'y';
        p2[0] = 'z';
        const RecordId r2 = store.ingest(tickWith(300, {{0x10000, p1}, {0x11000, p2}}));

        const QVector<ChangedSpan> at1 = store.changedSpansAt(r1);
        QCOMPARE(at1.size(), 1);
        QCOMPARE(at1[0].addr, uint64_t(0x1000A));
        QCOMPARE(at1[0].len, uint32_t(4));
        QCOMPARE(store.changedBytesOf(r1), uint32_t(4));

        const QVector<ChangedSpan> at2 = store.changedSpansAt(r2);
        QCOMPARE(at2.size(), 2);
        QCOMPARE(at2[0].addr, uint64_t(0x10FA0));
        QCOMPARE(at2[0].len, uint32_t(1));
        QCOMPARE(at2[1].addr, uint64_t(0x10FFF));   // joined across the page boundary
        QCOMPARE(at2[1].len, uint32_t(2));
        QVERIFY(store.changedSpansAt(r0).isEmpty()); // first sight is not a change

        const RecordId all = store.lastRecord();
        QCOMPARE(store.changeRecordsForRange(0x1000C, 1, 0, all), QVector<RecordId>{r1});
        QCOMPARE(store.changeRecordsForRange(0x10FFE, 3, 0, all), QVector<RecordId>{r2});
        QCOMPARE(store.changeRecordsForRange(0x10000, kPageSize, 0, all), (QVector<RecordId>{r1, r2}));
        QVERIFY(store.changeRecordsForRange(0x10100, 16, 0, all).isEmpty());
        QVERIFY(store.changeRecordsForRange(0x1000A, 4, r2, all).isEmpty());
    }

    // Retention drops old chunks, and everything still retained reconstructs
    // exactly — the re-anchoring is the hard part.
    void trimmingKeepsTheRestExact() {
        StoreConfig cfg;
        cfg.chunkRawBytes = 3000;
        PageStore store(cfg);
        Sim sim(88);
        const auto snaps = runSim(store, sim, 1500);
        const RecordId mid = store.firstRecord() + (store.lastRecord() - store.firstRecord()) / 2;
        const RecordId first = store.trimBefore(mid);
        QVERIFY(first > 0);
        QVERIFY(first <= mid);
        QCOMPARE(store.firstRecord(), first);
        for (auto it = snaps.constBegin(); it != snaps.constEnd(); ++it) {
            const FramePtr f = store.frameAt(it.key(), sim.addrs);
            if (it.key() < first) {
                for (uint64_t p : sim.addrs) QCOMPARE(f->stateOf(p), PageState::NotCovered);
                continue;
            }
            const QString mismatch = frameMismatch(*f, it.value(), sim.addrs);
            QVERIFY2(mismatch.isEmpty(), qPrintable(mismatch));
        }
        // Keep going after the trim: new records must still be exact.
        const auto more = runSim(store, sim, 300);
        for (auto it = more.constBegin(); it != more.constEnd(); ++it) {
            const QString mismatch = frameMismatch(*store.frameAt(it.key(), sim.addrs), it.value(), sim.addrs);
            QVERIFY2(mismatch.isEmpty(), qPrintable(mismatch));
        }
    }

    void retentionHonoursBudgetWindowAndProtection() {
        StoreConfig cfg;
        cfg.chunkRawBytes = 2000;
        PageStore store(cfg);
        Sim sim(99);
        const auto snaps = runSim(store, sim, 2000);
        const qint64 full = store.stats().totalBytes();
        const int fullRecords = store.stats().records;

        // Age: keep roughly the last 20 s of a 200 s capture, but never drop
        // what is protected.
        const RecordId protect = store.firstRecord() + RecordId(fullRecords / 4);
        store.enforceRetention(0, sim.now, 20'000, protect);
        QVERIFY(store.firstRecord() <= protect);

        store.enforceRetention(0, sim.now, 20'000);
        QVERIFY(store.stats().records < fullRecords);
        QVERIFY(store.timeOf(store.firstRecord()) >= sim.now - 20'000 - 5'000);

        // Bytes.
        store.enforceRetention(full / 4, sim.now, 0);
        QVERIFY(store.stats().totalBytes() <= full / 4 || store.stats().chunks <= 2);

        for (auto it = snaps.constBegin(); it != snaps.constEnd(); ++it) {
            if (!store.contains(it.key())) continue;
            const QString mismatch = frameMismatch(*store.frameAt(it.key(), sim.addrs), it.value(), sim.addrs);
            QVERIFY2(mismatch.isEmpty(), qPrintable(mismatch));
        }
    }

    // "Captured, nothing changed" and "not watched" must never look alike.
    void gapsAreExplicitIntervals() {
        PageStore store;
        std::mt19937 rng(111);
        QByteArray p = randomPage(rng);
        store.ingest(tickWith(1000, {{0x1000, p}}));
        store.beginGap(1500, GapReason::Paused);
        store.beginGap(1600, GapReason::Suspended);     // already open: ignored
        QVERIFY(store.gapAt(1550).has_value());
        QVERIFY(store.gapAt(99999).has_value());         // still open
        store.endGap(2500);
        QVERIFY(!store.gapAt(1499).has_value());
        QCOMPARE(store.gapAt(2000)->reason, GapReason::Paused);
        QVERIFY(!store.gapAt(2500).has_value());

        p[0] = char(p[0] ^ 1);
        const RecordId r = store.ingest(tickWith(2600, {{0x1000, p}}));
        QVERIFY(store.flagsOf(r) & RF_AfterGap);
        p[1] = char(p[1] ^ 1);
        const RecordId r2 = store.ingest(tickWith(2700, {{0x1000, p}}));
        QVERIFY(!(store.flagsOf(r2) & RF_AfterGap));
    }

    void recordLookupByTime() {
        PageStore store;
        QByteArray p(int(kPageSize), '\x01');
        for (int i = 0; i < 10; ++i) {
            p[0] = char(i + 2);
            store.ingest(tickWith(1000 + i * 100, {{0x1000, p}}));
        }
        QCOMPARE(store.recordAtOrBefore(999), kNoRecord);
        QCOMPARE(store.recordAtOrBefore(1000), RecordId(0));
        QCOMPARE(store.recordAtOrBefore(1450), RecordId(4));
        QCOMPARE(store.recordAtOrBefore(1e9), RecordId(9));
        QCOMPARE(store.timeOf(4), int64_t(1400));
        QCOMPARE(store.changedBytesPyramid().size(), 10);
    }

    // A corrupt block costs the pages that needed it — never a crash, never
    // silently wrong bytes.
    void corruptChunkBecomesLostData() {
        StoreConfig cfg;
        cfg.chunkRawBytes = 600;
        cfg.compressSealed = false;
        PageStore store(cfg);
        std::mt19937 rng(123);
        QByteArray p = randomPage(rng);
        QHash<RecordId, QByteArray> truth;
        for (int t = 0; t < 400; ++t) {
            for (int i = 0; i < 8; ++i) p[int(rng() % kPageSize)] = char(rng() & 0xFF);
            const RecordId r = store.ingest(tickWith(100 + t * 10, {{0x3000, p}}));
            if (r != kNoRecord) truth.insert(r, p);
        }
        QVERIFY(store.stats().chunks > 5);
        QVERIFY(store.corruptSealedChunkForTest(1));
        bool sawLost = false;
        for (auto it = truth.constBegin(); it != truth.constEnd(); ++it) {
            QByteArray bytes;
            const PageState st = store.pageAt(0x3000, it.key(), &bytes);
            if (st == PageState::Lost) { sawLost = true; continue; }
            QCOMPARE(st, PageState::Valid);
            QCOMPARE(bytes, it.value());
        }
        QVERIFY(sawLost);
        QCOMPARE(store.stats().lostChunks, 1);
    }

    // ── The facade the UI thread uses ──

    // Commits reach the UI-side model, which mirrors the store's index; idle
    // ticks send nothing home at all.
    void contextCommitsMirrorTheStore() {
        auto exec = std::make_shared<InlineExecutor>();
        CaptureContext ctx(exec);
        int commits = 0;
        ctx.addListener([&](const CommitBatch&) { ++commits; });
        std::mt19937 rng(7);
        QByteArray p = randomPage(rng);
        QCOMPARE(ctx.append(tickWith(1000, {{0x1000, p}})), AppendStatus::Accepted);
        QCOMPARE(commits, 1);
        for (int i = 0; i < 50; ++i) ctx.append(tickWith(1100 + i * 100, {{0x1000, p}}));
        QCOMPARE(commits, 1);                          // idle: nothing crossed
        for (int i = 0; i < 10; ++i) {
            p[i] = char(p[i] ^ 0xFF);
            ctx.append(tickWith(7000 + i * 100, {{0x1000, p}}));
        }
        const TimelineModel& m = ctx.model();
        QCOMPARE(m.size(), 11);
        QCOMPARE(m.firstRecord(), RecordId(0));
        QCOMPARE(m.lastRecord(), RecordId(10));
        QCOMPARE(m.timeOf(3), int64_t(7200));
        QCOMPARE(m.recordAtOrBefore(7250), RecordId(3));
        QCOMPARE(m.changedBytesOf(4), uint32_t(1));
        const auto agg = m.aggregate(7000, 7500);   // records 1..5
        QCOMPARE(agg.count, 5);
        QCOMPARE(agg.sum, uint64_t(5));

        ctx.beginGap(9000, GapReason::Paused);
        QVERIFY(m.gapAt(9500).has_value());
        ctx.endGap(9800);
        QVERIFY(!m.gapAt(9900).has_value());
    }

    // A scrub fires far more requests than can be served; only the newest per
    // requester is ever computed, and each requester is independent.
    void framesAreLatestWinsPerRequester() {
        auto exec = std::make_shared<ManualExecutor>();
        CaptureContext ctx(exec);
        QByteArray p(int(kPageSize), '\0');
        for (int i = 0; i < 20; ++i) {
            p[0] = char(i + 1);
            ctx.append(tickWith(1000 + i * 100, {{0x2000, p}}));
        }
        exec->runAll();
        QObject receiver;
        QVector<RecordId> delivered;
        for (RecordId r = 0; r < 15; ++r)
            ctx.requestFrame(1, r, {0x2000}, &receiver,
                             [&](FramePtr f) { delivered.append(f->record); });
        RecordId other = kNoRecord;
        ctx.requestFrame(2, 3, {0x2000}, &receiver, [&](FramePtr f) { other = f->record; });
        exec->runAll();
        QCOMPARE(delivered, QVector<RecordId>{14});
        QCOMPARE(other, RecordId(3));

        // The frame really is record 14's bytes, and a step from it lands right.
        FramePtr last;
        ctx.requestFrame(1, 9, {0x2000}, &receiver, [&](FramePtr f) { last = f; });
        exec->runAll();
        QVERIFY(last);
        QCOMPARE(uint8_t(last->pages.value(0x2000)[0]), uint8_t(10));
    }

    // Results addressed to a dead object are dropped, and a destroyed context
    // leaves its queued jobs safe to run.
    void deadAddresseesAndDeadContextsAreSafe() {
        auto exec = std::make_shared<ManualExecutor>();
        bool called = false;
        {
            CaptureContext ctx(exec);
            ctx.append(tickWith(100, {{0x1000, QByteArray(int(kPageSize), '\x07')}}));
            auto* receiver = new QObject;
            ctx.requestFrame(1, 0, {0x1000}, receiver, [&](FramePtr) { called = true; });
            ctx.queryChangeRecords(0x1000, 4, 0, 10, receiver, [&](QVector<RecordId>) { called = true; });
            delete receiver;
            exec->runAll();
            QVERIFY(!called);

            ctx.append(tickWith(200, {{0x1000, QByteArray(int(kPageSize), '\x08')}}));
            QObject alive;
            ctx.requestFrame(1, 1, {0x1000}, &alive, [&](FramePtr) { called = true; });
        }   // context destroyed with jobs still queued
        exec->runAll();
        QVERIFY(!called);
    }

    // Backpressure never silently loses ticks: it opens a Dropped gap and
    // closes it when capture catches up.
    void backpressureBecomesAGap() {
        auto exec = std::make_shared<ManualExecutor>();
        CaptureContext ctx(exec);
        ctx.setBackpressureLimits(2, 1LL << 30);
        QByteArray p(int(kPageSize), '\x01');
        auto tick = [&](int64_t t) {
            p[0] = char(p[0] + 1);
            return ctx.append(tickWith(t, {{0x1000, p}}));
        };
        QCOMPARE(tick(100), AppendStatus::Accepted);
        QCOMPARE(tick(200), AppendStatus::Accepted);
        QCOMPARE(tick(300), AppendStatus::Dropped);
        QCOMPARE(tick(400), AppendStatus::Dropped);
        exec->runAll();
        QCOMPARE(tick(500), AppendStatus::Accepted);
        exec->runAll();
        const auto gap = ctx.model().gapAt(350);
        QVERIFY(gap.has_value());
        QCOMPARE(gap->reason, GapReason::Dropped);
        QVERIFY(!ctx.model().gapAt(500).has_value());
    }

    // The real strand: jobs run one at a time, in order, off the UI thread,
    // and results arrive back on the UI thread.
    void poolStrandIsSerialAndDeliversHome() {
        QThreadPool pool;
        pool.setMaxThreadCount(4);
        auto strand = std::make_shared<PoolStrand>(&pool);
        QVector<int> order;
        std::atomic<int> concurrent{0}, maxConcurrent{0};
        std::atomic<bool> offUiThread{true};
        const QThread* ui = QThread::currentThread();
        for (int i = 0; i < 2000; ++i) {
            strand->post([&, i]() {
                const int c = ++concurrent;
                int m = maxConcurrent.load();
                while (c > m && !maxConcurrent.compare_exchange_weak(m, c)) {}
                if (QThread::currentThread() == ui) offUiThread = false;
                order.append(i);
                --concurrent;
            }, (i % 3 == 0) ? Lane::Maintenance : Lane::Ingest);
        }
        QObject home;
        bool deliveredOnUi = false;
        strand->post([&]() {
            strand->deliver(QPointer<QObject>(&home),
                            [&]() { deliveredOnUi = (QThread::currentThread() == ui); });
        }, Lane::Maintenance);
        QVERIFY(strand->waitIdle(10'000));
        QTRY_VERIFY(deliveredOnUi);
        QCOMPARE(maxConcurrent.load(), 1);
        QVERIFY(offUiThread.load());
        QCOMPARE(order.size(), 2000);
        // Within a lane, order is preserved.
        QVector<int> ingest, maintenance;
        for (int v : order) (v % 3 == 0 ? maintenance : ingest).append(v);
        QVERIFY(std::is_sorted(ingest.begin(), ingest.end()));
        QVERIFY(std::is_sorted(maintenance.begin(), maintenance.end()));
    }

    // One history per source: every tab on the same process shares it.
    void oneContextPerSource() {
        auto& svc = TimelineService::instance();
        svc.setExecutorFactoryForTest([] { return std::make_shared<InlineExecutor>(); });
        auto sourceA = std::make_shared<int>(1);
        auto sourceB = std::make_shared<int>(2);
        auto a1 = svc.contextFor(sourceA);
        auto a2 = svc.contextFor(sourceA);
        auto b1 = svc.contextFor(sourceB);
        QVERIFY(a1 && b1);
        QCOMPARE(a1.get(), a2.get());
        QVERIFY(a1.get() != b1.get());
        const int live = svc.liveContexts();
        QVERIFY(live >= 2);
        a1.reset(); a2.reset();
        QCOMPARE(svc.liveContexts(), live - 1);   // nobody holds A's history now
        auto again = svc.contextFor(sourceA);
        QVERIFY(again && again.get() != b1.get());
        svc.setExecutorFactoryForTest({});
    }

    // ── Past frames never touch live memory ──

    void frameProviderNeverFillsInMissingPages() {
        struct CountingReal : rcx::Provider {
            mutable int reads = 0;
            bool read(uint64_t, void* buf, int len) const override {
                ++reads;
                std::memset(buf, 0xEE, size_t(len));
                return true;
            }
            int size() const override { return 1 << 30; }
            bool isLive() const override { return true; }
            bool isWritable() const override { return true; }
            QString name() const override { return QStringLiteral("game.exe"); }
            int pointerSize() const override { return 8; }
        };
        auto real = std::make_shared<CountingReal>();
        auto frame = std::make_shared<Frame>();
        QByteArray page(int(kPageSize), '\0');
        for (int i = 0; i < page.size(); ++i) page[i] = char(i & 0x7F);
        frame->pages.insert(0x1000, page);
        frame->states.insert(0x1000, PageState::Valid);
        frame->states.insert(0x2000, PageState::NotCovered);
        TimelineFrameProvider fp(real, frame, 0x2000);

        char buf[8];
        std::memset(buf, 0x55, sizeof(buf));
        QVERIFY(!fp.read(0x1FFC, buf, 8));                 // straddles into an uncaptured page
        QCOMPARE(QByteArray(buf, 4), page.mid(4092, 4));  // captured half is real
        QCOMPARE(QByteArray(buf + 4, 4), QByteArray(4, '\0'));   // missing half is zeros, not live bytes
        QVERIFY(fp.read(0x1010, buf, 8));
        QCOMPARE(QByteArray(buf, 8), page.mid(0x10, 8));
        QVERIFY(fp.isReadable(0x1000, 4096));
        QVERIFY(!fp.notCaptured(0x1000, 4096));
        // Uncaptured is not a failed read: compose may lay it out (a pointer's
        // target keeps its address), and it is reported as not captured so
        // the view shows "??" instead of the zeros read() hands back.
        QVERIFY(fp.isReadable(0x1FFE, 4));
        QVERIFY(fp.notCaptured(0x1FFE, 4));
        QVERIFY(fp.notCaptured(0x9000, 1));
        frame->states.insert(0x3000, PageState::Unreadable);
        QVERIFY(!fp.isReadable(0x2FFF, 2));                // a read that failed THEN
        QVERIFY(!fp.notCaptured(0x3000, 4));
        QCOMPARE(fp.stateOf(0x2004), PageState::NotCovered);
        QCOMPARE(real->reads, 0);                          // THE point

        QVERIFY(!fp.isWritable());
        QVERIFY(!fp.write(0x1000, buf, 4));
        QVERIFY(fp.isLive());
        QCOMPARE(fp.name(), QStringLiteral("game.exe"));
        QVERIFY(fp.size() >= 1);
    }

    void coveredPagesFollowCoverageOverTime() {
        PageStore store;
        TickInput in;
        in.timeMs = in.readStartMs = 100;
        in.coverage = QVector<uint64_t>{0x1000, 0x2000, 0x3000};
        const RecordId r0 = store.ingest(in);
        TickInput less;
        less.timeMs = less.readStartMs = 200;
        less.coverage = QVector<uint64_t>{0x1000, 0x3000};
        const RecordId r1 = store.ingest(less);
        QCOMPARE(store.coveredPagesAt(r0), (QVector<uint64_t>{0x1000, 0x2000, 0x3000}));
        QCOMPARE(store.coveredPagesAt(r1), (QVector<uint64_t>{0x1000, 0x3000}));
    }

    // ── Class capture state ──

    void hubModesPinsAndRetention() {
        auto exec = std::make_shared<InlineExecutor>();
        auto ctx = std::make_shared<CaptureContext>(exec);
        QByteArray p(int(kPageSize), '\x01');
        for (int i = 0; i < 30; ++i) {
            p[0] = char(i + 2);
            ctx->append(tickWith(1000 + i * 100, {{0x1000, p}}));
        }
        TimelineHub hub;
        int changes = 0;
        hub.onChanged = [&]() { ++changes; };
        TimelineHub::Budgets budgets;
        budgets.rollingBytes = 1 << 20;
        budgets.recordingBytes = 8 << 20;
        budgets.rollingWindowMs = 60'000;
        hub.setBudgets(budgets);
        hub.bindSource(ctx, QStringLiteral("game.exe"), 900);
        QVERIFY(changes > 0);
        QCOMPARE(hub.events().first().kind, TimelineEventKind::SourceAttached);

        const uint64_t cls = 42, other = 7;
        QCOMPARE(hub.mode(cls), CaptureMode::Rolling);
        QCOMPARE(hub.retention().budgetBytes, qint64(1 << 20));
        QCOMPARE(hub.retention().protectFrom, kNoRecord);

        // Start keeps what is already retained (replay buffer).
        hub.startRecording(cls, 5000);
        QCOMPARE(hub.mode(cls), CaptureMode::Recording);
        QCOMPARE(hub.track(cls).pin, ctx->model().firstRecord());
        QCOMPARE(hub.retention().budgetBytes, qint64(8 << 20));
        QCOMPARE(hub.retention().protectFrom, ctx->model().firstRecord());
        QCOMPARE(hub.mode(other), CaptureMode::Rolling);   // per class

        // Pause while recording, then resume back into recording.
        hub.pause(cls, 6000);
        QCOMPARE(hub.mode(cls), CaptureMode::Paused);
        QVERIFY(hub.isHiddenFor(cls, 6500));
        QVERIFY(hub.isHiddenFor(other, 6500));   // never recorded: no history at all
        QCOMPARE(hub.firstVisibleRecord(other), kNoRecord);
        QVERIFY(hub.isHiddenFor(cls, 4999));     // before its own first Record
        QVERIFY(!hub.isHiddenFor(cls, 5500));
        QCOMPARE(hub.retention().budgetBytes, qint64(8 << 20));   // still a recording
        hub.resume(cls, 7000);
        QCOMPARE(hub.mode(cls), CaptureMode::Recording);
        QVERIFY(hub.isHiddenFor(cls, 6999));
        QVERIFY(!hub.isHiddenFor(cls, 7000));

        // Stop keeps what was recorded until Clear, and from Stop on nothing
        // is recorded for the class: "not recorded", not "nothing changed".
        const RecordId pin = hub.track(cls).pin;
        hub.toggleRecording(cls, 8000);
        QCOMPARE(hub.mode(cls), CaptureMode::Rolling);
        QCOMPARE(hub.track(cls).pin, pin);
        QCOMPARE(hub.retention().budgetBytes, qint64(8 << 20));
        QCOMPARE(hub.retention().protectFrom, pin);
        QVERIFY(hub.isHiddenFor(cls, 8500));
        QVERIFY(hub.isHiddenFor(other, 8500));
        // Record again: the gap closes, and the first recording is still kept.
        hub.startRecording(cls, 8600);
        QVERIFY(hub.isHiddenFor(cls, 8599));
        QVERIFY(!hub.isHiddenFor(cls, 8600));
        QCOMPARE(hub.track(cls).pin, pin);
        hub.stopRecording(cls, 8700);

        // Pausing another class keeps what is retained from ageing out too.
        hub.togglePause(other, 9000);
        QCOMPARE(hub.mode(other), CaptureMode::Paused);
        QVERIFY(hub.retention().protectFrom != kNoRecord);
        hub.togglePause(other, 9500);
        QCOMPARE(hub.mode(other), CaptureMode::Rolling);
        QCOMPARE(hub.retention().protectFrom, pin);   // the stopped recording's

        // Reset hides this class's past, and only this class's.
        hub.reset(cls, 3000);
        QVERIFY(hub.isHiddenFor(cls, 2999));
        QVERIFY(hub.isHiddenFor(other, 2999));                  // never recorded
        // Cleared while stopped: nothing before the clear, and nothing after
        // it either — the class is not recording.
        QCOMPARE(hub.firstVisibleRecord(cls), kNoRecord);
        for (const TimelineEvent& e : hub.events())
            QVERIFY(!(e.classId == cls && e.timeMs < 3000));
    }

    void hubBaseEpochsAndSegments() {
        TimelineHub hub;
        hub.noteBase(0x1000, 100);
        hub.noteBase(0x1000, 150);   // same base: not a new epoch
        hub.noteBase(0x2000, 500);
        QCOMPARE(hub.baseAt(50, 0xDEAD), uint64_t(0x1000));
        QCOMPARE(hub.baseAt(300, 0xDEAD), uint64_t(0x1000));
        QCOMPARE(hub.baseAt(500, 0xDEAD), uint64_t(0x2000));
        QCOMPARE(hub.baseAt(9999, 0xDEAD), uint64_t(0x2000));
        TimelineHub empty;
        QCOMPARE(empty.baseAt(10, 0xBEEF), uint64_t(0xBEEF));

        auto exec = std::make_shared<InlineExecutor>();
        auto a = std::make_shared<CaptureContext>(exec);
        auto b = std::make_shared<CaptureContext>(exec);
        hub.bindSource(a, QStringLiteral("a.exe"), 10);
        hub.bindSource(b, QStringLiteral("b.exe"), 20);
        QCOMPARE(hub.segments().size(), 2);
        QCOMPARE(hub.context().get(), b.get());
        hub.unbindSource(QStringLiteral("b.exe"), 30);
        QVERIFY(!hub.context());
        QCOMPARE(hub.events().last().kind, TimelineEventKind::SourceDetached);
    }

    // ── Recordings on disk ──

    // Everything a recording spilled reads back exactly — chunks and page
    // images, across segment files — and trimming deletes what nothing uses.
    void spilledHistoryReadsBackExactly() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        auto dir = SpillDir::create(tmp.path());
        QVERIFY(dir);
        QVERIFY(QFileInfo::exists(QDir(dir->path()).filePath(QStringLiteral("LOCK"))));
        StoreConfig cfg;
        cfg.chunkRawBytes = 2048;
        PageStore store(cfg);
        auto chunks = std::make_shared<SpillFile>(dir, 1, 'c', 64 * 1024);   // small segments: rotation
        auto blobs = std::make_shared<SpillFile>(dir, 1, 'b', 64 * 1024);
        store.setSpill(chunks, blobs, /*blobRamBytes=*/0);
        store.setSpillActive(true);
        Sim sim(99);
        const auto snaps = runSim(store, sim, 1500);
        const StoreStats st = store.stats();
        QVERIFY(st.spilledChunks > 3);
        QVERIFY(st.spilledBlobs > 0);
        QVERIFY(st.diskBytes > 0);
        QVERIFY(!st.diskDegraded);
        QVERIFY(chunks->segmentCount() > 1);
        int checked = 0;
        for (auto it = snaps.constBegin(); it != snaps.constEnd(); ++it) {
            if (it.key() % 5) continue;
            const FramePtr f = store.frameAt(it.key(), sim.addrs);
            const QString why = frameMismatch(*f, it.value(), sim.addrs);
            QVERIFY2(why.isEmpty(), qPrintable(why));
            ++checked;
        }
        QVERIFY(checked > 100);

        const int segments = chunks->segmentCount();
        const RecordId last = store.lastRecord();
        store.trimBefore(last);
        QVERIFY(chunks->segmentCount() < segments);
        QVERIFY(store.stats().diskBytes < st.diskBytes);
        for (auto it = snaps.constBegin(); it != snaps.constEnd(); ++it) {
            if (!store.contains(it.key())) continue;
            const QString why = frameMismatch(*store.frameAt(it.key(), sim.addrs), it.value(), sim.addrs);
            QVERIFY2(why.isEmpty(), qPrintable(why));
        }
    }

    // A flipped byte in a spill file costs the pages that needed that chunk.
    void aCorruptSpillFileBecomesLostData() {
        QTemporaryDir tmp;
        auto dir = SpillDir::create(tmp.path());
        QVERIFY(dir);
        StoreConfig cfg;
        cfg.chunkRawBytes = 600;
        cfg.compressSealed = false;
        PageStore store(cfg);
        store.setSpill(std::make_shared<SpillFile>(dir, 2, 'c'), std::make_shared<SpillFile>(dir, 2, 'b'),
                       1LL << 30);
        store.setSpillActive(true);
        std::mt19937 rng(321);
        QByteArray p = randomPage(rng);
        QHash<RecordId, QByteArray> truth;
        for (int t = 0; t < 400; ++t) {
            for (int i = 0; i < 8; ++i) p[int(rng() % kPageSize)] = char(rng() & 0xFF);
            const RecordId r = store.ingest(tickWith(100 + t * 10, {{0x3000, p}}));
            if (r != kNoRecord) truth.insert(r, p);
        }
        QVERIFY(store.stats().spilledChunks > 5);
        QVERIFY(store.corruptSpilledChunkForTest(1));
        bool sawLost = false;
        for (auto it = truth.constBegin(); it != truth.constEnd(); ++it) {
            QByteArray bytes;
            const PageState st = store.pageAt(0x3000, it.key(), &bytes);
            if (st == PageState::Lost) { sawLost = true; continue; }
            QCOMPARE(st, PageState::Valid);
            QCOMPARE(bytes, it.value());
        }
        QVERIFY(sawLost);
        QCOMPARE(store.stats().lostChunks, 1);
    }

    // A disk that refuses a write degrades the recording to RAM, losing nothing.
    void aFailedSpillWriteKeepsHistoryInRam() {
        QTemporaryDir tmp;
        auto dir = SpillDir::create(tmp.path());
        QVERIFY(dir);
        StoreConfig cfg;
        cfg.chunkRawBytes = 2048;
        PageStore store(cfg);
        auto chunks = std::make_shared<SpillFile>(dir, 3, 'c');
        chunks->failWritesForTest(true);
        store.setSpill(chunks, std::make_shared<SpillFile>(dir, 3, 'b'), 1LL << 30);
        store.setSpillActive(true);
        Sim sim(7);
        const auto snaps = runSim(store, sim, 400);
        const StoreStats st = store.stats();
        QVERIFY(st.diskDegraded);
        QCOMPARE(st.spilledChunks, 0);
        QVERIFY(st.chunks > 3);
        for (auto it = snaps.constBegin(); it != snaps.constEnd(); ++it) {
            const QString why = frameMismatch(*store.frameAt(it.key(), sim.addrs), it.value(), sim.addrs);
            QVERIFY2(why.isEmpty(), qPrintable(why));
        }
    }

    // The session directory lives exactly as long as the files in it; a later
    // launch sweeps what a dead session left, never a live one.
    void sessionDirectoriesGoWithTheirProcess() {
        QTemporaryDir tmp;
        QString live;
        {
            auto dir = SpillDir::create(tmp.path());
            QVERIFY(dir);
            live = dir->path();
            auto file = std::make_shared<SpillFile>(dir, 4, 'c');
            const SpillLocation loc = file->write(QByteArray(1000, 'x'));
            QVERIFY(loc.isValid());
            QByteArray back;
            QVERIFY(file->read(loc, back));
            QCOMPARE(back, QByteArray(1000, 'x'));
            dir.reset();                                  // the file still needs it
            QVERIFY(QDir(live).exists());
            QCOMPARE(SpillDir::removeStaleSessions(tmp.path(), 0), 0);   // locked: alive
            QVERIFY(QDir(live).exists());
        }
        QVERIFY(!QDir(live).exists());

        QDir root(tmp.path());
        QVERIFY(root.mkpath(QStringLiteral("s999999-abc")));     // no process has that pid on Windows
        {
            QFile lock(root.filePath(QStringLiteral("s999999-abc/LOCK")));
            QVERIFY(lock.open(QIODevice::WriteOnly));
            lock.write("999999\nRC\n\n");
        }
        QVERIFY(root.mkpath(QStringLiteral("s999998-def")));     // no LOCK at all
        QVERIFY(root.mkpath(QStringLiteral("notasession")));
        QCOMPARE(SpillDir::removeStaleSessions(tmp.path(), 0), 2);
        QVERIFY(!root.exists(QStringLiteral("s999999-abc")));
        QVERIFY(!root.exists(QStringLiteral("s999998-def")));
        QVERIFY(root.exists(QStringLiteral("notasession")));
    }

    // Rolling stays in RAM; starting a recording opens the spill, and a disk
    // budget drops the oldest history like any other budget.
    void aRecordingSpillsAndRollingDoesNot() {
        QTemporaryDir tmp;
        auto dir = SpillDir::create(tmp.path());
        QVERIFY(dir);
        auto exec = std::make_shared<InlineExecutor>();
        StoreConfig cfg;
        cfg.chunkRawBytes = 1024;
        CaptureContext ctx(exec, cfg);
        int opened = 0;
        ctx.setSpillFactory([&](char kind) { ++opened; return std::make_shared<SpillFile>(dir, 5, kind); });
        std::mt19937 rng(5);
        QByteArray p = randomPage(rng);
        auto run = [&](int64_t t0, int n) {
            for (int i = 0; i < n; ++i) {
                for (int k = 0; k < 16; ++k) p[int(rng() % kPageSize)] = char(rng() & 0xFF);
                ctx.append(tickWith(t0 + i * 100, {{0x1000, p}}));
            }
        };
        run(1000, 200);
        QCOMPARE(opened, 0);
        QCOMPARE(ctx.model().stats().diskBytes, qint64(0));

        TimelineHub hub;
        hub.startRecording(42, 30'000);
        RetentionPolicy rec = hub.retention();
        QVERIFY(rec.spill);
        QVERIFY(rec.diskBudgetBytes > 0);
        rec.windowMs = 0;
        ctx.setRetention(rec);
        QCOMPARE(opened, 2);
        run(100'000, 300);
        QVERIFY(ctx.model().stats().spilledChunks > 0);
        QVERIFY(ctx.model().stats().diskBytes > 0);

        const RecordId first = ctx.model().firstRecord();
        rec.diskBudgetBytes = 4096;
        ctx.setRetention(rec);
        run(200'000, 50);
        QVERIFY(ctx.model().firstRecord() > first);

        // Stop keeps the recording — and where it spilled — until Clear.
        hub.stopRecording(42, 40'000);
        QVERIFY(hub.retention().spill);
        hub.reset(42, 41'000);
        QVERIFY(!hub.retention().spill);
    }

    // A recording started before anything was captured still keeps what
    // arrives — the rolling window never ages it out — and clearing a
    // recording discards what it kept but records on.
    void aRecordingStartedEmptyKeepsItsHistory() {
        auto exec = std::make_shared<InlineExecutor>();
        StoreConfig cfg;
        cfg.chunkRawBytes = 600;
        auto ctx = std::make_shared<CaptureContext>(exec, cfg);
        TimelineHub hub;
        TimelineHub::Budgets b;
        b.rollingWindowMs = 10'000;
        hub.setBudgets(b);
        hub.bindSource(ctx, QStringLiteral("game.exe"), 0);
        hub.startRecording(7, 0);
        QCOMPARE(hub.mode(7), CaptureMode::Recording);
        std::mt19937 rng(11);
        QByteArray p = randomPage(rng);
        auto run = [&](int64_t t0, int n) {
            for (int i = 0; i < n; ++i) {
                p[int(rng() % kPageSize)] = char(rng() & 0xFF);
                p[int(rng() % kPageSize)] = char(rng() & 0xFF);
                ctx->append(tickWith(t0 + i * 100, {{0x1000, p}}));
            }
        };
        run(100, 400);                                   // 40 s against a 10 s window
        QVERIFY(ctx->model().stats().chunks > 3);
        QCOMPARE(ctx->model().firstRecord(), RecordId(0));

        hub.reset(7, 40'200);
        QCOMPARE(hub.mode(7), CaptureMode::Recording);
        QVERIFY(hub.track(7).recordingSinceMs == 40'200);
        run(40'300, 400);
        QVERIFY2(ctx->model().firstRecord() > RecordId(0), "cleared history was still protected");
        QVERIFY(ctx->model().timeOf(ctx->model().firstRecord()) <= 40'300);   // the new recording is kept
    }

    // Two hubs on one context (a document's tabs and an instance tab on the
    // same process): each states its own retention and the context keeps
    // what the most demanding one needs — a hub binding later, with nothing
    // recorded, never trims the other's recording, nor does it going away.
    void hubsSharingAContextMergeTheirRetention() {
        auto exec = std::make_shared<InlineExecutor>();
        StoreConfig cfg;
        cfg.chunkRawBytes = 600;
        auto ctx = std::make_shared<CaptureContext>(exec, cfg);
        TimelineHub::Budgets b;
        b.rollingWindowMs = 10'000;
        TimelineHub recorder;
        recorder.setBudgets(b);
        recorder.bindSource(ctx, QStringLiteral("game.exe"), 0);
        recorder.startRecording(7, 0);
        std::mt19937 rng(13);
        QByteArray p = randomPage(rng);
        auto run = [&](int64_t t0, int n) {
            for (int i = 0; i < n; ++i) {
                p[int(rng() % kPageSize)] = char(rng() & 0xFF);
                p[int(rng() % kPageSize)] = char(rng() & 0xFF);
                ctx->append(tickWith(t0 + i * 100, {{0x1000, p}}));
            }
        };
        {
            TimelineHub instanceTab;
            instanceTab.setBudgets(b);
            instanceTab.bindSource(ctx, QStringLiteral("game.exe"), 50);   // rolling, no pin
            run(100, 400);                                                 // 40 s against a 10 s window
            QVERIFY(ctx->model().stats().chunks > 3);
            QCOMPARE(ctx->model().firstRecord(), RecordId(0));
        }
        run(40'100, 100);                                                  // that hub is gone
        QCOMPARE(ctx->model().firstRecord(), RecordId(0));
    }

    // Onsets: fields the record before did not change. Steady churn starts
    // nothing; a bounce does — the strip's beads.
    void onsetsAreFieldsThatStartedChanging() {
        ClassChangeSeries s;
        s.append(1, 100, 2, {10, 11}, 0, 8);            // the first record starts nothing
        s.append(2, 133, 2, {10, 11}, 0, 8);            // the same fields again
        s.append(3, 166, 3, {10, 11, 12}, 0, 12);       // 12 starts
        s.append(4, 200, 2, {10, 11}, 0, 8);
        s.append(5, 233, 4, {10, 11, 12, 13}, 0, 16);   // 12 and 13 start
        QCOMPARE(s.onsetsOf(1), 0);
        QCOMPARE(s.onsetsOf(2), 0);
        QCOMPARE(s.onsetsOf(3), 1);
        QCOMPARE(s.onsetsOf(4), 0);
        QCOMPARE(s.onsetsOf(5), 2);
        QCOMPARE(s.onsetMax(0, 1000), uint32_t(2));
        QCOMPARE(s.onsetMax(170, 230), uint32_t(0));

        // A flag flipping every few seconds is an event each time: the record
        // before it is too far back to count as "already changing".
        s.append(6, 3233, 1, {20}, 0, 4);
        s.append(7, 6233, 1, {20}, 0, 4);
        QCOMPARE(s.onsetsOf(6), 1);
        QCOMPARE(s.onsetsOf(7), 1);

        // Trimming keeps each count with its record.
        s.dropBefore(3);
        QCOMPARE(s.onsetsOf(3), 1);
        QCOMPARE(s.onsetsOf(5), 2);
        QCOMPARE(s.onsetMax(230, 240), uint32_t(2));

        // Thousands of onsets: the newest are still found — no cap.
        ClassChangeSeries busy;
        for (int k = 0; k < 3000; ++k)
            busy.append(RecordId(k + 1), 100 * k, 1, {uint64_t(k % 2)}, 0, 4);
        QCOMPARE(busy.onsetMax(100 * 2990, 100 * 3000), uint32_t(1));

        // The selection lane: a hit in every column, all the way to the end.
        QVector<char> cols;
        const QVector<FieldSpan> scope{FieldSpan{0, 4, 1}};
        busy.matchColumns(0, 1000, 300, &scope, cols);
        QCOMPARE(cols.size(), 300);
        QVERIFY(cols.first() && cols.last());
        QCOMPARE(int(std::count(cols.cbegin(), cols.cend(), char(1))), 300);
    }

    // A changed span lands on every field it overlaps — unions included, and
    // a long member before a short field is still found.
    void fieldIndexFindsEveryFieldASpanTouches() {
        FieldIndex idx;
        idx.build({{0x110, 8, 5}, {0x100, 4, 1}, {0x104, 4, 2}, {0x108, 8, 3}, {0x108, 4, 4}, {0x200, 0, 9}});
        QCOMPARE(idx.size(), 5);                       // the empty span is dropped
        QVector<int> hits;
        auto idsOf = [&](const QVector<ChangedSpan>& spans) {
            idx.hits(spans, hits);
            QVector<uint64_t> ids;
            for (int h : hits) ids << idx.at(h).id;
            std::sort(ids.begin(), ids.end());
            return ids;
        };
        QCOMPARE(idsOf({{0x103, 2}}), (QVector<uint64_t>{1, 2}));   // straddles two fields
        QCOMPARE(idsOf({{0x10C, 1}}), (QVector<uint64_t>{3}));      // the union's tail only
        QCOMPARE(idsOf({{0x108, 1}}), (QVector<uint64_t>{3, 4}));
        QCOMPARE(idsOf({{0x0F0, 0x10}}), QVector<uint64_t>{});      // ends where the class begins
        QCOMPARE(idsOf({{0x118, 4}}), QVector<uint64_t>{});         // starts where it ends
        QCOMPARE(idsOf({{0x100, 1}, {0x117, 1}}), (QVector<uint64_t>{1, 5}));

        FieldIndex wide;
        wide.build({{0x1000, 0x100, 7}, {0x1004, 4, 8}});
        wide.hits({{0x10F0, 1}}, hits);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(wide.at(hits[0]).id, uint64_t(7));

        FieldIndex top;
        top.build({{UINT64_MAX - 3, 4, 1}});
        top.hits({{UINT64_MAX - 1, 2}}, hits);
        QCOMPARE(hits.size(), 1);
    }

    // Fields changed per record: kept only when something changed, stepped
    // by class or by field, trimmed with retention, merged after a recount.
    void classChangeSeriesCountsStepsAndTrims() {
        ClassChangeSeries s;
        s.append(10, 1000, 2, {1, 2}, 0x100, 0x108);
        s.append(11, 1100, 0, {}, 0, 0);                  // nothing in the class: not kept
        s.append(12, 1200, 1, {2}, 0x104, 0x108);
        s.append(12, 1250, 1, {1}, 0x100, 0x104);         // replayed: ignored
        QVector<uint64_t> many;
        for (uint64_t i = 0; i < 40; ++i) many << 100 + i;
        s.append(15, 1500, 40, many, 0x200, 0x300);       // more fields than it lists
        QCOMPARE(s.size(), 3);
        QCOMPARE(s.countOf(11), 0);
        QCOMPARE(s.countOf(15), 40);
        QCOMPARE(s.idsOf(15).size(), ClassChangeSeries::kMaxIdsPerRecord);
        QCOMPARE(s.idsOf(12), (QVector<uint64_t>{2}));

        const auto agg = s.aggregate(1000, 1300);
        QCOMPARE(agg.max, uint32_t(2));
        QCOMPARE(agg.sum, uint64_t(3));
        QCOMPARE(agg.count, 2);

        QCOMPARE(s.previous(16), RecordId(15));
        QCOMPARE(s.previous(15), RecordId(12));
        QCOMPARE(s.previous(12, nullptr, 11), kNoRecord);  // below the floor
        QCOMPARE(s.next(10), RecordId(12));
        QCOMPARE(s.next(15), kNoRecord);

        const QVector<FieldSpan> field1 = {{0x100, 4, 1}};
        QCOMPARE(s.previous(16, &field1), RecordId(10));
        QCOMPARE(s.next(10, &field1), kNoRecord);
        const QVector<FieldSpan> late = {{0x2F0, 4, 999}};   // only in the big record's hull
        QCOMPARE(s.previous(16, &late), RecordId(15));
        QCOMPARE(s.previous(15, &late), kNoRecord);
        QCOMPARE(s.times(0, 2000, &field1), (QVector<int64_t>{1000}));
        QCOMPARE(s.times(0, 2000), (QVector<int64_t>{1000, 1200, 1500}));

        ClassChangeSeries recount;
        recount.append(8, 800, 1, {1}, 0x100, 0x104);
        recount.append(10, 1000, 2, {1, 2}, 0x100, 0x108);
        recount.appendNewer(s);
        QCOMPARE(recount.size(), 4);
        QCOMPARE(recount.previous(10), RecordId(8));
        QCOMPARE(recount.idsOf(12), (QVector<uint64_t>{2}));
        QCOMPARE(recount.countOf(15), 40);

        recount.dropBefore(11);
        QCOMPARE(recount.size(), 2);
        QCOMPARE(recount.idsOf(12), (QVector<uint64_t>{2}));
        QCOMPARE(recount.idsOf(15).size(), ClassChangeSeries::kMaxIdsPerRecord);
        QCOMPARE(recount.aggregate(0, 5000).sum, uint64_t(41));
        recount.dropBefore(100);
        QVERIFY(recount.isEmpty());
    }
};

QTEST_MAIN(TestTimeline)
#include "test_timeline.moc"
