#pragma once
#include "core.h"

namespace rcx {

// Export a NodeTree to ReClass .NET / ReClassEx compatible XML format.
// Returns true on success; populates errorMsg on failure if non-null.
bool exportReclassXml(const NodeTree& tree, const QString& filePath, QString* errorMsg = nullptr);

} // namespace rcx
