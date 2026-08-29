#pragma once

#include "util/std_allocator.hpp"

#include <cstdint>
#include <mutex>
#include <memory>
#include <vector>

namespace cairns::rhi {

template<typename T>
struct Handle {
    
    // slide 22/page 36, handles must fit in 4 bytes to make Draw packet 64 bytes as Hypehype claims
    using id_type = uint32_t;
    using gen_type = uint32_t;
    
    Handle() = default;
    
    explicit Handle(id_type id, gen_type gen) : id_(id), gen_(gen) {
        static_assert(std::is_trivially_destructible_v<Handle<T>> and std::is_standard_layout_v<Handle<T>>, "destroying this must take no work");
    }
    
    inline static const Handle Null{
        std::numeric_limits<id_type>::max(),
        std::numeric_limits<id_type>::max()
    };
    
    id_type get_id() const {
        return id_;
    }
    
    gen_type get_gen() const {
        return gen_;
    }
    
    bool is_valid() const {
        return id_ != std::numeric_limits<uint32_t>::max();
    }
    
    auto operator<=>(const Handle<T>& other) const = default;
    
private:
    uint32_t id_ = std::numeric_limits<uint32_t>::max();
    uint32_t gen_ = std::numeric_limits<uint32_t>::max();
};

template<typename Tag>
struct ResourceObject;

template<typename Tag>
struct ResourceDescriptor;

template<typename Tag>
struct ResourceManager {
    
    ResourceManager(Arena& arena, uint32_t capacity) : arena_(arena), objects(cairns::Allocator<ResourceObject<Tag>>(arena_)) {
        capacity_ = capacity;
        objects.resize(capacity);
        descriptors.resize(capacity);
        generations.resize(capacity, 0);
        free_list.resize(capacity);
        for ( int64_t i = 0; i < capacity; ++i ) {
            free_list[i] = static_cast<uint32_t>(i);
        }
    }
    
    Handle<Tag> New() {
        std::unique_lock<std::mutex> lock(alloc_mutex);
        
        // Check if we need to grow the capacity
        if (free_list_ptr >= objects.size()) {
            uint32_t old_capacity = static_cast<uint32_t>(objects.size());
            uint32_t new_capacity = old_capacity * 2;
            
            objects.resize(new_capacity);
            descriptors.resize(new_capacity);
            generations.resize(new_capacity, 0);
            free_list.resize(new_capacity);
            capacity_ = new_capacity;
            
            for (uint32_t i = old_capacity; i < new_capacity; ++i) {
                free_list[i] = i;
            }
        }
        
        const uint32_t id = free_list[free_list_ptr++];
        return Handle<Tag>(id, generations[id]);
    }
    
    void Delete(Handle<Tag>& h) {
        if (!h.is_valid()) return;
        
        const uint32_t id = h.id;
        const uint32_t gen = h.generation;
        
        // Check handle validity without locking first
        if (id < generations.size() && generations[id] == gen) {
            std::unique_lock<std::mutex> lock(alloc_mutex);
            // Re-check under lock
            if (generations[id] == gen) {
                generations[id]++; // Invalidate all existing handles
                free_list[--free_list_ptr] = id;
                h = Handle<Tag>::Null;
            }
        }
    }
    
    ResourceObject<Tag>* GetObj(Handle<Tag> h) {
        if (!h.is_valid() || h.get_id() >= objects.size()) {
            return nullptr;
        }
        if (generations[h.get_id()] != h.get_gen()) {
            return nullptr;
        }
        return &objects[h.get_id()];
    }
    
    ResourceDescriptor<Tag>* GetDesc(Handle<Tag> h) {
        if (!h.is_valid() || h.get_id() >= descriptors.size()) {
            return nullptr;
        }
        if (generations[h.get_id()] != h.get_gen()) {
            return nullptr;
        }
        return &descriptors[h.get_id()];
    }
    
    uint32_t GetCapacity() const {
        return capacity_;
    }
    
private:
    Arena& arena_;
    
    std::vector<ResourceObject<Tag>, cairns::Allocator<ResourceObject<Tag>>> objects;
    std::vector<ResourceDescriptor<Tag>> descriptors;
    std::vector<uint32_t> generations;
    
    uint32_t capacity_ = 0;
    
    std::mutex alloc_mutex;
    uint32_t free_list_ptr = 0;
    std::vector<uint32_t> free_list;
};

} // namespace cairns::rhi
