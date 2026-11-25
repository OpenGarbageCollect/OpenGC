/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 OpenGC contributors */
/*
 * The OpenGC heap: a link-time interface with one implementation per build (docs: PROTOCOL.md §8).
 * A language runtime links exactly one heap provider and calls these functions directly — no function
 * table, so the slow paths cost what a C call costs — and its compiler inlines the fast paths the
 * provider's mutator header (`opengc/mutator_<provider>.h`) describes: the allocation regions, the
 * object header, the write barrier.
 *
 * Model: non-moving, single owner thread, conservative roots (the calling thread's stack and
 * registers, registered ranges, the handle table), objects scanned precisely by their descriptor's
 * reference offsets. A reference is a payload address. Allocation may collect; nothing else does.
 * Valid instances are required except where null is explicitly accepted. All operations on an instance
 * are serialized by its owner; separate instances share no descriptor, root, handle or barrier state.
 * Optional status-returning operations answer OGC_HEAP_UNSUPPORTED without modifying output or objects.
 */
#ifndef OPENGC_HEAP_H
#define OPENGC_HEAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capability bits (ogc_heap_capabilities). */
#define OGC_HEAP_ABI_VERSION 2u
#define OGC_HEAP_RETAG (1ull << 5)
#define OGC_HEAP_WALK (1ull << 6)

#define OGC_HEAP_INCREMENTAL   (1ull << 0)  /* ogc_heap_step marks in bounded steps; a write barrier runs while marking */
#define OGC_HEAP_GENERATIONAL  (1ull << 1)  /* ogc_heap_minor collects young objects only */
#define OGC_HEAP_COLLECTS      (1ull << 2)  /* collect frees anything at all (retain does not) */
#define OGC_HEAP_RELEASE_HOOKS (1ull << 3)  /* descriptors may carry a release hook */
#define OGC_HEAP_INLINE_ALLOC  (1ull << 4)  /* the mutator header's regions are bump-allocated inline */

/* An object layout: a fixed payload of `size` bytes, or (vec) a u64 length followed by elements of
 * `size` bytes; `refs` are the byte offsets of the words that may hold references (in the payload, or
 * in one element). Interned: one descriptor per distinct layout, alive until its heap is destroyed. */
typedef struct OgcTypeDesc {
    uint32_t id;
    uint32_t size;
    uint32_t align;               /* 8 or 16 */
    uint8_t vec;
    uint8_t reserved[3];
    const uint32_t* refs;
    uint32_t nrefs;
    uint32_t reserved2;
    /* Called once with the payload of a dead object, outside the collector, from finalize or destroy.
     * The object's references may already be garbage: it reads only its own scalars. */
    void (*release)(void* payload);
    const char* name;             /* part of the identity (a class name), or "" */
} OgcTypeDesc;

/* A thread of execution's allocation state; its layout is the mutator header's (the compiler reads it
 * at fixed offsets). Registered while the language's context lives. */
typedef struct OgcMutator OgcMutator;
typedef struct OgcHeap OgcHeap;
typedef struct OgcHeapConfig {
    uint32_t abi_version, struct_size;
    uint64_t max_bytes;             /* object mappings only; 0 is unlimited */
    uint64_t required_capabilities;
    uint64_t disabled_capabilities; /* optional RETAG/WALK only */
} OgcHeapConfig;
typedef struct OgcHeapStepResult { int status, finished; } OgcHeapStepResult;
typedef struct OgcHeapBarrier { uint8_t* marking; uint8_t* cards; } OgcHeapBarrier;

typedef struct OgcHeapStats {
    uint64_t collections;         /* of either kind */
    uint64_t minors;
    uint64_t live_bytes;          /* marked by the last collection */
    uint64_t heap_bytes;          /* pages in use plus large objects */
    uint64_t allocated;           /* over this heap's life */
} OgcHeapStats;

typedef struct OgcRootRange {
    const void* base;
    size_t bytes;
} OgcRootRange;

/* Downward-growing execution stacks, on the current OS thread. Bounds need not be
 * fully committed below the current stack pointer. A scope must remain at a stable
 * address until leave; its fields are private while entered. */
typedef struct OgcStackBounds { const void* low; const void* high; } OgcStackBounds;
typedef struct OgcStackScope {
    OgcStackBounds bounds;
    struct OgcStackScope* previous;
} OgcStackScope;

/* By default stack roots use the OS thread/fiber bounds. An alternate-stack
 * runtime enters a scope ON that stack, before any heap allocation/walk, and
 * leaves in strict LIFO order before scanning a different stack. Scopes are
 * thread-local, apply to every heap called by that thread, and do not allocate.
 * Enter rejects null/reversed bounds, a range excluding its own stack frame or
 * an already-entered scope. Leave rejects a foreign-thread/non-top scope.
 * Invalid operations preserve the current scope and the supplied storage.
 * Valid enter/leave return BUSY while a collector/walker snapshot is active,
 * including inside its callbacks; such callbacks cannot switch execution stacks.
 * Scopes only select the running stack; suspended callers must separately spill
 * their registers and register readable stack ranges as roots. No switching,
 * lifetime extension, unwinding or trap recovery is performed by these calls. */
OgcStackBounds ogc_heap_stack_current(void);
int ogc_heap_stack_enter(OgcStackScope* scope, OgcStackBounds bounds);
int ogc_heap_stack_leave(OgcStackScope* scope);

/* A walk of what given roots reach, apart from the collector (it marks and frees nothing): roots are
 * followed conservatively, objects precisely, each object once. `object` sees every object reached;
 * `word` every root word and every 8-byte word of every reached payload, references or not. */
