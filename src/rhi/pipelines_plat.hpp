// rhi/pipelines_plat.hpp
//
// Platform-resolver header. Builds in the right backend's PipelinesPlat type
// based on the build's CAIRNS_METAL / CAIRNS_VULKAN macros; the common header
// rhi/pipelines.hpp pulls this in and uses PipelinesPlat as a fully-defined
// member type, with NO ifdef in the Pipelines class body.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL
#include "rhi/metal/pipelines_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/pipelines_plat.hpp"
#else
#error "Unsupported backend: define CAIRNS_METAL or CAIRNS_VULKAN"
#endif
