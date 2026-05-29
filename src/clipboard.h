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

    // Parse a clipboard hex string into raw bytes.
    //
    // The contract: tokenize on any non-hex character, treat each token
    // as a hex number, and concatenate left-to-right. Numbers are
    // left-padded with `0` to an even nibble count so a stand-alone
    // odd-length token like "A" yields one byte 0x0A (not 0xA0).
    //
    // Accepts every common encoding of a hex byte stream:
    //   "DE AD BE EF"        →  DE AD BE EF
    //   "DEADBEEF"           →  DE AD BE EF
    //   "0xDEADBEEF"         →  DE AD BE EF
    //   "{0xDE, 0xAD, 0xBE}" →  DE AD BE
    //   "DE,AD"              →  DE AD
    //   "1 2 3"              →  01 02 03   (per-token left-pad)
    //   "0x100"              →  01 00      (token "100" left-padded to "0100")
    //
    // On a malformed token the parser returns an empty array and writes
    // a one-line reason into *err. Empty clipboard / no hex digits also
    // returns empty (with err = "No hex data").
    static QByteArray parseLenientHex(const QString& src, QString* err = nullptr) {
        QByteArray out;
        out.reserve(src.size() / 2);
        QString tok;
        auto isHex = [](QChar c) {
            return c.isDigit()
                || (c >= QChar('a') && c <= QChar('f'))
                || (c >= QChar('A') && c <= QChar('F'));
        };
        auto flush = [&]() -> bool {
            if (tok.isEmpty()) return true;
            // Strip optional 0x/0X prefix on the token (not mid-token).
            if (tok.size() > 2 && tok[0] == QChar('0')
                && (tok[1] == QChar('x') || tok[1] == QChar('X')))
                tok.remove(0, 2);
            // The 'x' itself is not a hex digit — any token that still
            // contains it after prefix-strip is malformed.
            for (QChar c : tok) {
                if (!isHex(c)) {
                    if (err) *err = QStringLiteral("Invalid hex digit '%1'").arg(c);
                    return false;
                }
            }
            // Left-pad to even so a bare "A" yields 0x0A, not 0xA0.
            if (tok.size() & 1) tok.prepend(QChar('0'));
            for (int i = 0; i + 1 < tok.size(); i += 2) {
                bool ok = false;
                uint8_t b = (uint8_t)tok.mid(i, 2).toUInt(&ok, 16);
                if (!ok) {
                    if (err) *err = QStringLiteral("Parse failed");
                    return false;
                }
                out.append((char)b);
            }
            tok.clear();
            return true;
        };
        for (int i = 0; i < src.size(); ++i) {
            QChar c = src[i];
            // Token grows on hex digit OR the literal 'x'/'X' immediately
            // after a leading '0' (so "0x" stays in the token to be stripped
            // by flush()). Anything else flushes.
            if (isHex(c)) { tok += c; continue; }
            if ((c == QChar('x') || c == QChar('X'))
                && tok.size() == 1 && tok[0] == QChar('0')) {
                tok += c; continue;
            }
            // Stray letter (including a mid-token x/X) is malformed —
            // refuse the whole paste rather than silently dropping the
            // invalid run and parsing whatever survives.
            if (c.isLetter()) {
                if (err) *err = QStringLiteral("Invalid hex digit '%1'").arg(c);
                return {};
            }
            // Whitespace, punctuation, control characters → separator.
            if (!flush()) return {};
        }
        if (!flush()) return {};
        if (out.isEmpty() && err && err->isEmpty())
            *err = QStringLiteral("No hex data");
        return out;
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
