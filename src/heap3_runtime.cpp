// SPDX-License-Identifier: GPL-3.0-or-later
// Shared runtime preset: types, roots, handles, WALK and language-neutral services.
#include "heap3_internal.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
namespace ogc_preset {
int check(OgcPortableHeap* h) {
    if (!h) return OGC_PORTABLE_INVALID;
    if (h->owner_token != thread_token()) return OGC_PORTABLE_WRONG_OWNER;
    return h->busy ? OGC_PORTABLE_BUSY : OGC_PORTABLE_OK;
}
bool local_type(OgcPortableHeap* h, const OgcPortableType* type) {
    return type && type->owner == h;
}

bool local_ref(OgcPortableHeap* h, OgcRef ref) { return !ref || object(h, ref); }
enum class RefSlot { Invalid, Other, Strong };
RefSlot ref_slot_kind(Object p, uint64_t offset) {
    if (!p || offset > p.bytes() || p.bytes() - offset < sizeof(OgcRef)) return RefSlot::Invalid;
    const auto& layout = p.type()->layout;
    if (offset < layout.prefix_bytes) return RefSlot::Invalid;
    const uint64_t relative = offset - layout.prefix_bytes;
    const uint32_t within = layout.size == sizeof(OgcRef)
        ? uint32_t(relative & (sizeof(OgcRef) - 1))
        : uint32_t(relative % layout.size);
    for (uint32_t i = 0; i < layout.nrefs; ++i)
        if (layout.refs[i] == within) return RefSlot::Strong;
    RefSlot kind = RefSlot::Invalid;
    for (uint32_t i = 0; i < layout.ntraces; ++i) {
        const auto& trace = layout.traces[i];
        if (trace.offset != within) continue;
        if (trace.kind == OGC_TRACE_STRONG) return RefSlot::Strong;
        if (trace.kind < OGC_TRACE_IF_U8) kind = RefSlot::Other;
    }
    return kind;
}
bool ref_slot(Object p, uint64_t offset) { return ref_slot_kind(p, offset) != RefSlot::Invalid; }
OgcRef* slot(OgcRef ref, uint64_t offset) {
    return reinterpret_cast<OgcRef*>(static_cast<uint8_t*>(ref) + offset);
}
template<class Trace, class Address, class Visit>
void trace_program(const Trace* program, size_t count, Address address, Visit visit) {
    uint32_t skip = 0;
    for (size_t j = 0; j < count; ++j) {
        const auto& t = program[j];
        if (t.kind == OGC_TRACE_END) { if (skip) --skip; continue; }
        if (t.kind == OGC_TRACE_IF_U8 || t.kind == OGC_TRACE_IF_U64) {
            if (skip) { ++skip; continue; }
            uint64_t tag = 0; std::memcpy(&tag, address(t), t.kind == OGC_TRACE_IF_U8 ? 1 : 8);
            if (tag >= 64 || !(t.tag_values & (UINT64_C(1) << tag))) skip = 1;
        } else if (!skip) visit(t);
    }
}
void shade_node(OgcPortableHeap* h, Object n, bool young = false) {
    if (n) h->algorithm->shade(&h->collector, &record(n)->gc, young);
}
void shade(OgcPortableHeap* h, OgcRef value, bool young = false) { shade_node(h, object(h, value), young); }
void remember(OgcPortableHeap* h, Object n) {
    if (!no_gc && n && record(n)->gc.old) h->algorithm->remember(&h->collector, &record(n)->gc);
}
void trace_strong(OgcPortableHeap* h, const void* address, OgcPolicyVisit visit, void* user) {
    OgcRef ref;
    std::memcpy(&ref, address, sizeof(ref));
    if (!ref) return;
    const auto target = object(h, ref);
    if (!target) std::abort();
    const OgcPolicyEdge edge{&record(target)->gc, const_cast<void*>(address), 0, 0};
    visit(user, &edge);
}
void trace_word(OgcPortableHeap* h, const void* address, uint32_t kind,
                const void* tag_address, uint64_t tags, OgcPolicyVisit visit, void* user) {
    if (kind == OGC_TRACE_STRONG) { trace_strong(h, address, visit, user); return; }
    if (kind == OGC_TRACE_CODE) return;
    if (kind == OGC_TRACE_TAGGED) {
        uint64_t tag; std::memcpy(&tag, tag_address, 8);
        if (tag >= 64 || !(tags & (UINT64_C(1) << tag))) return;
    }
    OgcRef ref; std::memcpy(&ref, address, 8);
    if (!ref) return;
    auto n = containing(h, uintptr_t(ref));
    if (!n) return;
    const OgcPolicyEdge edge{&record(n)->gc, const_cast<void*>(address), intptr_t(uintptr_t(ref) - uintptr_t(payload(n))),
        kind == OGC_TRACE_PINNED_AMBIGUOUS ? OGC_POLICY_PINNED : 0u};
    visit(user, &edge);
}
void visit_edges(OgcPortableHeap* h, Object n, OgcPolicyVisit visit, void* user) {
    // Full marking enumerates even leaves. Minor/WALK must not refresh this
    // conservative block summary; allocations publish their own epoch.
    if (h->collector.marking) n.block->live_epoch = h->collector.epoch;
    const auto& l = n.type()->layout;
    const uint64_t count = n.count();
    if (!count || (!l.nrefs && !l.ntraces)) return;
    auto* data = static_cast<uint8_t*>(payload(n)) + l.prefix_bytes;
    if (!l.nrefs && l.ntraces == 1 && l.traces[0].kind == OGC_TRACE_STRONG) {
        auto* address = data + l.traces[0].offset;
        for (uint64_t i = 0; i < count; ++i)
            trace_strong(h, address + i * l.size, visit, user);
        return;
    }
    for (uint64_t i = 0; i < count; ++i) {
        auto* base = data + i * l.size;
        for (uint32_t j = 0; j < l.nrefs; ++j) trace_strong(h, base + l.refs[j], visit, user);
        trace_program(l.traces, l.ntraces, [&](const auto& t) { return base + t.offset; }, [&](const auto& t) {
            trace_word(h, base + t.offset, t.kind, base + t.tag_offset, t.tag_values, visit, user);
        });
    }
}
void roots(OgcPortableHeap* h, bool young) {
    struct Trace { OgcPortableHeap* heap; bool young; } trace{h, young};
    h->services.roots(h, [](void* p, const OgcPolicyEdge* edge) {
        const auto& t = *static_cast<Trace*>(p); t.heap->algorithm->shade(&t.heap->collector, edge->target, t.young);
    }, &trace);
}
void bind_services(OgcPortableHeap* h) {
    h->services = {OGC_POLICY_ABI, sizeof(OgcPolicyServices), h,
        [](void* p, OgcPolicyVisit visit, void* user) {
            auto* heap = static_cast<OgcPortableHeap*>(p);
            for (auto* r = heap->roots; r; r = r->next) {
                if (!r->typed) for (size_t i = 0; i < r->count; ++i)
                    trace_strong(heap, &r->slots[i], visit, user);
                else trace_program(r->typed, r->count, [](const auto& t) { return t.address; }, [&](const auto& t) {
                    trace_word(heap, t.address, t.kind, t.tag_address, t.tag_values, visit, user);
                });
            }
            for (auto* frame = heap->frames; frame; frame = frame->next)
                trace_program(frame->slots, frame->count, [](const auto& t) { return t.address; }, [&](const auto& t) {
                    trace_word(heap, t.address, t.kind, t.tag_address, t.tag_values, visit, user);
                });
            for (auto* t = heap->handles; t; t = t->next) trace_strong(heap, &t->value, visit, user);
            for (auto* t = heap->pins; t; t = t->next) {
                const OgcPolicyEdge edge{&record(object(heap, t->value))->gc, &t->value, 0, OGC_POLICY_PINNED}; visit(user, &edge);
            }
        },
        [](void* p, OgcPolicyRecord* r, OgcPolicyVisit visit, void* user) { visit_edges(static_cast<OgcPortableHeap*>(p), view(r), visit, user); },
        [](void* p, OgcPolicyVisitRecord visit, void* user) { visit_young(static_cast<OgcPortableHeap*>(p), visit, user); },
        [](void* p, uint32_t young) {
            auto* heap = static_cast<OgcPortableHeap*>(p);
            retire_dead(heap);
            for (auto** link = &heap->weak; *link;) {
                auto* token = *link;
                if (token->owner && !object(heap, token->owner)) {
                    *link = token->next;
                    std::free(token);
                } else {
                    if (token->value && !object(heap, token->value)) token->value = nullptr;
                    link = &token->next;
                }
            }
            heap->statistics.live_bytes = heap->collector.old_charge; ++heap->statistics.collections;
            if (young) ++heap->statistics.minors;
            heap->since = 0;
            if (!young) {
                heap->allocated_at_full = heap->statistics.allocated;
                heap->live_at_full = heap->collector.old_charge;
                heap->tracking = true;
                heap->hook_callback = true;
                for (auto* hook = heap->hooks; hook; hook = hook->next) hook->fn(hook->user);
                heap->hook_callback = false;
            }
        }, nullptr};
}
void sync_barrier(OgcPortableHeap* h) { h->barrier = uint8_t(h->collector.marking || h->tracking); }
void full(OgcPortableHeap* h) { h->algorithm->collect(&h->collector, &h->services); sync_barrier(h); }
void young_collection(OgcPortableHeap* h) {
    if (!h->tracking) { full(h); h->tracking = true; sync_barrier(h); return; }
    h->algorithm->minor(&h->collector, &h->services); sync_barrier(h);
}
bool incremental(OgcPortableHeap* h) {
    int finished = 0; h->algorithm->step(&h->collector, &h->services, 0, &finished); sync_barrier(h); return finished;
}
void scan(OgcPortableHeap* h, Object n, bool = false) {
    visit_edges(h, n, [](void* p, const OgcPolicyEdge* e) {
        auto* heap = static_cast<OgcPortableHeap*>(p); heap->algorithm->shade(&heap->collector, e->target, 0);
    }, h);
}
void before_alloc(OgcPortableHeap* h) {
    if (no_gc || !h->policy.automatic) return;
    if (h->policy.stress == 1) { full(h); return; }
    if (h->policy.stress == 2) {
        if (h->collector.marking || ++h->stress_count % 16 == 0) incremental(h);
        else young_collection(h);
    } else if (h->since >= (8u << 20) && h->since >= h->collector.old_charge) {
        // Collect once allocation since the last cycle reaches the surviving bytes: a fixed budget
        // re-traces a large live set every 8 MiB, which makes total cost quadratic in that set.
        full(h);
    }
    for (uint32_t i = 0; i < h->policy.steps; ++i) incremental(h);
}
uint64_t nursery_budget(const OgcPortableHeap* h) {
    const uint64_t limit = h->storage.limit;
    if (!limit || limit / 4 >= h->nursery_target) return h->nursery_target;
    return limit >= 4 ? limit / 4 : 1;
}
void request_poll(OgcPortableHeap* h) {
    h->poll = !no_gc && h->policy.automatic &&
        (h->policy.stress || h->policy.steps || h->since >= nursery_budget(h));
}
void store(OgcPortableHeap* h, Object owner, OgcRef* word, OgcRef value) {
    if (h->collector.marking) shade(h, *word);
    remember(h, owner);
    *word = value;
    // Root slots may introduce a new root during an incremental cycle.
    if (!owner && h->collector.marking) shade(h, value);
}
int create(const OgcPortableConfig* c, OgcPortableHeap** out) {
    if (!c || !out || c->abi != OGC_PORTABLE_ABI || c->size != sizeof(*c) ||
        c->model != OGC_PORTABLE_MODEL || c->reserved) return OGC_PORTABLE_INVALID;
    if (c->required_capabilities & ~capabilities) return OGC_PORTABLE_UNSUPPORTED;
    auto* h = new (std::nothrow) OgcPortableHeap;
    if (!h) return OGC_PORTABLE_OOM;
    if (ogc_policy_query(OGC_POLICY_ABI, sizeof(OgcPolicy), &h->algorithm) ||
        (h->algorithm->capabilities & OGC_POLICY_MOVING) ||
        bool(h->algorithm->capabilities & OGC_POLICY_COLLECTS) == no_gc) { delete h; return OGC_PORTABLE_UNSUPPORTED; }
    h->algorithm->init(&h->collector); bind_services(h);
    h->storage.limit = c->max_bytes; *out = h; return OGC_PORTABLE_OK;
}
void free_type(OgcPortableType* t) {
    std::free(const_cast<uint32_t*>(t->layout.refs)); std::free(const_cast<OgcPortableTrace*>(t->layout.traces));
    std::free(const_cast<char*>(t->layout.name)); std::free(t);
}
void release(OgcPortableHeap* h, Object n) {
    const auto& l = n.type()->layout;
    if (l.release) l.release(l.user, payload(n));
    release_storage(h, n);
}
int destroy(OgcPortableHeap* h) {
    const int s = check(h); if (s) return s;
    flush_allocation_run(h);
    if (h->roots || h->frames || h->handles || h->pins || h->windows || h->hooks) return OGC_PORTABLE_BUSY;
    for (auto* token = h->weak; token; token = token->next)
        if (!token->owner) return OGC_PORTABLE_BUSY;
    h->busy = true;
    while (h->weak) {
        auto* token = h->weak;
        h->weak = token->next;
        std::free(token);
    }
    visit_resources(h, [](OgcPortableHeap*, Object o) {
        if (o.type()->layout.release) o.type()->layout.release(o.type()->layout.user, payload(o));
    });
    destroy_storage(h);
    while (h->types) { auto* t = h->types; h->types = t->next; free_type(t); }
    while (h->root_pool) { auto* r = h->root_pool; h->root_pool = r->next; std::free(r); }
    delete h; return OGC_PORTABLE_OK;
}
int type_register(OgcPortableHeap* h, const OgcPortableLayout* l, const OgcPortableType** out) {
    const int s = check(h); if (s) return s;
    if (!l || !out || l->struct_size != sizeof(*l) || l->reserved || (l->align != 8 && l->align != 16) || !l->size || (l->nrefs && l->size % 8) ||
        l->is_array > 1 || (l->nrefs && !l->refs) || l->nrefs > l->size / 8) return OGC_PORTABLE_INVALID;
    for (uint32_t i = 0; i < l->nrefs; ++i)
        if (l->refs[i] % 8 || l->refs[i] > l->size - 8 || (i && l->refs[i] <= l->refs[i - 1])) return OGC_PORTABLE_INVALID;
    if ((l->ntraces && !l->traces) || (!l->is_array && l->prefix_bytes)) return OGC_PORTABLE_INVALID;
    uint32_t depth = 0;
    for (uint32_t i = 0; i < l->ntraces; ++i) {
        const auto& t = l->traces[i];
        if (t.reserved || t.kind > OGC_TRACE_CODE) return OGC_PORTABLE_INVALID;
        if (t.kind == OGC_TRACE_END) { if (!depth) return OGC_PORTABLE_INVALID; --depth; continue; }
        const uint32_t width = t.kind == OGC_TRACE_IF_U8 ? 1 : 8;
        if (t.offset > l->size || l->size - t.offset < width) return OGC_PORTABLE_INVALID;
        if (t.kind == OGC_TRACE_IF_U8 || t.kind == OGC_TRACE_IF_U64) ++depth;
        if (t.kind == OGC_TRACE_TAGGED && (t.tag_offset > l->size || l->size - t.tag_offset < 8)) return OGC_PORTABLE_INVALID;
    }
    if (depth) return OGC_PORTABLE_INVALID;
    auto* t = static_cast<OgcPortableType*>(std::calloc(1, sizeof(OgcPortableType)));
    if (!t) return OGC_PORTABLE_OOM;
    t->layout = *l; t->owner = h;
    const char* name = l->name ? l->name : "";
    auto* copied = static_cast<char*>(std::malloc(std::strlen(name) + 1));
    auto* refs = l->nrefs ? static_cast<uint32_t*>(std::malloc(size_t(l->nrefs) * 4)) : nullptr;
    auto* traces = l->ntraces ? static_cast<OgcPortableTrace*>(std::malloc(size_t(l->ntraces) * sizeof(OgcPortableTrace))) : nullptr;
    t->layout.name = copied; t->layout.refs = refs; t->layout.traces = traces;
    if (!copied || (l->nrefs && !refs) || (l->ntraces && !traces)) { free_type(t); return OGC_PORTABLE_OOM; }
    std::memcpy(copied, name, std::strlen(name) + 1);
    if (l->nrefs) std::memcpy(refs, l->refs, size_t(l->nrefs) * 4);
    if (l->ntraces) std::memcpy(traces, l->traces, size_t(l->ntraces) * sizeof(OgcPortableTrace));
    t->next = h->types; h->types = t; *out = t; return OGC_PORTABLE_OK;
}
template<bool deferred>
int allocate(OgcPortableHeap* h, const OgcPortableType* t, uint64_t count, OgcRef* out) {
    const int s = check(h); if (s) return s;
    if (h->windows) return OGC_PORTABLE_BUSY;
    if (!out || !local_type(h, t)) return OGC_PORTABLE_INVALID;
    if (!t->layout.is_array) count = 1;
    if (count > (SIZE_MAX - 15 - t->layout.prefix_bytes) / t->layout.size || h->storage.next_identity == UINT64_MAX) return OGC_PORTABLE_OOM;
    const uint64_t bytes = t->layout.prefix_bytes + count * t->layout.size;
    flush_allocation_run(h);
    if constexpr (!deferred && !no_gc) {
        h->busy = true; before_alloc(h); h->busy = false;
    }
    auto n = register_allocation(h, t, count, bytes);
    if (!n) return OGC_PORTABLE_OOM;
    if constexpr (deferred && !no_gc) if (!h->poll) request_poll(h);
    *out = payload(n); return OGC_PORTABLE_OK;
}
int refill_run(OgcPortableHeap* h, const OgcPortableType* t, const void* key, OgcRef* out) {
    const int s = check(h); if (s) return s;
    if (h->windows) return OGC_PORTABLE_BUSY;
    if (!out || !key || !local_type(h, t) || t->layout.is_array || t->layout.release ||
        t->layout.prefix_bytes || t->layout.size > 256) return OGC_PORTABLE_INVALID;
    const auto n = refill_allocation_run(h, t, key);
    if (!n) return OGC_PORTABLE_OOM;
    if constexpr (!no_gc) if (!h->poll) request_poll(h);
    *out = payload(n);
    return OGC_PORTABLE_OK;
}
int type_of(OgcPortableHeap* h, OgcRef ref, const OgcPortableType** out) {
    const int s = check(h); if (s) return s;
    auto n = object(h, ref); if (!out || !n) return OGC_PORTABLE_INVALID;
    *out = n.type(); return OGC_PORTABLE_OK;
}
int identity(OgcPortableHeap* h, OgcRef ref, uint64_t* out) {
    const int s = check(h); if (s) return s;
    auto n = object(h, ref); if (!out || !n) return OGC_PORTABLE_INVALID;
    *out = n.identity(); return OGC_PORTABLE_OK;
}
int load_ref(OgcPortableHeap* h, OgcRef ref, uint64_t offset, OgcRef* out) {
    const int s = check(h); if (s) return s;
    if (!out || !ref_slot(object(h, ref), offset)) return OGC_PORTABLE_INVALID;
    *out = *slot(ref, offset); return OGC_PORTABLE_OK;
}
int store_ref(OgcPortableHeap* h, OgcRef ref, uint64_t offset, OgcRef value) {
    const int s = check(h); if (s) return s;
    auto n = object(h, ref);
    if (!ref_slot(n, offset) || !local_ref(h, value)) return OGC_PORTABLE_INVALID;
    store(h, n, slot(ref, offset), value); return OGC_PORTABLE_OK;
}
int copy_refs(OgcPortableHeap* h, OgcRef dst, uint64_t dst_offset, OgcRef src, uint64_t src_offset, uint64_t count) {
    const int s = check(h); if (s) return s;
    auto d = object(h, dst); const auto n = object(h, src);
    if (!d || !n || dst_offset > d.bytes() || src_offset > n.bytes() ||
        count > (d.bytes() - dst_offset) / 8 || count > (n.bytes() - src_offset) / 8) return OGC_PORTABLE_INVALID;
    for (uint64_t i = 0; i < count; ++i)
        if (!ref_slot(d, dst_offset + i * 8) || !ref_slot(n, src_offset + i * 8)) return OGC_PORTABLE_INVALID;
    // Shade every overwritten edge before memmove; no temporary roots or safepoint.
    if (h->collector.marking) for (uint64_t i = 0; i < count; ++i) shade(h, *slot(dst, dst_offset + i * 8));
    if (count) std::memmove(slot(dst, dst_offset), slot(src, src_offset), size_t(count) * 8);
    remember(h, d); return OGC_PORTABLE_OK;
}
OgcPortableRoots* root_take(OgcPortableHeap* h) {
    if (auto* r = h->root_pool) { h->root_pool = r->next; return r; }
    return static_cast<OgcPortableRoots*>(std::malloc(sizeof(OgcPortableRoots)));
}
void root_give(OgcPortableHeap* h, OgcPortableRoots* r) { r->next = h->root_pool; h->root_pool = r; }
int roots_enter(OgcPortableHeap* h, OgcRef* slots, size_t count, OgcPortableRoots** out) {
    const int s = check(h); if (s) return s;
    if (!out || (count && (!slots || uintptr_t(slots) % 8)) || count > SIZE_MAX / 8) return OGC_PORTABLE_INVALID;
    for (size_t i = 0; i < count; ++i) if (!local_ref(h, slots[i])) return OGC_PORTABLE_INVALID;
    auto* r = root_take(h);
    if (!r) return OGC_PORTABLE_OOM;
    *r = {slots, count, nullptr, h->roots}; h->roots = r;
    if (h->collector.marking) for (size_t i = 0; i < count; ++i) shade(h, slots[i]);
    *out = r; return OGC_PORTABLE_OK;
}
int roots_leave(OgcPortableHeap* h, OgcPortableRoots* r) {
    const int s = check(h); if (s) return s;
    if (!r || h->roots != r) return OGC_PORTABLE_INVALID;
    h->roots = r->next; root_give(h, r); return OGC_PORTABLE_OK;
}
template<class T> bool member(T* list, T* token) {
    for (auto* n = list; n; n = n->next) if (n == token) return true;
    return false;
}
template<class T> int token_new(OgcPortableHeap* h, T*& list, OgcRef ref, T** out, bool root = true) {
    if (!out || !local_ref(h, ref)) return OGC_PORTABLE_INVALID;
    auto* t = static_cast<T*>(std::malloc(sizeof(T)));
    if (!t) return OGC_PORTABLE_OOM;
    *t = {ref, list}; list = t;
    if (root && h->collector.marking) shade(h, ref);
    *out = t; return OGC_PORTABLE_OK;
}
template<class T> int token_free(T*& list, T* token) {
    auto** at = &list; while (*at && *at != token) at = &(*at)->next;
    if (!*at) return OGC_PORTABLE_INVALID;
    *at = token->next; std::free(token); return OGC_PORTABLE_OK;
}
int handle_new(OgcPortableHeap* h, OgcRef ref, OgcPortableHandle** out) {
    const int s = check(h); if (s) return s; return token_new(h, h->handles, ref, out);
}
int handle_get(OgcPortableHeap* h, OgcPortableHandle* t, OgcRef* out) {
    const int s = check(h); if (s) return s;
    if (!out || !member(h->handles, t)) return OGC_PORTABLE_INVALID;
    *out = t->value; return OGC_PORTABLE_OK;
}
int handle_set(OgcPortableHeap* h, OgcPortableHandle* t, OgcRef ref) {
    const int s = check(h); if (s) return s;
    if (!member(h->handles, t) || !local_ref(h, ref)) return OGC_PORTABLE_INVALID;
    store(h, {}, &t->value, ref); return OGC_PORTABLE_OK;
}
int handle_free(OgcPortableHeap* h, OgcPortableHandle* t) {
    const int s = check(h); if (s) return s; return token_free(h->handles, t);
}
int pin(OgcPortableHeap* h, OgcRef ref, OgcPortablePin** out, void** address) {
    const int s = check(h); if (s) return s;
    if (!address || !ref) return OGC_PORTABLE_INVALID;
    const int result = token_new(h, h->pins, ref, out); if (!result) *address = ref; return result;
}
int unpin(OgcPortableHeap* h, OgcPortablePin* t) {
    const int s = check(h); if (s) return s; return token_free(h->pins, t);
}
int safepoint(OgcPortableHeap* h) {
    const int s = check(h); if (s) return s;
    if (!h->poll || h->windows) return OGC_PORTABLE_OK;
    flush_allocation_run(h);
    h->busy = true;
    if (h->policy.stress) before_alloc(h);
    else {
        const uint64_t budget = nursery_budget(h);
        if (h->since >= budget) {
            const uint64_t since_full = h->statistics.allocated - h->allocated_at_full;
            constexpr uint64_t min_target = 8u << 20, max_target = 64u << 20;
            const uint64_t major_interval = h->storage.limit ? 2 * budget :
                (h->nursery_target > 2 * min_target ? 2 * h->nursery_target : 4 * min_target);
            if (h->collector.marking ||
                (since_full >= major_interval && (h->storage.limit || since_full >= h->live_at_full))) full(h);
            else {
                const uint64_t before = h->collector.old_charge;
                const uint64_t charged = h->since;
                const bool was_tracking = h->tracking;
                young_collection(h);
                if (was_tracking) {
                    const uint64_t promoted = h->collector.old_charge - before;
                    // Frequent promotion means the next nursery has room to
                    // absorb a larger burst without revisiting its live prefix.
                    if (promoted >= charged / 8 && h->nursery_target < max_target) h->nursery_target *= 2;
                    else if (promoted < charged / 32 && h->nursery_target > min_target) h->nursery_target /= 2;
                }
            }
        }
        for (uint32_t i = 0; i < h->policy.steps; ++i) incremental(h);
    }
    h->poll = 0;
    h->busy = false;
    return OGC_PORTABLE_OK;
}
int collect(OgcPortableHeap* h) {
    const int s = check(h); if (s) return s;
    if (h->windows) return OGC_PORTABLE_BUSY;
    if (no_gc) return OGC_PORTABLE_UNSUPPORTED;
    flush_allocation_run(h);
    h->busy = true; full(h); h->busy = false; return OGC_PORTABLE_OK;
}
int minor(OgcPortableHeap* h) {
    const int s = check(h); if (s) return s;
    if (h->windows) return OGC_PORTABLE_BUSY;
    if (no_gc) return OGC_PORTABLE_UNSUPPORTED;
    flush_allocation_run(h);
    h->busy = true; young_collection(h); h->busy = false; return OGC_PORTABLE_OK;
}
int step(OgcPortableHeap* h, uint64_t budget, int* finished) {
    const int s = check(h); if (s) return s;
    if (!finished || budget > UINT32_MAX) return OGC_PORTABLE_INVALID;
    if (h->windows) return OGC_PORTABLE_BUSY;
    if (no_gc) return OGC_PORTABLE_UNSUPPORTED;
    flush_allocation_run(h);
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(budget);
    h->busy = true;
    bool done;
    do { done = incremental(h); } while (!done && std::chrono::steady_clock::now() < end);
    h->busy = false; *finished = done; return OGC_PORTABLE_OK;
}
int finalize(OgcPortableHeap* h, uint64_t budget, uint64_t* released) {
    const int s = check(h); if (s) return s;
    if (!released || budget > UINT32_MAX) return OGC_PORTABLE_INVALID;
    flush_allocation_run(h);
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(budget);
    h->busy = true; uint64_t n = 0;
    do {
        auto o = take_pending(h); if (!o) break;
        release(h, o); ++n;
    } while (std::chrono::steady_clock::now() < end);
    h->busy = false; *released = n; return OGC_PORTABLE_OK;
}
int stats(OgcPortableHeap* h, OgcPortableStats* out) {
    const int s = check(h); if (s) return s;
    if (!out) return OGC_PORTABLE_INVALID;
    flush_allocation_run(h);
    auto st = h->statistics; st.heap_bytes = h->storage.mapped; *out = st; return OGC_PORTABLE_OK;
}
int extent(OgcPortableHeap* h, OgcRef ref, uint64_t* bytes, uint64_t* count) {
    const int s = check(h); if (s) return s;
    auto n = object(h, ref); if (!n || !bytes || !count) return OGC_PORTABLE_INVALID;
    *bytes = n.bytes(); *count = n.count(); return OGC_PORTABLE_OK;
}
int walk(OgcPortableHeap* h, const OgcPortableWalk* w) {
    if (!h || !w || (w->nrefs && !w->refs) || (w->nwords && !w->words) || w->reserved || w->registered_roots > 7)
        return OGC_PORTABLE_INVALID;
    if (h->owner != std::this_thread::get_id()) return OGC_PORTABLE_WRONG_OWNER;
    if (h->walking || (h->busy && !h->hook_callback)) return OGC_PORTABLE_BUSY;
    flush_allocation_run(h);
    for (size_t i = 0; i < w->nrefs; ++i) if (!local_ref(h, w->refs[i])) return OGC_PORTABLE_INVALID;
    const bool was_busy = h->busy; h->busy = h->walking = true;
    ++h->walk_epoch;
    OgcRef queue = nullptr;
    auto add = [&](Object n) { if (n && mark_walk(n, h->walk_epoch)) { set_walk_next(n, queue); queue = payload(n); } };
    auto word = [&](uintptr_t value) { if (w->word) w->word(w->user, value); add(containing(h, value)); };
    for (size_t i = 0; i < w->nrefs; ++i) add(object(h, w->refs[i]));
    for (size_t i = 0; i < w->nwords; ++i) word(w->words[i]);
    if (w->registered_roots) {
        if (w->registered_roots & 1) for (auto* r = h->roots; r; r = r->next) {
            if (!r->typed) for (size_t i = 0; i < r->count; ++i) add(object(h, r->slots[i]));
            else trace_program(r->typed, r->count, [](const auto& t) { return t.address; }, [&](const auto& t) {
                uint64_t tag = 0; if (t.kind == OGC_TRACE_TAGGED) std::memcpy(&tag, t.tag_address, 8);
                if (t.kind != OGC_TRACE_TAGGED || (tag < 64 && (t.tag_values & (UINT64_C(1) << tag)))) {
                    uintptr_t value; std::memcpy(&value, t.address, 8);
                    if (w->word) w->word(w->user, value);
                    if (t.kind != OGC_TRACE_CODE) add(containing(h, value));
                }
            });
        }
        if (w->registered_roots & 5) for (auto* frame = h->frames; frame; frame = frame->next) {
            if (w->word) w->word(w->user, frame->pc);
            trace_program(frame->slots, frame->count, [](const auto& t) { return t.address; }, [&](const auto& t) {
                uint64_t tag = 0;
                if (t.kind == OGC_TRACE_TAGGED) std::memcpy(&tag, t.tag_address, 8);
                if (t.kind == OGC_TRACE_TAGGED && (tag >= 64 || !(t.tag_values & (UINT64_C(1) << tag)))) return;
                uintptr_t value;
                std::memcpy(&value, t.address, 8);
                if (w->word) w->word(w->user, value);
                if (t.kind != OGC_TRACE_CODE) add(containing(h, value));
            });
        }
        if (w->registered_roots & 3) {
            for (auto* t = h->handles; t; t = t->next) add(object(h, t->value));
            for (auto* t = h->pins; t; t = t->next) add(object(h, t->value));
        }
    }
    while (queue) {
        auto n = object(h, queue); queue = walk_next(n);
        if (w->object) w->object(w->user, payload(n), n.type());
        const auto& l = n.type()->layout;
        for (uint64_t i = 0; i < n.count(); ++i)
            for (uint32_t j = 0; j < l.nrefs; ++j) add(object(h, *slot(payload(n), l.prefix_bytes + i * l.size + l.refs[j])));
        for (uint64_t i = 0; i < n.count(); ++i) {
            const auto* base = static_cast<const uint8_t*>(payload(n)) + l.prefix_bytes + i * l.size;
            trace_program(l.traces, l.ntraces, [&](const auto& t) { return base + t.offset; }, [&](const auto& t) {
                uint64_t tag = 0; if (t.kind == OGC_TRACE_TAGGED) std::memcpy(&tag, base + t.tag_offset, 8);
                if (t.kind != OGC_TRACE_TAGGED || (tag < 64 && (t.tag_values & (UINT64_C(1) << tag)))) {
                    uintptr_t value; std::memcpy(&value, base + t.offset, 8);
                    if (w->word) w->word(w->user, value);
                    if (t.kind != OGC_TRACE_CODE) add(containing(h, value));
                }
            });
        }

    }
    h->walking = false; h->busy = was_busy; return OGC_PORTABLE_OK;
}
int weak_new(OgcPortableHeap* h, OgcRef ref, OgcPortableWeak** out) {
    const int s = check(h); if (s) return s; return token_new(h, h->weak, ref, out, false);
}
int weak_new_owned(OgcPortableHeap* h, OgcRef owner, OgcRef ref, OgcPortableWeak** out) {
    const int status = check(h);
    if (status) return status;
    if (!object(h, owner)) return OGC_PORTABLE_INVALID;
    const int created = token_new(h, h->weak, ref, out, false);
    if (!created) (*out)->owner = owner;
    return created;
}
int weak_get(OgcPortableHeap* h, OgcPortableWeak* w, OgcRef* out) {
    const int s = check(h); if (s) return s;
    if (!out || !member(h->weak, w)) return OGC_PORTABLE_INVALID;
    *out = w->value; return OGC_PORTABLE_OK;
}
int weak_free(OgcPortableHeap* h, OgcPortableWeak* w) {
    const int s = check(h); if (s) return s; return token_free(h->weak, w);
}
int no_gc_enter(OgcPortableHeap* h, OgcPortableNoGC** out) {
    const int s = check(h); if (s) return s;
    if (!out) return OGC_PORTABLE_INVALID;
    auto* t = static_cast<OgcPortableNoGC*>(std::malloc(sizeof(OgcPortableNoGC)));
    if (!t) return OGC_PORTABLE_OOM;
    flush_allocation_run(h);
    t->next = h->windows; h->windows = t; *out = t; return OGC_PORTABLE_OK;
}
int no_gc_leave(OgcPortableHeap* h, OgcPortableNoGC* t) {
    const int s = check(h); if (s) return s;
    if (!t || t != h->windows) return OGC_PORTABLE_INVALID;
    h->windows = t->next; std::free(t); return OGC_PORTABLE_OK;
}
int hook_add(OgcPortableHeap* h, void (*fn)(void*), void* user, OgcPortableHook** out) {
    const int s = check(h); if (s) return s;
    if (!fn || !out) return OGC_PORTABLE_INVALID;
    auto* t = static_cast<OgcPortableHook*>(std::malloc(sizeof(OgcPortableHook)));
    if (!t) return OGC_PORTABLE_OOM;
    *t = {fn, user, h->hooks}; h->hooks = t; *out = t; return OGC_PORTABLE_OK;
}
int hook_remove(OgcPortableHeap* h, OgcPortableHook* hook) {
    const int s = check(h); if (s) return s; return token_free(h->hooks, hook);
}
const uint8_t* barrier_flag(OgcPortableHeap* h) { return h ? &h->barrier : nullptr; }
const uint8_t* poll_flag(OgcPortableHeap* h) { return h ? &h->poll : nullptr; }
OgcPortableAllocRun* allocation_run(OgcPortableHeap* h) { return h ? h->runs : nullptr; }
OgcPortableRootFrame** root_head(OgcPortableHeap* h) { return h ? &h->frames : nullptr; }
int set_policy(OgcPortableHeap* h, const OgcPortablePolicy* policy) {
    const int s = check(h); if (s) return s;
    if (!policy || policy->automatic > 1 || policy->stress > 2 || policy->reserved || policy->steps > 1000000)
        return OGC_PORTABLE_INVALID;
    flush_allocation_run(h);
    h->policy = *policy;
    h->poll = 0;
    return OGC_PORTABLE_OK;
}
int pending_releases(OgcPortableHeap* h, uint64_t* out) {
    const int s = check(h); if (s) return s;
    if (!out) return OGC_PORTABLE_INVALID;
    *out = h->storage.pending_count; return OGC_PORTABLE_OK;
}
int roots_enter_typed(OgcPortableHeap* h, const OgcPortableStackRoot* slots, size_t count, OgcPortableRoots** out) {
    const int s = check(h); if (s) return s;
    if (!out || (count && !slots)) return OGC_PORTABLE_INVALID;
    uint32_t depth = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& t = slots[i];
        if (t.reserved || t.kind > OGC_TRACE_CODE) return OGC_PORTABLE_INVALID;
        if (t.kind == OGC_TRACE_END) { if (!depth) return OGC_PORTABLE_INVALID; --depth; continue; }
        if (!t.address || (t.kind == OGC_TRACE_TAGGED && !t.tag_address)) return OGC_PORTABLE_INVALID;
        if (t.kind == OGC_TRACE_IF_U8 || t.kind == OGC_TRACE_IF_U64) ++depth;
    }
    if (depth) return OGC_PORTABLE_INVALID;
    auto* r = root_take(h);
    if (!r) return OGC_PORTABLE_OOM;
    *r = {nullptr, count, slots, h->roots}; h->roots = r;
    if (h->collector.marking) roots(h, false);
    *out = r; return OGC_PORTABLE_OK;
}
int type_layout(OgcPortableHeap* h, const OgcPortableType* t, const OgcPortableLayout** out) {
    if (!h || !out || !local_type(h, t)) return OGC_PORTABLE_INVALID;
    if (h->owner != std::this_thread::get_id()) return OGC_PORTABLE_WRONG_OWNER;
    *out = &t->layout; return OGC_PORTABLE_OK;
}
int read_word(OgcPortableHeap* h, const void* address, OgcRef* out) {
    const int s = check(h); if (s) return s;
    if (!address || !out) return OGC_PORTABLE_INVALID;
    std::memcpy(out, address, 8); return OGC_PORTABLE_OK;
}
int write_word(OgcPortableHeap* h, void* address, OgcRef value) {
    const int s = check(h); if (s) return s;
    if (!address) return OGC_PORTABLE_INVALID;
    auto owner = containing(h, uintptr_t(address));
    if (owner) {
        const uint64_t offset = uintptr_t(address) - uintptr_t(payload(owner));
        const auto kind = ref_slot_kind(owner, offset);
        if (kind == RefSlot::Invalid || (kind == RefSlot::Strong && !local_ref(h, value)))
            return OGC_PORTABLE_INVALID;
    }
    OgcRef previous; std::memcpy(&previous, address, 8);
    if (h->collector.marking) { if (auto n = containing(h, uintptr_t(previous))) shade(h, payload(n)); }
    remember(h, owner);
    std::memcpy(address, &value, 8); return OGC_PORTABLE_OK;
}
int copy_memory(OgcPortableHeap* h, void* dst, const void* src, size_t bytes) {
    const int s = check(h); if (s) return s;
    if (bytes && (!dst || !src)) return OGC_PORTABLE_INVALID;
    auto owner = containing(h, uintptr_t(dst));
    if (owner) {
        const uint64_t offset = uintptr_t(dst) - uintptr_t(payload(owner));
        if (bytes > owner.bytes() - offset) return OGC_PORTABLE_INVALID;
        if (h->collector.marking) scan(h, owner, false);
        remember(h, owner);
    }
    if (bytes) std::memmove(dst, src, bytes);
    return OGC_PORTABLE_OK;
}
int marking(OgcPortableHeap* h, int* out) {
    const int s = check(h); if (s) return s;
    if (!out) return OGC_PORTABLE_INVALID;
    *out = h->collector.marking; return OGC_PORTABLE_OK;
}
int roots_remove(OgcPortableHeap* h, OgcPortableRoots* token) {
    const int s = check(h);
    if (s && !(s == OGC_PORTABLE_BUSY && h->hook_callback && !h->walking)) return s;
    auto** at = &h->roots; while (*at && *at != token) at = &(*at)->next;
    if (!*at) return OGC_PORTABLE_INVALID;
    *at = token->next; root_give(h, token); return OGC_PORTABLE_OK;
}
const OgcPortableApi api{
    OGC_PORTABLE_ABI, sizeof(OgcPortableApi), OGC_PORTABLE_MODEL, 0,
    no_gc ? "bump" : "marksweep", capabilities, create, destroy, type_register, allocate<false>,
    type_of, identity, load_ref, store_ref, copy_refs, roots_enter, roots_leave,
    handle_new, handle_get, handle_set, handle_free, pin, unpin, safepoint,
    collect, minor, step, finalize, stats, extent, walk, weak_new, weak_get, weak_free,
    no_gc_enter, no_gc_leave, hook_add, hook_remove, set_policy, pending_releases,
    roots_enter_typed, type_layout, read_word, write_word, copy_memory, marking, roots_remove, barrier_flag,
    allocate<true>, poll_flag, allocation_run, refill_run, root_head, weak_new_owned
};
}
extern "C" int ogc_portable_query(uint32_t abi, uint32_t model, uint32_t min_size, const OgcPortableApi** out) {
    if (!out || abi != OGC_PORTABLE_ABI || model != OGC_PORTABLE_MODEL || min_size > sizeof(ogc_preset::api)) return OGC_PORTABLE_INVALID;
    *out = &ogc_preset::api; return OGC_PORTABLE_OK;
}
