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

// Compute kernel set 0 (particle/skin/anim-eval): one entry per DynamicBinding.
// kUniform -> uniform (dynamic-offset per has_dynamic_offset); kStorage ->
// read_write storage. WGSL declares read-only storages as read_write too, so a
// single Storage binding type covers src+dst -- they never alias within a set.
// Compute-visible; wgpu-native dedups identical descriptors so the bind group
// (CreateDynamicBuffers) and the pipeline layout share one layout object.
inline constexpr size_t kMaxComputeBindings = 16;  // anim_eval = 7 bindings (#231)

inline WGPUBindGroupLayout MakeComputeSetLayout(
    WGPUDevice device, const DynamicBinding* bindings, size_t count) {
    WGPUBindGroupLayoutEntry e[kMaxComputeBindings] = {};
    const size_t n = count < kMaxComputeBindings ? count : kMaxComputeBindings;
    for (size_t i = 0; i < n; ++i) {
        e[i].binding = bindings[i].slot;
        e[i].visibility = WGPUShaderStage_Compute;
        e[i].buffer.type = bindings[i].kind == BufferKind::kUniform
                               ? WGPUBufferBindingType_Uniform
                               : WGPUBufferBindingType_Storage;
        e[i].buffer.hasDynamicOffset = bindings[i].has_dynamic_offset ? 1 : 0;
        e[i].buffer.minBindingSize = 0;
    }
    WGPUBindGroupLayoutDescriptor d = {};
    d.entryCount = n;
    d.entries = e;
    return wgpuDeviceCreateBindGroupLayout(device, &d);
}

// #231 anim_eval set 0: 7 bindings matching MakeComputeSetLayout's output for
// the ae_b[] DynamicBindings (binding 0 = dynamic UBO, 1-6 = packed storage).
// Built here from desc.layout==kAnimEval (like vk's anim_eval_layout_) so the
// kernel's pipeline layout exists independent of when dyn_anim_eval_'s backings
// resolve; wgpu-native dedups it against the bind group's identical descriptor.
// All storage stays read_write (matching MakeComputeSetLayout + the wgsl's
// read_write declarations) so the two descriptors stay byte-identical.
inline WGPUBindGroupLayout MakeAnimEvalSetLayout(WGPUDevice device) {
    WGPUBindGroupLayoutEntry e[7] = {};
    for (uint32_t i = 0; i < 7; ++i) {
        e[i].binding = i;
        e[i].visibility = WGPUShaderStage_Compute;
        if (i == 0) {
            e[i].buffer.type = WGPUBufferBindingType_Uniform;
            e[i].buffer.hasDynamicOffset = 1;
        } else {
            e[i].buffer.type = WGPUBufferBindingType_Storage;
        }
        e[i].buffer.minBindingSize = 0;
    }
    WGPUBindGroupLayoutDescriptor d = {};
    d.entryCount = 7;
    d.entries = e;
    return wgpuDeviceCreateBindGroupLayout(device, &d);
}

// Skin kernel set 0 (webgpu fold of vk group A+B). 7 bindings, all NON-dynamic
// (the recorder bakes the 256-aligned params/inst_meta offsets into per-batch
// bind groups; pos/skin/palette/pool bind whole at 0). 0=params UBO,
// 1=palette SSBO, 2=inst_meta SSBO, 3=out_pos SSBO, 4=mesh_pos SSBO,
// 5=skin_packed SSBO, 6=WebBases UBO (per-mesh element bases).
inline WGPUBindGroupLayout MakeSkinSetLayout(WGPUDevice device) {
    // 0,6 uniform; 3 (out_pos) read_write storage; 1,2,4,5 read-only storage so
    // the shared kDynamic master (uniforms + inst_meta) stays all-reads.
    WGPUBindGroupLayoutEntry e[7] = {};
    for (uint32_t i = 0; i < 7; ++i) {
        e[i].binding = i;
        e[i].visibility = WGPUShaderStage_Compute;
        if (i == 0 || i == 6) {
            e[i].buffer.type = WGPUBufferBindingType_Uniform;
        } else if (i == 3) {
            e[i].buffer.type = WGPUBufferBindingType_Storage;
        } else {
            e[i].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        }
        e[i].buffer.minBindingSize = 0;
    }
    WGPUBindGroupLayoutDescriptor d = {};
    d.entryCount = 7;
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
