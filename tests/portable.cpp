// SPDX-License-Identifier: GPL-3.0-or-later
#include "opengc/portable.h"
#include <cstdio>
#include <thread>
extern "C" int opengc_portable_client(const OgcPortableApi*);
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "portable line %d: %s\n", __LINE__, #x); return 1; } } while (0)
namespace {
struct WalkProbe {
    const OgcPortableApi* api;
    OgcPortableHeap* heap;
    unsigned objects = 0, code = 0;
    int alloc_status = 0, collect_status = 0, nested_status = 0;
};
int deferred_allocation(const OgcPortableApi* api) {
    const OgcPortableConfig config{OGC_PORTABLE_ABI, sizeof(config), OGC_PORTABLE_MODEL, 0,
                                   OGC_PORTABLE_DEFERRED_ALLOC, 0};
    OgcPortableHeap* heap = nullptr;
    CHECK(!api->create(&config, &heap));
    const uint8_t* pending = api->poll_flag(heap);
    CHECK(pending && !*pending);
    const OgcPortablePolicy stress{1, 1, 0, 0};
    CHECK(!api->set_policy(heap, &stress));
    OgcPortableLayout layout{};
    layout.struct_size = sizeof(layout);
    layout.size = layout.align = 8;
    const OgcPortableType* type = nullptr;
    CHECK(!api->type_register(heap, &layout, &type));
    OgcRef refs[2]{};
    CHECK(!api->alloc_deferred(heap, type, 0, &refs[0]));
    CHECK(!api->alloc_deferred(heap, type, 0, &refs[1]));
    const bool collects = api->capabilities & OGC_PORTABLE_COLLECTS;
    CHECK(bool(*pending) == collects);
    OgcPortableStats stats{};
    CHECK(!api->stats(heap, &stats) && !stats.collections);
    OgcPortableRoots* roots = nullptr;
    CHECK(!api->roots_enter(heap, refs, 2, &roots));
    CHECK(!api->safepoint(heap) && !*pending);
    CHECK(!api->stats(heap, &stats) && stats.collections == uint64_t(collects));
    CHECK(!api->safepoint(heap));
    CHECK(!api->stats(heap, &stats) && stats.collections == uint64_t(collects));
    for (OgcRef ref : refs) {
        const OgcPortableType* actual = nullptr;
        CHECK(!api->type_of(heap, ref, &actual) && actual == type);
    }
    CHECK(!api->roots_leave(heap, roots));
    CHECK(!api->destroy(heap));
    return 0;
}
int generated_state(const OgcPortableApi* api) {
    const OgcPortableConfig config{OGC_PORTABLE_ABI, sizeof(config), OGC_PORTABLE_MODEL, 0,
                                   OGC_PORTABLE_ALLOC_RUN | OGC_PORTABLE_STACK_FRAMES, 0};
    OgcPortableHeap* heap = nullptr;
    CHECK(!api->create(&config, &heap));
    OgcPortableLayout layout{};
    layout.struct_size = sizeof(layout);
    layout.size = layout.align = 8;
    const OgcPortableType* type = nullptr;
    CHECK(!api->type_register(heap, &layout, &type));
    OgcRef first = nullptr;
    CHECK(!api->refill_run(heap, type, type, &first));
    auto* run = api->allocation_run(heap);
    CHECK(run->key == type && run->begin == uintptr_t(first) && run->end > run->next);
    OgcRef second = reinterpret_cast<OgcRef>(run->next);
    const OgcPortableType* actual = nullptr;
    CHECK(api->type_of(heap, second, &actual) == OGC_PORTABLE_INVALID);
    run->next += 16;
    CHECK(!api->type_of(heap, second, &actual) && actual == type);
    CHECK(!*static_cast<uint64_t*>(second));
    uint64_t first_id = 0, second_id = 0;
    CHECK(!api->identity(heap, first, &first_id) && !api->identity(heap, second, &second_id));
    CHECK(second_id > first_id);
    const OgcPortableStackRoot slot{&second, nullptr, OGC_TRACE_STRONG, 0, 0};
    OgcPortableRootFrame frame{nullptr, &slot, 1, 0};
    auto** head = api->root_head(heap);
    *head = &frame;
    CHECK(api->destroy(heap) == OGC_PORTABLE_BUSY);
    CHECK(!run->key);
    if (api->capabilities & OGC_PORTABLE_COLLECTS) {
        CHECK(!api->collect(heap));
        CHECK(api->type_of(heap, first, &actual) == OGC_PORTABLE_INVALID);
        CHECK(!api->type_of(heap, second, &actual) && actual == type);
    }
    *head = nullptr;
    CHECK(!api->destroy(heap));
    return 0;
}
int run_refill_and_abandon(const OgcPortableApi* api) {
    CHECK(api->capabilities & OGC_PORTABLE_ALLOC_RUN_PAIR);
    const OgcPortableConfig config{OGC_PORTABLE_ABI, sizeof(config), OGC_PORTABLE_MODEL, 0,
                                   OGC_PORTABLE_ALLOC_RUN, 0};
    OgcPortableHeap* heap = nullptr;
    CHECK(!api->create(&config, &heap));
    OgcPortableLayout layout{};
    layout.struct_size = sizeof(layout);
    layout.size = layout.align = 8;
    const OgcPortableType *type = nullptr, *other = nullptr;
    CHECK(!api->type_register(heap, &layout, &type));
    CHECK(!api->type_register(heap, &layout, &other));
    auto* run = api->allocation_run(heap);
    OgcRef first = nullptr;
    OgcPortableStats stats{};
    CHECK(!api->alloc_deferred(heap, other, 0, &first));
    CHECK(!api->stats(heap, &stats) && stats.allocated);
    const uint64_t cell_charge = stats.allocated;
    uint64_t issued_count = 0;
    CHECK(!api->refill_run(heap, type, type, &first));
    for (unsigned i = 0; i < 6; ++i) {
        issued_count += (run->end - run->begin) / 16;
        run->next = run->end;
        CHECK(!api->refill_run(heap, type, type, &first));
    }
    CHECK(run->end - run->next >= 64 * 16);
    const auto issued = reinterpret_cast<OgcRef>(run->next + 40 * 16);
    run->next += 41 * 16;
    const auto reserved = reinterpret_cast<OgcRef>(run->next);
    const OgcPortableType* actual = nullptr;
    CHECK(api->type_of(heap, reserved, &actual) == OGC_PORTABLE_INVALID);
    CHECK(!api->refill_run(heap, other, other, &first));
    CHECK(run[0].key == other && run[1].key == type && run[1].next == uintptr_t(reserved));
    CHECK(api->type_of(heap, reserved, &actual) == OGC_PORTABLE_INVALID);
    CHECK(!api->type_of(heap, issued, &actual) && actual == type);
    CHECK(!*static_cast<uint64_t*>(issued));
    uint64_t issued_id = 0, other_id = 0;
    CHECK(!api->identity(heap, issued, &issued_id) && !api->identity(heap, first, &other_id));
    CHECK(other_id > issued_id);
    run[1].next += 16;
    CHECK(!api->type_of(heap, reserved, &actual) && actual == type);
    const auto promoted = reinterpret_cast<OgcRef>(run[1].next);
    CHECK(!api->refill_run(heap, type, type, &first) && first == promoted);
    CHECK(run[0].key == type && run[1].key == other);
    CHECK(!api->stats(heap, &stats));
    CHECK(!run[0].key && !run[1].key);
    CHECK(stats.allocated == (issued_count + 44 + 2) * cell_charge);
    CHECK(!api->refill_run(heap, type, type, &first));
    CHECK(!api->refill_run(heap, type, other, &first));
    CHECK(run[0].key == other && !run[1].key);
    CHECK(!api->refill_run(heap, other, other, &first));
    CHECK(run[0].key == other && !run[1].key);
    CHECK(!api->type_of(heap, first, &actual) && actual == other);
    CHECK(!api->destroy(heap));
    return 0;
}
int allocation_reuse(const OgcPortableApi* api) {
    if (!(api->capabilities & OGC_PORTABLE_COLLECTS)) return 0;
    const OgcPortableConfig config{OGC_PORTABLE_ABI, sizeof(config), OGC_PORTABLE_MODEL, 0, 0, 32768};
    OgcPortableHeap* h = nullptr; CHECK(!api->create(&config, &h));
    const OgcPortablePolicy policy{}; CHECK(!api->set_policy(h, &policy));
    OgcPortableLayout layout{}; layout.struct_size = sizeof(layout); layout.size = 8; layout.align = 8;
    const OgcPortableType *small = nullptr, *large = nullptr;
    CHECK(!api->type_register(h, &layout, &small));
    layout.size = 16000; CHECK(!api->type_register(h, &layout, &large));
    OgcRef ref = nullptr;
    uint64_t identity = 0;
    for (unsigned i = 0; i < 128; ++i) {
        CHECK(!api->alloc(h, small, 0, &ref)); *static_cast<uint64_t*>(ref) = UINT64_MAX;
        CHECK(!api->identity(h, ref, &identity));
    }
    CHECK(!api->collect(h));
    // Empty slabs must be released when another size needs the quota.
    CHECK(!api->alloc(h, large, 0, &ref));
    CHECK(!api->alloc(h, small, 0, &ref));
    CHECK(*static_cast<uint64_t*>(ref) == 0);
    uint64_t replacement = 0; CHECK(!api->identity(h, ref, &replacement));
    CHECK(replacement > identity);
    const OgcPortableType* actual = nullptr; CHECK(!api->type_of(h, ref, &actual) && actual == small);
    OgcPortableStats stats{}; CHECK(!api->stats(h, &stats) && stats.heap_bytes <= config.max_bytes);
    CHECK(!api->destroy(h));
    return 0;
}
int allocator_contract(const OgcPortableApi* api) {
    const OgcPortableConfig config{OGC_PORTABLE_ABI, sizeof(config), OGC_PORTABLE_MODEL, 0, 0, 0};
    OgcPortableHeap *h = nullptr, *other = nullptr;
    CHECK(!api->create(&config, &h)); CHECK(!api->create(&config, &other));
    const OgcPortablePolicy policy{}; CHECK(!api->set_policy(h, &policy));
    const uint32_t sizes[]{1, 8, 16, 24};
    const OgcPortableType* types[4]{};
    OgcPortableLayout layout{}; layout.struct_size = sizeof(layout); layout.align = 16;
    for (unsigned i = 0; i < 4; ++i) {
        layout.size = sizes[i]; CHECK(!api->type_register(h, &layout, &types[i]));
    }
    constexpr unsigned n = 4096;
    OgcRef refs[n + 1]{}; uint64_t ids[n]{};
    for (unsigned i = 0; i < n; ++i) {
        CHECK(!api->alloc(h, types[i % 4], 0, &refs[i]));
        CHECK(!(reinterpret_cast<uintptr_t>(refs[i]) % 16));
        for (unsigned j = 0; j < sizes[i % 4]; ++j) CHECK(!static_cast<unsigned char*>(refs[i])[j]);
        CHECK(!api->identity(h, refs[i], &ids[i]));
        CHECK(ids[i] && (!i || ids[i] > ids[i - 1]));
    }
    // Broad traversal crosses slab boundaries and revisits aliases without changing identities.
    if (!(api->capabilities & OGC_PORTABLE_COLLECTS)) {
        OgcPortableStats stats{}; CHECK(!api->stats(h, &stats));
        CHECK(stats.heap_bytes < uint64_t(n) * 80);
    }
    refs[n] = refs[0];
    unsigned visited = 0;
    OgcPortableWalk walk{}; walk.refs = refs; walk.nrefs = n + 1; walk.user = &visited;
    walk.object = [](void* user, OgcRef, const OgcPortableType*) { ++*static_cast<unsigned*>(user); };
    CHECK(!api->walk(h, &walk) && visited == n);
    visited = 0; CHECK(!api->walk(h, &walk) && visited == n);
    for (unsigned i = 0; i < n; ++i) {
        uint64_t id = 0; CHECK(!api->identity(h, refs[i], &id) && id == ids[i]);
        const OgcPortableType* type = nullptr;
        CHECK(!api->type_of(h, refs[i], &type) && type == types[i % 4]);
        CHECK(api->identity(other, refs[i], &id) == OGC_PORTABLE_INVALID);
        CHECK(api->identity(h, static_cast<unsigned char*>(refs[i]) + 1, &id) == OGC_PORTABLE_INVALID);
        if (sizes[i % 4] % 16) CHECK(api->identity(h, static_cast<unsigned char*>(refs[i]) + sizes[i % 4], &id) == OGC_PORTABLE_INVALID);
    }
    // Large and empty byte arrays use the same public extent and identity contract.
    layout.size = 1; layout.is_array = 1;
    const OgcPortableType* bytes = nullptr; CHECK(!api->type_register(h, &layout, &bytes));
    for (uint64_t count : {UINT64_C(0), UINT64_C(70001)}) {
        OgcRef ref = nullptr; CHECK(!api->alloc(h, bytes, count, &ref));
        uint64_t size = 1, actual = 1; CHECK(!api->extent(h, ref, &size, &actual));
        CHECK(size == count && actual == count);
        walk.refs = &ref; walk.nrefs = 1; visited = 0;
        CHECK(!api->walk(h, &walk) && visited == 1);
        if (count) CHECK(static_cast<unsigned char*>(ref)[count - 1] == 0);
    }
    if (api->capabilities & OGC_PORTABLE_COLLECTS) {
        CHECK(!api->collect(h));
        // Distinct array lengths must not retain all dead size classes indefinitely.
        for (uint64_t count = 1000; count < 1064; ++count) {
            OgcRef ref = nullptr; CHECK(!api->alloc(h, bytes, count, &ref));
            CHECK(!api->collect(h));
        }
        OgcPortableStats stats{}; CHECK(!api->stats(h, &stats)); CHECK(stats.heap_bytes < 65536);
    }
    CHECK(!api->destroy(other)); CHECK(!api->destroy(h));
    return 0;
}
int block_reuse(const OgcPortableApi* api) {
    if (!(api->capabilities & OGC_PORTABLE_COLLECTS)) return 0;
    const OgcPortableConfig config{OGC_PORTABLE_ABI, sizeof(config), OGC_PORTABLE_MODEL, 0, 0, 0};
    OgcPortableHeap* h = nullptr; CHECK(!api->create(&config, &h));
    const OgcPortablePolicy policy{}; CHECK(!api->set_policy(h, &policy));
    OgcPortableLayout layout{}; layout.struct_size = sizeof(layout); layout.size = 24; layout.align = 8;
    const OgcPortableType* type = nullptr; CHECK(!api->type_register(h, &layout, &type));
    OgcRef keep[3]{}; uint64_t ids[3]{};
    uint64_t last = 0;
    for (unsigned i = 0; i < 8192; ++i) {
        OgcRef ref = nullptr; CHECK(!api->alloc(h, type, 0, &ref));
        for (unsigned j = 0; j < 24; ++j) static_cast<unsigned char*>(ref)[j] = 0xa5;
        CHECK(!api->identity(h, ref, &last));
        if (i == 0 || i == 3000 || i == 8191) {
            const unsigned slot = i == 0 ? 0 : i == 3000 ? 1 : 2;
            keep[slot] = ref; ids[slot] = last;
        }
    }
    OgcPortableRoots* roots = nullptr; CHECK(!api->roots_enter(h, keep, 3, &roots));
    for (unsigned pass = 0; pass < 3; ++pass) {
        CHECK(!api->collect(h));
        for (unsigned i = 0; i < 8192; ++i) {
            OgcRef ref = nullptr; CHECK(!api->alloc(h, type, 0, &ref));
            for (unsigned j = 0; j < 24; ++j) {
                CHECK(!static_cast<unsigned char*>(ref)[j]);
                static_cast<unsigned char*>(ref)[j] = 0x5a;
            }
            uint64_t id = 0; CHECK(!api->identity(h, ref, &id) && id > last); last = id;
        }
        // A leaf survivor must prevent whole-block reuse even though it has no outgoing edges.
        for (unsigned i = 0; i < 3; ++i) {
            uint64_t id = 0; CHECK(!api->identity(h, keep[i], &id) && id == ids[i]);
            for (unsigned j = 0; j < 24; ++j) CHECK(static_cast<unsigned char*>(keep[i])[j] == 0xa5);
        }
    }
    CHECK(!api->roots_leave(h, roots));
    CHECK(!api->collect(h));
    OgcRef ref = nullptr; CHECK(!api->alloc(h, type, 0, &ref));
    for (unsigned j = 0; j < 24; ++j) CHECK(!static_cast<unsigned char*>(ref)[j]);
    CHECK(!api->destroy(h));

    // Birth during an unfinished full mark is black, including unrooted leaves.
    CHECK(!api->create(&config, &h)); CHECK(!api->set_policy(h, &policy));
    const uint32_t offset = 0;
    layout.size = 16; layout.refs = &offset; layout.nrefs = 1;
    CHECK(!api->type_register(h, &layout, &type));
    OgcRef head = nullptr;
    for (unsigned i = 0; i < 2048; ++i) {
        CHECK(!api->alloc(h, type, 0, &ref)); CHECK(!api->store_ref(h, ref, 0, head)); head = ref;
    }
    CHECK(!api->roots_enter(h, &head, 1, &roots));
    int finished = 1; CHECK(!api->step(h, 0, &finished) && !finished);
    layout.size = 8; layout.refs = nullptr; layout.nrefs = 0;
    CHECK(!api->type_register(h, &layout, &type));
    OgcRef black[128]{}; uint64_t black_ids[128]{};
    for (unsigned i = 0; i < 128; ++i) {
        CHECK(!api->alloc(h, type, 0, &black[i]));
        *static_cast<uint64_t*>(black[i]) = i + 1;
        CHECK(!api->identity(h, black[i], &black_ids[i]));
    }
    CHECK(!api->collect(h));
    for (unsigned i = 0; i < 4096; ++i) CHECK(!api->alloc(h, type, 0, &ref));
    for (unsigned i = 0; i < 128; ++i) {
        uint64_t id = 0; CHECK(!api->identity(h, black[i], &id) && id == black_ids[i]);
        CHECK(*static_cast<uint64_t*>(black[i]) == i + 1);
    }
    CHECK(!api->roots_leave(h, roots)); CHECK(!api->destroy(h));

    // Pending resource payloads survive repeated collections and same-type refills.
    struct Releases { uint64_t count = 0, sum = 0; } releases;
    layout.release = [](void* user, void* payload) {
        auto& r = *static_cast<Releases*>(user); ++r.count;
        r.sum += *static_cast<uint64_t*>(payload);
    };
    layout.user = &releases;
    CHECK(!api->create(&config, &h)); CHECK(!api->set_policy(h, &policy));
    CHECK(!api->type_register(h, &layout, &type));
    for (unsigned i = 1; i <= 256; ++i) {
        CHECK(!api->alloc(h, type, 0, &ref)); *static_cast<uint64_t*>(ref) = i;
    }
    CHECK(!api->collect(h)); CHECK(!api->collect(h));
    for (unsigned i = 1; i <= 256; ++i) {
        CHECK(!api->alloc(h, type, 0, &ref)); *static_cast<uint64_t*>(ref) = 1000 + i;
    }
    uint64_t released = 0; CHECK(!api->finalize(h, 1000000, &released));
    CHECK(released == 256 && releases.count == 256 && releases.sum == 32896);
    CHECK(!api->destroy(h)); CHECK(releases.count == 512 && releases.sum == 321792);
    return 0;
}
int services(const OgcPortableApi* api) {
    const OgcPortableConfig config{OGC_PORTABLE_ABI, sizeof(config), OGC_PORTABLE_MODEL, 0, 0, 0};
    OgcPortableHeap* h = nullptr;
    CHECK(!api->create(&config, &h));
    OgcPortableLayout scalar{}; scalar.struct_size = sizeof(scalar); scalar.size = 8; scalar.align = 8;
    const OgcPortableType* type = nullptr;
    CHECK(!api->type_register(h, &scalar, &type));
    OgcRef first = nullptr, second = nullptr;
    CHECK(!api->alloc(h, type, 0, &first));
    CHECK(!api->alloc(h, type, 0, &second));
    // Integer aliases and unselected arms must never turn into GC roots.
    uint64_t tag = 0;
    uintptr_t code = 0x12345678;
    OgcPortableStackRoot entries[] = {
        {&tag, nullptr, OGC_TRACE_IF_U64, 0, 2},
        {&second, nullptr, OGC_TRACE_STRONG, 0, 0},
        {nullptr, nullptr, OGC_TRACE_END, 0, 0},
        {&first, nullptr, OGC_TRACE_STRONG, 0, 0},
        {&code, nullptr, OGC_TRACE_CODE, 0, 0}
    };
    OgcPortableRoots* roots = nullptr;
    CHECK(!api->roots_enter_typed(h, entries, 5, &roots));
    OgcPortableWeak* weak = nullptr;
    CHECK(!api->weak_new(h, second, &weak));
    OgcPortableWeak* owned = nullptr;
    CHECK(api->capabilities & OGC_PORTABLE_OWNED_WEAK);
    CHECK(!api->weak_new_owned(h, first, second, &owned));
    WalkProbe probe{api, h};
    OgcPortableWalk walk{}; walk.registered_roots = 1; walk.user = &probe;
    walk.object = [](void* user, OgcRef, const OgcPortableType* t) {
        auto& p = *static_cast<WalkProbe*>(user); ++p.objects;
        OgcRef out = nullptr; OgcPortableWalk nested{};
        p.alloc_status = p.api->alloc(p.heap, t, 0, &out);
        p.collect_status = p.api->collect(p.heap);
        p.nested_status = p.api->walk(p.heap, &nested);
    };
    walk.word = [](void* user, uintptr_t word) {
        if (word == 0x12345678) ++static_cast<WalkProbe*>(user)->code;
    };
    CHECK(!api->walk(h, &walk));
    CHECK(probe.objects == 1 && probe.code == 1);
    CHECK(probe.alloc_status == OGC_PORTABLE_BUSY && probe.collect_status == OGC_PORTABLE_BUSY && probe.nested_status == OGC_PORTABLE_BUSY);
    const bool collects = api->capabilities & OGC_PORTABLE_COLLECTS;
    if (collects) {
        CHECK(!api->collect(h));
        OgcRef value = first;
        CHECK(!api->weak_get(h, weak, &value) && !value);
        value = first;
        CHECK(!api->weak_get(h, owned, &value) && !value);
    }
    // A rooted object reports declared code fields, never arbitrary scalars.
    OgcPortableTrace traces[]{{0, OGC_TRACE_CODE, 0, 0, 0}};
    OgcPortableLayout closure = scalar; closure.size = 16; closure.traces = traces; closure.ntraces = 1;
    CHECK(!api->type_register(h, &closure, &type));
    CHECK(!api->alloc(h, type, 0, &first));
    auto* words = static_cast<uintptr_t*>(first); words[0] = code; words[1] = code;
    probe.objects = probe.code = 0;
    CHECK(!api->walk(h, &walk));
    CHECK(probe.objects == 1 && probe.code == 2); // one exact root, one declared code field
    CHECK(!api->roots_remove(h, roots));
    CHECK(api->destroy(h) == OGC_PORTABLE_BUSY);
    CHECK(!api->weak_free(h, weak));
    CHECK(!api->destroy(h));
    return 0;
}
}
int main() {
    const OgcPortableApi* api = nullptr;
    CHECK(ogc_portable_query(2, OGC_PORTABLE_MODEL, sizeof(OgcPortableApi), &api) == OGC_PORTABLE_INVALID);
    CHECK(!api);
    CHECK(ogc_portable_query(OGC_PORTABLE_ABI, 99, sizeof(OgcPortableApi), &api) == OGC_PORTABLE_INVALID);
    CHECK(!api);
    CHECK(ogc_portable_query(OGC_PORTABLE_ABI, OGC_PORTABLE_MODEL, UINT32_MAX, &api) == OGC_PORTABLE_INVALID);
    CHECK(!api);
    CHECK(ogc_portable_query(OGC_PORTABLE_ABI, OGC_PORTABLE_MODEL, sizeof(OgcPortableApi), &api) == OGC_PORTABLE_OK);
    CHECK(api && api->size >= sizeof(OgcPortableApi));
    CHECK(!opengc_portable_client(api));
    CHECK(!deferred_allocation(api));
    CHECK(!generated_state(api));
    CHECK(!run_refill_and_abandon(api));
    CHECK(!services(api));
    CHECK(!allocation_reuse(api));
    CHECK(!allocator_contract(api));
    CHECK(!block_reuse(api));
    const OgcPortableConfig config{OGC_PORTABLE_ABI, sizeof(OgcPortableConfig), OGC_PORTABLE_MODEL, 0, 0, 0};
    OgcPortableHeap* heap = nullptr;
    CHECK(api->create(&config, &heap) == OGC_PORTABLE_OK);
    int result = 0;
    std::thread wrong_owner([&] { result = api->destroy(heap); }); wrong_owner.join();
    CHECK(result == OGC_PORTABLE_WRONG_OWNER);
    CHECK(api->destroy(heap) == OGC_PORTABLE_OK);
    std::printf("portable ABI 3: %s passed\n", api->provider);
}
