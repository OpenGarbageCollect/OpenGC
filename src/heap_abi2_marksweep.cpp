// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 OpenGC contributors
//
// OpenGC::marksweep — the heap of lisp_core's redesigned language (docs/COMPILER_DESIGN.md), moved
// here behind opengc/heap.h: non-moving, mark-only, independent explicitly owned instances.
//
// An object is a cell of a size class (16-byte multiples up to 8 KiB, in 64 KiB pages) or a large
// object of its own mapping. The cell starts with an 8-byte header naming its descriptor; the payload
// follows it (at +16 when the type needs 16-byte alignment). A reference is the payload's address.
//
// Roots: the calling thread's stack and registers, scanned CONSERVATIVELY, plus every registered root
// range the same way, plus the handle table. A word is a reference when it points into an allocated
// cell (interior addresses included). What makes this sound is that nothing moves: a word that only
// looks like a reference retains garbage, it never corrupts. Objects are scanned PRECISELY by their
// descriptor's reference offsets, each referent still checked against the heap.
//
// Sweeping is lazy: a page's allocation bits are cut down to its mark bits when the allocator next
// takes a cell from it, or when the next collection starts. A collection's cost is the live objects
// plus a bitmap pass per page, never the dead objects.
//
// No Foundation, no STL containers, no exceptions: metadata lives in the small arrays below, and a
// failure to grow metadata aborts (it is bounded by the heap the program asked for); a failure to get
// object memory answers null to the caller.
#include "opengc/heap.h"
#include "opengc/mutator_marksweep.h"
#include "heap_support.h"

#include <chrono>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <new>

struct OgcHeap { void* state; };

namespace {
using namespace opengc::heap_detail;

constexpr uint32_t kClasses[] = OGC_MUTATOR_CLASSES_INIT;
constexpr uint32_t kNumClasses = OGC_MUTATOR_NUM_CLASSES;
static_assert(sizeof kClasses / sizeof kClasses[0] == kNumClasses, "the class table and its count agree");
constexpr uint32_t size_class(uint32_t bytes) {
    for (uint32_t i = 0; i < kNumClasses; ++i)
        if (kClasses[i] >= bytes) return i;
    return kNumClasses;
}

constexpr uintptr_t kPage = 64 * 1024;
constexpr uint32_t kPagesPerChunk = 64;
constexpr uintptr_t kChunk = kPage * kPagesPerChunk;   // 4 MiB, aligned to itself
constexpr uint32_t kMaxSmall = 8192;
constexpr uint32_t kMaxCells = kPage / 16;
constexpr uint32_t kWords = kMaxCells / 64;
constexpr uint16_t kFree = 0xFFFF;
constexpr uint64_t kMinTrigger = 8u << 20;
constexpr uint64_t kSlice = 512;                      // vector elements one mark-stack entry scans

// The first word of every cell.
struct Header {
    uint32_t desc;   // descriptor id
    uint32_t aux;
};

struct Page {
    uint8_t* base = nullptr;
    uint16_t cls = kFree;
    uint32_t cell = 0, ncells = 0;
    uint32_t hint = 0;             // first word that may have a free cell
    uint32_t mark_epoch = 0;       // mark bits are this major epoch's; otherwise they read as zero
    uint32_t swept_cycle = 0;      // alloc bits were cut to the marks as of this collection
    bool has_release = false;      // listed in Heap::release_pages (kept across a free and a retake)
    bool pending = false;          // in Heap::pending: doomed cells wait for their hooks
    uint64_t alloc[kWords];
    uint64_t mark[kWords];
    uint64_t release[kWords];      // cells whose object has a release hook
    uint64_t doomed[kWords];       // of those, found dead: allocated until finalize() runs the hook
};

struct Chunk {
    uint8_t* base = nullptr;
    uint8_t* mapping = nullptr;    // the over-sized mapping the aligned chunk was cut from
    size_t mapping_size = 0;
    Page pages[kPagesPerChunk];
};

struct Large {
    uint8_t* base = nullptr;       // header at base, payload at base + 16
    size_t size = 0;               // the mapping
    uint32_t mark_epoch = 0;
    bool doomed = false;           // dead with a release hook not yet run: kept mapped until finalize()
};

struct Hook {
    void (*fn)(void*);
    void* user;
};

// Chunks by base address, sorted: a few dozen at most for any heap the tests build.
struct ChunkEntry {
    uintptr_t base;
    Chunk* chunk;
};

struct Grey { uint8_t* payload; const OgcTypeDesc* desc; uint64_t from; };

struct Heap {
    uint8_t marking_flag = 0;
    uint8_t* cards = nullptr;
    uint64_t capabilities = 0;
    uint64_t limit = 0, mapped = 0;
    uint32_t finalizing = 0;
    bool destroying = false;
    Vec<OgcTypeDesc*> descs;
    Vec<ChunkEntry> chunks;        // sorted by base
    Vec<Chunk*> chunk_list;
    Vec<Page*> free_pages;
    Vec<Page*> classes[kNumClasses];
    uint32_t cursor[kNumClasses] = {};
    Vec<Large*> large;             // sorted by address
    Vec<OgcRootRange> roots;
    Vec<Hook> hooks;               // add_cycle_hook
    Vec<OgcMutator*> mutators;     // every live mutator: their regions are emptied when a collection starts
    Vec<Page*> release_pages;
    Vec<Page*> pending;
    uint64_t pending_count = 0;
    Vec<void*> handles;            // the handle table: slot i is handle i + 1; a free slot holds null
    Vec<uint32_t> free_handles;
    Vec<uint8_t> handle_used;
    // A vector is scanned kSlice elements per entry from `from`, the rest pushed back first, so one
    // object never makes a scan step unbounded.
    Vec<Grey> stack;               // the mark stack: marked objects whose references are not scanned yet
    uintptr_t last_chunk = 1;      // find's one-entry cache: a chunk base and its Chunk
    Chunk* last = nullptr;
    // An object is old when it is marked in the current epoch. A major collection starts a new epoch
    // (every mark bit reads as zero) and marks everything; a minor one keeps the epoch, so marking
    // stops at old objects and its survivors stay marked: promoted where they are.
    uint32_t epoch = 1;
    uint32_t cycle = 1;            // collections of either kind: what the lazy sweep is keyed on
    uint64_t since = 0, trigger = kMinTrigger, live = 0;
    uint64_t major_at = 2 * kMinTrigger;   // a collection with more live (old) bytes than this is major
    uint32_t stress_count = 0;
    uint64_t marked_bytes = 0;
    OgcHeapStats st = {};
    uint32_t stress = 0;           // 1 a major before every allocation, 2 minors (see before_alloc)
    uint32_t step_stress = 0;      // mark-stack entries each allocation scans
    bool marking = false;          // a cycle's marking is under way (marking_flag mirrors it)
    bool automatic = true;         // allocation may start a collection; off, only the host collects
    bool collecting = false;
};

OgcTypeDesc* new_desc() {
    OgcTypeDesc* d = static_cast<OgcTypeDesc*>(std::calloc(1, sizeof(OgcTypeDesc)));
    if (!d) metadata_exhausted();
    d->name = "";
    d->align = 8;
    return d;
}

Heap& heap(OgcHeap* instance) { return *static_cast<Heap*>(instance->state); }
const Heap& heap(const OgcHeap* instance) { return *static_cast<const Heap*>(instance->state); }

uint32_t ctz64(uint64_t x) { return uint32_t(__builtin_ctzll(x)); }
uint32_t popcount64(uint64_t x) { return uint32_t(__builtin_popcountll(x)); }

// The allocation bits of word w that stand for cells past the page's last whole one: always set.
uint64_t pad(const Page* p, uint32_t w) {
    const uint32_t first = w * 64;
    if (first + 64 <= p->ncells) return 0;
    if (first >= p->ncells) return ~uint64_t(0);
    return ~uint64_t(0) << (p->ncells - first);
}

// The first set bit of `bits` at or after `from`, or `end` (the pad bits past a page's last cell are
// set, so a page's allocation bits always stop a scan at its end).
uint32_t next_set(const uint64_t* bits, uint32_t from, uint32_t end) {
    uint32_t w = from / 64;
    uint64_t x = bits[w] & (~uint64_t(0) << (from % 64));
    while (!x) {
        if (++w == kWords) return end;
        x = bits[w];
    }
    const uint32_t i = w * 64 + ctz64(x);
    return i < end ? i : end;
}

// Sets bits [first, last).
void set_range(uint64_t* bits, uint32_t first, uint32_t last) {
    while (first < last) {
        const uint32_t w = first / 64, b = first % 64, n = last - first < 64 - b ? last - first : 64 - b;
        bits[w] |= (n == 64 ? ~uint64_t(0) : ((uint64_t(1) << n) - 1)) << b;
        first += n;
    }
}

Chunk* chunk_at(Heap& h, uintptr_t base) {
    uint32_t lo = 0, hi = h.chunks.size();
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (h.chunks[mid].base < base) lo = mid + 1;
        else hi = mid;
    }
    return lo < h.chunks.size() && h.chunks[lo].base == base ? h.chunks[lo].chunk : nullptr;
}

