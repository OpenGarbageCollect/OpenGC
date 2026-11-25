// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "opengc/heap.h"
#include "stack_bounds.h"
#include <cstdlib>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace {
thread_local OgcStackScope* active = nullptr;
thread_local uint32_t pins = 0;
bool contains(OgcStackBounds bounds, const void* address) {
    const auto low = reinterpret_cast<uintptr_t>(bounds.low);
    const auto high = reinterpret_cast<uintptr_t>(bounds.high);
    const auto word = reinterpret_cast<uintptr_t>(address);
    return low && low < high && word >= low && word < high;
}
OgcStackBounds system_bounds() {
#if defined(_WIN32)
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    return {reinterpret_cast<const void*>(low), reinterpret_cast<const void*>(high)};
#elif defined(__APPLE__)
    const auto thread = pthread_self();
    const auto high = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(thread));
    const auto bytes = pthread_get_stacksize_np(thread);
    return {reinterpret_cast<const void*>(high - bytes), reinterpret_cast<const void*>(high)};
#elif defined(__linux__)
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) return {};
    void* low = nullptr;
    size_t bytes = 0;
    const int status = pthread_attr_getstack(&attributes, &low, &bytes);
    pthread_attr_destroy(&attributes);
    if (status != 0 || bytes > UINTPTR_MAX - reinterpret_cast<uintptr_t>(low)) return {};
    return {low, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(low) + bytes)};
#else
    return {};
#endif
}
}
extern "C" OgcStackBounds ogc_heap_stack_current(void) {
    return active ? active->bounds : system_bounds();
}
extern "C" int ogc_heap_stack_enter(OgcStackScope* scope, OgcStackBounds bounds) {
    uintptr_t current = 0;
    if (!scope || !contains(bounds, &current)) return OGC_HEAP_INVALID_ARGUMENT;
    for (auto* s = active; s; s = s->previous)
        if (s == scope) return OGC_HEAP_INVALID_ARGUMENT;
    if (pins) return OGC_HEAP_BUSY;
    scope->bounds = bounds;
    scope->previous = active;
    active = scope;
    return OGC_HEAP_OK;
}
extern "C" int ogc_heap_stack_leave(OgcStackScope* scope) {
    if (!scope || scope != active) return OGC_HEAP_INVALID_ARGUMENT;
    if (pins) return OGC_HEAP_BUSY;
    active = scope->previous;
    scope->bounds = {};
    scope->previous = nullptr;
    return OGC_HEAP_OK;
}
namespace opengc::heap_detail {
void stack_pin() { if (pins == UINT32_MAX) std::abort(); ++pins; }
void stack_unpin() { if (!pins) std::abort(); --pins; }
uint8_t* stack_scan_top(const void* current) {
    const uintptr_t frame = 0;
    const auto bounds = ogc_heap_stack_current();
    if (!contains(bounds, current) || !contains(bounds, &frame)) std::abort();
    return static_cast<uint8_t*>(const_cast<void*>(bounds.high));
}
}
