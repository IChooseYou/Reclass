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
    const QHash<NodeKind, QString>* typeAliases = nullptr;

    QString uniquePadName() {
        return QStringLiteral("_pad%1").arg(padCounter++, 4, 16, QChar('0'));
    }

    // Resolve the C type name for a primitive, consulting aliases first
    QString cType(NodeKind kind) const {
        if (typeAliases) {
            auto it = typeAliases->find(kind);
            if (it != typeAliases->end() && !it.value().isEmpty())
                return it.value();
        }
        return cTypeName(kind);
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

// ── Field line with offset comment (code + marker + comment) ──
// We use a \x01 marker to separate the code part from the offset comment.
// After all output is generated, alignComments() replaces markers with padding.

static const QChar kCommentMarker = QChar(0x01);

static QString offsetComment(int offset) {
    return QString(kCommentMarker) + QStringLiteral("// 0x%1").arg(QString::number(offset, 16).toUpper());
}

static QString emitField(GenContext& ctx, const Node& node) {
    const NodeTree& tree = ctx.tree;
    QString name = sanitizeIdent(node.name.isEmpty()
        ? QStringLiteral("field_%1").arg(node.offset, 2, 16, QChar('0'))
        : node.name);
    QString oc = offsetComment(node.offset);

    switch (node.kind) {
    case NodeKind::Vec2:
        return QStringLiteral("    %1 %2[2];").arg(ctx.cType(NodeKind::Float), name) + oc;
    case NodeKind::Vec3:
        return QStringLiteral("    %1 %2[3];").arg(ctx.cType(NodeKind::Float), name) + oc;
    case NodeKind::Vec4:
        return QStringLiteral("    %1 %2[4];").arg(ctx.cType(NodeKind::Float), name) + oc;
    case NodeKind::Mat4x4:
        return QStringLiteral("    %1 %2[4][4];").arg(ctx.cType(NodeKind::Float), name) + oc;
    case NodeKind::UTF8:
        return QStringLiteral("    %1 %2[%3];").arg(ctx.cType(NodeKind::UTF8), name).arg(node.strLen) + oc;
    case NodeKind::UTF16:
        return QStringLiteral("    %1 %2[%3];").arg(ctx.cType(NodeKind::UTF16), name).arg(node.strLen) + oc;
    case NodeKind::Pointer32: {
        if (node.refId != 0) {
            int refIdx = tree.indexOfId(node.refId);
            if (refIdx >= 0) {
                QString target = ctx.structName(tree.nodes[refIdx]);
                return QStringLiteral("    %1 %2;").arg(ctx.cType(NodeKind::Pointer32), name) +
                    offsetComment(node.offset).replace(QStringLiteral("//"), QStringLiteral("// -> %1*").arg(target));
            }
        }
        return QStringLiteral("    %1 %2;").arg(ctx.cType(NodeKind::Pointer32), name) + oc;
    }
    case NodeKind::Pointer64: {
        if (node.refId != 0) {
            int refIdx = tree.indexOfId(node.refId);
            if (refIdx >= 0) {
                QString target = ctx.structName(tree.nodes[refIdx]);
                return QStringLiteral("    %1* %2;").arg(target, name) + oc;
            }
        }
        return QStringLiteral("    void* %1;").arg(name) + oc;
    }
    default:
        return QStringLiteral("    %1 %2;").arg(ctx.cType(node.kind), name) + oc;
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

    // Helper: emit a padding/hex run as a single collapsed byte array
    auto emitPadRun = [&](int offset, int size) {
        if (size <= 0) return;
        ctx.output += QStringLiteral("    %1 %2[0x%3];%4\n")
            .arg(QStringLiteral("uint8_t"))
            .arg(ctx.uniquePadName())
            .arg(QString::number(size, 16).toUpper())
            .arg(offsetComment(offset));
    };

    int cursor = 0;
    int i = 0;

    while (i < children.size()) {
        const Node& child = tree.nodes[children[i]];
        int childSize;
        if (child.kind == NodeKind::Struct || child.kind == NodeKind::Array)
            childSize = tree.structSpan(child.id, &ctx.childMap);
        else
            childSize = child.byteSize();

        // Gap before this field
        if (child.offset > cursor)
            emitPadRun(cursor, child.offset - cursor);
        else if (child.offset < cursor)
            ctx.output += QStringLiteral("    // WARNING: overlap at offset 0x%1 (previous field ends at 0x%2)\n")
                .arg(QString::number(child.offset, 16).toUpper())
                .arg(QString::number(cursor, 16).toUpper());

        // Collapse consecutive hex nodes into a single padding array
        if (isHexNode(child.kind)) {
            int runStart = child.offset;
            int runEnd = child.offset + childSize;
            int j = i + 1;
            while (j < children.size()) {
                const Node& next = tree.nodes[children[j]];
                if (!isHexNode(next.kind)) break;
                int nextSize = next.byteSize();
                // Allow gaps within the run (they become part of the pad)
                if (next.offset < runEnd) break;  // overlap — stop merging
                runEnd = next.offset + nextSize;
                j++;
            }
            emitPadRun(runStart, runEnd - runStart);
            cursor = runEnd;
            i = j;
            continue;
        }

        // Emit the field
        if (child.kind == NodeKind::Struct) {
            emitStruct(ctx, child.id);
            QString typeName = ctx.structName(child);
            QString fieldName = sanitizeIdent(child.name);
            ctx.output += QStringLiteral("    %1 %2;%3\n").arg(typeName, fieldName, offsetComment(child.offset));
        } else if (child.kind == NodeKind::Array) {
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
                ctx.output += QStringLiteral("    %1 %2[%3];%4\n")
                    .arg(elemTypeName, fieldName).arg(child.arrayLen).arg(offsetComment(child.offset));
            } else {
                ctx.output += QStringLiteral("    %1 %2[%3];%4\n")
                    .arg(ctx.cType(child.elementKind), fieldName).arg(child.arrayLen).arg(offsetComment(child.offset));
            }
        } else {
            ctx.output += emitField(ctx, child) + QStringLiteral("\n");
        }

        int childEnd = child.offset + childSize;
        if (childEnd > cursor) cursor = childEnd;
        i++;
    }

    // Tail padding
    if (cursor < structSize)
        emitPadRun(cursor, structSize - cursor);
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

    // Deduplicate by struct type name (different nodes may share the same type)
    QString typeName = ctx.structName(node);
    if (ctx.emittedTypeNames.contains(typeName)) {
        ctx.emittedIds.insert(structId);
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
                QString fwdKw = ctx.tree.nodes[refIdx].resolvedClassKeyword();
                if (fwdKw == QStringLiteral("enum")) fwdKw = QStringLiteral("struct");
                ctx.output += QStringLiteral("%1 %2;\n").arg(fwdKw, fwdName);
                ctx.forwardDeclared.insert(child.refId);
            }
        }
    }

    ctx.emittedIds.insert(structId);
    ctx.emittedTypeNames.insert(typeName);
    int structSize = ctx.tree.structSpan(structId, &ctx.childMap);

    QString kw = node.resolvedClassKeyword();
    if (kw == QStringLiteral("enum")) kw = QStringLiteral("struct");  // enum is cosmetic
    ctx.output += QStringLiteral("%1 %2 {\n").arg(kw, typeName);

    emitStructBody(ctx, structId);

    ctx.output += QStringLiteral("};\n");
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

