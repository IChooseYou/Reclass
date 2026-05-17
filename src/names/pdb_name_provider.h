#pragma once

#include "name_provider.h"

namespace rcx {

// Lists every PDB symbol loaded in SymbolStore as a navigable
// {name, absoluteAddress} pair, plus every TPI type as an address-less
// entry that the unified Symbols panel can right-click to import.
class PdbNameProvider : public NameProvider {
public:
    QString id() const override { return QStringLiteral("pdb-symbols"); }
    QString displayName() const override { return QStringLiteral("PDB Symbols"); }
    uint32_t accent() const override;

    QVector<NamedAddress> entries(const Provider* active) const override;
    QString  nameFor(uint64_t addr, const Provider* active) const override;
    uint64_t addressFor(const QString& name, const Provider* active) const override;
};

} // namespace rcx
