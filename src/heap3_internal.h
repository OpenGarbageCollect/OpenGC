// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "opengc/portable.h"
#include "opengc/policy.h"
#include <cstdint>
#include <cstring>
#include <thread>
namespace ogc_preset { struct Block; struct AllocationClass; }
struct OgcPortableType {
    OgcPortableLayout layout; OgcPortableType* next; OgcPortableHeap* owner;
    ogc_preset::AllocationClass *fixed = nullptr, *last = nullptr;
};
struct OgcPortableRoots { OgcRef* slots; size_t count; const OgcPortableStackRoot* typed; OgcPortableRoots* next; };
struct OgcPortableHandle { OgcRef value; OgcPortableHandle* next; };
struct OgcPortablePin { OgcRef value; OgcPortablePin* next; };
struct OgcPortableWeak { OgcRef value; OgcPortableWeak* next; OgcRef owner = nullptr; };
struct OgcPortableNoGC { OgcPortableNoGC* next; };
struct OgcPortableHook { void (*fn)(void*); void* user; OgcPortableHook* next; };
namespace ogc_preset {
inline constexpr bool no_gc = OGC_PORTABLE_NOGC != 0;
inline constexpr uint64_t capabilities = OGC_PORTABLE_PIN | OGC_PORTABLE_RELEASE | OGC_PORTABLE_WALK |
    OGC_PORTABLE_WEAK | OGC_PORTABLE_NO_GC | OGC_PORTABLE_PLAIN_READ | OGC_PORTABLE_BARRIER_FLAG |
    OGC_PORTABLE_DEFERRED_ALLOC | OGC_PORTABLE_ALLOC_RUN | OGC_PORTABLE_ALLOC_RUN_PAIR |
    OGC_PORTABLE_STACK_FRAMES | OGC_PORTABLE_OWNED_WEAK |
    (no_gc ? 0 : OGC_PORTABLE_COLLECTS | OGC_PORTABLE_MINOR | OGC_PORTABLE_STEP);
inline constexpr size_t page_size = 4096;
struct Node {
    OgcPolicyRecord gc;
    Block* block;
    uint64_t identity;
    OgcRef walk_next;
};
static_assert(sizeof(Node) == 80);
struct Block {
    Block *next, *previous, *type_next, **type_link, *available_next, **available_link;
    Block *nursery_next, *resource_next, **resource_link;
    Block *range_left, *range_right;
    uint64_t range_priority;
    AllocationClass* cls;
    const OgcPortableType* type;
    void* raw;
    uint8_t* data;
    Node* records;
    OgcRef* walk_links;
    uint64_t* identities;
    uint64_t *used, *pending, *seen, *maps;
    uint64_t bytes, count, nursery_generation, walk_epoch, live_epoch;
    size_t stride, data_bytes, physical;
    uint32_t capacity, words, frontier, used_count, free_word, dirty_frontier;
};
struct AllocationClass {
    const OgcPortableType* type;
    uint64_t count, bytes, scan_generation;
    size_t stride, next_data;
    Block *blocks, *available, *scan;
    AllocationClass *next, *previous;
};
struct PageEntry { uintptr_t page; Block* block; };
struct AllocatorRegistry {
    Block *blocks = nullptr, *nursery = nullptr, *resources = nullptr, *range_root = nullptr, *sweep = nullptr;
    Node* pending = nullptr;
    AllocationClass* classes = nullptr;
    AllocationClass** class_index = nullptr;
    size_t class_capacity = 0, class_count = 0, class_occupied = 0;
    PageEntry* pages = nullptr;
    size_t page_capacity = 0, page_count = 0, page_occupied = 0;
    Block *cached_block = nullptr, *previous_block = nullptr;
    uint64_t generation = 1, next_identity = 1, limit = 0, mapped = 0, pending_count = 0;
};
struct Object {
    Block* block = nullptr;
    uint32_t index = 0;
    explicit operator bool() const { return block != nullptr; }
    const OgcPortableType* type() const { return block->type; }
    uint64_t bytes() const { return block->bytes; }
    uint64_t count() const { return block->count; }
    uint64_t identity() const {
        if (!no_gc) return block->records[index].identity;
        if (block->identities) return block->identities[index];
        uint64_t value; std::memcpy(&value, block->data + size_t(index) * block->stride + block->stride - 8, 8); return value;
    }
};
inline Node* record(Object o) { return o.block->records + o.index; }
inline Object view(OgcPolicyRecord* r) { auto* n = reinterpret_cast<Node*>(r); return {n->block, uint32_t(n - n->block->records)}; }
inline OgcRef payload(Object o) { return o.block->data + size_t(o.index) * o.block->stride; }
inline bool bit(const uint64_t* map, uint32_t i) { return (map[i / 64] >> (i % 64)) & 1; }
}
namespace ogc_preset {
// Identifies the calling thread for the owner check on every heap entry: one register or TCB read
// instead of a pthread_self() call per helper invocation. Darwin's __builtin_thread_pointer() is the
// same constant on every thread, so Apple arm64 reads tpidrro_el0 (what pthread_self derives from).
inline uintptr_t thread_token() {
#if defined(__APPLE__) && defined(__aarch64__)
    uintptr_t v;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v));
    return v;
