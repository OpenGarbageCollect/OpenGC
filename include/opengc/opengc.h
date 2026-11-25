/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 OpenGC contributors */
#ifndef OPENGC_H
#define OPENGC_H
#include "protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OgcDomain OgcDomain;
typedef struct OgcDomainConfig {
    uint32_t struct_size;
    uint32_t abi_major;
    const OgcProvider* provider;
    OgcAllocator data_allocator;
    OgcAllocator metadata_allocator;
    uint64_t data_limit;
    uint64_t metadata_limit;
    uint64_t scratch_limit;
} OgcDomainConfig;
typedef struct OgcLayout {
    uint64_t bytes;
    uint64_t alignment;
    const uint64_t* reference_offsets;
    uint64_t reference_count;
} OgcLayout;
typedef struct OgcStats {
    uint64_t domain_id;
    uint64_t published_objects;
    uint64_t constructing_objects;
    uint64_t roots;
    uint64_t data_bytes;
    uint64_t metadata_bytes;
    uint64_t completed_collections;
} OgcStats;

const OgcAllocator* OGC_CALL ogc_system_allocator(void);
OgcStatus OGC_CALL ogc_query_full(const OgcQuery*, const OgcProvider**);
OgcStatus OGC_CALL ogc_query_retain(const OgcQuery*, const OgcProvider**);

/* All limits are required byte caps; UINT64_MAX means no configured cap.
 * Tables are copied; callback code and contexts must outlive domain destruction. */
OgcStatus OGC_CALL ogc_domain_create(const OgcDomainConfig*, OgcDomain**);
/* Busy with outstanding roots or constructions. Success nulls the pointer. */
OgcStatus OGC_CALL ogc_domain_destroy(OgcDomain**);
/* Copies offsets and zeroes payload. Identity is never recycled. */
OgcStatus OGC_CALL ogc_construct(OgcDomain*, const OgcLayout*, OgcObjectInfo*);
OgcStatus OGC_CALL ogc_abort(OgcDomain*, OgcId);
OgcStatus OGC_CALL ogc_publish_batch(OgcDomain*, const OgcId*, uint64_t count);
/* Only declared reference slots are writable here; scalars are caller-owned.
 * Constructing sources may refer to constructions; published sources may not. */
OgcStatus OGC_CALL ogc_store(OgcDomain*, OgcId owner, uint64_t offset, const void* target);
/* Exact lookup rejects interior pointers; interior lookup never dereferences them.
 * Nonmatches succeed with a zero object. Only published objects are visible. */
OgcStatus OGC_CALL ogc_inspect(OgcDomain*, uintptr_t address, uint32_t kind, OgcObjectInfo*);
/* Roots own copied edges, not borrowed stack slots. Use root_set to update them. */
OgcStatus OGC_CALL ogc_root_create(OgcDomain*, OgcEdge, OgcId* root);
OgcStatus OGC_CALL ogc_root_set(OgcDomain*, OgcId root, OgcEdge);
OgcStatus OGC_CALL ogc_root_remove(OgcDomain*, OgcId root);
/* Explicit whole-domain maintenance; allocation and stores never collect. */
OgcStatus OGC_CALL ogc_collect(OgcDomain*, OgcCollection*);
OgcStatus OGC_CALL ogc_stats(OgcDomain*, OgcStats*);
#ifdef __cplusplus
}
#endif
#endif
