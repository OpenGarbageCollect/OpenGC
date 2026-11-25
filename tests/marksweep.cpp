// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 OpenGC contributors
//
// OpenGC::marksweep through opengc/heap.h alone (no language runtime): a list kept by a handle survives
// collections and a dropped one does not; a release hook runs from finalize, never inside a collection,
// once; an object reachable only from a registered root range survives; an incremental cycle with the
// barrier keeps an object moved behind the marker; a minor collection keeps an old-to-young edge recorded
// in a dirty card (both seen through a release hook: an object wrongly found dead becomes pending). Every check prints its name when it fails.
#include "opengc/heap.h"
#include "opengc/mutator_marksweep.h"

#include <cstdio>
#include <cstring>

namespace {

OgcHeap* instance = nullptr;
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

struct Node {
    Node* next;
    uint64_t value;
};

int released = 0;
void on_release(void*) { ++released; }
OgcHeap* closing_heap = nullptr;
void on_shutdown(void*) {
    ++released;
    check(ogc_heap_destroy(closing_heap) == OGC_HEAP_BUSY, "destroy is busy during shutdown callback");
}

OgcMutator mutator;

// Builds the list in a frame that has returned before anything is collected, so no stack word of the
// caller still points at it unless the caller keeps it.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#else
__declspec(noinline)
#endif
Node* build(const OgcTypeDesc* d, uint64_t n) {
    Node* head = nullptr;
    for (uint64_t i = 0; i < n; ++i) {
        Node* x = static_cast<Node*>(ogc_heap_alloc(instance, &mutator, d, 0));
        if (!x) return nullptr;
        x->next = head;   // initialising store into a fresh object: no barrier needed
        x->value = i;
        head = x;
    }
    return head;
}

// Overwrites the stack below the caller, so a stale word of an earlier call does not keep garbage
// alive under the conservative scan.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#else
__declspec(noinline)
#endif
void scrub() {
    volatile uint8_t junk[16384];
    for (uint32_t i = 0; i < sizeof junk; ++i) junk[i] = 0;
}

uint64_t sum(const Node* n) {
    uint64_t s = 0;
    for (; n; n = n->next) s += n->value;
    return s;
}

// A store of a reference outside a frame, as generated code makes it: dirty the card, shade while marking.
void store(Node** slot, Node* v) {
    ogc_heap_barrier(instance).cards[ogc_mutator_card_index(reinterpret_cast<uintptr_t>(slot))] = 0;
    if (*ogc_heap_barrier(instance).marking) ogc_heap_shade(instance, reinterpret_cast<void* const*>(slot));
    *slot = v;
}


// For the barrier and generational checks: two references per object, and an object kind with a hook.
struct Pair {
    Pair* next;
    Pair* side;
    uint64_t value;
};
const uint32_t kPairRefs[2] = {0, 8};
const OgcTypeDesc* g_pair = nullptr;
const OgcTypeDesc* g_hooked = nullptr;
int hooked_released = 0;
void on_hooked(void*) { ++hooked_released; }
// Not roots (statics are not scanned): where the test keeps B and C between steps without the
// conservative scan seeing them.
Pair* g_b = nullptr;
Pair* g_cobj = nullptr;
uint32_t g_chain = 0, g_c = 0;

#if defined(__clang__) || defined(__GNUC__)
#define OGC_TEST_NOINLINE __attribute__((noinline))
#else
#define OGC_TEST_NOINLINE __declspec(noinline)
#endif

OGC_TEST_NOINLINE void satb_setup() {
    Pair* head = nullptr;
    for (int i = 0; i < 20000; ++i) {
        Pair* x = static_cast<Pair*>(ogc_heap_alloc(instance, &mutator, g_pair, 0));
        x->next = head;
        head = x;
        if (i == 0) {               // the chain's far end: marked last
            g_b = x;
            x->side = static_cast<Pair*>(ogc_heap_alloc(instance, &mutator, g_hooked, 0));
        }
    }
    g_chain = ogc_heap_handle_new(instance, head);       // handles are marked in order: the chain first,
    g_cobj = static_cast<Pair*>(ogc_heap_alloc(instance, &mutator, g_pair, 0));
    g_c = ogc_heap_handle_new(instance, g_cobj);         // C last, so it is scanned first
    std::memset(mutator.regions, 0, sizeof mutator.regions);  // no region cell points anywhere useful
}

OGC_TEST_NOINLINE void satb_move() {
    store(reinterpret_cast<Node**>(&g_cobj->side), reinterpret_cast<Node*>(g_b->side));
    store(reinterpret_cast<Node**>(&g_b->side), nullptr);
}

OGC_TEST_NOINLINE void gen_setup() {
    g_cobj = static_cast<Pair*>(ogc_heap_alloc(instance, &mutator, g_pair, 0));
    g_c = ogc_heap_handle_new(instance, g_cobj);
    ogc_heap_collect(instance);                        // C is old now
    Pair* a = static_cast<Pair*>(ogc_heap_alloc(instance, &mutator, g_hooked, 0));
    store(reinterpret_cast<Node**>(&g_cobj->side), reinterpret_cast<Node*>(a));
    std::memset(mutator.regions, 0, sizeof mutator.regions);
}
}  // namespace

