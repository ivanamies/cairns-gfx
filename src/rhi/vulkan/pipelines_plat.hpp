// rhi/vulkan/pipelines_plat.hpp
//
// Vulkan side of Pipelines::plat. See rhi/pipelines_plat.hpp.

#pragma once

#include <vulkan/vulkan.h>

namespace cairns::rhi {

struct PipelinesPlat {
    VkDevice device_ = VK_NULL_HANDLE;  // mirrored from Device
};

}  // namespace cairns::rhi
