/*
 * RCX RPC Protocol  --  shared between plugin DLL and payload DLL/SO.
 * No dependencies beyond standard C headers.
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── constants ─────────────────────────────────────────────────────── */
#define RCX_RPC_VERSION       1
#define RCX_RPC_MAX_BATCH     256
#define RCX_RPC_SHM_SIZE      (1024 * 1024)     /* 1 MB                */
#define RCX_RPC_HEADER_SIZE   4096
#define RCX_RPC_DATA_OFFSET   RCX_RPC_HEADER_SIZE
#define RCX_RPC_DATA_SIZE     (RCX_RPC_SHM_SIZE - RCX_RPC_DATA_OFFSET)

/* status codes */
#define RCX_RPC_STATUS_OK       0
#define RCX_RPC_STATUS_ERROR    1
#define RCX_RPC_STATUS_PARTIAL  2

/* ── commands ──────────────────────────────────────────────────────── */
#ifdef __cplusplus
enum RcxRpcCommand : uint32_t {
#else
typedef uint32_t RcxRpcCommand;
enum {
#endif
    RPC_CMD_NONE         = 0,
    RPC_CMD_READ_BATCH   = 1,   /* batch read: N {address, length} pairs  */
    RPC_CMD_WRITE        = 2,   /* single write                           */
    RPC_CMD_ENUM_MODULES = 3,   /* enumerate loaded modules               */
    RPC_CMD_PING         = 4,   /* heartbeat                              */
    RPC_CMD_SHUTDOWN     = 5,   /* graceful teardown                      */
};

/* ── wire structs (natural alignment, verified by static_assert) ─── */

struct RcxRpcReadEntry {
    uint64_t address;
    uint32_t length;
    uint32_t dataOffset;   /* offset into data region for response bytes */
};

struct RcxRpcModuleEntry {
    uint64_t base;
    uint64_t size;
    uint32_t nameOffset;   /* offset into data region, UTF-16 on Win, UTF-8 on Linux */
    uint32_t nameLength;   /* in bytes */
};

/*
 * Header -- lives at shared-memory offset 0, padded to 4096 bytes.
 *
 *   offset  field
 *   ------  -----
 *     0     version          (4)
 *     4     payloadReady     (4)
 *     8     command          (4)
 *    12     requestCount     (4)
 *    16     writeAddress     (8)
 *    24     writeLength      (4)
 *    28     status           (4)
 *    32     responseCount    (4)
 *    36     totalDataUsed    (4)
 *    40     imageBase        (8)  -- main module base from PEB / procfs
 *    48     pointerSize      (4)  -- 4 for 32-bit, 8 for 64-bit payload
 *    52     _pad[4044]
 */
struct RcxRpcHeader {
    uint32_t version;
    uint32_t payloadReady;     /* payload sets to 1 after init */
    uint32_t command;          /* RcxRpcCommand                */
    uint32_t requestCount;
    uint64_t writeAddress;
    uint32_t writeLength;
    uint32_t status;           /* RCX_RPC_STATUS_*             */
    uint32_t responseCount;
    uint32_t totalDataUsed;
    uint64_t imageBase;        /* main module base (PEB on Win, /proc on Linux) */
    uint32_t pointerSize;      /* 4 for 32-bit, 8 for 64-bit payload           */
    uint8_t  _pad[RCX_RPC_HEADER_SIZE - 52];
};

/* ── name formatting helpers (PID-only, no nonce) ─────────────────── */

static inline void rcx_rpc_shm_name(char* buf, int n, uint32_t pid) {
#ifdef _WIN32
    snprintf(buf, n, "Local\\RCX_SHM_%u", pid);
#else
    snprintf(buf, n, "/rcx_shm_%u", pid);
#endif
}

static inline void rcx_rpc_req_name(char* buf, int n, uint32_t pid) {
#ifdef _WIN32
    snprintf(buf, n, "Local\\RCX_REQ_%u", pid);
#else
    snprintf(buf, n, "/rcx_req_%u", pid);
#endif
}

static inline void rcx_rpc_rsp_name(char* buf, int n, uint32_t pid) {
#ifdef _WIN32
    snprintf(buf, n, "Local\\RCX_RSP_%u", pid);
#else
    snprintf(buf, n, "/rcx_rsp_%u", pid);
#endif
}

#ifdef __cplusplus
static_assert(sizeof(RcxRpcHeader) == RCX_RPC_HEADER_SIZE, "Header must be 4096 bytes");
#endif
