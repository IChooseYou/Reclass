/*
 * rcxdrv.c -- Minimal kernel-mode memory driver for Reclass.
 *
 * Provides: virtual memory R/W (per-process), physical memory R/W,
 *           region/PEB/module/TEB query, CR3 read, virtual-to-physical translation.
 *
 * Safety: all inputs validated, SEH around privileged instructions,
 *         MmCopyVirtualMemory for cross-process reads (no attach deadlock),
 *         METHOD_BUFFERED (no raw user pointers).
 */
#include <ntifs.h>
#include "../rcx_drv_protocol.h"

/* ── Undocumented but stable kernel exports (Vista+) ────────────── */

NTSTATUS NTAPI MmCopyVirtualMemory(
    PEPROCESS SourceProcess, PVOID SourceAddress,
    PEPROCESS TargetProcess, PVOID TargetAddress,
    SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode,
    PSIZE_T ReturnSize);

PPEB NTAPI PsGetProcessPeb(PEPROCESS Process);
PVOID NTAPI PsGetProcessWow64Process(PEPROCESS Process);
PVOID NTAPI PsGetThreadTeb(PETHREAD Thread);

/*
 * PsGetNextProcessThread is undocumented (not in any .lib).
 * We resolve it dynamically via MmGetSystemRoutineAddress.
 */
typedef PETHREAD (NTAPI *PsGetNextProcessThread_t)(PEPROCESS Process, PETHREAD Thread);
static PsGetNextProcessThread_t g_PsGetNextProcessThread = NULL;

/* ── Manual structure definitions (kernel-mode) ─────────────────── */
/* These are partially opaque in WDK headers; define just the offsets we need. */

typedef struct _MEMORY_BASIC_INFORMATION_KM {
    PVOID  BaseAddress;
    PVOID  AllocationBase;
    ULONG  AllocationProtect;
    SIZE_T RegionSize;
    ULONG  State;
    ULONG  Protect;
    ULONG  Type;
} MEMORY_BASIC_INFORMATION_KM;

#define MEM_COMMIT_KM 0x1000

/* PEB.Ldr minimal definition for module enumeration */
typedef struct _PEB_LDR_DATA_KM {
    UCHAR      Reserved1[8];
    PVOID      Reserved2[3];
    LIST_ENTRY InLoadOrderModuleList;
} PEB_LDR_DATA_KM;

/* PEB minimal: only need Ldr at offset 0x18 (x64) */
typedef struct _PEB_KM {
    UCHAR          Reserved1[2];
    UCHAR          BeingDebugged;
    UCHAR          Reserved2[0x15];
    PEB_LDR_DATA_KM* Ldr;  /* offset 0x18 on x64 */
} PEB_KM;

/* LDR_DATA_TABLE_ENTRY minimal for walking InLoadOrderModuleList */
typedef struct _LDR_DATA_TABLE_ENTRY_KM {
    LIST_ENTRY     InLoadOrderLinks;        /* offset 0x00 */
    LIST_ENTRY     InMemoryOrderLinks;      /* offset 0x10 */
    LIST_ENTRY     InInitializationOrderLinks; /* offset 0x20 */
    PVOID          DllBase;                 /* offset 0x30 */
    PVOID          EntryPoint;              /* offset 0x38 */
    ULONG          SizeOfImage;             /* offset 0x40 */
    ULONG          _pad;
    UNICODE_STRING FullDllName;             /* offset 0x48 */
    UNICODE_STRING BaseDllName;             /* offset 0x58 */
} LDR_DATA_TABLE_ENTRY_KM;

/* ── Forward declarations ────────────────────────────────────────── */

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT dev, PIRP irp);
static NTSTATUS DispatchIoctl(PDEVICE_OBJECT dev, PIRP irp);
DRIVER_UNLOAD   DriverUnload;

/* ZwCurrentProcess() macro for ZwQueryVirtualMemory */
#ifndef ZwCurrentProcess
#define ZwCurrentProcess() ((HANDLE)(LONG_PTR)-1)
#endif

/* ── Helpers ─────────────────────────────────────────────────────── */

#define VALIDATE_INPUT(irp, stk, T) \
    do { \
        if ((stk)->Parameters.DeviceIoControl.InputBufferLength < sizeof(T)) { \
            (irp)->IoStatus.Status = STATUS_BUFFER_TOO_SMALL; \
            (irp)->IoStatus.Information = 0; \
            IoCompleteRequest((irp), IO_NO_INCREMENT); \
            return STATUS_BUFFER_TOO_SMALL; \
        } \
    } while (0)

