/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENGC_POLICY_H
#define OPENGC_POLICY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define OGC_POLICY_ABI 1u
#define OGC_POLICY_COLLECTS 1u
#define OGC_POLICY_MINOR 2u
#define OGC_POLICY_STEP 4u
#define OGC_POLICY_MOVING 8u
#define OGC_POLICY_PINNED 1u
/* Allocator-owned, stable record. Links are algorithm work queues, never an
 * ownership registry. bytes is supplied by the allocator; reserved is its own. */
typedef struct OgcPolicyRecord {
    struct OgcPolicyRecord *queue_next, *dirty_next;
    uint64_t bytes, mark_epoch, minor_epoch, dirty_epoch;
    uint8_t old, dead, reserved[6];
} OgcPolicyRecord;
typedef struct OgcPolicyState {
    OgcPolicyRecord *queue, *remembered;
    uint64_t epoch, completed_epoch, minor_epoch, dirty_epoch;
    uint64_t live_charge, old_charge;
    uint8_t marking;
} OgcPolicyState;
/* Shared preset operations belong to the allocator, not policy callbacks.
 * A new record is zeroed by its owner, bytes is set before birth publication.
 * New allocations are black during marking; NoGC leaves epochs at zero. */
static inline int ogc_policy_record_live(const OgcPolicyState* s, const OgcPolicyRecord* n) {
    return n && !n->dead && n->mark_epoch >= s->completed_epoch;
}
static inline void ogc_policy_record_allocated(OgcPolicyState* s, OgcPolicyRecord* n) {
    n->mark_epoch = s->epoch; n->minor_epoch = n->dirty_epoch = 0;
    n->old = s->marking; n->dead = 0;
    if (s->marking) s->live_charge += (n->bytes + 8 + 15) & ~(uint64_t)15;
}
/* slot is writable for relocating edges; offset is relative to the allocator's
 * current payload. PINNED candidates must not be rewritten or moved. */
typedef struct OgcPolicyEdge {
    OgcPolicyRecord* target;
    void* slot;
    intptr_t offset;
    uint32_t flags;
} OgcPolicyEdge;
typedef void (*OgcPolicyVisit)(void*, const OgcPolicyEdge*);
typedef void (*OgcPolicyVisitRecord)(void*, OgcPolicyRecord*);
typedef struct OgcPolicyServices {
    uint32_t abi, size;
    void* context;
    void (*roots)(void*, OgcPolicyVisit, void*);
    void (*edges)(void*, OgcPolicyRecord*, OgcPolicyVisit, void*);
    void (*young)(void*, OgcPolicyVisitRecord, void*);
    /* Allocator retires according to record_live(), then runtime processes weak refs,
     * pending resources and notifications. No mutator resumes before return. */
    void (*complete)(void*, uint32_t young);
    /* Optional moving contract: reserve/copy a payload and update its location
     * without invalidating record. Old lookup aliases survive until complete.
     * Caller fixes precise edges before resume; pinned records return BUSY.
     * Missing support rejects a MOVING policy before execution. */
    int (*relocate)(void*, OgcPolicyRecord*, void** payload);
} OgcPolicyServices;
typedef struct OgcPolicy {
    uint32_t abi, size, capabilities, reserved;
    const char* name;
    void (*init)(OgcPolicyState*);
    void (*shade)(OgcPolicyState*, OgcPolicyRecord*, uint32_t young);
    void (*remember)(OgcPolicyState*, OgcPolicyRecord*);
    int (*collect)(OgcPolicyState*, const OgcPolicyServices*);
    int (*minor)(OgcPolicyState*, const OgcPolicyServices*);
    int (*step)(OgcPolicyState*, const OgcPolicyServices*, uint64_t budget_us, int* finished);
} OgcPolicy;
/* Status values: OK=0, OOM=1, UNSUPPORTED=2, INVALID=3, BUSY=4. Owner,
 * exclusivity and record/slot lifetimes belong to the allocator/runtime host.
 * Query leaves output untouched on failure. Link one policy implementation. */
int ogc_policy_query(uint32_t abi, uint32_t min_size, const OgcPolicy**);
#ifdef __cplusplus
}
#endif
#endif