void chunk_insert(Heap& h, Chunk* c) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(c->base);
    uint32_t at = h.chunks.size();
    h.chunks.push_back(ChunkEntry{base, c});
    for (; at > 0 && h.chunks[at - 1].base > base; --at) h.chunks[at] = h.chunks[at - 1];
    h.chunks[at] = ChunkEntry{base, c};
}

void chunk_erase(Heap& h, uintptr_t base) {
    for (uint32_t i = 0; i < h.chunks.size(); ++i)
        if (h.chunks[i].base == base) { h.chunks.erase_at(i); return; }
}

bool new_chunk(Heap& h) {
    // Over-map and keep the aligned middle (released whole with the mapping).
    const size_t n = kChunk * 2;
    if (h.limit && (n > h.limit || h.mapped > h.limit - n)) return false;
    uint8_t* m = static_cast<uint8_t*>(os_map(n));
    if (!m) return false;
    Chunk* c = new (std::nothrow) Chunk;
    if (!c) {
        os_unmap(m, n);
        return false;
    }
    c->mapping = m;
    c->mapping_size = n;
    h.mapped += n;
    c->base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(m) + kChunk - 1) & ~(kChunk - 1));
    for (uint32_t i = 0; i < kPagesPerChunk; ++i) c->pages[i].base = c->base + i * kPage;
    for (uint32_t i = kPagesPerChunk; i-- > 0;) h.free_pages.push_back(&c->pages[i]);
    chunk_insert(h, c);
    h.chunk_list.push_back(c);
    return true;
}

Page* take_page(Heap& h, uint32_t cls) {
    if (h.free_pages.empty() && !new_chunk(h)) return nullptr;
    Page* p = h.free_pages.back();
    h.free_pages.pop_back();
    p->cls = uint16_t(cls);
    p->cell = kClasses[cls];
    p->ncells = uint32_t(kPage / p->cell);
    p->hint = 0;
    p->mark_epoch = 0;
    p->pending = false;
    std::memset(p->release, 0, sizeof p->release);
    std::memset(p->doomed, 0, sizeof p->doomed);
    // Taken while marking: swept again after the cycle, so what is allocated past the end of the
    // marking is judged by the next one rather than kept for a cycle more.
    p->swept_cycle = h.marking ? h.cycle - 1 : h.cycle;
    for (uint32_t w = 0; w < kWords; ++w) p->alloc[w] = pad(p, w);
    h.classes[cls].push_back(p);
    h.st.heap_bytes += kPage;
    return p;
}

// Cut a page's allocation bits to the last collection's marks.
void sweep(Heap& h, Page* p) {
    if (p->swept_cycle == h.cycle) return;
    const bool marked = p->mark_epoch == h.epoch;
    for (uint32_t w = 0; w < kWords; ++w) p->alloc[w] = (p->alloc[w] & (marked ? p->mark[w] : 0)) | p->doomed[w] | pad(p, w);
    p->hint = 0;
    p->swept_cycle = h.cycle;
}

bool page_empty(const Page* p) {
    for (uint32_t w = 0; w < kWords; ++w)
        if (p->alloc[w] != pad(p, w)) return false;
    return true;
}

// Cells [first, last) of p allocated while marking: marked now (allocated black), so the sweep after
// this cycle keeps them although nothing scanned them. Their fields start zero, and every reference
// stored into them later either is to a snapshot object, which the barrier preserves, or is new.
void mark_black(Heap& h, Page* p, uint32_t first, uint32_t last) {
    if (p->mark_epoch != h.epoch) {
        std::memset(p->mark, 0, sizeof p->mark);
        p->mark_epoch = h.epoch;
    }
    set_range(p->mark, first, last);
    h.marked_bytes += uint64_t(last - first) * p->cell;
}

