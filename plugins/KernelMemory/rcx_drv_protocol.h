/*
 * RCX Driver Protocol -- shared between kernel driver and usermode plugin.
 * No dependencies beyond standard C headers.  Pure C, no Windows types.
 */
#pragma once

#ifdef KERNEL
/* Kernel mode build: avoid stdint.h (not in WDK km/crt) */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned __int64   uint64_t;
typedef signed   __int64   int64_t;
#else
#include <stdint.h>
#endif

/* ── Device / service names ───────────────────────────────────────── */
#define RCX_DRV_DEVICE_NAME     L"\\Device\\RcxDrv"
#define RCX_DRV_SYMLINK_NAME    L"\\DosDevices\\RcxDrv"
#define RCX_DRV_USERMODE_PATH   "\\\\.\\RcxDrv"
#define RCX_DRV_SERVICE_NAME    "RcxDrv"

/* ── Protocol version ─────────────────────────────────────────────── */
#define RCX_DRV_VERSION         1

/* ── Size limits ──────────────────────────────────────────────────── */
#define RCX_DRV_MAX_VIRTUAL     (1024 * 1024)   /* 1 MB per virtual read/write  */
#define RCX_DRV_MAX_PHYSICAL    4096             /* 4 KB per physical read/write */

/* ── IOCTL codes ──────────────────────────────────────────────────── */
/* CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, function, METHOD_BUFFERED=0, FILE_ANY_ACCESS=0) */

/* Virtual memory (per-process) */
#define IOCTL_RCX_READ_MEMORY     0x222000  /* function 0x800 */
#define IOCTL_RCX_WRITE_MEMORY    0x222004  /* function 0x801 */
#define IOCTL_RCX_QUERY_REGIONS   0x222008  /* function 0x802 */
#define IOCTL_RCX_QUERY_PEB       0x22200C  /* function 0x803 */
#define IOCTL_RCX_QUERY_MODULES   0x222010  /* function 0x804 */
#define IOCTL_RCX_QUERY_TEBS      0x222014  /* function 0x805 */
#define IOCTL_RCX_PING            0x222018  /* function 0x806 */

/* Physical memory (MMIO) */
#define IOCTL_RCX_READ_PHYS       0x22201C  /* function 0x807 */
#define IOCTL_RCX_WRITE_PHYS      0x222020  /* function 0x808 */

/* Paging / address translation */
#define IOCTL_RCX_READ_CR3        0x222044  /* function 0x811 */
#define IOCTL_RCX_VTOP            0x222048  /* function 0x812 */

/* ── Request / Response structures ────────────────────────────────── */
/* All structs are naturally aligned.  Padding fields are explicit.     */

/* -- Virtual memory -- */

struct RcxDrvReadRequest {
    uint32_t pid;
    uint32_t _pad0;
    uint64_t address;
    uint32_t length;        /* max RCX_DRV_MAX_VIRTUAL */
    uint32_t _pad1;
};

/* Write: input = header + inline data bytes */
struct RcxDrvWriteRequest {
    uint32_t pid;
    uint32_t _pad0;
    uint64_t address;
    uint32_t length;        /* max RCX_DRV_MAX_VIRTUAL */
    uint32_t _pad1;
    /* uint8_t data[length] follows */
};

/* -- Region enumeration -- */

struct RcxDrvQueryRegionsRequest {
    uint32_t pid;
    uint32_t _pad;
};

struct RcxDrvRegionEntry {
    uint64_t base;
    uint64_t size;
    uint32_t protect;       /* raw PAGE_* flags */
    uint32_t state;         /* MEM_COMMIT etc.  */
};

/* -- PEB -- */

struct RcxDrvQueryPebRequest {
    uint32_t pid;
    uint32_t _pad;
};

struct RcxDrvQueryPebResponse {
    uint64_t pebAddress;
    uint32_t pointerSize;   /* 4 or 8 */
    uint32_t _pad;
};

/* -- Modules -- */

struct RcxDrvQueryModulesRequest {
    uint32_t pid;
    uint32_t _pad;
};

struct RcxDrvModuleEntry {
    uint64_t base;
    uint64_t size;
    uint16_t name[260];     /* wide-char, null-terminated */
};

/* -- TEBs -- */

struct RcxDrvQueryTebsRequest {
    uint32_t pid;
    uint32_t _pad;
};

struct RcxDrvTebEntry {
    uint64_t tebAddress;
    uint32_t threadId;
    uint32_t _pad;
};

/* -- Ping -- */

struct RcxDrvPingResponse {
    uint32_t version;
    uint32_t driverBuild;
};

/* -- Physical memory -- */

struct RcxDrvPhysReadRequest {
    uint64_t physAddress;
    uint32_t length;        /* max RCX_DRV_MAX_PHYSICAL */
    uint32_t width;         /* access width: 1, 2, or 4 (0 = memcpy) */
};

struct RcxDrvPhysWriteRequest {
    uint64_t physAddress;
    uint32_t length;        /* max RCX_DRV_MAX_PHYSICAL */
    uint32_t width;         /* access width: 1, 2, or 4 (0 = memcpy) */
    /* uint8_t data[length] follows */
};

/* -- Paging / address translation -- */

struct RcxDrvReadCr3Request {
    uint32_t pid;
    uint32_t _pad;
};

struct RcxDrvReadCr3Response {
    uint64_t cr3;           /* DirectoryTableBase (PML4 physical address) */
    uint64_t kernelCr3;     /* KernelDirectoryTableBase (KPTI shadow) */
};

struct RcxDrvVtopRequest {
    uint32_t pid;
    uint32_t _pad;
    uint64_t virtualAddress;
};

struct RcxDrvVtopResponse {
    uint64_t physicalAddress;   /* final translated physical address (with page offset) */
    uint64_t pml4e;             /* raw PML4 entry value */
    uint64_t pdpte;             /* raw PDPT entry value */
    uint64_t pde;               /* raw PD entry value */
    uint64_t pte;               /* raw PT entry value (0 if large/huge page) */
    uint8_t  pageSize;          /* 0=4KB, 1=2MB, 2=1GB */
    uint8_t  valid;             /* 1 if translation succeeded, 0 if not present */
    uint8_t  _pad2[6];
};

/* ── Compile-time validation ──────────────────────────────────────── */
#ifdef __cplusplus
static_assert(sizeof(RcxDrvReadRequest) == 24, "ReadRequest layout");
static_assert(sizeof(RcxDrvWriteRequest) == 24, "WriteRequest layout");
static_assert(sizeof(RcxDrvRegionEntry) == 24, "RegionEntry layout");
static_assert(sizeof(RcxDrvModuleEntry) == 536, "ModuleEntry layout");
static_assert(sizeof(RcxDrvTebEntry) == 16, "TebEntry layout");
static_assert(sizeof(RcxDrvPingResponse) == 8, "PingResponse layout");
static_assert(sizeof(RcxDrvReadCr3Response) == 16, "ReadCr3Response layout");
static_assert(sizeof(RcxDrvVtopRequest) == 16, "VtopRequest layout");
static_assert(sizeof(RcxDrvVtopResponse) == 48, "VtopResponse layout");
#endif
