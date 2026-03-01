// Force-included before every raw_pdb translation unit (and consumers).
// PDB_CRT.h forward-declares printf/memcmp/etc with extern "C" __cdecl,
// which conflicts with MinGW's CRT headers (C++ linkage, no __cdecl).
//
// Fix: include the real CRT headers, then include PDB_CRT.h with function
// names macro-renamed to harmless dummies.  This triggers #pragma once so
// no raw_pdb source file ever processes PDB_CRT.h's conflicting declarations.
//
// Guarded with __cplusplus because PUBLIC propagation applies this to C
// sources (fadec) where PDB_CRT.h is irrelevant and <cstdio> doesn't exist.
#ifdef __cplusplus
#include <cstdio>
#include <cstring>

#undef  __cdecl
#define __cdecl

#define printf  _pdb_crt_unused_printf
#define memcmp  _pdb_crt_unused_memcmp
#define memcpy  _pdb_crt_unused_memcpy
#define strlen  _pdb_crt_unused_strlen
#define strcmp  _pdb_crt_unused_strcmp
#include "Foundation/PDB_CRT.h"
#undef printf
#undef memcmp
#undef memcpy
#undef strlen
#undef strcmp
#endif
