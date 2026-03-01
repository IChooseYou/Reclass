#pragma once
#include "core.h"

namespace rcx {

// Import C/C++ struct definitions from source code into a NodeTree.
// Supports two modes (auto-detected):
//   1. With comment offsets (// 0xNN) - trusts the offset values
//   2. Without comment offsets - computes offsets from type sizes
// pointerSize: 4 for 32-bit targets, 8 for 64-bit (default).
// Returns an empty NodeTree on failure; populates errorMsg if non-null.
NodeTree importFromSource(const QString& sourceCode, QString* errorMsg = nullptr,
                          int pointerSize = 8);

} // namespace rcx