uint8_t* alloc_small(Heap& h, OgcMutator& m, uint32_t cls) {
    OgcRegion& r = m.regions[cls];
    if (uintptr_t(r.limit) - uintptr_t(r.next) >= kClasses[cls]) {   // an allocation the inline path does not take (a vector, a call from C)
        uint8_t* cell = r.next;
        r.next += kClasses[cls];
        return cell;
    }
    Vec<Page*>& pages = h.classes[cls];
    for (uint32_t& k = h.cursor[cls];; ++k) {
        if (k == pages.size() && !take_page(h, cls)) return nullptr;
        Page* p = pages[k];
        // While marking, every page is already swept for the epoch before (the cycle's start swept
        // them all) and this epoch's marks are not complete: sweeping now would free live objects.
        if (!h.marking) sweep(h, p);
        for (uint32_t w = p->hint; w < kWords; ++w) {
            const uint64_t freebits = ~p->alloc[w];
            if (!freebits) continue;
            // The whole run of free cells from the first one: this request takes its first cell and,
            // unless every allocation must reach the collector, the rest becomes the class's region.
            const uint32_t first = w * 64 + ctz64(freebits);
            const uint32_t last = h.stress || h.step_stress ? first + 1 : next_set(p->alloc, first, p->ncells);
            set_range(p->alloc, first, last);
            if (h.marking) mark_black(h, p, first, last);
            p->hint = (last - 1) / 64;
            uint8_t* cell = p->base + first * p->cell;
            std::memset(cell, 0, size_t(last - first) * p->cell);
            h.since += uint64_t(last - first - 1) * p->cell;
            r = OgcRegion{cell + p->cell, p->base + last * p->cell};
            return cell;
        }
        p->hint = kWords;
    }
}

// The cell holding address `a`, or nullptr when `a` is not inside an allocated object.
uint8_t* find(Heap& h, uintptr_t a, const Page** page_out, Large** large_out) {
    const uintptr_t base = a & ~(kChunk - 1);
    Chunk* chunk = nullptr;
    if (base == h.last_chunk) {
        chunk = h.last;
    } else if (Chunk* c = chunk_at(h, base)) {
        chunk = c;
        h.last_chunk = base;
        h.last = chunk;
    }
    if (chunk) {
        Page* p = &chunk->pages[(a & (kChunk - 1)) / kPage];
        if (p->cls == kFree) return nullptr;
        const uint32_t i = uint32_t((a - reinterpret_cast<uintptr_t>(p->base)) / p->cell);
        if (i >= p->ncells || !((p->alloc[i / 64] >> (i % 64)) & 1)) return nullptr;
        *page_out = p;
        return p->base + i * p->cell;
    }
    // Large objects: the last one starting at or below `a`.
    uint32_t lo = 0, hi = h.large.size();
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (reinterpret_cast<uintptr_t>(h.large[mid]->base) <= a) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return nullptr;
    Large* l = h.large[lo - 1];
    if (a >= reinterpret_cast<uintptr_t>(l->base) + l->size) return nullptr;
    *large_out = l;
    return l->base;
}

uint32_t payload_offset(const OgcTypeDesc* d) { return d->align > 8 && !d->vec ? 16 : 8; }

void mark_word(Heap& h, uintptr_t a) {
    if (a < 4096 || (a & 7)) return;
    const Page* p = nullptr;
    Large* l = nullptr;
    uint8_t* cell = find(h, a, &p, &l);
    if (!cell) return;
    if (l) {
        if (l->mark_epoch == h.epoch) return;
        l->mark_epoch = h.epoch;
    } else {
        Page* mp = const_cast<Page*>(p);
        if (mp->mark_epoch != h.epoch) {
            std::memset(mp->mark, 0, sizeof mp->mark);
            mp->mark_epoch = h.epoch;
        }
        const uint32_t i = uint32_t((cell - mp->base) / mp->cell);
        uint64_t& w = mp->mark[i / 64];
        const uint64_t bit = uint64_t(1) << (i % 64);
        if (w & bit) return;
        w |= bit;
        h.marked_bytes += mp->cell;
    }
    const Header* hd = reinterpret_cast<const Header*>(cell);
    const OgcTypeDesc* d = h.descs[hd->desc];
    if (d->nrefs) h.stack.push_back(Grey{cell + (l ? 16 : payload_offset(d)), d, 0});
}

OGC_NO_ASAN void scan_range(Heap& h, const uint8_t* lo, const uint8_t* hi) {
    for (const uint8_t* q = reinterpret_cast<const uint8_t*>((reinterpret_cast<uintptr_t>(lo) + 7) & ~uintptr_t(7)); q + 8 <= hi; q += 8) {
        uintptr_t w;
        std::memcpy(&w, q, 8);
        mark_word(h, w);
    }
}

// Scans up to `budget` mark-stack entries; answers whether the stack is empty.
bool drain(Heap& h, uint64_t budget = UINT64_MAX) {
    for (; budget && !h.stack.empty(); --budget) {
        const Grey g = h.stack.back();
        h.stack.pop_back();
        uint8_t* payload = g.payload;
        const OgcTypeDesc* d = g.desc;
        if (!h.stack.empty()) __builtin_prefetch(h.stack.back().payload);
        if (!d->vec) {
            for (uint32_t k = 0; k < d->nrefs; ++k) {
                uintptr_t w;
                std::memcpy(&w, payload + d->refs[k], 8);
                mark_word(h, w);
            }
            continue;
        }
        uint64_t len;
        std::memcpy(&len, payload, 8);
        uint64_t end = len;
        if (len - g.from > kSlice) {
            end = g.from + kSlice;
            h.stack.push_back(Grey{payload, d, end});
        }
        const uint8_t* e = payload + 8 + g.from * d->size;
        for (uint64_t i = g.from; i < end; ++i, e += d->size)
            for (uint32_t k = 0; k < d->nrefs; ++k) {
                uintptr_t w;
                std::memcpy(&w, e + d->refs[k], 8);
                mark_word(h, w);
            }
    }
    return h.stack.empty();
}

// Every page swept for the epoch ending, empty pages returned to the pool, unmarked large objects
// unmapped: what the next epoch's marking may forget.
void finish_epoch(Heap& h) {
    for (uint32_t c = 0; c < kNumClasses; ++c) {
        Vec<Page*>& pages = h.classes[c];
        uint32_t keep = 0;
        for (uint32_t k = 0; k < pages.size(); ++k) {
            Page* p = pages[k];
            sweep(h, p);
            if (page_empty(p)) {
                p->cls = kFree;
                h.free_pages.push_back(p);
                h.st.heap_bytes -= kPage;
            } else {
                pages[keep++] = p;
            }
        }
        pages.resize(keep);
        h.cursor[c] = 0;
    }
    uint32_t keep = 0;
    for (uint32_t i = 0; i < h.large.size(); ++i) {
        Large* l = h.large[i];
        if (l->mark_epoch == h.epoch || l->doomed) {
            h.large[keep++] = l;
        } else {
            h.st.heap_bytes -= l->size;
            h.mapped -= l->size;
            os_unmap(l->base, l->size);
            delete l;
        }
    }
    h.large.resize(keep);
}

