// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace opengc::heap_detail {
// Refuse a stale alternate-stack scope before a collector can scan an address gap.
uint8_t* stack_scan_top(const void* current);
// A collector/walker snapshot cannot be suspended with its internal roots live.
void stack_pin();
void stack_unpin();
struct StackPin {
    StackPin() { stack_pin(); }
    ~StackPin() { stack_unpin(); }
    StackPin(const StackPin&) = delete;
    StackPin& operator=(const StackPin&) = delete;
};
}
