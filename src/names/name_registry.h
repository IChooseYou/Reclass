#pragma once

#include "name_provider.h"
#include <QObject>
#include <QVector>
#include <memory>

namespace rcx {

class Provider;

// Global aggregator for NameProvider implementations. Built-in providers
// (PDB, RTTI, Bookmarks) get registered at MainWindow startup; plugins can
// register additional ones later. The unified Symbols panel reads its rows
// here, and address→name lookups elsewhere in the app go through nameFor().
class NameRegistry : public QObject {
    Q_OBJECT
public:
    static NameRegistry& instance();

    void registerProvider(std::shared_ptr<NameProvider> p);
    void unregisterProvider(const QString& id);

    QVector<std::shared_ptr<NameProvider>> providers() const { return m_providers; }

    // Aggregated reverse lookup over every registered provider, in
    // registration order. First non-empty answer wins. Used by status
    // bar / tooltips / expression parser.
    QString  nameFor(uint64_t addr, const Provider* active) const;
    uint64_t addressFor(const QString& name, const Provider* active) const;

    // Providers call this when their underlying data shifts so listeners
    // (e.g. the Symbols panel) can refresh.
    void emitChanged();

signals:
    void providersChanged();

private:
    NameRegistry() = default;
    QVector<std::shared_ptr<NameProvider>> m_providers;
};

} // namespace rcx
