/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 OpenGC contributors */
/*
 * The services heaps (heap.h implemented over an ABI 1 provider: OpenGC::heap_full, OpenGC::heap_retain)
 * as generated code sees them: nothing inline. Every allocation calls ogc_heap_alloc, which registers the
 * object with the heap; no write barrier (collection is stop-the-world and whole-heap, or none at all).
 *
 * The object layout is the same as marksweep's, because the language reads it: an 8-byte header whose
 * low 32 bits are the descriptor id sits right before the payload (16 bytes before it for a 16-byte
 * aligned fixed object, the word at -8 then zero). The regions exist only so a language context has the
 * same shape under every provider; nothing bumps them.
 */
#ifndef OPENGC_MUTATOR_SERVICES_H
#define OPENGC_MUTATOR_SERVICES_H

#include <stdint.h>

#define OGC_MUTATOR_INLINE_ALLOC 0
#define OGC_MUTATOR_CARDS 0
#define OGC_MUTATOR_SATB 0

#define OGC_MUTATOR_CLASSES_INIT {16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, \
                                  1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192}
#define OGC_MUTATOR_NUM_CLASSES 32

typedef struct OgcRegion {
    uint8_t* next;
    uint8_t* limit;
} OgcRegion;

struct OgcMutator {
    OgcRegion regions[OGC_MUTATOR_NUM_CLASSES];
};

/* No card table: one byte that nothing reads, and a marking flag that stays zero. */
#define OGC_MUTATOR_CARD_SHIFT 9
#define OGC_MUTATOR_CARD_BITS 0

#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t ogc_heap_marking_flag;
extern uint8_t ogc_heap_cards[1];
#ifdef __cplusplus
}
#endif

static inline uint64_t ogc_mutator_card_index(uintptr_t a) {
    (void)a;
    return 0;
}

#endif
