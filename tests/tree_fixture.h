#pragma once

// Shared fixtures for the drawn tree lines (test_tree_guides,
// test_tree_guides_render): trees that exercise every kind of row compose can
// hang off a parent — nested and embedded structs, a matrix in the middle and
// at the end, enum and bitfield members, primitive, struct and materialised
// arrays, expanded, materialised and collapsed pointers, code, empty and
// collapsed structs — plus the two-root shape and deep nesting.

#include "core.h"
#include "providers/buffer_provider.h"

#include <QByteArray>
#include <QString>
#include <cstring>

namespace treefix {

using namespace rcx;

inline uint64_t add(NodeTree& t, NodeKind kind, const QString& name, uint64_t parent,
                    int offset, bool collapsed = false) {
    Node n;
    n.kind = kind;
    n.name = name;
    n.parentId = parent;
    n.offset = offset;
    n.collapsed = collapsed;
    return t.nodes[t.addNode(n)].id;
}

inline Node& node(NodeTree& t, uint64_t id) { return t.nodes[t.indexOfId(id)]; }

struct Rich {
    NodeTree tree;
    QByteArray data;
    uint64_t rootId = 0;
};

// Every child kind in one class. Offsets are real so values decode; the
// pointers aim at 0x200, inside the buffer.
inline Rich richTree(bool lastChildIsMatrix = true) {
    Rich r;
    NodeTree& t = r.tree;
    t.baseAddress = 0;

    // A small class used by reference (struct array, embedded, pointers).
    Node vec;
    vec.kind = NodeKind::Struct;
    vec.structTypeName = QStringLiteral("Vec");
    vec.name = QStringLiteral("Vec");
    vec.parentId = 0;
    vec.collapsed = false;
    const uint64_t vecId = t.nodes[t.addNode(vec)].id;
    add(t, NodeKind::Float, QStringLiteral("x"), vecId, 0);
    add(t, NodeKind::Float, QStringLiteral("y"), vecId, 4);

    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = QStringLiteral("Rich");
    root.name = QStringLiteral("rich");
    root.parentId = 0;
    root.collapsed = false;
    const uint64_t rootId = t.nodes[t.addNode(root)].id;
    r.rootId = rootId;

    int off = 0;
    add(t, NodeKind::Int32, QStringLiteral("health"), rootId, off); off += 4;
    add(t, NodeKind::Mat4x4, QStringLiteral("view"), rootId, off); off += 64;   // middle matrix

    const uint64_t inner = add(t, NodeKind::Struct, QStringLiteral("inner"), rootId, off);
    node(t, inner).structTypeName = QStringLiteral("Inner");
    add(t, NodeKind::Float, QStringLiteral("a"), inner, 0);
    const uint64_t deeper = add(t, NodeKind::Struct, QStringLiteral("deeper"), inner, 4);
    node(t, deeper).structTypeName = QStringLiteral("Deeper");
    add(t, NodeKind::UInt16, QStringLiteral("p"), deeper, 0);
    add(t, NodeKind::UInt16, QStringLiteral("q"), deeper, 2);
    add(t, NodeKind::Float, QStringLiteral("b"), inner, 8);
    off += 12;

    const uint64_t en = add(t, NodeKind::Struct, QStringLiteral("state"), rootId, off);
    node(t, en).classKeyword = QStringLiteral("enum");
    node(t, en).structTypeName = QStringLiteral("State");
    node(t, en).elementKind = NodeKind::UInt32;
    node(t, en).enumMembers = {{QStringLiteral("Idle"), 0}, {QStringLiteral("Run"), 1},
                               {QStringLiteral("Dead"), 2}};
    off += 4;

    // An int field typed as that enum, open: its members hang off it, one marked.
    const uint64_t mode = add(t, NodeKind::UInt32, QStringLiteral("mode"), rootId, off);
    node(t, mode).refId = en;
    const int modeOff = off;
    off += 4;

    const uint64_t bf = add(t, NodeKind::Struct, QStringLiteral("flags"), rootId, off);
    node(t, bf).classKeyword = QStringLiteral("bitfield");
    node(t, bf).elementKind = NodeKind::Hex32;
    node(t, bf).bitfieldMembers = {{QStringLiteral("active"), 0, 1},
                                   {QStringLiteral("level"), 1, 3},
                                   {QStringLiteral("mode"), 4, 4}};
    off += 4;

    const uint64_t prim = add(t, NodeKind::Array, QStringLiteral("scores"), rootId, off);
    node(t, prim).elementKind = NodeKind::UInt32;
    node(t, prim).arrayLen = 3;
    off += 12;

    const uint64_t sarr = add(t, NodeKind::Array, QStringLiteral("points"), rootId, off);
    node(t, sarr).elementKind = NodeKind::Struct;
    node(t, sarr).refId = vecId;
    node(t, sarr).arrayLen = 2;
    off += 16;

    const uint64_t marr = add(t, NodeKind::Array, QStringLiteral("pairs"), rootId, off);
    node(t, marr).elementKind = NodeKind::Struct;
    node(t, marr).arrayLen = 2;
    for (int i = 0; i < 2; ++i) {
        const uint64_t el = add(t, NodeKind::Struct, QStringLiteral("pair%1").arg(i), marr, i * 8);
        add(t, NodeKind::Int32, QStringLiteral("k"), el, 0);
        add(t, NodeKind::Int32, QStringLiteral("v"), el, 4);
    }
    off += 16;

    const uint64_t emb = add(t, NodeKind::Struct, QStringLiteral("pos"), rootId, off);
    node(t, emb).refId = vecId;
    off += 8;

    const uint64_t ptrOff = off;
    const uint64_t ptr = add(t, NodeKind::Pointer64, QStringLiteral("target"), rootId, off);
    node(t, ptr).refId = vecId;
    off += 8;

    const uint64_t mptrOff = off;
    const uint64_t mptr = add(t, NodeKind::Pointer64, QStringLiteral("owned"), rootId, off);
    add(t, NodeKind::Int32, QStringLiteral("id"), mptr, 0);
    add(t, NodeKind::Int32, QStringLiteral("count"), mptr, 4);
    off += 8;

    const uint64_t cptr = add(t, NodeKind::Pointer64, QStringLiteral("folded"), rootId, off,
                              /*collapsed=*/true);
    node(t, cptr).refId = vecId;
    off += 8;

    const uint64_t selfPtrOff = off;
    const uint64_t self = add(t, NodeKind::Pointer64, QStringLiteral("self"), rootId, off);
    node(t, self).refId = rootId;
    off += 8;

    const uint64_t code = add(t, NodeKind::Asm, QStringLiteral("hook"), rootId, off);
    node(t, code).arrayLen = 8;
    const int codeOff = off;
    off += 8;

    const uint64_t shut = add(t, NodeKind::Struct, QStringLiteral("closed"), rootId, off,
                              /*collapsed=*/true);
    add(t, NodeKind::Int32, QStringLiteral("hidden"), shut, 0);
    off += 4;

    add(t, NodeKind::Struct, QStringLiteral("empty"), rootId, off);

    // An anonymous struct whose type has a space and runs past the type column.
    const uint64_t anon = add(t, NodeKind::Struct, QString(), rootId, off);
    node(t, anon).structTypeName = QStringLiteral("std::pair<int, float>");
    add(t, NodeKind::Int32, QStringLiteral("first"), anon, 0);
    off += 4;

    add(t, NodeKind::Int8, QStringLiteral("tail"), rootId, off);
    off += 1;
    if (lastChildIsMatrix)
        add(t, NodeKind::Mat4x4, QStringLiteral("world"), rootId, off);   // last matrix

    r.data = QByteArray(0x400, '\0');
    auto w64 = [&](uint64_t at, quint64 v) { memcpy(r.data.data() + at, &v, 8); };
    w64(ptrOff, 0x200);
    w64(mptrOff, 0x210);
    w64(selfPtrOff, 0x220);
    const quint32 running = 1;   // "Run"
    memcpy(r.data.data() + modeOff, &running, 4);
    static const unsigned char kCode[] = {0x55, 0x48, 0x89, 0xE5, 0x5D, 0xC3, 0xCC, 0xCC};
    memcpy(r.data.data() + codeOff, kCode, sizeof(kCode));
    for (int i = 0; i < 16; ++i) {
        const float one = (i % 5 == 0) ? 1.0f : 0.0f;
        memcpy(r.data.data() + 4 + i * 4, &one, 4);
    }
    return r;
}

// The shape of testTreeLinesDepth2: two roots (viewRoot 0), an expanded
// pointer to the second one.
inline Rich twoRoots() {
    Rich r;
    NodeTree& t = r.tree;
    t.baseAddress = 0;
    Node root;
    root.kind = NodeKind::Struct;
    root.name = QStringLiteral("Unnamed");
    root.parentId = 0;
    root.collapsed = false;
    const uint64_t rootId = t.nodes[t.addNode(root)].id;
    r.rootId = rootId;
    add(t, NodeKind::Hex64, QString(), rootId, 0);

    Node inner;
    inner.kind = NodeKind::Struct;
    inner.name = QStringLiteral("NewClass");
    inner.parentId = 0;
    inner.collapsed = false;
    inner.offset = 200;
    const uint64_t innerId = t.nodes[t.addNode(inner)].id;
    add(t, NodeKind::Hex64, QString(), innerId, 0);
    add(t, NodeKind::Hex64, QString(), innerId, 8);
    add(t, NodeKind::Hex64, QString(), innerId, 16);

    const uint64_t ptr = add(t, NodeKind::Pointer64, QStringLiteral("field_0008"), rootId, 8);
    node(t, ptr).refId = innerId;
    add(t, NodeKind::Hex64, QString(), rootId, 16);

    r.data = QByteArray(256, '\0');
    const quint64 ptrVal = 100;
    memcpy(r.data.data() + 8, &ptrVal, 8);
    return r;
}

// `levels` nested structs, two fields at each level.
inline Rich deepTree(int levels) {
    Rich r;
    NodeTree& t = r.tree;
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = QStringLiteral("Deep");
    root.name = QStringLiteral("deep");
    root.parentId = 0;
    root.collapsed = false;
    uint64_t parent = t.nodes[t.addNode(root)].id;
    r.rootId = parent;
    for (int l = 0; l < levels; ++l) {
        add(t, NodeKind::Int32, QStringLiteral("a%1").arg(l), parent, 0);
        const uint64_t next = add(t, NodeKind::Struct, QStringLiteral("s%1").arg(l), parent, 4);
        add(t, NodeKind::Int32, QStringLiteral("b%1").arg(l), parent, 4 + 4 * (levels - l));
        parent = next;
    }
    add(t, NodeKind::Int32, QStringLiteral("leaf"), parent, 0);
    r.data = QByteArray(0x200, '\0');
    return r;
}

// Types longer than compact mode's 20-character type column: a collapsed
// pointer, an embedded struct header and a scalar field beside them, so the
// overflowing rows move their column cells while their neighbours don't.
inline Rich overflowTree() {
    Rich r;
    NodeTree& t = r.tree;
    t.baseAddress = 0;
    Node target;
    target.kind = NodeKind::Struct;
    target.structTypeName = QStringLiteral("AnExceptionallyLongStructureName");
    target.name = target.structTypeName;
    target.parentId = 0;
    target.collapsed = false;
    const uint64_t targetId = t.nodes[t.addNode(target)].id;
    add(t, NodeKind::Int32, QStringLiteral("inside"), targetId, 0);

    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = QStringLiteral("Overflow");
    root.name = QStringLiteral("overflow");
    root.parentId = 0;
    root.collapsed = false;
    const uint64_t rootId = t.nodes[t.addNode(root)].id;
    r.rootId = rootId;
    add(t, NodeKind::Int32, QStringLiteral("before"), rootId, 0);
    // A short name and a comment chip after the value: an unpadded name would
    // leave the value short of its column and a blank cell inside the chip gap.
    const uint64_t far = add(t, NodeKind::Pointer64, QStringLiteral("p"), rootId, 4, /*collapsed=*/true);
    node(t, far).refId = targetId;
    node(t, far).comment = QStringLiteral("hi there");
    const uint64_t emb = add(t, NodeKind::Struct, QStringLiteral("embedded"), rootId, 12);
    node(t, emb).structTypeName = QStringLiteral("AnotherRidiculouslyLongTypeName");
    add(t, NodeKind::Int32, QStringLiteral("x"), emb, 0);
    add(t, NodeKind::Float, QStringLiteral("after"), rootId, 16);
    r.data = QByteArray(0x100, '\0');
    const quint64 far64 = 0x80;
    memcpy(r.data.data() + 4, &far64, 8);
    return r;
}

// Code whose bytes can't be read: the one "(unreadable)" member row.
inline Rich unreadableCode() {
    Rich r;
    NodeTree& t = r.tree;
    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = QStringLiteral("Stub");
    root.name = QStringLiteral("stub");
    root.parentId = 0;
    root.collapsed = false;
    const uint64_t rootId = t.nodes[t.addNode(root)].id;
    r.rootId = rootId;
    add(t, NodeKind::Int32, QStringLiteral("before"), rootId, 0);
    const uint64_t code = add(t, NodeKind::Asm, QStringLiteral("gone"), rootId, 0x100);
    node(t, code).arrayLen = 16;
    add(t, NodeKind::Int32, QStringLiteral("after"), rootId, 0x110);
    r.data = QByteArray(8, '\0');   // everything past 8 bytes is unreadable
    return r;
}

} // namespace treefix
