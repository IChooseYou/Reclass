#pragma once
#include <QString>
#include <cstdint>
#include <functional>

namespace rcx {

struct AddressParseResult {
    bool ok;
    uint64_t value;
    QString error;
    int errorPos;
};

struct AddressParserCallbacks {
    std::function<uint64_t(const QString& name, bool* ok)> resolveModule;
    std::function<uint64_t(uint64_t addr, bool* ok)>       readPointer;
    std::function<uint64_t(const QString& name, bool* ok)> resolveIdentifier;

    // Kernel paging functions (optional — only wired when kernel provider active)
    std::function<uint64_t(uint32_t pid, uint64_t va, bool* ok)> vtop;
    std::function<uint64_t(uint32_t pid, bool* ok)>               cr3;
    std::function<uint64_t(uint64_t physAddr, bool* ok)>          physRead;
};

class AddressParser {
public:
    static AddressParseResult evaluate(const QString& formula, int ptrSize = 8,
                                       const AddressParserCallbacks* cb = nullptr);
    static QString validate(const QString& formula);
};

} // namespace rcx
