#pragma once
#include <QString>
#include <QStringList>
#include <QHash>
#include <QVector>
#include <QPair>
#include <algorithm>

namespace rcx {

class Provider;  // forward declaration

struct PdbSymbolSet {
    QString pdbPath;
    QString moduleName;          // canonical lowercase name (e.g. "ntoskrnl")
    QHash<QString, uint32_t> nameToRva;
    QHash<QString, uint32_t> nameToTypeIndex; // symbol name → TPI typeIndex (0 = no type info)
    QVector<QPair<uint32_t, QString>> rvaToName;  // sorted by RVA for binary search

    void sortRvaIndex() {
        std::sort(rvaToName.begin(), rvaToName.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
    }
};

class SymbolStore {
public:
    static SymbolStore& instance() {
        static SymbolStore s;
        return s;
    }

    // Add a pre-extracted symbol set for a module.
    // moduleName is the canonical name (e.g. "ntoskrnl").
    // Returns the number of unique symbols stored.
    int addModule(const QString& moduleName, const QString& pdbPath,
                  const QVector<QPair<QString, uint32_t>>& symbols);

    // Store symbol→typeIndex mapping for a previously-added module.
    // Called after addModule with the typeIndex data from PdbSymbol records.
    void addModuleTypeIndices(const QString& moduleName,
                              const QHash<QString, uint32_t>& nameToTypeIndex);

    // Look up the TPI typeIndex for a qualified symbol (e.g. "ntdll!g_pShimEngineModule").
    // Returns 0 if not found or no type info available.
    uint32_t typeIndexForSymbol(const QString& qualifiedSymbol) const;

    // Unload symbols for a module.
    void unloadModule(const QString& moduleName);

    // Resolve a token from the expression parser.
    // Handles "module!symbol" (qualified) and bare "symbol" (unqualified).
    // Uses provider->symbolToAddress() to get the module's runtime base address.
    uint64_t resolve(const QString& token, const Provider* provider, bool* ok) const;

    // Reverse lookup: given an absolute address and a provider, find the nearest symbol.
    // Returns "module!symbol" or "module!symbol+0xN", or empty if no match.
    QString getSymbolForAddress(uint64_t addr, const Provider* provider) const;

    // Check if any symbols are loaded.
    bool hasSymbols() const { return !m_modules.isEmpty(); }

    // List loaded module names.
    QStringList loadedModules() const { return m_modules.keys(); }

    // Number of loaded modules.
    int moduleCount() const { return m_modules.size(); }

    // Access module data by name (returns nullptr if not found).
    const PdbSymbolSet* moduleData(const QString& moduleName) const {
        QString canonical = resolveAlias(moduleName);
        auto it = m_modules.find(canonical);
        return it != m_modules.end() ? &*it : nullptr;
    }

    // Add a module alias (e.g. "nt" → "ntoskrnl").
    void addAlias(const QString& alias, const QString& canonicalModule);

    // Resolve alias to canonical module name (public for callers that need it)
    QString resolveAlias(const QString& name) const {
        QString lower = name.toLower();
        if (lower.endsWith(QStringLiteral(".exe")) || lower.endsWith(QStringLiteral(".dll")) ||
            lower.endsWith(QStringLiteral(".sys")))
            lower = lower.left(lower.lastIndexOf('.'));
        auto it = m_aliases.find(lower);
        return it != m_aliases.end() ? *it : lower;
    }

private:
    SymbolStore() {
        // Common Windows kernel aliases
        m_aliases[QStringLiteral("nt")] = QStringLiteral("ntoskrnl");
        m_aliases[QStringLiteral("ntkrnlmp")] = QStringLiteral("ntoskrnl");
        m_aliases[QStringLiteral("ntkrnlpa")] = QStringLiteral("ntoskrnl");
        m_aliases[QStringLiteral("ntkrpamp")] = QStringLiteral("ntoskrnl");
    }

    // Get the module base address, trying various name forms
    uint64_t getModuleBase(const Provider* provider, const QString& canonical) const;

    QHash<QString, PdbSymbolSet> m_modules;  // canonical lowercase name → symbol set
    QHash<QString, QString> m_aliases;        // alias → canonical name
};

} // namespace rcx
