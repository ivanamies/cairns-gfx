// rhi/webgpu/layouts_plat.hpp
//
// Canonical bind-group layouts shared by the unlit forward path. WebGPU
// validates setBindGroup by "group-equivalence" (identical layout entries),
// and wgpu-native dedups identical layout descriptors -- so building the SAME
// descriptor in CreateDynamicBuffers / CreateBindGroup (the bind groups) and
// CreateGraphicsPipeline (the pipeline layout) yields compatible layouts
// without threading the layout objects through the shared engine code.
#pragma once

#include "util/define.hpp"

#if CAIRNS_WEBGPU

#include <webgpu/webgpu.h>

#include "rhi/resource_manager.hpp"

namespace cairns::rhi::webgpu {

// Group 0 (globals) / group 2 (drawtmp): one dynamic-offset uniform buffer at
// binding 0, visible to vertex+fragment. minBindingSize 0 => the bound range is
// validated per draw from the bind group's entry size, so globals (176B) and
// drawtmp (80B) share one layout object.
inline WGPUBindGroupLayout MakeDynUboLayout(WGPUDevice device) {
    WGPUBindGroupLayoutEntry e = {};
    e.binding = 0;
    e.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    e.buffer.type = WGPUBufferBindingType_Uniform;
    e.buffer.hasDynamicOffset = 1;
    e.buffer.minBindingSize = 0;
    WGPUBindGroupLayoutDescriptor d = {};
    d.entryCount = 1;
    d.entries = &e;
    return wgpuDeviceCreateBindGroupLayout(device, &d);
}

// Group 1 (material): texture_2d<f32> at binding 0 + filtering sampler at
// binding 1, both fragment-only. Mirrors the GLSL combined sampler2D split.
inline WGPUBindGroupLayout MakeMaterialLayout(WGPUDevice device) {
    WGPUBindGroupLayoutEntry e[2] = {};
    e[0].binding = 0;
    e[0].visibility = WGPUShaderStage_Fragment;
    e[0].texture.sampleType = WGPUTextureSampleType_Float;
    e[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    e[1].binding = 1;
    e[1].visibility = WGPUShaderStage_Fragment;
    e[1].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor d = {};
    d.entryCount = 2;
    d.entries = e;
    return wgpuDeviceCreateBindGroupLayout(device, &d);
}

inline WGPUVertexFormat ToWgpuVertexFormat(Format f) {
    switch (f) {
        case Format::kRgba32F: return WGPUVertexFormat_Float32x4;
        case Format::kRg32F: return WGPUVertexFormat_Float32x2;
        case Format::kR32F: return WGPUVertexFormat_Float32;
        case Format::kRgba8Unorm: return WGPUVertexFormat_Unorm8x4;
        default: return WGPUVertexFormat_Float32x4;
    }
}

}  // namespace cairns::rhi::webgpu

#endif  // CAIRNS_WEBGPU
