// util/alloc_count.hpp
//
// Global heap-allocation receipt counters. Under CAIRNS_ALLOC_TRACE the TU
// alloc_count.cpp replaces the global operator new/delete set to bump these
// atomics; with the flag off the operators are NOT replaced and the counters
// stay zero (the module compiles to a few inert accessors). The engine
// snapshots at the [LOAD]/[RELOAD] fences and prints the delta, so a
// load/reload allocation storm surfaces as a single receipt line instead of
// tens of thousands of [ALLOC] lines.
//
// This is the always-on TOTAL receipt. print_allocator (per-container, tagged)
// stays the per-phase attribution tool, deployed selectively then reverted.
#pragma once

#include <cstdint>

namespace cairns::alloc_count {

struct Snapshot {
    uint64_t allocs = 0;
    uint64_t frees = 0;
    uint64_t bytes = 0;
};

// Current global counters. Returns zeros in non-trace builds.
Snapshot Now();

// Print an "[ALLOC-RECEIPT] <label> allocs=.. frees=.. bytes=.." delta (since
// `before`) to stderr. Safe to call in non-trace builds (prints zeros) but the
// engine gates its calls under CAIRNS_ALLOC_TRACE to keep normal logs clean.
void PrintDelta(const char* label, const Snapshot& before);

}  // namespace cairns::alloc_count
