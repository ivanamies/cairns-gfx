#pragma once

#include "util/offset_allocator.hpp"

#include <cstdint>
#include <map>

namespace cairns {

// SINGLE THREADED.
// DO NOT CALL IN THE MIDDLE OF THE FRAME.
struct Arena {
    inline static constexpr bool kEnabled = false;

    explicit Arena(void* mem, uint32_t capacity) : mem_(mem), capacity_(capacity), alloc_(capacity_) {
    }
    
    void* allocate(size_t bytes) {
        if ( kEnabled ) {
            assert(bytes < std::numeric_limits<uint32_t>::max());
            OffsetAllocator::Allocation t = alloc_.allocate(static_cast<uint32_t>(bytes));
            uint8_t* res = (uint8_t *)(mem_);
            res += t.offset;
            m_[res] = t;
            return res;
        }
        else {
            return malloc(bytes);
        }
    }
    
    void deallocate(void* ptr, [[maybe_unused]] size_t bytes) {
        if ( kEnabled ) {
            OffsetAllocator::Allocation t = m_[ptr];
            alloc_.free(t);
            m_.erase(ptr);
        }
        else {
            free(ptr);
        }
    }
    
private:
    void* mem_;
    uint32_t capacity_;
    OffsetAllocator::Allocator alloc_;
    // this looks stupid because it is.
    std::map<void*, OffsetAllocator::Allocation> m_;
};

template<typename T>
class Allocator {
public:
    using value_type = T;
    explicit Allocator(Arena& arena) : arena_(arena) { }
    
    template<typename U>
    Allocator(const Allocator<U>& other) : arena_(other.arena_) { }
    
    T* allocate(size_t n) {
        T* res = static_cast<T*>(arena_.allocate(n * sizeof(T)));
        if ( !res ) {
            printf("std::bad alloc\n");
            std::terminate();
        }
        return res;
    }
    
    void deallocate(T* p, size_t n) {
        arena_.deallocate(p, n*sizeof(T));
    }
    
    bool operator==(const Allocator& other) const {
        return arena_ == other.arena_;
    }
    
    bool operator!=(const Allocator& other) const {
        return !(arena_ == other.arena_);
    }
    
private:
    template<typename U>
    friend class Allocator;
    
    Arena& arena_;
};

} // namespace cairns
