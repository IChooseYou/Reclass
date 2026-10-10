#pragma once

#include "core.h"
#include <QJsonArray>

namespace rcx::resolution {

// Read validity is explicit: failed reads must never become observations of zero.
struct Sample {
    QByteArray bytes;
    QByteArray valid;
    qint64 timestampMs = 0;
    QString error;
};

struct Candidate {
    QString id;
    int offset = 0; // relative to the selected parent, never the display row
    int size = 0;
    NodeKind kind = NodeKind::Hex8;
    int score = 0; // ranking score, NOT a calibrated probability
    int observations = 0;
    int distinctValues = 0;
    QStringList evidence;
    QStringList caveats;
    QString target; // exact pointer value, if applicable
    bool verified = false; // independently bound matching debug metadata, never LLM confidence
    bool autoApplicable = false; // passes RC's conservative automatic-application gate
    QString evidenceLevel = QStringLiteral("hypothesis"); // hypothesis, high, authoritative
    QString declaredName;
    QJsonArray values;
    QJsonObject toJson() const;
};

struct Analysis {
    QVector<Candidate> candidates;
    QJsonArray suggestedFields;
    QJsonArray unresolved;
    QJsonArray pointers;
    QJsonArray modules;
    bool truncated = false;
};

// Only unannotated, little-endian hex leaves are eligible for replacement.
// Names are user-owned metadata: they are preserved, but do not block type recovery.
QByteArray eligibleBytes(const NodeTree& tree, uint64_t parentId, int offset, int length);
// Existing float/vector/matrix leaves supply read-only context on later AUTO runs.
QByteArray knownFloatBytes(const NodeTree& tree, uint64_t parentId, int offset, int length);
QVector<Candidate> infer(const QVector<Sample>& samples, const QByteArray& eligible,
                         int regionOffset, int pointerSize, const QByteArray& knownFloats = {});
void selectLayout(Analysis& analysis, const QByteArray& eligible, int regionOffset,
                  bool automaticOnly = false);

// Validates all fields on a detached tree. No live state or IDs are mutated.
// Leaves must cite server-generated candidates. Structs/arrays can group them.
bool prepare(const NodeTree& tree, uint64_t parentId, int regionOffset, int length,
             const QVector<Candidate>& candidates, const QJsonArray& fields,
             NodeTree& result, QJsonArray& appliedFields, QString& error);

} // namespace rcx::resolution
