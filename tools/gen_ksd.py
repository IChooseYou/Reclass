#!/usr/bin/env python3
"""Generate full KUSER_SHARED_DATA (0xA80 bytes) tree.apply JSON from exact SDK offsets."""
import json

P = "$0"  # parent placeholder for the root struct

fields = [
    # ── Time data ──
    (0x000, "UInt32",  "TickCountLowDeprecated"),
    (0x004, "UInt32",  "TickCountMultiplier"),
    # InterruptTime: _KSYSTEM_TIME (3x ULONG)
    (0x008, "UInt32",  "InterruptTime.LowPart"),
    (0x00C, "UInt32",  "InterruptTime.High1Time"),
    (0x010, "UInt32",  "InterruptTime.High2Time"),
    # SystemTime: _KSYSTEM_TIME
    (0x014, "UInt32",  "SystemTime.LowPart"),
    (0x018, "UInt32",  "SystemTime.High1Time"),
    (0x01C, "UInt32",  "SystemTime.High2Time"),
    # TimeZoneBias: _KSYSTEM_TIME
    (0x020, "UInt32",  "TimeZoneBias.LowPart"),
    (0x024, "UInt32",  "TimeZoneBias.High1Time"),
    (0x028, "UInt32",  "TimeZoneBias.High2Time"),

    # ── Image ──
    (0x02C, "UInt16",  "ImageNumberLow"),
    (0x02E, "UInt16",  "ImageNumberHigh"),

    # ── NtSystemRoot WCHAR[260] = 520 bytes ──
    (0x030, "Utf16",   "NtSystemRoot", {"strLen": 260}),

    # ── Misc ──
    (0x238, "UInt32",  "MaxStackTraceDepth"),
    (0x23C, "UInt32",  "CryptoExponent"),
    (0x240, "UInt32",  "TimeZoneId"),
    (0x244, "UInt32",  "LargePageMinimum"),
    (0x248, "UInt32",  "AitSamplingValue"),
    (0x24C, "UInt32",  "AppCompatFlag"),
    (0x250, "UInt64",  "RNGSeedVersion"),
    (0x258, "UInt32",  "GlobalValidationRunlevel"),
    (0x25C, "Int32",   "TimeZoneBiasStamp"),

    # ── Build / product info ──
    (0x260, "UInt32",  "NtBuildNumber"),
    (0x264, "UInt32",  "NtProductType"),
    (0x268, "UInt8",   "ProductTypeIsValid"),
    (0x269, "UInt8",   "Reserved0"),
    (0x26A, "UInt16",  "NativeProcessorArchitecture"),
    (0x26C, "UInt32",  "NtMajorVersion"),
    (0x270, "UInt32",  "NtMinorVersion"),

    # ── ProcessorFeatures UCHAR[64] — 8 x Hex64 ──
    (0x274, "Hex64",   "ProcessorFeatures[0..7]"),
    (0x27C, "Hex64",   "ProcessorFeatures[8..15]"),
    (0x284, "Hex64",   "ProcessorFeatures[16..23]"),
    (0x28C, "Hex64",   "ProcessorFeatures[24..31]"),
    (0x294, "Hex64",   "ProcessorFeatures[32..39]"),
    (0x29C, "Hex64",   "ProcessorFeatures[40..47]"),
    (0x2A4, "Hex64",   "ProcessorFeatures[48..55]"),
    (0x2AC, "Hex64",   "ProcessorFeatures[56..63]"),

    # ── System info ──
    (0x2B4, "UInt32",  "Reserved1"),
    (0x2B8, "UInt32",  "Reserved3"),
    (0x2BC, "UInt32",  "TimeSlip"),
    (0x2C0, "UInt32",  "AlternativeArchitecture"),
    (0x2C4, "UInt32",  "BootId"),
    (0x2C8, "UInt64",  "SystemExpirationDate"),
    (0x2D0, "UInt32",  "SuiteMask"),
    (0x2D4, "UInt8",   "KdDebuggerEnabled"),
    (0x2D5, "UInt8",   "MitigationPolicies"),
    (0x2D6, "UInt16",  "CyclesPerYield"),

    # ── Console / pages ──
    (0x2D8, "UInt32",  "ActiveConsoleId"),
    (0x2DC, "UInt32",  "DismountCount"),
    (0x2E0, "UInt32",  "ComPlusPackage"),
    (0x2E4, "UInt32",  "LastSystemRITEventTickCount"),
    (0x2E8, "UInt32",  "NumberOfPhysicalPages"),
    (0x2EC, "UInt8",   "SafeBootMode"),
    (0x2ED, "UInt8",   "VirtualizationFlags"),
    (0x2EE, "UInt16",  "Reserved12"),

    # ── Shared data flags ──
    (0x2F0, "Hex32",   "SharedDataFlags"),
    (0x2F4, "UInt32",  "DataFlagsPad"),

    # ── QPC / system call ──
    (0x2F8, "UInt64",  "TestRetInstruction"),
    (0x300, "Int64",   "QpcFrequency"),
    (0x308, "UInt32",  "SystemCall"),
    (0x30C, "UInt32",  "Reserved2"),
    (0x310, "UInt64",  "FullNumberOfPhysicalPages"),
    (0x318, "UInt64",  "SystemCallPad"),

    # ── TickCount: _KSYSTEM_TIME ──
    (0x320, "UInt32",  "TickCount.LowPart"),
    (0x324, "UInt32",  "TickCount.High1Time"),
    (0x328, "UInt32",  "TickCount.High2Time"),
    (0x32C, "UInt32",  "TickCountPad"),

    # ── Cookie ──
    (0x330, "UInt32",  "Cookie"),
    (0x334, "UInt32",  "CookiePad"),

    # ── Session / QPC timing ──
    (0x338, "Int64",   "ConsoleSessionForegroundProcessId"),
    (0x340, "UInt64",  "TimeUpdateLock"),
    (0x348, "UInt64",  "BaselineSystemTimeQpc"),
    (0x350, "UInt64",  "BaselineInterruptTimeQpc"),
    (0x358, "UInt64",  "QpcSystemTimeIncrement"),
    (0x360, "UInt64",  "QpcInterruptTimeIncrement"),
    (0x368, "UInt8",   "QpcSystemTimeIncrementShift"),
    (0x369, "UInt8",   "QpcInterruptTimeIncrementShift"),
    (0x36A, "UInt16",  "UnparkedProcessorCount"),

    # ── Enclave ──
    (0x36C, "Hex32",   "EnclaveFeatureMask[0]"),
    (0x370, "Hex32",   "EnclaveFeatureMask[1]"),
    (0x374, "Hex32",   "EnclaveFeatureMask[2]"),
    (0x378, "Hex32",   "EnclaveFeatureMask[3]"),

    (0x37C, "UInt32",  "TelemetryCoverageRound"),

    # ── UserModeGlobalLogger USHORT[16] = 32 bytes ──
    (0x380, "UInt16",  "UserModeGlobalLogger[0]"),
    (0x382, "UInt16",  "UserModeGlobalLogger[1]"),
    (0x384, "UInt16",  "UserModeGlobalLogger[2]"),
    (0x386, "UInt16",  "UserModeGlobalLogger[3]"),
    (0x388, "UInt16",  "UserModeGlobalLogger[4]"),
    (0x38A, "UInt16",  "UserModeGlobalLogger[5]"),
    (0x38C, "UInt16",  "UserModeGlobalLogger[6]"),
    (0x38E, "UInt16",  "UserModeGlobalLogger[7]"),
    (0x390, "UInt16",  "UserModeGlobalLogger[8]"),
    (0x392, "UInt16",  "UserModeGlobalLogger[9]"),
    (0x394, "UInt16",  "UserModeGlobalLogger[10]"),
    (0x396, "UInt16",  "UserModeGlobalLogger[11]"),
    (0x398, "UInt16",  "UserModeGlobalLogger[12]"),
    (0x39A, "UInt16",  "UserModeGlobalLogger[13]"),
    (0x39C, "UInt16",  "UserModeGlobalLogger[14]"),
    (0x39E, "UInt16",  "UserModeGlobalLogger[15]"),

    (0x3A0, "UInt32",  "ImageFileExecutionOptions"),
    (0x3A4, "UInt32",  "LangGenerationCount"),
    (0x3A8, "UInt64",  "Reserved4"),
    (0x3B0, "UInt64",  "InterruptTimeBias"),
    (0x3B8, "UInt64",  "QpcBias"),
    (0x3C0, "UInt32",  "ActiveProcessorCount"),
    (0x3C4, "UInt8",   "ActiveGroupCount"),
    (0x3C5, "UInt8",   "Reserved9"),
    (0x3C6, "UInt16",  "QpcData"),
    (0x3C8, "UInt64",  "TimeZoneBiasEffectiveStart"),
    (0x3D0, "UInt64",  "TimeZoneBiasEffectiveEnd"),

    # ═══════════════════════════════════════════════
    # _XSTATE_CONFIGURATION  (0x3D8 .. 0x71F)
    # ═══════════════════════════════════════════════
    (0x3D8, "UInt64",  "XState.EnabledFeatures"),
    (0x3E0, "UInt64",  "XState.EnabledVolatileFeatures"),
    (0x3E8, "UInt32",  "XState.Size"),
    (0x3EC, "Hex32",   "XState.ControlFlags"),

    # XState.Features[64]: each {ULONG Offset, ULONG Size} = 8 bytes
    # 64 entries = 512 bytes: 0x3F0 .. 0x5EF
]

