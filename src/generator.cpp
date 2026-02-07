#include "generator.h"
#include <QHash>
#include <QVector>
#include <QStringList>
#include <algorithm>

namespace rcx {

namespace {

// ── Identifier sanitisation ──

static QString sanitizeIdent(const QString& name) {
    if (name.isEmpty()) return QStringLiteral("unnamed");
    QString out;
    out.reserve(name.size());
    for (QChar c : name) {
        if (c.isLetterOrNumber() || c == '_') out += c;
        else out += '_';
    }
    if (!out[0].isLetter() && out[0] != '_')
        out.prepend('_');
    return out;
}

// ── C type name for a primitive NodeKind ──

static QString cTypeName(NodeKind kind) {
    switch (kind) {
    case NodeKind::Hex8:      return QStringLiteral("uint8_t");
    case NodeKind::Hex16:     return QStringLiteral("uint16_t");
    case NodeKind::Hex32:     return QStringLiteral("uint32_t");
    case NodeKind::Hex64:     return QStringLiteral("uint64_t");
    case NodeKind::Int8:      return QStringLiteral("int8_t");
    case NodeKind::Int16:     return QStringLiteral("int16_t");
    case NodeKind::Int32:     return QStringLiteral("int32_t");
    case NodeKind::Int64:     return QStringLiteral("int64_t");
    case NodeKind::UInt8:     return QStringLiteral("uint8_t");
    case NodeKind::UInt16:    return QStringLiteral("uint16_t");
    case NodeKind::UInt32:    return QStringLiteral("uint32_t");
    case NodeKind::UInt64:    return QStringLiteral("uint64_t");
    case NodeKind::Float:     return QStringLiteral("float");
    case NodeKind::Double:    return QStringLiteral("double");
    case NodeKind::Bool:      return QStringLiteral("bool");
    case NodeKind::Pointer32: return QStringLiteral("uint32_t");
    case NodeKind::Pointer64: return QStringLiteral("uint64_t");
    case NodeKind::Vec2:      return QStringLiteral("float");
    case NodeKind::Vec3:      return QStringLiteral("float");
    case NodeKind::Vec4:      return QStringLiteral("float");
    case NodeKind::Mat4x4:    return QStringLiteral("float");
    case NodeKind::UTF8:      return QStringLiteral("char");
    case NodeKind::UTF16:     return QStringLiteral("wchar_t");
    case NodeKind::Padding:   return QStringLiteral("uint8_t");
    default:                  return QStringLiteral("uint8_t");
    }
}

// ── Generator context ──

struct GenContext {
    const NodeTree& tree;
    QHash<uint64_t, QVector<int>> childMap;
    QSet<QString>   emittedTypeNames;   // struct type names already emitted
    QSet<uint64_t>  emittedIds;         // struct node IDs already emitted
    QSet<uint64_t>  visiting;           // cycle guard
    QSet<uint64_t>  forwardDeclared;    // forward-declared type IDs
    QString         output;
    int             padCounter = 0;

    QString uniquePadName() {
        return QStringLiteral("_pad%1").arg(padCounter++, 4, 16, QChar('0'));
    }

    // Resolve the canonical type name for a struct/array node
    QString structName(const Node& n) const {
        if (!n.structTypeName.isEmpty()) return sanitizeIdent(n.structTypeName);
        if (!n.name.isEmpty())           return sanitizeIdent(n.name);
        return QStringLiteral("anon_%1").arg(n.id, 0, 16);
    }
};

// Forward declarations
static void emitStruct(GenContext& ctx, uint64_t structId);

// ── Emit a single field declaration ──

static QString emitField(GenContext& ctx, const Node& node) {
    const NodeTree& tree = ctx.tree;
    QString name = sanitizeIdent(node.name.isEmpty()
        ? QStringLiteral("field_%1").arg(node.offset, 2, 16, QChar('0'))
        : node.name);

    switch (node.kind) {
    case NodeKind::Vec2:
        return QStringLiteral("    float %1[2];").arg(name);
    case NodeKind::Vec3:
        return QStringLiteral("    float %1[3];").arg(name);
    case NodeKind::Vec4:
        return QStringLiteral("    float %1[4];").arg(name);
    case NodeKind::Mat4x4:
        return QStringLiteral("    float %1[4][4];").arg(name);
    case NodeKind::UTF8:
        return QStringLiteral("    char %1[%2];").arg(name).arg(node.strLen);
    case NodeKind::UTF16:
        return QStringLiteral("    wchar_t %1[%2];").arg(name).arg(node.strLen);
    case NodeKind::Padding:
        return QStringLiteral("    uint8_t %1[%2];").arg(name).arg(qMax(1, node.arrayLen));
    case NodeKind::Pointer32: {
        if (node.refId != 0) {
            int refIdx = tree.indexOfId(node.refId);
            if (refIdx >= 0) {
                QString target = ctx.structName(tree.nodes[refIdx]);
                return QStringLiteral("    uint32_t %1; // -> %2*").arg(name, target);
            }
        }
        return QStringLiteral("    uint32_t %1;").arg(name);
    }
    case NodeKind::Pointer64: {
        if (node.refId != 0) {
            int refIdx = tree.indexOfId(node.refId);
            if (refIdx >= 0) {
                QString target = ctx.structName(tree.nodes[refIdx]);
                return QStringLiteral("    %1* %2;").arg(target, name);
            }
        }
        return QStringLiteral("    void* %1;").arg(name);
    }
    default:
        return QStringLiteral("    %1 %2;").arg(cTypeName(node.kind), name);
    }
}

// ── Emit struct body (fields + padding) ──

static void emitStructBody(GenContext& ctx, uint64_t structId) {
    const NodeTree& tree = ctx.tree;
    int idx = tree.indexOfId(structId);
    if (idx < 0) return;

    int structSize = tree.structSpan(structId, &ctx.childMap);

    QVector<int> children = ctx.childMap.value(structId);
    std::sort(children.begin(), children.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });

