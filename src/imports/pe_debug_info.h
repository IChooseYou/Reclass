#pragma once
#include <QString>
#include <cstdint>

namespace rcx {

class Provider;

struct PdbDebugInfo {
    QString pdbName;      // e.g. "ntoskrnl.pdb"
    QString guidString;   // 32 hex chars, no dashes, uppercase
    uint32_t age = 0;
    bool valid = false;
};

// Extract PDB debug info (GUID, age, filename) from a PE module in memory.
// Reads DOS header → PE header → debug directory → CodeView RSDS record.
PdbDebugInfo extractPdbDebugInfo(const Provider& prov, uint64_t moduleBase);

} // namespace rcx
