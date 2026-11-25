/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENGC_PORTABLE_H
#define OPENGC_PORTABLE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Shared runtime preset ABI 3. Algorithms implement policy.h instead.
 * Independent of heap.h ABI 2 and protocol.h ABI 1.
 * Object references are payload pointers, never identities. All live references
 * across alloc/collect/minor/step/safepoint must occupy registered writable root
 * slots or traced object slots. A provider may rewrite them before returning.
 * Register values must be reloaded afterwards. Typed BORROWED roots require
 * base+offset relocation; opaque native borrows require pin.
 * One owner thread per heap. Foreign/stale heap pointers are caller errors.
 */
#define OGC_PORTABLE_ABI 3u
#define OGC_PORTABLE_MODEL 3u
#define OGC_PORTABLE_COLLECTS (UINT64_C(1) << 0)
#define OGC_PORTABLE_MINOR (UINT64_C(1) << 1)
#define OGC_PORTABLE_STEP (UINT64_C(1) << 2)
#define OGC_PORTABLE_PIN (UINT64_C(1) << 3)
#define OGC_PORTABLE_RELEASE (UINT64_C(1) << 4)
#define OGC_PORTABLE_MOVING (UINT64_C(1) << 5)
#define OGC_PORTABLE_WALK (UINT64_C(1) << 6)
#define OGC_PORTABLE_WEAK (UINT64_C(1) << 7)
#define OGC_PORTABLE_NO_GC (UINT64_C(1) << 8)
/* read_word on the owner thread is exactly an aligned 8-byte load with no
 * side effect, so a consumer may load registered or local reference slots itself. */
#define OGC_PORTABLE_PLAIN_READ (UINT64_C(1) << 9)
/* barrier_flag(heap) names a byte fixed for the heap's lifetime. While it reads zero on the owner
 * thread, write_word into a local object or registered slot would only store the word, so a consumer
 * may store a validated reference itself; otherwise it calls write_word, or write_word(slot, *slot)
 * (shade the old value, remember the owner) before its own store. The provider sets the byte only
 * inside its own entries (collect, step, minor, alloc). */
#define OGC_PORTABLE_BARRIER_FLAG (UINT64_C(1) << 10)
/* alloc_deferred never collects or runs callbacks. A nonzero poll byte requests
 * safepoint with writable roots published. The byte is stable for the heap's
 * lifetime and is read only by its owner. The original alloc still may collect. */
#define OGC_PORTABLE_DEFERRED_ALLOC (UINT64_C(1) << 11)
/* An allocation run contains zeroed, consecutive, pre-registered fixed cells.
 * A consumer may take one by advancing next by the cell stride (size rounded up
 * to 16) when key matches and next is below end. Only next is consumer-writable.
 * A provider operation may invalidate a run by clearing key. Read the run
 * afresh after calls, even while the heap and its run record remain live.
 * In [begin, next), issued cells are from the current nursery and no
 * incremental mark is active. A consumer may directly store a validated
 * local reference into a declared slot in that range. A closed run is zeroed. */
#define OGC_PORTABLE_ALLOC_RUN (UINT64_C(1) << 12)
/* The consumer may link initialized stack frames to a heap-owned head.
 * Frames follow LIFO order and stay valid during collection, WALK and stack
 * suspension. The consumer unlinks abandoned frames before stack teardown. */
#define OGC_PORTABLE_STACK_FRAMES (UINT64_C(1) << 13)
#define OGC_PORTABLE_OWNED_WEAK (UINT64_C(1) << 14)
/* With ALLOC_RUN, allocation_run returns a stable array of two run records.
 * Either record may be consumed under the usual key and bounds checks.
 * Refill still selects record zero and may change both records. */
#define OGC_PORTABLE_ALLOC_RUN_PAIR (UINT64_C(1) << 15)

enum { OGC_PORTABLE_OK = 0, OGC_PORTABLE_OOM = 1,
       OGC_PORTABLE_UNSUPPORTED = 2, OGC_PORTABLE_INVALID = 3,
       OGC_PORTABLE_BUSY = 4, OGC_PORTABLE_WRONG_OWNER = 5 };
typedef void* OgcRef;
typedef struct OgcPortableAllocRun {
    const void* key;
    uintptr_t next, end, begin;
} OgcPortableAllocRun;
typedef struct OgcPortableHeap OgcPortableHeap;
typedef struct OgcPortableType OgcPortableType;
typedef struct OgcPortableRoots OgcPortableRoots;
typedef struct OgcPortableHandle OgcPortableHandle;
typedef struct OgcPortablePin OgcPortablePin;
typedef struct OgcPortableWeak OgcPortableWeak;
typedef struct OgcPortableNoGC OgcPortableNoGC;
typedef struct OgcPortableHook OgcPortableHook;
typedef struct OgcPortableWalk {
    const OgcRef* refs;
    size_t nrefs;
    const uintptr_t* words;
    size_t nwords;
    uint32_t registered_roots, reserved; /* bit 0: all registered roots; bit 1: handles/pins; bit 2: stack frames */
    void (*object)(void*, OgcRef, const OgcPortableType*);
    void (*word)(void*, uintptr_t);
    void* user;
} OgcPortableWalk;
typedef struct OgcPortablePolicy {
    uint32_t automatic, stress, steps, reserved;
} OgcPortablePolicy;