    int cursor = 0;

    for (int ci : children) {
        const Node& child = tree.nodes[ci];
        int childSize;
        if (child.kind == NodeKind::Struct || child.kind == NodeKind::Array)
            childSize = tree.structSpan(child.id, &ctx.childMap);
        else
            childSize = child.byteSize();

        // Gap before this field
        if (child.offset > cursor) {
            int gap = child.offset - cursor;
            ctx.output += QStringLiteral("    uint8_t %1[0x%2];\n")
                .arg(ctx.uniquePadName())
                .arg(QString::number(gap, 16).toUpper());
        } else if (child.offset < cursor) {
            // Overlap
            ctx.output += QStringLiteral("    // WARNING: overlap at offset 0x%1 (previous field ends at 0x%2)\n")
                .arg(QString::number(child.offset, 16).toUpper())
                .arg(QString::number(cursor, 16).toUpper());
        }

        // Emit the field
        if (child.kind == NodeKind::Struct) {
            // Ensure the nested struct type is emitted first
            emitStruct(ctx, child.id);
            QString typeName = ctx.structName(child);
            QString fieldName = sanitizeIdent(child.name);
            ctx.output += QStringLiteral("    %1 %2;\n").arg(typeName, fieldName);
        } else if (child.kind == NodeKind::Array) {
            // Check if array has struct element children
            QVector<int> arrayKids = ctx.childMap.value(child.id);
            bool hasStructChild = false;
            QString elemTypeName;

            for (int ak : arrayKids) {
                if (tree.nodes[ak].kind == NodeKind::Struct) {
                    hasStructChild = true;
                    emitStruct(ctx, tree.nodes[ak].id);
                    elemTypeName = ctx.structName(tree.nodes[ak]);
                    break;
                }
            }

            QString fieldName = sanitizeIdent(child.name);
            if (hasStructChild && !elemTypeName.isEmpty()) {
                ctx.output += QStringLiteral("    %1 %2[%3];\n")
                    .arg(elemTypeName, fieldName).arg(child.arrayLen);
            } else {
                ctx.output += QStringLiteral("    %1 %2[%3];\n")
                    .arg(cTypeName(child.elementKind), fieldName).arg(child.arrayLen);
            }
        } else {
            ctx.output += emitField(ctx, child) + QStringLiteral("\n");
        }

        int childEnd = child.offset + childSize;
        if (childEnd > cursor) cursor = childEnd;
    }

