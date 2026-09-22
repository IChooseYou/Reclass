#pragma once

// ── A live example to open on: a bouncing ball ──
//
// The first class a new user sees should be doing something, so the timeline,
// the change highlights and the value types have something to show. This is a
// tiny physics simulation living in a buffer inside RC itself (the "New
// Class" self-attach): a ball dropped into a box bounces, rolls, comes to
// rest and starts over, while a camera orbits it. Each step reads its state
// back from the buffer first, so editing a value in the view — gravity, the
// bounciness, the ball's velocity — changes what the simulation does next.
//
// Layout (little-endian, offsets from the buffer start):
//   0x000  BouncingBall  sim     the class that opens
//   0x100  World                 where sim.world points
//   sim.state is a BallState

#include "core.h"

#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rcx::demo {

enum class BallState : uint32_t { Dropping = 0, Bouncing = 1, Rolling = 2, Resting = 3, Restarting = 4 };

// BouncingBall
namespace ball {
inline constexpr int state = 0x00, bounces = 0x04, time = 0x08, position = 0x10, velocity = 0x1C,
                     radius = 0x28, restitution = 0x2C, transform = 0x30, color = 0x70,
                     restarts = 0x80, squash = 0x84, world = 0x88, restartIn = 0x90, size = 0x98;
}
// World, at kWorldOffset
namespace world {
inline constexpr int gravity = 0x00, floorY = 0x0C, halfWidth = 0x10, frame = 0x14, dt = 0x18,
                     view = 0x20, size = 0x60;
}
inline constexpr int kWorldOffset = 0x100;
inline constexpr int kBufferMin = kWorldOffset + world::size;

struct BuiltDemo {
    uint64_t ballClassId = 0;
    uint64_t worldClassId = 0;
    uint64_t stateEnumId = 0;
};

// The classes that describe the buffer. Adds three root nodes to `tree`:
// the BallState enum, the World struct and the BouncingBall class.
inline BuiltDemo buildBouncingBall(NodeTree& tree) {
    BuiltDemo out;
    auto addRoot = [&](const QString& name, const QString& type, const QString& keyword) {
        Node n;
        n.kind = NodeKind::Struct;
        n.name = name;
        n.structTypeName = type;
        n.classKeyword = keyword;
        n.collapsed = false;
        return tree.nodes[tree.addNode(n)].id;
    };
    auto add = [&](uint64_t parent, int offset, NodeKind kind, const char* name,
                   uint64_t refId = 0, bool collapsed = true) {
        Node n;
        n.kind = kind;
        n.name = QString::fromLatin1(name);
        n.parentId = parent;
        n.offset = offset;
        n.refId = refId;
        n.collapsed = collapsed;
        tree.addNode(n);
    };

    {
        Node e;
        e.kind = NodeKind::Struct;
        e.classKeyword = QStringLiteral("enum");
        e.name = QStringLiteral("BallState");
        e.structTypeName = QStringLiteral("BallState");
        e.enumMembers = {
            {QStringLiteral("Dropping"), 0}, {QStringLiteral("Bouncing"), 1},
            {QStringLiteral("Rolling"), 2},  {QStringLiteral("Resting"), 3},
            {QStringLiteral("Restarting"), 4},
        };
        out.stateEnumId = tree.nodes[tree.addNode(e)].id;
    }

    out.worldClassId = addRoot(QStringLiteral("World"), QStringLiteral("World"), QStringLiteral("struct"));
    add(out.worldClassId, world::gravity,   NodeKind::Vec3,   "gravity");
    add(out.worldClassId, world::floorY,    NodeKind::Float,  "floorY");
    add(out.worldClassId, world::halfWidth, NodeKind::Float,  "halfWidth");
    add(out.worldClassId, world::frame,     NodeKind::UInt32, "frame");
    add(out.worldClassId, world::dt,        NodeKind::Double, "deltaTime");
    add(out.worldClassId, world::view,      NodeKind::Mat4x4, "cameraView");

    out.ballClassId = addRoot(QStringLiteral("sim"), QStringLiteral("BouncingBall"), QStringLiteral("class"));
    const uint64_t b = out.ballClassId;
    add(b, ball::state,       NodeKind::UInt32,    "state", out.stateEnumId);
    add(b, ball::bounces,     NodeKind::UInt32,    "bounces");
    add(b, ball::time,        NodeKind::Double,    "time");
    add(b, ball::position,    NodeKind::Vec3,      "position");
    add(b, ball::velocity,    NodeKind::Vec3,      "velocity");
    add(b, ball::radius,      NodeKind::Float,     "radius");
    add(b, ball::restitution, NodeKind::Float,     "restitution");
    add(b, ball::transform,   NodeKind::Mat4x4,    "transform");
    add(b, ball::color,       NodeKind::Vec4,      "color");
    add(b, ball::restarts,    NodeKind::UInt32,    "restarts");
    add(b, ball::squash,      NodeKind::Float,     "squash");
    add(b, ball::world,       NodeKind::Pointer64, "world", out.worldClassId, /*collapsed=*/false);
    add(b, ball::restartIn,   NodeKind::Float,     "restartIn");
    add(b, ball::restartIn + 4, NodeKind::Hex32,   "");
    return out;
}

