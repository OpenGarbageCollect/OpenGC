/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 OpenGC contributors */
#include <opengc/opengc.h>
int main(void) {
    const OgcProvider* provider = 0;
    const OgcQuery query = {sizeof(OgcQuery), OGC_ABI_MAJOR, 0, 8, OGC_CAP_STABLE_ADDRESSES};
#if USE_RETAIN
    if (ogc_query_retain(&query, &provider)) return 1;
#else
    if (ogc_query_full(&query, &provider)) return 1;
#endif
    const OgcDomainConfig config = {sizeof(OgcDomainConfig), OGC_ABI_MAJOR, provider,
        *ogc_system_allocator(), *ogc_system_allocator(), UINT64_MAX, UINT64_MAX, UINT64_MAX};
    OgcDomain* domain = 0;
    OgcCollection result;
    if (ogc_domain_create(&config, &domain)) return 2;
    if (ogc_collect(domain, &result)) return 3;
#if USE_RETAIN
    if (result.outcome != OGC_DISABLED) return 4;
#else
    if (result.outcome != OGC_COLLECTED) return 4;
#endif
    if (ogc_domain_destroy(&domain)) return 5;
    return domain ? 6 : 0;
}
