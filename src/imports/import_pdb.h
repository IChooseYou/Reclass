#pragma once
#include "core.h"
#include <QVector>
#include <functional>

namespace rcx {

// ── PDB Symbol Extraction ──

struct PdbSymbol {
    QString name;
    uint32_t rva;
    uint32_t typeIndex = 0;  // TPI type index (0 = unknown / public symbol)
};

struct PdbSymbolResult {
    QString moduleName;          // derived from PDB filename (e.g. "ntoskrnl")
    QVector<PdbSymbol> symbols;
};

// Extract public/global symbols (name → RVA) from a PDB file.
// This reads the DBI stream's public and global symbol sub-streams.
PdbSymbolResult extractPdbSymbols(const QString& pdbPath,
                                   QString* errorMsg = nullptr);

// ── PDB Type Import ──

struct PdbTypeInfo {
    uint32_t typeIndex;      // TPI type index
    QString  name;           // struct/class/union/enum name
    uint64_t size;           // sizeof in bytes
    int      childCount;     // direct member count
    bool     isUnion;        // union vs struct/class
    bool     isEnum = false; // enum type
};

// Phase 1: Enumerate all UDT types in the PDB (fast scan, no recursive import).
QVector<PdbTypeInfo> enumeratePdbTypes(const QString& pdbPath,
                                       QString* errorMsg = nullptr);

// Phase 2: Import selected types with full recursive child types.
// progressCb is called with (current, total) for each top-level type;
// return false from the callback to cancel the import.
using ProgressCb = std::function<bool(int current, int total)>;
NodeTree importPdbSelected(const QString& pdbPath,
                           const QVector<uint32_t>& typeIndices,
                           QString* errorMsg = nullptr,
                           ProgressCb progressCb = {});

// Legacy single-call API: import one struct by name (or all if filter empty).
NodeTree importPdb(const QString& pdbPath,
                   const QString& structFilter = {},
                   QString* errorMsg = nullptr);

// Import the type associated with a global symbol's typeIndex.
// Opens the PDB, resolves the typeIndex to a UDT/enum, and returns the imported tree.
// Returns empty tree if the symbol has no associated type or the type is a simple primitive.
NodeTree importTypeForSymbol(const QString& pdbPath,
                             uint32_t typeIndex,
                             QString* typeName = nullptr,
                             QString* errorMsg = nullptr);

} // namespace rcx
