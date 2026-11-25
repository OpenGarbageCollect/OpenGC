/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 OpenGC contributors */
#include <opengc/opengc.h>
#include <stdio.h>
#include <inttypes.h>
#define REQUIRE(expression) do { \
    OgcStatus status = (expression); \
    if (status != OGC_OK) { fprintf(stderr, "%s: status %u\n", #expression, (unsigned)status); return 1; } \
} while (0)

typedef struct Node {
    void* next;
    int32_t value;
} Node;

static OgcStatus print_stats(OgcDomain* domain, const char* stage) {
    OgcStats stats;
    OgcStatus status = ogc_stats(domain, &stats);
    if (status != OGC_OK) return status;
    printf("C %-20s objects=%" PRIu64 " roots=%" PRIu64 " data_bytes=%" PRIu64 "\n",
        stage, stats.published_objects, stats.roots, stats.data_bytes);
    return OGC_OK;
}

int main(void) {
    const OgcProvider* provider = NULL;
    OgcQuery query = {sizeof(OgcQuery), OGC_ABI_MAJOR, 0, 8, OGC_CAP_EXPLICIT_FULL};
    REQUIRE(ogc_query_full(&query, &provider));
    OgcDomainConfig config = {sizeof(OgcDomainConfig), OGC_ABI_MAJOR, provider,
        *ogc_system_allocator(), *ogc_system_allocator(), UINT64_MAX, UINT64_MAX, UINT64_MAX};
    OgcDomain* domain = NULL;
    REQUIRE(ogc_domain_create(&config, &domain));
    /* Only next is a GC edge; value is a scalar, even if its bits resemble a pointer. */
    const uint64_t offsets[] = {offsetof(Node, next)};
    const OgcLayout layout = {sizeof(Node), sizeof(void*), offsets, 1};
    OgcObjectInfo a, b, orphan;
    REQUIRE(ogc_construct(domain, &layout, &a));
    REQUIRE(ogc_construct(domain, &layout, &b));
    REQUIRE(ogc_construct(domain, &layout, &orphan));
    ((Node*)a.base)->value = 10;
    ((Node*)b.base)->value = 20;
    ((Node*)orphan.base)->value = 99;
    REQUIRE(ogc_store(domain, a.id, offsetof(Node, next), b.base));
    REQUIRE(ogc_store(domain, b.id, offsetof(Node, next), a.base));
    const OgcId group[] = {a.id, b.id, orphan.id};
    REQUIRE(ogc_publish_batch(domain, group, 3));
    OgcId root;
    const OgcEdge edge = {(uintptr_t)a.base, OGC_EDGE_EXACT, 0};
    REQUIRE(ogc_root_create(domain, edge, &root));
    REQUIRE(print_stats(domain, "before collection"));
    OgcCollection result;
    REQUIRE(ogc_collect(domain, &result));
    if (result.live_objects != 2 || result.retired_objects != 1) return 2;
    /* orphan.base is now invalid. A and B remain reachable through root. */
    REQUIRE(print_stats(domain, "root keeps A <-> B"));
    if (((Node*)a.base)->value + ((Node*)((Node*)a.base)->next)->value != 30) return 3;
    REQUIRE(ogc_root_remove(domain, root));
    REQUIRE(ogc_collect(domain, &result));
    if (result.retired_objects != 2) return 4;
    /* Neither stack variables nor the A/B cycle keep an object alive. */
    REQUIRE(print_stats(domain, "after removing root"));
    REQUIRE(ogc_domain_destroy(&domain));
    return 0;
}
