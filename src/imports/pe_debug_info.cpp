#include "pe_debug_info.h"
#include "../providers/provider.h"
#include <cstring>

namespace rcx {

// Minimal PE structures (no Windows SDK dependency)
#pragma pack(push, 1)
struct DosHeader {
    uint16_t e_magic;     // 'MZ'
    uint8_t  pad[58];
    int32_t  e_lfanew;    // offset to PE signature
};

struct CoffHeader {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
};

struct DataDirectory {
    uint32_t VirtualAddress;
    uint32_t Size;
};

// Only the fields we need from the optional header
struct OptionalHeader32 {
    uint16_t Magic;  // 0x10b = PE32, 0x20b = PE32+
    uint8_t  pad[90];
    uint32_t NumberOfRvaAndSizes;
    // DataDirectory[0] = Export, [1] = Import, ... [6] = Debug
};

struct OptionalHeader64 {
    uint16_t Magic;  // 0x20b = PE32+
    uint8_t  pad[106];
    uint32_t NumberOfRvaAndSizes;
};

struct DebugDirectory {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t Type;
    uint32_t SizeOfData;
    uint32_t AddressOfRawData;  // RVA when loaded
    uint32_t PointerToRawData;  // file offset (not used for memory reads)
};

struct CvInfoPdb70 {
    uint32_t Signature;  // 'RSDS'
    uint8_t  Guid[16];
    uint32_t Age;
    // char PdbFileName[] follows
};
#pragma pack(pop)

static constexpr uint16_t kMZ      = 0x5A4D;
static constexpr uint32_t kPE      = 0x00004550;
static constexpr uint16_t kPE32    = 0x10b;
static constexpr uint16_t kPE32P   = 0x20b;
static constexpr uint32_t kRSDS    = 0x53445352;
static constexpr uint32_t kDebugType_CodeView = 2;

static QString guidToString(const uint8_t guid[16]) {
    // Windows GUID is mixed-endian: Data1(4B LE), Data2(2B LE), Data3(2B LE), Data4(8B sequential)
    // MS symbol server expects native integer values for Data1/2/3, sequential for Data4
    uint32_t d1; memcpy(&d1, guid, 4);
    uint16_t d2; memcpy(&d2, guid + 4, 2);
    uint16_t d3; memcpy(&d3, guid + 6, 2);
    QString s = QStringLiteral("%1%2%3")
        .arg(d1, 8, 16, QLatin1Char('0'))
        .arg(d2, 4, 16, QLatin1Char('0'))
        .arg(d3, 4, 16, QLatin1Char('0'));
    for (int i = 8; i < 16; i++)
        s += QStringLiteral("%1").arg(guid[i], 2, 16, QLatin1Char('0'));
    return s.toUpper();
}

PdbDebugInfo extractPdbDebugInfo(const Provider& prov, uint64_t moduleBase) {
    PdbDebugInfo result;

    // Read DOS header
    DosHeader dos;
    if (!prov.read(moduleBase, &dos, sizeof(dos)))
        return result;
    if (dos.e_magic != kMZ)
        return result;

    uint64_t peOffset = moduleBase + dos.e_lfanew;

    // Read PE signature
    uint32_t peSig = 0;
    if (!prov.read(peOffset, &peSig, 4))
        return result;
    if (peSig != kPE)
        return result;

    // Read COFF header
    uint64_t coffOffset = peOffset + 4;
    CoffHeader coff;
    if (!prov.read(coffOffset, &coff, sizeof(coff)))
        return result;

    // Read optional header magic to determine PE32 vs PE32+
    uint64_t optOffset = coffOffset + sizeof(CoffHeader);
    uint16_t optMagic = 0;
    if (!prov.read(optOffset, &optMagic, 2))
        return result;

    // Locate debug data directory (index 6)
    uint32_t numRvaAndSizes = 0;
    uint64_t dataDirsOffset = 0;

    if (optMagic == kPE32) {
        // PE32: NumberOfRvaAndSizes at offset 92, data dirs at offset 96
        if (!prov.read(optOffset + 92, &numRvaAndSizes, 4))
            return result;
        dataDirsOffset = optOffset + 96;
    } else if (optMagic == kPE32P) {
        // PE32+: NumberOfRvaAndSizes at offset 108, data dirs at offset 112
        if (!prov.read(optOffset + 108, &numRvaAndSizes, 4))
            return result;
        dataDirsOffset = optOffset + 112;
    } else {
        return result;
    }

    if (numRvaAndSizes <= 6)
        return result; // no debug directory

    DataDirectory debugDir;
    if (!prov.read(dataDirsOffset + 6 * sizeof(DataDirectory), &debugDir, sizeof(debugDir)))
        return result;

    if (debugDir.VirtualAddress == 0 || debugDir.Size == 0)
        return result;

    // Read debug directory entries
    int numEntries = debugDir.Size / sizeof(DebugDirectory);
    for (int i = 0; i < numEntries; i++) {
        DebugDirectory entry;
        uint64_t entryAddr = moduleBase + debugDir.VirtualAddress + i * sizeof(DebugDirectory);
        if (!prov.read(entryAddr, &entry, sizeof(entry)))
            continue;

        if (entry.Type != kDebugType_CodeView)
            continue;

        // Read CodeView info (RSDS)
        if (entry.AddressOfRawData == 0 || entry.SizeOfData < sizeof(CvInfoPdb70) + 1)
            continue;

        CvInfoPdb70 cv;
        uint64_t cvAddr = moduleBase + entry.AddressOfRawData;
        if (!prov.read(cvAddr, &cv, sizeof(cv)))
            continue;

        if (cv.Signature != kRSDS)
            continue;

        // Read PDB filename (null-terminated string after the struct)
        int nameMaxLen = entry.SizeOfData - sizeof(CvInfoPdb70);
        if (nameMaxLen > 260) nameMaxLen = 260;
        char nameBuf[261] = {};
        if (!prov.read(cvAddr + sizeof(CvInfoPdb70), nameBuf, nameMaxLen))
            continue;
        nameBuf[nameMaxLen] = '\0';

        result.pdbName = QString::fromLatin1(nameBuf);
        // Extract just the filename if it contains a path
        int lastSlash = result.pdbName.lastIndexOf('\\');
        if (lastSlash >= 0)
            result.pdbName = result.pdbName.mid(lastSlash + 1);
        int lastFwdSlash = result.pdbName.lastIndexOf('/');
        if (lastFwdSlash >= 0)
            result.pdbName = result.pdbName.mid(lastFwdSlash + 1);

        result.guidString = guidToString(cv.Guid);
        result.age = cv.Age;
        result.valid = true;
        return result;
    }

    return result;
}

} // namespace rcx
