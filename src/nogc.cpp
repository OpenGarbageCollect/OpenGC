// SPDX-License-Identifier: GPL-3.0-or-later
// NoGC policy owns no allocations or object registry; its allocator owns storage.
#include "opengc/policy.h"
namespace {
void init(OgcPolicyState* s) { *s = {}; s->dirty_epoch = 1; }
void shade(OgcPolicyState*, OgcPolicyRecord*, uint32_t) {}
void remember(OgcPolicyState*, OgcPolicyRecord*) {}
int collect(OgcPolicyState*, const OgcPolicyServices*) { return 2; }
int step(OgcPolicyState*, const OgcPolicyServices*, uint64_t, int*) { return 2; }
const OgcPolicy policy{OGC_POLICY_ABI, sizeof(OgcPolicy), 0, 0, "nogc", init, shade, remember, collect, collect, step};
}
extern "C" int ogc_policy_query(uint32_t abi, uint32_t size, const OgcPolicy** out) {
    if (!out || abi != OGC_POLICY_ABI || size > sizeof(policy)) return 3;
    *out = &policy; return 0;
}
