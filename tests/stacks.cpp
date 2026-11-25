// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
// Keep mmap extensions visible alongside the XSI context declarations.
#define _DARWIN_C_SOURCE
#endif
#include "opengc/heap.h"
#ifdef OGC_TEST_BUMP
#include "opengc/mutator_bump.h"
#else
#include "opengc/mutator_marksweep.h"
#endif
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace {
int checks = 0, failures = 0, released = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}
bool equal(OgcStackBounds a, OgcStackBounds b) { return a.low == b.low && a.high == b.high; }
void release(void*) { ++released; }
struct State {
    OgcHeap* heap;
    OgcMutator mutator{};
    const OgcTypeDesc* descriptor;
    unsigned depth = 0;
};
struct Stack {
    State* state;
    OgcStackBounds bounds{};
#if defined(_WIN32)
    void* fiber = nullptr;
    void* caller = nullptr;
#else
    void* memory = nullptr;
    size_t mapping_bytes = 0;
    ucontext_t context{}, caller{};
#endif
};
thread_local Stack* entering = nullptr;
void exercise(Stack& stack);
#if defined(_WIN32)
void WINAPI entry(void* data) {
    auto& stack = *static_cast<Stack*>(data);
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    stack.bounds = {reinterpret_cast<const void*>(low), reinterpret_cast<const void*>(high)};
    exercise(stack);
    SwitchToFiber(stack.caller);
    std::abort();
}
#else
void entry() {
    auto& stack = *entering;
    exercise(stack);
    if (setcontext(&stack.caller) != 0) std::abort();
    std::abort();
}
#endif