OGC_NO_ASAN void scan_stack(Heap& h) {
    jmp_buf regs;
    setjmp(regs);   // the callee-saved registers, wherever the caller's frames left them
    scan_range(h, reinterpret_cast<const uint8_t*>(&regs), reinterpret_cast<const uint8_t*>(&regs) + sizeof regs);
    volatile uint8_t here = 0;
    scan_range(h, const_cast<const uint8_t*>(&here), stack_top());
}

void release_chunks(Heap& h) {
    // Chunks with every page free go back to the OS, but one stays mapped so a heap oscillating at
    // a chunk boundary does not map and unmap on every collection.
    uint32_t free_chunks = 0;
    Vec<Chunk*> kept;
    for (Chunk* c : h.chunk_list) {
        bool all = true;
        for (const Page& p : c->pages) all = all && p.cls == kFree;
        if (all && free_chunks++ > 0) {
            chunk_erase(h, reinterpret_cast<uintptr_t>(c->base));
            if (h.last == c) { h.last = nullptr; h.last_chunk = 1; }
            h.mapped -= c->mapping_size;
            os_unmap(c->mapping, c->mapping_size);
            delete c;
        } else {
            kept.push_back(c);
        }
    }
    if (kept.size() == h.chunk_list.size()) return;
    h.chunk_list = kept;
    h.free_pages.clear();
    for (Chunk* c : h.chunk_list)
        for (uint32_t i = kPagesPerChunk; i-- > 0;)
            if (c->pages[i].cls == kFree) h.free_pages.push_back(&c->pages[i]);
}

void scan_roots(Heap& h) {
    for (const OgcRootRange& r : h.roots) scan_range(h, static_cast<const uint8_t*>(r.base), static_cast<const uint8_t*>(r.base) + r.bytes);
    for (void* o : h.handles) mark_word(h, reinterpret_cast<uintptr_t>(o));
    scan_stack(h);
}

void clean_cards(Heap& h) { std::memset(h.cards, 0xFF, size_t(1) << OGC_MUTATOR_CARD_BITS); }

// The reference words of the object at `payload` that lie in [lo, hi), marked.
void scan_part(Heap& h, uint8_t* payload, const OgcTypeDesc* d, const uint8_t* lo, const uint8_t* hi) {
    if (!d->nrefs) return;
    auto word = [&](const uint8_t* q) {
        if (q < lo || q >= hi) return;
        uintptr_t w;
        std::memcpy(&w, q, 8);
        mark_word(h, w);
    };
    if (!d->vec) {
        for (uint32_t k = 0; k < d->nrefs; ++k) word(payload + d->refs[k]);
        return;
    }
    uint64_t len;
    std::memcpy(&len, payload, 8);
    const uint8_t* e0 = payload + 8;
    if (!d->size || hi <= e0) return;
    uint64_t i = lo > e0 ? uint64_t(lo - e0) / d->size : 0;
    const uint64_t end = (uint64_t(hi - e0) + d->size - 1) / d->size;
    for (; i < len && i < end; ++i)
        for (uint32_t k = 0; k < d->nrefs; ++k) word(e0 + i * d->size + d->refs[k]);
}

// A minor collection's other roots: the reference words of old objects in dirty cards, which is
// where every old-to-young edge made since the last collection is (the barrier dirtied the card of
// the word it overwrote, initialising writes included).
void scan_cards(Heap& h) {
    constexpr uintptr_t kCard = uintptr_t(1) << OGC_MUTATOR_CARD_SHIFT;
    for (Chunk* c : h.chunk_list)
        for (Page& p : c->pages) {
            if (p.cls == kFree || p.mark_epoch != h.epoch) continue;   // no old object on it
            for (uintptr_t lo = 0; lo < kPage; lo += kCard) {
                if (h.cards[ogc_mutator_card_index(reinterpret_cast<uintptr_t>(p.base) + lo)] != 0) continue;
                const uint32_t first = uint32_t(lo / p.cell);
                uint32_t last = uint32_t((lo + kCard - 1) / p.cell);
                if (last >= p.ncells) last = p.ncells - 1;
                for (uint32_t i = first; i <= last; ++i) {
                    const uint64_t bit = uint64_t(1) << (i % 64);
                    if (!(p.alloc[i / 64] & bit) || !(p.mark[i / 64] & bit)) continue;
                    uint8_t* cell = p.base + uint64_t(i) * p.cell;
                    const Header* hd = reinterpret_cast<const Header*>(cell);
                    const OgcTypeDesc* d = h.descs[hd->desc];
                    scan_part(h, cell + payload_offset(d), d, p.base + lo, p.base + lo + kCard);
                }
            }
        }
    for (Large* l : h.large) {
        if (l->mark_epoch != h.epoch) continue;
        const Header* hd = reinterpret_cast<const Header*>(l->base);
        const OgcTypeDesc* d = h.descs[hd->desc];
        if (!d->nrefs) continue;
        for (uintptr_t lo = 0; lo < l->size; lo += kCard)
            if (h.cards[ogc_mutator_card_index(reinterpret_cast<uintptr_t>(l->base) + lo)] == 0)
                scan_part(h, l->base + 16, d, l->base + lo, l->base + lo + kCard);
    }
}

// At the end of a collection every allocated cell is marked or dead (nothing was allocated
// unmarked since the cycle began), so an unmarked object with a release hook is doomed: its cell
// stays allocated, the sweep keeps it, and finalize() runs the hook later. A page joins `pending`
// at most once.
void doom(Heap& h) {
    uint32_t keep = 0;
    for (uint32_t k = 0; k < h.release_pages.size(); ++k) {
        Page* p = h.release_pages[k];
        if (p->cls == kFree || !p->has_release) { p->has_release = false; continue; }
        h.release_pages[keep++] = p;
        const bool marked = p->mark_epoch == h.epoch;
        uint64_t any = 0;
        for (uint32_t w = 0; w < kWords; ++w) {
            const uint64_t dead = p->alloc[w] & p->release[w] & ~(marked ? p->mark[w] : 0) & ~p->doomed[w];
            p->doomed[w] |= dead;
            h.pending_count += popcount64(dead);
            any |= dead;
        }
        if (any && !p->pending) {
            p->pending = true;
            h.pending.push_back(p);
        }
    }
    h.release_pages.resize(keep);
    for (Large* l : h.large) {
        if (l->doomed || l->mark_epoch == h.epoch) continue;
        if (!h.descs[reinterpret_cast<const Header*>(l->base)->desc]->release) continue;
        l->doomed = true;
        h.pending_count++;
    }
}