typedef struct OgcPortableConfig {
    uint32_t abi, size, model, reserved;
    uint64_t required_capabilities, max_bytes;
} OgcPortableConfig;
/* Fixed objects: size bytes. Arrays: prefix_bytes followed by count elements of
 * size bytes. Reference offsets are sorted, unique, 8-byte aligned and fit
 * inside one element; reference-bearing strides must preserve slot alignment.
 * Payload alignment is 8 or 16; size is positive, including byte-array strides.
 * Registration copies name/offsets/trace programs. release reads only scalars;
 * references may be dead. It may not reenter this heap. user outlives the heap.
 */
enum { OGC_TRACE_STRONG = 0, OGC_TRACE_BORROWED = 1,
       OGC_TRACE_TAGGED = 2, OGC_TRACE_PINNED_AMBIGUOUS = 3,
       OGC_TRACE_IF_U8 = 4, OGC_TRACE_IF_U64 = 5, OGC_TRACE_END = 6, OGC_TRACE_CODE = 7 };
/* TAGGED traces only when tag < 64 and its bit is in tag_values. BORROWED
 * pointers may be external or interior; relocation preserves their base+offset.
 * PINNED_AMBIGUOUS is exclusively for explicitly untagged union storage: it
 * may retain/pin a candidate but must never rewrite the possibly scalar word. */
typedef struct OgcPortableTrace {
    uint32_t offset, kind, tag_offset, reserved;
    uint64_t tag_values;
} OgcPortableTrace;
typedef struct OgcPortableStackRoot {
    void* address;
    const void* tag_address;
    uint32_t kind, reserved;
    uint64_t tag_values;
} OgcPortableStackRoot;
typedef struct OgcPortableRootFrame {
    struct OgcPortableRootFrame* next;
    const OgcPortableStackRoot* slots;
    size_t count;
    uintptr_t pc;
} OgcPortableRootFrame;
typedef struct OgcPortableLayout {
    uint32_t struct_size, reserved;
    uint32_t size, align, is_array, nrefs;
    const uint32_t* refs;
    const char* name;
    void (*release)(void* user, void* payload);
    void* user;
    const OgcPortableTrace* traces;
    uint32_t ntraces, prefix_bytes;
} OgcPortableLayout;
typedef struct OgcPortableStats {
    uint64_t collections, minors, live_bytes, heap_bytes, allocated;
} OgcPortableStats;

/* Query validates ABI/model/minimum table size before returning a static table;
 * on any failure out is untouched. No provider-private layout is public.
 * Functions return status; failure leaves outputs untouched. Invalid structural
 * inputs are rejected. Supplied refs must be null or live exact local payloads.
 * Heap/type/token lifetimes and writable root memory remain caller contracts.
 */
