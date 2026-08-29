// rhi/vulkan/allocator_plat.hpp

#pragma once

#include <cstdint>

#include "rhi/vulkan/memory_allocator.hpp"

namespace cairns::rhi {

struct AllocatorPlat {
    vulkan::MemoryAllocator memory_;
    uint32_t uniform_align_ = 256;
    uint32_t storage_align_ = 256;
};

}  // namespace cairns::rhi
