"""
Fix char/wchar arrays in RCX example files that should be UTF8/UTF16 string nodes.

Converts Array nodes with elementKind UInt16/Int8 to UTF16/UTF8 nodes
when the field name indicates a string buffer.
"""
import json
import re

# ── UInt16 arrays that are strings (WCHAR/wchar_t buffers) ──
# Matched by field name pattern
UTF16_NAME_PATTERNS = re.compile(
    r'(?i)('
    r'Name$|Path$|String$|Text$|FileName|DirName|FilePath|'
    r'szMod|szRes|szLog|szText|szName|'
    r'cFileName|cStreamName|'
    r'CodePage|Caption|Label|Title|Message|'
    r'Description|Desc$|'
    r'SymbolicLink|DisplayName|'
    r'Reason$|Prefix$|'
    r'StaticUnicodeBuffer|'
    r'TemplateFilePath|'
    r'productString|'
    r'NtSystemRoot|NtBuildLab|'
    r'VolumeLabel|'
    r'ImageName$|'
    r'Manufacturer|'
    r'NICName|'
    r'ChipType|DACType|AdapterString|BiosString|'
    r'DeviceId$|'
    r'ComputerName|DomainName|'
    r'NameBuffer|'
    r'EafPlusModuleList|'
    r'TimeZoneKeyName|StandardName|DaylightName|'
    r'LogicalLogFile|'
    r'lcsFilename'
    r')'
)

# ── UInt16 arrays that are NOT strings (exclude explicitly) ──
UTF16_EXCLUDE = {
    'Fill', 'Fill1', 'Fill3', 'Pad',
    'e_res', 'e_res2',
    'UserModeGlobalLogger', 'EtwpSecurityLoggers',
    'FormatInterfaceCodes', 'RootProcNumaNodes',
    'H',  # _ARM64_NT_NEON128 half vector
    'Reserved', 'Spare3',
    'HookId', 'Counts', 'Map',
    'ThreadSeed', 'IdealProcessor', 'IdealNode',
    'PerformanceScoreByClass', 'EfficiencyScoreByClass',
    'FrequencyBucketThresholds', 'QosEquivalencyMasks',
    'StackTableHash',
    'ContextRegisterHookIdMap',
    'ProcessorCount', 'UsableProcessorCount',
}

# ── Int8 arrays that are strings (char/CHAR buffers) ──
UTF8_NAME_PATTERNS = re.compile(
    r'(?i)('
    r'Name$|Path$|FileName|cFileName|'
    r'szMod|szRes|'
    r'CodePage|'
    r'Vector$|'
    r'lcsFilename|'
    r'OEMID|OEMTableID|CreatorID|'
    r'LoggerName|'
    r'NtBuildLab|'
    r'SourceName|'
    r'FRUText'
    r')'
)

# ── Int8 arrays that are NOT strings ──
UTF8_EXCLUDE = {
    'ThreadBasePriority',
    'FileObjectBody',
    'PlaceholderCompatibilityModeReserved',
    'PlaceholderReserved',
}


def process_file(filepath):
    with open(filepath, 'r', encoding='utf-8') as f:
        data = json.load(f)

    nodes = data.get('nodes', [])
    if not nodes:
        return

    utf16_fixed = 0
    utf8_fixed = 0

    for n in nodes:
        if n.get('kind') != 'Array':
            continue

        ek = n.get('elementKind', '')
        name = n.get('name', '')
        alen = n.get('arrayLen', 0)

        if ek == 'UInt16' and alen > 1:
            if name in UTF16_EXCLUDE:
                continue
            if UTF16_NAME_PATTERNS.search(name):
                n['kind'] = 'UTF16'
                n['strLen'] = alen
                # Clean up array-specific fields
                n.pop('elementKind', None)
                n.pop('arrayLen', None)
                utf16_fixed += 1

        elif ek == 'Int8' and alen > 1:
            if name in UTF8_EXCLUDE:
                continue
            if UTF8_NAME_PATTERNS.search(name):
                n['kind'] = 'UTF8'
                n['strLen'] = alen
                n.pop('elementKind', None)
                n.pop('arrayLen', None)
                utf8_fixed += 1

    if utf16_fixed or utf8_fixed:
        with open(filepath, 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False)
        print(f"  {filepath}: fixed {utf16_fixed} UTF16 + {utf8_fixed} UTF8 strings")
    else:
        print(f"  {filepath}: no string arrays to fix")


if __name__ == '__main__':
    files = [
        'src/examples/WinSDK.rcx',
        'src/examples/EPROCESS.rcx',
        'src/examples/KUSER_SHARED_DATA.rcx',
        'src/examples/MMPFN.rcx',
        'src/examples/Vergilius_25H2.rcx',
        'src/examples/t6zm.rcx',
    ]
    for f in files:
        try:
            process_file(f)
        except Exception as e:
            print(f"  ERROR {f}: {e}")
