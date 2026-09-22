// Class timelines, end to end through the controller.
//
// A scripted live "process" changes a struct on command; the real refresh
// loop reads it; the timeline records it; the view is rewound. What is
// pinned here is what the feature promises the user:
//
//   · the past shows exactly what the view showed then — and gets it from
//     the timeline, never by quietly reading today's memory;
//   · a rebase does not lose history, and an old moment is shown at the
//     address it was captured at;
//   · you cannot write while looking at the past;
//   · capture keeps going while Reclass is minimised, without composing;
//   · stepping change to change lands on changes, and past the newest
//     change you are back to live.

#include <QtTest/QTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QMutex>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <cstring>

#include "controller.h"
#include "core.h"
#include "providers/provider.h"
#include "timeline/capture_context.h"
#include "timeline/executor.h"
#include "timeline/timeline_hub.h"
#include "timeline/timeline_service.h"

using namespace rcx;

namespace {

// A live process whose memory the test writes, with a tripwire: while
// `forbidUiReads` is set, any read made ON THE UI THREAD is counted and
// answered with 0xEE bytes. The refresh loop's reads run on a worker and
// are fine; compose, hover previews, copy — anything that renders — runs on
// the UI thread and must be served by the timeline instead.
class ScriptedLiveProvider : public Provider {
public:
    static constexpr uint64_t kBaseA = 0x10000;
    static constexpr uint64_t kBaseB = 0x20000;
    static constexpr int      kSize  = 0x40000;

    mutable QMutex          mutex;
    QByteArray              mem{kSize, '\0'};
    std::atomic<bool>       forbidUiReads{false};
    mutable std::atomic<int> illegalReads{0};
    std::atomic<int>        writes{0};
    QThread*                uiThread = QThread::currentThread();

    bool read(uint64_t addr, void* buf, int len) const override {
        if (forbidUiReads.load() && QThread::currentThread() == uiThread) {
            illegalReads.fetch_add(1);
            std::memset(buf, 0xEE, size_t(len));
            return true;
        }
        QMutexLocker lock(&mutex);
        if (len <= 0 || addr + uint64_t(len) > uint64_t(mem.size())) return false;
        std::memcpy(buf, mem.constData() + addr, size_t(len));
        return true;
    }
    bool write(uint64_t addr, const void* buf, int len) override {
        writes.fetch_add(1);
        QMutexLocker lock(&mutex);
        if (len <= 0 || addr + uint64_t(len) > uint64_t(mem.size())) return false;
        std::memcpy(mem.data() + addr, buf, size_t(len));
        return true;
    }
    int size() const override { return kSize; }
    bool isWritable() const override { return true; }
    bool isLive() const override { return true; }
    QString name() const override { return QStringLiteral("scripted.exe"); }
    QString kind() const override { return QStringLiteral("Process"); }

    void poke32(uint64_t addr, uint32_t v) {
        QMutexLocker lock(&mutex);
        std::memcpy(mem.data() + addr, &v, 4);
    }
};

} // namespace

class TestTimelineCapture : public QObject {
    Q_OBJECT
private:
    RcxDocument* m_doc = nullptr;
    RcxController* m_ctrl = nullptr;
    ScriptedLiveProvider* m_prov = nullptr;
    uint64_t m_rootId = 0;
    QTemporaryDir m_spillRoot;

    // `record`: press Record for the class, as a user would before anything
    // is kept.
    void setup(bool record = true) {
        m_doc = new RcxDocument();
        NodeTree& tree = m_doc->tree;
        tree.baseAddress = ScriptedLiveProvider::kBaseA;
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Player");
        root.name = QStringLiteral("Player");
        root.parentId = 0;
        m_rootId = tree.nodes[tree.addNode(root)].id;
        auto field = [&](int off, NodeKind k, const char* name) {
            Node n;
            n.kind = k;
            n.name = QString::fromLatin1(name);
            n.parentId = m_rootId;
            n.offset = off;
            tree.addNode(n);
        };
        field(0, NodeKind::UInt32, "health");
        field(4, NodeKind::UInt32, "ammo");
        field(8, NodeKind::Hex64, "blob");

        auto prov = std::make_shared<ScriptedLiveProvider>();
        m_prov = prov.get();
        m_prov->poke32(ScriptedLiveProvider::kBaseA, 100);
        m_prov->poke32(ScriptedLiveProvider::kBaseB, 555);
        m_doc->provider = prov;

        m_ctrl = new RcxController(m_doc, nullptr);
        m_ctrl->setViewRootId(m_rootId);
        m_ctrl->setRefreshInterval(20);
        if (record) m_ctrl->timelineToggleRecording();
    }

    tl::RecordId lastRecord() const {
        auto* ctx = m_ctrl->timelineContext();
        return (ctx && !ctx->model().isEmpty()) ? ctx->model().lastRecord() : tl::kNoRecord;
    }

