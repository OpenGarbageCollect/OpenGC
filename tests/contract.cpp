/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 OpenGC contributors */
#include <opengc/opengc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return false; \
} } while (0)
#define OK(expression) CHECK((expression) == OGC_OK)

namespace {
static_assert(sizeof(OgcAbi) == 24 && sizeof(OgcQuery) == 24);
static_assert(sizeof(OgcAllocation) == 32 && sizeof(OgcId) == 16);
static_assert(sizeof(OgcObjectInfo) == 40 && sizeof(OgcEdge) == 16);
static_assert(sizeof(OgcCollection) == 48);

OgcDomainConfig config(bool retain = false) {
    const OgcProvider* provider = nullptr;
    const OgcQuery query{sizeof(OgcQuery), OGC_ABI_MAJOR, 0, 8,
        retain ? OGC_CAP_RETAIN : OGC_CAP_EXPLICIT_FULL};
    const auto status = retain ? ogc_query_retain(&query, &provider) : ogc_query_full(&query, &provider);
    if (status != OGC_OK) std::abort();
    return {sizeof(OgcDomainConfig), OGC_ABI_MAJOR, provider, *ogc_system_allocator(),
        *ogc_system_allocator(), UINT64_MAX, UINT64_MAX, UINT64_MAX};
}
const uint64_t offsets[] = {0};
const OgcLayout node{16, 8, offsets, 1};
OgcEdge exact(const void* address) { return {reinterpret_cast<uintptr_t>(address), OGC_EDGE_EXACT, 0}; }

bool abi_and_allocator() {
    const OgcProvider* provider = reinterpret_cast<const OgcProvider*>(uintptr_t{1});
    OgcQuery query{sizeof(OgcQuery), OGC_ABI_MAJOR, 0, 8, UINT64_C(1) << 63};
    CHECK(ogc_query_full(&query, &provider) == OGC_UNSUPPORTED);
    CHECK(provider == nullptr);
    query.required_capabilities = OGC_CAP_RETAIN;
    CHECK(ogc_query_full(&query, &provider) == OGC_UNSUPPORTED);
    query.required_capabilities = OGC_CAP_EXPLICIT_FULL;
    CHECK(ogc_query_retain(&query, &provider) == OGC_UNSUPPORTED);
    query.required_capabilities = 0;
    query.major = 2;
    CHECK(ogc_query_full(&query, &provider) == OGC_INCOMPATIBLE_ABI);
    query.major = OGC_ABI_MAJOR;
    query.minimum_minor = 1;
    CHECK(ogc_query_full(&query, &provider) == OGC_INCOMPATIBLE_ABI);
    query.minimum_minor = 0;
    query.pointer_bytes = 4;
    CHECK(ogc_query_full(&query, &provider) == OGC_INCOMPATIBLE_ABI);
    query.pointer_bytes = 8;
    query.struct_size = sizeof(uint32_t);
    CHECK(ogc_query_full(&query, &provider) == OGC_INCOMPATIBLE_ABI);
    query.struct_size = sizeof(query);
    OK(ogc_query_full(&query, &provider));
    const uint32_t short_size = sizeof(uint32_t);
    CHECK(ogc_validate_provider(&query, reinterpret_cast<const OgcProvider*>(&short_size)) == OGC_INCOMPATIBLE_ABI);
    CHECK(ogc_validate_allocator(reinterpret_cast<const OgcAllocator*>(&short_size)) == OGC_INCOMPATIBLE_ABI);
    auto broken = *provider;
    broken.collect = nullptr;
    CHECK(ogc_validate_provider(&query, &broken) == OGC_INVALID_ARGUMENT);
    auto* allocator = ogc_system_allocator();
    OgcAllocation allocation{};
    allocation.bytes = 73;
    CHECK(allocator->allocate(nullptr, 1, 3, &allocation) == OGC_INVALID_ARGUMENT);
    CHECK(allocation.bytes == 73);
    CHECK(allocator->allocate(nullptr, UINT64_MAX, 8, &allocation) == OGC_SIZE_OVERFLOW);
    CHECK(allocation.bytes == 73);
    OK(allocator->allocate(nullptr, 19, 4096, &allocation));
    CHECK(reinterpret_cast<uintptr_t>(allocation.base) % 4096 == 0);
    allocator->deallocate(nullptr, allocation);
    auto options = config();
    options.metadata_limit = 0;
    OgcDomain* domain = nullptr;
    CHECK(ogc_domain_create(&options, &domain) == OGC_BUDGET_EXCEEDED);
    CHECK(domain == nullptr);
    options = config();
    options.abi_major = 9;
    CHECK(ogc_domain_create(&options, &domain) == OGC_INCOMPATIBLE_ABI);
    return true;
}

bool cycles_publication_and_identity() {
    auto options = config();
    OgcDomain* domain = nullptr;
    OK(ogc_domain_create(&options, &domain));
    OgcObjectInfo a{}, b{}, found{};
    OK(ogc_construct(domain, &node, &a));
    OK(ogc_construct(domain, &node, &b));
    OK(ogc_store(domain, a.id, 0, b.base));
    OK(ogc_store(domain, b.id, 0, a.base));
    CHECK(ogc_publish_batch(domain, &a.id, 1) == OGC_INVALID_EDGE);
    OgcStats stats{};
    OK(ogc_stats(domain, &stats));
    CHECK(stats.constructing_objects == 2 && stats.published_objects == 0);
    OgcCollection result{};
    result.examined_objects = 95;
    CHECK(ogc_collect(domain, &result) == OGC_BUSY);
    CHECK(result.examined_objects == 95);
    CHECK(ogc_domain_destroy(&domain) == OGC_BUSY && domain);
    OgcId root{};
    CHECK(ogc_root_create(domain, exact(a.base), &root) == OGC_INVALID_EDGE);
    const OgcId repeated[] = {a.id, a.id};
    CHECK(ogc_publish_batch(domain, repeated, 2) == OGC_INVALID_EDGE);
    const OgcId group[] = {a.id, b.id};
    OK(ogc_publish_batch(domain, group, 2));
    CHECK(ogc_abort(domain, a.id) == OGC_INVALID_ARGUMENT);
    OK(ogc_inspect(domain, reinterpret_cast<uintptr_t>(a.base) + 15, OGC_EDGE_INTERIOR, &found));
    CHECK(found.id.serial == a.id.serial);
    OK(ogc_inspect(domain, reinterpret_cast<uintptr_t>(a.base) + 16, OGC_EDGE_INTERIOR, &found));
    CHECK(found.base == nullptr || found.base != a.base);
    OK(ogc_inspect(domain, reinterpret_cast<uintptr_t>(a.base) + 1, OGC_EDGE_EXACT, &found));
    CHECK(found.base == nullptr);
    OK(ogc_inspect(domain, UINTPTR_MAX, OGC_EDGE_INTERIOR, &found));
    CHECK(found.base == nullptr);
    OK(ogc_root_create(domain, exact(a.base), &root));
    OK(ogc_collect(domain, &result));
    CHECK(result.live_objects == 2 && result.retired_objects == 0 && result.examined_edges == 3);
    CHECK(ogc_domain_destroy(&domain) == OGC_BUSY);
    auto bad = exact(reinterpret_cast<void*>(uintptr_t{1}));
    CHECK(ogc_root_set(domain, root, bad) == OGC_INVALID_EDGE);
    CHECK(ogc_store(domain, a.id, 8, b.base) == OGC_INVALID_ARGUMENT);
    CHECK(ogc_store(domain, a.id, 0, reinterpret_cast<void*>(uintptr_t{1})) == OGC_INVALID_EDGE);
    OK(ogc_collect(domain, &result));
    CHECK(result.live_objects == 2);
    OK(ogc_root_remove(domain, root));
    CHECK(ogc_root_remove(domain, root) == OGC_STALE_HANDLE);
    OK(ogc_collect(domain, &result));
    CHECK(result.retired_objects == 2 && result.retired_bytes == 32);
    CHECK(ogc_store(domain, a.id, 0, nullptr) == OGC_STALE_HANDLE);
    OgcObjectInfo c{};
    OK(ogc_construct(domain, &node, &c));
    CHECK(c.id.serial > a.id.serial && c.id.serial > b.id.serial);
    OK(ogc_abort(domain, c.id));
    CHECK(ogc_abort(domain, c.id) == OGC_STALE_HANDLE);
    OK(ogc_collect(domain, &result));
    CHECK(result.examined_objects == 0);
    OK(ogc_domain_destroy(&domain));
    CHECK(domain == nullptr);
    return true;
}

bool domains_layouts_and_roots() {
    auto options = config();
    OgcDomain* a = nullptr;
    OgcDomain* b = nullptr;
    OK(ogc_domain_create(&options, &a));
    OK(ogc_domain_create(&options, &b));
    OgcObjectInfo object{}, foreign{};
    const uint64_t duplicate[] = {0, 0};
    OgcLayout invalid{16, 8, duplicate, 2};
    CHECK(ogc_construct(a, &invalid, &object) == OGC_INVALID_ARGUMENT);
    const uint64_t past_end[] = {16};
    invalid = {16, 8, past_end, 1};
    CHECK(ogc_construct(a, &invalid, &object) == OGC_INVALID_ARGUMENT);
    invalid = {UINT64_MAX, 8, offsets, UINT64_MAX};
    CHECK(ogc_construct(a, &invalid, &object) == OGC_SIZE_OVERFLOW);
    OK(ogc_construct(a, &node, &object));
    OK(ogc_construct(b, &node, &foreign));
    CHECK(object.id.domain != foreign.id.domain);
    CHECK(ogc_store(a, foreign.id, 0, nullptr) == OGC_STALE_HANDLE);
    OK(ogc_publish_batch(a, &object.id, 1));
    OK(ogc_publish_batch(b, &foreign.id, 1));
    CHECK(ogc_store(a, object.id, 0, foreign.base) == OGC_INVALID_EDGE);
    OgcId root{};
    const OgcEdge interior{reinterpret_cast<uintptr_t>(object.base) + 7, OGC_EDGE_INTERIOR, 0};
    OK(ogc_root_create(a, interior, &root));
    CHECK(ogc_root_remove(b, root) == OGC_STALE_HANDLE);
    OgcCollection result{};
    OK(ogc_collect(a, &result));
    CHECK(result.live_objects == 1);
    auto miss = interior;
    miss.address = UINTPTR_MAX;
    OK(ogc_root_set(a, root, miss));
    OK(ogc_collect(a, &result));
    CHECK(result.retired_objects == 1);
    OgcObjectInfo construction{};
    OK(ogc_construct(a, &node, &construction));
    OK(ogc_store(a, construction.id, 0, construction.base));
    const OgcId repeated[] = {construction.id, construction.id};
    CHECK(ogc_publish_batch(a, repeated, 2) == OGC_INVALID_ARGUMENT);
    OK(ogc_abort(a, construction.id));
    OK(ogc_root_remove(a, root));
    OK(ogc_domain_destroy(&a));
    OK(ogc_domain_destroy(&b));
    return true;
}

struct Probe {
    uint64_t live = 0;
    uint64_t allocations = 0;
    bool fail = false;
    OgcDomain* domain = nullptr;
    OgcStatus reentry = OGC_OK;
    static OgcStatus OGC_CALL allocate(void* context, uint64_t bytes, uint64_t alignment, OgcAllocation* out) {
        auto& probe = *static_cast<Probe*>(context);
        if (probe.domain) {
            OgcStats stats{};
            probe.reentry = ogc_stats(probe.domain, &stats);
        }
        if (probe.fail) return OGC_ALLOCATION_FAILED;
        const auto status = ogc_system_allocator()->allocate(nullptr, bytes, alignment, out);
        if (status == OGC_OK) { probe.live += bytes; ++probe.allocations; }
        return status;
    }
    static void OGC_CALL deallocate(void* context, OgcAllocation receipt) {
        auto& probe = *static_cast<Probe*>(context);
        if (probe.domain) {
            OgcStats stats{};
            probe.reentry = ogc_stats(probe.domain, &stats);
        }
        probe.live -= receipt.bytes;
        ogc_system_allocator()->deallocate(nullptr, receipt);
    }
    OgcAllocator table() { return {sizeof(OgcAllocator), OGC_ABI_MAJOR, this, allocate, deallocate}; }
};

bool pressure_and_reentry() {
    auto options = config();
    Probe data, metadata;
    options.data_allocator = data.table();
    options.metadata_allocator = metadata.table();
    options.data_limit = node.bytes;
    OgcDomain* domain = nullptr;
    metadata.fail = true;
    CHECK(ogc_domain_create(&options, &domain) == OGC_ALLOCATION_FAILED);
    CHECK(!domain && metadata.live == 0);
    metadata.fail = false;
    OK(ogc_domain_create(&options, &domain));
    data.domain = metadata.domain = domain;
    OgcStats initial{}, after{};
    OK(ogc_stats(domain, &initial));
    OgcObjectInfo object{};
    object.bytes = 87;
    data.fail = true;
    CHECK(ogc_construct(domain, &node, &object) == OGC_ALLOCATION_FAILED);
    CHECK(object.bytes == 87 && data.live == 0 && metadata.live == initial.metadata_bytes);
    data.fail = false;
    metadata.fail = true;
    CHECK(ogc_construct(domain, &node, &object) == OGC_ALLOCATION_FAILED);
    CHECK(data.live == 0 && metadata.live == initial.metadata_bytes);
    metadata.fail = false;
    OK(ogc_construct(domain, &node, &object));
    CHECK(data.reentry == OGC_BUSY && metadata.reentry == OGC_BUSY);
    OK(ogc_publish_batch(domain, &object.id, 1));
    OgcObjectInfo extra{};
    CHECK(ogc_construct(domain, &node, &extra) == OGC_BUDGET_EXCEEDED);
    OgcId root{75, 89};
    metadata.fail = true;
    CHECK(ogc_root_create(domain, exact(object.base), &root) == OGC_ALLOCATION_FAILED);
    CHECK(root.domain == 75 && root.serial == 89);
    OgcCollection result{};
    CHECK(ogc_collect(domain, &result) == OGC_ALLOCATION_FAILED);
    CHECK(result.retired_objects == 0 && data.live == node.bytes);
    metadata.fail = false;
    OK(ogc_collect(domain, &result));
    CHECK(result.retired_objects == 1 && data.live == 0 && metadata.live == initial.metadata_bytes);
    OK(ogc_stats(domain, &after));
    CHECK(after.metadata_bytes == initial.metadata_bytes && after.completed_collections == 1);
    // The final allocator callback runs after the domain's lifetime ends.
    data.domain = metadata.domain = nullptr;
    OK(ogc_domain_destroy(&domain));
    CHECK(data.live == 0 && metadata.live == 0);
    options = config();
    options.scratch_limit = 0;
    OK(ogc_domain_create(&options, &domain));
    OK(ogc_construct(domain, &node, &object));
    OK(ogc_publish_batch(domain, &object.id, 1));
    CHECK(ogc_collect(domain, &result) == OGC_BUDGET_EXCEEDED);
    OK(ogc_domain_destroy(&domain));
    return true;
}

bool retain_and_thread_owner() {
    auto options = config(true);
    OgcDomain* domain = nullptr;
    OK(ogc_domain_create(&options, &domain));
    OgcObjectInfo object{};
    OK(ogc_construct(domain, &node, &object));
    OK(ogc_publish_batch(domain, &object.id, 1));
    OgcCollection result{};
    OK(ogc_collect(domain, &result));
    CHECK(result.outcome == OGC_DISABLED && result.retired_objects == 0);
    OgcStats stats{};
    OK(ogc_stats(domain, &stats));
    CHECK(stats.published_objects == 1 && stats.completed_collections == 0);
    CHECK(ogc_store(domain, object.id, 0, reinterpret_cast<void*>(uintptr_t{1})) == OGC_INVALID_EDGE);
    OgcStatus read_status = OGC_OK, collect_status = OGC_OK, destroy_status = OGC_OK;
    std::thread foreign([&] {
        OgcStats untouched{};
        untouched.roots = 76;
        read_status = ogc_stats(domain, &untouched);
        if (untouched.roots != 76) read_status = OGC_SCAN_FAILURE;
        collect_status = ogc_collect(domain, &result);
        destroy_status = ogc_domain_destroy(&domain);
    });
    foreign.join();
    CHECK(read_status == OGC_WRONG_OWNER && collect_status == OGC_WRONG_OWNER && destroy_status == OGC_WRONG_OWNER);
    OK(ogc_domain_destroy(&domain));
    return true;
}

struct ServicesFixture {
    unsigned char payload[32]{};
    bool active = false;
    uint32_t begins = 0, ends = 0, retires = 0;
    OgcStatus scan_status = OGC_OK;
    bool invalid_root = false, bad_count = false;
    bool no_roots = false, two_objects = false, fail_retirement = false;
    static OgcStatus OGC_CALL begin(void* context, uint64_t* count) {
        auto& self = *static_cast<ServicesFixture*>(context);
        if (self.active) return OGC_BUSY;
        self.active = true;
        ++self.begins;
        *count = (self.bad_count || self.two_objects) ? 2 : 1;
        return OGC_OK;
    }
    static void OGC_CALL end(void* context) {
        auto& self = *static_cast<ServicesFixture*>(context);
        self.active = false;
        ++self.ends;
    }
    static OgcStatus OGC_CALL object_next(void* context, uint64_t* cursor, OgcObjectInfo* info, uint32_t* has) {
        auto& self = *static_cast<ServicesFixture*>(context);
        if (*cursor >= (self.two_objects ? 2u : 1u)) { *has = 0; return OGC_OK; }
        *info = {{1, *cursor + 1}, self.payload + *cursor * 16, 16, 1};
        ++*cursor;
        *has = 1;
        return OGC_OK;
    }
    static OgcStatus OGC_CALL root_next(void* context, uint64_t* cursor, OgcEdge* edge, uint32_t* has) {
        auto& self = *static_cast<ServicesFixture*>(context);
        if (*cursor || self.no_roots) { *has = 0; return OGC_OK; }
        *edge = self.invalid_root ? exact(reinterpret_cast<void*>(uintptr_t{1})) : exact(self.payload);
        *cursor = 1;
        *has = 1;
        return OGC_OK;
    }
    static OgcStatus OGC_CALL trace(void* context, OgcId, uint64_t, OgcEdge* edge) {
        auto& self = *static_cast<ServicesFixture*>(context);
        if (self.scan_status != OGC_OK) return self.scan_status;
        *edge = exact(nullptr);
        return OGC_OK;
    }
    static OgcStatus OGC_CALL retire(void* context, OgcId) {
        auto& self = *static_cast<ServicesFixture*>(context);
        if (self.fail_retirement && self.retires == 1) return OGC_STALE_HANDLE;
        ++self.retires;
        return OGC_OK;
    }
    OgcServices table() { return {sizeof(OgcServices), OGC_ABI_MAJOR, this, begin, end, object_next, root_next, trace, retire}; }
};

bool provider_failure_contract() {
    auto options = config();
    ServicesFixture fixture;
    const auto services = fixture.table();
    OgcCollection result{};
    fixture.scan_status = OGC_SCAN_FAILURE;
    CHECK(options.provider->collect(&services, ogc_system_allocator(), UINT64_MAX, &result) == OGC_SCAN_FAILURE);
    CHECK(fixture.begins == 1 && fixture.ends == 1 && fixture.retires == 0 && !fixture.active);
    CHECK(result.examined_objects == 1 && result.retired_objects == 0);
    fixture.scan_status = OGC_OK;
    fixture.invalid_root = true;
    CHECK(options.provider->collect(&services, ogc_system_allocator(), UINT64_MAX, &result) == OGC_INVALID_EDGE);
    CHECK(fixture.retires == 0 && fixture.ends == 2);
    fixture.invalid_root = false;
    fixture.bad_count = true;
    CHECK(options.provider->collect(&services, ogc_system_allocator(), UINT64_MAX, &result) == OGC_SCAN_FAILURE);
    CHECK(fixture.retires == 0 && fixture.ends == 3);
    fixture.bad_count = false;
    OK(options.provider->collect(&services, ogc_system_allocator(), UINT64_MAX, &result));
    CHECK(result.live_objects == 1 && fixture.ends == 4);
    fixture.active = true;
    result.live_objects = 86;
    CHECK(options.provider->collect(&services, ogc_system_allocator(), UINT64_MAX, &result) == OGC_BUSY);
    CHECK(result.live_objects == 86 && fixture.ends == 4);
    fixture.active = false;
    fixture.no_roots = true;
    fixture.two_objects = true;
    fixture.fail_retirement = true;
    CHECK(options.provider->collect(&services, ogc_system_allocator(), UINT64_MAX, &result) == OGC_STALE_HANDLE);
    CHECK(result.retired_objects == 1 && result.retired_bytes == 16 && fixture.ends == 5);
    return true;
}

bool corrupted_graph_is_not_reclaimed() {
    auto options = config();
    OgcDomain* domain = nullptr;
    OK(ogc_domain_create(&options, &domain));
    OgcObjectInfo live{}, dead{};
    OK(ogc_construct(domain, &node, &live));
    OK(ogc_construct(domain, &node, &dead));
    const OgcId group[] = {live.id, dead.id};
    OK(ogc_publish_batch(domain, group, 2));
    OgcId root{};
    OK(ogc_root_create(domain, exact(live.base), &root));
    // Simulate a broken foreign writer; the valid store API rejects this edge.
    void* invalid = reinterpret_cast<void*>(uintptr_t{1});
    std::memcpy(live.base, &invalid, sizeof(invalid));
    OgcCollection result{};
    CHECK(ogc_collect(domain, &result) == OGC_INVALID_EDGE);
    CHECK(result.retired_objects == 0);
    OgcStats stats{};
    OK(ogc_stats(domain, &stats));
    CHECK(stats.published_objects == 2 && stats.completed_collections == 0);
    OK(ogc_store(domain, live.id, 0, nullptr));
    OK(ogc_collect(domain, &result));
    CHECK(result.live_objects == 1 && result.retired_objects == 1);
    OK(ogc_root_remove(domain, root));
    OK(ogc_domain_destroy(&domain));
    return true;
}

bool long_graph_no_recursion() {
    auto options = config();
    OgcDomain* domain = nullptr;
    OK(ogc_domain_create(&options, &domain));
    OgcObjectInfo previous{};
    for (unsigned i = 0; i < 3000; ++i) {
        OgcObjectInfo current{};
        OK(ogc_construct(domain, &node, &current));
        OK(ogc_store(domain, current.id, 0, previous.base));
        OK(ogc_publish_batch(domain, &current.id, 1));
        previous = current;
    }
    OgcId root{};
    OK(ogc_root_create(domain, exact(previous.base), &root));
    OgcCollection result{};
    OK(ogc_collect(domain, &result));
    CHECK(result.live_objects == 3000 && result.retired_objects == 0);
    OK(ogc_root_remove(domain, root));
    OK(ogc_collect(domain, &result));
    CHECK(result.retired_objects == 3000);
    OK(ogc_domain_destroy(&domain));
    return true;
}

bool graph_reachability_oracle() {
    constexpr unsigned count = 128;
    constexpr unsigned root_count = 3;
    const uint64_t fields[] = {0, 8};
    const OgcLayout layout{16, 8, fields, 2};
    for (unsigned seed = 1; seed <= 12; ++seed) {
        auto options = config();
        OgcDomain* domain = nullptr;
        OK(ogc_domain_create(&options, &domain));
        OgcObjectInfo objects[count]{};
        OgcId ids[count]{};
        int edges[count][2]{};
        uint32_t random = seed;
        for (unsigned i = 0; i < count; ++i) {
            OK(ogc_construct(domain, &layout, &objects[i]));
            ids[i] = objects[i].id;
        }
        for (unsigned i = 0; i < count; ++i) {
            for (unsigned j = 0; j < 2; ++j) {
                random = random * UINT32_C(1664525) + UINT32_C(1013904223);
                // Disconnected partitions ensure both reachable and dead graphs.
                const unsigned partition = i / 32;
                const int target = random % 4 == 0 ? -1 : static_cast<int>(partition * 32 + (random >> 8) % 32);
                edges[i][j] = target;
                OK(ogc_store(domain, ids[i], j * 8, target < 0 ? nullptr : objects[target].base));
            }
        }
        OK(ogc_publish_batch(domain, ids, count));
        OgcId roots[root_count]{};
        bool reachable[count]{};
        unsigned queue[count]{}, queued = 0;
        for (unsigned i = 0; i < root_count; ++i) {
            const auto index = (seed * 7 + i * 11) % 32;
            OK(ogc_root_create(domain, exact(objects[index].base), &roots[i]));
            if (!reachable[index]) { reachable[index] = true; queue[queued++] = index; }
        }
        for (unsigned head = 0; head < queued; ++head)
            for (const int target : edges[queue[head]])
                if (target >= 0 && !reachable[target]) { reachable[target] = true; queue[queued++] = target; }
        OgcCollection result{};
        OK(ogc_collect(domain, &result));
        CHECK(result.live_objects == queued && result.retired_objects == count - queued);
        for (unsigned i = 0; i < count; ++i) {
            OgcObjectInfo found{};
            OK(ogc_inspect(domain, reinterpret_cast<uintptr_t>(objects[i].base), OGC_EDGE_EXACT, &found));
            CHECK((found.base != nullptr) == reachable[i]);
            if (reachable[i]) CHECK(found.id.serial == ids[i].serial);
        }
        for (auto root : roots) OK(ogc_root_remove(domain, root));
        OK(ogc_collect(domain, &result));
        CHECK(result.retired_objects == queued);
        OK(ogc_domain_destroy(&domain));
    }
    return true;
}
}

int main() {
    struct Test { const char* name; bool (*run)(); };
    const Test tests[] = {{"abi_and_allocator", abi_and_allocator},
        {"cycles_publication_and_identity", cycles_publication_and_identity},
        {"domains_layouts_and_roots", domains_layouts_and_roots},
        {"pressure_and_reentry", pressure_and_reentry},
        {"retain_and_thread_owner", retain_and_thread_owner},
        {"provider_failure_contract", provider_failure_contract},
        {"corrupted_graph_is_not_reclaimed", corrupted_graph_is_not_reclaimed},
        {"long_graph_no_recursion", long_graph_no_recursion},
        {"graph_reachability_oracle", graph_reachability_oracle}};
    for (const auto& test : tests) {
        if (!test.run()) { std::fprintf(stderr, "FAIL %s\n", test.name); return 1; }
        std::printf("PASS %s\n", test.name);
    }
    return 0;
}