void instance_checks() {
    OgcHeapConfig config{OGC_HEAP_ABI_VERSION, sizeof(OgcHeapConfig), 0, 0, 0};
    config.abi_version++;
    check(!ogc_heap_create(&config), "reject heap ABI mismatch");
    config.abi_version = OGC_HEAP_ABI_VERSION;
    config.struct_size--;
    check(!ogc_heap_create(&config), "reject truncated heap config");
    config.struct_size = sizeof config;
    config.required_capabilities = uint64_t(1) << 63;
    check(!ogc_heap_create(&config), "reject unsupported required capability");
    config.required_capabilities = 0;
    config.disabled_capabilities = OGC_HEAP_RETAG | OGC_HEAP_WALK;
    OgcHeap* a = ogc_heap_create(nullptr);
    OgcHeap* b = ogc_heap_create(&config);
    check(a && b, "create independent heaps");
    if (!a || !b) return;
    check(!(ogc_heap_capabilities(b) & config.disabled_capabilities), "instance optional capabilities disabled");
    const auto ba = ogc_heap_barrier(a), bb = ogc_heap_barrier(b);
    check(ba.cards != bb.cards && ba.marking != bb.marking, "barrier state belongs to heap");
    ba.cards[0] = 0;
    check(bb.cards[0] == 255, "dirty card isolated");
    OgcMutator ma{}, mb{};
    check(ogc_heap_mutator_register(a, &ma) == OGC_HEAP_OK, "register heap A mutator");
    check(ogc_heap_mutator_register(b, &mb) == OGC_HEAP_OK, "register heap B mutator");
    check(ogc_heap_mutator_register(b, &ma) != OGC_HEAP_OK, "reject foreign mutator registration");
    check(ogc_heap_mutator_unregister(b, &ma) != OGC_HEAP_OK && ma.heap == a, "reject foreign detach without mutation");
    const auto* da = ogc_heap_intern(a, 16, 8, 0, nullptr, 0, nullptr, "local");
    const auto* db = ogc_heap_intern(b, 16, 8, 0, nullptr, 0, nullptr, "local");
    check(da != db && da->id == db->id, "descriptor IDs are heap relative");
    void* pa = ogc_heap_alloc(a, &ma, da, 0);
    void* pb = ogc_heap_alloc(b, &mb, db, 0);
    check(pa && pb, "allocate in both heaps");
    check(!ogc_heap_alloc(a, &mb, da, 0) && !ogc_heap_alloc(a, &ma, db, 0), "reject foreign mutator and descriptor");
    check(!ogc_heap_desc_of(a, pb) && !ogc_heap_desc_of(b, pa), "foreign payload has no local descriptor");
    const uint32_t handle = ogc_heap_handle_new(a, pa);
    ogc_heap_handle_set(a, handle, pb);
    check(ogc_heap_handle_get(a, handle) == pb, "handle stores a conservative root word");
    ogc_heap_handle_set(a, handle, pa);
    uint64_t room = 1234;
    check(ogc_heap_payload_room(a, pb, &room) == OGC_HEAP_INVALID_ARGUMENT && room == 1234, "foreign payload room refused");
    check(ogc_heap_payload_room(b, pb, &room) == OGC_HEAP_UNSUPPORTED && room == 1234, "unsupported room leaves output untouched");
    *static_cast<uint64_t*>(pb) = 91;
    check(ogc_heap_retag(b, pb, db) == OGC_HEAP_UNSUPPORTED && *static_cast<uint64_t*>(pb) == 91,
          "unsupported retag leaves payload untouched");
    int visited = 0;
    auto each = [](void* user, void*) { ++*static_cast<int*>(user); };
    check(ogc_heap_objects_of(b, db, each, &visited) == OGC_HEAP_UNSUPPORTED && visited == 0, "unsupported enumeration has no callbacks");
    OgcWalk walk{};
    walk.object = [](void* user, void*, const OgcTypeDesc*) { ++*static_cast<int*>(user); };
    walk.user = &visited;
    const uintptr_t root = reinterpret_cast<uintptr_t>(pb);
    walk.words = &root; walk.nwords = 1;
    check(ogc_heap_walk(b, &walk) == OGC_HEAP_UNSUPPORTED && visited == 0, "unsupported walk has no callbacks");
    check(ogc_heap_walk_roots(b, &walk) == OGC_HEAP_UNSUPPORTED && visited == 0, "unsupported root walk has no callbacks");
    const auto before = ogc_heap_stats(b);
    int cycles = 0;
    check(ogc_heap_add_cycle_hook(a, [](void* user) { ++*static_cast<int*>(user); }, &cycles) == OGC_HEAP_OK, "register local hook");
    check(ogc_heap_collect(a) == OGC_HEAP_OK && cycles == 1, "local collection runs local hook");
    check(ogc_heap_stats(b).collections == before.collections && !*bb.marking, "collection state isolated");
    check(ogc_heap_destroy(a) == OGC_HEAP_BUSY && ogc_heap_desc_of(a, pa) == da, "busy destroy preserves heap");
    ogc_heap_mutator_unregister(a, &ma);
    ogc_heap_mutator_unregister(b, &mb);
    check(ogc_heap_destroy(a) == OGC_HEAP_BUSY, "live handle and hook prevent destroy");
    ogc_heap_handle_free(a, handle);
    ogc_heap_handle_free(a, handle);
    check(ogc_heap_destroy(a) == OGC_HEAP_BUSY, "hook alone prevents destroy");
    ogc_heap_remove_cycle_hook(a, &cycles);
    check(ogc_heap_add_roots(a, &pa, sizeof pa) == OGC_HEAP_OK, "register local roots");
    check(ogc_heap_destroy(a) == OGC_HEAP_BUSY, "root range prevents destroy");
    ogc_heap_remove_roots(a, &pa);
    const auto* replacement = ogc_heap_intern(a, 8, 8, 0, nullptr, 0, nullptr, "replacement");
    *static_cast<uint64_t*>(pa) = 99;
    check(ogc_heap_retag(a, pa, db) == OGC_HEAP_INVALID_ARGUMENT && *static_cast<uint64_t*>(pa) == 99,
          "foreign retag rejected without payload mutation");
    check(ogc_heap_retag(a, pa, replacement) == OGC_HEAP_OK && ogc_heap_desc_of(a, pa) == replacement &&
          *static_cast<uint64_t*>(pa) == 0, "local retag updates descriptor and zeroes payload");
    const auto* large_desc = ogc_heap_intern(a, 65536, 8, 0, nullptr, 0, nullptr, "large");
    void* large = ogc_heap_alloc(a, &ma, large_desc, 0);
    check(!large, "detached mutator cannot allocate");
    ogc_heap_mutator_register(a, &ma);
    large = ogc_heap_alloc(a, &ma, large_desc, 0);
    check(large && ogc_heap_retag(a, large, replacement) == OGC_HEAP_OK &&
          ogc_heap_desc_of(a, large) == replacement, "large-object retag preserves padded payload address");
    ogc_heap_mutator_unregister(a, &ma);
    check(ogc_heap_destroy(a) == OGC_HEAP_OK, "destroy first heap");
    check(ogc_heap_desc_of(b, pb) == db && *static_cast<uint64_t*>(pb) == 91, "destroy preserves other heap");
    check(ogc_heap_destroy(b) == OGC_HEAP_OK, "destroy second heap");
    config.disabled_capabilities = 0;
    config.max_bytes = 1;
    a = ogc_heap_create(&config);
    ma = {};
    ogc_heap_mutator_register(a, &ma);
    da = ogc_heap_intern(a, 16, 8, 0, nullptr, 0, nullptr, "limited");
    check(!ogc_heap_alloc(a, &ma, da, 0), "mapping limit rejects allocation");
    ogc_heap_mutator_unregister(a, &ma);
    check(ogc_heap_destroy(a) == OGC_HEAP_OK, "destroy exhausted heap");
    const int start = released;
    for (int i = 0; i < 20; ++i) {
        a = ogc_heap_create(nullptr); ma = {}; closing_heap = a;
        ogc_heap_mutator_register(a, &ma);
        da = ogc_heap_intern(a, i % 3 ? 16 : 65536, i % 2 ? 8 : 16, 0, nullptr, 0, on_shutdown, "shutdown");
        check(ogc_heap_alloc(a, &ma, da, 0) != nullptr, "shutdown resource allocated");
        ogc_heap_mutator_unregister(a, &ma);
        check(ogc_heap_destroy(a) == OGC_HEAP_OK, "repeated heap lifecycle");
    }
    check(released == start + 20, "shutdown releases live resources exactly once");
}

