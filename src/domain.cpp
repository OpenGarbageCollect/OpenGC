/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 OpenGC contributors */
#include <opengc/opengc.h>
#include "memory.h"
#include <atomic>
#include <new>
#include <thread>

namespace {
struct Object {
    Object* next;
    OgcObjectInfo info;
    OgcAllocation storage;
    OgcAllocation metadata;
    bool published;
    uint64_t* offsets() { return reinterpret_cast<uint64_t*>(this + 1); }
};
struct Root {
    Root* next;
    OgcId id;
    OgcEdge edge;
    OgcAllocation metadata;
};
std::atomic<uint64_t> next_domain{1};

OgcStatus issue_domain(uint64_t& result) {
    auto current = next_domain.load(std::memory_order_relaxed);
    for (;;) {
        if (current == UINT64_MAX) return OGC_ID_EXHAUSTED;
        if (next_domain.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
            result = current;
            return OGC_OK;
        }
    }
}
}

struct OgcDomain {
    OgcDomainConfig config;
    OgcProvider provider;
    OgcAllocation self;
    std::thread::id owner;
    OgcStats stats{};
    uint64_t next_serial = 1;
    Object* objects = nullptr;
    Root* roots = nullptr;
    bool busy = false;

    OgcStatus admission() const {
        if (owner != std::this_thread::get_id()) return OGC_WRONG_OWNER;
        return busy ? OGC_BUSY : OGC_OK;
    }
    Object* find(OgcId id) const {
        if (id.domain != stats.domain_id || !id.serial) return nullptr;
        for (auto* object = objects; object; object = object->next)
            if (opengc::same_id(object->info.id, id)) return object;
        return nullptr;
    }
    Object* inspect(uintptr_t address, uint32_t kind, bool published_only) const {
        if (!address) return nullptr;
        for (auto* object = objects; object; object = object->next) {
            if (published_only && !object->published) continue;
            const auto base = reinterpret_cast<uintptr_t>(object->info.base);
            if (address == base || (kind == OGC_EDGE_INTERIOR && address > base &&
                address - base < object->info.bytes)) return object;
        }
        return nullptr;
    }
    Root* find_root(OgcId id) const {
        if (id.domain != stats.domain_id || !id.serial) return nullptr;
        for (auto* root = roots; root; root = root->next)
            if (opengc::same_id(root->id, id)) return root;
        return nullptr;
    }
    OgcStatus validate_edge(OgcEdge edge) const {
        if (edge.reserved || edge.kind > OGC_EDGE_INTERIOR) return OGC_INVALID_ARGUMENT;
        if (!edge.address || edge.kind == OGC_EDGE_INTERIOR) return OGC_OK;
        return inspect(edge.address, OGC_EDGE_EXACT, true) ? OGC_OK : OGC_INVALID_EDGE;
    }
    void release_object(Object* object) {
        const auto storage = object->storage;
        const auto metadata = object->metadata;
        if (object->published) --stats.published_objects;
        else --stats.constructing_objects;
        opengc::deallocate(config.data_allocator, storage, stats.data_bytes);
        opengc::deallocate(config.metadata_allocator, metadata, stats.metadata_bytes);
    }
};

