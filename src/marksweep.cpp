// SPDX-License-Identifier: GPL-3.0-or-later
// Pure policy: allocator-owned records and reverse tracing services only.
#include "opengc/policy.h"
#include <chrono>
namespace {
uint64_t charge(const OgcPolicyRecord* n) { return (n->bytes + 8 + 15) & ~uint64_t(15); }
void init(OgcPolicyState* s) { *s = {}; s->dirty_epoch = 1; }
void shade(OgcPolicyState* s, OgcPolicyRecord* n, uint32_t young) {
    if (!ogc_policy_record_live(s, n)) return;
    if (young) {
        if (n->old || n->minor_epoch == s->minor_epoch) return;
        n->minor_epoch = s->minor_epoch;
    } else {
        if (n->mark_epoch == s->epoch) return;
        n->mark_epoch = s->epoch; n->old = 1; s->live_charge += charge(n);
    }
    n->queue_next = s->queue; s->queue = n;
}
void remember(OgcPolicyState* s, OgcPolicyRecord* n) {
    if (!n || !n->old || n->dirty_epoch == s->dirty_epoch) return;
    n->dirty_epoch = s->dirty_epoch; n->dirty_next = s->remembered; s->remembered = n;
}
struct Trace { OgcPolicyState* state; uint32_t young; };
void visit(void* p, const OgcPolicyEdge* edge) {
    const auto& t = *static_cast<Trace*>(p); shade(t.state, edge->target, t.young);
}
void roots(OgcPolicyState* s, const OgcPolicyServices* host, uint32_t young) {
    Trace t{s, young}; host->roots(host->context, &visit, &t);
}
void edges(OgcPolicyState* s, const OgcPolicyServices* host, OgcPolicyRecord* n, uint32_t young) {
    Trace t{s, young}; host->edges(host->context, n, &visit, &t);
}
bool drain(OgcPolicyState* s, const OgcPolicyServices* host, uint64_t quota, uint32_t young) {
    while (s->queue && quota--) {
        auto* n = s->queue; s->queue = n->queue_next; edges(s, host, n, young);
    }
    return !s->queue;
}
void begin(OgcPolicyState* s, const OgcPolicyServices* host) {
    ++s->epoch; s->live_charge = 0; s->marking = 1; s->queue = nullptr; roots(s, host, 0);
}
void complete(OgcPolicyState* s, const OgcPolicyServices* host, uint32_t young) {
    if (young) host->young(host->context, [](void* p, OgcPolicyRecord* n) {
        auto* state = static_cast<OgcPolicyState*>(p);
        if (n->minor_epoch == state->minor_epoch) { n->old = 1; state->old_charge += charge(n); }
        else n->dead = 1;
    }, s);
    else { s->completed_epoch = s->epoch; s->old_charge = s->live_charge; }
    s->remembered = nullptr; ++s->dirty_epoch; s->marking = 0;
    host->complete(host->context, young);
}
bool valid(OgcPolicyState* s, const OgcPolicyServices* h) {
    return s && h && h->abi == OGC_POLICY_ABI && h->size >= sizeof(*h) && h->roots && h->edges && h->young && h->complete;
}
int collect(OgcPolicyState* s, const OgcPolicyServices* host) {
    if (!valid(s, host)) return 3;
    if (!s->marking) begin(s, host);
    roots(s, host, 0); drain(s, host, UINT64_MAX, 0); complete(s, host, 0); return 0;
}
int minor(OgcPolicyState* s, const OgcPolicyServices* host) {
    if (!valid(s, host)) return 3;
    if (s->marking) return collect(s, host);
    ++s->minor_epoch; s->queue = nullptr; roots(s, host, 1);
    for (auto* n = s->remembered; n; n = n->dirty_next) if (ogc_policy_record_live(s, n)) edges(s, host, n, 1);
    drain(s, host, UINT64_MAX, 1); complete(s, host, 1); return 0;
}
int step(OgcPolicyState* s, const OgcPolicyServices* host, uint64_t budget, int* finished) {
    if (!valid(s, host) || !finished || budget > UINT32_MAX) return 3;
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(budget);
    bool done;
    do {
        if (!s->marking) begin(s, host);
        roots(s, host, 0); done = drain(s, host, 256, 0);
        if (done) complete(s, host, 0);
    } while (!done && std::chrono::steady_clock::now() < end);
    *finished = done; return 0;
}
const OgcPolicy policy{OGC_POLICY_ABI, sizeof(OgcPolicy), OGC_POLICY_COLLECTS | OGC_POLICY_MINOR | OGC_POLICY_STEP, 0,
    "marksweep", init, shade, remember, collect, minor, step};
}
extern "C" int ogc_policy_query(uint32_t abi, uint32_t size, const OgcPolicy** out) {
    if (!out || abi != OGC_POLICY_ABI || size > sizeof(policy)) return 3;
    *out = &policy; return 0;
}
