// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 OpenGC contributors
//
// opengc/heap.h implemented over any ABI 1 provider (protocol.h): OpenGC::heap_full links the full
// provider, OpenGC::heap_retain the retain provider (OGC_HEAP_QUERY / OGC_HEAP_NAME come from CMake). This
// is where the two layers meet: the heap owns object identity and roots, the provider only traces through
// OgcServices and retires.
//
// Objects come one by one from the system allocator, each a cell [header][payload] laid out as marksweep
// lays it out (mutator_services.h), and are registered in a table sorted by payload address when a lookup
// or a collection needs it. The provider sees an object's payload address as its base, so a Lisp reference
// (a payload address) is an exact edge. A reference field holding anything but a live object's payload
// (an `(fn …)` environment pointing into a frame, a stale word) is reported as null, never as an edge the
// provider would reject. Roots are a snapshot taken when a collection starts: the calling thread's
// registers and stack, the registered ranges and the handles, all as interior edges (a miss is ignored).
//
// Stop-the-world and whole-heap: no inline allocation, no barrier, no incremental step (step runs a whole
// collection), no minor (a minor is a whole collection). Release hooks: an object the provider retires
// whose descriptor has a hook waits, still allocated, for ogc_heap_finalize.
#include "opengc/heap.h"
#include "opengc/opengc.h"
#include "opengc/mutator_services.h"

#include <chrono>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#if defined(__clang__) || defined(__GNUC__)
#define OGC_NO_ASAN __attribute__((no_sanitize("address"), noinline))
#else
#define OGC_NO_ASAN __declspec(noinline)
#endif

extern "C" OgcStatus OGC_CALL OGC_HEAP_QUERY(const OgcQuery*, const OgcProvider**);

extern "C" {
uint8_t ogc_heap_marking_flag = 0;
uint8_t ogc_heap_cards[1] = {0xFF};
}

