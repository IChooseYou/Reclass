#!/usr/bin/env python3
"""Generate EPROCESS.rcx from WinDbg dt output."""
import json

next_id = [0]
def nid():
    next_id[0] += 1
    return str(next_id[0])

def node(kind, name, offset, parent_id, **kw):
    n = {
        "arrayLen": kw.get("arrayLen", 1),
        "collapsed": kw.get("collapsed", False),
        "elementKind": kw.get("elementKind", "UInt8"),
        "id": nid(),
        "kind": kind,
        "name": name,
        "offset": offset,
        "parentId": parent_id,
        "refId": kw.get("refId", "0"),
        "strLen": kw.get("strLen", 64),
    }
    if kind == "Struct" and "structTypeName" in kw:
        n["structTypeName"] = kw["structTypeName"]
    return n

nodes = []

# ── _LIST_ENTRY definition (root-level, reusable) ──
le = node("Struct", "list_entry", 0, "0", structTypeName="_LIST_ENTRY")
le_id = le["id"]
nodes.append(le)
nodes.append(node("Pointer64", "Flink", 0, le_id))
nodes.append(node("Pointer64", "Blink", 8, le_id))

# ── Root _EPROCESS struct ──
ep = node("Struct", "eprocess", 0, "0", structTypeName="_EPROCESS")
EP = ep["id"]
nodes.append(ep)

# Helper to add a _LIST_ENTRY field
def list_entry(name, off, parent=EP):
    return node("Struct", name, off, parent, structTypeName="_LIST_ENTRY",
                refId=le_id, collapsed=True)

# Helper for EX_PUSH_LOCK (just uint64)
def push_lock(name, off, parent=EP):
    return node("UInt64", name, off, parent)

# Helper for LARGE_INTEGER
def large_int(name, off, parent=EP):
    return node("Int64", name, off, parent)

# Helper for pointer
def ptr(name, off, parent=EP):
    return node("Pointer64", name, off, parent)

# ═══════════════════════════════════════════════════════════════
# +0x000 Pcb : _KPROCESS  (0x438 bytes, with children)
# ═══════════════════════════════════════════════════════════════
pcb = node("Struct", "Pcb", 0x000, EP, structTypeName="_KPROCESS")
PCB = pcb["id"]
nodes.append(pcb)

# -- _DISPATCHER_HEADER at +0x000 within _KPROCESS --
dh = node("Struct", "Header", 0x000, PCB, structTypeName="_DISPATCHER_HEADER", collapsed=True)
DH = dh["id"]
nodes.append(dh)
nodes.append(node("UInt8", "Type", 0, DH))
nodes.append(node("UInt8", "Signalling", 1, DH))
nodes.append(node("UInt8", "Size", 2, DH))
nodes.append(node("UInt8", "Reserved1", 3, DH))
nodes.append(node("Int32", "SignalState", 4, DH))
nodes.append(list_entry("WaitListHead", 8, DH))

# +0x018 ProfileListHead
nodes.append(list_entry("ProfileListHead", 0x018, PCB))
# +0x028 DirectoryTableBase
nodes.append(node("UInt64", "DirectoryTableBase", 0x028, PCB))
# +0x030 ThreadListHead
nodes.append(list_entry("ThreadListHead", 0x030, PCB))
# +0x040 ProcessLock
nodes.append(node("UInt32", "ProcessLock", 0x040, PCB))
# +0x044 ProcessTimerDelay
nodes.append(node("UInt32", "ProcessTimerDelay", 0x044, PCB))
# +0x048 DeepFreezeStartTime
nodes.append(node("UInt64", "DeepFreezeStartTime", 0x048, PCB))

# +0x050 Affinity : _KAFFINITY_EX (0xa8 bytes)
aff = node("Struct", "Affinity", 0x050, PCB, structTypeName="_KAFFINITY_EX", collapsed=True)
AFF = aff["id"]
nodes.append(aff)
nodes.append(node("UInt16", "Count", 0, AFF))
nodes.append(node("UInt16", "Size", 2, AFF))
nodes.append(node("UInt32", "Reserved", 4, AFF))
nodes.append(node("Array", "Bitmap", 8, AFF, elementKind="UInt64", arrayLen=20, collapsed=True))