struct StackProbe { OgcHeap* heap; uint32_t calls = 0; bool collector_word = false, caller_word = false; };
constexpr uintptr_t kCollectorWord = 0x37D1A29B4068C5E0ull, kCallerWord = 0x71E4B83A2096D5C0ull;
[[gnu::noinline]] void walk_in_cycle(void* user) {
    auto& probe = *static_cast<StackProbe*>(user);
    volatile uintptr_t collector_local = kCollectorWord;
    OgcWalk walk{};
    walk.stack = 1;
    walk.user = &probe;
    walk.word = [](void* user, uintptr_t word) {
        auto& probe = *static_cast<StackProbe*>(user);
        probe.collector_word |= word == kCollectorWord;
        probe.caller_word |= word == kCallerWord;
    };
    check(ogc_heap_walk(probe.heap, &walk) == OGC_HEAP_OK, "cycle hook can walk mutator roots");
    check(collector_local == kCollectorWord, "collector-only probe remains live in hook frame");
    ++probe.calls;
}
[[gnu::noinline]] void cycle_stack_checks() {
    OgcHeap* heap = ogc_heap_create(nullptr);
    StackProbe probe{heap};
    OgcMutator mutator{};
    check(heap && ogc_heap_mutator_register(heap, &mutator) == OGC_HEAP_OK, "stack probe heap and mutator");
    if (!heap) return;
    volatile uintptr_t caller_local = kCallerWord;
    ogc_heap_add_cycle_hook(heap, walk_in_cycle, &probe);
    check(ogc_heap_collect(heap) == OGC_HEAP_OK, "stack probe explicit collection");
    auto step = ogc_heap_step(heap, 1000000);
    check(step.status == OGC_HEAP_OK && step.finished, "stack probe incremental completion");
    const auto* desc = ogc_heap_intern(heap, 8, 8, 0, nullptr, 0, nullptr, "stack-probe");
    ogc_heap_set_stress(heap, 1, 0);
    check(ogc_heap_alloc(heap, &mutator, desc, 0) != nullptr, "stack probe automatic collection");
    check(probe.calls == 3 && probe.caller_word && caller_local == kCallerWord, "walk preserves calling mutator stack");
    check(!probe.collector_word, "walk excludes collector and cycle hook temporaries");
    ogc_heap_remove_cycle_hook(heap, &probe);
    ogc_heap_mutator_unregister(heap, &mutator);
    check(ogc_heap_destroy(heap) == OGC_HEAP_OK, "destroy stack probe heap");
}

