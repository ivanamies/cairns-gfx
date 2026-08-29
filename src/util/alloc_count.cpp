// util/alloc_count.cpp
//
// Global allocation counters + (under CAIRNS_ALLOC_TRACE) the replaced global
// operator new/delete set that feeds them. One TU owns the replacement so the
// One-Definition-Rule holds. The engine's [LOAD]/[RELOAD] fences reference
// Now(), which forces this object into the link in trace builds and brings the
// operator replacements with it.

#include "util/alloc_count.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>  // std::align_val_t

namespace cairns::alloc_count {
namespace {
std::atomic<uint64_t> g_allocs{0};
std::atomic<uint64_t> g_frees{0};
std::atomic<uint64_t> g_bytes{0};
}  // namespace

Snapshot Now() {
    Snapshot s;
    s.allocs = g_allocs.load(std::memory_order_relaxed);
    s.frees = g_frees.load(std::memory_order_relaxed);
    s.bytes = g_bytes.load(std::memory_order_relaxed);
    return s;
}

void PrintDelta(const char* label, const Snapshot& before) {
    const Snapshot now = Now();
    std::fprintf(
        stderr,
        "[ALLOC-RECEIPT] %s allocs=%llu frees=%llu bytes=%llu\n", label,
        static_cast<unsigned long long>(now.allocs - before.allocs),
        static_cast<unsigned long long>(now.frees - before.frees),
        static_cast<unsigned long long>(now.bytes - before.bytes));
}

#if CAIRNS_ALLOC_TRACE
namespace {
void* CountedAlloc(std::size_t n) {
    // n==0 is legal; allocate 1 so every new returns a distinct pointer.
    void* p = std::malloc(n ? n : 1);
    if (!p) {
        // -fno-exceptions: cannot throw std::bad_alloc; receipts build, fail
        // loud.
        std::abort();
    }
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(n, std::memory_order_relaxed);
    return p;
}
void CountedFree(void* p) {
    if (p) {
        g_frees.fetch_add(1, std::memory_order_relaxed);
        std::free(p);
    }
}
}  // namespace
#endif

}  // namespace cairns::alloc_count

#if CAIRNS_ALLOC_TRACE
// Replaceable global allocation functions (C++17 set, incl. aligned). All route
// through malloc/free so mismatched new/delete forms stay consistent and every
// (de)allocation in the final binary is counted.
void* operator new(std::size_t n) {
    return cairns::alloc_count::CountedAlloc(n);
}
void* operator new[](std::size_t n) {
    return cairns::alloc_count::CountedAlloc(n);
}
void* operator new(std::size_t n, std::align_val_t) {
    return cairns::alloc_count::CountedAlloc(n);
}
void* operator new[](std::size_t n, std::align_val_t) {
    return cairns::alloc_count::CountedAlloc(n);
}
void operator delete(void* p) noexcept { cairns::alloc_count::CountedFree(p); }
void operator delete[](void* p) noexcept {
    cairns::alloc_count::CountedFree(p);
}
void operator delete(void* p, std::size_t) noexcept {
    cairns::alloc_count::CountedFree(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    cairns::alloc_count::CountedFree(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    cairns::alloc_count::CountedFree(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    cairns::alloc_count::CountedFree(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    cairns::alloc_count::CountedFree(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    cairns::alloc_count::CountedFree(p);
}
#endif