# +0x0f8 AffinityPadding (12 * uint64 = 0x60)
nodes.append(node("Array", "AffinityPadding", 0x0f8, PCB, elementKind="UInt64", arrayLen=12, collapsed=True))

# +0x158 ReadyListHead
nodes.append(list_entry("ReadyListHead", 0x158, PCB))
# +0x168 SwapListEntry (single pointer)
nodes.append(node("Pointer64", "SwapListEntry", 0x168, PCB))
# +0x170 ActiveProcessors : _KAFFINITY_EX
aff2 = node("Struct", "ActiveProcessors", 0x170, PCB, structTypeName="_KAFFINITY_EX", collapsed=True)
AFF2 = aff2["id"]
nodes.append(aff2)
nodes.append(node("UInt16", "Count", 0, AFF2))
nodes.append(node("UInt16", "Size", 2, AFF2))
nodes.append(node("UInt32", "Reserved", 4, AFF2))
nodes.append(node("Array", "Bitmap", 8, AFF2, elementKind="UInt64", arrayLen=20, collapsed=True))

# +0x218 ActiveProcessorsPadding
nodes.append(node("Array", "ActiveProcessorsPadding", 0x218, PCB, elementKind="UInt64", arrayLen=12, collapsed=True))

# +0x278 ProcessFlags
nodes.append(node("UInt32", "ProcessFlags", 0x278, PCB))
# +0x27c ActiveGroupsMask
nodes.append(node("UInt32", "ActiveGroupsMask", 0x27c, PCB))
# +0x280 BasePriority
nodes.append(node("UInt8", "BasePriority", 0x280, PCB))
# +0x281 QuantumReset
nodes.append(node("UInt8", "QuantumReset", 0x281, PCB))
# +0x282 Visited
nodes.append(node("UInt8", "Visited", 0x282, PCB))
# +0x283 Flags (KEXECUTE_OPTIONS - 1 byte)
nodes.append(node("UInt8", "Flags", 0x283, PCB))

# +0x284 ThreadSeed [20] UInt16
nodes.append(node("Array", "ThreadSeed", 0x284, PCB, elementKind="UInt16", arrayLen=20, collapsed=True))
# +0x2ac ThreadSeedPadding [12] UInt64
nodes.append(node("Array", "ThreadSeedPadding", 0x2ac, PCB, elementKind="UInt16", arrayLen=12, collapsed=True))
# +0x2c4 IdealProcessor [20] UInt16
nodes.append(node("Array", "IdealProcessor", 0x2c4, PCB, elementKind="UInt16", arrayLen=20, collapsed=True))
# +0x2ec IdealProcessorPadding [12] UInt16
nodes.append(node("Array", "IdealProcessorPadding", 0x2ec, PCB, elementKind="UInt16", arrayLen=12, collapsed=True))
# +0x304 IdealNode [20] UInt16
nodes.append(node("Array", "IdealNode", 0x304, PCB, elementKind="UInt16", arrayLen=20, collapsed=True))
# +0x32c IdealNodePadding [12] UInt16
nodes.append(node("Array", "IdealNodePadding", 0x32c, PCB, elementKind="UInt16", arrayLen=12, collapsed=True))

# +0x344 IdealGlobalNode
nodes.append(node("UInt16", "IdealGlobalNode", 0x344, PCB))
# +0x346 Spare1
nodes.append(node("UInt16", "Spare1", 0x346, PCB))

# +0x348 StackCount (union, just uint32 visible)
nodes.append(node("UInt32", "StackCount", 0x348, PCB))

