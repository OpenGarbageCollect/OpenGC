/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Compiled ONCE into an archive; neither provider nor mutator headers allowed. */
#include "opengc/portable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "portable client line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define OK(x) CHECK((x) == OGC_PORTABLE_OK)
struct ReleaseState {
    const OgcPortableApi* api;
    OgcPortableHeap* heap;
    int count, reentry;
    uint64_t total;
};
static void release(void* user, void* payload) {
    struct ReleaseState* s = (struct ReleaseState*)user;
    uint64_t value;
    memcpy(&value, (char*)payload + 8, 8);
    s->total += value;
    ++s->count;
    s->reentry = s->api->safepoint(s->heap);
}
#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE __declspec(noinline)
#endif
static NOINLINE int garbage(const OgcPortableApi* api, OgcPortableHeap* h, const OgcPortableType* type) {
    unsigned i;
    for (i = 0; i < 100; ++i) {
        OgcRef value = NULL;
        if (api->alloc(h, type, 0, &value)) return 1;
    }
    return 0;
}
static NOINLINE void scrub(void) {
    volatile uintptr_t words[4096];
    size_t i;
    for (i = 0; i < 4096; ++i) words[i] = 0;
    (void)words[0];
}
int opengc_portable_client(const OgcPortableApi* api) {
    OgcPortableConfig config = {OGC_PORTABLE_ABI, sizeof(OgcPortableConfig), OGC_PORTABLE_MODEL, 0, 0, 0};
    OgcPortableHeap *h = NULL, *other = NULL, *sentinel;
    const OgcPortableType *node, *array, *refs_array, *empty_type, *actual;
    uint32_t node_refs[] = {0}, array_refs[] = {0};
    struct ReleaseState releases = {api, NULL, 0, 0, 0};
    OgcPortableLayout layout = {0};
    layout.struct_size = sizeof(layout); layout.size = 16; layout.align = 8;
    layout.nrefs = 1; layout.refs = node_refs; layout.name = "node";
    layout.release = release; layout.user = &releases;
    OgcRef* roots = (OgcRef*)calloc(6, sizeof(OgcRef));
    OgcPortableRoots *frame, *nested;
    OgcPortableHandle* handle;
    OgcPortablePin* pin;
    void* address;
    OgcRef got;
    uint64_t id, again, released = 99;
    int finished = 71;
    CHECK(roots);
    CHECK(api->abi == OGC_PORTABLE_ABI && api->model == OGC_PORTABLE_MODEL);
    config.required_capabilities = UINT64_C(1) << 63;
    sentinel = (OgcPortableHeap*)&config;
    CHECK(api->create(&config, &sentinel) == OGC_PORTABLE_UNSUPPORTED);
    CHECK(sentinel == (OgcPortableHeap*)&config);
    config.required_capabilities = OGC_PORTABLE_PIN | OGC_PORTABLE_RELEASE;
    OK(api->create(&config, &h)); releases.heap = h;
    OK(api->create(&config, &other));
    OK(api->type_register(h, &layout, &node));
    /* Registration owns its metadata, not these caller buffers. */
    node_refs[0] = 8;
    OK(api->roots_enter(h, roots, 6, &frame));
    OK(api->alloc(h, node, 0, &roots[0]));
    OK(api->alloc(h, node, 0, &roots[1]));
    CHECK(api->alloc(other, node, 0, &roots[2]) == OGC_PORTABLE_INVALID);
    OK(api->identity(h, roots[0], &id)); CHECK(id);
    OK(api->identity(h, roots[1], &again)); CHECK(again != id);
    OK(api->type_of(h, roots[0], &actual)); CHECK(actual == node);
    OK(api->store_ref(h, roots[0], 0, roots[1]));
    OK(api->store_ref(h, roots[1], 0, roots[0]));
    CHECK(api->store_ref(h, roots[0], 8, roots[1]) == OGC_PORTABLE_INVALID);
    CHECK(api->store_ref(other, roots[0], 0, NULL) == OGC_PORTABLE_INVALID);
    CHECK(api->load_ref(h, roots[0], UINT64_MAX, &roots[2]) == OGC_PORTABLE_INVALID);
    CHECK(!roots[2]);
    OK(api->pin(h, roots[0], &pin, &address));
    { uint64_t value = 42; memcpy((char*)address + 8, &value, 8); }
    CHECK(api->destroy(h) == OGC_PORTABLE_BUSY);
    OK(api->unpin(h, pin));
    OK(api->handle_new(h, roots[0], &handle));
    CHECK(api->handle_get(other, handle, &roots[2]) == OGC_PORTABLE_INVALID);
    OK(api->roots_enter(h, roots, 1, &nested));
    CHECK(api->roots_leave(h, frame) == OGC_PORTABLE_INVALID);
    OK(api->roots_leave(h, nested));
    layout.size = 8; layout.align = 8; layout.is_array = 1;
    layout.refs = array_refs; layout.name = "ref-array"; layout.release = NULL;
    OK(api->type_register(h, &layout, &refs_array));
    OK(api->alloc(h, refs_array, 4, &roots[2]));
    OK(api->store_ref(h, roots[2], 0, roots[0]));
    OK(api->store_ref(h, roots[2], 8, roots[1]));
    OK(api->copy_refs(h, roots[2], 8, roots[2], 0, 3));
    OK(api->load_ref(h, roots[2], 8, &got)); CHECK(got == roots[0]);
    OK(api->load_ref(h, roots[2], 16, &got)); CHECK(got == roots[1]);
    OK(api->load_ref(h, roots[2], 24, &got)); CHECK(!got);
    CHECK(api->copy_refs(h, roots[2], 0, roots[2], 24, 2) == OGC_PORTABLE_INVALID);
    OK(api->load_ref(h, roots[2], 0, &got)); CHECK(got == roots[0]);
    /* Array-of-struct reference offsets repeat per element; scalar slots reject. */
    layout.size = 16; layout.align = 16;
    OK(api->type_register(h, &layout, &array));
    OK(api->alloc(h, array, 3, &roots[3]));
    CHECK((uintptr_t)roots[3] % 16 == 0);
    OK(api->store_ref(h, roots[3], 32, roots[0]));
    CHECK(api->store_ref(h, roots[3], 40, roots[0]) == OGC_PORTABLE_INVALID);
    OK(api->write_word(h, (char*)roots[3] + 32, roots[1]));
    CHECK(api->write_word(h, (char*)roots[3] + 40, roots[1]) == OGC_PORTABLE_INVALID);
    CHECK(api->write_word(h, (char*)roots[3] + 32, (char*)roots[1] + 8) == OGC_PORTABLE_INVALID);
    OK(api->read_word(h, (char*)roots[3] + 32, &got)); CHECK(got == roots[1]);
    OK(api->write_word(h, (char*)roots[3] + 32, roots[0]));
    CHECK(api->alloc(h, array, UINT64_MAX, &roots[4]) == OGC_PORTABLE_OOM);
    CHECK(!roots[4]);
    layout.nrefs = 0; layout.refs = NULL; layout.size = 8; layout.align = 8;
    OK(api->type_register(h, &layout, &empty_type));
    OK(api->alloc(h, empty_type, 0, &roots[4]));
    OK(api->identity(h, roots[4], &again));
    if (api->capabilities & OGC_PORTABLE_COLLECTS) {
        OK(api->collect(h));
        OK(api->alloc(h, node, 0, &roots[5]));
        OK(api->store_ref(h, roots[0], 0, roots[5]));
        roots[5] = NULL;
        OK(api->minor(h));
        OK(api->load_ref(h, roots[0], 0, &roots[5]));
        OK(api->type_of(h, roots[5], &actual)); CHECK(actual == node);
        do { OK(api->step(h, 0, &finished)); } while (!finished);
    } else {
        CHECK(api->collect(h) == OGC_PORTABLE_UNSUPPORTED);
        CHECK(api->minor(h) == OGC_PORTABLE_UNSUPPORTED);
        CHECK(api->step(h, 0, &finished) == OGC_PORTABLE_UNSUPPORTED);
        CHECK(finished == 71);
    }
    OK(api->identity(h, roots[4], &id)); CHECK(id == again);
    OK(api->identity(h, roots[0], &id));
    OK(api->handle_get(h, handle, &got)); CHECK(got == roots[0]);
    OK(api->safepoint(h));
    OK(api->load_ref(h, roots[0], 0, &got));
    CHECK(got == ((api->capabilities & OGC_PORTABLE_COLLECTS) ? roots[5] : roots[1]));
    OK(api->finalize(h, 10000, &released)); CHECK(released == 0 && releases.count == 0);
    CHECK(api->step(h, UINT64_MAX, &finished) == OGC_PORTABLE_INVALID);
    { OgcPortableStats stats; OK(api->stats(h, &stats)); CHECK(stats.allocated > 0 && stats.heap_bytes > 0); }
    OK(api->handle_set(h, handle, NULL));
    OK(api->handle_free(h, handle));
    OK(api->roots_leave(h, frame));
    OK(api->destroy(h));
    CHECK(releases.count == ((api->capabilities & OGC_PORTABLE_COLLECTS) ? 3 : 2) &&
          releases.total == 42 && releases.reentry == OGC_PORTABLE_BUSY);
    OK(api->destroy(other));
    free(roots);
    /* Garbage must actually be collected, not just released at shutdown. A few
     * stale conservative register/stack words may retain objects in ABI 2. */
    releases.count = 0; releases.total = 0;
    OK(api->create(&config, &h)); releases.heap = h;
    layout.size = 16; layout.align = 8; layout.is_array = 0;
    layout.release = release; layout.user = &releases;
    OK(api->type_register(h, &layout, &node));
    CHECK(!garbage(api, h, node));
    scrub();
    if (api->capabilities & OGC_PORTABLE_COLLECTS) {
        OK(api->collect(h));
        OK(api->finalize(h, 100000, &released));
        CHECK(released == 100 && releases.count == 100);
    }
    OK(api->destroy(h)); CHECK(releases.count == 100);
    layout.release = NULL;
    /* Optional services are native ABI 3 operations, not ABI 2 adapters. */
    OK(api->create(&config, &h));
    layout.release = NULL;
    OK(api->type_register(h, &layout, &node));
    {
        OgcPortableNoGC *window, *inner;
        OgcPortableWeak* weak;
        uint64_t bytes, count;
        OgcRef value = NULL;
        volatile uintptr_t scalar;
        OK(api->alloc(h, node, 0, &value));
        scalar = (uintptr_t)value;
        OK(api->extent(h, value, &bytes, &count)); CHECK(bytes == 16 && count == 1);
        OK(api->weak_new(h, value, &weak));
        OK(api->no_gc_enter(h, &window));
        OK(api->no_gc_enter(h, &inner));
        CHECK(api->no_gc_leave(h, window) == OGC_PORTABLE_INVALID);
        CHECK(api->alloc(h, node, 0, &got) == OGC_PORTABLE_BUSY);
        CHECK(api->collect(h) == OGC_PORTABLE_BUSY);
        OK(api->no_gc_leave(h, inner)); OK(api->no_gc_leave(h, window));
        if (api->capabilities & OGC_PORTABLE_COLLECTS) {
            /* Neither the scalar nor this raw C local is a root. */
            OK(api->collect(h)); OK(api->weak_get(h, weak, &got)); CHECK(!got);
        } else { OK(api->weak_get(h, weak, &got)); CHECK(got == value); }
        CHECK(scalar != 0);
        OK(api->weak_free(h, weak));
    }
    OK(api->destroy(h));
    /* Object mapping OOM and outputs stay intact. */
    config.max_bytes = 1;
    OK(api->create(&config, &h));
    layout.is_array = 0;
    OK(api->type_register(h, &layout, &node));
    got = (OgcRef)&config;
    CHECK(api->alloc(h, node, 0, &got) == OGC_PORTABLE_OOM);
    CHECK(got == (OgcRef)&config);
    OK(api->destroy(h));
    return 0;
}
