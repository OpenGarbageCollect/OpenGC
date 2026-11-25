// SPDX-License-Identifier: GPL-3.0-or-later
#include "opengc/heap.h"
#include "opengc/mutator_bump.h"
#include <cstdio>
#include <cstring>
#include <climits>
namespace {
int checks = 0, failures = 0, released = 0;
OgcHeap *closing = nullptr;
void check(bool ok, const char *name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}
void release(void *payload) {
    ++released;
    check(*static_cast<uint64_t *>(payload) == 42, "resource remains intact until shutdown");
    check(ogc_heap_destroy(closing) == OGC_HEAP_BUSY, "shutdown callback cannot reenter destroy");
}
struct Node {
    Node *next;
    uint64_t value;
};
} // namespace
int main() {
    check(std::strcmp(ogc_heap_provider(), "bump") == 0, "selected provider is bump");
    check(!(ogc_heap_supported_capabilities() &
            (OGC_HEAP_COLLECTS | OGC_HEAP_INCREMENTAL | OGC_HEAP_GENERATIONAL | OGC_HEAP_RETAG)),
          "no false collector capabilities");
    OgcHeapConfig config{OGC_HEAP_ABI_VERSION, sizeof(OgcHeapConfig), 0, OGC_HEAP_COLLECTS, 0};
    check(!ogc_heap_create(&config), "unsupported required capability refuses creation");
    config.required_capabilities = 0;
    config.abi_version++;
    check(!ogc_heap_create(&config), "wrong ABI rejected");
    config.abi_version = OGC_HEAP_ABI_VERSION;
    config.struct_size--;
    check(!ogc_heap_create(&config), "wrong config size rejected");
    OgcHeap *heap = ogc_heap_create(nullptr), *other = ogc_heap_create(nullptr);
    check(heap && other, "create independent heaps");
    if (!heap || !other)
        return 1;
    check(!ogc_heap_barrier(heap).cards && !ogc_heap_barrier(heap).marking, "no barrier storage");
    OgcMutator m{}, foreign{};
    check(ogc_heap_mutator_register(heap, &m) == OGC_HEAP_OK, "mutator registered");
    check(ogc_heap_mutator_register(other, &foreign) == OGC_HEAP_OK, "other mutator registered");
    const uint32_t refs[] = {0};
    const auto *node = ogc_heap_intern(heap, sizeof(Node), 8, 0, refs, 1, nullptr, "Node");
    const auto *alien = ogc_heap_intern(other, sizeof(Node), 8, 0, refs, 1, nullptr, "Node");
    check(node != alien && node->id == alien->id, "descriptor IDs belong to each heap");
    check(ogc_heap_intern(heap, sizeof(Node), 8, 0, refs, 1, nullptr, "Node") == node, "interned descriptor identity");
    check(!ogc_heap_alloc(heap, &foreign, node, 0) && !ogc_heap_alloc(heap, &m, alien, 0),
          "foreign mutator/descriptor refused");
    auto *first = static_cast<Node *>(ogc_heap_alloc(heap, &m, node, 0));
    check(first && !first->next && !first->value && uintptr_t(first) % 16 == 0, "zeroed aligned object");
    check(first && *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(first) + OGC_MUTATOR_DESC_ID_OFFSET) ==
                       node->id,
          "descriptor at published offset");
    check(ogc_heap_desc_of(heap, first) == node && !ogc_heap_desc_of(other, first), "trusted local descriptor lookup");
    check(!ogc_heap_desc_of(heap, reinterpret_cast<uint8_t *>(first) + 8), "descriptor lookup requires exact payload");
    // The same two stores as generated allocation; no provider allocation call or side counter.
    auto &region = m.regions[0];
    auto *cell = region.next;
    region.next += 32;
    *reinterpret_cast<uint64_t *>(cell) = node->id;
    auto *second = reinterpret_cast<Node *>(cell + OGC_MUTATOR_HEADER_BYTES);
    check(ogc_heap_desc_of(heap, second) == node, "inline allocation is recognized");
    first->next = second;
    second->next = first;
    first->value = 7;
    second->value = 11;
    const auto stats = ogc_heap_stats(heap);
    check(stats.allocated == 64 && stats.live_bytes == 64 && stats.heap_bytes >= 64,
          "stats account for inline allocation frontiers");
    OgcMutator concurrent{};
    check(ogc_heap_mutator_register(heap, &concurrent) == OGC_HEAP_OK, "second mutator uses the same heap");
    const auto *plain = ogc_heap_intern(heap, 16, 8, 0, nullptr, 0, nullptr, "plain");
    check(ogc_heap_alloc(heap, &concurrent, plain, 0) && ogc_heap_stats(heap).allocated == 96,
          "separate mutator frontiers are counted independently");
    check(ogc_heap_mutator_unregister(heap, &concurrent) == OGC_HEAP_OK && ogc_heap_stats(heap).allocated == 96,
          "detaching one mutator preserves the other's frontier");
    const uint32_t handle = ogc_heap_handle_new(heap, first);
    check(handle && ogc_heap_handle_get(heap, handle) == first, "heap-relative handle");
    ogc_heap_handle_set(heap, handle, second);
    check(ogc_heap_handle_get(heap, handle) == second, "handle update");
    int cycles = 0;
    ogc_heap_add_cycle_hook(heap, [](void *user) { ++*static_cast<int *>(user); }, &cycles);
    check(ogc_heap_collect(heap) == OGC_HEAP_UNSUPPORTED && ogc_heap_minor(heap) == OGC_HEAP_UNSUPPORTED,
          "collect/minor explicitly unsupported");
    const auto step = ogc_heap_step(heap, UINT64_MAX);
    check(step.status == OGC_HEAP_UNSUPPORTED && !step.finished, "step does not fake a completed cycle");
    check(!cycles && !ogc_heap_stats(heap).collections && !ogc_heap_stats(heap).minors,
          "unsupported collection does not invoke hooks or count cycles");
    ogc_heap_set_automatic(heap, 1);
    ogc_heap_set_stress(heap, 2, 100);
    check(!ogc_heap_automatic(heap) && !ogc_heap_marking(heap), "no automatic collector");
    uint64_t room = 99;
    check(ogc_heap_payload_room(heap, first, &room) == OGC_HEAP_UNSUPPORTED && room == 99,
          "unsupported room preserves output");
    check(ogc_heap_retag(heap, first, node) == OGC_HEAP_UNSUPPORTED && first->value == 7,
          "unsupported retag preserves object");
    check(!ogc_heap_pending_releases(heap) && !ogc_heap_finalize(heap, UINT64_MAX),
          "NoGC does not finalize unreachable resources");
    struct State {
        OgcHeap *heap;
        const OgcTypeDesc *node;
        int found;
    } state{heap, node, 0};
    OgcWalk walk{};
    walk.handles = 1;
    walk.user = &state;
    walk.object = [](void *user, void *, const OgcTypeDesc *d) {
        auto &s = *static_cast<State *>(user);
        s.found += d == s.node;
    };
    check(ogc_heap_walk(heap, &walk) == OGC_HEAP_OK && state.found == 2, "precise cycle walk visits each object once");
    ogc_heap_handle_free(heap, handle);
    ogc_heap_handle_free(heap, handle);
    check(!ogc_heap_handle_get(heap, handle) && first->value == 7, "dropping handle does not reclaim object");
    void *root = first;
    ogc_heap_add_roots(heap, &root, sizeof root);
    walk.handles = 0;
    state.found = 0;
    check(ogc_heap_walk(heap, &walk) == OGC_HEAP_OK && !state.found, "selective walk does not add registered roots");
    check(ogc_heap_walk_roots(heap, &walk) == OGC_HEAP_OK && state.found == 2,
          "complete root walk follows registered roots");
    check(ogc_heap_destroy(heap) == OGC_HEAP_BUSY, "active registrations keep heap alive");
    ogc_heap_remove_roots(heap, &root);
    ogc_heap_remove_cycle_hook(heap, &cycles);
    bool refill = true;
    for (uint32_t i = 0; i < 2048; ++i)
        if (!ogc_heap_alloc(heap, &m, node, 0)) {
            refill = false;
            break;
        }
    check(refill, "allocation refills an exhausted class run");
    check(ogc_heap_stats(heap).allocated == 2051 * 32, "sealed predecessors plus current frontiers remain exact");
    const auto *resource = ogc_heap_intern(heap, 70000, 16, 0, nullptr, 0, release, "resource");
    auto *value = static_cast<uint64_t *>(ogc_heap_alloc(heap, &m, resource, 0));
    check(value && uintptr_t(value) % 16 == 0, "large aligned resource");
    if (value)
        *value = 42;
    const auto *vector = ogc_heap_intern(heap, 8, 8, 1, nullptr, 0, nullptr, "vector");
    auto *vec = static_cast<uint64_t *>(ogc_heap_alloc(heap, &m, vector, 3));
    check(vec && vec[0] == 3 && vec[1] == 0 && vec[3] == 0, "vector length and zero elements");
    check(!ogc_heap_alloc(heap, &m, vector, UINT64_MAX), "vector size overflow refused");
    const uint64_t retained = ogc_heap_stats(heap).allocated;
    check(ogc_heap_mutator_unregister(heap, &m) == OGC_HEAP_OK, "unregister seals all frontiers");
    check(ogc_heap_stats(heap).allocated == retained, "stats remain exact after mutator storage can disappear");
    const uintptr_t start = uintptr_t(first);
    OgcWalk quiescent{};
    quiescent.words = &start;
    quiescent.nwords = 1;
    quiescent.user = heap;
    quiescent.object = [](void *user, void *, const OgcTypeDesc *) {
        check(ogc_heap_destroy(static_cast<OgcHeap *>(user)) == OGC_HEAP_BUSY,
              "active walk callback prevents heap destruction");
    };
    check(ogc_heap_walk(heap, &quiescent) == OGC_HEAP_OK, "walk works after all mutators detach");
    closing = heap;
    check(ogc_heap_destroy(heap) == OGC_HEAP_OK && released == 1,
          "heap shutdown releases retained resource exactly once");
    check(ogc_heap_stats(other).allocated == 0, "other heap remains independent");
    ogc_heap_mutator_unregister(other, &foreign);
    check(ogc_heap_destroy(other) == OGC_HEAP_OK, "destroy other heap");
    config = {OGC_HEAP_ABI_VERSION, sizeof config, 65536, OGC_HEAP_INLINE_ALLOC, OGC_HEAP_WALK};
    heap = ogc_heap_create(&config);
    m = {};
    ogc_heap_mutator_register(heap, &m);
    node = ogc_heap_intern(heap, sizeof(Node), 8, 0, refs, 1, nullptr, "limited");
    bool within = true;
    for (uint32_t i = 0; i < 2048; ++i)
        if (!ogc_heap_alloc(heap, &m, node, 0)) {
            within = false;
            break;
        }
    check(within, "2048 allocations fit the configured mapping limit");
    check(!ogc_heap_alloc(heap, &m, node, 0), "NoGC limit exhausts without reclaiming");
    check(ogc_heap_stats(heap).allocated == 65536 && ogc_heap_stats(heap).heap_bytes == 65536,
          "limit failure preserves exact counters");
    check(ogc_heap_walk(heap, &walk) == OGC_HEAP_UNSUPPORTED &&
              ogc_heap_walk_roots(heap, &walk) == OGC_HEAP_UNSUPPORTED,
          "disabled WALK rejects both entries");
    ogc_heap_mutator_unregister(heap, &m);
    check(ogc_heap_destroy(heap) == OGC_HEAP_OK, "destroy exhausted heap");
    std::printf("%s bump: %d checks\n", failures ? "FAIL" : "OK", checks);
    return failures ? 1 : 0;
}