// Keep the calling stack's locals and saved registers readable while another
// stack runs. The called frame is below every parent frame that must remain live.
void switch_stack(Stack& stack) {
    jmp_buf registers;
    setjmp(registers);
    uintptr_t marker = 0;
    const auto high = reinterpret_cast<uintptr_t>(ogc_heap_stack_current().high);
    const auto low = reinterpret_cast<uintptr_t>(&marker) < reinterpret_cast<uintptr_t>(&registers)
        ? reinterpret_cast<uintptr_t>(&marker) : reinterpret_cast<uintptr_t>(&registers);
    check(high > low, "parked stack has downward bounds");
    check(ogc_heap_add_roots(stack.state->heap, reinterpret_cast<const void*>(low), high - low) == OGC_HEAP_OK,
          "park suspended stack as a readable root range");
    check(ogc_heap_add_roots(stack.state->heap, &registers, sizeof registers) == OGC_HEAP_OK,
          "park caller registers");
#if defined(_WIN32)
    stack.caller = GetCurrentFiber();
    SwitchToFiber(stack.fiber);
#else
    auto* previous = entering;
    entering = &stack;
    check(swapcontext(&stack.caller, &stack.context) == 0, "switch alternate stack and return");
    entering = previous;
#endif
    ogc_heap_remove_roots(stack.state->heap, &registers);
    ogc_heap_remove_roots(stack.state->heap, reinterpret_cast<const void*>(low));
}
bool run_stack(State& state) {
    Stack stack{}; stack.state = &state;
#if defined(_WIN32)
    stack.fiber = CreateFiberEx(1024 * 1024, 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, entry, &stack);
    if (!stack.fiber) return false;
    switch_stack(stack);
    DeleteFiber(stack.fiber);
#else
    const auto page = size_t(sysconf(_SC_PAGESIZE));
    stack.mapping_bytes = 1024 * 1024 + 2 * page;
    stack.memory = mmap(nullptr, stack.mapping_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack.memory == MAP_FAILED) return false;
    char* low = static_cast<char*>(stack.memory) + page;
    if (mprotect(low, 1024 * 1024, PROT_READ | PROT_WRITE) != 0 || getcontext(&stack.context) != 0) {
        munmap(stack.memory, stack.mapping_bytes); return false;
    }
    stack.bounds = {low, low + 1024 * 1024};
    stack.context.uc_stack.ss_sp = low;
    stack.context.uc_stack.ss_size = 1024 * 1024;
    stack.context.uc_link = &stack.caller;
    makecontext(&stack.context, entry, 0);
    switch_stack(stack);
    munmap(stack.memory, stack.mapping_bytes);
#endif
    return true;
}
void exercise(Stack& stack) {
    auto& state = *stack.state;
    const unsigned depth = ++state.depth;
    OgcStackScope scope{}, nested{};
    check(ogc_heap_stack_enter(&scope, stack.bounds) == OGC_HEAP_OK, "enter real alternate stack");
    check(equal(ogc_heap_stack_current(), stack.bounds), "GC stack bounds follow current execution stack");
    check(ogc_heap_stack_enter(&nested, stack.bounds) == OGC_HEAP_OK, "nest scope on the same stack");
    const auto saved = scope;
    check(ogc_heap_stack_leave(&scope) == OGC_HEAP_INVALID_ARGUMENT && equal(ogc_heap_stack_current(), stack.bounds) &&
          equal(scope.bounds, saved.bounds) && scope.previous == saved.previous, "out-of-order leave preserves state");
    check(ogc_heap_stack_leave(&nested) == OGC_HEAP_OK, "leave nested scope");
    auto* volatile object = static_cast<uint64_t*>(ogc_heap_alloc(state.heap, &state.mutator, state.descriptor, 0));
    if (!object) { check(false, "allocate stack-rooted object"); std::abort(); }
    *object = uint64_t(1) << depth;
    struct Probe { OgcStackScope* scope; OgcStackBounds bounds; unsigned calls = 0; bool refused = false; } probe{&scope, stack.bounds};
    check(ogc_heap_add_cycle_hook(state.heap, [](void* user) {
        auto& p = *static_cast<Probe*>(user);
        ++p.calls;
        OgcStackScope blocked{};
        p.refused = ogc_heap_stack_enter(&blocked, p.bounds) == OGC_HEAP_BUSY &&
            ogc_heap_stack_leave(p.scope) == OGC_HEAP_BUSY && !blocked.bounds.low && !blocked.previous &&
            equal(ogc_heap_stack_current(), p.bounds);
    }, &probe) == OGC_HEAP_OK, "register collector stack-transition probe");
    const int collected = ogc_heap_collect(state.heap);
    ogc_heap_remove_cycle_hook(state.heap, &probe);
    check(collected == ((ogc_heap_capabilities(state.heap) & OGC_HEAP_COLLECTS) ? OGC_HEAP_OK : OGC_HEAP_UNSUPPORTED),
          "collect on alternate stack uses provider capability");
    check((ogc_heap_capabilities(state.heap) & OGC_HEAP_COLLECTS) ? probe.calls == 1 && probe.refused : probe.calls == 0,
          "collector callbacks cannot enter/leave stack scope; NoGC invokes no cycle hook");
    struct Visit {
        uint64_t seen = 0;
        OgcStackScope* scope;
        OgcStackBounds bounds;
        bool refused = true;
    } visit{0, &scope, stack.bounds, true};
    OgcWalk walk{};
    walk.user = &visit;
    walk.object = [](void* user, void* payload, const OgcTypeDesc*) {
        auto& v = *static_cast<Visit*>(user);
        v.seen |= *static_cast<uint64_t*>(payload);
        OgcStackScope blocked{};
        v.refused = v.refused && ogc_heap_stack_enter(&blocked, v.bounds) == OGC_HEAP_BUSY &&
            ogc_heap_stack_leave(v.scope) == OGC_HEAP_BUSY && !blocked.bounds.low && !blocked.previous &&
            equal(ogc_heap_stack_current(), v.bounds);
    };
    walk.stack = 1;
    check(ogc_heap_walk_roots(state.heap, &walk) == OGC_HEAP_OK &&
          (visit.seen & ((uint64_t(1) << (depth + 1)) - 2)) == ((uint64_t(1) << (depth + 1)) - 2),
          "active and suspended callers' objects remain reachable");
    check(visit.refused, "walk callbacks cannot enter/leave stack scope or modify the selected stack");
    check(released == 0 && *object == (uint64_t(1) << depth), "collection preserves live stack-only resource");
    if (depth < 3) check(run_stack(state), "run nested alternate stack");
    check(equal(ogc_heap_stack_current(), stack.bounds) && *object == (uint64_t(1) << depth),
          "return restores parent bounds and stack-rooted value");
    check(ogc_heap_stack_leave(&scope) == OGC_HEAP_OK, "leave alternate stack before storage is freed");
    --state.depth;
}
}

