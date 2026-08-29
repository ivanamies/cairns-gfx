// rhi/rhi.hpp
//
// The single owner of every RHI subsystem. The engine holds one Rhi by value.
// Field order = construction order = reverse teardown order. No methods: the
// subsystems keep their own; nothing stores a pointer to a sibling.

#pragma once

#include "rhi/device.hpp"
#include "rhi/allocator.hpp"
#include "rhi/resources.hpp"
#include "rhi/bindless.hpp"
#include "rhi/frames.hpp"
#include "rhi/pipelines.hpp"

namespace cairns::rhi {

struct Rhi {
    Device device;
    Allocator alloc;
    Resources resources;
    Bindless bindless;
    Frames frames;
    Pipelines pipelines;
};

}  // namespace cairns::rhi
