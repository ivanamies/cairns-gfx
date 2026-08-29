#pragma once

#include "util/gfx_config.hpp"

#if defined(__APPLE__) and defined(__aarch64__)
#define CAIRNS_APPLE 1
#else // defined(__APPLE__) and defined(__aarch64__)
#define CAIRNS_APPLE 0
#endif // defined(__APPLE__) and defined(__aarch64__)

#if defined(__ANDROID__)
#define CAIRNS_ANDROID 1
#else // defined(__ANDROID__)
#define CAIRNS_ANDROID 0
#endif // defined(__ANDROID__)

#if defined(__linux__)
#define CAIRNS_LINUX 1
#else // defined(__linux__)
#define CAIRNS_LINUX 0
#endif // defined(__linux__)

#if defined(_WIN32)
    #define CAIRNS_D3D12 1
#else
    #define CAIRNS_D3D12 0
#endif

#if CAIRNS_APPLE and (CAIRNS_GFX_BACKEND_METAL == 1)
    #define CAIRNS_METAL 1
#else
    #define CAIRNS_METAL 0
#endif

#if (CAIRNS_GFX_BACKEND_VULKAN == 1)
    #define CAIRNS_VULKAN 1
#else
    #define CAIRNS_VULKAN 0
#endif

#if (CAIRNS_GFX_BACKEND_WEBGPU == 1)
    #define CAIRNS_WEBGPU 1
#else
    #define CAIRNS_WEBGPU 0
#endif