void account(Heap& h) {
    doom(h);
    uint64_t large_live = 0;
    for (const Large* l : h.large)
        if (l->mark_epoch == h.epoch) large_live += l->size;
    h.live = h.marked_bytes + large_live;
    h.st.live_bytes = h.live;
    h.st.collections++;
    h.since = 0;
    // Pages the allocator passed while marking (unswept then) are swept from the start again.
    for (uint32_t& c : h.cursor) c = 0;
}

// A region's unused cells are allocated but unmarked: the sweep that follows may free them.
void empty_regions(Heap& h) {
    for (OgcMutator* m : h.mutators)
        for (OgcRegion& r : m->regions) r = OgcRegion{nullptr, nullptr};
}

// A major cycle: begin marks from the roots as they are now (the snapshot), steps scan the mark
// stack, and finish ends the epoch. Between begin and finish the mutator runs with the write barrier
// on: a reference word overwritten outside a frame has its old referent shaded, and new objects are
// allocated marked, so every object reachable at begin is marked by the end (the snapshot property)
// and the stacks need no second scan. Nothing is swept while marking.
void begin(Heap& h) {
    empty_regions(h);
    finish_epoch(h);
    release_chunks(h);
    ++h.epoch;
    ++h.cycle;
    h.marked_bytes = 0;
    scan_roots(h);
    h.marking = true;
    h.marking_flag = 1;
}

void finish(Heap& h) {
    h.marking = false;
    h.marking_flag = 0;
    // Every object is now old or garbage, so no card holds an old-to-young edge: an edge to an object
    // allocated from here on is made by a store from here on, which dirties its card then.
    clean_cards(h);
    account(h);
    h.major_at = 2 * h.live > 2 * kMinTrigger ? 2 * h.live : 2 * kMinTrigger;
    // A hook may remove itself (or another) while running: walk a copy.
    if (!h.hooks.empty()) {
        const Vec<Hook> hooks = h.hooks;
        for (const Hook& e : hooks) e.fn(e.user);
    }
}

// Ends a major cycle under way, or runs a whole one.
void collect(Heap& h) {
    if (h.collecting) return;
    h.collecting = true;
    if (!h.marking) begin(h);
    drain(h);
    finish(h);
    h.collecting = false;
}

// A minor collection, all at once: young objects reachable from the roots or from old objects in
// dirty cards are marked, which makes them old. Old objects are not traced and not freed.
void minor(Heap& h) {
    if (h.collecting) return;
    if (h.marking) { collect(h); return; }
    h.collecting = true;
    empty_regions(h);
    finish_epoch(h);
    release_chunks(h);
    ++h.cycle;
    scan_roots(h);
    scan_cards(h);
    drain(h);
    clean_cards(h);
    account(h);
    h.st.minors++;
    h.collecting = false;
}

// Up to `entries` mark-stack entries of a major cycle, starting one when none is under way; answers
// whether this call finished a cycle.
bool step(Heap& h, uint64_t entries) {
    if (h.collecting) return false;
    h.collecting = true;
    if (!h.marking) begin(h);
    const bool done = drain(h, entries);
    if (done) finish(h);
    h.collecting = false;
    return done;
}

// What allocation does before taking `bytes`: a collection when the nursery is full (major when the
// old generation has outgrown its budget), or the stress modes.
void before_alloc(Heap& h, uint64_t bytes) {
    if (!h.automatic) return;
    if (!h.stress && !h.step_stress && h.since + bytes <= h.trigger) return;
    opengc::heap_detail::MutatorStack mutator_stack;
    if (h.stress == 1) { collect(h); return; }
    if (h.stress == 2) {
        // Generational stress: a minor before every allocation, and every 16th starts a major cycle
        // that then advances one mark-stack entry per allocation, so old, young, black and barrier
        // paths all run under the corpus.
        if (h.marking) { step(h, 1); return; }
        if (++h.stress_count % 16 == 0) step(h, 1);
        else minor(h);
        return;
    }
    if (h.since + bytes > h.trigger) {
        if (h.marking || h.live > h.major_at) collect(h);
        else minor(h);
    } else if (h.step_stress) {
        step(h, h.step_stress);
    }
}

struct FindWalkObject {
    opengc::heap_detail::WalkObject operator()(Heap& h, uintptr_t address) const {
        const Page* page = nullptr; Large* large = nullptr;
        uint8_t* cell = find(h, address, &page, &large);
        if (!cell) return {nullptr, nullptr};
        const OgcTypeDesc* d = h.descs[reinterpret_cast<const Header*>(cell)->desc];
        return d->id ? opengc::heap_detail::WalkObject{cell + (large ? 16 : payload_offset(d)), d}
                     : opengc::heap_detail::WalkObject{nullptr, nullptr};
    }
};
using Walker = opengc::heap_detail::Walker<Heap, FindWalkObject>;

int walk_heap(OgcHeap* instance, const OgcWalk* walk, bool roots) {
    if (!instance || !walk) return OGC_HEAP_INVALID_ARGUMENT;
    Heap& h = heap(instance);
    if (!(h.capabilities & OGC_HEAP_WALK)) return OGC_HEAP_UNSUPPORTED;
    Walker w{h, *walk, {}, {}};
    for (size_t i = 0; i < walk->nwords; ++i) w.from(walk->words[i]);
    for (size_t i = 0; i < walk->nranges; ++i) w.from_range(walk->ranges[i].base, walk->ranges[i].bytes);
    if (walk->handles || roots)
        for (void* o : h.handles) w.reach(reinterpret_cast<uintptr_t>(o));
    if (roots)
        for (const auto& r : h.roots) w.from_range(r.base, r.bytes);
    if (walk->stack || roots) w.from_stack();
    w.run();
    return OGC_HEAP_OK;
}

}  // namespace

