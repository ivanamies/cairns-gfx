// rhi/vulkan/internal/allocator_impl.hpp
//
// Internal: defines Allocator::Impl for the Vulkan backend. Shared between
// vulkan/allocator.cpp and vulkan/resource_manager.cpp (which reaches the
// MemoryAllocator via friendship during the transitional split). Not public.

#pragma once

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <cstdint>

#include "rhi/allocator.hpp"
#include "rhi/vulkan/memory_allocator.hpp"

namespace cairns::rhi {

struct Allocator::Impl {
    vulkan::MemoryAllocator memory;
    uint32_t uniform_align = 256;
    uint32_t storage_align = 256;
};

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