    // Tail padding
    if (cursor < structSize) {
        int gap = structSize - cursor;
        ctx.output += QStringLiteral("    uint8_t %1[0x%2];\n")
            .arg(ctx.uniquePadName())
            .arg(QString::number(gap, 16).toUpper());
    }
}

// ── Emit a complete struct definition ──

static void emitStruct(GenContext& ctx, uint64_t structId) {
    if (ctx.emittedIds.contains(structId)) return;
    if (ctx.visiting.contains(structId)) return; // cycle
    ctx.visiting.insert(structId);

    int idx = ctx.tree.indexOfId(structId);
    if (idx < 0) { ctx.visiting.remove(structId); return; }

    const Node& node = ctx.tree.nodes[idx];
    if (node.kind != NodeKind::Struct && node.kind != NodeKind::Array) {
        ctx.visiting.remove(structId);
        return;
    }

    // For arrays, we don't emit a top-level struct — the array itself
    // is a field inside its parent.  But we do emit struct element types.
    if (node.kind == NodeKind::Array) {
        QVector<int> kids = ctx.childMap.value(structId);
        for (int ki : kids) {
            if (ctx.tree.nodes[ki].kind == NodeKind::Struct)
                emitStruct(ctx, ctx.tree.nodes[ki].id);
        }
        ctx.visiting.remove(structId);
        return;
    }

    // Emit nested struct types first (dependency order)
    QVector<int> children = ctx.childMap.value(structId);
    for (int ci : children) {
        const Node& child = ctx.tree.nodes[ci];
        if (child.kind == NodeKind::Struct)
            emitStruct(ctx, child.id);
        else if (child.kind == NodeKind::Array) {
            QVector<int> arrayKids = ctx.childMap.value(child.id);
            for (int ak : arrayKids) {
                if (ctx.tree.nodes[ak].kind == NodeKind::Struct)
                    emitStruct(ctx, ctx.tree.nodes[ak].id);
            }
        }
        // Forward-declare pointer target types if they're outside this subtree
        if (child.kind == NodeKind::Pointer64 && child.refId != 0) {
            int refIdx = ctx.tree.indexOfId(child.refId);
            if (refIdx >= 0 && !ctx.emittedIds.contains(child.refId)
                && !ctx.forwardDeclared.contains(child.refId)) {
                QString fwdName = ctx.structName(ctx.tree.nodes[refIdx]);
                ctx.output += QStringLiteral("struct %1;\n").arg(fwdName);
                ctx.forwardDeclared.insert(child.refId);
            }
        }
    }

    ctx.emittedIds.insert(structId);

    QString typeName = ctx.structName(node);
    int structSize = ctx.tree.structSpan(structId, &ctx.childMap);

    ctx.output += QStringLiteral("#pragma pack(push, 1)\n");
    ctx.output += QStringLiteral("struct %1 {\n").arg(typeName);

    emitStructBody(ctx, structId);

    ctx.output += QStringLiteral("};\n");
    ctx.output += QStringLiteral("#pragma pack(pop)\n");
    ctx.output += QStringLiteral("static_assert(sizeof(%1) == 0x%2, \"Size mismatch for %1\");\n\n")
        .arg(typeName)
        .arg(QString::number(structSize, 16).toUpper());

    ctx.visiting.remove(structId);
}

// ── Build the child map used by all generators ──

static QHash<uint64_t, QVector<int>> buildChildMap(const NodeTree& tree) {
    QHash<uint64_t, QVector<int>> map;
    for (int i = 0; i < tree.nodes.size(); i++)
        map[tree.nodes[i].parentId].append(i);
    return map;
}

// ── Path breadcrumb for header comment ──

static QString nodePath(const NodeTree& tree, uint64_t nodeId) {
    QStringList parts;
    QSet<uint64_t> seen;
    uint64_t cur = nodeId;
    while (cur != 0 && !seen.contains(cur)) {
        seen.insert(cur);
        int idx = tree.indexOfId(cur);
        if (idx < 0) break;
        const Node& n = tree.nodes[idx];
        parts << (n.name.isEmpty() ? QStringLiteral("<unnamed>") : n.name);
        cur = n.parentId;
    }
    std::reverse(parts.begin(), parts.end());
    return parts.join(QStringLiteral(" > "));
}

} // anonymous namespace

// ── Public API ──

QString renderCpp(const NodeTree& tree, uint64_t rootStructId) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};

    const Node& root = tree.nodes[idx];
    if (root.kind != NodeKind::Struct) return {};

    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, 0};
    int rootSize = tree.structSpan(rootStructId, &ctx.childMap);
    QString typeName = ctx.structName(root);

    ctx.output += QStringLiteral("// Generated by ReclassX\n");
    ctx.output += QStringLiteral("// Rendered from: %1  (id=0x%2, size=0x%3)\n\n")
        .arg(nodePath(tree, rootStructId))
        .arg(QString::number(rootStructId, 16).toUpper())
        .arg(QString::number(rootSize, 16).toUpper());
    ctx.output += QStringLiteral("#pragma once\n");
    ctx.output += QStringLiteral("#include <cstdint>\n\n");

    emitStruct(ctx, rootStructId);

    return ctx.output;
}

QString renderCppAll(const NodeTree& tree) {
    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, 0};

    ctx.output += QStringLiteral("// Generated by ReclassX\n");
    ctx.output += QStringLiteral("// Full SDK export\n\n");
    ctx.output += QStringLiteral("#pragma once\n");
    ctx.output += QStringLiteral("#include <cstdint>\n\n");

    QVector<int> roots = ctx.childMap.value(0);
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });

    for (int ri : roots) {
        if (tree.nodes[ri].kind == NodeKind::Struct)
            emitStruct(ctx, tree.nodes[ri].id);
    }

    return ctx.output;
}

QString renderNull(const NodeTree&, uint64_t) {
    return {};
}

} // namespace rcx
