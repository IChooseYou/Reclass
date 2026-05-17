#include "pdb_name_provider.h"
#include "symbol_demangle.h"
#include "symbolstore.h"
#include "providers/provider.h"
#include "themes/thememanager.h"
#include <QColor>

namespace rcx {

uint32_t PdbNameProvider::accent() const {
    return ThemeManager::instance().current().syntaxKeyword.rgba();
}

static uint64_t moduleBaseFor(const Provider* active, const QString& canonical) {
    if (!active) return 0;
    uint64_t base = active->symbolToAddress(canonical);
    if (base == 0) base = active->symbolToAddress(canonical + QStringLiteral(".exe"));
    if (base == 0) base = active->symbolToAddress(canonical + QStringLiteral(".dll"));
    if (base == 0) base = active->symbolToAddress(canonical + QStringLiteral(".sys"));
    return base;
}

QVector<NamedAddress> PdbNameProvider::entries(const Provider* active) const {
    QVector<NamedAddress> out;
    auto& store = SymbolStore::instance();
    const QStringList modules = store.loadedModules();
    for (const QString& mod : modules) {
        const auto* set = store.moduleData(mod);
        if (!set) continue;
        const uint64_t base = moduleBaseFor(active, mod);
        // Symbols only — types belong to PdbTypeProvider so they show as
        // a distinct filter chip with a different accent color.
        //
        // When the owning module isn't attached to a live target, base==0
        // and we MUST NOT publish "absolute = 0 + rva". Doing so makes the
        // editor's address→name annotation false-match user documents at
        // the symbol's RVA — e.g. loading advapi32.pdb (no advapi32 in the
        // target process) and then a fresh project at base 0x2300 would
        // paint the mangled "??0?$vector@..." in the comment column at
        // offset 0x2300 because some advapi32 symbol happens to have RVA
        // 0x2300. We mark the address as 0 ("no live address") so the row
        // still shows in the panel (useful for browsing what's in the PDB)
        // but the reverse address→name lookup skips it.
        for (auto it = set->nameToRva.constBegin(); it != set->nameToRva.constEnd(); ++it) {
            NamedAddress n;
            n.name = it.key();
            n.displayName = humanizeSymbolName(it.key());
            n.address = (base != 0) ? (base + it.value()) : 0;
            auto tIt = set->nameToTypeIndex.constFind(it.key());
            if (tIt != set->nameToTypeIndex.constEnd()) n.typeIndex = tIt.value();
            n.kind = QStringLiteral("symbol");
            n.meta = set->pdbPath;
            out.append(std::move(n));
        }
    }
    return out;
}

// Reverse lookup: address → "module!Name" or "module!Name+0xN", with the
// symbol portion run through humanizeSymbolName so the editor's comment
// column shows a readable C++ name instead of a raw mangled token like
// "??0?$vector@EV?$allocator@E@utl@@@utl@@QEAA@XZ".
//
// Routes through SymbolStore::getSymbolForAddress which binary-searches a
// sorted per-module RVA index, and also implicitly requires the owning
// module to be attached to the active provider — so it never false-matches
// an unloaded PDB's RVA against an unrelated user base address.
QString PdbNameProvider::nameFor(uint64_t addr, const Provider* active) const {
    if (addr == 0) return {};
    QString raw = SymbolStore::instance().getSymbolForAddress(addr, active);
    if (raw.isEmpty()) return {};
    int bang = raw.indexOf(QLatin1Char('!'));
    if (bang < 0) return raw;
    QString prefix = raw.left(bang + 1);  // "module!"
    QString rest = raw.mid(bang + 1);
    int plus = rest.indexOf(QLatin1Char('+'));
    QString sym = (plus < 0) ? rest : rest.left(plus);
    QString suffix = (plus < 0) ? QString() : rest.mid(plus);
    QString humanized = humanizeSymbolName(sym);
    return prefix + (humanized.isEmpty() ? sym : humanized) + suffix;
}

uint64_t PdbNameProvider::addressFor(const QString& name, const Provider* active) const {
    // Try the qualified form first ("module!symbol") then bare symbol via
    // SymbolStore's own resolver, which does the same work and accepts both.
    bool ok = false;
    uint64_t a = SymbolStore::instance().resolve(name, active, &ok);
    return ok ? a : 0;
}

} // namespace rcx
