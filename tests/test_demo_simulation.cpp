// The bouncing-ball example a first launch opens on: its classes describe
// its buffer exactly, and the simulation does what the demo promises —
// bounces, rolls, rests and starts over, forever, without leaving the box,
// and takes values typed into the view.

#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "demo_simulation.h"

using namespace rcx;
using namespace rcx::demo;

class TestDemoSimulation : public QObject {
    Q_OBJECT
private slots:
    void classesDescribeTheBuffer() {
        NodeTree tree;
        const BuiltDemo d = buildBouncingBall(tree);
        QVERIFY(d.ballClassId && d.worldClassId && d.stateEnumId);
        for (uint64_t cls : {d.ballClassId, d.worldClassId}) {
            QVector<QPair<int, int>> spans;
            for (const Node& n : tree.nodes)
                if (n.parentId == cls) spans.append({n.offset, n.byteSize()});
            QVERIFY(spans.size() >= 6);
            std::sort(spans.begin(), spans.end());
            for (int i = 1; i < spans.size(); ++i)
                QVERIFY2(spans[i].first >= spans[i - 1].first + spans[i - 1].second,
                         qPrintable(QStringLiteral("fields overlap at 0x%1").arg(spans[i].first, 0, 16)));
        }
        QCOMPARE(tree.structSpan(d.ballClassId), ball::size);
        QCOMPARE(tree.structSpan(d.worldClassId), world::size);
        QVERIFY(kWorldOffset >= ball::size);

        bool stateTyped = false, worldPointer = false;
        for (const Node& n : tree.nodes) {
            if (n.parentId != d.ballClassId) continue;
            if (n.name == QStringLiteral("state")) stateTyped = n.refId == d.stateEnumId;
            if (n.name == QStringLiteral("world"))
                worldPointer = n.kind == NodeKind::Pointer64 && n.refId == d.worldClassId && !n.collapsed;
        }
        QVERIFY2(stateTyped, "state is not typed as BallState");
        QVERIFY2(worldPointer, "world is not an open pointer to World");
    }

    void theBallBouncesRestsAndStartsOver() {
        std::vector<uint8_t> buf(size_t(kBufferMin) + 64);
        BouncingBallSim sim(buf.data(), buf.size());
        sim.reset();
        QSet<int> seen;
        uint32_t maxBounces = 0;
        for (int i = 0; i < 30 * 120 && sim.restarts() < 2; ++i) {
            sim.step();
            seen.insert(int(sim.state()));
            maxBounces = std::max(maxBounces, sim.bounces());
            QVERIFY(std::isfinite(sim.x()) && std::isfinite(sim.y()) && std::isfinite(sim.z()));
            QVERIFY2(sim.y() >= -1e-3f, qPrintable(QString::number(sim.y())));
            QVERIFY2(std::abs(sim.x()) <= 3.0f + 1e-3f, qPrintable(QString::number(sim.x())));
            QVERIFY2(std::abs(sim.z()) <= 3.0f + 1e-3f, qPrintable(QString::number(sim.z())));
        }
        QVERIFY2(sim.restarts() >= 2, "the simulation never started over");
        QVERIFY(maxBounces >= 3);
        for (BallState s : {BallState::Dropping, BallState::Bouncing, BallState::Rolling,
                            BallState::Resting, BallState::Restarting})
            QVERIFY2(seen.contains(int(s)), qPrintable(QStringLiteral("never in state %1").arg(int(s))));
    }

    // Values typed into the view are what the next step reads.
    void editsFeedBackIntoTheSimulation() {
        std::vector<uint8_t> buf(size_t(kBufferMin) + 64);
        BouncingBallSim sim(buf.data(), buf.size());
        sim.reset();
        const float zeroG[3] = {0.0f, 0.0f, 0.0f};
        std::memcpy(buf.data() + kWorldOffset + world::gravity, zeroG, sizeof zeroG);
        for (int i = 0; i < 30; ++i) sim.step();
        QVERIFY2(sim.y() > 4.5f, "with gravity off the ball should drift up, not fall");

        const float nanVelocity[3] = {std::nanf(""), 0.0f, 0.0f};
        std::memcpy(buf.data() + ball::velocity, nanVelocity, sizeof nanVelocity);
        sim.step();
        QCOMPARE(sim.state(), BallState::Dropping);   // nonsense starts over, never explodes
        QVERIFY(std::isfinite(sim.x()));

        // deltaTime 0 pauses; an out-of-range radius is clamped and shown clamped.
        sim.reset();
        const double zeroDt = 0.0;
        std::memcpy(buf.data() + kWorldOffset + world::dt, &zeroDt, sizeof zeroDt);
        const float yBefore = sim.y();
        for (int i = 0; i < 10; ++i) sim.step();
        QCOMPARE(sim.y(), yBefore);
        const float hugeRadius = 50.0f;
        std::memcpy(buf.data() + ball::radius, &hugeRadius, sizeof hugeRadius);
        sim.step();
        float shown = 0.0f;
        std::memcpy(&shown, buf.data() + ball::radius, sizeof shown);
        QVERIFY2(shown <= 1.5f, qPrintable(QString::number(shown)));

        // Gravity typed upward: the ball leaves, and the simulation starts over.
        sim.reset();
        const float upG[3] = {0.0f, 30.0f, 0.0f};
        std::memcpy(buf.data() + kWorldOffset + world::gravity, upG, sizeof upG);
        uint32_t restartsBefore = sim.restarts();
        for (int i = 0; i < 30 * 20 && sim.restarts() == restartsBefore; ++i) sim.step();
        QVERIFY2(sim.restarts() > restartsBefore, "a ball that flew off never started over");
    }
};

QTEST_MAIN(TestDemoSimulation)
#include "test_demo_simulation.moc"
