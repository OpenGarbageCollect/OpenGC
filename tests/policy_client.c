/* SPDX-License-Identifier: GPL-3.0-or-later */
/* External allocator/runtime host: no portable.h, domain or heap preset. */
#include "opengc/policy.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "policy line %d: %s\n", __LINE__, #x); return 1; } } while (0)
struct Object { OgcPolicyRecord record; struct Object* edge; unsigned registered; };
struct Host {
    const OgcPolicy* policy;
    OgcPolicyState state;
    struct Object objects[304];
    struct Object* root;
    unsigned count, retired, roots_called, edges_called, completed;
};
static struct Object* object(OgcPolicyRecord* record) { return (struct Object*)record; }
static void emit(struct Object** slot, OgcPolicyVisit visit, void* user) {
    if (*slot) {
        OgcPolicyEdge edge = {&(*slot)->record, slot, 0, 0};
        visit(user, &edge);
    }
}
static void roots(void* context, OgcPolicyVisit visit, void* user) {
    struct Host* h = (struct Host*)context; ++h->roots_called; emit(&h->root, visit, user);
}
static void edges(void* context, OgcPolicyRecord* record, OgcPolicyVisit visit, void* user) {
    struct Host* h = (struct Host*)context; ++h->edges_called; emit(&object(record)->edge, visit, user);
}
static void young(void* context, OgcPolicyVisitRecord visit, void* user) {
    struct Host* h = (struct Host*)context;
    unsigned i;
    for (i = 0; i < h->count; ++i) if (h->objects[i].registered && !h->objects[i].record.old)
        visit(user, &h->objects[i].record);
}
static void complete(void* context, uint32_t minor) {
    struct Host* h = (struct Host*)context;
    unsigned i; (void)minor; ++h->completed;
    for (i = 0; i < h->count; ++i) if (h->objects[i].registered && !ogc_policy_record_live(&h->state, &h->objects[i].record)) {
        h->objects[i].registered = 0; ++h->retired;
    }
}
static struct Object* allocate(struct Host* h) {
    struct Object* o = &h->objects[h->count++];
    memset(o, 0, sizeof(*o)); o->registered = 1; o->record.bytes = 16;
    ogc_policy_record_allocated(&h->state, &o->record); return o;
}
int main(void) {
    const OgcPolicy* p = NULL;
    struct Host h;
    OgcPolicyServices services = {OGC_POLICY_ABI, sizeof(services), &h, roots, edges, young, complete, NULL};
    int finished = 91;
    unsigned i;
    CHECK(ogc_policy_query(99, sizeof(*p), &p) == 3 && !p);
    CHECK(!ogc_policy_query(OGC_POLICY_ABI, sizeof(*p), &p));
    CHECK(p && p->abi == OGC_POLICY_ABI && !(p->capabilities & OGC_POLICY_MOVING));
    memset(&h, 0, sizeof(h)); h.policy = p; p->init(&h.state);
    h.root = allocate(&h); h.root->edge = allocate(&h); allocate(&h);
    if (!(p->capabilities & OGC_POLICY_COLLECTS)) {
        CHECK(p->collect(&h.state, &services) == 2 && p->minor(&h.state, &services) == 2);
        CHECK(p->step(&h.state, &services, 0, &finished) == 2 && finished == 91);
        CHECK(!h.retired && !h.roots_called && !h.edges_called && !h.completed);
    } else {
        CHECK(!p->collect(&h.state, &services));
        CHECK(h.retired == 1 && h.objects[0].registered && h.objects[1].registered && !h.objects[2].registered);
        CHECK(h.roots_called && h.edges_called && h.completed == 1);
        h.root->edge = allocate(&h); p->remember(&h.state, &h.root->record);
        CHECK(!p->minor(&h.state, &services) && h.objects[3].registered && h.objects[3].record.old);
        h.root = NULL;
        CHECK(!p->collect(&h.state, &services) && h.retired == 4);
        memset(&h, 0, sizeof(h)); h.policy = p; p->init(&h.state);
        for (i = 0; i < 300; ++i) {
            struct Object* o = allocate(&h);
            if (i) h.objects[i - 1].edge = o;
        }
        h.root = &h.objects[0];
        CHECK(!p->step(&h.state, &services, 0, &finished) && !finished && h.state.marking);
        /* Runtime applies SATB before overwriting an unvisited edge. */
        p->shade(&h.state, &h.objects[296].record, 0); h.objects[295].edge = NULL;
        CHECK(!p->collect(&h.state, &services) && !h.retired && !h.state.marking);
        CHECK(!p->collect(&h.state, &services) && h.retired == 4);
        services.size = 0;
        CHECK(p->collect(&h.state, &services) == 3);
    }
    printf("external allocator / %s policy passed\n", p->name);
    return 0;
}