#elif defined(__linux__) && defined(__aarch64__)
    return reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
#elif defined(__linux__) && defined(__x86_64__)
    uintptr_t v;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(v));
    return v;
#else
    static thread_local char token;
    return reinterpret_cast<uintptr_t>(&token);
#endif
}
}
struct OgcPortableHeap {
    std::thread::id owner = std::this_thread::get_id();
    uintptr_t owner_token = ogc_preset::thread_token();
    ogc_preset::AllocatorRegistry storage;
    OgcPolicyState collector{};
    const OgcPolicy* algorithm = nullptr;
    OgcPolicyServices services{};
    OgcPortableType* types = nullptr;
    OgcPortableRoots* roots = nullptr;
    OgcPortableRootFrame* frames = nullptr;
    // Left frames, reused by the next enter: generated code enters one per call.
    OgcPortableRoots* root_pool = nullptr;
    OgcPortableHandle* handles = nullptr;
    OgcPortablePin* pins = nullptr;
    OgcPortableWeak* weak = nullptr;
    OgcPortableNoGC* windows = nullptr;
    OgcPortableHook* hooks = nullptr;
    uint64_t walk_epoch = 0, since = 0, stress_count = 0;
    uint64_t allocated_at_full = 0, live_at_full = 0;
    uint64_t nursery_target = 8u << 20;
    OgcPortableStats statistics{};
    OgcPortablePolicy policy{1, 0, 0, 0};
    OgcPortableAllocRun runs[2]{};
    ogc_preset::Block* run_blocks[2]{};
    bool busy = false, walking = false, hook_callback = false;
    // barrier: OGC_PORTABLE_BARRIER_FLAG's byte, nonzero while marking or once minor collections use the
    // remembered set (tracking). Before tracking starts, plain stores may have skipped remember, so the
    // first minor collection is a full one: afterwards every survivor is old and the set starts empty.
    uint8_t barrier = 0;
    uint8_t poll = 0;
    bool tracking = false;
};
namespace ogc_preset {
inline bool live(OgcPortableHeap* h, Object o) {
    return no_gc || ogc_policy_record_live(&h->collector, &record(o)->gc);
}
Block* locate_uncached(AllocatorRegistry&, uintptr_t);
inline Block* locate(AllocatorRegistry& storage, uintptr_t word) {
    const auto contains = [word](const Block* block) {
        return block && word - uintptr_t(block->data) < block->data_bytes;
    };
    if (contains(storage.cached_block)) return storage.cached_block;
    if (contains(storage.previous_block)) return storage.previous_block;
    return locate_uncached(storage, word);
}
template<bool exact>
Object find_object(OgcPortableHeap* h, uintptr_t word) {
    auto* b = locate(h->storage, word);
    if (!b) return {};
    for (unsigned i = 0; i < 2; ++i)
        if (b == h->run_blocks[i] && word >= h->runs[i].next && word < h->runs[i].end) return {};
    const size_t offset = size_t(word - uintptr_t(b->data));
    // Small objects share the minimum 16-byte stride.
    const size_t stride = b->stride;
    const size_t index = stride == 16 ? offset / 16 : offset / stride;
    if (index >= b->frontier) return {};
    const size_t within = offset - index * stride;
    if constexpr (exact) {
        if (within) return {};
    } else if (within >= (b->bytes ? b->bytes : 1)) return {};
    Object o{b, uint32_t(index)};
    if (!no_gc && (!bit(b->used, o.index) || bit(b->pending, o.index) || !live(h, o))) return {};
    return o;
}
inline Object object(OgcPortableHeap* h, OgcRef ref) {
    return find_object<true>(h, uintptr_t(ref));
}
Object containing(OgcPortableHeap*, uintptr_t);
Object register_allocation(OgcPortableHeap*, const OgcPortableType*, uint64_t, uint64_t);
Object refill_allocation_run(OgcPortableHeap*, const OgcPortableType*, const void*);
void flush_allocation_run(OgcPortableHeap*);
void retire_dead(OgcPortableHeap*);
void visit_young(OgcPortableHeap*, OgcPolicyVisitRecord, void*);
void visit_resources(OgcPortableHeap*, void (*)(OgcPortableHeap*, Object));
void release_storage(OgcPortableHeap*, Object);
Object take_pending(OgcPortableHeap*);
bool mark_walk(Object, uint64_t);
void set_walk_next(Object, OgcRef);
OgcRef walk_next(Object);
void destroy_storage(OgcPortableHeap*);
}
