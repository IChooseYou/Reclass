#include "symbol_demangle.h"
#include "rtti.h"  // demangleRttiName + demangleItaniumName

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dbghelp.h>
#endif

namespace rcx {

QString humanizeSymbolName(const QString& mangled) {
    if (mangled.isEmpty()) return {};

    // 1. MSVC RTTI type descriptor (".?AV...@@" or "?A..."). Use the
    //    in-house parser — UnDecorateSymbolName emits garbage on these
    //    per the comment in rtti.cpp.
    if (mangled.startsWith(QLatin1String(".?A"))
        || mangled.startsWith(QLatin1String("?A"))) {
        QString d = demangleRttiName(mangled);
        return (d != mangled) ? d : QString();
    }

    // 2. Itanium ABI — works wherever __cxa_demangle is linked (libstdc++
    //    on GCC / Clang / MinGW, which covers all our build targets).
    //    "_Z" (functions), "_ZN" (nested), "_ZT" (vtable/typeinfo), etc.
    if (mangled.startsWith(QLatin1String("_Z"))) {
        QString d = demangleItaniumName(mangled);
        return (d != mangled && !d.isEmpty()) ? d : QString();
    }

    // 3. MSVC function/method mangle ("?...", "_?..."). Windows-only.
#ifdef Q_OS_WIN
    QString trimmed = mangled;
    if (trimmed.startsWith(QLatin1Char('_'))) trimmed = trimmed.mid(1);
    if (!trimmed.startsWith(QLatin1Char('?'))) return {};
    QByteArray utf8 = trimmed.toUtf8();
    char buf[2048];
    const DWORD flags = UNDNAME_NAME_ONLY
                      | UNDNAME_NO_ACCESS_SPECIFIERS
                      | UNDNAME_NO_THISTYPE
                      | UNDNAME_NO_RETURN_UDT_MODEL;
    DWORD n = UnDecorateSymbolName(utf8.constData(), buf, sizeof(buf), flags);
    if (n == 0) return {};
    QString out = QString::fromUtf8(buf, (int)n);
    if (out.isEmpty() || out == trimmed || out == mangled) return {};
    return out;
#else
    // Non-Windows: no equivalent of UnDecorateSymbolName. MSVC-mangled
    // names from imported PDBs stay raw on those platforms. (PDB files
    // are Windows-specific anyway; native ELF/Mach-O symbols come pre-
    // mangled in Itanium ABI which is handled above.)
    return {};
#endif
}

} // namespace rcx
