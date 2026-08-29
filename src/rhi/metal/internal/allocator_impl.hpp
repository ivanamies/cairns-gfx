// rhi/metal/internal/allocator_impl.hpp
//
// Internal: defines Allocator::Impl for the Metal backend. Shared between
// metal/allocator.cpp and metal/resource_manager.cpp (friend access during the
// transitional split). Not a public header.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#include "rhi/allocator.hpp"
#include "rhi/metal/memory_allocator.hpp"

namespace cairns::rhi {

struct Allocator::Impl {
    metal::MemoryAllocator memory;
};

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
