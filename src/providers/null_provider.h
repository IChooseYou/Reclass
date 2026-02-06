#pragma once
#include "provider.h"

namespace rcx {

class NullProvider : public Provider {
public:
    int  size() const override { return 0; }
    bool read(uint64_t, void*, int) const override { return false; }
    // name() returns "" via base default -- triggers <Select Source> in command row
    // kind() returns "File" via base default
};

} // namespace rcx
