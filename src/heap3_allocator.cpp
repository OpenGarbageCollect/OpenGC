// SPDX-License-Identifier: GPL-3.0-or-later
// One block registry. No per-object ownership list, prefix or address hash.
#include "heap3_internal.h"
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
namespace ogc_preset {
namespace {
AllocationClass* const deleted_class = reinterpret_cast<AllocationClass*>(uintptr_t(1));
uint64_t mix(uint64_t x) { x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9); x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb); return x ^ (x >> 31); }
size_t class_hash(const OgcPortableType* t, uint64_t count) { return size_t(mix(uintptr_t(t) ^ mix(count))); }
void set(uint64_t* map, uint32_t i) { map[i / 64] |= UINT64_C(1) << (i % 64); }
void clear(uint64_t* map, uint32_t i) { map[i / 64] &= ~(UINT64_C(1) << (i % 64)); }
template<bool occupied>
void set_range(uint64_t* map, uint32_t first, uint32_t count) {
    while (count) {
        const uint32_t bit = first % 64;
        const uint32_t length = count < 64 - bit ? count : 64 - bit;
        const uint64_t mask = (UINT64_MAX >> (64 - length)) << bit;
        if constexpr (occupied) map[first / 64] |= mask;
        else map[first / 64] &= ~mask;
        first += length;
        count -= length;
    }
}
uint64_t valid_bits(const Block* b, uint32_t w) {
    const uint32_t tail = b->capacity % 64;
    return tail && w + 1 == b->words ? (UINT64_C(1) << tail) - 1 : UINT64_MAX;
}
template<class F> void each_used(Block* b, F visit) {
    if (no_gc) { for (uint32_t i = 0; i < b->frontier; ++i) visit(i); return; }
    for (uint32_t w = 0; w < b->words; ++w) {
        uint64_t bits = b->used[w];
        while (bits) { const auto i = uint32_t(std::countr_zero(bits)); bits &= bits - 1; visit(w * 64 + i); }
    }
}
void available_add(Block* b) {
    if (b->available_link) return;
    auto* c = b->cls; b->available_next = c->available; b->available_link = &c->available;
    if (b->available_next) b->available_next->available_link = &b->available_next;
    c->available = b;
}
void available_remove(Block* b) {
    if (!b->available_link) return;
    *b->available_link = b->available_next;
    if (b->available_next) b->available_next->available_link = b->available_link;
    b->available_link = nullptr; b->available_next = nullptr;
}
void class_insert(AllocatorRegistry& s, AllocationClass* c) {
    size_t i = class_hash(c->type, c->count) & (s.class_capacity - 1);
    while (s.class_index[i] && s.class_index[i] != deleted_class) i = (i + 1) & (s.class_capacity - 1);
    if (!s.class_index[i]) ++s.class_occupied;
    s.class_index[i] = c; ++s.class_count;
}
bool class_reserve(AllocatorRegistry& s) {
    if (s.class_capacity && (s.class_occupied + 1) * 2 < s.class_capacity) return true;
    size_t cap = s.class_capacity ? s.class_capacity : 64;
    if ((s.class_count + 1) * 2 >= cap) {
        if (cap > SIZE_MAX / 2 / sizeof(AllocationClass*)) return false;
        cap *= 2;
    }
    auto** table = static_cast<AllocationClass**>(std::calloc(cap, sizeof(AllocationClass*)));
    if (!table) return false;
    auto** old = s.class_index; s.class_index = table; s.class_capacity = cap; s.class_count = s.class_occupied = 0;
    for (auto* c = s.classes; c; c = c->next) if (c->type->layout.is_array) class_insert(s, c);
    std::free(old); return true;
}
AllocationClass* allocation_class(OgcPortableHeap* h, const OgcPortableType* type, uint64_t count, uint64_t bytes) {
    auto* t = const_cast<OgcPortableType*>(type); auto& s = h->storage;
    if (!t->layout.is_array && t->fixed) return t->fixed;
    if (t->last && t->last->count == count) return t->last;
    if (t->layout.is_array && s.class_capacity) {
        size_t i = class_hash(t, count) & (s.class_capacity - 1);
        while (auto* c = s.class_index[i]) {
            if (c != deleted_class && c->type == t && c->count == count) { t->last = c; return c; }
            i = (i + 1) & (s.class_capacity - 1);
        }
    }
    if (t->layout.is_array && !class_reserve(s)) return nullptr;
    auto* c = static_cast<AllocationClass*>(std::calloc(1, sizeof(AllocationClass)));
    if (!c) return nullptr;
    c->type = t; c->count = count; c->bytes = bytes; c->stride = (size_t(bytes ? bytes : 1) + 15) & ~size_t(15);
    c->next_data = page_size; c->next = s.classes;
    if (c->next) c->next->previous = c;
    s.classes = c;
    if (t->layout.is_array) class_insert(s, c); else t->fixed = c;
    t->last = c; return c;
}
void destroy_class(AllocatorRegistry& s, AllocationClass* c) {
    auto* t = const_cast<OgcPortableType*>(c->type);
    if (t->last == c) t->last = nullptr;
    if (t->fixed == c) t->fixed = nullptr;
    if (t->layout.is_array) {
        size_t i = class_hash(t, c->count) & (s.class_capacity - 1);
        while (s.class_index[i] != c) i = (i + 1) & (s.class_capacity - 1);
        s.class_index[i] = deleted_class; --s.class_count;
    }
    if (c->previous) c->previous->next = c->next; else s.classes = c->next;
    if (c->next) c->next->previous = c->previous;
    std::free(c);
}
size_t page_hash(uintptr_t page) { return (page / page_size) * size_t(UINT64_C(11400714819323198485)); }
void page_insert(AllocatorRegistry& s, uintptr_t page, Block* b) {
    size_t i = page_hash(page) & (s.page_capacity - 1);
    while (s.pages[i].page > 1) i = (i + 1) & (s.page_capacity - 1);
    if (!s.pages[i].page) ++s.page_occupied;
    s.pages[i] = {page, b}; ++s.page_count;
}
bool page_reserve(AllocatorRegistry& s, size_t extra, size_t backing) {
    if (!extra) return !s.limit || (backing <= s.limit && s.mapped <= s.limit - backing);
    if (s.page_capacity && (s.page_occupied + extra + 1) * 2 < s.page_capacity)
        return !s.limit || (backing <= s.limit && s.mapped <= s.limit - backing);
    size_t cap = s.page_capacity ? s.page_capacity : 64;
    while ((s.page_count + extra + 1) * 2 >= cap) {
        if (cap > SIZE_MAX / 2 / sizeof(PageEntry)) return false;
        cap *= 2;
    }
    const size_t old_bytes = s.page_capacity * sizeof(PageEntry), bytes = cap * sizeof(PageEntry);
    if (s.limit && (backing > s.limit || bytes > s.limit - backing || s.mapped - old_bytes > s.limit - backing - bytes)) return false;
    auto* table = static_cast<PageEntry*>(std::calloc(cap, sizeof(PageEntry)));
    if (!table) return false;
    auto* old = s.pages; const size_t old_cap = s.page_capacity;
    s.pages = table; s.page_capacity = cap; s.page_count = s.page_occupied = 0;
    for (size_t i = 0; i < old_cap; ++i) if (old[i].page > 1) page_insert(s, old[i].page, old[i].block);
    s.mapped += bytes - old_bytes; std::free(old); return true;
}
void page_remove(AllocatorRegistry& s, uintptr_t page) {
    size_t i = page_hash(page) & (s.page_capacity - 1);
    while (s.pages[i].page != page) i = (i + 1) & (s.page_capacity - 1);
    s.pages[i] = {1, nullptr}; --s.page_count;
}
Block* range_merge(Block* a, Block* b) {
    if (!a) return b;
    if (!b) return a;
    if (a->range_priority < b->range_priority) { a->range_right = range_merge(a->range_right, b); return a; }
    b->range_left = range_merge(a, b->range_left); return b;
}
Block* range_insert(Block* root, Block* b) {
    if (!root) return b;
    if (uintptr_t(b->data) < uintptr_t(root->data)) {
        root->range_left = range_insert(root->range_left, b);
        if (root->range_left->range_priority < root->range_priority) {
            auto* n = root->range_left; root->range_left = n->range_right; n->range_right = root; return n;
        }
    } else {
        root->range_right = range_insert(root->range_right, b);
        if (root->range_right->range_priority < root->range_priority) {
            auto* n = root->range_right; root->range_right = n->range_left; n->range_left = root; return n;
        }
    }
    return root;
}
Block* range_remove(Block* root, Block* b) {
    if (root == b) return range_merge(root->range_left, root->range_right);
    if (uintptr_t(b->data) < uintptr_t(root->data)) root->range_left = range_remove(root->range_left, b);
    else root->range_right = range_remove(root->range_right, b);
    return root;
}
}
Block* locate_uncached(AllocatorRegistry& s, uintptr_t word) {
    Block* b = nullptr;
    if (s.page_capacity) {
        const uintptr_t page = word & ~uintptr_t(page_size - 1);
        size_t i = page_hash(page) & (s.page_capacity - 1);
        while (s.pages[i].page) {
            if (s.pages[i].page == page) { b = s.pages[i].block; break; }
            i = (i + 1) & (s.page_capacity - 1);
        }
    }
    if (!b) for (auto* n = s.range_root; n;) {
        if (word < uintptr_t(n->data)) n = n->range_left;
        else if (word - uintptr_t(n->data) >= n->data_bytes) n = n->range_right;
        else { b = n; break; }
    }
    if (b) {
        // Reference stores often alternate between the destination and the value's block.
        s.previous_block = s.cached_block;
        s.cached_block = b;
    }
    return b;
}
namespace {
void free_block(OgcPortableHeap* h, Block* b, AllocationClass* protect) {
    auto& s = h->storage; auto* c = b->cls;
    if (s.sweep == b) s.sweep = b->next;
    if (s.cached_block == b) s.cached_block = nullptr;
    if (s.previous_block == b) s.previous_block = nullptr;
    if (b->data_bytes <= 65536) for (size_t at = 0; at < b->data_bytes; at += page_size) page_remove(s, uintptr_t(b->data) + at);
    else s.range_root = range_remove(s.range_root, b);
    available_remove(b);
    if (c->scan == b) c->scan = b->type_next;
    *b->type_link = b->type_next;
    if (b->type_next) b->type_next->type_link = b->type_link;
    if (b->previous) b->previous->next = b->next; else s.blocks = b->next;
    if (b->next) b->next->previous = b->previous;
    if (b->resource_link) {
        *b->resource_link = b->resource_next;
        if (b->resource_next) b->resource_next->resource_link = b->resource_link;
    }
    s.mapped -= b->physical; std::free(b->identities); std::free(b->walk_links); std::free(b->maps); std::free(b->records); std::free(b->raw); std::free(b);
    if (!c->blocks && c != protect && c->type->layout.is_array) destroy_class(s, c);
}
void release_cell(Object o) {
    auto* b = o.block;
    clear(b->used, o.index); clear(b->pending, o.index); --b->used_count;
    if (o.index / 64 < b->free_word) b->free_word = o.index / 64;
    available_add(b);
}
void sweep_block(OgcPortableHeap* h, Block* b) {
    if (no_gc || b->type->layout.release) return;
    if (b->live_epoch < h->collector.completed_epoch) {
        // Every full-cycle survivor is enumerated or born black. No survivor in
        // this block: preserve stable side records and discard the occupied range.
        if (b->frontier) b->dirty_frontier = b->frontier;
        b->frontier = b->used_count = b->free_word = 0;
        std::memset(b->used, 0, size_t(b->words) * sizeof(uint64_t));
        available_add(b); return;
    }
    uint32_t removed = 0;
    for (uint32_t w = 0; w < b->words; ++w) {
        uint64_t bits = b->used[w], survivors = bits;
        while (bits) {
            const uint32_t i = uint32_t(std::countr_zero(bits)); bits &= bits - 1;
            if (!live(h, {b, w * 64 + i})) survivors &= ~(UINT64_C(1) << i);
        }
        const uint32_t dead = uint32_t(std::popcount(b->used[w] ^ survivors));
        if (dead && w < b->free_word) b->free_word = w;
        removed += dead; b->used[w] = survivors;
    }
    if (removed) {
        b->used_count -= removed;
        if (!b->used_count) {
            b->dirty_frontier = b->frontier;
            b->frontier = b->free_word = 0;
        }
        available_add(b);
    }
}
void trim_empty(OgcPortableHeap* h, AllocationClass* protect) {
    if (no_gc) return;
    for (auto* b = h->storage.blocks; b;) {
        auto* next = b->next; sweep_block(h, b);
        if (!b->used_count) free_block(h, b, protect);
        b = next;
    }
}
Block* make_block(OgcPortableHeap* h, AllocationClass* c) {
    auto& s = h->storage;
    // Bound cross-class reclamation per refill; do not retain dead array sizes forever.
    if (!no_gc) for (unsigned i = 0; i < 4 && s.sweep; ++i) {
        auto* b = s.sweep; s.sweep = b->next;
        sweep_block(h, b);
        if (!b->used_count) free_block(h, b, c);
    }
    if (c->stride > SIZE_MAX - (page_size - 1)) return nullptr;
    size_t data = c->next_data;
    if (c->stride > data) data = (c->stride + page_size - 1) & ~(page_size - 1);
    uint32_t capacity = uint32_t(data / c->stride);
    if (!capacity) return nullptr;
    // Under quotas use a smaller side table instead of requiring a full slab.
    bool trimmed = false;
    for (;;) {
        const size_t words = (size_t(capacity) + 63) / 64;
        const size_t map_bytes = words * sizeof(uint64_t) * (no_gc ? 1 : 3);
        const size_t walk_bytes = no_gc && c->stride - c->bytes < 2 * sizeof(OgcRef) ? size_t(capacity) * sizeof(OgcRef) : 0;
        const size_t identity_bytes = no_gc && c->stride - c->bytes < sizeof(uint64_t) ? size_t(capacity) * sizeof(uint64_t) : 0;
        const size_t meta_bytes = (no_gc ? 0 : size_t(capacity) * sizeof(Node)) + walk_bytes + identity_bytes;
        if (data > SIZE_MAX - page_size - sizeof(Block) - map_bytes - meta_bytes) return nullptr;
        const size_t physical = data + page_size - 1 + sizeof(Block) + map_bytes + meta_bytes;
        const size_t pages = data <= 65536 ? data / page_size : 0;
        if (page_reserve(s, pages, physical)) {
            auto* b = static_cast<Block*>(std::calloc(1, sizeof(Block)));
            if (!b) return nullptr;
            b->raw = std::calloc(1, data + page_size - 1);
            b->maps = static_cast<uint64_t*>(std::calloc(1, map_bytes));
            b->records = no_gc ? nullptr : static_cast<Node*>(std::calloc(capacity, sizeof(Node)));
            b->walk_links = walk_bytes ? static_cast<OgcRef*>(std::calloc(1, walk_bytes)) : nullptr;
            b->identities = identity_bytes ? static_cast<uint64_t*>(std::calloc(1, identity_bytes)) : nullptr;
            if (!b->raw || !b->maps || (!no_gc && !b->records) || (walk_bytes && !b->walk_links) || (identity_bytes && !b->identities)) {
                std::free(b->identities); std::free(b->walk_links); std::free(b->records);
                std::free(b->maps); std::free(b->raw); std::free(b); return nullptr;
            }
            b->data = reinterpret_cast<uint8_t*>((uintptr_t(b->raw) + page_size - 1) & ~uintptr_t(page_size - 1));
            b->cls = c; b->type = c->type; b->bytes = c->bytes; b->count = c->count;
            b->stride = c->stride; b->data_bytes = data; b->physical = physical; b->capacity = capacity; b->words = uint32_t(words);
            if (no_gc) b->seen = b->maps;
            else {
                b->used = b->maps; b->pending = b->used + words; b->seen = b->pending + words;
                for (uint32_t i = 0; i < capacity; ++i) {
                    b->records[i].gc.bytes = c->bytes; b->records[i].block = b;
                }
            }
            b->next = s.blocks; if (b->next) b->next->previous = b; s.blocks = b;
            b->type_next = c->blocks; b->type_link = &c->blocks;
            if (b->type_next) b->type_next->type_link = &b->type_next;
            c->blocks = b; available_add(b);
            if (c->type->layout.release) {
                b->resource_next = s.resources; b->resource_link = &s.resources;
                if (b->resource_next) b->resource_next->resource_link = &b->resource_next;
                s.resources = b;
            }
            if (pages) for (size_t at = 0; at < data; at += page_size) page_insert(s, uintptr_t(b->data) + at, b);
            else { b->range_priority = mix(uintptr_t(b->data)); s.range_root = range_insert(s.range_root, b); }
            s.mapped += physical;
            if (data < 65536 && capacity == data / c->stride) c->next_data = data * 2;
            return b;
        }
        if (!trimmed) { trim_empty(h, c); trimmed = true; continue; }
        if (data > page_size && c->stride <= data / 2) { data /= 2; capacity = uint32_t(data / c->stride); continue; }
        if (capacity > 1) { capacity /= 2; continue; }
        return nullptr;
    }
}
}
Object containing(OgcPortableHeap* h, uintptr_t word) {
    return find_object<false>(h, word);
}
namespace {
Block* allocation_block(OgcPortableHeap* h, const OgcPortableType* type, uint64_t count, uint64_t bytes) {
    auto* c = allocation_class(h, type, count, bytes); if (!c) return {};
    if (!c->available) {
        if (!no_gc && c->scan_generation != h->storage.generation) { c->scan = c->blocks; c->scan_generation = h->storage.generation; }
        while (!c->available && c->scan) { auto* b = c->scan; c->scan = b->type_next; sweep_block(h, b); }
        if (!c->available && !make_block(h, c)) {
            if (!c->blocks && c->type->layout.is_array) destroy_class(h->storage, c);
            return {};
        }
    }
    return c->available;
}
template<bool run = false>
void publish_cell(OgcPortableHeap* h, Block* b, uint32_t index) {
    const uint64_t identity = h->storage.next_identity++;
    if constexpr (no_gc) {
        if (b->identities) b->identities[index] = identity;
        else std::memcpy(b->data + size_t(index) * b->stride + b->stride - 8, &identity, 8);
    } else {
        if constexpr (!run) set(b->used, index);
        auto* n = &b->records[index];
        n->identity = identity;
        ogc_policy_record_allocated(&h->collector, &n->gc);
    }
}
void publish_allocation(OgcPortableHeap* h, Block* b, uint32_t count) {
    if constexpr (!no_gc) {
        b->live_epoch = h->collector.epoch;
        if (!h->collector.marking && b->nursery_generation != h->storage.generation) {
            b->nursery_generation = h->storage.generation;
            b->nursery_next = h->storage.nursery; h->storage.nursery = b;
        }
    }
    b->used_count += count;
    if (b->used_count == b->capacity) available_remove(b);
    const uint64_t charge = count * (b->stride + (no_gc ? 0 : sizeof(Node)));
    h->since += charge; h->statistics.allocated += charge;
    if constexpr (no_gc) h->statistics.live_bytes += count * ((b->bytes + 8 + 15) & ~UINT64_C(15));
}
void clear_frontier(Block* b) {
    if (b->frontier || !b->dirty_frontier) return;
    std::memset(b->data, 0, size_t(b->dirty_frontier) * b->stride);
    b->dirty_frontier = 0;
}
uint32_t free_index(Block* b) {
    for (; b->free_word < b->words; ++b->free_word) {
        const uint64_t free = ~b->used[b->free_word] & valid_bits(b, b->free_word);
        if (free) return b->free_word * 64 + uint32_t(std::countr_zero(free));
    }
    std::abort(); // An available block must have a free cell.
}
}
Object register_allocation(OgcPortableHeap* h, const OgcPortableType* type, uint64_t count, uint64_t bytes) {
    auto* b = allocation_block(h, type, count, bytes);
    if (!b) return {};
    uint32_t index;
    if (no_gc) {
        index = b->frontier++;
    }
    else {
        const bool fresh = b->frontier < b->capacity;
        if (fresh) {
            clear_frontier(b);
            index = b->frontier++;
        }
        else {
            index = free_index(b);
        }
        if (!fresh) {
            auto* p = b->data + size_t(index) * b->stride;
            // Small cells may clear their private padding, avoiding a variable-size libc call.
            if (bytes <= 16) std::memset(p, 0, 16);
            else if (bytes <= 32) std::memset(p, 0, 32);
            else std::memset(p, 0, size_t(bytes));
        }
    }
    publish_cell(h, b, index);
    publish_allocation(h, b, 1);
    return {b, index};
}
namespace {
void flush_run(OgcPortableHeap* h, unsigned slot) {
    auto* b = h->run_blocks[slot];
    if (!b) return;
    auto& run = h->runs[slot];
    const uint32_t unused = uint32_t((run.end - run.next) / b->stride);
    const uint32_t first = uint32_t((run.next - uintptr_t(b->data)) / b->stride);
    run = {};
    h->run_blocks[slot] = nullptr;
    if (!unused) return;
    const uint64_t charge = unused * (b->stride + (no_gc ? 0 : sizeof(Node)));
    h->since -= charge; h->statistics.allocated -= charge;
    if constexpr (no_gc) h->statistics.live_bytes -= unused * ((b->bytes + 8 + 15) & ~UINT64_C(15));
    if constexpr (!no_gc) {
        set_range<false>(b->used, first, unused);
        if (first / 64 < b->free_word) b->free_word = first / 64;
    }
    if (first + unused == b->frontier) b->frontier = first;
    b->used_count -= unused;
    available_add(b);
}
void swap_runs(OgcPortableHeap* h) {
    std::swap(h->runs[0], h->runs[1]);
    std::swap(h->run_blocks[0], h->run_blocks[1]);
}
}
Object refill_allocation_run(OgcPortableHeap* h, const OgcPortableType* type, const void* key) {
    const auto matches = [&](unsigned slot) {
        return h->run_blocks[slot] && h->runs[slot].key == key && h->run_blocks[slot]->type == type;
    };
    if (matches(1)) swap_runs(h);
    auto& run = h->runs[0];
    // A small starting reservation limits wasted work for one-off types.
    constexpr uint32_t initial_batch = 2, max_batch = 256;
    uint32_t batch = initial_batch;
    if (matches(0)) {
        auto* block = h->run_blocks[0];
        if (run.next < run.end) {
            const auto index = uint32_t((run.next - uintptr_t(block->data)) / block->stride);
            run.next += block->stride;
            return {block, index};
        }
        const auto previous = (run.end - run.begin) / block->stride;
        const auto grown = previous * 2;
        if (grown > initial_batch) batch = grown < max_batch ? uint32_t(grown) : max_batch;
    } else if (run.key != key && run.next < run.end && h->run_blocks[0]->type != type) {
        flush_run(h, 1);
        swap_runs(h);
    } else if (h->run_blocks[1] && (h->runs[1].key == key || h->run_blocks[1]->type == type)) {
        // Keep at most one reservation per type so NoGC can rewind its frontier.
        flush_run(h, 1);
    }
    flush_run(h, 0);
    if (h->storage.next_identity == UINT64_MAX) return {};
    auto* b = allocation_block(h, type, 1, type->layout.size);
    if (!b) return {};
    if (h->collector.marking) return register_allocation(h, type, 1, type->layout.size);
    const bool fresh = b->frontier < b->capacity;
    uint32_t first = b->frontier;
    uint32_t count = b->capacity - first;
    if (!fresh) {
        first = free_index(b);
        const uint64_t free_bits = ~b->used[b->free_word] & valid_bits(b, b->free_word);
        count = uint32_t(std::countr_one(free_bits >> (first % 64)));
    }
    if (count > batch) count = batch;
    const uint64_t identities = UINT64_MAX - h->storage.next_identity;
    if (count > identities) count = uint32_t(identities);
    if (!count) return {};
    if (fresh) {
        clear_frontier(b);
        b->frontier += count;
    } else {
        std::memset(payload({b, first}), 0, count * b->stride);
    }
    if constexpr (!no_gc) set_range<true>(b->used, first, count);
    for (uint32_t i = first; i < first + count; ++i) publish_cell<true>(h, b, i);
    publish_allocation(h, b, count);
    const uintptr_t begin = uintptr_t(payload({b, first}));
    run = {key, begin + b->stride, begin + count * b->stride, begin};
    h->run_blocks[0] = b;
    return {b, first};
}
void flush_allocation_run(OgcPortableHeap* h) {
    flush_run(h, 0);
    flush_run(h, 1);
}
void visit_young(OgcPortableHeap* h, OgcPolicyVisitRecord visit, void* user) {
    for (auto* b = h->storage.nursery; b; b = b->nursery_next) for (uint32_t w = 0; w < b->words; ++w) {
        uint64_t bits = b->used[w] & ~b->pending[w];
        while (bits) {
            const uint32_t i = w * 64 + uint32_t(std::countr_zero(bits)); bits &= bits - 1;
            auto* n = &b->records[i].gc;
            if (!n->old && ogc_policy_record_live(&h->collector, n)) visit(user, n);
        }
    }
}
void retire_dead(OgcPortableHeap* h) {
    if (no_gc) return;
    for (auto* b = h->storage.resources; b; b = b->resource_next) each_used(b, [&](uint32_t i) {
        Object o{b, i}; if (bit(b->pending, i) || live(h, o)) return;
        set(b->pending, i); auto* n = record(o);
        n->gc.queue_next = h->storage.pending ? &h->storage.pending->gc : nullptr;
        h->storage.pending = n; ++h->storage.pending_count;
    });
    h->storage.nursery = nullptr; h->storage.sweep = h->storage.blocks; ++h->storage.generation;
}
Object take_pending(OgcPortableHeap* h) {
    auto* n = h->storage.pending; if (!n) return {};
    h->storage.pending = reinterpret_cast<Node*>(n->gc.queue_next); --h->storage.pending_count;
    return view(&n->gc);
}
void visit_resources(OgcPortableHeap* h, void (*visit)(OgcPortableHeap*, Object)) {
    for (auto* b = h->storage.resources; b; b = b->resource_next) each_used(b, [&](uint32_t i) { visit(h, {b, i}); });
}
void release_storage(OgcPortableHeap*, Object o) { if (!no_gc) release_cell(o); }
bool mark_walk(Object o, uint64_t epoch) {
    auto* b = o.block;
    if (b->walk_epoch != epoch) { b->walk_epoch = epoch; std::memset(b->seen, 0, size_t(b->words) * 8); }
    if (bit(b->seen, o.index)) return false;
    set(b->seen, o.index); return true;
}
void set_walk_next(Object o, OgcRef next) {
    if (!no_gc) { record(o)->walk_next = next; return; }
    if (o.block->walk_links) o.block->walk_links[o.index] = next;
    else std::memcpy(static_cast<uint8_t*>(payload(o)) + o.block->stride - 2 * sizeof(next), &next, sizeof(next));
}
OgcRef walk_next(Object o) {
    if (!no_gc) return record(o)->walk_next;
    if (o.block->walk_links) return o.block->walk_links[o.index];
    OgcRef next; std::memcpy(&next, static_cast<uint8_t*>(payload(o)) + o.block->stride - 2 * sizeof(next), sizeof(next)); return next;
}
void destroy_storage(OgcPortableHeap* h) {
    auto& s = h->storage;
    while (s.blocks) free_block(h, s.blocks, nullptr);
    while (s.classes) destroy_class(s, s.classes);
    std::free(s.pages); std::free(s.class_index);
}
}