typedef struct OgcWalk {
    void (*object)(void* user, void* payload, const OgcTypeDesc* d);
    void (*word)(void* user, uintptr_t w);
    void* user;
    const uintptr_t* words;
    size_t nwords;
    const OgcRootRange* ranges;
    size_t nranges;
    uint8_t stack;                /* the calling thread's registers and stack */
    uint8_t handles;              /* the handle table */
} OgcWalk;

enum { OGC_HEAP_OK = 0, OGC_HEAP_OUT_OF_MEMORY = 1, OGC_HEAP_UNSUPPORTED = 2, OGC_HEAP_INVALID_ARGUMENT = 3, OGC_HEAP_BUSY = 4 };

/* The provider's name ("marksweep", …) and capabilities: what the build linked. */
const char* ogc_heap_provider(void);
uint64_t ogc_heap_supported_capabilities(void);
uint64_t ogc_heap_capabilities(const OgcHeap* heap);
OgcHeap* ogc_heap_create(const OgcHeapConfig* config);
/* Null config uses defaults. Config must match ABI and size exactly. Unsupported required capabilities
 * reject creation. Only RETAG/WALK may be disabled. Object mapping limits exclude heap metadata.
 * Destroy returns BUSY without mutation while mutators, roots, handles, hooks or a callback are active.
 * Quiescent destroy releases remaining resources once, then frees storage and descriptors. Its release
 * callbacks must not use this heap (a repeated destroy is allowed and returns BUSY). Other heaps remain valid. */
int ogc_heap_destroy(OgcHeap* heap);
/* Stable addresses until destroy; generated barrier slots bind these when installing JIT/AOT code.
 * A provider without barriers returns null addresses. */
OgcHeapBarrier ogc_heap_barrier(OgcHeap* heap);
/* Stress modes for testing: stress 1 = a major collection before every allocation, 2 = minors with a
 * periodic stepped major; steps > 0 = that many mark steps per allocation. Unsupported modes are ignored. */
void ogc_heap_set_stress(OgcHeap* heap, uint32_t stress, uint32_t steps);

const OgcTypeDesc* ogc_heap_intern(OgcHeap* heap, uint32_t size, uint32_t align, uint8_t vec, const uint32_t* refs, uint32_t nrefs,
                                   void (*release)(void* payload), const char* name);
const OgcTypeDesc* ogc_heap_desc_of(OgcHeap* heap, const void* payload);

/* A zeroed object of `d` (a vector of `count` elements; ignored for a fixed object), or null when the
 * memory cannot be had. The slow path of the inline allocation; refills the mutator's region. */
void* ogc_heap_alloc(OgcHeap* heap, OgcMutator* m, const OgcTypeDesc* d, uint64_t count);
int ogc_heap_mutator_register(OgcHeap* heap, OgcMutator* m);
/* Register a zero-initialized mutator once; unregister clears its regions and heap membership. */
int ogc_heap_mutator_unregister(OgcHeap* heap, OgcMutator* m);

int ogc_heap_collect(OgcHeap* heap);
int ogc_heap_minor(OgcHeap* heap);
/* Interpret finished only when status is OK: 0 = more work, 1 = cycle finished. */
OgcHeapStepResult ogc_heap_step(OgcHeap* heap, uint64_t budget_us);
int ogc_heap_marking(OgcHeap* heap);
void ogc_heap_set_automatic(OgcHeap* heap, int on);
int ogc_heap_automatic(OgcHeap* heap);
/* The write barrier's slow half (called while marking): shades the current referent of *word. */
void ogc_heap_shade(OgcHeap* heap, void* const* word);

uint64_t ogc_heap_finalize(OgcHeap* heap, uint64_t budget_us);
uint64_t ogc_heap_pending_releases(OgcHeap* heap);

int ogc_heap_add_roots(OgcHeap* heap, const void* base, size_t bytes);
void ogc_heap_remove_roots(OgcHeap* heap, const void* base);
/* Duplicate bases are allowed. remove_roots removes the latest matching range,
 * so a nested task's temporary range cannot replace a caller's earlier range. */
/* Handles store conservative root words, including null/non-heap/interior addresses. A foreign heap's
 * payload is not rooted in its owning heap by this handle. Indices are relative to heap; 0 is failure. */
uint32_t ogc_heap_handle_new(OgcHeap* heap, void* obj);
void* ogc_heap_handle_get(OgcHeap* heap, uint32_t handle);
void ogc_heap_handle_set(OgcHeap* heap, uint32_t handle, void* obj);
void ogc_heap_handle_free(OgcHeap* heap, uint32_t handle);

/* In-place migration: every object of `d` the last major collection marked (right after a collection,
 * with automatic collection off), the bytes a payload's cell has, and retagging an object as a zeroed
 * object of `to` at the same address (`to` must fit and keep the payload offset). */
int ogc_heap_objects_of(OgcHeap* heap, const OgcTypeDesc* d, void (*each)(void* user, void* payload), void* user);
int ogc_heap_payload_room(OgcHeap* heap, const void* payload, uint64_t* bytes);
int ogc_heap_retag(OgcHeap* heap, void* payload, const OgcTypeDesc* to);
int ogc_heap_walk(OgcHeap* heap, const OgcWalk* walk);
/* WALK capability: as walk, also traversing the complete owner root set (stack/registers, handles
 * and registered ranges). The ordinary walk deliberately stays selective for code retirement. */
int ogc_heap_walk_roots(OgcHeap* heap, const OgcWalk* walk);

/* Called at the end of every major collection, inside it; a hook must not allocate. One per user. */
int ogc_heap_add_cycle_hook(OgcHeap* heap, void (*hook)(void* user), void* user);
void ogc_heap_remove_cycle_hook(OgcHeap* heap, void* user);

OgcHeapStats ogc_heap_stats(OgcHeap* heap);

#ifdef __cplusplus
}
#endif

#endif
