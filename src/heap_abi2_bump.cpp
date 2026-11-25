// SPDX-License-Identifier: GPL-3.0-or-later
// Stable-address NoGC arenas. Small allocations reserve zeroed class runs; generated code bumps
// their regions without per-object metadata, calls or counters. Storage lives until heap destruction.
#include "opengc/mutator_bump.h"
#include "heap_support.h"
#include <new>

struct OgcHeap {
    void *state;
};
namespace {
using namespace opengc::heap_detail;
constexpr uint32_t classes[] = OGC_MUTATOR_CLASSES_INIT;
constexpr uint64_t kRun = 65536, kChunk = 1048576;
constexpr uint32_t kPrefix = OGC_MUTATOR_HEADER_BYTES;
struct Header {
    uint32_t desc, reserved;
    uint64_t padding;
};
static_assert(sizeof(Header) == kPrefix);
struct Segment {
    uint8_t *base;
    uint64_t bytes, cell;
    OgcMutator *owner;
    uint32_t cls;
    uint64_t sealed;
};
struct Chunk {
    uint8_t *base = nullptr;
    uint64_t bytes = 0, used = 0;
    Vec<Segment> segments;
};
struct Hook {
    void (*fn)(void *);
    void *user;
};
struct Handle {
    void *word;
    bool used;
};
struct Heap {
    Vec<OgcTypeDesc *> descs;
    Vec<Chunk *> chunks; // sorted by base; chunks and runs themselves never move
    Chunk *last = nullptr;
    Vec<OgcMutator *> mutators;
    Vec<OgcRootRange> roots;
    Vec<Handle> handles;
    Vec<uint32_t> free_handles;
    Vec<Hook> hooks;
    uint64_t mapped = 0, limit = 0, capabilities = OGC_MUTATOR_CAPABILITIES;
    uint32_t walking = 0;
    bool destroying = false;
};
Heap &heap(OgcHeap *instance) { return *static_cast<Heap *>(instance->state); }
bool busy(const Heap &h) { return h.destroying || h.walking; }
uint32_t size_class(uint64_t bytes) {
    for (uint32_t i = 0; i < OGC_MUTATOR_NUM_CLASSES; ++i)
        if (bytes <= classes[i])
            return i;
    return OGC_MUTATOR_NUM_CLASSES;
}
uint64_t used(const Segment &run) {
    if (!run.owner)
        return run.sealed;
    const uintptr_t next = uintptr_t(run.owner->regions[run.cls].next), base = uintptr_t(run.base);
    // The region may have advanced to a successor run; the predecessor was completely consumed.
    return next >= base && next <= base + run.bytes ? next - base : run.bytes;
}
uint8_t *reserve(Heap &h, uint64_t bytes, uint64_t cell, OgcMutator *owner = nullptr, uint32_t cls = 0) {
    Chunk *chunk = h.last;
    if (!chunk || bytes > chunk->bytes - chunk->used) {
        if (bytes > SIZE_MAX - (kRun - 1))
            return nullptr;
        uint64_t mapping = bytes > kChunk ? (bytes + kRun - 1) / kRun * kRun : kChunk;
        if (h.limit) {
            const uint64_t remaining = h.limit - h.mapped;
            if (mapping > remaining)
                mapping = remaining / kRun * kRun;
            if (mapping < bytes)
                return nullptr;
        }
        auto *base = static_cast<uint8_t *>(os_map(size_t(mapping)));
        if (!base)
            return nullptr;
        chunk = new (std::nothrow) Chunk;
        if (!chunk) {
            os_unmap(base, size_t(mapping));
            return nullptr;
        }
        chunk->base = base;
        chunk->bytes = mapping;
        uint32_t at = h.chunks.size();
        h.chunks.push_back(chunk);
        while (at && uintptr_t(h.chunks[at - 1]->base) > uintptr_t(base)) {
            h.chunks[at] = h.chunks[at - 1];
            --at;
        }
        h.chunks[at] = chunk;
        h.last = chunk;
        h.mapped += mapping;
    }
    uint8_t *base = chunk->base + chunk->used;
    chunk->segments.push_back({base, bytes / cell * cell, cell, owner, cls, owner ? 0 : bytes});
    chunk->used += bytes;
    return base;
}
WalkObject find(Heap &h, uintptr_t address, bool exact = false) {
    uint32_t lo = 0, hi = h.chunks.size();
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (uintptr_t(h.chunks[mid]->base) <= address)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return {};
    const Chunk &chunk = *h.chunks[lo - 1];
    if (address - uintptr_t(chunk.base) >= chunk.used)
        return {};
    lo = 0;
    hi = chunk.segments.size();
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (uintptr_t(chunk.segments[mid].base) <= address)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return {};
    const Segment &run = chunk.segments[lo - 1];
    const uint64_t offset = address - uintptr_t(run.base);
    if (offset >= used(run))
        return {};
    auto *cell = run.base + offset / run.cell * run.cell;
    const uint32_t id = reinterpret_cast<const Header *>(cell)->desc;
    if (!id || id >= h.descs.size() || (exact && address != uintptr_t(cell + kPrefix)))
        return {};
    return {cell + kPrefix, h.descs[id]};
}
struct Find {
    WalkObject operator()(Heap &h, uintptr_t address) const { return find(h, address); }
};
int walk_heap(OgcHeap *instance, const OgcWalk *request, bool roots) {
    if (!instance || !request)
        return OGC_HEAP_INVALID_ARGUMENT;
    Heap &h = heap(instance);
    if (!(h.capabilities & OGC_HEAP_WALK))
        return OGC_HEAP_UNSUPPORTED;
    if (busy(h))
        return OGC_HEAP_BUSY;
    opengc::heap_detail::StackPin stack_pin;
    ++h.walking;
    Walker<Heap, Find> walk{h, *request, {}, {}};
    for (size_t i = 0; i < request->nwords; ++i)
        walk.from(request->words[i]);
    for (size_t i = 0; i < request->nranges; ++i)
        walk.from_range(request->ranges[i].base, request->ranges[i].bytes);
    if (roots || request->handles)
        for (const auto &handle : h.handles)
            if (handle.used)
                walk.reach(uintptr_t(handle.word));
    if (roots)
        for (const auto &root : h.roots)
            walk.from_range(root.base, root.bytes);
    if (roots || request->stack)
        walk.from_stack();
    walk.run();
    --h.walking;
    return OGC_HEAP_OK;
}
bool valid_desc(const Heap &h, const OgcTypeDesc *d) {
    return d && d->id && d->id < h.descs.size() && h.descs[d->id] == d;
}
} // namespace
extern "C" {
const char *ogc_heap_provider(void) { return OGC_MUTATOR_PROVIDER; }
uint64_t ogc_heap_supported_capabilities(void) { return OGC_MUTATOR_CAPABILITIES; }
uint64_t ogc_heap_capabilities(const OgcHeap *instance) {
    return instance ? static_cast<const Heap *>(instance->state)->capabilities : 0;
}
OgcHeap *ogc_heap_create(const OgcHeapConfig *config) {
    const uint64_t disabled = config ? config->disabled_capabilities : 0;
    const uint64_t caps = OGC_MUTATOR_CAPABILITIES & ~disabled;
    if (config && (config->abi_version != OGC_HEAP_ABI_VERSION || config->struct_size != sizeof *config ||
                   (disabled & ~(OGC_HEAP_RETAG | OGC_HEAP_WALK)) || (config->required_capabilities & ~caps)))
        return nullptr;
    auto *instance = new (std::nothrow) OgcHeap{};
    auto *h = new (std::nothrow) Heap;
    if (!instance || !h) {
        delete instance;
        delete h;
        return nullptr;
    }
    h->limit = config ? config->max_bytes : 0;
    h->capabilities = caps;
    h->descs.push_back(nullptr);
    instance->state = h;
    return instance;
}
int ogc_heap_destroy(OgcHeap *instance) {
    if (!instance)
        return OGC_HEAP_INVALID_ARGUMENT;
    Heap &h = heap(instance);
    if (busy(h) || !h.mutators.empty() || !h.roots.empty() || !h.hooks.empty() ||
        h.handles.size() != h.free_handles.size())
        return OGC_HEAP_BUSY;
    h.destroying = true;
    for (Chunk *chunk : h.chunks) {
        for (const Segment &run : chunk->segments)
            for (uint64_t at = 0; at < used(run); at += run.cell) {
                uint8_t *cell = run.base + at;
                const uint32_t id = reinterpret_cast<const Header *>(cell)->desc;
                if (id && h.descs[id]->release)
                    h.descs[id]->release(cell + kPrefix);
            }
        os_unmap(chunk->base, size_t(chunk->bytes));
        delete chunk;
    }
    for (uint32_t i = 1; i < h.descs.size(); ++i) {
        std::free(const_cast<uint32_t *>(h.descs[i]->refs));
        std::free(const_cast<char *>(h.descs[i]->name));
        std::free(h.descs[i]);
    }
    delete &h;
    delete instance;
    return OGC_HEAP_OK;
}
OgcHeapBarrier ogc_heap_barrier(OgcHeap *) { return {nullptr, nullptr}; }
void ogc_heap_set_stress(OgcHeap *, uint32_t, uint32_t) {}
const OgcTypeDesc *ogc_heap_intern(OgcHeap *instance, uint32_t size, uint32_t align, uint8_t vec, const uint32_t *refs,
                                   uint32_t nrefs, void (*release)(void *), const char *name) {
    Heap &h = heap(instance);
    if (busy(h) || vec > 1 || (nrefs && !refs))
        return nullptr;
    for (uint32_t i = 0; i < nrefs; ++i)
        if (refs[i] % 8 || refs[i] > size || size - refs[i] < 8)
            return nullptr;
    align = align > 8 ? 16 : 8;
    if (!name)
        name = "";
    for (uint32_t i = 1; i < h.descs.size(); ++i) {
        const auto *d = h.descs[i];
        if (d->size == size && d->align == align && d->vec == vec && d->nrefs == nrefs && d->release == release &&
            !std::strcmp(d->name, name) && (!nrefs || !std::memcmp(d->refs, refs, nrefs * sizeof(uint32_t))))
            return d;
    }
    auto *d = static_cast<OgcTypeDesc *>(std::calloc(1, sizeof(OgcTypeDesc)));
    auto *copied_refs = nrefs ? static_cast<uint32_t *>(std::malloc(size_t(nrefs) * sizeof(uint32_t))) : nullptr;
    auto *copied_name = static_cast<char *>(std::malloc(std::strlen(name) + 1));
    if (!d || (nrefs && !copied_refs) || !copied_name)
        metadata_exhausted();
    std::memcpy(copied_name, name, std::strlen(name) + 1);
    if (nrefs)
        std::memcpy(copied_refs, refs, size_t(nrefs) * 4);
    d->id = h.descs.size();
    d->size = size;
    d->align = align;
    d->vec = vec;
    d->refs = copied_refs;
    d->nrefs = nrefs;
    d->release = release;
    d->name = copied_name;
    h.descs.push_back(d);
    return d;
}
const OgcTypeDesc *ogc_heap_desc_of(OgcHeap *instance, const void *payload) {
    return instance && payload ? find(heap(instance), uintptr_t(payload), true).descriptor : nullptr;
}
void *ogc_heap_alloc(OgcHeap *instance, OgcMutator *m, const OgcTypeDesc *d, uint64_t count) {
    Heap &h = heap(instance);
    if (busy(h) || !m || m->heap != instance || !valid_desc(h, d))
        return nullptr;
    uint64_t payload = d->size;
    if (d->vec) {
        if (d->size && count > ((uint64_t(1) << 40) - 8) / d->size)
            return nullptr;
        payload = 8 + count * d->size;
    }
    if (payload > SIZE_MAX - kPrefix - 15)
        return nullptr;
    const uint64_t bytes = payload + kPrefix;
    const uint32_t cls = size_class(bytes);
    uint8_t *cell;
    if (cls < OGC_MUTATOR_NUM_CLASSES) {
        auto &region = m->regions[cls];
        if (!region.next || uint64_t(region.limit - region.next) < classes[cls]) {
            cell = reserve(h, kRun, classes[cls], m, cls);
            if (!cell)
                return nullptr;
            region.next = cell;
            region.limit = cell + kRun / classes[cls] * classes[cls];
        }
        cell = region.next;
        region.next += classes[cls];
    } else {
        const uint64_t cell_bytes = (bytes + 15) / 16 * 16;
        cell = reserve(h, cell_bytes, cell_bytes);
        if (!cell)
            return nullptr;
    }
    reinterpret_cast<Header *>(cell)->desc = d->id;
    uint8_t *object = cell + kPrefix;
    if (d->vec)
        std::memcpy(object, &count, 8);
    return object;
}
int ogc_heap_mutator_register(OgcHeap *instance, OgcMutator *m) {
    if (!instance || !m || m->heap)
        return OGC_HEAP_INVALID_ARGUMENT;
    Heap &h = heap(instance);
    if (busy(h))
        return OGC_HEAP_BUSY;
    for (auto &region : m->regions)
        region = {};
    h.mutators.push_back(m);
    m->heap = instance;
    return OGC_HEAP_OK;
}
int ogc_heap_mutator_unregister(OgcHeap *instance, OgcMutator *m) {
    if (!instance || !m || m->heap != instance)
        return OGC_HEAP_INVALID_ARGUMENT;
    Heap &h = heap(instance);
    if (busy(h))
        return OGC_HEAP_BUSY;
    for (uint32_t i = 0; i < h.mutators.size(); ++i)
        if (h.mutators[i] == m) {
            for (Chunk *chunk : h.chunks)
                for (auto &run : chunk->segments)
                    if (run.owner == m) {
                        run.sealed = used(run);
                        run.owner = nullptr;
                    }
            h.mutators[i] = h.mutators.back();
            h.mutators.pop_back();
            for (auto &region : m->regions)
                region = {};
            m->heap = nullptr;
            return OGC_HEAP_OK;
        }
    return OGC_HEAP_INVALID_ARGUMENT;
}
int ogc_heap_collect(OgcHeap *) { return OGC_HEAP_UNSUPPORTED; }
int ogc_heap_minor(OgcHeap *) { return OGC_HEAP_UNSUPPORTED; }
OgcHeapStepResult ogc_heap_step(OgcHeap *, uint64_t) { return {OGC_HEAP_UNSUPPORTED, 0}; }
int ogc_heap_marking(OgcHeap *) { return 0; }
void ogc_heap_set_automatic(OgcHeap *, int) {}
int ogc_heap_automatic(OgcHeap *) { return 0; }
void ogc_heap_shade(OgcHeap *, void *const *) {}
uint64_t ogc_heap_finalize(OgcHeap *, uint64_t) { return 0; }
uint64_t ogc_heap_pending_releases(OgcHeap *) { return 0; }
int ogc_heap_add_roots(OgcHeap *instance, const void *base, size_t bytes) {
    if (!instance || !base || !bytes || bytes > UINTPTR_MAX - uintptr_t(base))
        return OGC_HEAP_INVALID_ARGUMENT;
    Heap &h = heap(instance);
    if (busy(h))
        return OGC_HEAP_BUSY;
    h.roots.push_back({base, bytes});
    return OGC_HEAP_OK;
}
void ogc_heap_remove_roots(OgcHeap *instance, const void *base) {
    Heap &h = heap(instance);
    if (busy(h))
        return;
    for (uint32_t i = h.roots.size(); i; --i)
        if (h.roots[i - 1].base == base) {
            h.roots.erase_at(i - 1);
            return;
        }
}
uint32_t ogc_heap_handle_new(OgcHeap *instance, void *object) {
    Heap &h = heap(instance);
    if (busy(h))
        return 0;
    if (!h.free_handles.empty()) {
        const uint32_t i = h.free_handles.back();
        h.free_handles.pop_back();
        h.handles[i] = {object, true};
        return i + 1;
    }
    h.handles.push_back({object, true});
    return h.handles.size();
}
void *ogc_heap_handle_get(OgcHeap *instance, uint32_t handle) {
    Heap &h = heap(instance);
    return handle && handle <= h.handles.size() && h.handles[handle - 1].used ? h.handles[handle - 1].word : nullptr;
}
void ogc_heap_handle_set(OgcHeap *instance, uint32_t handle, void *object) {
    Heap &h = heap(instance);
    if (!busy(h) && handle && handle <= h.handles.size() && h.handles[handle - 1].used)
        h.handles[handle - 1].word = object;
}
void ogc_heap_handle_free(OgcHeap *instance, uint32_t handle) {
    Heap &h = heap(instance);
    if (busy(h) || !handle || handle > h.handles.size() || !h.handles[handle - 1].used)
        return;
    h.handles[handle - 1] = {nullptr, false};
    h.free_handles.push_back(handle - 1);
}
int ogc_heap_objects_of(OgcHeap *, const OgcTypeDesc *, void (*)(void *, void *), void *) {
    return OGC_HEAP_UNSUPPORTED;
}
int ogc_heap_payload_room(OgcHeap *, const void *, uint64_t *) { return OGC_HEAP_UNSUPPORTED; }
int ogc_heap_retag(OgcHeap *, void *, const OgcTypeDesc *) { return OGC_HEAP_UNSUPPORTED; }
int ogc_heap_walk(OgcHeap *instance, const OgcWalk *request) { return walk_heap(instance, request, false); }
int ogc_heap_walk_roots(OgcHeap *instance, const OgcWalk *request) { return walk_heap(instance, request, true); }
int ogc_heap_add_cycle_hook(OgcHeap *instance, void (*hook)(void *), void *user) {
    Heap &h = heap(instance);
    if (busy(h))
        return OGC_HEAP_BUSY;
    if (!hook)
        return OGC_HEAP_INVALID_ARGUMENT;
    for (auto &entry : h.hooks)
        if (entry.user == user) {
            entry.fn = hook;
            return OGC_HEAP_OK;
        }
    h.hooks.push_back({hook, user});
    return OGC_HEAP_OK;
}
void ogc_heap_remove_cycle_hook(OgcHeap *instance, void *user) {
    Heap &h = heap(instance);
    if (busy(h))
        return;
    for (uint32_t i = 0; i < h.hooks.size(); ++i)
        if (h.hooks[i].user == user) {
            h.hooks.erase_at(i);
            return;
        }
}
OgcHeapStats ogc_heap_stats(OgcHeap *instance) {
    Heap &h = heap(instance);
    OgcHeapStats stats{};
    stats.heap_bytes = h.mapped;
    // Allocation frontiers account for inline bumps, without scanning objects or adding hot counters.
    for (const Chunk *chunk : h.chunks)
        for (const Segment &run : chunk->segments)
            stats.allocated += used(run);
    stats.live_bytes = stats.allocated;
    return stats;
}
} // extern "C"