#define VALIDATE_OUTPUT(irp, stk, minSize) \
    do { \
        if ((stk)->Parameters.DeviceIoControl.OutputBufferLength < (ULONG)(minSize)) { \
            (irp)->IoStatus.Status = STATUS_BUFFER_TOO_SMALL; \
            (irp)->IoStatus.Information = 0; \
            IoCompleteRequest((irp), IO_NO_INCREMENT); \
            return STATUS_BUFFER_TOO_SMALL; \
        } \
    } while (0)

static NTSTATUS LookupProcess(ULONG pid, PEPROCESS* proc)
{
    return PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, proc);
}

/* ── Safe physical mapping (MDL-based, avoids MmMapIoSpace BSOD) ── */
/*
 * MmMapIoSpace/MmUnmapIoSpace BSODs (bugcheck 0x50 in
 * MiClearMappingAndDereferenceIoSpace) when used on RAM-backed physical
 * addresses.  MDL-based mapping is safe for both RAM and MMIO.
 *
 * CRITICAL: cacheType must match the existing kernel mapping of the page.
 * Use MmCached for RAM pages (already mapped cached by the kernel).
 * Use MmNonCached ONLY for MMIO/device registers.
 * Mismatched cache attributes (e.g. MmNonCached on RAM) cause silent
 * kernel memory corruption via CPU cache coherency conflicts.
 */

typedef struct { PMDL mdl; PVOID base; } PHYS_MAP_CTX;

static PVOID MapPhysical(uint64_t physAddr, SIZE_T size,
                         MEMORY_CACHING_TYPE cacheType, PHYS_MAP_CTX* ctx)
{
    ctx->mdl = NULL;
    ctx->base = NULL;

    ULONG_PTR pageOff = (ULONG_PTR)(physAddr & (PAGE_SIZE - 1));
    SIZE_T totalSize = pageOff + size;
    ULONG pages = (ULONG)((totalSize + PAGE_SIZE - 1) / PAGE_SIZE);

    PMDL mdl = IoAllocateMdl(NULL, (ULONG)totalSize, FALSE, FALSE, NULL);
    if (!mdl) return NULL;

    PPFN_NUMBER pfn = MmGetMdlPfnArray(mdl);
    PFN_NUMBER startPfn = (PFN_NUMBER)(physAddr / PAGE_SIZE);
    for (ULONG i = 0; i < pages; i++)
        pfn[i] = startPfn + i;
    mdl->MdlFlags |= MDL_PAGES_LOCKED;

    __try {
        ctx->base = MmMapLockedPagesSpecifyCache(
            mdl, KernelMode, cacheType, NULL, FALSE, NormalPagePriority);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        IoFreeMdl(mdl);
        return NULL;
    }
    if (!ctx->base) { IoFreeMdl(mdl); return NULL; }

    ctx->mdl = mdl;
    return (PUCHAR)ctx->base + pageOff;
}

static void UnmapPhysical(PHYS_MAP_CTX* ctx)
{
    if (ctx->base) MmUnmapLockedPages(ctx->base, ctx->mdl);
    if (ctx->mdl)  IoFreeMdl(ctx->mdl);
    ctx->base = NULL;
    ctx->mdl = NULL;
}

/* ── Virtual memory read ─────────────────────────────────────────── */