int main() {
    const auto original = ogc_heap_stack_current();
    OgcStackScope scope{}, rejected{};
    check(ogc_heap_stack_enter(nullptr, original) == OGC_HEAP_INVALID_ARGUMENT, "null scope refused");
    check(ogc_heap_stack_enter(&rejected, {}) == OGC_HEAP_INVALID_ARGUMENT && !rejected.previous && !rejected.bounds.low,
          "empty bounds refused without writing storage");
    check(ogc_heap_stack_enter(&rejected, {original.high, original.low}) == OGC_HEAP_INVALID_ARGUMENT,
          "reversed bounds refused");
    check(ogc_heap_stack_enter(&rejected, {reinterpret_cast<void*>(1), reinterpret_cast<void*>(4096)}) == OGC_HEAP_INVALID_ARGUMENT,
          "bounds excluding running stack refused");
    check(ogc_heap_stack_enter(&scope, original) == OGC_HEAP_OK, "enter OS thread stack");
    check(ogc_heap_stack_enter(&scope, original) == OGC_HEAP_INVALID_ARGUMENT, "duplicate active scope refused");
    bool foreign = false;
    std::thread thread([&] {
        OgcStackScope own{};
        const auto bounds = ogc_heap_stack_current();
        foreign = ogc_heap_stack_enter(&own, bounds) == OGC_HEAP_OK &&
            ogc_heap_stack_leave(&scope) == OGC_HEAP_INVALID_ARGUMENT &&
            equal(ogc_heap_stack_current(), bounds) && ogc_heap_stack_leave(&own) == OGC_HEAP_OK;
    });
    thread.join();
    check(foreign && equal(ogc_heap_stack_current(), original), "foreign thread cannot pop owner's scope");
    State state{};
    state.heap = ogc_heap_create(nullptr);
    if (!state.heap || ogc_heap_mutator_register(state.heap, &state.mutator) != OGC_HEAP_OK) return 1;
    state.descriptor = ogc_heap_intern(state.heap, 8, 8, 0, nullptr, 0, release, "stack resource");
    bool converted = false;
#if defined(_WIN32)
    if (!IsThreadAFiber()) { if (!ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH)) return 1; converted = true; }
#endif
    check(run_stack(state), "run first alternate stack");
#if defined(_WIN32)
    if (converted) check(ConvertFiberToThread() != 0, "restore original thread mode");
#else
    (void)converted;
#endif
    check(equal(ogc_heap_stack_current(), original), "all switches restore original stack bounds");
    const int collected = ogc_heap_collect(state.heap);
    check(collected == ((ogc_heap_capabilities(state.heap) & OGC_HEAP_COLLECTS) ? OGC_HEAP_OK : OGC_HEAP_UNSUPPORTED),
          "collection after alternate stacks are released scans no disposed stack");
    check(ogc_heap_stack_leave(&scope) == OGC_HEAP_OK && ogc_heap_stack_leave(&scope) == OGC_HEAP_INVALID_ARGUMENT,
          "leave OS scope once");
    check(ogc_heap_mutator_unregister(state.heap, &state.mutator) == OGC_HEAP_OK &&
          ogc_heap_destroy(state.heap) == OGC_HEAP_OK, "all parked roots removed before heap shutdown");
    check(released == 3, "three stack resources released exactly once at shutdown");
    std::printf("%s stack scopes: %d checks\n", failures ? "FAIL" : "OK", checks);
    return failures ? 1 : 0;
}
