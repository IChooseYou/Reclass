#include "pdb_type_provider.h"
#include "symbol_demangle.h"
#include "symbolstore.h"
#include "providers/provider.h"
#include "themes/thememanager.h"
#include <QColor>

namespace rcx {

uint32_t PdbTypeProvider::accent() const {
    // Teal — matches how type names are colored in the syntax theme, and
    // visually contrasts with PdbNameProvider's blue source pip.
    return ThemeManager::instance().current().syntaxType.rgba();
}

QVector<NamedAddress> PdbTypeProvider::entries(const Provider* /*active*/) const {
    QVector<NamedAddress> out;
    auto& store = SymbolStore::instance();
    const QStringList modules = store.loadedModules();
    for (const QString& mod : modules) {
        const auto* set = store.moduleData(mod);
        if (!set) continue;
        for (const auto& ti : set->types) {
            NamedAddress n;
            n.name = ti.name;
            n.displayName = humanizeSymbolName(ti.name);
            n.address = 0;
            n.size = static_cast<uint32_t>(ti.size);
            n.typeIndex = ti.typeIndex;
            // Distinguish struct/union/enum so the panel can show the
            // correct word + badge letter. PDB doesn't separately tag
            // class vs struct (LF_STRUCTURE / LF_CLASS share semantics)
            // so we use "struct" as the catch-all for non-union, non-enum.
            n.kind = ti.isEnum  ? QStringLiteral("enum")
                   : ti.isUnion ? QStringLiteral("union")
                                : QStringLiteral("struct");
            n.meta = set->pdbPath;
            out.append(std::move(n));
        }
    }
    return out;
}

} // namespace rcx
