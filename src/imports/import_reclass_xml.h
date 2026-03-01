#pragma once
#include "core.h"

namespace rcx {

// Import a ReClass XML file (.reclass, .MemeCls, etc.) into a NodeTree.
// Supports ReClassEx, MemeClsEx, ReClass 2011/2013/2016 XML formats.
// pointerSize: 4 for 32-bit targets, 8 for 64-bit (default).
// Returns an empty NodeTree on failure; populates errorMsg if non-null.
NodeTree importReclassXml(const QString& filePath, QString* errorMsg = nullptr,
                          int pointerSize = 8);

} // namespace rcx
