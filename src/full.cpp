/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 OpenGC contributors */
#include <opengc/opengc.h>
#include "memory.h"
#include <cstdlib>

namespace {
struct Entry {
    OgcObjectInfo info;
    bool marked;
};
struct Lease {
    const OgcServices& services;
    ~Lease() { services.end(services.context); }
};
struct Scratch {
    const OgcAllocator& allocator;
    OgcAllocation receipt{};
    uint64_t bytes = 0;
    ~Scratch() { if (receipt.base) opengc::deallocate(allocator, receipt, bytes); }
};
int compare_entries(const void* left, const void* right) {
    const auto a = reinterpret_cast<uintptr_t>(static_cast<const Entry*>(left)->info.base);
    const auto b = reinterpret_cast<uintptr_t>(static_cast<const Entry*>(right)->info.base);
    return a < b ? -1 : a > b ? 1 : 0;
}
struct Marker {
    Entry* entries;
    uint64_t* queue;
    uint64_t count;
    uint64_t queued = 0;

    OgcStatus shade(OgcEdge edge) {
        if (edge.reserved || edge.kind > OGC_EDGE_INTERIOR) return OGC_INVALID_EDGE;
        if (!edge.address) return OGC_OK;
        uint64_t low = 0, high = count;
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            const auto base = reinterpret_cast<uintptr_t>(entries[middle].info.base);
            if (base <= edge.address) low = middle + 1;
            else high = middle;
        }
        if (!low) return edge.kind == OGC_EDGE_INTERIOR ? OGC_OK : OGC_INVALID_EDGE;
        auto& entry = entries[low - 1];
        const auto base = reinterpret_cast<uintptr_t>(entry.info.base);
        const bool matched = edge.address == base ||
            (edge.kind == OGC_EDGE_INTERIOR && edge.address - base < entry.info.bytes);
        if (!matched) return edge.kind == OGC_EDGE_INTERIOR ? OGC_OK : OGC_INVALID_EDGE;
        if (!entry.marked) {
            entry.marked = true;
            queue[queued++] = low - 1;
        }
        return OGC_OK;
    }
};
OgcStatus run(const OgcServices& services, const OgcAllocator& allocator, uint64_t limit,
              uint64_t count, OgcCollection& result) {
    if (opengc::multiply_overflow(count, sizeof(Entry) + sizeof(uint64_t))) return OGC_SIZE_OVERFLOW;
    Scratch scratch{allocator};
    const auto bytes = count * (sizeof(Entry) + sizeof(uint64_t));
    if (bytes) {
        const auto status = opengc::allocate(allocator, bytes, alignof(Entry), limit, scratch.bytes, scratch.receipt);
        if (status != OGC_OK) return status;
    }
    auto* entries = static_cast<Entry*>(scratch.receipt.base);
    auto* queue = count ? reinterpret_cast<uint64_t*>(entries + count) : nullptr;
    uint64_t cursor = 0;
    uint64_t total_bytes = 0;
    for (uint64_t i = 0; i <= count; ++i) {
        OgcObjectInfo info{};
        uint32_t has = 0;
        const auto status = services.object_next(services.context, &cursor, &info, &has);
        if (status != OGC_OK) return status;
        if (has > 1 || (has != 0) != (i < count)) return OGC_SCAN_FAILURE;
        if (!has) break;
        const auto base = reinterpret_cast<uintptr_t>(info.base);
        if (!base || !info.id.domain || !info.id.serial || !info.bytes || info.bytes > UINTPTR_MAX - base ||
            opengc::add_overflow(total_bytes, info.bytes)) return OGC_SCAN_FAILURE;
        total_bytes += info.bytes;
        entries[i] = {info, false};
        ++result.examined_objects;
    }
    if (count) std::qsort(entries, static_cast<size_t>(count), sizeof(Entry), compare_entries);
    for (uint64_t i = 1; i < count; ++i) {
        const auto previous = reinterpret_cast<uintptr_t>(entries[i - 1].info.base);
        const auto current = reinterpret_cast<uintptr_t>(entries[i].info.base);
        if (current - previous < entries[i - 1].info.bytes) return OGC_SCAN_FAILURE;
    }
    Marker marker{entries, queue, count};
    cursor = 0;
    for (;;) {
        OgcEdge edge{};
        uint32_t has = 0;
        auto status = services.root_next(services.context, &cursor, &edge, &has);
        if (status != OGC_OK) return status;
        if (!has) break;
        if (has != 1 || result.examined_edges == UINT64_MAX) return OGC_SCAN_FAILURE;
        ++result.examined_edges;
        status = marker.shade(edge);
        if (status != OGC_OK) return status;
    }
    for (uint64_t head = 0; head < marker.queued; ++head) {
        const auto& info = entries[queue[head]].info;
        for (uint64_t index = 0; index < info.reference_count; ++index) {
            OgcEdge edge{};
            auto status = services.trace_edge(services.context, info.id, index, &edge);
            if (status != OGC_OK) return status;
            if (edge.kind != OGC_EDGE_EXACT || result.examined_edges == UINT64_MAX) return OGC_INVALID_EDGE;
            ++result.examined_edges;
            status = marker.shade(edge);
            if (status != OGC_OK) return status;
        }
    }
    result.live_objects = marker.queued;
    // The snapshot, all roots and every reachable field have passed validation.
    for (uint64_t i = 0; i < count; ++i) {
        if (entries[i].marked) continue;
        const auto status = services.retire(services.context, entries[i].info.id);
        if (status != OGC_OK) return status;
        ++result.retired_objects;
        result.retired_bytes += entries[i].info.bytes;
    }
    return OGC_OK;
}
OgcStatus OGC_CALL collect(const OgcServices* services, const OgcAllocator* allocator,
                          uint64_t limit, OgcCollection* out) {
    if (!out) return OGC_INVALID_ARGUMENT;
    auto status = ogc_validate_services(services);
    if (status != OGC_OK) return status;
    status = ogc_validate_allocator(allocator);
    if (status != OGC_OK) return status;
    uint64_t count = 0;
    status = services->begin(services->context, &count);
    if (status != OGC_OK) return status;
    Lease lease{*services};
    OgcCollection result{};
    status = run(*services, *allocator, limit, count, result);
    *out = result;
    return status;
}
const OgcProvider provider{{sizeof(OgcProvider), OGC_ABI_MAJOR, OGC_ABI_MINOR, 8,
    OGC_CAP_STABLE_ADDRESSES | OGC_CAP_EXPLICIT_FULL}, collect};
}
extern "C" OgcStatus OGC_CALL ogc_query_full(const OgcQuery* query, const OgcProvider** out) {
    if (!out) return OGC_INVALID_ARGUMENT;
    *out = nullptr;
    const auto status = ogc_validate_provider(query, &provider);
    if (status == OGC_OK) *out = &provider;
    return status;
}
