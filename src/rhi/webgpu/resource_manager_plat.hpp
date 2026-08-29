// rhi/webgpu/resource_manager_plat.hpp
//
// WebGPU side of the resource_manager seam. Like vulkan, Api*Handle is
// type-erased to void* (the cross-backend Hot/Cold structs store one pointer;
// the .cpp reinterpret_cast to the concrete WGPU handle). WebGPU tracks
// resource usage implicitly, so the per-resource Plat sub-structs are near-empty
// (no image-layout cache, no barrier fence).
#pragma once

#include <cstdint>

// Forward-declare the opaque WGPU handle typedefs instead of including
// webgpu.h (mirrors the metal plat header's MTL:: forward declarations): the
// pure-CPU spec tests include this transitively and carry no backend include
// dirs. Redeclaring an identical typedef is legal if webgpu.h is also seen.
extern "C" {
typedef struct WGPUPipelineLayoutImpl* WGPUPipelineLayout;
typedef struct WGPUBindGroupLayoutImpl* WGPUBindGroupLayout;
typedef struct WGPUBindGroupImpl* WGPUBindGroup;
typedef struct WGPUDeviceImpl* WGPUDevice;
typedef struct WGPUQueueImpl* WGPUQueue;
}

namespace cairns::rhi {

// Type-erased (matches vulkan). Concrete types stored:
//   ApiTextureHandle  -> WGPUTextureView (Hot.api_view) / WGPUTexture (Cold.api_image)
//   ApiSamplerHandle  -> WGPUSampler
//   ApiPsoHandle      -> WGPURenderPipeline
//   ApiArgBufferHandle-> WGPUBindGroup
//   ApiKernelHandle   -> WGPUComputePipeline
using ApiTextureHandle = void*;
using ApiSamplerHandle = void*;
using ApiPsoHandle = void*;
using ApiArgBufferHandle = void*;
using ApiKernelHandle = void*;

// WebGPU synchronizes implicitly -> no layout cache, no per-resource fence.
struct TextureColdPlat {};
struct BufferColdPlat {};
struct ShaderHotPlat {
    WGPUPipelineLayout layout = nullptr;       // owned alongside the pipeline
    WGPUBindGroupLayout bind_group_layouts[4] = {nullptr, nullptr, nullptr,
                                                 nullptr};
};
struct KernelHotPlat {
    WGPUPipelineLayout layout = nullptr;
    WGPUBindGroupLayout set0_bgl = nullptr;  // skin: per-batch bind group layout
};
// Dynamic-offset bind groups: one bind-group layout + a per-FIF bind group.
struct DynamicBuffersHotPlat {
    WGPUBindGroupLayout layout = nullptr;
    static constexpr uint32_t kMaxFrames = 4;
    WGPUBindGroup sets[kMaxFrames] = {nullptr, nullptr, nullptr, nullptr};
};

struct BackendInitParams {
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
};

}  // namespace cairns::rhi