# +0x350 ProcessListEntry
nodes.append(list_entry("ProcessListEntry", 0x350, PCB))
# +0x360 CycleTime
nodes.append(node("UInt64", "CycleTime", 0x360, PCB))
# +0x368 ContextSwitches
nodes.append(node("UInt64", "ContextSwitches", 0x368, PCB))
# +0x370 SchedulingGroup
nodes.append(node("Pointer64", "SchedulingGroup", 0x370, PCB))
# +0x378 FreezeCount
nodes.append(node("UInt32", "FreezeCount", 0x378, PCB))
# +0x37c KernelTime
nodes.append(node("UInt32", "KernelTime", 0x37c, PCB))
# +0x380 UserTime
nodes.append(node("UInt32", "UserTime", 0x380, PCB))
# +0x384 ReadyTime
nodes.append(node("UInt32", "ReadyTime", 0x384, PCB))
# +0x388 UserDirectoryTableBase
nodes.append(node("UInt64", "UserDirectoryTableBase", 0x388, PCB))
# +0x390 AddressPolicy
nodes.append(node("UInt8", "AddressPolicy", 0x390, PCB))

# +0x3d8 InstrumentationCallback
nodes.append(node("Pointer64", "InstrumentationCallback", 0x3d8, PCB))
# +0x3e0 SecureState (uint64 union)
nodes.append(node("UInt64", "SecureState", 0x3e0, PCB))
# +0x3e8 KernelWaitTime
nodes.append(node("UInt64", "KernelWaitTime", 0x3e8, PCB))
# +0x3f0 UserWaitTime
nodes.append(node("UInt64", "UserWaitTime", 0x3f0, PCB))

# +0x3f8 EndPadding [8] uint64
nodes.append(node("Array", "EndPadding", 0x3f8, PCB, elementKind="UInt64", arrayLen=8, collapsed=True))


# ═══════════════════════════════════════════════════════════════
# _EPROCESS fields after Pcb (offset >= 0x438)
# ═══════════════════════════════════════════════════════════════

