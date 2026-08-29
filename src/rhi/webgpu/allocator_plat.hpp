// rhi/webgpu/allocator_plat.hpp
#pragma once

#include "rhi/webgpu/memory_allocator.hpp"

namespace cairns::rhi {

struct AllocatorPlat {
    webgpu::MemoryAllocator memory_;
};

}  // namespace cairns::rhi
