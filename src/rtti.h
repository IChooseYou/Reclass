#pragma once
#include <QString>
#include <QVector>
#include <cstdint>

namespace rcx {

class Provider;

// MSVC RTTI walking. The runtime type system Microsoft compilers emit is:
//
//   vtable[0]      → first virtual method
//   vtable[-1]     → RTTICompleteObjectLocator* (called "meta")
//   COL.pTypeDescriptor            → RTTITypeDescriptor   (.?AVName@@ string)
//   COL.pClassDescriptor           → RTTIClassHierarchyDescriptor
//   CHD.pBaseClassArray            → array of RTTIBaseClassDescriptor*
//   BCD.pTypeDescriptor            → TypeDescriptor for that base
//
// On x64, all "pointers" inside RTTI are 32-bit RVAs from the module image
// base (COL stores its own image base too — we use that to relocate). On x86,
// they're absolute pointers.
//
// This walker is byte-level and uses only Provider::read* + module enumeration.
// It compiles on every platform — it just won't find anything useful on
// non-MSVC binaries (Itanium ABI used by GCC/Clang on Linux/macOS is a
// different layout entirely; that's a future feature). To keep the parser
// itself testable on Linux/macOS/CI, the synthetic-RTTI test feeds bytes into
// a BufferProvider — no Windows dependency.

struct RttiBaseClass {
    QString  rawName;        // ".?AVFoo@@"
    QString  demangledName;  // "Foo"
    int      depth = 0;      // 0 = self, 1 = direct base, 2+ = grandparent
};

struct RttiVirtualMethod {
    int      slot = 0;       // index in vtable
    uint64_t address = 0;    // absolute address of method
    QString  symbol;         // resolved via SymbolStore (empty when no PDB loaded)
};

struct RttiInfo {
    bool     ok = false;
    QString  error;
    QString  abi;                 // "MSVC" / "Itanium" — empty when ok=false
    uint64_t vtableAddress = 0;
    uint64_t imageBase = 0;       // module that owns the COL (MSVC) / type_info (Itanium)
    QString  moduleName;
    uint64_t completeLocator = 0; // MSVC: VA of COL; Itanium: VA of type_info
    int      offset = 0;          // MSVC: COL.offset; Itanium: offset_to_top
    QString  rawName;             // top class raw name (MSVC ".?AVFoo@@" or Itanium "3Foo")
    QString  demangledName;       // top class demangled name ("Foo")
    QVector<RttiBaseClass>     bases;     // class hierarchy (excluding self if you prefer)
    QVector<RttiVirtualMethod> vtable;    // vtable entries
};

// Walk RTTI starting from a vtable address. The Provider must be able to
// read the vtable bytes plus its [-8] qword (or [-4] on x86) plus the COL
// + CHD + BCD chain. Returns RttiInfo with ok=false + an error string when
// the structure doesn't match MSVC layout (stripped binary, GCC binary,
// arbitrary memory pointer, etc).
//
// pointerSize: 4 (x86) or 8 (x64). When 8, RTTI fields are 32-bit RVAs.
//              When 4, they're absolute pointers.
// maxVtableSlots: cap on how many vtable slots to enumerate (default 64).
RttiInfo walkRtti(const Provider& prov, uint64_t vtableAddr,
                  int pointerSize = 8, int maxVtableSlots = 64);

// Demangle an MSVC RTTI mangled name (e.g. ".?AVFoo@Bar@@" → "Bar::Foo").
// On Windows this calls UnDecorateSymbolName via dbghelp.
// On other platforms we run a small built-in parser that handles the common
// .?AV / .?AU / nested namespace forms — enough for testing and for the
// occasional MSVC-built blob someone analyses on Linux.
QString demangleRttiName(const QString& mangled);

// Walk Itanium ABI RTTI from a vtable address. Used by GCC/Clang/MinGW
// binaries (Linux, macOS, MinGW-built Windows binaries). Itanium layout:
//   vtable[-16] = offset_to_top (ptrdiff_t, 0 for most-derived subobject)
//   vtable[-8]  = type_info*    (points at __cxxabiv1 type_info structure)
//   type_info[0] = vtable ptr (one of the three __cxxabiv1 type_info classes)
//   type_info[8] = __name (NUL-terminated Itanium-mangled name string)
//
// v1 returns the demangled class name only; subclass discrimination
// (__class / __si / __vmi) for hierarchy walking is a future extension.
RttiInfo walkRttiItanium(const Provider& prov, uint64_t vtableAddr,
                         int pointerSize = 8, int maxVtableSlots = 64);

// Demangle an Itanium ABI mangled type name (e.g. "N3Bar3FooE" → "Bar::Foo").
// On GCC/Clang/MinGW (anything with `<cxxabi.h>`) this calls
// abi::__cxa_demangle and supports full templates. On other compilers
// (MSVC) it falls back to a minimal length-prefix parser handling the
// `<n><chars>` and `N<segments>E` forms — enough for class-name display
// and for the headless test suite to run anywhere.
QString demangleItaniumName(const QString& mangled);

// Locate the module containing a given absolute address by walking
// Provider::enumerateModules(). Returns empty when not found.
struct OwningModule {
    QString  name;
    QString  fullPath;
    uint64_t base = 0;
    uint64_t size = 0;
    bool     valid = false;
};
OwningModule findOwningModule(const Provider& prov, uint64_t addr);

} // namespace rcx
