// SPDX-License-Identifier: GPL-3.0-or-later
// Private non-collecting primitives shared by link-time heap implementations.
#pragma once
#include "opengc/heap.h"
#include "stack_bounds.h"
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#endif
#if defined(__clang__) || defined(__GNUC__)
#define OGC_NO_ASAN __attribute__((no_sanitize("address"), noinline))
#else
#define OGC_NO_ASAN __declspec(noinline)
#endif
namespace opengc::heap_detail {
[[noreturn]] inline void metadata_exhausted() { std::abort(); }

// A growable array of trivially copyable values.
template <class T> struct Vec {
    T *p = nullptr;
    uint32_t n = 0, cap = 0;
    Vec() = default;
    Vec(const Vec &o) { *this = o; }
    Vec &operator=(const Vec &o) {
        if (this == &o)
            return *this;
        n = 0;
        reserve(o.n);
        if (o.n)
            std::memcpy(static_cast<void *>(p), o.p, sizeof(T) * o.n);
        n = o.n;
        return *this;
    }
    ~Vec() { std::free(p); }
    void reserve(uint32_t c) {
        if (c <= cap)
            return;
        uint32_t nc = cap ? cap * 2 : 8;
        while (nc < c)
            nc *= 2;
        void *q = std::realloc(static_cast<void *>(p), sizeof(T) * nc);
        if (!q)
            metadata_exhausted();
        p = static_cast<T *>(q);
        cap = nc;
    }
    void push_back(const T &v) {
        if (n == cap)
            reserve(n + 1);
        p[n++] = v;
    }
    void pop_back() { --n; }
    T &back() { return p[n - 1]; }
    const T &back() const { return p[n - 1]; }
    uint32_t size() const { return n; }
    bool empty() const { return n == 0; }
    T &operator[](uint32_t i) { return p[i]; }
    const T &operator[](uint32_t i) const { return p[i]; }
    T *begin() { return p; }
    T *end() { return p + n; }
    const T *begin() const { return p; }
    const T *end() const { return p + n; }
    void resize(uint32_t k) {
        reserve(k);
        for (uint32_t i = n; i < k; ++i)
            p[i] = T{};
        n = k;
    }
    void clear() { n = 0; }
    void erase_at(uint32_t i) {
        std::memmove(static_cast<void *>(p + i), p + i + 1, sizeof(T) * (n - i - 1));
        --n;
    }
    void swap(Vec &o) {
        T *tp = p;
        p = o.p;
        o.p = tp;
        uint32_t t = n;
        n = o.n;
        o.n = t;
        t = cap;
        cap = o.cap;
        o.cap = t;
    }
};

inline void *os_map(size_t n) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}
inline void os_unmap(void *p, size_t n) {
#if defined(_WIN32)
    (void)n;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, n);
#endif
}

inline uint8_t *stack_top() {
    const uintptr_t current = 0;
    return stack_scan_top(&current);
}