# +0x438 ProcessLock
nodes.append(push_lock("ProcessLock", 0x438))
# +0x440 UniqueProcessId
nodes.append(ptr("UniqueProcessId", 0x440))
# +0x448 ActiveProcessLinks
nodes.append(list_entry("ActiveProcessLinks", 0x448))
# +0x458 RundownProtect (8 bytes)
nodes.append(node("UInt64", "RundownProtect", 0x458, EP))
# +0x460 Flags2
nodes.append(node("UInt32", "Flags2", 0x460, EP))
# +0x464 Flags
nodes.append(node("UInt32", "Flags", 0x464, EP))
# +0x468 CreateTime
nodes.append(large_int("CreateTime", 0x468))
# +0x470 ProcessQuotaUsage [2]
nodes.append(node("Array", "ProcessQuotaUsage", 0x470, EP, elementKind="UInt64", arrayLen=2, collapsed=True))
# +0x480 ProcessQuotaPeak [2]
nodes.append(node("Array", "ProcessQuotaPeak", 0x480, EP, elementKind="UInt64", arrayLen=2, collapsed=True))
# +0x490 PeakVirtualSize
nodes.append(node("UInt64", "PeakVirtualSize", 0x490, EP))
# +0x498 VirtualSize
nodes.append(node("UInt64", "VirtualSize", 0x498, EP))
# +0x4a0 SessionProcessLinks
nodes.append(list_entry("SessionProcessLinks", 0x4a0))
# +0x4b0 ExceptionPortData / ExceptionPortValue (union)
nodes.append(ptr("ExceptionPortData", 0x4b0))
nodes.append(node("UInt64", "ExceptionPortValue", 0x4b0, EP))
# +0x4b8 Token
nodes.append(node("UInt64", "Token", 0x4b8, EP))
# +0x4c0 MmReserved
nodes.append(node("UInt64", "MmReserved", 0x4c0, EP))
# +0x4c8 AddressCreationLock
nodes.append(push_lock("AddressCreationLock", 0x4c8))
# +0x4d0 PageTableCommitmentLock
nodes.append(push_lock("PageTableCommitmentLock", 0x4d0))
# +0x4d8 RotateInProgress
nodes.append(ptr("RotateInProgress", 0x4d8))
# +0x4e0 ForkInProgress
nodes.append(ptr("ForkInProgress", 0x4e0))
# +0x4e8 CommitChargeJob
nodes.append(ptr("CommitChargeJob", 0x4e8))
# +0x4f0 CloneRoot (RTL_AVL_TREE = single pointer)
nodes.append(node("UInt64", "CloneRoot", 0x4f0, EP))
# +0x4f8 NumberOfPrivatePages
nodes.append(node("UInt64", "NumberOfPrivatePages", 0x4f8, EP))
# +0x500 NumberOfLockedPages
nodes.append(node("UInt64", "NumberOfLockedPages", 0x500, EP))
# +0x508 Win32Process
nodes.append(ptr("Win32Process", 0x508))
# +0x510 Job
nodes.append(ptr("Job", 0x510))
# +0x518 SectionObject
nodes.append(ptr("SectionObject", 0x518))
# +0x520 SectionBaseAddress
nodes.append(ptr("SectionBaseAddress", 0x520))
# +0x528 Cookie
nodes.append(node("UInt32", "Cookie", 0x528, EP))
# +0x530 WorkingSetWatch
nodes.append(ptr("WorkingSetWatch", 0x530))
# +0x538 Win32WindowStation
nodes.append(ptr("Win32WindowStation", 0x538))
# +0x540 InheritedFromUniqueProcessId
nodes.append(ptr("InheritedFromUniqueProcessId", 0x540))
# +0x548 OwnerProcessId
nodes.append(node("UInt64", "OwnerProcessId", 0x548, EP))
# +0x550 Peb
nodes.append(ptr("Peb", 0x550))
# +0x558 Session
nodes.append(ptr("Session", 0x558))
# +0x560 Spare1
nodes.append(ptr("Spare1", 0x560))
# +0x568 QuotaBlock
nodes.append(ptr("QuotaBlock", 0x568))
# +0x570 ObjectTable
nodes.append(ptr("ObjectTable", 0x570))
# +0x578 DebugPort
nodes.append(ptr("DebugPort", 0x578))
# +0x580 WoW64Process
nodes.append(ptr("WoW64Process", 0x580))
# +0x588 DeviceMap
nodes.append(ptr("DeviceMap", 0x588))
# +0x590 EtwDataSource
nodes.append(ptr("EtwDataSource", 0x590))
# +0x598 PageDirectoryPte
nodes.append(node("UInt64", "PageDirectoryPte", 0x598, EP))
# +0x5a0 ImageFilePointer
nodes.append(ptr("ImageFilePointer", 0x5a0))
# +0x5a8 ImageFileName [15] UChar → UTF8
nodes.append(node("UTF8", "ImageFileName", 0x5a8, EP, strLen=15))
# +0x5b7 PriorityClass
nodes.append(node("UInt8", "PriorityClass", 0x5b7, EP))
# +0x5b8 SecurityPort
nodes.append(ptr("SecurityPort", 0x5b8))
# +0x5c0 SeAuditProcessCreationInfo (single pointer inside)
nodes.append(node("UInt64", "SeAuditProcessCreationInfo", 0x5c0, EP))
# +0x5c8 JobLinks
nodes.append(list_entry("JobLinks", 0x5c8))
# +0x5d8 HighestUserAddress
nodes.append(ptr("HighestUserAddress", 0x5d8))
# +0x5e0 ThreadListHead
nodes.append(list_entry("ThreadListHead", 0x5e0))
# +0x5f0 ActiveThreads
nodes.append(node("UInt32", "ActiveThreads", 0x5f0, EP))
# +0x5f4 ImagePathHash
nodes.append(node("UInt32", "ImagePathHash", 0x5f4, EP))
# +0x5f8 DefaultHardErrorProcessing
nodes.append(node("UInt32", "DefaultHardErrorProcessing", 0x5f8, EP))
# +0x5fc LastThreadExitStatus
nodes.append(node("Int32", "LastThreadExitStatus", 0x5fc, EP))
# +0x600 PrefetchTrace (EX_FAST_REF = uint64)
nodes.append(node("UInt64", "PrefetchTrace", 0x600, EP))
# +0x608 LockedPagesList
nodes.append(ptr("LockedPagesList", 0x608))
# +0x610 ReadOperationCount
nodes.append(large_int("ReadOperationCount", 0x610))
# +0x618 WriteOperationCount
nodes.append(large_int("WriteOperationCount", 0x618))
# +0x620 OtherOperationCount
nodes.append(large_int("OtherOperationCount", 0x620))
# +0x628 ReadTransferCount
nodes.append(large_int("ReadTransferCount", 0x628))
# +0x630 WriteTransferCount
nodes.append(large_int("WriteTransferCount", 0x630))
# +0x638 OtherTransferCount
nodes.append(large_int("OtherTransferCount", 0x638))
# +0x640 CommitChargeLimit
nodes.append(node("UInt64", "CommitChargeLimit", 0x640, EP))
# +0x648 CommitCharge
nodes.append(node("UInt64", "CommitCharge", 0x648, EP))
# +0x650 CommitChargePeak
nodes.append(node("UInt64", "CommitChargePeak", 0x650, EP))

