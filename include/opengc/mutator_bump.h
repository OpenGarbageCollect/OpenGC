/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bump heap: 16-byte prefix, inline fixed allocations, no collection or write barriers. */
#ifndef OPENGC_MUTATOR_BUMP_H
#define OPENGC_MUTATOR_BUMP_H

#include <stdint.h>

#define OGC_MUTATOR_PROVIDER "bump"
#define OGC_MUTATOR_ABI_VERSION 2
#define OGC_MUTATOR_CAPABILITIES 0x58ull
#define OGC_MUTATOR_HEADER_BYTES 16
#define OGC_MUTATOR_DESC_ID_OFFSET (-16)
#define OGC_MUTATOR_DESC_ID_BITS 32
#define OGC_MUTATOR_ALIGN16_PAYLOAD_OFFSET 16
#define OGC_MUTATOR_REGION_OFFSET 0
#define OGC_MUTATOR_REGION_STRIDE 16
#define OGC_MUTATOR_INLINE_ALLOC 1
#define OGC_MUTATOR_CARDS 0
#define OGC_MUTATOR_SATB 0

/* Cells of each size class (header included); objects above the last are large. */
#define OGC_MUTATOR_CLASSES_INIT                                                                                       \
    {32,  48,  64,   80,   96,   112,  128,  160,  192,  224,  256,  320,  384,  448,  512,  640,                      \
     768, 896, 1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192, 16384}
#define OGC_MUTATOR_NUM_CLASSES 32

/* A run of free, zeroed cells of one class: the next cell is at `next`, used up when next + cell passes
 * `limit`. {null, null} is empty: the inline compare fails and the slow path refills it. */
typedef struct OgcRegion {
    uint8_t *next;
    uint8_t *limit;
} OgcRegion;

/* At offset 0 of the language's context, so a fast path reaches region c at c * 16 with one load pair. */
struct OgcMutator {
    OgcRegion regions[OGC_MUTATOR_NUM_CLASSES];
    struct OgcHeap *heap;
};

/* Required layout declarations; no card table exists and no barrier may be generated. */
#define OGC_MUTATOR_CARD_SHIFT 0
#define OGC_MUTATOR_CARD_BITS 0

#endif
