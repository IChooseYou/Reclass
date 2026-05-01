#include "rtti.h"
#include "providers/provider.h"
#include "symbolstore.h"
#include <QStringList>
#include <QRegularExpression>
#include <memory>
#include <cstdlib>

// __cxa_demangle is provided by libstdc++ on GCC/Clang and by MinGW.
// MSVC has no equivalent — we ship a fallback parser for the common forms.
#if defined(__GNUG__)
#  include <cxxabi.h>
#  define RCX_HAVE_CXA_DEMANGLE 1
#endif

namespace rcx {

// ── Module lookup ──

OwningModule findOwningModule(const Provider& prov, uint64_t addr) {
    OwningModule out;
    auto mods = prov.enumerateModules();
    for (const auto& m : mods) {
        if (addr >= m.base && addr < m.base + m.size) {
            out.name     = m.name;
            out.fullPath = m.fullPath;
            out.base     = m.base;
            out.size     = m.size;
            out.valid    = true;
            return out;
        }
    }
    return out;
}

// ── Demangler ──

QString demangleRttiName(const QString& mangled) {
    if (mangled.isEmpty()) return {};

    // The MSVC RTTI mangled-name format is well defined and small enough to
    // parse in-house — far simpler than wrestling with dbghelp's
    // UnDecorateSymbolName, which is designed for *full symbols* and emits
    // garbage like "?? const Foo::&" when fed bare ".?AV" type strings.
    // The Itanium-ABI demangler on Linux/macOS doesn't recognise this form
    // either, so a portable in-house parser is the right call.
    //
    // Format:   .?AV<segments>@@   (V = class, U = struct, W = enum, X = void)
    //   .?AVFoo@@           → Foo
    //   .?AVBar@Foo@@       → Foo::Bar           (segments listed inner→outer)
    //   .?AUMyStruct@N@@    → N::MyStruct
    //
    // Anything that doesn't fit the form is returned verbatim — callers (UI
    // text, "Copy as Tree" reports) tolerate it and tests assert on it.
    if (!mangled.startsWith(QStringLiteral(".?A")) &&
        !mangled.startsWith(QStringLiteral("?A"))) {
        return mangled;
    }
    QString s = mangled;
    if (s.startsWith('.')) s = s.mid(1);  // drop leading '.'
    if (s.startsWith('?')) s = s.mid(1);  // drop '?'
    if (s.size() < 3) return mangled;
    // s now starts with kind char ('A') + class-kind char (V/U/W/X)
    // Skip the 2-char prefix:
    QString body = s.mid(2);
    int term = body.indexOf(QStringLiteral("@@"));
    if (term < 0) return mangled;
    QString segments = body.left(term);
    QStringList parts = segments.split('@', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return mangled;
    // Segments are listed inner-most first; reverse for outer::inner display.
    std::reverse(parts.begin(), parts.end());
    return parts.join(QStringLiteral("::"));
}

// ── RTTI walker helpers ──

namespace {

// Resolve an RTTI "pointer field" — 32-bit RVA on x64, absolute on x86.
inline uint64_t rttiResolve(uint32_t field, uint64_t imageBase, int ptrSize) {
    if (ptrSize == 8) return imageBase + (uint64_t)field;
    return (uint64_t)field;
}

// Read a 32-bit field at addr.  Returns 0 if read fails.
inline uint32_t readU32At(const Provider& p, uint64_t addr, bool* ok) {
    uint32_t v = 0;
    *ok = p.read(addr, &v, 4);
    return v;
}

// Read a string starting at addr until NUL (max len cap).
QString readCString(const Provider& p, uint64_t addr, int maxLen = 512) {
    QByteArray out;
    out.reserve(64);
    for (int i = 0; i < maxLen; ++i) {
        uint8_t b = 0;
        if (!p.read(addr + i, &b, 1)) break;
        if (b == 0) break;
        out.append((char)b);
    }
    return QString::fromUtf8(out);
}

} // anon namespace

RttiInfo walkRtti(const Provider& prov, uint64_t vtableAddr,
                  int pointerSize, int maxVtableSlots) {
    RttiInfo info;
    info.vtableAddress = vtableAddr;

    if (pointerSize != 4 && pointerSize != 8) {
        info.error = QStringLiteral("invalid pointer size");
        return info;
    }

    // [vtable - ptrSize] holds a pointer to the COL.
    // On x64 it's an absolute VA inside the module image; on x86 too.
    uint64_t metaPtrAddr = vtableAddr - (uint64_t)pointerSize;
    uint64_t colAddr = 0;
    bool ok = false;
    if (pointerSize == 8) {
        colAddr = prov.readAs<uint64_t>(metaPtrAddr);
        ok = prov.read(metaPtrAddr, &colAddr, 8);
    } else {
        uint32_t v = 0;
        ok = prov.read(metaPtrAddr, &v, 4);
        colAddr = v;
    }
    if (!ok || colAddr == 0) {
        info.error = QStringLiteral("could not read meta pointer at vtable[-1]");
        return info;
    }
    info.completeLocator = colAddr;

    // Find which module owns the COL (we use its image base for RVA decoding).
    OwningModule owner = findOwningModule(prov, colAddr);
    uint64_t imageBase = 0;
    if (owner.valid) {
        info.moduleName = owner.name;
        imageBase = owner.base;
    } else if (pointerSize == 8) {
        // No enumerable modules — fall back to the COL's self-image-base
        // field (offset 0x14).  Lets synthetic-bytes tests run.
        bool ib_ok = false;
        uint32_t ib = readU32At(prov, colAddr + 0x14, &ib_ok);
        if (ib_ok && ib != 0) imageBase = (uint64_t)ib;
    }
    info.imageBase = imageBase;

    // RTTICompleteObjectLocator layout (x64):
    //   +0x00 DWORD  signature        (must be 0 or 1; 1 = relative)
    //   +0x04 DWORD  offset           (offset of subobject in complete object)
    //   +0x08 DWORD  cdOffset         (constructor displacement)
    //   +0x0C DWORD  pTypeDescriptor  (RVA on x64, abs on x86)
    //   +0x10 DWORD  pClassHierarchy  (RVA on x64, abs on x86)
    //   +0x14 DWORD  pSelf            (RVA back to this COL — x64 only)
    bool sig_ok = false;
    uint32_t sig = readU32At(prov, colAddr + 0x00, &sig_ok);
    if (!sig_ok) {
        info.error = QStringLiteral("could not read COL signature");
        return info;
    }
    if (sig != 0 && sig != 1) {
        info.error = QStringLiteral("COL signature 0x%1 not 0/1 — not MSVC RTTI")
            .arg(sig, 0, 16);
        return info;
    }
    bool off_ok = false;
    info.offset = (int)readU32At(prov, colAddr + 0x04, &off_ok);

    bool td_ok = false, chd_ok = false;
    uint32_t tdField  = readU32At(prov, colAddr + 0x0C, &td_ok);
    uint32_t chdField = readU32At(prov, colAddr + 0x10, &chd_ok);
    if (!td_ok || !chd_ok) {
        info.error = QStringLiteral("could not read COL TypeDescriptor / CHD fields");
        return info;
    }

    uint64_t tdAddr  = rttiResolve(tdField,  imageBase, pointerSize);
    uint64_t chdAddr = rttiResolve(chdField, imageBase, pointerSize);

    // RTTITypeDescriptor:
    //   +0x00 ptr to type_info vtable (we don't need this)
    //   +ptrSize  ptr  spare (0)
    //   +2*ptrSize  char name[]  (the .?AV... string, NUL-terminated)
    int nameOff = 2 * pointerSize;
    info.rawName = readCString(prov, tdAddr + nameOff);
    info.demangledName = demangleRttiName(info.rawName);
    if (info.rawName.isEmpty()) {
        info.error = QStringLiteral("type descriptor name empty");
        return info;
    }

    // RTTIClassHierarchyDescriptor:
    //   +0x00 DWORD signature
    //   +0x04 DWORD attributes
    //   +0x08 DWORD numBaseClasses
    //   +0x0C DWORD/RVA pBaseClassArray  → array of BCD pointers
    bool nb_ok = false, bca_ok = false;
    uint32_t numBases = readU32At(prov, chdAddr + 0x08, &nb_ok);
    uint32_t bcaField = readU32At(prov, chdAddr + 0x0C, &bca_ok);
    if (!nb_ok || !bca_ok) {
        info.error = QStringLiteral("could not read CHD");
        return info;
    }
    if (numBases > 256) {
        info.error = QStringLiteral("CHD.numBaseClasses unreasonably large (%1) — not RTTI")
            .arg(numBases);
        return info;
    }

    uint64_t bcaAddr = rttiResolve(bcaField, imageBase, pointerSize);
    // Each entry in the array is a 32-bit RVA on x64, ptr on x86.
    int entrySize = (pointerSize == 8) ? 4 : pointerSize;
    for (uint32_t i = 0; i < numBases; ++i) {
        bool e_ok = false;
        uint32_t bcdField = (pointerSize == 8)
            ? readU32At(prov, bcaAddr + i * entrySize, &e_ok)
            : readU32At(prov, bcaAddr + i * entrySize, &e_ok);
        if (!e_ok) break;
        uint64_t bcdAddr = rttiResolve(bcdField, imageBase, pointerSize);

        // RTTIBaseClassDescriptor:
        //   +0x00 RVA pTypeDescriptor
        //   +0x04 DWORD numContainedBases (depth-ish)
        //   +0x08 PMD where { mdisp, pdisp, vdisp }
        //   +0x14 DWORD attributes
        bool tdf_ok = false;
        uint32_t bcdTd = readU32At(prov, bcdAddr + 0x00, &tdf_ok);
        if (!tdf_ok) continue;
        uint64_t bcdTdAddr = rttiResolve(bcdTd, imageBase, pointerSize);
        QString rawBase = readCString(prov, bcdTdAddr + nameOff);

        RttiBaseClass b;
        b.rawName = rawBase;
        b.demangledName = demangleRttiName(rawBase);
        b.depth = (int)i;
        info.bases.append(b);
    }

    // Vtable entries — stop when we hit a non-readable / null slot or pass
    // the cap. Each slot is a code pointer of pointerSize bytes.
    for (int slot = 0; slot < maxVtableSlots; ++slot) {
        uint64_t entryAddr = vtableAddr + (uint64_t)slot * pointerSize;
        uint64_t target = 0;
        bool tok = false;
        if (pointerSize == 8) {
            tok = prov.read(entryAddr, &target, 8);
        } else {
            uint32_t v = 0;
            tok = prov.read(entryAddr, &v, 4);
            target = v;
        }
        if (!tok) break;
        if (target == 0) break;
        // Heuristic: real virtual methods point into executable memory of
        // some module. If we can't enumerate modules (synthetic provider),
        // we still add the slot and let the caller decide.
        bool inSomeModule = false;
        if (!owner.valid) {
            inSomeModule = true;  // can't tell; trust the input
        } else {
            auto t = findOwningModule(prov, target);
            inSomeModule = t.valid;
        }
        if (!inSomeModule) break;

        RttiVirtualMethod m;
        m.slot = slot;
        m.address = target;
        m.symbol  = SymbolStore::instance().getSymbolForAddress(target, &prov);
        info.vtable.append(m);
    }

    info.ok = true;
    info.abi = QStringLiteral("MSVC");
    return info;
}

// ── Itanium ABI demangler ──

QString demangleItaniumName(const QString& mangled) {
    if (mangled.isEmpty()) return {};

#ifdef RCX_HAVE_CXA_DEMANGLE
    // Preferred path: __cxa_demangle handles every Itanium mangle including
    // templates, function pointers, lambdas, etc. Caller frees the result.
    QByteArray utf8 = mangled.toUtf8();
    int status = 0;
    std::unique_ptr<char, decltype(&std::free)> demangled(
        abi::__cxa_demangle(utf8.constData(), nullptr, nullptr, &status),
        std::free);
    if (status == 0 && demangled) {
        QString out = QString::fromUtf8(demangled.get());
        // __cxa_demangle of a typeinfo-name returns just the type as text.
        return out.trimmed();
    }
    // Fall through to the in-house parser if cxa_demangle rejects (e.g. when
    // fed a name fragment it doesn't recognise).
#endif

    // Fallback: handles the simplest Itanium type-name forms used by the
    // RTTI walker. Class type names look like:
    //   3Foo                        → Foo
    //   N3Bar3FooE                  → Bar::Foo
    //   St9type_info                → std::type_info  (St shorthand)
    // Anything we don't understand is returned verbatim.
    auto parseLength = [](const QString& s, int& pos) -> int {
        int n = 0;
        bool any = false;
        while (pos < s.size() && s[pos].isDigit()) {
            n = n * 10 + (s[pos].unicode() - '0');
            ++pos; any = true;
        }
        return any ? n : -1;
    };

    QString s = mangled;
    int p = 0;

    auto consumeSegment = [&](QStringList& parts) -> bool {
        if (p >= s.size()) return false;
        // "St" → "std" segment
        if (s[p] == 'S' && p + 1 < s.size() && s[p + 1] == 't') {
            parts << QStringLiteral("std");
            p += 2;
            return true;
        }
        int len = parseLength(s, p);
        if (len <= 0 || p + len > s.size()) return false;
        parts << s.mid(p, len);
        p += len;
        return true;
    };

    QStringList parts;
    if (s[0] == 'N') {
        ++p;
        while (p < s.size() && s[p] != 'E') {
            if (!consumeSegment(parts)) return mangled;
        }
        if (p >= s.size() || s[p] != 'E') return mangled;
    } else {
        if (!consumeSegment(parts)) return mangled;
    }
    if (parts.isEmpty()) return mangled;
    return parts.join(QStringLiteral("::"));
}

// ── Itanium ABI RTTI walker ──

RttiInfo walkRttiItanium(const Provider& prov, uint64_t vtableAddr,
                         int pointerSize, int maxVtableSlots) {
    RttiInfo info;
    info.vtableAddress = vtableAddr;

    if (pointerSize != 4 && pointerSize != 8) {
        info.error = QStringLiteral("invalid pointer size");
        return info;
    }

    // ── 1. type_info* lives at vtable[-pointerSize] ──
    uint64_t tiPtrAddr = vtableAddr - (uint64_t)pointerSize;
    uint64_t tiAddr = 0;
    bool ok = false;
    if (pointerSize == 8) {
        ok = prov.read(tiPtrAddr, &tiAddr, 8);
    } else {
        uint32_t v = 0;
        ok = prov.read(tiPtrAddr, &v, 4);
        tiAddr = v;
    }
    if (!ok || tiAddr == 0) {
        info.error = QStringLiteral("could not read type_info pointer at vtable[-1]");
        return info;
    }

    // ── 2. type_info must live in a known module ──
    OwningModule tiOwner = findOwningModule(prov, tiAddr);
    if (!tiOwner.valid) {
        info.error = QStringLiteral("type_info pointer outside any module");
        return info;
    }
    info.imageBase  = tiOwner.base;
    info.moduleName = tiOwner.name;
    info.completeLocator = tiAddr;

    // ── 3. offset_to_top is one extra slot above type_info ──
    // For the most-derived class subobject, this is 0. Validate the
    // magnitude is plausibly a small ptrdiff_t (tighter filter than
    // "any 8 bytes" — rejects most random data).
    int64_t offsetToTop = 0;
    {
        uint64_t addr = vtableAddr - (uint64_t)pointerSize * 2;
        if (pointerSize == 8) {
            int64_t v = 0;
            if (prov.read(addr, &v, 8)) offsetToTop = v;
        } else {
            int32_t v = 0;
            if (prov.read(addr, &v, 4)) offsetToTop = v;
        }
    }
    // ptrdiff_t magnitudes inside a real C++ object are bounded by the
    // largest plausible class layout — anything past 16 MB is junk.
    if (offsetToTop > 0x1000000 || offsetToTop < -0x1000000) {
        info.error = QStringLiteral("offset_to_top implausible — not Itanium RTTI");
        return info;
    }
    info.offset = (int)offsetToTop;

    // ── 4. type_info[0] = vtable ptr (one of __cxxabiv1's three) ──
    // Just verify it points into a module; we don't try to discriminate
    // __class / __si / __vmi without symbol info (v1 limitation).
    uint64_t tiVtable = 0;
    if (pointerSize == 8) {
        if (!prov.read(tiAddr, &tiVtable, 8)) {
            info.error = QStringLiteral("could not read type_info vtable ptr");
            return info;
        }
    } else {
        uint32_t v = 0;
        if (!prov.read(tiAddr, &v, 4)) {
            info.error = QStringLiteral("could not read type_info vtable ptr");
            return info;
        }
        tiVtable = v;
    }
    if (tiVtable == 0 || !findOwningModule(prov, tiVtable).valid) {
        info.error = QStringLiteral("type_info vtable not in any module");
        return info;
    }

    // ── 5. type_info[8] = char* __name ──
    uint64_t namePtr = 0;
    uint64_t nameAddr = tiAddr + (uint64_t)pointerSize;
    if (pointerSize == 8) {
        if (!prov.read(nameAddr, &namePtr, 8)) {
            info.error = QStringLiteral("could not read __name pointer");
            return info;
        }
    } else {
        uint32_t v = 0;
        if (!prov.read(nameAddr, &v, 4)) {
            info.error = QStringLiteral("could not read __name pointer");
            return info;
        }
        namePtr = v;
    }
    if (namePtr == 0 || !findOwningModule(prov, namePtr).valid) {
        info.error = QStringLiteral("__name pointer not in any module");
        return info;
    }

    // ── 6. Read NUL-terminated mangled name ──
    QByteArray nameBytes;
    nameBytes.reserve(64);
    for (int i = 0; i < 256; ++i) {
        uint8_t b = 0;
        if (!prov.read(namePtr + (uint64_t)i, &b, 1)) break;
        if (b == 0) break;
        if (b < 0x20 || b > 0x7E) { nameBytes.clear(); break; }  // non-printable → reject
        nameBytes.append((char)b);
    }
    if (nameBytes.size() < 2) {
        info.error = QStringLiteral("__name string empty or non-printable");
        return info;
    }
    // First char of an Itanium type-name is a digit (length prefix), 'N'
    // (nested), 'S' (substitution / std::), 'P' (pointer), or similar.
    // GCC on platforms where __GXX_TYPEINFO_EQUALITY_INLINE is 0 (notably
    // MinGW and historical macOS) prepends '*' to mark vague-linkage
    // types so type_info::operator== falls back to pointer-identity
    // comparison. The '*' is part of the stored string but NOT part of
    // the mangle — strip it before validating + demangling.
    int validateOff = 0;
    if (nameBytes[0] == '*') validateOff = 1;
    if (validateOff >= nameBytes.size()) {
        info.error = QStringLiteral("__name is just a vague-linkage marker");
        return info;
    }
    QChar c0 = QChar((uchar)nameBytes[validateOff]);
    if (!(c0.isDigit() || c0 == QChar('N') || c0 == QChar('S')
          || c0 == QChar('P') || c0 == QChar('K') || c0 == QChar('R'))) {
        info.error = QStringLiteral("__name doesn't start with Itanium mangle marker");
        return info;
    }

    // Store the raw bytes including the '*' prefix so the browser dialog
    // can show what was actually in memory; pass the prefix-stripped form
    // to the demangler so it sees a valid Itanium mangle.
    info.rawName       = QString::fromLatin1(nameBytes);
    info.demangledName = demangleItaniumName(
        QString::fromLatin1(nameBytes.mid(validateOff)));

    // ── 7. Vtable enumeration (best-effort, mirrors MSVC walker) ──
    for (int slot = 0; slot < maxVtableSlots; ++slot) {
        uint64_t entryAddr = vtableAddr + (uint64_t)slot * (uint64_t)pointerSize;
        uint64_t target = 0;
        bool tok = false;
        if (pointerSize == 8) {
            tok = prov.read(entryAddr, &target, 8);
        } else {
            uint32_t v = 0;
            tok = prov.read(entryAddr, &v, 4);
            target = v;
        }
        if (!tok) break;
        if (target == 0) break;
        if (!findOwningModule(prov, target).valid) break;

        RttiVirtualMethod m;
        m.slot = slot;
        m.address = target;
        m.symbol = SymbolStore::instance().getSymbolForAddress(target, &prov);
        info.vtable.append(m);
    }

    info.ok  = true;
    info.abi = QStringLiteral("Itanium");
    return info;
}

} // namespace rcx