# Generate XState.Features[0..63]
for i in range(64):
    base = 0x3F0 + i * 8
    fields.append((base,     "UInt32", f"XState.Features[{i}].Offset"))
    fields.append((base + 4, "UInt32", f"XState.Features[{i}].Size"))

fields += [
    # After Features[64]: offset 0x3D8+0x218 = 0x5F0
    (0x5F0, "UInt64",  "XState.EnabledSupervisorFeatures"),
    (0x5F8, "UInt64",  "XState.AlignedFeatures"),
    (0x600, "UInt32",  "XState.AllFeatureSize"),
]

# XState.AllFeatures[64]: ULONG[64] = 256 bytes: 0x604 .. 0x703
for i in range(64):
    off = 0x604 + i * 4
    fields.append((off, "UInt32", f"XState.AllFeatures[{i}]"))

fields += [
    # 0x3D8+0x32C = 0x704
    (0x704, "UInt64",  "XState.EnabledUserVisibleSupervisorFeatures"),
    # Padding to reach 0x720: 0x70C .. 0x71F (20 bytes)
    (0x70C, "Hex64",   "XState.Padding0"),
    (0x714, "Hex64",   "XState.Padding1"),
    (0x71C, "Hex32",   "XState.Padding2"),

    # ═══════════════════════════════════════════════
    # After XState
    # ═══════════════════════════════════════════════
    # FeatureConfigurationChangeStamp: _KSYSTEM_TIME
    (0x720, "UInt32",  "FeatureConfigChangeStamp.LowPart"),
    (0x724, "UInt32",  "FeatureConfigChangeStamp.High1Time"),
    (0x728, "UInt32",  "FeatureConfigChangeStamp.High2Time"),
    (0x72C, "UInt32",  "Spare"),
    (0x730, "UInt64",  "UserPointerAuthMask"),
]

# Reserved10[210]: ULONG[210] = 840 bytes: 0x738 .. 0xA7F
# Just show as Hex64 blocks (105 x 8 bytes)
for i in range(105):
    off = 0x738 + i * 8
    fields.append((off, "Hex64", f"Reserved10[{i*2}..{i*2+1}]"))

# Build operations list
ops = [{"op": "insert", "kind": "Struct", "name": "ksd",
        "parentId": "0", "offset": 0, "structTypeName": "KUSER_SHARED_DATA"}]

for item in fields:
    offset, kind, name = item[0], item[1], item[2]
    extra = item[3] if len(item) > 3 else {}
    op = {"op": "insert", "kind": kind, "name": name,
          "parentId": P, "offset": offset}
    op.update(extra)
    ops.append(op)

print(f"# {len(ops)} operations ({len(ops)-1} fields + 1 struct)", flush=True, file=__import__('sys').stderr)

msg = {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
       "params": {"name": "tree.apply",
                  "arguments": {"macroName": "Full KUSER_SHARED_DATA (0xA80)",
                                "operations": ops}}}
print(json.dumps(msg))
