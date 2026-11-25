/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 OpenGC contributors */
#ifndef OPENGC_MEMORY_H
#define OPENGC_MEMORY_H
#include <opengc/protocol.h>
#include <cstring>
#include <limits>

namespace opengc {
inline bool valid_alignment(uint64_t alignment) {
    return alignment && !(alignment & (alignment - 1)) && alignment <= SIZE_MAX;
}
inline bool add_overflow(uint64_t a, uint64_t b) { return b > UINT64_MAX - a; }
inline bool multiply_overflow(uint64_t a, uint64_t b) { return b && a > UINT64_MAX / b; }
inline bool same_id(OgcId a, OgcId b) { return a.domain == b.domain && a.serial == b.serial; }
inline OgcStatus allocate(const OgcAllocator& allocator, uint64_t bytes, uint64_t alignment,
                          uint64_t limit, uint64_t& used, OgcAllocation& result) {
    if (!bytes || !valid_alignment(alignment)) return OGC_INVALID_ARGUMENT;
    if (bytes > SIZE_MAX || add_overflow(used, bytes)) return OGC_SIZE_OVERFLOW;
    if (used > limit || bytes > limit - used) return OGC_BUDGET_EXCEEDED;
    OgcAllocation receipt{};
    const auto status = allocator.allocate(allocator.context, bytes, alignment, &receipt);
    if (status != OGC_OK) return status;
    // Allocator success guarantees this receipt by protocol; no speculative frees.
    used += bytes;
    result = receipt;
    return OGC_OK;
}
inline void deallocate(const OgcAllocator& allocator, OgcAllocation receipt, uint64_t& used) {
    used -= receipt.bytes;
    allocator.deallocate(allocator.context, receipt);
}
}
#endif