namespace {

constexpr uint64_t kMinTrigger = 8u << 20;

[[noreturn]] void metadata_exhausted() { std::abort(); }

template <class T> struct Vec {
    T* p = nullptr;
    uint32_t n = 0, cap = 0;
    Vec() = default;
    Vec(const Vec& o) { *this = o; }
    Vec& operator=(const Vec& o) {
        if (this == &o) return *this;
        n = 0;
        reserve(o.n);
        if (o.n) std::memcpy(static_cast<void*>(p), o.p, sizeof(T) * o.n);
        n = o.n;
        return *this;
    }
    ~Vec() { std::free(p); }
    void reserve(uint32_t c) {
        if (c <= cap) return;
        uint32_t nc = cap ? cap * 2 : 8;
        while (nc < c) nc *= 2;
        void* q = std::realloc(static_cast<void*>(p), sizeof(T) * nc);
        if (!q) metadata_exhausted();
        p = static_cast<T*>(q);
        cap = nc;
    }
    void push_back(const T& v) {
        if (n == cap) reserve(n + 1);
        p[n++] = v;
    }
    void pop_back() { --n; }
    T& back() { return p[n - 1]; }
    uint32_t size() const { return n; }
    bool empty() const { return n == 0; }
    T& operator[](uint32_t i) { return p[i]; }
    const T& operator[](uint32_t i) const { return p[i]; }
    T* begin() { return p; }
    T* end() { return p + n; }
    void clear() { n = 0; }
    void erase_at(uint32_t i) {
        std::memmove(static_cast<void*>(p + i), p + i + 1, sizeof(T) * (n - i - 1));
        --n;
    }
};

struct Header {
    uint32_t desc;
    uint32_t aux;
};

enum : uint8_t { kLive = 0, kDoomed = 1, kDead = 2 };

struct Obj {
    uint8_t* cell;
    uint8_t* payload;
    uint64_t bytes;      // the payload's room (at least 1: the provider rejects empty objects)
    uint64_t cell_bytes;
    const OgcTypeDesc* d;
    uint8_t state;
};

struct Hook {
    void (*fn)(void*);
    void* user;
};

struct Heap {
    Vec<OgcTypeDesc*> descs;
    Vec<Obj> objs;
    bool sorted = true;
    Vec<OgcRootRange> roots;
    Vec<void*> handles;
    Vec<uint32_t> free_handles;
    Vec<Hook> hooks;
    Vec<uintptr_t> snapshot;       // the roots of the collection under way
    uint64_t pending_count = 0;
    uint64_t since = 0, trigger = kMinTrigger;
    OgcHeapStats st = {};
    uint32_t stress = 0;
    bool automatic = true;
    bool collecting = false;
    const OgcProvider* provider = nullptr;
};

OgcTypeDesc* new_desc() {
    OgcTypeDesc* d = static_cast<OgcTypeDesc*>(std::calloc(1, sizeof(OgcTypeDesc)));
    if (!d) metadata_exhausted();
    d->name = "";
    d->align = 8;
    return d;
}

Heap& heap() {
    static Heap* h = [] {
        Heap* x = new (std::nothrow) Heap;
        if (!x) metadata_exhausted();
        x->descs.push_back(new_desc());   // id 0: no object
        OgcQuery q{sizeof(OgcQuery), OGC_ABI_MAJOR, 0, 8, OGC_CAP_STABLE_ADDRESSES};
        if (OGC_HEAP_QUERY(&q, &x->provider) != OGC_OK || ogc_validate_provider(&q, x->provider) != OGC_OK) metadata_exhausted();
        return x;
    }();
    return *h;
}

uint32_t payload_offset(const OgcTypeDesc* d) { return d->align > 8 && !d->vec ? 16 : 8; }

int by_payload(const void* a, const void* b) {
    const uintptr_t x = reinterpret_cast<uintptr_t>(static_cast<const Obj*>(a)->payload);
    const uintptr_t y = reinterpret_cast<uintptr_t>(static_cast<const Obj*>(b)->payload);
    return x < y ? -1 : x > y ? 1 : 0;
}

void sort(Heap& h) {
    if (h.sorted) return;
    std::qsort(h.objs.p, h.objs.n, sizeof(Obj), by_payload);
    h.sorted = true;
}

// The index of the live object whose payload room contains `a` (interior) or starts at it (exact), or -1.
int64_t lookup(Heap& h, uintptr_t a, bool exact) {
    sort(h);
    uint32_t lo = 0, hi = h.objs.size();
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (reinterpret_cast<uintptr_t>(h.objs[mid].payload) <= a) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return -1;
    const Obj& o = h.objs[lo - 1];
    const uintptr_t base = reinterpret_cast<uintptr_t>(o.payload);
    if (o.state != kLive) return -1;
    if (exact ? a != base : a - base >= o.bytes) return -1;
    return int64_t(lo - 1);
}

uint64_t reference_count(const Obj& o) {
    if (!o.d->nrefs) return 0;
    if (!o.d->vec) return o.d->nrefs;
    uint64_t len;
    std::memcpy(&len, o.payload, 8);
    return len * o.d->nrefs;
}

const uint8_t* slot_of(const Obj& o, uint64_t index) {
    if (!o.d->vec) return o.payload + o.d->refs[index];
    const uint64_t elem = index / o.d->nrefs, k = index % o.d->nrefs;
    return o.payload + 8 + elem * o.d->size + o.d->refs[k];
}

uint8_t* stack_top() {
#if defined(__APPLE__)
    return static_cast<uint8_t*>(pthread_get_stackaddr_np(pthread_self()));
#elif defined(_WIN32)
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    return reinterpret_cast<uint8_t*>(hi);
#else
    pthread_attr_t attr;
    void* addr = nullptr;
    size_t size = 0;
    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    return static_cast<uint8_t*>(addr) + size;
#endif
}

OGC_NO_ASAN void words_of(Vec<uintptr_t>& out, const void* base, size_t bytes) {
    const uint8_t* lo = static_cast<const uint8_t*>(base);
    const uint8_t* hi = lo + bytes;
    for (const uint8_t* q = reinterpret_cast<const uint8_t*>((reinterpret_cast<uintptr_t>(lo) + 7) & ~uintptr_t(7)); q + 8 <= hi; q += 8) {
        uintptr_t w;
        std::memcpy(&w, q, 8);
        if (w >= 4096 && !(w & 7)) out.push_back(w);
    }
}

OGC_NO_ASAN void snapshot_roots(Heap& h) {
    h.snapshot.clear();
    for (const OgcRootRange& r : h.roots) words_of(h.snapshot, r.base, r.bytes);
    for (void* o : h.handles)
        if (o) h.snapshot.push_back(reinterpret_cast<uintptr_t>(o));
    jmp_buf regs;
    setjmp(regs);
    words_of(h.snapshot, &regs, sizeof regs);
    volatile uint8_t here = 0;
    const uint8_t* lo = const_cast<const uint8_t*>(&here);
    words_of(h.snapshot, lo, size_t(stack_top() - lo));
}

// ── OgcServices over the table (context: the Heap) ──────────────────────────────────────────────────
uint64_t live_count(Heap& h) {
    uint64_t n = 0;
    for (const Obj& o : h.objs) n += o.state == kLive;
    return n;
}

OgcStatus OGC_CALL svc_begin(void* c, uint64_t* count) {
    *count = live_count(*static_cast<Heap*>(c));
    return OGC_OK;
}
void OGC_CALL svc_end(void*) {}

OgcStatus OGC_CALL svc_object_next(void* c, uint64_t* cursor, OgcObjectInfo* info, uint32_t* has) {
    Heap& h = *static_cast<Heap*>(c);
    while (*cursor < h.objs.size() && h.objs[uint32_t(*cursor)].state != kLive) ++*cursor;
    if (*cursor >= h.objs.size()) { *has = 0; return OGC_OK; }
    const Obj& o = h.objs[uint32_t(*cursor)];
    info->id = OgcId{1, *cursor + 1};   // the serial is the table index + 1: the table is frozen during the lease
    info->base = o.payload;
    info->bytes = o.bytes;
    info->reference_count = reference_count(o);
    ++*cursor;
    *has = 1;
    return OGC_OK;
}

OgcStatus OGC_CALL svc_root_next(void* c, uint64_t* cursor, OgcEdge* edge, uint32_t* has) {
    Heap& h = *static_cast<Heap*>(c);
    if (*cursor >= h.snapshot.size()) { *has = 0; return OGC_OK; }
    *edge = OgcEdge{h.snapshot[uint32_t(*cursor)], OGC_EDGE_INTERIOR, 0};
    ++*cursor;
    *has = 1;
    return OGC_OK;
}

OgcStatus OGC_CALL svc_trace_edge(void* c, OgcId id, uint64_t index, OgcEdge* edge) {
    Heap& h = *static_cast<Heap*>(c);
    if (id.domain != 1 || !id.serial || id.serial > h.objs.size()) return OGC_STALE_HANDLE;
    const Obj& o = h.objs[uint32_t(id.serial - 1)];
    if (index >= reference_count(o)) return OGC_INVALID_ARGUMENT;
    uintptr_t w;
    std::memcpy(&w, slot_of(o, index), 8);
    *edge = OgcEdge{lookup(h, w, true) >= 0 ? w : 0, OGC_EDGE_EXACT, 0};
    return OGC_OK;
}

OgcStatus OGC_CALL svc_retire(void* c, OgcId id) {
    Heap& h = *static_cast<Heap*>(c);
    if (id.domain != 1 || !id.serial || id.serial > h.objs.size()) return OGC_STALE_HANDLE;
    Obj& o = h.objs[uint32_t(id.serial - 1)];
    if (o.d->release) {
        o.state = kDoomed;
        h.pending_count++;
    } else {
        o.state = kDead;
    }
    return OGC_OK;
}

void free_obj(Heap& h, Obj& o) {
    h.st.heap_bytes -= o.cell_bytes;
    std::free(o.cell);
}

void collect(Heap& h) {
    if (h.collecting) return;
    h.collecting = true;
    sort(h);
    snapshot_roots(h);
    OgcServices svc{sizeof(OgcServices), OGC_ABI_MAJOR, &h, svc_begin, svc_end, svc_object_next, svc_root_next, svc_trace_edge, svc_retire};
    OgcCollection result{};
    const OgcStatus status = h.provider->collect(&svc, ogc_system_allocator(), UINT64_MAX, &result);
    if (status != OGC_OK) metadata_exhausted();   // a scan failure here is a broken invariant, not a recoverable state
    uint32_t keep = 0;
    uint64_t live = 0;
    for (uint32_t i = 0; i < h.objs.size(); ++i) {
        Obj& o = h.objs[i];
        if (o.state == kDead) { free_obj(h, o); continue; }
        if (o.state == kLive) live += o.cell_bytes;
        h.objs[keep++] = o;
    }
    h.objs.n = keep;
    h.snapshot.clear();
    h.st.live_bytes = live;
    h.st.collections++;
    h.since = 0;
    h.trigger = 2 * live > kMinTrigger ? 2 * live : kMinTrigger;
    h.collecting = false;
    if (!h.hooks.empty()) {
        const Vec<Hook> hooks = h.hooks;
        for (const Hook& e : hooks) e.fn(e.user);
    }
}

struct Walker {
    const OgcWalk& w;
    Vec<uint8_t> seen;
    Vec<uint32_t> todo;
    void reach(uintptr_t a, bool exact) {
        if (a < 4096 || (a & 7)) return;
        Heap& h = heap();
        const int64_t i = lookup(h, a, exact);
        if (i < 0 || seen[uint32_t(i)]) return;
        seen[uint32_t(i)] = 1;
        todo.push_back(uint32_t(i));
    }
    void from(uintptr_t x) {
        if (w.word) w.word(w.user, x);
        reach(x, false);
    }
    OGC_NO_ASAN void from_range(const void* base, size_t bytes) {
        const uint8_t* lo = static_cast<const uint8_t*>(base);
        const uint8_t* hi = lo + bytes;
        for (const uint8_t* q = reinterpret_cast<const uint8_t*>((reinterpret_cast<uintptr_t>(lo) + 7) & ~uintptr_t(7)); q + 8 <= hi; q += 8) {
            uintptr_t x;
            std::memcpy(&x, q, 8);
            from(x);
        }
    }
    OGC_NO_ASAN void from_stack() {
        jmp_buf regs;
        setjmp(regs);
        from_range(&regs, sizeof regs);
        volatile uint8_t here = 0;
        const uint8_t* lo = const_cast<const uint8_t*>(&here);
        from_range(lo, size_t(stack_top() - lo));
    }
    void run() {
        Heap& h = heap();
        while (!todo.empty()) {
            const Obj o = h.objs[todo.back()];
            todo.pop_back();
            if (w.object) w.object(w.user, o.payload, o.d);
            uint64_t bytes = o.d->size;
            if (o.d->vec) {
                uint64_t len;
                std::memcpy(&len, o.payload, 8);
                bytes = 8 + len * o.d->size;
            }
            if (w.word)
                for (uint64_t off = 0; off + 8 <= bytes; off += 8) {
                    uintptr_t x;
                    std::memcpy(&x, o.payload + off, 8);
                    w.word(w.user, x);
                }
            const uint64_t n = reference_count(o);
            for (uint64_t k = 0; k < n; ++k) {
                uintptr_t x;
                std::memcpy(&x, slot_of(o, k), 8);
                reach(x, true);
            }
        }
    }
};

}  // namespace