# +0x680 Vm : _MMSUPPORT_FULL (0x140 bytes, collapsed)
vm = node("Struct", "Vm", 0x680, EP, structTypeName="_MMSUPPORT_FULL", collapsed=True)
VM = vm["id"]
nodes.append(vm)
# Instance sub-struct
inst = node("Struct", "Instance", 0, VM, structTypeName="_MMSUPPORT_INSTANCE", collapsed=True)
INST = inst["id"]
nodes.append(inst)
nodes.append(node("UInt32", "NextPageColor", 0x000, INST))
nodes.append(node("UInt32", "PageFaultCount", 0x004, INST))
nodes.append(node("UInt64", "TrimmedPageCount", 0x008, INST))
nodes.append(node("Pointer64", "VmWorkingSetList", 0x010, INST))
nodes.append(list_entry("WorkingSetExpansionLinks", 0x018, INST))
nodes.append(node("Array", "AgeDistribution", 0x028, INST, elementKind="UInt64", arrayLen=8, collapsed=True))
nodes.append(node("Pointer64", "ExitOutswapGate", 0x068, INST))
nodes.append(node("UInt64", "MinimumWorkingSetSize", 0x070, INST))
nodes.append(node("UInt64", "WorkingSetLeafSize", 0x078, INST))
nodes.append(node("UInt64", "WorkingSetLeafPrivateSize", 0x080, INST))
nodes.append(node("UInt64", "WorkingSetSize", 0x088, INST))
nodes.append(node("UInt64", "WorkingSetPrivateSize", 0x090, INST))
nodes.append(node("UInt64", "MaximumWorkingSetSize", 0x098, INST))
nodes.append(node("UInt64", "PeakWorkingSetSize", 0x0a0, INST))
nodes.append(node("UInt32", "HardFaultCount", 0x0a8, INST))
nodes.append(node("UInt16", "LastTrimStamp", 0x0ac, INST))
nodes.append(node("UInt16", "PartitionId", 0x0ae, INST))
nodes.append(node("UInt64", "SelfmapLock", 0x0b0, INST))
nodes.append(node("UInt32", "Flags", 0x0b8, INST))

# Shared sub-struct at +0xc0 within _MMSUPPORT_FULL
shared = node("Struct", "Shared", 0xc0, VM, structTypeName="_MMSUPPORT_SHARED", collapsed=True)
SH = shared["id"]
nodes.append(shared)
nodes.append(node("Int32", "WorkingSetLock", 0, SH))
nodes.append(node("Int32", "GoodCitizenWaiting", 4, SH))
nodes.append(node("UInt64", "ReleasedCommitDebt", 8, SH))
nodes.append(node("UInt64", "ResetPagesRepurposedCount", 0x10, SH))
nodes.append(node("Pointer64", "WsSwapSupport", 0x18, SH))
nodes.append(node("Pointer64", "CommitReleaseContext", 0x20, SH))
nodes.append(node("Pointer64", "AccessLog", 0x28, SH))
nodes.append(node("UInt64", "ChargedWslePages", 0x30, SH))
nodes.append(node("UInt64", "ActualWslePages", 0x38, SH))
nodes.append(node("UInt64", "WorkingSetCoreLock", 0x40, SH))
nodes.append(node("Pointer64", "ShadowMapping", 0x48, SH))