int main() {
    cycle_stack_checks();
    instance_checks();
    released = 0;
    instance = ogc_heap_create(nullptr);
    check(instance != nullptr, "create explicit heap");
    if (!instance) return 1;
    check(std::strcmp(ogc_heap_provider(), "marksweep") == 0, "the provider names itself");
    const uint64_t caps = ogc_heap_capabilities(instance);
    check((caps & OGC_HEAP_INCREMENTAL) && (caps & OGC_HEAP_GENERATIONAL) && (caps & OGC_HEAP_COLLECTS), "its capabilities");
    ogc_heap_mutator_register(instance, &mutator);
    ogc_heap_set_automatic(instance, 0);   // collections only where this test asks for them

    const uint32_t next_off = 0;
    const OgcTypeDesc* d = ogc_heap_intern(instance, sizeof(Node), 8, 0, &next_off, 1, nullptr, "node");
    g_pair = ogc_heap_intern(instance, sizeof(Pair), 8, 0, kPairRefs, 2, nullptr, "pair");
    g_hooked = ogc_heap_intern(instance, sizeof(Pair), 8, 0, kPairRefs, 2, &on_hooked, "hooked");
    check(ogc_heap_intern(instance, sizeof(Node), 8, 0, &next_off, 1, nullptr, "node") == d, "descriptors are interned");

    // A handle keeps a list alive across collections.
    const uint32_t h = ogc_heap_handle_new(instance, build(d, 10000));
    const uint64_t want = uint64_t(10000) * 9999 / 2;
    ogc_heap_collect(instance);
    check(sum(static_cast<Node*>(ogc_heap_handle_get(instance, h))) == want, "a list a handle holds survives a collection");
    check(ogc_heap_desc_of(instance, ogc_heap_handle_get(instance, h)) == d, "desc_of names the object's descriptor");
    const uint64_t live_kept = ogc_heap_stats(instance).live_bytes;
    ogc_heap_handle_free(instance, h);
    scrub();
    ogc_heap_collect(instance);
    check(ogc_heap_stats(instance).live_bytes < live_kept / 2, "a dropped list is not live after the next collection");

    // A release hook runs from finalize only, once.
    const OgcTypeDesc* r = ogc_heap_intern(instance, 16, 8, 0, nullptr, 0, &on_release, "res");
    for (int i = 0; i < 100; ++i) ogc_heap_alloc(instance, &mutator, r, 0);
    std::memset(mutator.regions, 0, sizeof mutator.regions);   // forget the region: nothing points into it
    ogc_heap_mutator_unregister(instance, &mutator);
    ogc_heap_mutator_register(instance, &mutator);
    scrub();
    ogc_heap_collect(instance);
    check(released == 0, "a hook never runs inside a collection");
    check(ogc_heap_pending_releases(instance) >= 90, "dead objects with hooks are pending");
    const uint64_t ran = ogc_heap_finalize(instance, 1000000);
    check(ran == uint64_t(released) && released >= 90, "finalize runs every pending hook");
    check(ogc_heap_finalize(instance, 1000000) == 0, "a hook runs once");

    // A root range keeps what it points to.
    static Node* range[1];
    ogc_heap_add_roots(instance, range, sizeof range);
    range[0] = build(d, 100);
    ogc_heap_collect(instance);
    check(sum(range[0]) == uint64_t(100) * 99 / 2, "a root range keeps its object");
    struct Reached { const OgcTypeDesc* descriptor; uint64_t count; } reached{d, 0};
    OgcWalk root_walk{};
    root_walk.user = &reached;
    root_walk.object = [](void* user, void*, const OgcTypeDesc* descriptor) {
        auto& r = *static_cast<Reached*>(user);
        r.count += descriptor == r.descriptor;
    };
    check(ogc_heap_walk(instance, &root_walk) == OGC_HEAP_OK && reached.count == 0,
          "selective walk does not implicitly traverse registered roots");
    scrub();
    check(ogc_heap_walk_roots(instance, &root_walk) == OGC_HEAP_OK && reached.count >= 100,
          "complete root walk includes registered ranges and transitive edges");

    // Incremental marking (SATB): A hangs off B deep in a long chain, which the first step does not reach;
    // C was scanned in that step. Moving A from B to C during marking leaves A reachable only from a
    // scanned object: without the barrier's shade, A dies — and since A has a release hook, dying is
    // seen as a pending release, without touching freed memory.
    satb_setup();
    scrub();
    const uint64_t pending0 = ogc_heap_pending_releases(instance);
    ogc_heap_step(instance, 0);   // begin (roots) + one bounded batch of mark-stack entries
    check(ogc_heap_marking(instance) != 0, "the first step leaves the cycle under way");
    satb_move();
    while (true) {
        const auto result = ogc_heap_step(instance, 1000);
        check(result.status == OGC_HEAP_OK, "incremental step status");
        if (result.status != OGC_HEAP_OK || result.finished) break;
    }
    check(ogc_heap_pending_releases(instance) == pending0, "the barrier keeps an object moved behind the marker");
    ogc_heap_handle_free(instance, g_chain);
    ogc_heap_handle_free(instance, g_c);

    // Generational: an old object pointing at a young one only through a dirty card keeps it in a minor.
    ogc_heap_collect(instance);
    ogc_heap_finalize(instance, 1000000);
    gen_setup();          // C is old (it survived a collection); A is young, reachable only via C->side
    scrub();
    const uint64_t pending1 = ogc_heap_pending_releases(instance);
    ogc_heap_minor(instance);
    check(ogc_heap_pending_releases(instance) == pending1, "a minor collection keeps an edge recorded in a dirty card");
    check(ogc_heap_stats(instance).minors >= 1, "the minor is counted");
    ogc_heap_handle_free(instance, g_c);

    ogc_heap_remove_roots(instance, range);
    ogc_heap_mutator_unregister(instance, &mutator);
    check(ogc_heap_destroy(instance) == OGC_HEAP_OK, "destroy quiescent heap");
    if (failures) return 1;
    std::printf("OK marksweep: %d checks\n", checks);
    return 0;
}
