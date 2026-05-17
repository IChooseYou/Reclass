#pragma once

#include "name_provider.h"

namespace rcx {

// Surfaces every TPI type (struct/class/union/enum) from each loaded PDB
// as an address-less entry with the type's `typeIndex` set, so the
// unified Symbols panel can right-click → "Import type" or multi-select
// → "Import N selected types". Distinct from PdbNameProvider so each
// gets its own filter chip + accent color in the UI.
class PdbTypeProvider : public NameProvider {
public:
    QString id() const override { return QStringLiteral("pdb-types"); }
    QString displayName() const override { return QStringLiteral("PDB Types"); }
    uint32_t accent() const override;

    QVector<NamedAddress> entries(const Provider* active) const override;
};

} // namespace rcx