# +0x7c0 MmProcessLinks
nodes.append(list_entry("MmProcessLinks", 0x7c0))
# +0x7d0 ModifiedPageCount
nodes.append(node("UInt32", "ModifiedPageCount", 0x7d0, EP))
# +0x7d4 ExitStatus
nodes.append(node("Int32", "ExitStatus", 0x7d4, EP))
# +0x7d8 VadRoot
nodes.append(node("UInt64", "VadRoot", 0x7d8, EP))
# +0x7e0 VadHint
nodes.append(ptr("VadHint", 0x7e0))
# +0x7e8 VadCount
nodes.append(node("UInt64", "VadCount", 0x7e8, EP))
# +0x7f0 VadPhysicalPages
nodes.append(node("UInt64", "VadPhysicalPages", 0x7f0, EP))
# +0x7f8 VadPhysicalPagesLimit
nodes.append(node("UInt64", "VadPhysicalPagesLimit", 0x7f8, EP))

# +0x800 AlpcContext : _ALPC_PROCESS_CONTEXT (0x20 bytes)
alpc = node("Struct", "AlpcContext", 0x800, EP, structTypeName="_ALPC_PROCESS_CONTEXT", collapsed=True)
ALPC = alpc["id"]
nodes.append(alpc)
nodes.append(node("UInt64", "Lock", 0, ALPC))
nodes.append(list_entry("ViewListHead", 8, ALPC))
nodes.append(node("UInt64", "PagedPoolQuotaCache", 0x18, ALPC))