typedef struct OgcPortableApi {
    uint32_t abi, size, model, reserved;
    const char* provider;
    uint64_t capabilities;
    int (*create)(const OgcPortableConfig*, OgcPortableHeap**);
    /* Busy while roots, handles or pins exist. Success invalidates heap. */
    int (*destroy)(OgcPortableHeap*);
    int (*type_register)(OgcPortableHeap*, const OgcPortableLayout*, const OgcPortableType**);
    int (*alloc)(OgcPortableHeap*, const OgcPortableType*, uint64_t count, OgcRef*);
    int (*type_of)(OgcPortableHeap*, OgcRef, const OgcPortableType**);
    /* Nonzero identity, unique within a heap, stable through relocation. */
    int (*identity)(OgcPortableHeap*, OgcRef, uint64_t*);
    /* These operations do not safepoint. offset is relative to visible payload,
     * must name a declared reference slot. No raw reference memcpy/stores. */
    int (*load_ref)(OgcPortableHeap*, OgcRef, uint64_t offset, OgcRef*);
    int (*store_ref)(OgcPortableHeap*, OgcRef, uint64_t offset, OgcRef);
    /* Snapshot/memmove semantics; all slots validated before any write. */
    int (*copy_refs)(OgcPortableHeap*, OgcRef dst, uint64_t dst_offset,
                     OgcRef src, uint64_t src_offset, uint64_t count);
    /* Slots remain writable and at stable addresses until roots_leave. Tokens
     * are heap-bound and leave in LIFO order; multiple frames may refer to
     * the same slot range. Direct slot updates are allowed between safepoints. */
    int (*roots_enter)(OgcPortableHeap*, OgcRef* slots, size_t count, OgcPortableRoots**);
    int (*roots_leave)(OgcPortableHeap*, OgcPortableRoots*);
    int (*handle_new)(OgcPortableHeap*, OgcRef, OgcPortableHandle**);
    int (*handle_get)(OgcPortableHeap*, OgcPortableHandle*, OgcRef*);
    int (*handle_set)(OgcPortableHeap*, OgcPortableHandle*, OgcRef);
    int (*handle_free)(OgcPortableHeap*, OgcPortableHandle*);
    /* pin does not safepoint. Address valid until unpin; only scalar bytes may
     * be accessed directly. Do not write references or access beyond payload.
     * NoGC windows/weak references are not extensions of pin in this ABI. */
    int (*pin)(OgcPortableHeap*, OgcRef, OgcPortablePin**, void**);
    int (*unpin)(OgcPortableHeap*, OgcPortablePin*);
    int (*safepoint)(OgcPortableHeap*);
    int (*collect)(OgcPortableHeap*);
    int (*minor)(OgcPortableHeap*);
    /* Budgets are microseconds, at most UINT32_MAX; they are best effort. */
    int (*step)(OgcPortableHeap*, uint64_t budget_us, int* finished);
    int (*finalize)(OgcPortableHeap*, uint64_t budget_us, uint64_t* released);
    int (*stats)(OgcPortableHeap*, OgcPortableStats*);
    int (*extent)(OgcPortableHeap*, OgcRef, uint64_t* bytes, uint64_t* count);
    /* Walk is read-only and non-collecting. refs are exact object roots. words
     * are inspected for code retirement and may conservatively select objects,
     * but never enter the collector's precise root set. Callback payloads are
     * borrowed until return; GC/allocation and recursive walk are rejected. */
    int (*walk)(OgcPortableHeap*, const OgcPortableWalk*);
    int (*weak_new)(OgcPortableHeap*, OgcRef, OgcPortableWeak**);
    int (*weak_get)(OgcPortableHeap*, OgcPortableWeak*, OgcRef*);
    int (*weak_free)(OgcPortableHeap*, OgcPortableWeak*);
    /* LIFO windows: alloc/collect/minor/step return BUSY until leave. */
    int (*no_gc_enter)(OgcPortableHeap*, OgcPortableNoGC**);
    int (*no_gc_leave)(OgcPortableHeap*, OgcPortableNoGC*);
    int (*hook_add)(OgcPortableHeap*, void (*)(void*), void*, OgcPortableHook**);
    int (*hook_remove)(OgcPortableHeap*, OgcPortableHook*);
    int (*set_policy)(OgcPortableHeap*, const OgcPortablePolicy*);
    int (*pending_releases)(OgcPortableHeap*, uint64_t*);
    int (*roots_enter_typed)(OgcPortableHeap*, const OgcPortableStackRoot*, size_t, OgcPortableRoots**);
    int (*type_layout)(OgcPortableHeap*, const OgcPortableType*, const OgcPortableLayout**);
    /* Typed memory operations never safepoint. Addresses may be registered
     * stack/global reference slots or slots in a local object. */
    int (*read_word)(OgcPortableHeap*, const void*, OgcRef*);
    int (*write_word)(OgcPortableHeap*, void*, OgcRef);
    int (*copy_memory)(OgcPortableHeap*, void*, const void*, size_t);
    int (*marking)(OgcPortableHeap*, int*);
    int (*roots_remove)(OgcPortableHeap*, OgcPortableRoots*);
    const uint8_t* (*barrier_flag)(OgcPortableHeap*);
    int (*alloc_deferred)(OgcPortableHeap*, const OgcPortableType*, uint64_t, OgcRef*);
    const uint8_t* (*poll_flag)(OgcPortableHeap*);
    OgcPortableAllocRun* (*allocation_run)(OgcPortableHeap*);
    int (*refill_run)(OgcPortableHeap*, const OgcPortableType*, const void* key, OgcRef*);
    OgcPortableRootFrame** (*root_head)(OgcPortableHeap*);
    /* Optional tail. The token may be freed explicitly and is otherwise retired
     * with owner. Neither owner nor target is rooted by the token. */
    int (*weak_new_owned)(OgcPortableHeap*, OgcRef owner, OgcRef target, OgcPortableWeak**);
} OgcPortableApi;

/* Link exactly one provider implementing this common entry point. */
int ogc_portable_query(uint32_t abi, uint32_t model, uint32_t min_size, const OgcPortableApi** out);
#ifdef __cplusplus
}
#endif
#endif