// A hash set of cell addresses for walks: open addressing, grown at half full.
struct Seen {
    const uint8_t **slots = nullptr;
    uint32_t cap = 0, n = 0;
    ~Seen() { std::free(static_cast<void *>(slots)); }
    static uint32_t hash(const uint8_t *p) {
        return uint32_t((reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 32);
    }
    bool insert(const uint8_t *p) {
        if (2 * (n + 1) > cap)
            grow();
        for (uint32_t i = hash(p) & (cap - 1);; i = (i + 1) & (cap - 1)) {
            if (slots[i] == p)
                return false;
            if (!slots[i]) {
                slots[i] = p;
                ++n;
                return true;
            }
        }
    }
    void grow() {
        const uint32_t nc = cap ? cap * 2 : 1024;
        const uint8_t **ns = static_cast<const uint8_t **>(std::calloc(nc, sizeof(const uint8_t *)));
        if (!ns)
            metadata_exhausted();
        for (uint32_t k = 0; k < cap; ++k)
            if (slots[k])
                for (uint32_t i = hash(slots[k]) & (nc - 1);; i = (i + 1) & (nc - 1))
                    if (!ns[i]) {
                        ns[i] = slots[k];
                        break;
                    }
        std::free(static_cast<void *>(slots));
        slots = ns;
        cap = nc;
    }
};

struct WalkObject {
    uint8_t *payload;
    const OgcTypeDesc *descriptor;
};

// A walk from a collection callback must inspect the calling mutator, not
// collector locals holding the very roots/descriptors being enumerated.
struct MutatorStack {
    inline static thread_local const MutatorStack* active = nullptr;
    const MutatorStack* previous = active;
    jmp_buf registers{};
    MutatorStack() {
        stack_pin();
        if (!previous) {
            setjmp(registers);
            active = this;
        }
    }
    ~MutatorStack() { active = previous; stack_unpin(); }
};

template <class Heap, class Find> struct Walker {
    Heap &h;
    const OgcWalk &w;
    Seen seen;
    struct Todo {
        uint8_t *payload;
        const OgcTypeDesc *d;
    };
    Vec<Todo> todo;

    void reach(uintptr_t a) {
        if (a < 4096 || (a & 7))
            return;
        const WalkObject object = Find{}(h, a);
        if (!object.payload || !seen.insert(object.payload))
            return;
        todo.push_back(Todo{object.payload, object.descriptor});
    }
    void from(uintptr_t x) {
        if (w.word)
            w.word(w.user, x);
        reach(x);
    }
    OGC_NO_ASAN void from_range(const void *base, size_t bytes) {
        const uint8_t *lo = static_cast<const uint8_t *>(base);
        const uint8_t *hi = lo + bytes;
        for (const uint8_t *q =
                 reinterpret_cast<const uint8_t *>((reinterpret_cast<uintptr_t>(lo) + 7) & ~uintptr_t(7));
             q + 8 <= hi; q += 8) {
            uintptr_t x;
            std::memcpy(&x, q, 8);
            from(x);
        }
    }
    OGC_NO_ASAN void from_stack() {
        if (const MutatorStack* mutator = MutatorStack::active) {
            from_range(&mutator->registers, sizeof mutator->registers);
            const auto* lo = reinterpret_cast<const uint8_t*>(mutator);
            from_range(lo, size_t(stack_scan_top(lo) - lo));
            return;
        }
        jmp_buf regs{};
        setjmp(regs);
        from_range(&regs, sizeof regs);
        volatile uint8_t here = 0;
        const uint8_t *lo = const_cast<const uint8_t *>(&here);
        from_range(lo, size_t(stack_scan_top(lo) - lo));
    }
    void run() {
        while (!todo.empty()) {
            const Todo t = todo.back();
            todo.pop_back();
            if (w.object)
                w.object(w.user, t.payload, t.d);
            uint64_t bytes = t.d->size;
            if (t.d->vec) {
                uint64_t len;
                std::memcpy(&len, t.payload, 8);
                bytes = 8 + len * t.d->size;
            }
            if (w.word)
                for (uint64_t off = 0; off + 8 <= bytes; off += 8) {
                    uintptr_t x;
                    std::memcpy(&x, t.payload + off, 8);
                    w.word(w.user, x);
                }
            if (!t.d->nrefs)
                continue;
            if (!t.d->vec) {
                for (uint32_t k = 0; k < t.d->nrefs; ++k) {
                    uintptr_t x;
                    std::memcpy(&x, t.payload + t.d->refs[k], 8);
                    reach(x);
                }
                continue;
            }
            uint64_t len;
            std::memcpy(&len, t.payload, 8);
            const uint8_t *e = t.payload + 8;
            for (uint64_t i = 0; i < len; ++i, e += t.d->size)
                for (uint32_t k = 0; k < t.d->nrefs; ++k) {
                    uintptr_t x;
                    std::memcpy(&x, e + t.d->refs[k], 8);
                    reach(x);
                }
        }
    }
};

} // namespace opengc::heap_detail
