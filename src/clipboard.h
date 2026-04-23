#pragma once
#include "core.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QSet>
#include <QHash>
// Clipboard codec — no QApplication/QClipboard dependency; callers supply
// the QMimeData roundtrip. Keeps this header usable in QtCore-only tests.

namespace rcx {

// ── ClipboardCodec ──
// Serializes a selection of nodes (plus their subtrees) to a portable blob
// suitable for a round-trip paste elsewhere in the same project, in another
// tab, or — via the plain-text fallback — pasted verbatim into a text editor
// for inspection. JSON form lives on the clipboard under the custom MIME
// type "application/x-reclass-nodes-v1"; plain text is set via QMimeData's
// text path so external apps (VS Code, Notepad, chat) get a readable dump.
//
// Design notes:
//   * Each serialized node reuses Node::toJson so we don't fork the field
//     schema. Parents are included when nested under the selection root.
//   * Deserialize regenerates IDs so pasted nodes don't collide with any
//     existing node in the target tree. Parent/refId references are re-
//     wired to the new IDs.
//   * The plain-text form is a listing, one node per line:
//       "+0x08  uint32_t  health"
//     good enough for humans to skim; not meant to round-trip back.

struct ClipboardCodec {
    static constexpr const char* kMimeType = "application/x-reclass-nodes-v1";

    // Collect node + all descendants (iterative, cycle-safe).
    // roots are ids the user explicitly picked; we include their subtrees.
    static QVector<Node> collectSubtrees(const NodeTree& tree,
                                         const QVector<uint64_t>& roots)
    {
        QVector<Node> out;
        QSet<uint64_t> seen;
        QVector<uint64_t> stack = roots;
        while (!stack.isEmpty()) {
            uint64_t id = stack.takeLast();
            if (seen.contains(id)) continue;
            seen.insert(id);
            int idx = tree.indexOfId(id);
            if (idx < 0) continue;
            out.append(tree.nodes[idx]);
            for (int ci : tree.childrenOf(id))
                stack.append(tree.nodes[ci].id);
        }
        return out;
    }

    // Produce a QMimeData carrying both MIME-typed JSON and plain-text.
    // Caller owns the returned QMimeData (typical clipboard handoff).
    static QMimeData* serialize(const NodeTree& tree,
                                const QVector<uint64_t>& rootIds,
                                const QSet<uint64_t>& clearParentFor = {})
    {
        auto* mime = new QMimeData;
        QVector<Node> nodes = collectSubtrees(tree, rootIds);

        // JSON payload
        QJsonArray arr;
        QJsonArray rootArr;
        for (uint64_t r : rootIds) rootArr.append(QString::number(r));
        for (const Node& n : nodes) {
            QJsonObject o = n.toJson();
            // Optional parent decoupling — when a caller wants a selection
            // to paste at a new location without carrying its old parent
            // linkage, they pass those ids in clearParentFor.
            if (clearParentFor.contains(n.id))
                o["parentId"] = QString::number(0);
            arr.append(o);
        }
        QJsonObject payload;
        payload["schema"]  = QStringLiteral("rcx-clipboard/v1");
        payload["roots"]   = rootArr;
        payload["nodes"]   = arr;
        mime->setData(kMimeType,
                      QJsonDocument(payload).toJson(QJsonDocument::Compact));

        // Plain-text fallback
        mime->setText(plainDump(tree, rootIds));
        return mime;
    }

    // Parse the custom MIME blob back into nodes with freshly minted IDs.
    // Returns an empty vector if the clipboard doesn't carry our format.
    // `tree` is needed so we can allocate non-colliding IDs via reserveId().
    struct PasteResult {
        QVector<Node>     nodes;        // ready to insert (IDs remapped)
        QVector<uint64_t> rootIds;      // new ids corresponding to original roots
    };
    static PasteResult deserialize(NodeTree& tree, const QMimeData* mime) {
        PasteResult r;
        if (!mime || !mime->hasFormat(kMimeType)) return r;
        QJsonDocument doc = QJsonDocument::fromJson(mime->data(kMimeType));
        if (!doc.isObject()) return r;
        QJsonObject payload = doc.object();
        if (payload.value("schema").toString() != QStringLiteral("rcx-clipboard/v1"))
            return r;

        // Load nodes verbatim first so we know every old id.
        QVector<Node> raw;
        for (const auto& v : payload.value("nodes").toArray())
            raw.append(Node::fromJson(v.toObject()));
        if (raw.isEmpty()) return r;

        // Build old→new id map.
        QHash<uint64_t, uint64_t> idMap;
        for (const Node& n : raw) idMap.insert(n.id, tree.reserveId());

        // Rewire.
        r.nodes.reserve(raw.size());
        for (Node n : raw) {
            n.id       = idMap.value(n.id, n.id);
            n.parentId = idMap.value(n.parentId, n.parentId);  // may be 0 → stays 0
            n.refId    = idMap.value(n.refId, n.refId);        // may be 0 → stays 0
            r.nodes.append(std::move(n));
        }
        for (const auto& v : payload.value("roots").toArray())
            r.rootIds.append(idMap.value(v.toString().toULongLong(), 0));
        return r;
    }

    // Human-readable dump used for plain-text clipboard + external pastes.
    static QString plainDump(const NodeTree& tree,
                             const QVector<uint64_t>& rootIds)
    {
        QStringList lines;
        for (uint64_t r : rootIds) {
            int idx = tree.indexOfId(r);
            if (idx < 0) continue;
            dumpNode(tree, idx, 0, lines);
        }
        return lines.join('\n');
    }

private:
    static void dumpNode(const NodeTree& tree, int idx, int depth,
                         QStringList& out)
    {
        const Node& n = tree.nodes[idx];
        const char* kindName = kindToString(n.kind);
        QString line = QString("%1+0x%2  %3  %4")
            .arg(QString(depth * 2, ' '))
            .arg(n.offset, 4, 16, QLatin1Char('0'))
            .arg(QString::fromLatin1(kindName), -8)
            .arg(n.name);
        out.append(line);
        for (int ci : tree.childrenOf(n.id))
            dumpNode(tree, ci, depth + 1, out);
    }
};

} // namespace rcx
