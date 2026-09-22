#pragma once
#include "addressparser.h"
#include "providers/provider.h"
#include "symbolstore.h"

#include <QRegularExpression>

namespace rcx {

// A bare hex / decimal literal — an expression that round-trips identically
// through the canonical "0xHEX" display, so keeping it as the formula would
// only shadow the number. rebaseTo clears the formula for these, and the MCP
// change_base op knows one evaluates with no source attached.
inline bool isBareAddressLiteral(const QString& expr) {
    static const QRegularExpression rx(
        QStringLiteral("^\\s*(?:0[xX][0-9A-Fa-f]+|\\d+)\\s*$"));
    return rx.match(expr).hasMatch();
}

// The one AddressParserCallbacks builder.
//
// Every place that evaluates a base formula against the attached source (the
// command-row live evaluator, the inline base commit, the attach-time
// re-evaluation in attachViaPlugin / selectSource, navigateToFormula) used to
// hand-roll this block, and the copies had drifted: the live evaluator and
// navigateToFormula never wired the kernel-paging trio, so a formula using
// vtop / cr3 / physRead evaluated on commit but was reported as an error while
// typing and refused by Goto and the bookmarks dock. Building the set here
// wires a provider capability once, for every caller. The Goto dialog's copy
// in main.cpp is left as-is for now.
//
// `prov` may be null (no source attached): the result then carries no
// callbacks and the parser reports module / symbol / pointer terms with its
// own "unavailable" error.
inline AddressParserCallbacks makeAddressCallbacks(Provider* prov, int ptrSize) {
    AddressParserCallbacks cbs;
    if (!prov) return cbs;
    cbs.resolveModule = [prov](const QString& name, bool* ok) -> uint64_t {
        uint64_t base = prov->symbolToAddress(name);
        *ok = (base != 0);
        return base;
    };
    cbs.readPointer = [prov, ptrSize](uint64_t addr, bool* ok) -> uint64_t {
        uint64_t val = 0;
        *ok = prov->read(addr, &val, ptrSize);
        return val;
    };
    cbs.resolveIdentifier = [prov](const QString& name, bool* ok) -> uint64_t {
        return SymbolStore::instance().resolve(name, prov, ok);
    };
    if (prov->hasKernelPaging()) {
        cbs.vtop = [prov](uint32_t pid, uint64_t va, bool* ok) -> uint64_t {
            Q_UNUSED(pid);  // the provider already targets one process
            auto r = prov->translateAddress(va);
            *ok = r.valid;
            return r.physical;
        };
        cbs.cr3 = [prov](uint32_t pid, bool* ok) -> uint64_t {
            Q_UNUSED(pid);
            uint64_t cr3 = prov->getCr3();
            *ok = (cr3 != 0);
            return cr3;
        };
        cbs.physRead = [prov](uint64_t physAddr, bool* ok) -> uint64_t {
            auto entries = prov->readPageTable(physAddr, 0, 1);
            *ok = !entries.isEmpty();
            return entries.isEmpty() ? 0 : entries[0];
        };
    }
    return cbs;
}

// Never persist a display label as an expression without verifying it against
// the current source. Demangled names and duplicate module names can be ambiguous.
inline QString durableAddressExpression(uint64_t address, Provider* provider,
                                        int pointerSize, const QString& preferred = {}) {
    const QString absolute = QStringLiteral("0x") + QString::number(address, 16).toUpper();
    if (!provider) return absolute;
    const auto callbacks = makeAddressCallbacks(provider, pointerSize);
    auto resolves = [&](const QString& expression) {
        if (expression.isEmpty() || isBareAddressLiteral(expression)) return false;
        const auto result = AddressParser::evaluate(expression, pointerSize, &callbacks);
        return result.ok && result.value == address;
    };
    if (resolves(preferred)) return preferred;
    const auto& modules = provider->modulesCached();
    for (const auto& module : modules) {
        if (address < module.base || address - module.base >= module.size) continue;
        const QString symbol = SymbolStore::instance().getSymbolForAddress(address, provider);
        if (resolves(symbol)) return symbol;
        const QString expression = QStringLiteral("<%1>+0x%2")
            .arg(module.name, QString::number(address - module.base, 16).toUpper());
        if (resolves(expression)) return expression;
    }
    return absolute;
}

} // namespace rcx