class BouncingBallSim {
public:
    static constexpr double kStepSeconds = 1.0 / 30.0;

    BouncingBallSim(uint8_t* buffer, size_t size) : m_buf(buffer), m_size(size) {}

    bool valid() const { return m_buf && m_size >= size_t(kBufferMin); }

    // Start from scratch: the world, then a fresh drop.
    void reset() {
        if (!valid()) return;
        std::memset(m_buf, 0, size_t(kBufferMin));
        const uint8_t* worldAddr = m_buf + kWorldOffset;
        putU64(ball::world, uint64_t(quintptr(worldAddr)));
        putV3(kWorldOffset + world::gravity, {0.0f, -9.81f, 0.0f});
        putF(kWorldOffset + world::floorY, 0.0f);
        putF(kWorldOffset + world::halfWidth, 3.0f);
        putD(kWorldOffset + world::dt, kStepSeconds);
        putF(ball::radius, 0.35f);
        putF(ball::restitution, 0.72f);
        drop(0);
        writeMatrices(0.0);
    }

    // One step of kStepSeconds. Reads everything back from the buffer first,
    // so edits made in the view take effect.
    void step() {
        if (!valid()) return;
        // deltaTime is a field like the others: 0 pauses the simulation, a
        // larger value runs it faster (capped so it stays stable).
        double dt = getD(kWorldOffset + world::dt);
        if (!std::isfinite(dt) || dt < 0.0) {
            dt = kStepSeconds;
            putD(kWorldOffset + world::dt, dt);
        }
        dt = std::min(dt, 0.1);
        V3 p = getV3(ball::position), v = getV3(ball::velocity);
        const V3 g = getV3(kWorldOffset + world::gravity);
        float r = getF(ball::radius), e = getF(ball::restitution);
        const float floorY = getF(kWorldOffset + world::floorY);
        const float half = std::max(0.5f, getF(kWorldOffset + world::halfWidth));
        auto state = BallState(getU32(ball::state));
        uint32_t bounces = getU32(ball::bounces);
        float squash = getF(ball::squash), restartIn = getF(ball::restartIn);
        uint32_t restarts = getU32(ball::restarts);

        // Nonsense typed into the view (or a stray write): start over, don't explode.
        if (!finite(p) || !finite(v) || !finite(g) || !std::isfinite(squash) || !std::isfinite(restartIn)
            || !std::isfinite(floorY) || uint32_t(state) > uint32_t(BallState::Restarting)) {
            reset();
            return;
        }
        // Out-of-range values are clamped AND written back, so the view shows
        // what the physics actually uses.
        r = std::clamp(std::isfinite(r) ? r : 0.35f, 0.05f, half * 0.5f);
        e = std::clamp(std::isfinite(e) ? e : 0.72f, 0.0f, 0.98f);
        putF(ball::radius, r);
        putF(ball::restitution, e);
        restartIn = std::clamp(restartIn, 0.0f, 2.0f);

        if (state == BallState::Resting || state == BallState::Restarting) {
            restartIn -= float(dt);
            if (restartIn <= 0.0f) {
                putU32(ball::restarts, restarts + 1);
                drop(restarts + 1);
                advanceClock(dt);
                return;
            }
            if (restartIn < 0.8f) state = BallState::Restarting;
        } else {
            v = add(v, mul(g, float(dt)));
            p = add(p, mul(v, float(dt)));
            const float impact = -v.y;
            if (p.y - r < floorY) {
                p.y = floorY + r;
                if (impact > 0.9f && bounces < 40) {
                    v.y = impact * e;
                    ++bounces;
                    state = BallState::Bouncing;
                    squash = std::clamp(1.0f - impact * 0.05f, 0.55f, 1.0f);
                    putV4(ball::color, hueColor(float(bounces) * 0.11f + float(restarts) * 0.37f));
                } else {
                    v.y = 0.0f;
                    state = BallState::Rolling;
                    const float friction = std::max(0.0f, 1.0f - 1.6f * float(dt));
                    v.x *= friction;
                    v.z *= friction;
                }
            }
            for (float V3::*axis : {&V3::x, &V3::z}) {
                if (std::abs(p.*axis) + r > half) {
                    p.*axis = std::copysign(half - r, p.*axis);
                    v.*axis = -(v.*axis) * 0.9f;   // .* binds looser than unary minus
                }
            }
            // Somewhere it can never come back from (gravity typed upward, the
            // floor moved far away): start over rather than drift forever.
            if (std::abs(p.y - floorY) > 100.0f) {
                state = BallState::Restarting;
                restartIn = 0.8f;
            }
            if (state == BallState::Rolling && std::hypot(v.x, v.z) < 0.08f) {
                state = BallState::Resting;
                restartIn = 2.0f;
            }
        }
        squash += (1.0f - squash) * std::min(1.0f, 8.0f * float(dt));

        putV3(ball::position, p);
        putV3(ball::velocity, v);
        putU32(ball::state, uint32_t(state));
        putU32(ball::bounces, bounces);
        putF(ball::squash, squash);
        putF(ball::restartIn, std::max(0.0f, restartIn));
        advanceClock(dt);
    }