extern "C" {

const char* ogc_heap_provider(void) { return OGC_HEAP_NAME; }

uint64_t ogc_heap_capabilities(void) {
    return (heap().provider->abi.capabilities & OGC_CAP_EXPLICIT_FULL) ? (OGC_HEAP_COLLECTS | OGC_HEAP_RELEASE_HOOKS) : 0;
}

void ogc_heap_set_stress(uint32_t stress, uint32_t steps) {
    (void)steps;   // no incremental marking to step
    heap().stress = stress;
}

const OgcTypeDesc* ogc_heap_intern(uint32_t size, uint32_t align, uint8_t vec, const uint32_t* refs, uint32_t nrefs,
                                   void (*release)(void*), const char* name) {
    Heap& h = heap();
    if (!name) name = "";
    align = align > 8 ? 16 : 8;
    for (OgcTypeDesc* d : h.descs) {
        if (d->size != size || d->align != align || d->vec != vec || d->nrefs != nrefs || d->release != release ||
            std::strcmp(d->name, name) != 0)
            continue;
        bool same = true;
        for (uint32_t i = 0; i < nrefs && same; ++i) same = d->refs[i] == refs[i];
        if (same) return d;
    }
    OgcTypeDesc* d = new_desc();
    d->id = h.descs.size();
    d->size = size;
    d->align = align;
    d->vec = vec;
    d->release = release;
    const size_t nl = std::strlen(name);
    char* copy = static_cast<char*>(std::malloc(nl + 1));
    uint32_t* rs = nrefs ? static_cast<uint32_t*>(std::malloc(sizeof(uint32_t) * nrefs)) : nullptr;
    if (!copy || (nrefs && !rs)) metadata_exhausted();
    std::memcpy(copy, name, nl + 1);
    if (nrefs) std::memcpy(rs, refs, sizeof(uint32_t) * nrefs);
    d->name = copy;
    d->refs = rs;
    d->nrefs = nrefs;
    h.descs.push_back(d);
    return d;
}

const OgcTypeDesc* ogc_heap_desc_of(const void* payload) {
    const uint8_t* p = static_cast<const uint8_t*>(payload);
    Header hd;
    std::memcpy(&hd, p - 8, sizeof hd);
    if (hd.desc == 0) std::memcpy(&hd, p - 16, sizeof hd);
    return heap().descs[hd.desc];
}

void* ogc_heap_alloc(OgcMutator* m, const OgcTypeDesc* d, uint64_t count) {
    (void)m;
    Heap& h = heap();
    uint64_t payload = d->size;
    if (d->vec) {
        if (d->size && count > (uint64_t(1) << 40) / d->size) return nullptr;
        payload = 8 + count * d->size;
    }
    if (!payload) payload = 1;
    const uint64_t room = (payload + 15) & ~uint64_t(15);
    const uint64_t cell_bytes = payload_offset(d) + room;
    if (h.automatic && (h.stress || h.since + cell_bytes > h.trigger)) collect(h);
    // The system allocator's alignment is 16 on every 64-bit target this builds for, which is what a
    // 16-byte aligned payload at +16 needs.
    uint8_t* cell = static_cast<uint8_t*>(std::calloc(1, size_t(cell_bytes)));
    if (!cell || (reinterpret_cast<uintptr_t>(cell) & 15)) {
        std::free(cell);
        return nullptr;
    }
    uint8_t* obj = cell + payload_offset(d);
    Header hd{d->id, 0};
    std::memcpy(cell, &hd, sizeof hd);
    if (d->vec) std::memcpy(obj, &count, 8);
    if (h.sorted && !h.objs.empty() && h.objs.back().payload > obj) h.sorted = false;
    h.objs.push_back(Obj{cell, obj, room, cell_bytes, d, kLive});
    h.since += cell_bytes;
    h.st.allocated += cell_bytes;
    h.st.heap_bytes += cell_bytes;
    return obj;
}

int ogc_heap_mutator_register(OgcMutator* m) {
    for (OgcRegion& r : m->regions) r = OgcRegion{nullptr, nullptr};
    return OGC_HEAP_OK;
}
void ogc_heap_mutator_unregister(OgcMutator* m) { (void)m; }

void ogc_heap_collect(void) { collect(heap()); }
void ogc_heap_minor(void) { collect(heap()); }   // no generations: a minor is a whole collection
int ogc_heap_step(uint64_t budget_us) {
    (void)budget_us;   // no incremental marking: a step is a whole collection, which finishes it
    collect(heap());
    return 1;
}
int ogc_heap_marking(void) { return 0; }
void ogc_heap_set_automatic(int on) { heap().automatic = on != 0; }
int ogc_heap_automatic(void) { return heap().automatic ? 1 : 0; }
void ogc_heap_shade(void* const* word) { (void)word; }   // never marking

uint64_t ogc_heap_finalize(uint64_t budget_us) {
    Heap& h = heap();
    if (h.collecting) return 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(budget_us);
    uint64_t ran = 0;
    // A hook may allocate (and so collect): take each doomed object out of the table before running it.
    for (uint32_t i = 0; i < h.objs.size();) {
        if (h.objs[i].state != kDoomed) { ++i; continue; }
        const Obj o = h.objs[i];
        h.objs.erase_at(i);
        h.pending_count--;
        o.d->release(o.payload);
        h.st.heap_bytes -= o.cell_bytes;
        std::free(o.cell);
        ++ran;
        if (std::chrono::steady_clock::now() >= end) break;
    }
    return ran;
}

uint64_t ogc_heap_pending_releases(void) { return heap().pending_count; }

int ogc_heap_add_roots(const void* base, size_t bytes) {
    heap().roots.push_back(OgcRootRange{base, bytes});
    return OGC_HEAP_OK;
}

void ogc_heap_remove_roots(const void* base) {
    Vec<OgcRootRange>& r = heap().roots;
    for (uint32_t i = 0; i < r.size(); ++i)
        if (r[i].base == base) { r[i] = r.back(); r.pop_back(); return; }
}

uint32_t ogc_heap_handle_new(void* obj) {
    Heap& h = heap();
    if (!h.free_handles.empty()) {
        const uint32_t i = h.free_handles.back();
        h.free_handles.pop_back();
        h.handles[i] = obj;
        return i + 1;
    }
    h.handles.push_back(obj);
    return h.handles.size();
}
void* ogc_heap_handle_get(uint32_t handle) {
    Heap& h = heap();
    return handle && handle <= h.handles.size() ? h.handles[handle - 1] : nullptr;
}
void ogc_heap_handle_set(uint32_t handle, void* obj) {
    Heap& h = heap();
    if (handle && handle <= h.handles.size()) h.handles[handle - 1] = obj;
}
void ogc_heap_handle_free(uint32_t handle) {
    Heap& h = heap();
    if (!handle || handle > h.handles.size()) return;
    h.handles[handle - 1] = nullptr;
    h.free_handles.push_back(handle - 1);
}

int ogc_heap_objects_of(const OgcTypeDesc* d, void (*each)(void* user, void* payload), void* user) {
    // Every registered live object of `d`: right after a collection these are exactly the reachable ones.
    Heap& h = heap();
    for (const Obj& o : h.objs)
        if (o.state == kLive && o.d == d) each(user, o.payload);
    return OGC_HEAP_OK;
}

uint64_t ogc_heap_payload_room(const void* payload) {
    Heap& h = heap();
    const int64_t i = lookup(h, reinterpret_cast<uintptr_t>(payload), false);
    if (i < 0) return 0;
    const Obj& o = h.objs[uint32_t(i)];
    return o.bytes - uint64_t(static_cast<const uint8_t*>(payload) - o.payload);
}

void ogc_heap_retag(void* payload, const OgcTypeDesc* to) {
    Heap& h = heap();
    const int64_t i = lookup(h, reinterpret_cast<uintptr_t>(payload), true);
    if (i < 0) return;
    Obj& o = h.objs[uint32_t(i)];
    uint8_t* p = static_cast<uint8_t*>(payload);
    Header hd;
    std::memcpy(&hd, p - payload_offset(to), sizeof hd);
    hd.desc = to->id;
    std::memcpy(p - payload_offset(to), &hd, sizeof hd);
    std::memset(p, 0, size_t(o.bytes));
    o.d = to;
}

int ogc_heap_walk(const OgcWalk* walk) {
    Heap& h = heap();
    sort(h);
    Walker w{*walk, {}, {}};
    w.seen.reserve(h.objs.size());
    for (uint32_t i = 0; i < h.objs.size(); ++i) w.seen.push_back(0);
    for (size_t i = 0; i < walk->nwords; ++i) w.from(walk->words[i]);
    for (size_t i = 0; i < walk->nranges; ++i) w.from_range(walk->ranges[i].base, walk->ranges[i].bytes);
    if (walk->handles)
        for (void* o : h.handles) w.reach(reinterpret_cast<uintptr_t>(o), false);
    if (walk->stack) w.from_stack();
    w.run();
    return OGC_HEAP_OK;
}

int ogc_heap_add_cycle_hook(void (*hook)(void*), void* user) {
    Heap& h = heap();
    for (Hook& e : h.hooks)
        if (e.user == user) { e.fn = hook; return OGC_HEAP_OK; }
    h.hooks.push_back(Hook{hook, user});
    return OGC_HEAP_OK;
}

void ogc_heap_remove_cycle_hook(void* user) {
    Vec<Hook>& v = heap().hooks;
    for (uint32_t i = 0; i < v.size(); ++i)
        if (v[i].user == user) { v.erase_at(i); return; }
}

OgcHeapStats ogc_heap_stats(void) { return heap().st; }

}  // extern "C"
