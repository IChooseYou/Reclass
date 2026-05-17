#include "name_provider.h"

namespace rcx {

QString NameProvider::nameFor(uint64_t addr, const Provider* active) const {
    if (addr == 0) return {};
    for (const auto& e : entries(active))
        if (e.address == addr) return e.name;
    return {};
}

uint64_t NameProvider::addressFor(const QString& name, const Provider* active) const {
    if (name.isEmpty()) return 0;
    for (const auto& e : entries(active))
        if (e.name == name) return e.address;
    return 0;
}

} // namespace rcx