# +0x820 TimerResolutionLink
nodes.append(list_entry("TimerResolutionLink", 0x820))
# +0x830 TimerResolutionStackRecord
nodes.append(ptr("TimerResolutionStackRecord", 0x830))
# +0x838 RequestedTimerResolution
nodes.append(node("UInt32", "RequestedTimerResolution", 0x838, EP))
# +0x83c SmallestTimerResolution
nodes.append(node("UInt32", "SmallestTimerResolution", 0x83c, EP))
# +0x840 ExitTime
nodes.append(large_int("ExitTime", 0x840))
# +0x848 InvertedFunctionTable
nodes.append(ptr("InvertedFunctionTable", 0x848))
# +0x850 InvertedFunctionTableLock
nodes.append(push_lock("InvertedFunctionTableLock", 0x850))
# +0x858 ActiveThreadsHighWatermark
nodes.append(node("UInt32", "ActiveThreadsHighWatermark", 0x858, EP))
# +0x85c LargePrivateVadCount
nodes.append(node("UInt32", "LargePrivateVadCount", 0x85c, EP))
# +0x860 ThreadListLock
nodes.append(push_lock("ThreadListLock", 0x860))
# +0x868 WnfContext
nodes.append(ptr("WnfContext", 0x868))
# +0x870 ServerSilo
nodes.append(ptr("ServerSilo", 0x870))
# +0x878 SignatureLevel
nodes.append(node("UInt8", "SignatureLevel", 0x878, EP))
# +0x879 SectionSignatureLevel
nodes.append(node("UInt8", "SectionSignatureLevel", 0x879, EP))
# +0x87a Protection (PS_PROTECTION = 1 byte)
nodes.append(node("UInt8", "Protection", 0x87a, EP))
# +0x87b HangCount/GhostCount bitfield byte
nodes.append(node("UInt8", "HangGhostFlags", 0x87b, EP))
# +0x87c Flags3
nodes.append(node("UInt32", "Flags3", 0x87c, EP))
# +0x880 DeviceAsid
nodes.append(node("Int32", "DeviceAsid", 0x880, EP))
# +0x888 SvmData
nodes.append(ptr("SvmData", 0x888))
# +0x890 SvmProcessLock
nodes.append(push_lock("SvmProcessLock", 0x890))
# +0x898 SvmLock
nodes.append(node("UInt64", "SvmLock", 0x898, EP))
# +0x8a0 SvmProcessDeviceListHead
nodes.append(list_entry("SvmProcessDeviceListHead", 0x8a0))
# +0x8b0 LastFreezeInterruptTime
nodes.append(node("UInt64", "LastFreezeInterruptTime", 0x8b0, EP))
# +0x8b8 DiskCounters
nodes.append(ptr("DiskCounters", 0x8b8))
# +0x8c0 PicoContext
nodes.append(ptr("PicoContext", 0x8c0))
# +0x8c8 EnclaveTable
nodes.append(ptr("EnclaveTable", 0x8c8))
# +0x8d0 EnclaveNumber
nodes.append(node("UInt64", "EnclaveNumber", 0x8d0, EP))
# +0x8d8 EnclaveLock
nodes.append(push_lock("EnclaveLock", 0x8d8))
# +0x8e0 HighPriorityFaultsAllowed
nodes.append(node("UInt32", "HighPriorityFaultsAllowed", 0x8e0, EP))
# +0x8e8 EnergyContext
nodes.append(ptr("EnergyContext", 0x8e8))
# +0x8f0 VmContext
nodes.append(ptr("VmContext", 0x8f0))
# +0x8f8 SequenceNumber
nodes.append(node("UInt64", "SequenceNumber", 0x8f8, EP))
# +0x900 CreateInterruptTime
nodes.append(node("UInt64", "CreateInterruptTime", 0x900, EP))
# +0x908 CreateUnbiasedInterruptTime
nodes.append(node("UInt64", "CreateUnbiasedInterruptTime", 0x908, EP))
# +0x910 TotalUnbiasedFrozenTime
nodes.append(node("UInt64", "TotalUnbiasedFrozenTime", 0x910, EP))
# +0x918 LastAppStateUpdateTime
nodes.append(node("UInt64", "LastAppStateUpdateTime", 0x918, EP))
# +0x920 LastAppStateUptime (bitfield union, show as uint64)
nodes.append(node("UInt64", "LastAppStateUptime", 0x920, EP))
# +0x928 SharedCommitCharge
nodes.append(node("UInt64", "SharedCommitCharge", 0x928, EP))
# +0x930 SharedCommitLock
nodes.append(push_lock("SharedCommitLock", 0x930))
# +0x938 SharedCommitLinks
nodes.append(list_entry("SharedCommitLinks", 0x938))
# +0x948 AllowedCpuSets / AllowedCpuSetsIndirect (union)
nodes.append(node("UInt64", "AllowedCpuSets", 0x948, EP))
# +0x950 DefaultCpuSets / DefaultCpuSetsIndirect (union)
nodes.append(node("UInt64", "DefaultCpuSets", 0x950, EP))
nodes.append(ptr("AllowedCpuSetsIndirect", 0x948))
nodes.append(ptr("DefaultCpuSetsIndirect", 0x950))
# +0x958 DiskIoAttribution
nodes.append(ptr("DiskIoAttribution", 0x958))
# +0x960 DxgProcess
nodes.append(ptr("DxgProcess", 0x960))
# +0x968 Win32KFilterSet
nodes.append(node("UInt32", "Win32KFilterSet", 0x968, EP))
# +0x970 ProcessTimerDelay (uint64)
nodes.append(node("UInt64", "ProcessTimerDelay", 0x970, EP))
# +0x978 KTimerSets
nodes.append(node("UInt32", "KTimerSets", 0x978, EP))
# +0x97c KTimer2Sets
nodes.append(node("UInt32", "KTimer2Sets", 0x97c, EP))
# +0x980 ThreadTimerSets
nodes.append(node("UInt32", "ThreadTimerSets", 0x980, EP))
# +0x988 VirtualTimerListLock
nodes.append(node("UInt64", "VirtualTimerListLock", 0x988, EP))
# +0x990 VirtualTimerListHead
nodes.append(list_entry("VirtualTimerListHead", 0x990))

