#pragma once

#include <QString>
#include <QVector>
#include <cstdint>

namespace rcx {

class Provider;

// One resolvable {name, address} pair contributed by some NameProvider.
//
// `address` is absolute (the provider is responsible for resolving any
// module-base offset against the active Provider*). `address == 0` is legal
// and means "this entry has no live address" — used for standalone PDB TPI
// types where only the type definition exists. Such rows render with `—`
// in the address column and are not navigable, but can still be acted on
// (e.g. right-click "Import type").
struct NamedAddress {
    QString  name;           // canonical identifier — used for reverse lookups
    QString  displayName;    // optional humanised form (e.g. demangled C++ name).
                              // If empty, consumers fall back to `name`.
    uint64_t address = 0;
    uint32_t size = 0;       // 0 = unknown
    uint32_t typeIndex = 0;  // non-zero ⇒ "Import type" affordance applies
    QString  source;         // filled by NameRegistry on aggregation (= provider id)
    QString  kind;           // optional sub-kind: "symbol" / "type" / "bookmark" / "rtti" / ...
    QString  meta;           // provider-private follow-up data (e.g. PDB path for type-import action)
};

// A pluggable source of {name, address} information. PDB, RTTI, bookmarks,
// and third-party plugins all implement this. NameRegistry::instance()
// aggregates registered providers so the rest of the app — the unified
// Symbols panel, the editor's address tooltips, the expression parser —
// reads through a single interface.
//
// Modeled on the rcx::Provider abstraction (src/providers/provider.h):
// pure-virtual identity + listing methods, optional reverse-lookup
// fast-paths, optional write API for providers that own user data.
class NameProvider {
public:
    virtual ~NameProvider() = default;

    // Stable identifier used for dedupe + UI filter chip key.
    virtual QString id() const = 0;

    // Display label (chip text).
    virtual QString displayName() const = 0;

    // Optional accent color for the source pip / filter chip, packed as
    // 0xAARRGGBB. 0 = "no opinion" → the panel hashes id() into the theme
    // accent palette. Packed-int form (rather than QColor) keeps the
    // header free of QtGui dependencies so providers can be used from
    // QtCore-only translation units (compose.cpp, tests).
    virtual uint32_t accent() const { return 0; }

    // List every entry this provider currently knows about. The Provider*
    // is the active data source; providers whose addresses depend on a
    // live process (PDB module bases, etc.) resolve through it.
    virtual QVector<NamedAddress> entries(const Provider* active) const = 0;

    // Reverse lookups. Defaults do a linear scan over entries(); override
    // for O(1) when the provider has a real reverse index.
    virtual QString  nameFor(uint64_t addr, const Provider* active) const;
    virtual uint64_t addressFor(const QString& name, const Provider* active) const;

    // Write API. Returns true if this provider can persist a user-created
    // entry (BookmarkNameProvider does; PDB/RTTI providers do not).
    virtual bool supportsAdd() const { return false; }
    virtual bool add(const QString& name, uint64_t address) {
        Q_UNUSED(name); Q_UNUSED(address); return false;
    }

    // Remove an existing entry (used by the panel's "Remove" right-click).
    // Returns true if the entry was removed. Default: not supported.
    virtual bool supportsRemove() const { return false; }
    virtual bool remove(const QString& name) { Q_UNUSED(name); return false; }
};

} // namespace rcx