static NTSTATUS HandleReadMemory(PIRP irp, PIO_STACK_LOCATION stk)
{
    VALIDATE_INPUT(irp, stk, struct RcxDrvReadRequest);

    struct RcxDrvReadRequest* req = (struct RcxDrvReadRequest*)irp->AssociatedIrp.SystemBuffer;
    if (req->length == 0 || req->length > RCX_DRV_MAX_VIRTUAL)
        return STATUS_INVALID_PARAMETER;

    VALIDATE_OUTPUT(irp, stk, req->length);

    /* Save request fields before MmCopyVirtualMemory overwrites SystemBuffer.
     * METHOD_BUFFERED aliases input and output to the same buffer, so the
     * copy destination (SystemBuffer) clobbers req->* fields. */
    ULONG pid = req->pid;
    uint64_t address = req->address;
    ULONG length = req->length;

    PEPROCESS proc = NULL;
    NTSTATUS st = LookupProcess(pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    SIZE_T bytesRead = 0;
    st = MmCopyVirtualMemory(
        proc, (PVOID)address,
        PsGetCurrentProcess(), irp->AssociatedIrp.SystemBuffer,
        (SIZE_T)length, KernelMode, &bytesRead);

    ObDereferenceObject(proc);

    /* Partial reads: zero remainder, report success */
    if (st == STATUS_PARTIAL_COPY) {
        RtlZeroMemory((PUCHAR)irp->AssociatedIrp.SystemBuffer + bytesRead,
                       length - bytesRead);
        irp->IoStatus.Information = length;
        return STATUS_SUCCESS;
    }

    irp->IoStatus.Information = NT_SUCCESS(st) ? length : 0;
    return st;
}

/* ── Virtual memory write ────────────────────────────────────────── */

static NTSTATUS HandleWriteMemory(PIRP irp, PIO_STACK_LOCATION stk)
{
    ULONG inputLen = stk->Parameters.DeviceIoControl.InputBufferLength;
    if (inputLen < sizeof(struct RcxDrvWriteRequest))
        return STATUS_BUFFER_TOO_SMALL;

    struct RcxDrvWriteRequest* req = (struct RcxDrvWriteRequest*)irp->AssociatedIrp.SystemBuffer;
    if (req->length == 0 || req->length > RCX_DRV_MAX_VIRTUAL)
        return STATUS_INVALID_PARAMETER;
    if (inputLen < sizeof(struct RcxDrvWriteRequest) + req->length)
        return STATUS_BUFFER_TOO_SMALL;

    PEPROCESS proc = NULL;
    NTSTATUS st = LookupProcess(req->pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    PUCHAR data = (PUCHAR)req + sizeof(struct RcxDrvWriteRequest);
    SIZE_T bytesWritten = 0;
    st = MmCopyVirtualMemory(
        PsGetCurrentProcess(), data,
        proc, (PVOID)req->address,
        (SIZE_T)req->length, KernelMode, &bytesWritten);

    ObDereferenceObject(proc);
    irp->IoStatus.Information = 0;
    return st;
}

/* ── Physical memory read ────────────────────────────────────────── */

static NTSTATUS HandleReadPhys(PIRP irp, PIO_STACK_LOCATION stk)
{
    VALIDATE_INPUT(irp, stk, struct RcxDrvPhysReadRequest);

    struct RcxDrvPhysReadRequest* req = (struct RcxDrvPhysReadRequest*)irp->AssociatedIrp.SystemBuffer;
    if (req->length == 0 || req->length > RCX_DRV_MAX_PHYSICAL)
        return STATUS_INVALID_PARAMETER;
    if (req->width != 0 && req->width != 1 && req->width != 2 && req->width != 4)
        return STATUS_INVALID_PARAMETER;

    VALIDATE_OUTPUT(irp, stk, req->length);

    /* Save request fields before SystemBuffer is overwritten (METHOD_BUFFERED
     * aliases input and output to the same buffer). */
    uint64_t physAddress = req->physAddress;
    ULONG length = req->length;
    ULONG width = req->width;

    PUCHAR dst = (PUCHAR)irp->AssociatedIrp.SystemBuffer;

    if (width == 0) {
        /* Byte copy -- use MmCopyMemory (safe for both RAM and MMIO) */
        MM_COPY_ADDRESS srcAddr;
        srcAddr.PhysicalAddress.QuadPart = (LONGLONG)physAddress;
        SIZE_T bytesCopied = 0;
        NTSTATUS st = MmCopyMemory(dst, srcAddr, (SIZE_T)length,
                                    MM_COPY_MEMORY_PHYSICAL, &bytesCopied);
        if (!NT_SUCCESS(st)) return st;
        if (bytesCopied < length)
            RtlZeroMemory(dst + bytesCopied, length - bytesCopied);
        irp->IoStatus.Information = length;
        return STATUS_SUCCESS;
    }

    /* Width-aware MMIO reads -- map via MDL (safe for all physical addresses).
     * Use MmNonCached: width>0 implies MMIO register access where uncached
     * semantics are required for correct device interaction. */
    PHYS_MAP_CTX mapCtx;
    PUCHAR src = (PUCHAR)MapPhysical(physAddress, (SIZE_T)length, MmNonCached, &mapCtx);
    if (!src) return STATUS_UNSUCCESSFUL;

    __try {
        ULONG off = 0;
        while (off + width <= length) {
            if (width == 1)
                dst[off] = READ_REGISTER_UCHAR(&src[off]);
            else if (width == 2)
                *(USHORT*)(dst + off) = READ_REGISTER_USHORT((PUSHORT)(src + off));
            else
                *(ULONG*)(dst + off) = READ_REGISTER_ULONG((PULONG)(src + off));
            off += width;
        }
        if (off < length)
            RtlZeroMemory(dst + off, length - off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        UnmapPhysical(&mapCtx);
        return STATUS_UNSUCCESSFUL;
    }

    UnmapPhysical(&mapCtx);
    irp->IoStatus.Information = length;
    return STATUS_SUCCESS;
}

/* ── Physical memory write ───────────────────────────────────────── */

static NTSTATUS HandleWritePhys(PIRP irp, PIO_STACK_LOCATION stk)
{
    ULONG inputLen = stk->Parameters.DeviceIoControl.InputBufferLength;
    if (inputLen < sizeof(struct RcxDrvPhysWriteRequest))
        return STATUS_BUFFER_TOO_SMALL;

    struct RcxDrvPhysWriteRequest* req = (struct RcxDrvPhysWriteRequest*)irp->AssociatedIrp.SystemBuffer;
    if (req->length == 0 || req->length > RCX_DRV_MAX_PHYSICAL)
        return STATUS_INVALID_PARAMETER;
    if (req->width != 0 && req->width != 1 && req->width != 2 && req->width != 4)
        return STATUS_INVALID_PARAMETER;
    if (inputLen < sizeof(struct RcxDrvPhysWriteRequest) + req->length)
        return STATUS_BUFFER_TOO_SMALL;

    PUCHAR src = (PUCHAR)req + sizeof(struct RcxDrvPhysWriteRequest);

    /* Map via MDL (safe for both RAM and MMIO).
     * width==0 → RAM byte write (MmCached to avoid cache attribute conflict).
     * width>0  → MMIO register write (MmNonCached for correct device semantics). */
    MEMORY_CACHING_TYPE ct = (req->width == 0) ? MmCached : MmNonCached;
    PHYS_MAP_CTX mapCtx;
    PUCHAR dst = (PUCHAR)MapPhysical(req->physAddress, (SIZE_T)req->length, ct, &mapCtx);
    if (!dst) return STATUS_UNSUCCESSFUL;

    __try {
        if (req->width == 0) {
            RtlCopyMemory(dst, src, req->length);
        } else {
            ULONG off = 0;
            while (off + req->width <= req->length) {
                if (req->width == 1)
                    WRITE_REGISTER_UCHAR(&dst[off], src[off]);
                else if (req->width == 2)
                    WRITE_REGISTER_USHORT((PUSHORT)(dst + off), *(USHORT*)(src + off));
                else
                    WRITE_REGISTER_ULONG((PULONG)(dst + off), *(ULONG*)(src + off));
                off += req->width;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        UnmapPhysical(&mapCtx);
        return STATUS_UNSUCCESSFUL;
    }

    UnmapPhysical(&mapCtx);
    irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

/* ── Ping ────────────────────────────────────────────────────────── */

static NTSTATUS HandlePing(PIRP irp, PIO_STACK_LOCATION stk)
{
    VALIDATE_OUTPUT(irp, stk, sizeof(struct RcxDrvPingResponse));

    struct RcxDrvPingResponse* rsp = (struct RcxDrvPingResponse*)irp->AssociatedIrp.SystemBuffer;
    rsp->version = RCX_DRV_VERSION;
    rsp->driverBuild = __LINE__;
    irp->IoStatus.Information = sizeof(struct RcxDrvPingResponse);
    return STATUS_SUCCESS;
}

/* ── Query PEB ───────────────────────────────────────────────────── */

static NTSTATUS HandleQueryPeb(PIRP irp, PIO_STACK_LOCATION stk)
{
    VALIDATE_INPUT(irp, stk, struct RcxDrvQueryPebRequest);
    VALIDATE_OUTPUT(irp, stk, sizeof(struct RcxDrvQueryPebResponse));

    struct RcxDrvQueryPebRequest* req = (struct RcxDrvQueryPebRequest*)irp->AssociatedIrp.SystemBuffer;
    struct RcxDrvQueryPebResponse* rsp = (struct RcxDrvQueryPebResponse*)irp->AssociatedIrp.SystemBuffer;

    PEPROCESS proc = NULL;
    NTSTATUS st = LookupProcess(req->pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    rsp->pebAddress = (uint64_t)(ULONG_PTR)PsGetProcessPeb(proc);
    rsp->pointerSize = 8;
    rsp->_pad = 0;

    /* Detect WoW64 (32-bit process on 64-bit OS) */
    PVOID wow64 = PsGetProcessWow64Process(proc);
    if (wow64) {
        rsp->pebAddress = (uint64_t)(ULONG_PTR)wow64;
        rsp->pointerSize = 4;
    }

    ObDereferenceObject(proc);
    irp->IoStatus.Information = sizeof(struct RcxDrvQueryPebResponse);
    return STATUS_SUCCESS;
}

/* ── Query Regions ───────────────────────────────────────────────── */

static NTSTATUS HandleQueryRegions(PIRP irp, PIO_STACK_LOCATION stk)
{
    VALIDATE_INPUT(irp, stk, struct RcxDrvQueryRegionsRequest);

    struct RcxDrvQueryRegionsRequest* req = (struct RcxDrvQueryRegionsRequest*)irp->AssociatedIrp.SystemBuffer;
    ULONG outputLen = stk->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG maxEntries = outputLen / sizeof(struct RcxDrvRegionEntry);
    if (maxEntries == 0) return STATUS_BUFFER_TOO_SMALL;

    PEPROCESS proc = NULL;
    NTSTATUS st = LookupProcess(req->pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    /* Attach to target process to query its address space.
     * IOCTLs arrive at PASSIVE_LEVEL; KeStackAttachProcess requires <= APC_LEVEL.
     * ZwQueryVirtualMemory with ZwCurrentProcess() while attached queries the
     * attached process's address space (correct). */
    KAPC_STATE apcState;
    KeStackAttachProcess(proc, &apcState);

    struct RcxDrvRegionEntry* entries = (struct RcxDrvRegionEntry*)irp->AssociatedIrp.SystemBuffer;
    ULONG count = 0;
    PVOID addr = NULL;
    MEMORY_BASIC_INFORMATION_KM mbi;

    while (count < maxEntries) {
        SIZE_T retLen = 0;
        st = ZwQueryVirtualMemory(ZwCurrentProcess(), addr, 0 /*MemoryBasicInformation*/,
                                   &mbi, sizeof(mbi), &retLen);
        if (!NT_SUCCESS(st)) break;

        if (mbi.State == MEM_COMMIT_KM) {
            entries[count].base = (uint64_t)(ULONG_PTR)mbi.BaseAddress;
            entries[count].size = (uint64_t)mbi.RegionSize;
            entries[count].protect = mbi.Protect;
            entries[count].state = mbi.State;
            count++;
        }

        ULONG_PTR next = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
        if (next <= (ULONG_PTR)addr) break; /* overflow */
        addr = (PVOID)next;
    }

    KeUnstackDetachProcess(&apcState);
    ObDereferenceObject(proc);

    irp->IoStatus.Information = count * sizeof(struct RcxDrvRegionEntry);
    return STATUS_SUCCESS;
}

/* ── Query Modules ───────────────────────────────────────────────── */

static NTSTATUS HandleQueryModules(PIRP irp, PIO_STACK_LOCATION stk)
{
    VALIDATE_INPUT(irp, stk, struct RcxDrvQueryModulesRequest);

    struct RcxDrvQueryModulesRequest* req = (struct RcxDrvQueryModulesRequest*)irp->AssociatedIrp.SystemBuffer;
    ULONG outputLen = stk->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG maxEntries = outputLen / sizeof(struct RcxDrvModuleEntry);
    if (maxEntries == 0) return STATUS_BUFFER_TOO_SMALL;

    PEPROCESS proc = NULL;
    NTSTATUS st = LookupProcess(req->pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    /* Attach to target process to read PEB->Ldr */
    KAPC_STATE apcState;
    KeStackAttachProcess(proc, &apcState);

    struct RcxDrvModuleEntry* entries = (struct RcxDrvModuleEntry*)irp->AssociatedIrp.SystemBuffer;
    ULONG count = 0;

    __try {
        /* Read PEB address */
        PEB_KM* peb = (PEB_KM*)PsGetProcessPeb(proc);
        if (!peb) goto done;
        ProbeForRead(peb, sizeof(PEB_KM), 1);

        /* PEB->Ldr at offset 0x18 (x64) */
        PEB_LDR_DATA_KM* ldr = peb->Ldr;
        if (!ldr) goto done;
        ProbeForRead(ldr, sizeof(PEB_LDR_DATA_KM), 1);

        /* Walk InLoadOrderModuleList */
        LIST_ENTRY* head = &ldr->InLoadOrderModuleList;
        LIST_ENTRY* cur  = head->Flink;

        while (cur != head && count < maxEntries) {
            LDR_DATA_TABLE_ENTRY_KM* entry = CONTAINING_RECORD(cur, LDR_DATA_TABLE_ENTRY_KM, InLoadOrderLinks);

            entries[count].base = (uint64_t)(ULONG_PTR)entry->DllBase;
            entries[count].size = (uint64_t)entry->SizeOfImage;

            /* Copy wide-char name (truncate to 259 chars + null) */
            USHORT nameLen = entry->BaseDllName.Length / sizeof(WCHAR);
            if (nameLen > 259) nameLen = 259;
            if (entry->BaseDllName.Buffer) {
                RtlCopyMemory(entries[count].name, entry->BaseDllName.Buffer,
                              nameLen * sizeof(uint16_t));
            }
            entries[count].name[nameLen] = 0;

            count++;
            cur = cur->Flink;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* Partial results are fine */
    }

done:
    KeUnstackDetachProcess(&apcState);
    ObDereferenceObject(proc);

    irp->IoStatus.Information = count * sizeof(struct RcxDrvModuleEntry);
    return STATUS_SUCCESS;
}

/* ── Query TEBs ──────────────────────────────────────────────────── */

/*
 * Walk the target process's thread list to collect TEB addresses.
 * Uses PsGetNextProcessThread (undocumented but stable since Vista).
 */
static NTSTATUS HandleQueryTebs(PIRP irp, PIO_STACK_LOCATION stk)
{
    VALIDATE_INPUT(irp, stk, struct RcxDrvQueryTebsRequest);

    struct RcxDrvQueryTebsRequest* req = (struct RcxDrvQueryTebsRequest*)irp->AssociatedIrp.SystemBuffer;
    ULONG outputLen = stk->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG maxEntries = outputLen / sizeof(struct RcxDrvTebEntry);
    if (maxEntries == 0) return STATUS_BUFFER_TOO_SMALL;

    PEPROCESS proc = NULL;
    NTSTATUS st = LookupProcess(req->pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    struct RcxDrvTebEntry* entries = (struct RcxDrvTebEntry*)irp->AssociatedIrp.SystemBuffer;
    ULONG count = 0;

    if (!g_PsGetNextProcessThread) {
        ObDereferenceObject(proc);
        return STATUS_NOT_SUPPORTED;
    }

    /* PsGetNextProcessThread increments the ref on the returned PETHREAD and
     * dereferences the previous one.  We must release the last thread if we
     * exit the loop early (exception or maxEntries hit). */
    {
        PETHREAD thread = NULL;
        __try {
            while ((thread = g_PsGetNextProcessThread(proc, thread)) != NULL) {
                if (count >= maxEntries) {
                    /* Hit limit — release the thread PsGetNextProcessThread just returned */
                    ObDereferenceObject(thread);
                    break;
                }
                PVOID teb = PsGetThreadTeb(thread);
                if (teb) {
                    entries[count].tebAddress = (uint64_t)(ULONG_PTR)teb;
                    entries[count].threadId = (uint32_t)(ULONG_PTR)PsGetThreadId(thread);
                    entries[count]._pad = 0;
                    count++;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* Exception mid-iteration: thread holds a referenced PETHREAD — release it */
            if (thread)
                ObDereferenceObject(thread);
        }
    }

    ObDereferenceObject(proc);

    irp->IoStatus.Information = count * sizeof(struct RcxDrvTebEntry);
    return STATUS_SUCCESS;
}

/* ── Read CR3 (DirectoryTableBase) ────────────────────────────────── */

/*
 * EPROCESS.DirectoryTableBase offset.  Stable across Win10/11 x64.
 * Verified: 0x028 on 1507-22H2+ (KPROCESS is at offset 0 of EPROCESS).
 */
#define KPROCESS_DIRECTORY_TABLE_BASE  0x028

static NTSTATUS HandleReadCr3(PIRP irp, PIO_STACK_LOCATION stk)
{
    VALIDATE_INPUT(irp, stk, struct RcxDrvReadCr3Request);
    VALIDATE_OUTPUT(irp, stk, sizeof(struct RcxDrvReadCr3Response));

    struct RcxDrvReadCr3Request* req = (struct RcxDrvReadCr3Request*)irp->AssociatedIrp.SystemBuffer;
    struct RcxDrvReadCr3Response* rsp = (struct RcxDrvReadCr3Response*)irp->AssociatedIrp.SystemBuffer;

    PEPROCESS proc = NULL;
    NTSTATUS st = LookupProcess(req->pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    __try {
        rsp->cr3 = *(uint64_t*)((PUCHAR)proc + KPROCESS_DIRECTORY_TABLE_BASE);
        /* Mask off PCID bits (bits 0-11) to get the PML4 physical address */
        rsp->cr3 &= ~0xFFFULL;
        rsp->kernelCr3 = rsp->cr3; /* same on non-KPTI; KPTI shadow is not easily accessible */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ObDereferenceObject(proc);
        return STATUS_UNSUCCESSFUL;
    }

    ObDereferenceObject(proc);
    irp->IoStatus.Information = sizeof(struct RcxDrvReadCr3Response);
    return STATUS_SUCCESS;
}

/* ── Virtual-to-Physical address translation ─────────────────────── */

/* NOTE: This walks the page table non-atomically via 4 sequential physical reads.
 * The page table can be modified between reads (e.g., page-out, remap).  This is
 * an inherent limitation shared by WinDbg's !vtop and similar tools.  For a
 * debugging/reversing tool this tradeoff is acceptable. */

/* Extract physical frame address from a page table entry (bits 51:12) */
#define PTE_FRAME(pte)  ((pte) & 0x000FFFFFFFFFF000ULL)
/* Check Present bit (bit 0) */
#define PTE_PRESENT(pte) ((pte) & 1ULL)
/* Check Page Size bit (bit 7) -- indicates large/huge page */
#define PTE_PS(pte)      ((pte) & (1ULL << 7))

static NTSTATUS HandleVtop(PIRP irp, PIO_STACK_LOCATION stk)
{
    VALIDATE_INPUT(irp, stk, struct RcxDrvVtopRequest);
    VALIDATE_OUTPUT(irp, stk, sizeof(struct RcxDrvVtopResponse));

    struct RcxDrvVtopRequest* req = (struct RcxDrvVtopRequest*)irp->AssociatedIrp.SystemBuffer;
    struct RcxDrvVtopResponse* rsp = (struct RcxDrvVtopResponse*)irp->AssociatedIrp.SystemBuffer;

    PEPROCESS proc = NULL;
    NTSTATUS st = LookupProcess(req->pid, &proc);
    if (!NT_SUCCESS(st)) return st;

    /* Read CR3 */
    uint64_t cr3;
    __try {
        cr3 = *(uint64_t*)((PUCHAR)proc + KPROCESS_DIRECTORY_TABLE_BASE);
        cr3 &= ~0xFFFULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ObDereferenceObject(proc);
        return STATUS_UNSUCCESSFUL;
    }
    ObDereferenceObject(proc);

    uint64_t va = req->virtualAddress;
    RtlZeroMemory(rsp, sizeof(*rsp));

    /* Extract indices from virtual address:
     * [47:39] = PML4 index, [38:30] = PDPT index,
     * [29:21] = PD index,   [20:12] = PT index,
     * [11:0]  = page offset */
    ULONG pml4Idx = (ULONG)((va >> 39) & 0x1FF);
    ULONG pdptIdx = (ULONG)((va >> 30) & 0x1FF);
    ULONG pdIdx   = (ULONG)((va >> 21) & 0x1FF);
    ULONG ptIdx   = (ULONG)((va >> 12) & 0x1FF);

    MM_COPY_ADDRESS ca;
    SIZE_T copied;
    uint64_t entry;

    /* Level 4: PML4 -- use MmCopyMemory (safe for RAM, unlike MmMapIoSpace) */
    ca.PhysicalAddress.QuadPart = (LONGLONG)(cr3 + pml4Idx * 8);
    st = MmCopyMemory(&entry, ca, 8, MM_COPY_MEMORY_PHYSICAL, &copied);
    if (!NT_SUCCESS(st) || copied < 8) return STATUS_UNSUCCESSFUL;
    rsp->pml4e = entry;
    if (!PTE_PRESENT(entry)) { rsp->valid = 0; goto done; }

    /* Level 3: PDPT */
    ca.PhysicalAddress.QuadPart = (LONGLONG)(PTE_FRAME(entry) + pdptIdx * 8);
    st = MmCopyMemory(&entry, ca, 8, MM_COPY_MEMORY_PHYSICAL, &copied);
    if (!NT_SUCCESS(st) || copied < 8) return STATUS_UNSUCCESSFUL;
    rsp->pdpte = entry;
    if (!PTE_PRESENT(entry)) { rsp->valid = 0; goto done; }
    if (PTE_PS(entry)) {
        /* 1GB huge page: physical = frame[51:30] | va[29:0] */
        rsp->physicalAddress = (entry & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL);
        rsp->pageSize = 2;
        rsp->valid = 1;
        goto done;
    }

    /* Level 2: PD */
    ca.PhysicalAddress.QuadPart = (LONGLONG)(PTE_FRAME(entry) + pdIdx * 8);
    st = MmCopyMemory(&entry, ca, 8, MM_COPY_MEMORY_PHYSICAL, &copied);
    if (!NT_SUCCESS(st) || copied < 8) return STATUS_UNSUCCESSFUL;
    rsp->pde = entry;
    if (!PTE_PRESENT(entry)) { rsp->valid = 0; goto done; }
    if (PTE_PS(entry)) {
        /* 2MB large page: physical = frame[51:21] | va[20:0] */
        rsp->physicalAddress = (entry & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL);
        rsp->pageSize = 1;
        rsp->valid = 1;
        goto done;
    }

    /* Level 1: PT */
    ca.PhysicalAddress.QuadPart = (LONGLONG)(PTE_FRAME(entry) + ptIdx * 8);
    st = MmCopyMemory(&entry, ca, 8, MM_COPY_MEMORY_PHYSICAL, &copied);
    if (!NT_SUCCESS(st) || copied < 8) return STATUS_UNSUCCESSFUL;
    rsp->pte = entry;
    if (!PTE_PRESENT(entry)) { rsp->valid = 0; goto done; }

    /* 4KB page: physical = frame[51:12] | va[11:0] */
    rsp->physicalAddress = PTE_FRAME(entry) | (va & 0xFFFULL);
    rsp->pageSize = 0;
    rsp->valid = 1;

done:
    irp->IoStatus.Information = sizeof(struct RcxDrvVtopResponse);
    return STATUS_SUCCESS;
}

/* ── IOCTL dispatch ──────────────────────────────────────────────── */

static NTSTATUS DispatchIoctl(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);

    PIO_STACK_LOCATION stk = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st;

    switch (stk->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_RCX_READ_MEMORY:     st = HandleReadMemory(irp, stk);  break;
    case IOCTL_RCX_WRITE_MEMORY:    st = HandleWriteMemory(irp, stk); break;
    case IOCTL_RCX_QUERY_REGIONS:   st = HandleQueryRegions(irp, stk); break;
    case IOCTL_RCX_QUERY_PEB:       st = HandleQueryPeb(irp, stk);     break;
    case IOCTL_RCX_QUERY_MODULES:   st = HandleQueryModules(irp, stk); break;
    case IOCTL_RCX_QUERY_TEBS:      st = HandleQueryTebs(irp, stk);   break;
    case IOCTL_RCX_PING:            st = HandlePing(irp, stk);        break;
    case IOCTL_RCX_READ_PHYS:       st = HandleReadPhys(irp, stk);   break;
    case IOCTL_RCX_WRITE_PHYS:      st = HandleWritePhys(irp, stk);  break;
    case IOCTL_RCX_READ_CR3:        st = HandleReadCr3(irp, stk);    break;
    case IOCTL_RCX_VTOP:            st = HandleVtop(irp, stk);       break;
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        irp->IoStatus.Information = 0;
        break;
    }

    irp->IoStatus.Status = st;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/* ── Create / Close (permit open/close) ──────────────────────────── */

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* ── Unload ──────────────────────────────────────────────────────── */

void DriverUnload(PDRIVER_OBJECT drv)
{
    UNICODE_STRING symlink = RTL_CONSTANT_STRING(L"\\DosDevices\\RcxDrv");
    IoDeleteSymbolicLink(&symlink);
    if (drv->DeviceObject)
        IoDeleteDevice(drv->DeviceObject);
}

/* ── Entry point ─────────────────────────────────────────────────── */

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING regPath)
{
    UNREFERENCED_PARAMETER(regPath);

    /* Resolve undocumented APIs */
    UNICODE_STRING fnName = RTL_CONSTANT_STRING(L"PsGetNextProcessThread");
    g_PsGetNextProcessThread = (PsGetNextProcessThread_t)MmGetSystemRoutineAddress(&fnName);

    UNICODE_STRING devName  = RTL_CONSTANT_STRING(L"\\Device\\RcxDrv");
    UNICODE_STRING symlink  = RTL_CONSTANT_STRING(L"\\DosDevices\\RcxDrv");

    PDEVICE_OBJECT devObj = NULL;
    NTSTATUS st = IoCreateDevice(drv, 0, &devName, FILE_DEVICE_UNKNOWN,
                                  FILE_DEVICE_SECURE_OPEN, FALSE, &devObj);
    if (!NT_SUCCESS(st)) return st;

    st = IoCreateSymbolicLink(&symlink, &devName);
    if (!NT_SUCCESS(st)) {
        IoDeleteDevice(devObj);
        return st;
    }

    drv->MajorFunction[IRP_MJ_CREATE]         = DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE]          = DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchIoctl;
    drv->DriverUnload = DriverUnload;

    devObj->Flags |= DO_BUFFERED_IO;
    devObj->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}
