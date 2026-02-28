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
};

class AddressParser {
public:
    static AddressParseResult evaluate(const QString& formula, int ptrSize = 8,
                                       const AddressParserCallbacks* cb = nullptr);
    static QString validate(const QString& formula);
};

} // namespace rcx