namespace {
struct Guard {
    OgcDomain& domain;
    explicit Guard(OgcDomain& value) : domain(value) { domain.busy = true; }
    ~Guard() { domain.busy = false; }
};
OgcStatus check(OgcDomain* domain) { return domain ? domain->admission() : OGC_INVALID_ARGUMENT; }

OgcStatus OGC_CALL begin(void* context, uint64_t* count) {
    auto& domain = *static_cast<OgcDomain*>(context);
    auto status = domain.admission();
    if (status != OGC_OK) return status;
    if (domain.stats.constructing_objects) return OGC_BUSY;
    domain.busy = true;
    *count = domain.stats.published_objects;
    return OGC_OK;
}
void OGC_CALL end(void* context) { static_cast<OgcDomain*>(context)->busy = false; }
OgcStatus OGC_CALL object_next(void* context, uint64_t* cursor, OgcObjectInfo* info, uint32_t* has) {
    auto& domain = *static_cast<OgcDomain*>(context);
    // UINT64_MAX is end; 0 starts the host-issued opaque traversal.
    auto* object = *cursor == 0 ? domain.objects :
        (*cursor == UINT64_MAX ? nullptr : reinterpret_cast<Object*>(static_cast<uintptr_t>(*cursor)));
    if (!object) { *has = 0; *cursor = UINT64_MAX; return OGC_OK; }
    *info = object->info;
    *cursor = object->next ? reinterpret_cast<uintptr_t>(object->next) : UINT64_MAX;
    *has = 1;
    return OGC_OK;
}
OgcStatus OGC_CALL root_next(void* context, uint64_t* cursor, OgcEdge* edge, uint32_t* has) {
    auto& domain = *static_cast<OgcDomain*>(context);
    auto* root = *cursor == 0 ? domain.roots :
        (*cursor == UINT64_MAX ? nullptr : reinterpret_cast<Root*>(static_cast<uintptr_t>(*cursor)));
    if (!root) { *has = 0; *cursor = UINT64_MAX; return OGC_OK; }
    *edge = root->edge;
    *cursor = root->next ? reinterpret_cast<uintptr_t>(root->next) : UINT64_MAX;
    *has = 1;
    return OGC_OK;
}
OgcStatus OGC_CALL trace_edge(void* context, OgcId id, uint64_t index, OgcEdge* edge) {
    auto* object = static_cast<OgcDomain*>(context)->find(id);
    if (!object || !object->published) return OGC_STALE_HANDLE;
    if (index >= object->info.reference_count) return OGC_INVALID_ARGUMENT;
    void* target = nullptr;
    std::memcpy(&target, static_cast<unsigned char*>(object->info.base) + object->offsets()[index], sizeof(target));
    *edge = {reinterpret_cast<uintptr_t>(target), OGC_EDGE_EXACT, 0};
    return OGC_OK;
}
OgcStatus OGC_CALL retire(void* context, OgcId id) {
    auto& domain = *static_cast<OgcDomain*>(context);
    auto** link = &domain.objects;
    while (*link && !opengc::same_id((*link)->info.id, id)) link = &(*link)->next;
    if (!*link || !(*link)->published) return OGC_STALE_HANDLE;
    auto* object = *link;
    *link = object->next;
    domain.release_object(object);
    return OGC_OK;
}
}

