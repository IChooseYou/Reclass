#pragma once

#include <QString>

namespace rcx {

// Humanise a (possibly mangled) symbol name for display in the Symbols
// panel and the editor's address-→-name comment column. Handles every
// mangling scheme we see in the wild:
//
//   1. MSVC RTTI type descriptors (".?AV<segments>@@", ".?AU...@@",
//      ".?AW...@@", and the no-dot "?A..." variant produced by some
//      tools) — routed through rtti.cpp's in-house parser. Works on
//      every platform.
//
//   2. Itanium ABI mangle ("_Z..." for functions, "_ZN...E" for nested
//      namespaces, "_ZTV..." for vtables, "_ZTI..." for type_info, etc.)
//      — routed through `abi::__cxa_demangle` via rtti.cpp's
//      `demangleItaniumName`. Available wherever the build uses
//      libstdc++ (GCC, Clang, MinGW), which covers all of our supported
//      platforms.
//
//   3. MSVC mangle for functions/methods ("?Method@Class@@QAEXXZ",
//      "??0Class@@..." for constructors, etc.) — routed through
//      `UnDecorateSymbolName` from dbghelp on Windows. On non-Windows
//      we have no equivalent (MSVC PDBs are Windows-specific anyway, and
//      symbols from ELF/Mach-O binaries use Itanium ABI handled above),
//      so the raw name is returned.
//
// Returns an empty QString when the input was already in human-readable
// form (e.g. extern "C" symbols like "GetProcAddress", plain C names,
// data symbols, ...) — callers fall back to the raw name in that case.
QString humanizeSymbolName(const QString& mangled);

} // namespace rcx
