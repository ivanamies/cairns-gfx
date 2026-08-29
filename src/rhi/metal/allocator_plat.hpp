// rhi/metal/allocator_plat.hpp

#pragma once

#include "rhi/metal/memory_allocator.hpp"

namespace cairns::rhi {

struct AllocatorPlat {
    metal::MemoryAllocator memory_;
};

}  // namespace cairns::rhi