// ── Align offset comments ──
// Replaces kCommentMarker with spaces so all "// 0x..." comments align to
// the same column (the longest code portion + 1 space).

static QString alignComments(const QString& raw) {
    QStringList lines = raw.split('\n');

    // First pass: find the maximum code width (text before the marker)
    int maxCode = 0;
    for (const QString& line : lines) {
        int pos = line.indexOf(kCommentMarker);
        if (pos >= 0)
            maxCode = qMax(maxCode, pos);
    }

    // Second pass: replace markers with padding
    QString result;
    result.reserve(raw.size() + lines.size() * 8);
    for (int i = 0; i < lines.size(); i++) {
        if (i > 0) result += '\n';
        const QString& line = lines[i];
        int pos = line.indexOf(kCommentMarker);
        if (pos >= 0) {
            result += line.left(pos);
            int pad = maxCode - pos + 1;
            if (pad < 1) pad = 1;
            result += QString(pad, ' ');
            result += line.mid(pos + 1);  // skip the marker char
        } else {
            result += line;
        }
    }
    return result;
}

} // anonymous namespace

// ── Public API ──

QString renderCpp(const NodeTree& tree, uint64_t rootStructId,
                  const QHash<NodeKind, QString>* typeAliases) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};

    const Node& root = tree.nodes[idx];
    if (root.kind != NodeKind::Struct) return {};

    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, 0, typeAliases};

    ctx.output += QStringLiteral("#pragma once\n\n");

    emitStruct(ctx, rootStructId);

    return alignComments(ctx.output);
}

QString renderCppAll(const NodeTree& tree,
                     const QHash<NodeKind, QString>* typeAliases) {
    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, 0, typeAliases};

    ctx.output += QStringLiteral("#pragma once\n\n");

    QVector<int> roots = ctx.childMap.value(0);
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });

    for (int ri : roots) {
        if (tree.nodes[ri].kind == NodeKind::Struct)
            emitStruct(ctx, tree.nodes[ri].id);
    }

    return alignComments(ctx.output);
}

QString renderNull(const NodeTree&, uint64_t) {
    return {};
}

} // namespace rcx
