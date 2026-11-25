/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 OpenGC contributors */
#include <opengc/opengc.h>
namespace {
OgcStatus OGC_CALL collect(const OgcServices* services, const OgcAllocator* allocator,
                          uint64_t, OgcCollection* out) {
    if (!out) return OGC_INVALID_ARGUMENT;
    auto status = ogc_validate_services(services);
    if (status != OGC_OK) return status;
    status = ogc_validate_allocator(allocator);
    if (status != OGC_OK) return status;
    uint64_t count = 0;
    status = services->begin(services->context, &count);
    if (status != OGC_OK) return status;
    *out = {};
    out->outcome = OGC_DISABLED;
    services->end(services->context);
    return OGC_OK;
}
const OgcProvider provider{{sizeof(OgcProvider), OGC_ABI_MAJOR, OGC_ABI_MINOR, 8,
    OGC_CAP_STABLE_ADDRESSES | OGC_CAP_RETAIN}, collect};
}
extern "C" OgcStatus OGC_CALL ogc_query_retain(const OgcQuery* query, const OgcProvider** out) {
    if (!out) return OGC_INVALID_ARGUMENT;
    *out = nullptr;
    const auto status = ogc_validate_provider(query, &provider);
    if (status == OGC_OK) *out = &provider;
    return status;
}