    BallState state() const { return BallState(getU32(ball::state)); }
    uint32_t bounces() const { return getU32(ball::bounces); }
    uint32_t restarts() const { return getU32(ball::restarts); }
    float y() const { return getV3(ball::position).y; }
    float x() const { return getV3(ball::position).x; }
    float z() const { return getV3(ball::position).z; }

private:
    struct V3 { float x = 0, y = 0, z = 0; };
    struct V4 { float x = 0, y = 0, z = 0, w = 0; };
    static V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    static V3 mul(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
    static bool finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

    // A new drop, varied by how many came before (no randomness: the demo
    // should look the same every run).
    void drop(uint32_t n) {
        const float half = std::max(0.5f, getF(kWorldOffset + world::halfWidth));
        const float r = std::max(0.05f, getF(ball::radius));
        const float span = std::max(0.1f, half - r - 0.05f);   // inside the walls
        putV3(ball::position, {std::fmod(float(n) * 1.37f, span * 2.0f) - span, 4.5f,
                               std::fmod(float(n) * 0.71f, span) - span * 0.5f});
        putV3(ball::velocity, {(n % 2 ? -1.0f : 1.0f) * (1.2f + float(n % 3) * 0.4f), 0.5f,
                               (n % 3 == 1 ? -0.8f : 0.6f)});
        putU32(ball::state, uint32_t(BallState::Dropping));
        putU32(ball::bounces, 0);
        putF(ball::squash, 1.0f);
        putF(ball::restartIn, 0.0f);
        putV4(ball::color, hueColor(float(n) * 0.37f));
    }

    void advanceClock(double dt) {
        const double raw = getD(ball::time) + dt;
        const double t = std::isfinite(raw) ? raw : 0.0;   // a typed NaN / inf never reaches the matrices
        putD(ball::time, t);
        putU32(kWorldOffset + world::frame, getU32(kWorldOffset + world::frame) + 1);
        writeMatrices(t);
    }

    // transform: the ball's world matrix (row-major, DirectX-style row
    // vectors: rows 0-2 the scaled rotation, row 3 the translation), rolling
    // about z as it moves and squashed on impact. cameraView: a look-at
    // matrix for a camera orbiting the box.
    void writeMatrices(double t) {
        const V3 p = getV3(ball::position);
        const float r = std::max(0.05f, getF(ball::radius));
        const float sq = getF(ball::squash) > 0 ? getF(ball::squash) : 1.0f;
        const float angle = -p.x / r;
        const float c = std::cos(angle), s = std::sin(angle);
        const float m[16] = {
            c * r,        s * r,         0.0f, 0.0f,
            -s * r * sq,  c * r * sq,    0.0f, 0.0f,
            0.0f,         0.0f,          r,    0.0f,
            p.x,          p.y,           p.z,  1.0f,
        };
        std::memcpy(m_buf + ball::transform, m, sizeof m);

        const float orbit = float(t) * 0.25f;
        const V3 eye{9.0f * std::sin(orbit), 4.0f, -9.0f * std::cos(orbit)};
        const V3 target{0.0f, 1.0f, 0.0f};
        auto sub = [](V3 a, V3 b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; };
        auto cross = [](V3 a, V3 b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
        auto dot = [](V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        auto norm = [&](V3 a) { const float l = std::sqrt(dot(a, a)); return l > 0 ? mul(a, 1.0f / l) : a; };
        const V3 zAxis = norm(sub(target, eye));
        const V3 xAxis = norm(cross(V3{0, 1, 0}, zAxis));
        const V3 yAxis = cross(zAxis, xAxis);
        const float view[16] = {
            xAxis.x, yAxis.x, zAxis.x, 0.0f,
            xAxis.y, yAxis.y, zAxis.y, 0.0f,
            xAxis.z, yAxis.z, zAxis.z, 0.0f,
            -dot(xAxis, eye), -dot(yAxis, eye), -dot(zAxis, eye), 1.0f,
        };
        std::memcpy(m_buf + kWorldOffset + world::view, view, sizeof view);
    }

    static V4 hueColor(float h) {
        h = h - std::floor(h);
        auto channel = [h](float offset) {
            const float k = std::fmod(h * 6.0f + offset, 6.0f);
            return 1.0f - std::max(0.0f, std::min({k, 4.0f - k, 1.0f}));
        };
        return {channel(5.0f), channel(3.0f), channel(1.0f), 1.0f};
    }

    float    getF(int o) const   { float v;    std::memcpy(&v, m_buf + o, 4); return v; }
    double   getD(int o) const   { double v;   std::memcpy(&v, m_buf + o, 8); return v; }
    uint32_t getU32(int o) const { uint32_t v; std::memcpy(&v, m_buf + o, 4); return v; }
    V3       getV3(int o) const  { V3 v;       std::memcpy(&v, m_buf + o, 12); return v; }
    void putF(int o, float v)     { std::memcpy(m_buf + o, &v, 4); }
    void putD(int o, double v)    { std::memcpy(m_buf + o, &v, 8); }
    void putU32(int o, uint32_t v){ std::memcpy(m_buf + o, &v, 4); }
    void putU64(int o, uint64_t v){ std::memcpy(m_buf + o, &v, 8); }
    void putV3(int o, V3 v)       { std::memcpy(m_buf + o, &v, 12); }
    void putV4(int o, V4 v)       { std::memcpy(m_buf + o, &v, 16); }

    uint8_t* m_buf = nullptr;
    size_t   m_size = 0;
};

} // namespace rcx::demo
