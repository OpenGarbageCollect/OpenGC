/* Compile the instance contract as C, then consume the C++ heap implementation. */
#include "opengc/heap.h"
#if OGC_TEST_BUMP
#include "opengc/mutator_bump.h"
#else
#include "opengc/mutator_marksweep.h"
#endif
#include <stdio.h>

static void reached(void* user, void* object, const OgcTypeDesc* descriptor) {
    (void)object;
    (void)descriptor;
    ++*(unsigned*)user;
}

int main(void) {
    OgcHeapConfig config = {OGC_HEAP_ABI_VERSION, sizeof(OgcHeapConfig), 0, OGC_HEAP_INLINE_ALLOC, 0};
    OgcHeap* heap = ogc_heap_create(&config);
    OgcMutator mutator = {0};
    const OgcTypeDesc* descriptor;
    void* object;
    unsigned count = 0;
    OgcWalk walk = {0};
    OgcStackScope stack_scope = {0};
    OgcStackBounds stack_bounds = ogc_heap_stack_current();
    uintptr_t root;
    if (ogc_heap_stack_enter(&stack_scope, stack_bounds) != OGC_HEAP_OK ||
        ogc_heap_stack_leave(&stack_scope) != OGC_HEAP_OK) return 1;
    if (!heap || ogc_heap_mutator_register(heap, &mutator) != OGC_HEAP_OK) return 1;
    descriptor = ogc_heap_intern(heap, 16, 8, 0, NULL, 0, NULL, "C consumer");
    object = ogc_heap_alloc(heap, &mutator, descriptor, 0);
    root = (uintptr_t)object;
    walk.words = &root; walk.nwords = 1; walk.object = reached; walk.user = &count;
    if (ogc_heap_walk_roots(heap, &walk) != OGC_HEAP_OK || count < 1) return 1;
    if (!object || ogc_heap_desc_of(heap, object) != descriptor ||
        ogc_heap_destroy(heap) != OGC_HEAP_BUSY ||
        ogc_heap_collect(heap) != ((ogc_heap_capabilities(heap) & OGC_HEAP_COLLECTS) ? OGC_HEAP_OK : OGC_HEAP_UNSUPPORTED) ||
        ogc_heap_mutator_unregister(heap, &mutator) != OGC_HEAP_OK || ogc_heap_destroy(heap) != OGC_HEAP_OK) return 1;
    puts("OK C heap instance consumer");
    return 0;
}
