/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 OpenGC contributors */
/*
 * The marksweep heap as generated code sees it (heap.h, PROTOCOL.md §8): everything a compiler inlines.
 * A build that links OpenGC::marksweep compiles against this header and no other mutator header.
 *
 *   allocation  bump `next` in the region of the size class, compare with `limit`, store the header
 *               word (the descriptor id in its low 32 bits) at the cell, the payload follows (+8, or +16
 *               for a 16-byte aligned fixed object); past `limit` call ogc_heap_alloc.
 *   barrier     before overwriting a reference word outside a frame: clear its card byte
 *               (ogc_heap_barrier(heap).cards[ogc_mutator_card_index(address)] = 0), then, unless the store initialises a
 *               fresh object, while *ogc_heap_barrier(heap).marking is nonzero call ogc_heap_shade(heap, &word).
 */
#ifndef OPENGC_MUTATOR_MARKSWEEP_H
#define OPENGC_MUTATOR_MARKSWEEP_H

#include <stdint.h>

#define OGC_MUTATOR_PROVIDER "marksweep"
#define OGC_MUTATOR_ABI_VERSION 2
#define OGC_MUTATOR_CAPABILITIES 0x7full
#define OGC_MUTATOR_HEADER_BYTES 8
#define OGC_MUTATOR_DESC_ID_OFFSET (-8)
#define OGC_MUTATOR_DESC_ID_BITS 32
#define OGC_MUTATOR_ALIGN16_PAYLOAD_OFFSET 16
#define OGC_MUTATOR_REGION_OFFSET 0
#define OGC_MUTATOR_REGION_STRIDE 16
#define OGC_MUTATOR_INLINE_ALLOC 1
#define OGC_MUTATOR_CARDS 1          /* the barrier dirties a card */
#define OGC_MUTATOR_SATB 1           /* the barrier shades the old referent while marking */

/* Cells of each size class (header included); objects above the last are large. */
#define OGC_MUTATOR_CLASSES_INIT {16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, \
                             1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192}
#define OGC_MUTATOR_NUM_CLASSES 32

/* A run of free, zeroed cells of one class: the next cell is at `next`, used up when next + cell passes
 * `limit`. {null, null} is empty: the inline compare fails and the slow path refills it. */
typedef struct OgcRegion {
    uint8_t* next;
    uint8_t* limit;
} OgcRegion;

/* At offset 0 of the language's context, so a fast path reaches region c at c * 16 with one load pair. */
struct OgcMutator {
    OgcRegion regions[OGC_MUTATOR_NUM_CLASSES];
    struct OgcHeap* heap;
};

/* The card table: one byte per 512-byte card of address space, hashed by the bits above the card
 * (0 = dirty, 0xFF = clean). */
#define OGC_MUTATOR_CARD_SHIFT 9
#define OGC_MUTATOR_CARD_BITS 20

static inline uint64_t ogc_mutator_card_index(uintptr_t a) {
    return (a >> OGC_MUTATOR_CARD_SHIFT) & (((uint64_t)1 << OGC_MUTATOR_CARD_BITS) - 1);
}

#endif