# +0x9a0 WakeChannel / WakeInfo (union)
nodes.append(node("UInt64", "WakeChannel", 0x9a0, EP))
wake = node("Struct", "WakeInfo", 0x9a0, EP, structTypeName="_PS_PROCESS_WAKE_INFORMATION", collapsed=True)
WAKE = wake["id"]
nodes.append(wake)
nodes.append(node("UInt64", "NotificationChannel", 0, WAKE))
nodes.append(node("Array", "WakeCounters", 8, WAKE, elementKind="UInt64", arrayLen=7, collapsed=True))
nodes.append(node("UInt32", "HighEdgeFilter", 0x24, WAKE))
nodes.append(node("UInt32", "LowEdgeFilter", 0x28, WAKE))
nodes.append(node("UInt32", "NoWakeCounter", 0x2c, WAKE))

# +0x9d0 MitigationFlags / MitigationFlagsValues (union)
nodes.append(node("UInt32", "MitigationFlags", 0x9d0, EP))
nodes.append(node("UInt32", "MitigationFlagsValues", 0x9d0, EP))
# +0x9d4 MitigationFlags2 / MitigationFlags2Values
nodes.append(node("UInt32", "MitigationFlags2", 0x9d4, EP))
nodes.append(node("UInt32", "MitigationFlags2Values", 0x9d4, EP))
# +0x9d8 PartitionObject
nodes.append(ptr("PartitionObject", 0x9d8))
# +0x9e0 SecurityDomain
nodes.append(node("UInt64", "SecurityDomain", 0x9e0, EP))
# +0x9e8 ParentSecurityDomain
nodes.append(node("UInt64", "ParentSecurityDomain", 0x9e8, EP))
# +0x9f0 CoverageSamplerContext
nodes.append(ptr("CoverageSamplerContext", 0x9f0))
# +0x9f8 MmHotPatchContext
nodes.append(ptr("MmHotPatchContext", 0x9f8))
# +0xa00 DynamicEHContinuationTargetsTree
nodes.append(node("UInt64", "DynamicEHContinuationTargetsTree", 0xa00, EP))
# +0xa08 DynamicEHContinuationTargetsLock
nodes.append(push_lock("DynamicEHContinuationTargetsLock", 0xa08))

# +0xa10 DynamicEnforcedCetCompatibleRanges : _PS_DYNAMIC_ENFORCED_ADDRESS_RANGES
dear = node("Struct", "DynamicEnforcedCetCompatibleRanges", 0xa10, EP,
            structTypeName="_PS_DYNAMIC_ENFORCED_ADDRESS_RANGES", collapsed=True)
DEAR = dear["id"]
nodes.append(dear)
nodes.append(node("UInt64", "Tree", 0, DEAR))
nodes.append(node("UInt64", "Lock", 8, DEAR))

# +0xa20 DisabledComponentFlags
nodes.append(node("UInt32", "DisabledComponentFlags", 0xa20, EP))
# +0xa28 PathRedirectionHashes
nodes.append(ptr("PathRedirectionHashes", 0xa28))
# +0xa30 MitigationFlags3 / MitigationFlags3Values
nodes.append(node("UInt32", "MitigationFlags3", 0xa30, EP))
nodes.append(node("UInt32", "MitigationFlags3Values", 0xa30, EP))


# ── Output ──
doc = {
    "baseAddress": "FFFFB9817BE2F080",
    "nextId": str(next_id[0] + 1),
    "nodes": nodes,
}

with open("src/examples/EPROCESS.rcx", "w", newline="\n") as f:
    json.dump(doc, f, indent=4)
    f.write("\n")

print(f"Generated {len(nodes)} nodes, next_id={next_id[0]+1}")
