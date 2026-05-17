#include "bookmark_name_provider.h"
#include "controller.h"
#include "core.h"
#include "addressparser.h"
#include "symbolstore.h"
#include "providers/provider.h"
#include "themes/thememanager.h"
#include <QColor>

namespace rcx {

uint32_t BookmarkNameProvider::accent() const {
    return ThemeManager::instance().current().indDataChanged.rgba();
}

static uint64_t evaluateFormula(const QString& formula, const Provider* prov,
                                int ptrSize) {
    AddressParserCallbacks cbs;
    if (prov) {
        cbs.resolveModule = [prov](const QString& name, bool* ok) -> uint64_t {
            uint64_t base = prov->symbolToAddress(name);
            *ok = (base != 0);
            return base;
        };
        cbs.readPointer = [prov, ptrSize](uint64_t addr, bool* ok) -> uint64_t {
            uint64_t v = 0;
            *ok = prov->read(addr, &v, ptrSize);
            return v;
        };
        cbs.resolveIdentifier = [prov](const QString& name, bool* ok) -> uint64_t {
            return SymbolStore::instance().resolve(name, prov, ok);
        };
    }
    auto result = AddressParser::evaluate(formula, ptrSize ? ptrSize : 8, &cbs);
    return result.ok ? result.value : 0;
}

QVector<NamedAddress> BookmarkNameProvider::entries(const Provider* active) const {
    QVector<NamedAddress> out;
    auto* ctrl = m_fn ? m_fn() : nullptr;
    if (!ctrl || !ctrl->document()) return out;
    const auto& bms = ctrl->document()->tree.bookmarks;
    int ptrSize = ctrl->document()->tree.pointerSize;
    for (const auto& b : bms) {
        NamedAddress n;
        n.name = b.name;
        n.address = evaluateFormula(b.addressFormula, active, ptrSize);
        n.kind = QStringLiteral("bookmark");
        out.append(std::move(n));
    }
    return out;
}

bool BookmarkNameProvider::add(const QString& name, uint64_t address) {
    auto* ctrl = m_fn ? m_fn() : nullptr;
    if (!ctrl) return false;
    QString formula = QStringLiteral("0x%1").arg(address, 0, 16);
    ctrl->addBookmark(name, formula);
    return true;
}

bool BookmarkNameProvider::remove(const QString& name) {
    auto* ctrl = m_fn ? m_fn() : nullptr;
    if (!ctrl || !ctrl->document()) return false;
    auto& bms = ctrl->document()->tree.bookmarks;
    for (int i = 0; i < bms.size(); i++) {
        if (bms[i].name == name) {
            ctrl->removeBookmark(i);
            return true;
        }
    }
    return false;
}

} // namespace rcx