extern "C" {
OgcStatus OGC_CALL ogc_domain_create(const OgcDomainConfig* config, OgcDomain** out) {
    if (!config || !out || *out) return OGC_INVALID_ARGUMENT;
    if (config->struct_size < sizeof(OgcDomainConfig) || config->abi_major != OGC_ABI_MAJOR)
        return OGC_INCOMPATIBLE_ABI;
    const OgcQuery query{sizeof(OgcQuery), OGC_ABI_MAJOR, 0, 8, OGC_CAP_STABLE_ADDRESSES};
    auto status = ogc_validate_provider(&query, config->provider);
    if (status != OGC_OK) return status;
    status = ogc_validate_allocator(&config->data_allocator);
    if (status != OGC_OK) return status;
    status = ogc_validate_allocator(&config->metadata_allocator);
    if (status != OGC_OK) return status;
    uint64_t domain_id = 0;
    status = issue_domain(domain_id);
    if (status != OGC_OK) return status;
    OgcAllocation self{};
    uint64_t metadata_bytes = 0;
    status = opengc::allocate(config->metadata_allocator, sizeof(OgcDomain), alignof(OgcDomain),
                              config->metadata_limit, metadata_bytes, self);
    if (status != OGC_OK) return status;
    auto* domain = new (self.base) OgcDomain;
    domain->config = *config;
    domain->provider = *config->provider;
    domain->config.provider = &domain->provider;
    domain->self = self;
    domain->owner = std::this_thread::get_id();
    domain->stats.domain_id = domain_id;
    domain->stats.metadata_bytes = metadata_bytes;
    *out = domain;
    return OGC_OK;
}
OgcStatus OGC_CALL ogc_domain_destroy(OgcDomain** pointer) {
    if (!pointer || !*pointer) return OGC_INVALID_ARGUMENT;
    auto* domain = *pointer;
    const auto status = domain->admission();
    if (status != OGC_OK) return status;
    if (domain->stats.roots || domain->stats.constructing_objects) return OGC_BUSY;
    domain->busy = true;
    while (domain->objects) {
        auto* object = domain->objects;
        domain->objects = object->next;
        domain->release_object(object);
    }
    const auto allocator = domain->config.metadata_allocator;
    const auto self = domain->self;
    domain->~OgcDomain();
    // Output is inaccessible to a reentrant deallocator after destruction.
    *pointer = nullptr;
    allocator.deallocate(allocator.context, self);
    return OGC_OK;
}
OgcStatus OGC_CALL ogc_construct(OgcDomain* domain, const OgcLayout* layout, OgcObjectInfo* out) {
    auto status = check(domain);
    if (status != OGC_OK) return status;
    if (!layout || !out || !layout->bytes || !opengc::valid_alignment(layout->alignment) ||
        (layout->reference_count && (!layout->reference_offsets || layout->alignment < alignof(void*))))
        return OGC_INVALID_ARGUMENT;
    if (layout->bytes > SIZE_MAX || opengc::multiply_overflow(layout->reference_count, sizeof(uint64_t)) ||
        opengc::add_overflow(sizeof(Object), layout->reference_count * sizeof(uint64_t))) return OGC_SIZE_OVERFLOW;
    for (uint64_t i = 0; i < layout->reference_count; ++i) {
        const auto offset = layout->reference_offsets[i];
        if (offset > layout->bytes || layout->bytes - offset < sizeof(void*) || offset % alignof(void*) ||
            (i && offset <= layout->reference_offsets[i - 1])) return OGC_INVALID_ARGUMENT;
    }
    if (domain->next_serial == UINT64_MAX) return OGC_ID_EXHAUSTED;
    Guard guard(*domain);
    OgcAllocation metadata{};
    status = opengc::allocate(domain->config.metadata_allocator,
        sizeof(Object) + layout->reference_count * sizeof(uint64_t), alignof(Object),
        domain->config.metadata_limit, domain->stats.metadata_bytes, metadata);
    if (status != OGC_OK) return status;
    OgcAllocation storage{};
    status = opengc::allocate(domain->config.data_allocator, layout->bytes, layout->alignment,
        domain->config.data_limit, domain->stats.data_bytes, storage);
    if (status != OGC_OK) {
        opengc::deallocate(domain->config.metadata_allocator, metadata, domain->stats.metadata_bytes);
        return status;
    }
    auto* object = new (metadata.base) Object{};
    object->storage = storage;
    object->metadata = metadata;
    object->info = {{domain->stats.domain_id, domain->next_serial++}, storage.base,
                     layout->bytes, layout->reference_count};
    if (layout->reference_count) std::memcpy(object->offsets(), layout->reference_offsets,
                                            layout->reference_count * sizeof(uint64_t));
    std::memset(storage.base, 0, static_cast<size_t>(layout->bytes));
    object->next = domain->objects;
    domain->objects = object;
    ++domain->stats.constructing_objects;
    *out = object->info;
    return OGC_OK;
}
OgcStatus OGC_CALL ogc_abort(OgcDomain* domain, OgcId id) {
    auto status = check(domain);
    if (status != OGC_OK) return status;
    auto* object = domain->find(id);
    if (!object) return OGC_STALE_HANDLE;
    if (object->published) return OGC_INVALID_ARGUMENT;
    Guard guard(*domain);
    auto** link = &domain->objects;
    while (*link != object) link = &(*link)->next;
    *link = object->next;
    domain->release_object(object);
    return OGC_OK;
}
OgcStatus OGC_CALL ogc_publish_batch(OgcDomain* domain, const OgcId* ids, uint64_t count) {
    auto status = check(domain);
    if (status != OGC_OK) return status;
    if (!ids || !count) return OGC_INVALID_ARGUMENT;
    for (uint64_t i = 0; i < count; ++i) {
        auto* object = domain->find(ids[i]);
        if (!object) return OGC_STALE_HANDLE;
        if (object->published) return OGC_INVALID_ARGUMENT;
        for (uint64_t j = 0; j < i; ++j)
            if (opengc::same_id(ids[i], ids[j])) return OGC_INVALID_ARGUMENT;
        for (uint64_t j = 0; j < object->info.reference_count; ++j) {
            void* address = nullptr;
            std::memcpy(&address, static_cast<unsigned char*>(object->info.base) + object->offsets()[j], sizeof(address));
            if (!address) continue;
            auto* target = domain->inspect(reinterpret_cast<uintptr_t>(address), OGC_EDGE_EXACT, false);
            if (!target) return OGC_INVALID_EDGE;
            if (target->published) continue;
            bool in_batch = false;
            for (uint64_t k = 0; k < count; ++k)
                if (opengc::same_id(target->info.id, ids[k])) { in_batch = true; break; }
            if (!in_batch) return OGC_INVALID_EDGE;
        }
    }
    // No callbacks, allocations or failures after complete admission.
    for (uint64_t i = 0; i < count; ++i) domain->find(ids[i])->published = true;
    domain->stats.constructing_objects -= count;
    domain->stats.published_objects += count;
    return OGC_OK;
}
OgcStatus OGC_CALL ogc_store(OgcDomain* domain, OgcId id, uint64_t offset, const void* target) {
    auto status = check(domain);
    if (status != OGC_OK) return status;
    auto* object = domain->find(id);
    if (!object) return OGC_STALE_HANDLE;
    bool slot = false;
    for (uint64_t i = 0; i < object->info.reference_count; ++i)
        if (object->offsets()[i] == offset) { slot = true; break; }
    if (!slot) return OGC_INVALID_ARGUMENT;
    if (target && !domain->inspect(reinterpret_cast<uintptr_t>(target), OGC_EDGE_EXACT, object->published))
        return OGC_INVALID_EDGE;
    std::memcpy(static_cast<unsigned char*>(object->info.base) + offset, &target, sizeof(target));
    return OGC_OK;
}
OgcStatus OGC_CALL ogc_inspect(OgcDomain* domain, uintptr_t address, uint32_t kind, OgcObjectInfo* out) {
    auto status = check(domain);
    if (status != OGC_OK) return status;
    if (!out || kind > OGC_EDGE_INTERIOR) return OGC_INVALID_ARGUMENT;
    const auto* object = domain->inspect(address, kind, true);
    *out = object ? object->info : OgcObjectInfo{};
    return OGC_OK;
}
OgcStatus OGC_CALL ogc_root_create(OgcDomain* domain, OgcEdge edge, OgcId* out) {
    auto status = check(domain);
    if (status != OGC_OK) return status;
    if (!out) return OGC_INVALID_ARGUMENT;
    status = domain->validate_edge(edge);
    if (status != OGC_OK) return status;
    if (domain->next_serial == UINT64_MAX) return OGC_ID_EXHAUSTED;
    Guard guard(*domain);
    OgcAllocation metadata{};
    status = opengc::allocate(domain->config.metadata_allocator, sizeof(Root), alignof(Root),
        domain->config.metadata_limit, domain->stats.metadata_bytes, metadata);
    if (status != OGC_OK) return status;
    auto* root = new (metadata.base) Root{domain->roots, {domain->stats.domain_id, domain->next_serial++}, edge, metadata};
    domain->roots = root;
    ++domain->stats.roots;
    *out = root->id;
    return OGC_OK;
}
OgcStatus OGC_CALL ogc_root_set(OgcDomain* domain, OgcId id, OgcEdge edge) {
    auto status = check(domain);
    if (status != OGC_OK) return status;
    auto* root = domain->find_root(id);
    if (!root) return OGC_STALE_HANDLE;
    status = domain->validate_edge(edge);
    if (status != OGC_OK) return status;
    root->edge = edge;
    return OGC_OK;
}
OgcStatus OGC_CALL ogc_root_remove(OgcDomain* domain, OgcId id) {
    auto status = check(domain);
    if (status != OGC_OK) return status;
    auto* root = domain->find_root(id);
    if (!root) return OGC_STALE_HANDLE;
    Guard guard(*domain);
    auto** link = &domain->roots;
    while (*link != root) link = &(*link)->next;
    *link = root->next;
    --domain->stats.roots;
    opengc::deallocate(domain->config.metadata_allocator, root->metadata, domain->stats.metadata_bytes);
    return OGC_OK;
}
OgcStatus OGC_CALL ogc_collect(OgcDomain* domain, OgcCollection* out) {
    auto status = check(domain);
    if (status != OGC_OK) return status;
    if (!out) return OGC_INVALID_ARGUMENT;
    if (domain->stats.completed_collections == UINT64_MAX) return OGC_ID_EXHAUSTED;
    const OgcServices services{sizeof(OgcServices), OGC_ABI_MAJOR, domain, begin, end,
                               object_next, root_next, trace_edge, retire};
    OgcCollection result{};
    status = domain->provider.collect(&services, &domain->config.metadata_allocator,
                                      domain->config.scratch_limit, &result);
    if (status == OGC_OK && result.outcome == OGC_COLLECTED) ++domain->stats.completed_collections;
    // Admission failure does not change the caller's output.
    if (status != OGC_BUSY && status != OGC_WRONG_OWNER) *out = result;
    return status;
}
OgcStatus OGC_CALL ogc_stats(OgcDomain* domain, OgcStats* out) {
    const auto status = check(domain);
    if (status != OGC_OK) return status;
    if (!out) return OGC_INVALID_ARGUMENT;
    *out = domain->stats;
    return OGC_OK;
}
}
