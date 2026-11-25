/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 OpenGC contributors */
#ifndef OPENGC_PROTOCOL_H
#define OPENGC_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#if defined(_MSC_VER)
#define OGC_CALL __cdecl
#else
#define OGC_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define OGC_ABI_MAJOR UINT32_C(1)
#define OGC_ABI_MINOR UINT32_C(0)
typedef uint32_t OgcStatus;
#define OGC_OK UINT32_C(0)
#define OGC_INVALID_ARGUMENT UINT32_C(1)
#define OGC_INCOMPATIBLE_ABI UINT32_C(2)
#define OGC_UNSUPPORTED UINT32_C(3)
#define OGC_WRONG_OWNER UINT32_C(4)
#define OGC_BUSY UINT32_C(5)
#define OGC_STALE_HANDLE UINT32_C(6)
#define OGC_INVALID_EDGE UINT32_C(7)
#define OGC_SIZE_OVERFLOW UINT32_C(8)
#define OGC_BUDGET_EXCEEDED UINT32_C(9)
#define OGC_ALLOCATION_FAILED UINT32_C(10)
#define OGC_ID_EXHAUSTED UINT32_C(11)
#define OGC_SCAN_FAILURE UINT32_C(12)

#define OGC_CAP_STABLE_ADDRESSES (UINT64_C(1) << 0)
#define OGC_CAP_EXPLICIT_FULL (UINT64_C(1) << 1)
#define OGC_CAP_RETAIN (UINT64_C(1) << 2)
#define OGC_KNOWN_CAPABILITIES (OGC_CAP_STABLE_ADDRESSES | OGC_CAP_EXPLICIT_FULL | OGC_CAP_RETAIN)
#define OGC_EDGE_EXACT UINT32_C(0)
#define OGC_EDGE_INTERIOR UINT32_C(1)
#define OGC_COLLECTED UINT32_C(0)
#define OGC_DISABLED UINT32_C(1)

typedef struct OgcAbi {
    uint32_t struct_size;
    uint32_t major;
    uint32_t minor;
    uint32_t pointer_bytes;
    uint64_t capabilities;
} OgcAbi;
typedef struct OgcQuery {
    uint32_t struct_size;
    uint32_t major;
    uint32_t minimum_minor;
    uint32_t pointer_bytes;
    uint64_t required_capabilities;
} OgcQuery;

/* A receipt is returned unchanged to its source allocator exactly once. */
typedef struct OgcAllocation {
    void* base;
    void* token;
    uint64_t bytes;
    uint64_t alignment;
} OgcAllocation;
typedef struct OgcAllocator {
    uint32_t struct_size;
    uint32_t abi_major;
    void* context;
    OgcStatus (OGC_CALL *allocate)(void*, uint64_t, uint64_t, OgcAllocation*);
    void (OGC_CALL *deallocate)(void*, OgcAllocation);
} OgcAllocator;

typedef struct OgcId {
    uint64_t domain;
    uint64_t serial;
} OgcId;
typedef struct OgcObjectInfo {
    OgcId id;
    void* base;
    uint64_t bytes;
    uint64_t reference_count;
} OgcObjectInfo;
typedef struct OgcEdge {
    uintptr_t address;
    uint32_t kind;
    uint32_t reserved;
} OgcEdge;

/* Streaming cursors are opaque, valid only inside one exclusive begin/end lease.
 * next returns one entry, or has_value=0 at end, and never calls the mutator. */
typedef struct OgcServices {
    uint32_t struct_size;
    uint32_t abi_major;
    void* context;
    OgcStatus (OGC_CALL *begin)(void*, uint64_t* object_count);
    void (OGC_CALL *end)(void*);
    OgcStatus (OGC_CALL *object_next)(void*, uint64_t* cursor, OgcObjectInfo*, uint32_t* has_value);
    OgcStatus (OGC_CALL *root_next)(void*, uint64_t* cursor, OgcEdge*, uint32_t* has_value);
    OgcStatus (OGC_CALL *trace_edge)(void*, OgcId, uint64_t index, OgcEdge*);
    OgcStatus (OGC_CALL *retire)(void*, OgcId);
} OgcServices;

/* Zero on entry; once a lease is acquired, failures report the completed prefix.
 * No retirement is allowed before the entire reachable graph is validated. */
typedef struct OgcCollection {
    uint64_t examined_objects;
    uint64_t examined_edges;
    uint64_t live_objects;
    uint64_t retired_objects;
    uint64_t retired_bytes;
    uint32_t outcome;
    uint32_t reserved;
} OgcCollection;
typedef struct OgcProvider {
    OgcAbi abi;
    OgcStatus (OGC_CALL *collect)(const OgcServices*, const OgcAllocator* metadata,
                                 uint64_t scratch_limit, OgcCollection*);
} OgcProvider;
typedef OgcStatus (OGC_CALL *OgcQueryProvider)(const OgcQuery*, const OgcProvider**);

/* The caller supplies at least one readable struct_size word for each table. */
static inline OgcStatus ogc_validate_provider(const OgcQuery* query, const OgcProvider* provider) {
    if (!query || !provider) return OGC_INVALID_ARGUMENT;
    if (query->struct_size < sizeof(OgcQuery) || provider->abi.struct_size < sizeof(OgcProvider))
        return OGC_INCOMPATIBLE_ABI;
    if (query->major != OGC_ABI_MAJOR || provider->abi.major != query->major ||
        provider->abi.minor < query->minimum_minor || query->pointer_bytes != 8 ||
        provider->abi.pointer_bytes != 8 || sizeof(uintptr_t) != 8) return OGC_INCOMPATIBLE_ABI;
    if ((query->required_capabilities & ~OGC_KNOWN_CAPABILITIES) ||
        (provider->abi.capabilities & query->required_capabilities) != query->required_capabilities ||
        !(provider->abi.capabilities & OGC_CAP_STABLE_ADDRESSES)) return OGC_UNSUPPORTED;
    return provider->collect ? OGC_OK : OGC_INVALID_ARGUMENT;
}
static inline OgcStatus ogc_validate_allocator(const OgcAllocator* allocator) {
    if (!allocator) return OGC_INVALID_ARGUMENT;
    if (allocator->struct_size < sizeof(OgcAllocator) || allocator->abi_major != OGC_ABI_MAJOR)
        return OGC_INCOMPATIBLE_ABI;
    return allocator->allocate && allocator->deallocate ? OGC_OK : OGC_INVALID_ARGUMENT;
}
static inline OgcStatus ogc_validate_services(const OgcServices* services) {
    if (!services) return OGC_INVALID_ARGUMENT;
    if (services->struct_size < sizeof(OgcServices) || services->abi_major != OGC_ABI_MAJOR)
        return OGC_INCOMPATIBLE_ABI;
    return services->begin && services->end && services->object_next && services->root_next &&
        services->trace_edge && services->retire ? OGC_OK : OGC_INVALID_ARGUMENT;
}

#ifdef __cplusplus
}
#endif
#endif