    // Spin until the timeline holds a record newer than `after`.
    bool waitForRecordAfter(tl::RecordId after, int timeoutMs = 4000) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            QTest::qWait(10);
            const tl::RecordId r = lastRecord();
            if (r != tl::kNoRecord && (after == tl::kNoRecord || r > after)) return true;
        }
        return false;
    }

    // The composed text of one field's line.
    QString lineOf(const char* name) const {
        const ComposeResult& res = m_ctrl->lastResult();
        const QStringList lines = res.text.split(QLatin1Char('\n'));
        for (int i = 0; i < res.meta.size() && i < lines.size(); ++i) {
            const LineMeta& lm = res.meta[i];
            if (lm.lineKind != LineKind::Field || lm.nodeIdx < 0
                || lm.nodeIdx >= m_doc->tree.nodes.size()) continue;
            if (m_doc->tree.nodes[lm.nodeIdx].name == QLatin1String(name)) return lines[i];
        }
        return {};
    }

    const LineMeta* metaOf(const char* name) const {
        for (const LineMeta& lm : m_ctrl->lastResult().meta) {
            if (lm.lineKind != LineKind::Field || lm.nodeIdx < 0
                || lm.nodeIdx >= m_doc->tree.nodes.size()) continue;
            if (m_doc->tree.nodes[lm.nodeIdx].name == QLatin1String(name)) return &lm;
        }
        return nullptr;
    }

    // Change `health` and wait until the view shows it. Returns the record
    // that captured the change and the line as the live view drew it.
    QPair<tl::RecordId, QString> setHealth(uint32_t v) {
        const tl::RecordId before = lastRecord();
        m_prov->poke32(m_doc->tree.baseAddress, v);
        if (!waitForRecordAfter(before)) return {tl::kNoRecord, {}};
        // The compose for that tick has run by now (same UI-thread turn).
        QTest::qWait(30);
        return {lastRecord(), lineOf("health")};
    }

