#pragma once

#include "name_provider.h"
#include <QVector>
#include <QHash>
#include <QMutex>

namespace rcx {

// In-memory store of RTTI walker discoveries. The compose path pushes hits
// here right after a successful walkRtti(); the unified Symbols panel and
// expression parser then see them like any other named address.
//
// Lives as a singleton — RTTI hits accumulate across compose sessions, so
// classes discovered earlier in the session remain reachable. Cleared on
// process detach (caller responsibility — clearForModule() helper provided).
class RttiNameProvider : public NameProvider {
public:
    static RttiNameProvider& instance();

    QString id() const override { return QStringLiteral("rtti"); }
    QString displayName() const override { return QStringLiteral("RTTI"); }
    uint32_t accent() const override;

    QVector<NamedAddress> entries(const Provider* active) const override;

    // Push a discovery. Idempotent (skips duplicates by name+address).
    void push(const QString& name, uint64_t address,
              const QString& moduleName = {});

    // Drop accumulated hits — call on process detach or explicit clear.
    void clear();
    void clearForModule(const QString& moduleName);

private:
    RttiNameProvider() = default;
    mutable QMutex m_lock;
    QVector<NamedAddress> m_hits;
    QHash<QString, int>   m_byKey; // "name@0xADDR" → index, dedupe
};

} // namespace rcx
