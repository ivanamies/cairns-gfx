#pragma once

#include "util/offset_allocator.hpp"

#include <memory>

namespace cairns {

template<typename T>
struct ArenaDeleter {
    void operator()(T* ptr) const {
        if (ptr) {
            ptr->~T(); // Manually call destructor
            // Memory is NOT freed here; the Arena handles it.
        }
    }
};

template<typename T>
using unique_ptr = std::unique_ptr<T, ArenaDeleter<T>>;

template<typename T, typename... Args>
unique_ptr<T> make_unique(Arena& arena, Args&&... args) {
    void* raw_mem = arena.allocate(sizeof(T));
    if (!raw_mem) {
        printf("std::bad_alloc\n");
        std::terminate();
    }
    T* obj_ptr = new (raw_mem) T(std::forward<Args>(args)...);
    return unique_ptr<T>(obj_ptr);
}

} // namespace cairns
