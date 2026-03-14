#include "symbolstore.h"
#include "providers/provider.h"
#include <QDebug>

namespace rcx {

uint64_t SymbolStore::getModuleBase(const Provider* provider, const QString& canonical) const {
    if (!provider)
        return 0;
    uint64_t base = provider->symbolToAddress(canonical);
    if (base == 0)
        base = provider->symbolToAddress(canonical + QStringLiteral(".exe"));
    if (base == 0)
        base = provider->symbolToAddress(canonical + QStringLiteral(".dll"));
    if (base == 0)
        base = provider->symbolToAddress(canonical + QStringLiteral(".sys"));
    return base;
}

int SymbolStore::addModule(const QString& moduleName, const QString& pdbPath,
                            const QVector<QPair<QString, uint32_t>>& symbols) {
    QString canonical = resolveAlias(moduleName);

    PdbSymbolSet set;
    set.pdbPath = pdbPath;
    set.moduleName = canonical;
    set.nameToRva.reserve(symbols.size());
    set.rvaToName.reserve(symbols.size());

    for (const auto& sym : symbols) {
        if (set.nameToRva.contains(sym.first))
            continue;
        set.nameToRva.insert(sym.first, sym.second);
        set.rvaToName.append({sym.second, sym.first});
    }

    set.sortRvaIndex();
    int count = set.nameToRva.size();

    // Register the raw module name as an alias if it differs from canonical
    QString rawLower = moduleName.toLower();
    if (rawLower.endsWith(QStringLiteral(".exe")) || rawLower.endsWith(QStringLiteral(".dll")) ||
        rawLower.endsWith(QStringLiteral(".sys")))
        rawLower = rawLower.left(rawLower.lastIndexOf('.'));
    if (rawLower != canonical)
        m_aliases[rawLower] = canonical;

    m_modules[canonical] = std::move(set);

    qDebug() << "[SymbolStore] loaded" << count << "symbols for module" << canonical
             << "(from" << pdbPath << ")";
    return count;
}

void SymbolStore::unloadModule(const QString& moduleName) {
    QString canonical = resolveAlias(moduleName);
    m_modules.remove(canonical);
}

uint64_t SymbolStore::resolve(const QString& token, const Provider* provider, bool* ok) const {
    *ok = false;

    // Check for "module!symbol" syntax
    int bangIdx = token.indexOf('!');
    if (bangIdx > 0 && bangIdx < token.size() - 1) {
        QString modPart = token.left(bangIdx);
        QString symPart = token.mid(bangIdx + 1);
        QString canonical = resolveAlias(modPart);

        auto modIt = m_modules.find(canonical);
        if (modIt == m_modules.end())
            return 0;

        auto symIt = modIt->nameToRva.find(symPart);
        if (symIt == modIt->nameToRva.end())
            return 0;

        uint32_t rva = *symIt;
        uint64_t moduleBase = getModuleBase(provider, canonical);
        // Also try the user-supplied module name form
        if (moduleBase == 0)
            moduleBase = getModuleBase(provider, modPart);

        *ok = true;
        return moduleBase + rva;
    }

    // Bare symbol — search all loaded modules
    uint32_t foundRva = 0;
    QString foundModule;
    int matches = 0;

    for (auto it = m_modules.begin(); it != m_modules.end(); ++it) {
        auto symIt = it->nameToRva.find(token);
        if (symIt != it->nameToRva.end()) {
            foundRva = *symIt;
            foundModule = it.key();
            matches++;
            if (matches > 1)
                return 0; // ambiguous
        }
    }

    if (matches == 1) {
        uint64_t moduleBase = getModuleBase(provider, foundModule);
        *ok = true;
        return moduleBase + foundRva;
    }

    // Fallback: treat bare token as a module name (e.g. "ntdll" → ntdll base)
    if (matches == 0) {
        QString canonical = resolveAlias(token);
        uint64_t moduleBase = getModuleBase(provider, canonical);
        if (moduleBase != 0) {
            *ok = true;
            return moduleBase;
        }
    }

    return 0;
}

QString SymbolStore::getSymbolForAddress(uint64_t addr, const Provider* provider) const {
    if (m_modules.isEmpty() || !provider)
        return {};

    for (auto it = m_modules.begin(); it != m_modules.end(); ++it) {
        const PdbSymbolSet& set = *it;

        uint64_t moduleBase = getModuleBase(provider, set.moduleName);
        if (moduleBase == 0)
            continue;

        if (addr < moduleBase)
            continue;

        uint32_t rva = static_cast<uint32_t>(addr - moduleBase);

        if (set.rvaToName.isEmpty())
            continue;

        // Binary search: find last entry with RVA <= target
        auto upper = std::upper_bound(set.rvaToName.begin(), set.rvaToName.end(), rva,
            [](uint32_t val, const QPair<uint32_t, QString>& entry) {
                return val < entry.first;
            });

        if (upper == set.rvaToName.begin())
            continue;

        --upper;
        uint32_t displacement = rva - upper->first;

        static constexpr uint32_t kMaxDisplacement = 0x1000;
        if (displacement > kMaxDisplacement)
            continue;

        if (displacement == 0)
            return set.moduleName + QStringLiteral("!") + upper->second;
        return set.moduleName + QStringLiteral("!") + upper->second
             + QStringLiteral("+0x") + QString::number(displacement, 16);
    }

    return {};
}

void SymbolStore::addAlias(const QString& alias, const QString& canonicalModule) {
    m_aliases[alias.toLower()] = canonicalModule.toLower();
}

} // namespace rcx
