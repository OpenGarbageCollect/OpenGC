/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 OpenGC contributors */
#include <opengc/opengc.h>
#include <cinttypes>
#include <cstdio>
#include <new>
#include <type_traits>

namespace {
// Reference fields have the protocol's void* representation. Scalars are not scanned.
struct Node {
    void* next = nullptr;
    int32_t value = 0;
};
static_assert(std::is_standard_layout_v<Node> && std::is_trivially_destructible_v<Node>);

OgcStatus make_node(OgcDomain* domain, int32_t value, OgcObjectInfo& out) {
    const uint64_t offsets[] = {offsetof(Node, next)};
    const OgcLayout layout{sizeof(Node), alignof(Node), offsets, 1};
    OgcObjectInfo candidate{};
    auto status = ogc_construct(domain, &layout, &candidate);
    if (status != OGC_OK) return status;
    // Start a trivial C++ object's lifetime in GC-owned storage. Never delete it.
    new (candidate.base) Node{nullptr, value};
    status = ogc_publish_batch(domain, &candidate.id, 1);
    if (status != OGC_OK) {
        const auto cleanup = ogc_abort(domain, candidate.id);
        return cleanup == OGC_OK ? status : cleanup;
    }
    out = candidate;
    return OGC_OK;
}

OgcStatus print_stats(OgcDomain* domain, const char* stage) {
    OgcStats stats{};
    const auto status = ogc_stats(domain, &stats);
    if (status != OGC_OK) return status;
    std::printf("C++ %-20s objects=%" PRIu64 " roots=%" PRIu64 " data_bytes=%" PRIu64 "\n",
        stage, stats.published_objects, stats.roots, stats.data_bytes);
    return OGC_OK;
}
}

#define REQUIRE(expression) do { \
    const auto status = (expression); \
    if (status != OGC_OK) { std::fprintf(stderr, "%s: status %u\n", #expression, static_cast<unsigned>(status)); return 1; } \
} while (0)

int main() {
    const OgcQuery query{sizeof(OgcQuery), OGC_ABI_MAJOR, 0, 8, OGC_CAP_EXPLICIT_FULL};
    const OgcProvider* provider = nullptr;
    REQUIRE(ogc_query_full(&query, &provider));
    const OgcDomainConfig config{sizeof(OgcDomainConfig), OGC_ABI_MAJOR, provider,
        *ogc_system_allocator(), *ogc_system_allocator(), UINT64_MAX, UINT64_MAX, UINT64_MAX};
    OgcDomain* domain = nullptr;
    REQUIRE(ogc_domain_create(&config, &domain));

    OgcObjectInfo a{}, b{}, orphan{};
    REQUIRE(make_node(domain, 10, a));
    REQUIRE(make_node(domain, 20, b));
    REQUIRE(make_node(domain, 99, orphan));
    REQUIRE(ogc_store(domain, a.id, offsetof(Node, next), b.base));
    REQUIRE(ogc_store(domain, b.id, offsetof(Node, next), a.base));
    OgcId root{};
    REQUIRE(ogc_root_create(domain, {reinterpret_cast<uintptr_t>(a.base), OGC_EDGE_EXACT, 0}, &root));

    REQUIRE(print_stats(domain, "before collection"));
    OgcCollection result{};
    REQUIRE(ogc_collect(domain, &result));
    if (result.live_objects != 2 || result.retired_objects != 1) return 2;
    REQUIRE(print_stats(domain, "root keeps A <-> B"));
    auto* first = static_cast<Node*>(a.base);
    auto* second = static_cast<Node*>(first->next);
    std::printf("C++ rooted values: %d + %d = %d\n", first->value, second->value, first->value + second->value);
    if (first->value + second->value != 30) return 3;

    REQUIRE(ogc_root_remove(domain, root));
    REQUIRE(ogc_collect(domain, &result));
    if (result.retired_objects != 2) return 4;
    // first, second and all object.base values must no longer be dereferenced.
    REQUIRE(print_stats(domain, "after removing root"));
    REQUIRE(ogc_domain_destroy(&domain));
    return 0;
}