extern "C" {

const char* ogc_heap_provider(void) { return OGC_MUTATOR_PROVIDER; }

uint64_t ogc_heap_supported_capabilities(void) {
    return OGC_MUTATOR_CAPABILITIES;
}

uint64_t ogc_heap_capabilities(const OgcHeap* instance) { return instance ? heap(instance).capabilities : 0; }

OgcHeap* ogc_heap_create(const OgcHeapConfig* config) {
    const uint64_t supported = ogc_heap_supported_capabilities();
    if (config && (config->abi_version != OGC_HEAP_ABI_VERSION || config->struct_size != sizeof(OgcHeapConfig) ||
        (config->disabled_capabilities & ~(OGC_HEAP_RETAG | OGC_HEAP_WALK)) ||
        (config->required_capabilities & ~(supported & ~config->disabled_capabilities)))) return nullptr;
    OgcHeap* instance = new (std::nothrow) OgcHeap{};
    Heap* h = new (std::nothrow) Heap;
    if (!instance || !h) { delete instance; delete h; return nullptr; }
    h->cards = static_cast<uint8_t*>(std::malloc(size_t(1) << OGC_MUTATOR_CARD_BITS));
    if (!h->cards) { delete instance; delete h; return nullptr; }
    h->capabilities = supported & ~(config ? config->disabled_capabilities : 0);
    h->limit = config ? config->max_bytes : 0;
    h->descs.push_back(new_desc());
    clean_cards(*h);
    instance->state = h;
    return instance;
}

int ogc_heap_destroy(OgcHeap* instance) {
    if (!instance) return OGC_HEAP_INVALID_ARGUMENT;
    Heap& h = heap(instance);
    if (h.destroying || h.collecting || h.finalizing || !h.mutators.empty() || !h.roots.empty() || !h.hooks.empty() ||
        h.handles.size() != h.free_handles.size()) return OGC_HEAP_BUSY;
    h.destroying = true;
    // Shutdown is quiescent: release every remaining resource once before releasing its storage.
    for (Chunk* c : h.chunk_list) {
        for (Page& page : c->pages) {
            if (page.cls == kFree) continue;
            for (uint32_t i = 0; i < page.ncells; ++i) {
                const uint64_t bit = uint64_t(1) << (i % 64);
                if (!(page.alloc[i / 64] & page.release[i / 64] & bit)) continue;
                uint8_t* cell = page.base + uint64_t(i) * page.cell;
                const OgcTypeDesc* d = h.descs[reinterpret_cast<const Header*>(cell)->desc];
                if (d->release) d->release(cell + payload_offset(d));
            }
        }
        os_unmap(c->mapping, c->mapping_size);
        delete c;
    }
    for (Large* l : h.large) {
        const OgcTypeDesc* d = h.descs[reinterpret_cast<const Header*>(l->base)->desc];
        if (d->release) d->release(l->base + 16);
        os_unmap(l->base, l->size);
        delete l;
    }
    for (uint32_t i = 0; i < h.descs.size(); ++i) {
        OgcTypeDesc* d = h.descs[i];
        if (i) std::free(const_cast<char*>(d->name));
        std::free(const_cast<uint32_t*>(d->refs));
        std::free(d);
    }
    std::free(h.cards);
    delete &h;
    delete instance;
    return OGC_HEAP_OK;
}

OgcHeapBarrier ogc_heap_barrier(OgcHeap* instance) {
    if (!instance) return {nullptr, nullptr};
    Heap& h = heap(instance);
    return {&h.marking_flag, h.cards};
}

void ogc_heap_set_stress(OgcHeap* instance, uint32_t stress, uint32_t steps) {
    Heap& h = heap(instance);
    h.stress = stress;
    h.step_stress = steps;
}

const OgcTypeDesc* ogc_heap_intern(OgcHeap* instance, uint32_t size, uint32_t align, uint8_t vec, const uint32_t* refs, uint32_t nrefs,
                                   void (*release)(void*), const char* name) {
    Heap& h = heap(instance);
    if (!name) name = "";
    align = align > 8 ? 16 : 8;
    for (OgcTypeDesc* d : h.descs) {
        if (d->size != size || d->align != align || d->vec != vec || d->nrefs != nrefs || d->release != release ||
            std::strcmp(d->name, name) != 0)
            continue;
        bool same = true;
        for (uint32_t i = 0; i < nrefs && same; ++i) same = d->refs[i] == refs[i];
        if (same) return d;
    }
    OgcTypeDesc* d = new_desc();
    d->id = h.descs.size();
    d->size = size;
    d->align = align;
    d->vec = vec;
    d->release = release;
    const size_t nl = std::strlen(name);
    char* copy = static_cast<char*>(std::malloc(nl + 1));
    uint32_t* rs = nrefs ? static_cast<uint32_t*>(std::malloc(sizeof(uint32_t) * nrefs)) : nullptr;
    if (!copy || (nrefs && !rs)) metadata_exhausted();
    std::memcpy(copy, name, nl + 1);
    if (nrefs) std::memcpy(rs, refs, sizeof(uint32_t) * nrefs);
    d->name = copy;
    d->refs = rs;
    d->nrefs = nrefs;
    h.descs.push_back(d);
    return d;
}

const OgcTypeDesc* ogc_heap_desc_of(OgcHeap* instance, const void* payload) {
    if (!instance || !payload) return nullptr;
    Heap& h = heap(instance);
    const Page* p = nullptr; Large* l = nullptr;
    uint8_t* cell = find(h, reinterpret_cast<uintptr_t>(payload), &p, &l);
    if (!cell) return nullptr;
    const uint32_t id = reinterpret_cast<const Header*>(cell)->desc;
    if (!id || id >= h.descs.size()) return nullptr;
    const OgcTypeDesc* d = h.descs[id];
    return cell + (l ? 16 : payload_offset(d)) == payload ? d : nullptr;
}

void* ogc_heap_alloc(OgcHeap* instance, OgcMutator* m, const OgcTypeDesc* d, uint64_t count) {
    Heap& h = heap(instance);
    if (h.destroying || !m || m->heap != instance || !d || d->id >= h.descs.size() || h.descs[d->id] != d) return nullptr;
    uint64_t payload = d->size;
    if (d->vec) {
        if (d->size && count > (uint64_t(1) << 40) / d->size) return nullptr;
        payload = 8 + count * d->size;
    }
    const uint64_t bytes = payload_offset(d) + payload;
    before_alloc(h, bytes);
    h.since += bytes;
    h.st.allocated += bytes;
    uint8_t* cell = nullptr;
    uint8_t* obj = nullptr;
    if (bytes <= kMaxSmall) {
        cell = alloc_small(h, *m, size_class(uint32_t(bytes)));
        if (!cell) return nullptr;
        obj = cell + payload_offset(d);
        if (d->release) {
            const Page* p = nullptr;
            Large* none = nullptr;
            find(h, reinterpret_cast<uintptr_t>(cell), &p, &none);
            Page* mp = const_cast<Page*>(p);
            const uint32_t i = uint32_t((cell - mp->base) / mp->cell);
            mp->release[i / 64] |= uint64_t(1) << (i % 64);
            if (!mp->has_release) {
                mp->has_release = true;
                h.release_pages.push_back(mp);
            }
        }
    } else {
        const size_t size = size_t((16 + payload + 16383) & ~uint64_t(16383));
        if (h.limit && (size > h.limit || h.mapped > h.limit - size)) return nullptr;
        cell = static_cast<uint8_t*>(os_map(size));
        if (!cell) return nullptr;
        Large* l = new (std::nothrow) Large{cell, size, 0, false};
        if (!l) {
            os_unmap(cell, size);
            return nullptr;
        }
        // Allocated after this epoch's marking: it survives until the next collection decides.
        l->mark_epoch = h.epoch;
        uint32_t at = h.large.size();
        h.large.push_back(l);
        for (; at > 0 && h.large[at - 1]->base > cell; --at) h.large[at] = h.large[at - 1];
        h.large[at] = l;
        h.mapped += size;
        h.st.heap_bytes += size;
        obj = cell + 16;
    }
    Header hd{d->id, 0};
    std::memcpy(cell, &hd, sizeof hd);
    if (d->vec) std::memcpy(obj, &count, 8);
    return obj;
}

int ogc_heap_mutator_register(OgcHeap* instance, OgcMutator* m) {
    if (!instance || !m || m->heap) return OGC_HEAP_INVALID_ARGUMENT;
    Heap& h = heap(instance);
    if (h.destroying) return OGC_HEAP_BUSY;
    for (OgcRegion& r : m->regions) r = OgcRegion{nullptr, nullptr};
    h.mutators.push_back(m);
    m->heap = instance;
    return OGC_HEAP_OK;
}

int ogc_heap_mutator_unregister(OgcHeap* instance, OgcMutator* m) {
    if (!instance || !m || m->heap != instance) return OGC_HEAP_INVALID_ARGUMENT;
    Vec<OgcMutator*>& v = heap(instance).mutators;
    for (uint32_t i = 0; i < v.size(); ++i) if (v[i] == m) {
        v[i] = v.back(); v.pop_back();
        for (OgcRegion& r : m->regions) r = OgcRegion{nullptr, nullptr};
        m->heap = nullptr;
        return OGC_HEAP_OK;
    }
    return OGC_HEAP_INVALID_ARGUMENT;
}

int ogc_heap_collect(OgcHeap* instance) {
    opengc::heap_detail::MutatorStack mutator_stack;
    if (!instance) return OGC_HEAP_INVALID_ARGUMENT;
    Heap& h = heap(instance);
    if (!(h.capabilities & OGC_HEAP_COLLECTS)) return OGC_HEAP_UNSUPPORTED;
    if (h.collecting || h.destroying) return OGC_HEAP_BUSY;
    collect(h); return OGC_HEAP_OK;
}
int ogc_heap_minor(OgcHeap* instance) {
    opengc::heap_detail::MutatorStack mutator_stack;
    if (!instance) return OGC_HEAP_INVALID_ARGUMENT;
    Heap& h = heap(instance);
    if (!(h.capabilities & OGC_HEAP_GENERATIONAL)) return OGC_HEAP_UNSUPPORTED;
    if (h.collecting || h.destroying) return OGC_HEAP_BUSY;
    minor(h); return OGC_HEAP_OK;
}

OgcHeapStepResult ogc_heap_step(OgcHeap* instance, uint64_t budget_us) {
    opengc::heap_detail::MutatorStack mutator_stack;
    if (!instance) return {OGC_HEAP_INVALID_ARGUMENT, 0};
    Heap& h = heap(instance);
    if (!(h.capabilities & OGC_HEAP_INCREMENTAL)) return {OGC_HEAP_UNSUPPORTED, 0};
    if (h.collecting || h.destroying) return {OGC_HEAP_BUSY, 0};
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(budget_us);
    do { if (step(h, 256)) return {OGC_HEAP_OK, 1}; } while (std::chrono::steady_clock::now() < end);
    return {OGC_HEAP_OK, 0};
}

int ogc_heap_marking(OgcHeap* instance) { return heap(instance).marking ? 1 : 0; }
void ogc_heap_set_automatic(OgcHeap* instance, int on) { heap(instance).automatic = on != 0; }
int ogc_heap_automatic(OgcHeap* instance) { return heap(instance).automatic ? 1 : 0; }

void ogc_heap_shade(OgcHeap* instance, void* const* word) {
    Heap& h = heap(instance);
    if (h.marking) mark_word(h, reinterpret_cast<uintptr_t>(*word));
}

uint64_t ogc_heap_finalize(OgcHeap* instance, uint64_t budget_us) {
    Heap& h = heap(instance);
    if (h.collecting || h.destroying) return 0;
    ++h.finalizing;
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(budget_us);
    auto spent = [&] { return std::chrono::steady_clock::now() >= end; };
    uint64_t ran = 0;
    // A hook may allocate, so collect, so doom more: work from a copy, put back what is left.
    Vec<Page*> work;
    work.swap(h.pending);
    uint32_t k = 0;
    for (; k < work.size(); ++k) {
        Page* p = work[k];
        for (uint32_t w = 0; w < kWords; ++w)
            while (p->doomed[w]) {
                const uint32_t b = ctz64(p->doomed[w]);
                const uint64_t bit = uint64_t(1) << b;
                uint8_t* cell = p->base + uint64_t(w * 64 + b) * p->cell;
                const OgcTypeDesc* d = h.descs[reinterpret_cast<const Header*>(cell)->desc];
                p->doomed[w] &= ~bit;
                p->release[w] &= ~bit;
                p->alloc[w] &= ~bit;
                if (w < p->hint) p->hint = w;
                h.pending_count--;
                d->release(cell + payload_offset(d));
                ++ran;
            }
        p->pending = false;
        if (spent()) { ++k; break; }
    }
    for (; k < work.size(); ++k) h.pending.push_back(work[k]);
    for (uint32_t i = 0; i < h.large.size() && !spent();) {
        Large* l = h.large[i];
        if (!l->doomed) { ++i; continue; }
        const OgcTypeDesc* d = h.descs[reinterpret_cast<const Header*>(l->base)->desc];
        h.large.erase_at(i);
        h.pending_count--;
        d->release(l->base + 16);
        ++ran;
        h.st.heap_bytes -= l->size;
        h.mapped -= l->size;
        os_unmap(l->base, l->size);
        delete l;
    }
    --h.finalizing;
    return ran;
}

uint64_t ogc_heap_pending_releases(OgcHeap* instance) { return heap(instance).pending_count; }

int ogc_heap_add_roots(OgcHeap* instance, const void* base, size_t bytes) {
    if (!instance || (!base && bytes)) return OGC_HEAP_INVALID_ARGUMENT;
    if (heap(instance).destroying) return OGC_HEAP_BUSY;
    if (!bytes) return OGC_HEAP_OK;
    heap(instance).roots.push_back(OgcRootRange{base, bytes});
    return OGC_HEAP_OK;
}

void ogc_heap_remove_roots(OgcHeap* instance, const void* base) {
    Vec<OgcRootRange>& r = heap(instance).roots;
    for (uint32_t i = r.size(); i; --i)
        if (r[i - 1].base == base) { r.erase_at(i - 1); return; }
}

uint32_t ogc_heap_handle_new(OgcHeap* instance, void* obj) {
    Heap& h = heap(instance);
    if (h.destroying) return 0;
    if (!h.free_handles.empty()) {
        const uint32_t i = h.free_handles.back();
        h.free_handles.pop_back();
        h.handle_used[i] = 1;
        h.handles[i] = obj;   // the slot held null: nothing to shade
        return i + 1;
    }
    h.handles.push_back(obj);
    h.handle_used.push_back(1);
    return h.handles.size();
}

void* ogc_heap_handle_get(OgcHeap* instance, uint32_t handle) {
    Heap& h = heap(instance);
    return handle && handle <= h.handles.size() && h.handle_used[handle - 1] ? h.handles[handle - 1] : nullptr;
}

void ogc_heap_handle_set(OgcHeap* instance, uint32_t handle, void* obj) {
    Heap& h = heap(instance);
    if (!handle || handle > h.handles.size() || !h.handle_used[handle - 1]) return;
    if (h.marking) mark_word(h, reinterpret_cast<uintptr_t>(h.handles[handle - 1]));
    h.handles[handle - 1] = obj;
}

void ogc_heap_handle_free(OgcHeap* instance, uint32_t handle) {
    Heap& h = heap(instance);
    if (!handle || handle > h.handles.size() || !h.handle_used[handle - 1]) return;
    if (h.marking) mark_word(h, reinterpret_cast<uintptr_t>(h.handles[handle - 1]));
    h.handle_used[handle - 1] = 0;
    h.handles[handle - 1] = nullptr;
    h.free_handles.push_back(handle - 1);
}

int ogc_heap_objects_of(OgcHeap* instance, const OgcTypeDesc* d, void (*each)(void* user, void* payload), void* user) {
    Heap& h = heap(instance);
    if (!(h.capabilities & OGC_HEAP_RETAG)) return OGC_HEAP_UNSUPPORTED;
    if (!d || d->id >= h.descs.size() || h.descs[d->id] != d || !each) return OGC_HEAP_INVALID_ARGUMENT;
    for (Chunk* c : h.chunk_list)
        for (Page& p : c->pages) {
            if (p.cls == kFree || p.mark_epoch != h.epoch) continue;   // no mark in this epoch: nothing live
            for (uint32_t i = 0; i < p.ncells; ++i) {
                const uint64_t bit = uint64_t(1) << (i % 64);
                if (!(p.alloc[i / 64] & p.mark[i / 64] & bit)) continue;
                uint8_t* cell = p.base + uint64_t(i) * p.cell;
                if (reinterpret_cast<const Header*>(cell)->desc == d->id) each(user, cell + payload_offset(d));
            }
        }
    for (Large* l : h.large)
        if (l->mark_epoch == h.epoch && reinterpret_cast<const Header*>(l->base)->desc == d->id) each(user, l->base + 16);
    return OGC_HEAP_OK;
}

int ogc_heap_payload_room(OgcHeap* instance, const void* payload, uint64_t* bytes) {
    if (!instance || !bytes) return OGC_HEAP_INVALID_ARGUMENT;
    Heap& h = heap(instance);
    if (!(h.capabilities & OGC_HEAP_RETAG)) return OGC_HEAP_UNSUPPORTED;
    if (!ogc_heap_desc_of(instance, payload)) return OGC_HEAP_INVALID_ARGUMENT;
    const Page* p = nullptr; Large* l = nullptr;
    uint8_t* cell = find(h, reinterpret_cast<uintptr_t>(payload), &p, &l);
    *bytes = l ? l->size - 16 : p->cell - uint64_t(static_cast<const uint8_t*>(payload) - cell);
    return OGC_HEAP_OK;
}

int ogc_heap_retag(OgcHeap* instance, void* payload, const OgcTypeDesc* to) {
    uint64_t room = 0;
    const int status = ogc_heap_payload_room(instance, payload, &room);
    if (status != OGC_HEAP_OK) return status;
    Heap& h = heap(instance);
    const OgcTypeDesc* from = ogc_heap_desc_of(instance, payload);
    const Page* page = nullptr; Large* large = nullptr;
    uint8_t* cell = find(h, reinterpret_cast<uintptr_t>(payload), &page, &large);
    if (!to || to->id >= h.descs.size() || h.descs[to->id] != to || to->vec || to->size > room ||
        to->release || from->release || from->vec || (!large && payload_offset(to) != payload_offset(from))) return OGC_HEAP_INVALID_ARGUMENT;
    uint8_t* p = static_cast<uint8_t*>(payload);
    Header hd;
    std::memcpy(&hd, cell, sizeof hd);
    hd.desc = to->id;
    std::memcpy(cell, &hd, sizeof hd);
    std::memset(p, 0, room);
    return OGC_HEAP_OK;
}

int ogc_heap_walk(OgcHeap* instance, const OgcWalk* walk) {
    opengc::heap_detail::MutatorStack mutator_stack;
    return walk_heap(instance, walk, false);
}

int ogc_heap_walk_roots(OgcHeap* instance, const OgcWalk* walk) {
    opengc::heap_detail::MutatorStack mutator_stack;
    return walk_heap(instance, walk, true);
}

int ogc_heap_add_cycle_hook(OgcHeap* instance, void (*hook)(void*), void* user) {
    Heap& h = heap(instance);
    for (Hook& e : h.hooks)
        if (e.user == user) { e.fn = hook; return OGC_HEAP_OK; }
    h.hooks.push_back(Hook{hook, user});
    return OGC_HEAP_OK;
}

void ogc_heap_remove_cycle_hook(OgcHeap* instance, void* user) {
    Vec<Hook>& v = heap(instance).hooks;
    for (uint32_t i = 0; i < v.size(); ++i)
        if (v[i].user == user) { v.erase_at(i); return; }
}

OgcHeapStats ogc_heap_stats(OgcHeap* instance) { return heap(instance).st; }

}  // extern "C"
