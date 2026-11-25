/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 OpenGC contributors */
#include <opengc/opengc.h>
#include "memory.h"
#include <cstdlib>

namespace {
OgcStatus OGC_CALL system_allocate(void*, uint64_t bytes, uint64_t alignment, OgcAllocation* out) {
    if (!out || !bytes || !opengc::valid_alignment(alignment)) return OGC_INVALID_ARGUMENT;
    if (bytes > SIZE_MAX || alignment - 1 > SIZE_MAX - bytes) return OGC_SIZE_OVERFLOW;
    void* token = std::malloc(static_cast<size_t>(bytes + alignment - 1));
    if (!token) return OGC_ALLOCATION_FAILED;
    const auto address = reinterpret_cast<uintptr_t>(token);
    const auto aligned = (address + alignment - 1) & ~(alignment - 1);
    *out = {reinterpret_cast<void*>(aligned), token, bytes, alignment};
    return OGC_OK;
}
void OGC_CALL system_deallocate(void*, OgcAllocation receipt) { std::free(receipt.token); }
const OgcAllocator allocator{sizeof(OgcAllocator), OGC_ABI_MAJOR, nullptr, system_allocate, system_deallocate};
}
extern "C" const OgcAllocator* OGC_CALL ogc_system_allocator(void) { return &allocator; }