private slots:
    void initTestCase() {
        // Deterministic timeline: ingest and frames run inline on the UI thread.
        tl::TimelineService::instance().setExecutorFactoryForTest(
            [] { return std::make_shared<tl::InlineExecutor>(); });
        // Recording spills: keep it out of the real cache location.
        QVERIFY(m_spillRoot.isValid());
        tl::TimelineService::instance().setSpillRootForTest(m_spillRoot.path());
    }

    void cleanup() {
        delete m_ctrl; m_ctrl = nullptr;
        delete m_doc;  m_doc = nullptr;
        m_prov = nullptr;
    }

    // A scrub stops where it is released. The moment asked for is the
    // playhead's own; what it SHOWS is the last record at or before it — so
    // standing between two records is a place to stand.
    void parkingBetweenChangesKeepsTheTimeAsked() {
        setup();
        const auto first = setHealth(111);
        QVERIFY(first.first != tl::kNoRecord);
        QTest::qWait(60);
        const auto second = setHealth(222);
        QVERIFY(second.first != tl::kNoRecord);
        const tl::TimelineModel& m = m_ctrl->timelineContext()->model();
        const int64_t t0 = m.timeOf(first.first), t1 = m.timeOf(second.first);
        QVERIFY2(t1 - t0 >= 2, "the two records share a millisecond");
        const int64_t between = t0 + (t1 - t0) / 2;

        m_ctrl->viewTimelineAt(between);
        QVERIFY(m_ctrl->isViewingPast());
        QCOMPARE(m_ctrl->pastTimeMs(), between);         // where it was released
        QCOMPARE(m_ctrl->pastRecord(), first.first);     // what that moment shows
        QCOMPARE(m_ctrl->pastRecordTimeMs(), t0);

        // Landing ON a record — stepping, "Show in Timeline" — puts both together.
        m_ctrl->viewTimelineRecord(second.first);
        QCOMPARE(m_ctrl->pastRecord(), second.first);
        QCOMPARE(m_ctrl->pastTimeMs(), t1);

        // A cancelled scrub puts back the moment, not merely the record.
        m_ctrl->viewTimelineAt(between);
        m_ctrl->beginTimelineScrub();
        m_ctrl->viewTimelineAt(t1);
        m_ctrl->cancelTimelineScrub();
        QCOMPARE(m_ctrl->pastRecord(), first.first);
        QCOMPARE(m_ctrl->pastTimeMs(), between);

        m_ctrl->returnToLive();
        QCOMPARE(m_ctrl->pastTimeMs(), int64_t(0));
    }

    // Nothing is kept until Record. The first record holds everything on
    // screen, not only what changed after the press; Stop keeps what was
    // recorded, and nothing more is kept after it.
    void nothingIsKeptUntilRecord() {
        setup(/*record=*/false);
        QVERIFY(m_ctrl->timelineContext() != nullptr);
        QVERIFY(m_ctrl->timelineHub() != nullptr);
        QVERIFY(!m_ctrl->timelineRecording());
        m_prov->poke32(m_doc->tree.baseAddress, 5);
        QTest::qWait(250);
        QVERIFY2(lastRecord() == tl::kNoRecord, "captured without Record");
        QVERIFY(lineOf("health").contains(QStringLiteral("0x5")));   // the view is live all the same

        m_ctrl->timelineToggleRecording();
        QVERIFY(m_ctrl->timelineRecording());
        QCOMPARE(m_ctrl->timelineHub()->mode(m_ctrl->timelineClassId()), tl::CaptureMode::Recording);
        QVERIFY2(waitForRecordAfter(tl::kNoRecord), "no first record after Record");
        const tl::RecordId first = m_ctrl->timelineContext()->model().firstRecord();
        m_prov->forbidUiReads = true;
        m_ctrl->viewTimelineRecord(first);
        const LineMeta* health = metaOf("health");
        QVERIFY(health);
        QVERIFY2(!health->notCaptured, "the first record did not hold what was already on screen");
        QVERIFY2(lineOf("health").contains(QStringLiteral("0x5")), qPrintable(lineOf("health")));
        QCOMPARE(m_prov->illegalReads.load(), 0);
        m_prov->forbidUiReads = false;
        m_ctrl->returnToLive();

        const auto kept = setHealth(6);
        QVERIFY(kept.first != tl::kNoRecord);
        m_ctrl->timelineToggleRecording();
        QVERIFY(!m_ctrl->timelineRecording());
        QTest::qWait(60);
        const tl::RecordId atStop = lastRecord();
        m_prov->poke32(m_doc->tree.baseAddress, 7);
        QTest::qWait(250);
        QCOMPARE(lastRecord(), atStop);                                  // nothing after Stop
        QVERIFY(lineOf("health").contains(QStringLiteral("0x7")));      // still live on screen
        QVERIFY(m_ctrl->timelineContext()->model().contains(kept.first)); // the recording stays
        QVERIFY(m_ctrl->timelineHub()->isHiddenFor(m_rootId, tl::CaptureClock::nowMs()));
        QVERIFY(!m_ctrl->isViewingPast());
    }

    // THE promise: the past is what the view showed then, and it comes from
    // the timeline — not from today's memory.
    void thePastIsWhatWasShownAndNeverReadsLiveMemory() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        const auto at2 = setHealth(2);
        QVERIFY(at2.first != tl::kNoRecord);
        const auto at3 = setHealth(3);
        QVERIFY(at3.first != tl::kNoRecord);
        QVERIFY(at2.second != at3.second);

        m_prov->forbidUiReads = true;
        m_ctrl->viewTimelineRecord(at2.first);
        QVERIFY(m_ctrl->isViewingPast());
        QCOMPARE(m_ctrl->pastRecord(), at2.first);
        QCOMPARE(lineOf("health"), at2.second);

        // Let live ticks run underneath: the past must not move or leak.
        m_prov->poke32(m_doc->tree.baseAddress, 4);
        QTest::qWait(150);
        QCOMPARE(lineOf("health"), at2.second);
        QVERIFY(lastRecord() > at3.first);           // capture continued

        m_ctrl->viewTimelineRecord(at3.first);
        QCOMPARE(lineOf("health"), at3.second);
        QCOMPARE(m_prov->illegalReads.load(), 0);

        m_prov->forbidUiReads = false;
        m_ctrl->returnToLive();
        QVERIFY(!m_ctrl->isViewingPast());
        QTest::qWait(60);
        QVERIFY(lineOf("health").contains(QStringLiteral("4")));
    }

    // Rebasing moves the view to another object; the history of the first
    // object is still there, drawn at the address it had.
    void aRebaseKeepsHistoryAtTheOldAddress() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        const auto atA = setHealth(7);
        QVERIFY(atA.first != tl::kNoRecord);
        const QString healthAtA = atA.second;
        const auto* ctxBefore = m_ctrl->timelineContext();

        QVERIFY(m_ctrl->navigateToAddress(ScriptedLiveProvider::kBaseB));
        QCOMPARE(m_doc->tree.baseAddress, uint64_t(ScriptedLiveProvider::kBaseB));
        QVERIFY(waitForRecordAfter(atA.first));
        QTest::qWait(60);
        QVERIFY(lineOf("health") != healthAtA);
        QCOMPARE(m_ctrl->timelineContext(), ctxBefore);   // same process, same history

        bool sawRebase = false;
        for (const auto& e : m_ctrl->timelineHub()->events())
            if (e.kind == tl::TimelineEventKind::Rebase) sawRebase = true;
        QVERIFY(sawRebase);

        m_prov->forbidUiReads = true;
        m_ctrl->viewTimelineRecord(atA.first);
        QCOMPARE(lineOf("health"), healthAtA);
        const LineMeta* lm = metaOf("health");
        QVERIFY(lm);
        QCOMPARE(lm->offsetAddr, uint64_t(ScriptedLiveProvider::kBaseA));
        QCOMPARE(m_prov->illegalReads.load(), 0);
        m_prov->forbidUiReads = false;
    }

    void writesAreRefusedInThePast() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        const auto at = setHealth(11);
        QVERIFY(at.first != tl::kNoRecord);
        m_ctrl->viewTimelineRecord(at.first);
        QVERIFY(m_ctrl->isViewingPast());

        QSignalSpy hints(m_ctrl, &RcxController::statusHint);
        const int writesBefore = m_prov->writes.load();
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); ++i)
            if (m_doc->tree.nodes[i].name == QStringLiteral("health")) idx = i;
        QVERIFY(idx >= 0);
        m_ctrl->setNodeValue(idx, 0, QStringLiteral("999"));
        QApplication::processEvents();
        QCOMPARE(m_prov->writes.load(), writesBefore);
        bool said = false;
        for (const auto& args : hints)
            if (args.value(0).toString().contains(QStringLiteral("read-only"))) said = true;
        QVERIFY2(said, "no read-only hint for a write in the past");

        m_ctrl->returnToLive();
        m_ctrl->setNodeValue(idx, 0, QStringLiteral("999"));
        QVERIFY(m_prov->writes.load() > writesBefore);
    }

    // Playing the target with Reclass minimised, then rewinding, is the core
    // use: capture keeps going, the view does not compose until it is back.
    void minimisedKeepsCapturingWithoutComposing() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        const auto at = setHealth(20);
        QVERIFY(at.first != tl::kNoRecord);
        const QString shown = lineOf("health");

        m_ctrl->setWindowState(/*focused=*/false, /*visible=*/false);
        QVERIFY2(m_ctrl->refreshTimerActive(), "the timer stopped while the timeline is capturing");
        m_prov->poke32(m_doc->tree.baseAddress, 21);
        QVERIFY2(waitForRecordAfter(at.first), "no capture while minimised");
        QTest::qWait(60);
        QCOMPARE(lineOf("health"), shown);                  // nothing composed

        m_ctrl->setWindowState(/*focused=*/true, /*visible=*/true);
        QApplication::processEvents();
        QVERIFY(lineOf("health") != shown);                 // one compose on restore

        // With background capture off, the old throttle is back.
        m_ctrl->setTimelineBackgroundCapture(false);
        m_ctrl->setWindowState(false, false);
        QVERIFY(!m_ctrl->refreshTimerActive());
        m_ctrl->setWindowState(true, true);
    }

    void steppingLandsOnChangesAndEndsAtLive() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        const auto a = setHealth(31);
        const auto b = setHealth(32);
        const auto c = setHealth(33);
        QVERIFY(a.first != tl::kNoRecord && b.first != tl::kNoRecord && c.first != tl::kNoRecord);

        QVERIFY(m_ctrl->stepTimelineChange(-1));
        QVERIFY(m_ctrl->isViewingPast());
        const tl::RecordId newest = m_ctrl->pastRecord();
        QVERIFY(newest >= c.first);
        QVERIFY(m_ctrl->timelineContext()->model().changedBytesOf(newest) > 0);

        QVERIFY(m_ctrl->stepTimelineChange(-1));
        QVERIFY(m_ctrl->pastRecord() < newest);

        // Forward past the newest change: live again.
        for (int i = 0; i < 50 && m_ctrl->isViewingPast(); ++i)
            QVERIFY(m_ctrl->stepTimelineChange(+1));
        QVERIFY(!m_ctrl->isViewingPast());
        QVERIFY(!m_ctrl->stepTimelineChange(+1));
    }

    // Record / Stop act on the class shown, and the view stays live the whole
    // time — there is no pausing it. Clear discards what was kept.
    void recordAndClearActOnTheClassShown() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        const uint64_t cls = m_ctrl->timelineClassId();
        QCOMPARE(cls, m_rootId);
        auto* hub = m_ctrl->timelineHub();
        QCOMPARE(hub->mode(cls), tl::CaptureMode::Recording);

        const tl::RecordId newest = lastRecord();
        m_prov->poke32(m_doc->tree.baseAddress, 777);
        QVERIFY2(waitForRecordAfter(newest), "nothing recorded");
        QTest::qWait(60);
        QVERIFY2(lineOf("health").contains(QStringLiteral("0x309")), qPrintable(lineOf("health")));   // live

        const auto at = setHealth(40);
        QVERIFY(at.first != tl::kNoRecord);
        m_ctrl->viewTimelineRecord(at.first);
        QVERIFY(m_ctrl->isViewingPast());
        m_ctrl->timelineReset();
        QVERIFY(!m_ctrl->isViewingPast());                  // the past just shown is gone
        QVERIFY(hub->isHiddenFor(cls, m_ctrl->timelineContext()->model().timeOf(at.first)));
        QCOMPARE(hub->mode(cls), tl::CaptureMode::Recording);   // Clear does not stop recording

        m_ctrl->timelineToggleRecording();
        QCOMPARE(hub->mode(cls), tl::CaptureMode::Rolling);
    }

    // Bytes nobody was watching then read "??" in the past — never a zero
    // that looks like data, and never the red unreadable strike: nothing
    // failed, they simply were not captured.
    void uncapturedBytesReadAsQuestionMarks() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        const auto before = setHealth(50);
        QVERIFY(before.first != tl::kNoRecord);

        // A field on a page the view was not watching until now.
        m_prov->poke32(m_doc->tree.baseAddress + 0x3000, 77);
        m_ctrl->insertNode(m_rootId, 0x3000, NodeKind::UInt32, QStringLiteral("far"));
        QVERIFY(waitForRecordAfter(before.first));
        QTest::qWait(60);
        const QString liveFar = lineOf("far");
        // uint32 values render in hex: 77 is 0x4d.
        QVERIFY2(liveFar.contains(QStringLiteral("0x4d")), qPrintable(liveFar));

        m_ctrl->viewTimelineRecord(before.first);
        const LineMeta* far = metaOf("far");
        QVERIFY(far);
        QVERIFY2(far->notCaptured, "an unwatched field is not marked not-captured");
        const QString pastFar = lineOf("far");
        QVERIFY2(pastFar.contains(QStringLiteral("??")), qPrintable(pastFar));
        QVERIFY(!pastFar.contains(QStringLiteral("4d")));
        QVERIFY(!pastFar.contains(QStringLiteral("0x0")));   // and not a plausible zero either
        QCOMPARE(pastFar.size(), liveFar.size());     // no column moved

        const LineMeta* health = metaOf("health");
        QVERIFY(health);
        QVERIFY(!health->notCaptured);                // watched then: real data
    }

    // A field that SHOWS another page's bytes — a primitive pointer that
    // dereferences its target — must have that page captured too, or the
    // past could only show it by reading today's memory. The range walk does
    // not follow primitive pointers; recording what compose reads does.
    void dereferencedTargetsAreCapturedForThePast() {
        setup();
        const uint64_t target = ScriptedLiveProvider::kBaseB + 0x100;   // another page entirely
        m_prov->poke32(target, 1234);
        uint64_t ptrValue = target;
        {
            QMutexLocker lock(&m_prov->mutex);
            std::memcpy(m_prov->mem.data() + ScriptedLiveProvider::kBaseA + 16, &ptrValue, 8);
        }
        int idx = -1;
        {
            Node p;
            p.kind = NodeKind::Pointer64;
            p.name = QStringLiteral("hp_ptr");
            p.parentId = m_rootId;
            p.offset = 16;
            p.ptrDepth = 1;
            p.elementKind = NodeKind::UInt32;
            idx = m_doc->tree.addNode(p);
            emit m_doc->documentChanged();
        }
        QVERIFY(idx >= 0);
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        // Give the touched-page set a few ticks to pull the target page in.
        QTest::qWait(250);
        const QString at1234 = lineOf("hp_ptr");
        QVERIFY2(!at1234.isEmpty(), "no line for the dereferencing pointer");
        const tl::RecordId before = lastRecord();

        m_prov->poke32(target, 4321);
        QVERIFY(waitForRecordAfter(before));
        QTest::qWait(80);
        const QString at4321 = lineOf("hp_ptr");
        QVERIFY2(at4321 != at1234, "the dereferenced value never updated live");

        m_prov->forbidUiReads = true;
        m_ctrl->viewTimelineRecord(before);
        QCOMPARE(lineOf("hp_ptr"), at1234);
        QCOMPARE(m_prov->illegalReads.load(), 0);
        m_prov->forbidUiReads = false;
    }

    // A pointer and what it points at are one moment. When a pointer inside
    // an embedded struct moves to an object on a page nobody watched, the
    // record where it moved already holds that object: the past at that
    // record shows the new target's value, not "??" and not the old target.
    void aMovedPointerAndItsNewTargetShareOneRecord() {
        setup();
        NodeTree& tree = m_doc->tree;
        Node target;
        target.kind = NodeKind::Struct;
        target.structTypeName = QStringLiteral("Target");
        target.name = QStringLiteral("Target");
        const uint64_t targetId = tree.nodes[tree.addNode(target)].id;
        Node value;
        value.kind = NodeKind::UInt32;
        value.name = QStringLiteral("value");
        value.parentId = targetId;
        tree.addNode(value);

        Node inner;
        inner.kind = NodeKind::Struct;
        inner.structTypeName = QStringLiteral("Inner");
        inner.name = QStringLiteral("inner");
        inner.parentId = m_rootId;
        inner.offset = 0x20;
        inner.collapsed = false;
        const uint64_t innerId = tree.nodes[tree.addNode(inner)].id;
        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = QStringLiteral("obj");
        ptr.parentId = innerId;
        ptr.offset = 8;
        ptr.refId = targetId;
        ptr.collapsed = false;
        tree.addNode(ptr);

        const uint64_t t1 = ScriptedLiveProvider::kBaseB + 0x100;
        const uint64_t t2 = ScriptedLiveProvider::kBaseB + 0x9100;  // a page never watched
        const uint64_t ptrAddr = ScriptedLiveProvider::kBaseA + 0x28;
        m_prov->poke32(t1, 0x111);
        m_prov->poke32(t2, 0x222);
        {
            QMutexLocker lock(&m_prov->mutex);
            std::memcpy(m_prov->mem.data() + ptrAddr, &t1, 8);
        }
        emit m_doc->documentChanged();

        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        QTest::qWait(150);
        QVERIFY2(lineOf("value").contains(QStringLiteral("0x111")), qPrintable(lineOf("value")));
        const tl::RecordId before = lastRecord();

        // Only the pointer moves; the object it now points at was already there.
        {
            QMutexLocker lock(&m_prov->mutex);
            std::memcpy(m_prov->mem.data() + ptrAddr, &t2, 8);
        }
        QVERIFY(waitForRecordAfter(before));
        QTest::qWait(80);
        QVERIFY2(lineOf("value").contains(QStringLiteral("0x222")), qPrintable(lineOf("value")));

        const tl::RecordId moved = before + 1;
        m_prov->forbidUiReads = true;
        m_ctrl->viewTimelineRecord(moved);
        QVERIFY(m_ctrl->isViewingPast());
        const QString past = lineOf("value");
        QVERIFY2(past.contains(QStringLiteral("0x222")), qPrintable(past));
        const LineMeta* lm = metaOf("value");
        QVERIFY(lm);
        QVERIFY2(!lm->notCaptured, "the new target was not captured in the tick the pointer moved");
        m_ctrl->viewTimelineRecord(before);
        QVERIFY2(lineOf("value").contains(QStringLiteral("0x111")), qPrintable(lineOf("value")));
        QCOMPARE(m_prov->illegalReads.load(), 0);
        m_prov->forbidUiReads = false;
    }

    // A scrub cancelled from a moment being shown puts that moment back; one
    // cancelled from live is live again.
    void aCancelledScrubPutsBackWhatWasShown() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        const auto first = setHealth(90);
        const auto second = setHealth(91);
        QVERIFY(first.first != tl::kNoRecord && second.first != tl::kNoRecord);
        const tl::TimelineModel& m = m_ctrl->timelineContext()->model();

        m_ctrl->viewTimelineRecord(first.first);
        m_ctrl->beginTimelineScrub();
        m_ctrl->viewTimelineRecord(second.first);            // the drag moved
        m_ctrl->cancelTimelineScrub();
        QVERIFY(m_ctrl->isViewingPast());
        QCOMPARE(m_ctrl->pastRecord(), first.first);

        m_ctrl->returnToLive();
        QVERIFY(!m_ctrl->isViewingPast());
        m_ctrl->beginTimelineScrub();
        m_ctrl->viewTimelineAt(m.timeOf(m.firstRecord()));
        m_ctrl->cancelTimelineScrub();
        QVERIFY(!m_ctrl->isViewingPast());
    }

    // What the address bar's buttons read: Record for a live source before
    // anything is recorded, Stop while recording, Back to live only while
    // looking back at a recording, and nothing with the timeline switched off.
    void theAddressBarButtonsReadTheTimeline() {
        setup(/*record=*/false);
        QTest::qWait(80);
        AddressBarState s = m_ctrl->addressBarStateForTest();
        QVERIFY(s.timeline);
        QVERIFY(s.canRecord);
        QVERIFY(!s.past);
        QVERIFY(!s.recording);
        m_ctrl->timelineToggleRecording();
        QVERIFY(m_ctrl->addressBarStateForTest().recording);
        QVERIFY(waitForRecordAfter(tl::kNoRecord));

        const tl::TimelineModel& m = m_ctrl->timelineContext()->model();
        m_ctrl->viewTimelineRecord(m.firstRecord());
        QVERIFY(m_ctrl->addressBarStateForTest().past);
        m_ctrl->returnToLive();                             // "Back to live"
        QVERIFY(!m_ctrl->addressBarStateForTest().past);

        m_ctrl->setTimelineEnabled(false);
        QVERIFY(!m_ctrl->addressBarStateForTest().timeline);
        m_ctrl->setTimelineEnabled(true);
    }

    // Switching the timeline off takes its buttons away, so it goes back to
    // live: the view is never left on a recorded moment with no way back.
    void turningTheTimelineOffGoesBackToLive() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        m_ctrl->viewTimelineRecord(m_ctrl->timelineContext()->model().firstRecord());
        QVERIFY(m_ctrl->isViewingPast());
        m_ctrl->setTimelineEnabled(false);
        QVERIFY(!m_ctrl->isViewingPast());
        m_ctrl->setTimelineEnabled(true);
    }

    // After Clear there is nothing to look back at: no visible record, and a
    // moment from before the Clear never comes back on screen.
    void aClearedRecordingCannotBeReached() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        const auto at = setHealth(70);
        QVERIFY(at.first != tl::kNoRecord);
        const int64_t atMs = m_ctrl->timelineContext()->model().timeOf(at.first);
        m_ctrl->timelineToggleRecording();   // Stop
        m_ctrl->timelineReset();             // Clear
        QCOMPARE(m_ctrl->timelineHub()->firstVisibleRecord(m_ctrl->timelineClassId()), tl::kNoRecord);
        m_ctrl->viewTimelineAt(atMs);
        QVERIFY2(!m_ctrl->isViewingPast(), "a cleared moment came back on screen");
        m_ctrl->viewTimelineAt(tl::CaptureClock::nowMs());
        QVERIFY(!m_ctrl->isViewingPast());
        QVERIFY(!m_ctrl->stepTimelineChange(-1));
    }

    // A class never recorded has no history, even while another class on the
    // same source is recording: nothing to step to, nothing to look back at.
    void aClassNeverRecordedHasNoHistory() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        QVERIFY(setHealth(71).first != tl::kNoRecord);
        Node other;
        other.kind = NodeKind::Struct;
        other.structTypeName = QStringLiteral("Other");
        other.name = QStringLiteral("Other");
        const uint64_t otherId = m_doc->tree.nodes[m_doc->tree.addNode(other)].id;
        emit m_doc->documentChanged();
        m_ctrl->setViewRootId(otherId);
        QCOMPARE(m_ctrl->timelineClassId(), otherId);
        QVERIFY(!m_ctrl->timelineRecording());
        QCOMPARE(m_ctrl->timelineHub()->firstVisibleRecord(otherId), tl::kNoRecord);
        QVERIFY(!m_ctrl->stepTimelineChange(-1));
        m_ctrl->viewTimelineAt(tl::CaptureClock::nowMs());
        QVERIFY(!m_ctrl->isViewingPast());
    }

    // A matrix's rows each light when THEY change — not only row 0, which
    // used to be the only one with a value history.
    void everyMatrixRowLightsWhenItChanges() {
        setup(/*record=*/false);
        {
            Node mat;
            mat.kind = NodeKind::Mat4x4;
            mat.name = QStringLiteral("view");
            mat.parentId = m_rootId;
            mat.offset = 0x40;
            m_doc->tree.addNode(mat);
            emit m_doc->documentChanged();
        }
        m_ctrl->setTrackValues(true);
        const uint64_t row2 = m_doc->tree.baseAddress + 0x40 + 2 * 16;   // row 2, column 0
        auto rowHeat = [&](int row) {
            for (const LineMeta& lm : m_ctrl->lastResult().meta)
                if (lm.nodeKind == NodeKind::Mat4x4 && lm.subLine == row) return lm.heatLevel;
            return -1;
        };
        QTest::qWait(120);
        QCOMPARE(rowHeat(2), 0);
        for (int i = 1; i <= 3; ++i) {
            const float v = float(i) * 1.5f;
            uint32_t bits = 0;
            std::memcpy(&bits, &v, 4);
            m_prov->poke32(row2, bits);
            QTest::qWait(120);
        }
        QVERIFY2(rowHeat(2) > 0, qPrintable(QStringLiteral("row 2 heat %1").arg(rowHeat(2))));
        QCOMPARE(rowHeat(0), 0);
        QCOMPARE(rowHeat(1), 0);
        QCOMPARE(rowHeat(3), 0);
    }

    // A pointer whose target nobody was watching at that moment shows "??"
    // for the target's fields in the past — not the zeros of a null target.
    void anUnwatchedPointerTargetReadsAsQuestionMarks() {
        setup();
        const uint64_t target = ScriptedLiveProvider::kBaseB + 0x9100;
        const uint64_t ptrAddr = ScriptedLiveProvider::kBaseA + 0x28;
        m_prov->poke32(target, 0x333);
        {
            QMutexLocker lock(&m_prov->mutex);
            std::memcpy(m_prov->mem.data() + ptrAddr, &target, 8);
        }
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        const auto before = setHealth(61);
        QVERIFY(before.first != tl::kNoRecord);

        NodeTree& tree = m_doc->tree;
        Node cls;
        cls.kind = NodeKind::Struct;
        cls.structTypeName = QStringLiteral("Target");
        cls.name = QStringLiteral("Target");
        const uint64_t targetId = tree.nodes[tree.addNode(cls)].id;
        Node value;
        value.kind = NodeKind::UInt32;
        value.name = QStringLiteral("value");
        value.parentId = targetId;
        tree.addNode(value);
        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = QStringLiteral("obj");
        ptr.parentId = m_rootId;
        ptr.offset = 0x28;
        ptr.refId = targetId;
        ptr.collapsed = false;
        tree.addNode(ptr);
        emit m_doc->documentChanged();
        QVERIFY(waitForRecordAfter(before.first));
        QTest::qWait(100);
        QVERIFY2(lineOf("value").contains(QStringLiteral("0x333")), qPrintable(lineOf("value")));

        m_prov->forbidUiReads = true;
        m_ctrl->viewTimelineRecord(before.first);
        const QString past = lineOf("value");
        QVERIFY2(past.contains(QStringLiteral("??")), qPrintable(past));
        const LineMeta* lm = metaOf("value");
        QVERIFY(lm);
        QVERIFY(lm->notCaptured);
        QCOMPARE(m_prov->illegalReads.load(), 0);
        m_prov->forbidUiReads = false;
    }

    // The graph counts FIELDS: health and ammo changing together is two
    // changes, however many bytes; the readout names them.
    void changesAreCountedInFields() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        QTest::qWait(60);
        QVERIFY(m_ctrl->timelineCountsFields());
        const tl::RecordId before = lastRecord();
        {
            QMutexLocker lock(&m_prov->mutex);
            const uint32_t health = 0x01020304, ammo = 0x0A0B0C0D;
            std::memcpy(m_prov->mem.data() + ScriptedLiveProvider::kBaseA, &health, 4);
            std::memcpy(m_prov->mem.data() + ScriptedLiveProvider::kBaseA + 4, &ammo, 4);
        }
        QVERIFY(waitForRecordAfter(before));
        QTest::qWait(30);
        const tl::ClassChangeSeries& series = m_ctrl->timelineClassChanges();
        const tl::RecordId r = series.lastRecord();
        QVERIFY(r != tl::kNoRecord && r > before);
        QCOMPARE(series.countOf(r), 2);
        const QString said = m_ctrl->describeTimelineAt(m_ctrl->timelineContext()->model().timeOf(r));
        QVERIFY2(said.startsWith(QStringLiteral("2 fields changed")), qPrintable(said));
        QVERIFY2(said.contains(QStringLiteral("health")) && said.contains(QStringLiteral("ammo")), qPrintable(said));

        const tl::RecordId before2 = lastRecord();
        {
            QMutexLocker lock(&m_prov->mutex);
            const uint64_t blob = 0x1122334455667788ull;
            std::memcpy(m_prov->mem.data() + ScriptedLiveProvider::kBaseA + 8, &blob, 8);
        }
        QVERIFY(waitForRecordAfter(before2));
        QTest::qWait(30);
        QCOMPARE(series.countOf(series.lastRecord()), 1);     // eight bytes, one field
    }

    // With a field selected, previous / next change walks THAT field's
    // changes and skips the rest of the class.
    void steppingFollowsTheSelectedField() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        QTest::qWait(60);
        auto setAmmo = [&](uint32_t v) {
            const tl::RecordId before = lastRecord();
            m_prov->poke32(m_doc->tree.baseAddress + 4, v);
            if (!waitForRecordAfter(before)) return tl::kNoRecord;
            QTest::qWait(30);
            return lastRecord();
        };
        const auto h1 = setHealth(1);
        const tl::RecordId a1 = setAmmo(5);
        const auto h2 = setHealth(2);
        const tl::RecordId a2 = setAmmo(6);
        const auto h3 = setHealth(3);
        QVERIFY(h1.first != tl::kNoRecord && a1 != tl::kNoRecord && h2.first != tl::kNoRecord
                && a2 != tl::kNoRecord && h3.first != tl::kNoRecord);

        uint64_t ammoId = 0;
        for (const Node& n : m_doc->tree.nodes)
            if (n.name == QStringLiteral("ammo")) ammoId = n.id;
        m_ctrl->setSelectedIdsForTest({ammoId});
        QCOMPARE(m_ctrl->timelineScopeLabel(), QStringLiteral("'ammo'"));

        QVERIFY(m_ctrl->stepTimelineChange(-1));
        QCOMPARE(m_ctrl->pastRecord(), a2);
        QVERIFY(m_ctrl->stepTimelineChange(-1));
        QCOMPARE(m_ctrl->pastRecord(), a1);
        QVERIFY(m_ctrl->stepTimelineChange(+1));
        QCOMPARE(m_ctrl->pastRecord(), a2);
        QVERIFY(m_ctrl->stepTimelineChange(+1));       // no later ammo change: live again
        QVERIFY(!m_ctrl->isViewingPast());

        m_ctrl->setSelectedIdsForTest({});
        QVERIFY(m_ctrl->stepTimelineChange(-1));
        QCOMPARE(m_ctrl->pastRecord(), h3.first);
    }

    void showInTimelineFramesTheFieldsChanges() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        QTest::qWait(60);
        const auto h1 = setHealth(7);
        const auto h2 = setHealth(8);
        QVERIFY(h1.first != tl::kNoRecord && h2.first != tl::kNoRecord);
        uint64_t healthId = 0;
        for (const Node& n : m_doc->tree.nodes)
            if (n.name == QStringLiteral("health")) healthId = n.id;
        m_ctrl->setSelectedIdsForTest({healthId});
        QSignalSpy spy(m_ctrl, &RcxController::timelineRevealRequested);
        m_ctrl->revealSelectionInTimeline();
        QCOMPARE(spy.count(), 1);
        const tl::TimelineModel& m = m_ctrl->timelineContext()->model();
        QVERIFY(spy[0][0].toLongLong() <= m.timeOf(h1.first));
        QVERIFY(spy[0][1].toLongLong() >= m.timeOf(h2.first));
    }

    // Detaching drops the capture binding; nothing dangles.
    void detachingStopsCapture() {
        setup();
        QVERIFY(waitForRecordAfter(tl::kNoRecord));
        m_ctrl->resetProvider();
        QVERIFY(m_ctrl->timelineContext() == nullptr);
        QVERIFY(!m_ctrl->isViewingPast());
        QTest::qWait(60);
    }
};

QTEST_MAIN(TestTimelineCapture)
#include "test_timeline_capture.moc"
