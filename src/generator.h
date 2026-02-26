#pragma once
#include "core.h"
#include <QString>
#include <QHash>
#include <QSet>

namespace rcx {

// Generate C++ struct definitions for a single root struct and all
// nested/referenced types reachable from it.
QString renderCpp(const NodeTree& tree, uint64_t rootStructId,
                  const QHash<NodeKind, QString>* typeAliases = nullptr,
                  bool emitAsserts = false);

// Generate C++ struct definitions for every root-level struct (full SDK).
QString renderCppAll(const NodeTree& tree,
                     const QHash<NodeKind, QString>* typeAliases = nullptr,
                     bool emitAsserts = false);

// Null generator placeholder (returns empty string).
QString renderNull(const NodeTree& tree, uint64_t rootStructId);

} // namespace rcx
