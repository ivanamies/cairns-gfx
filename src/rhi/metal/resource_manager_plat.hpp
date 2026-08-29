// rhi/metal/resource_manager_plat.hpp
//
// Metal side of the resource_manager seam: typed API-handle aliases that
// the cross-backend Texture/Sampler/Shader/Kernel Hot structs use, plus
// the per-resource Plat sub-structs (Texture::Cold's vk-only image-layout
// cache; Shader::Hot/Kernel::Hot's vk pipeline/layout/desc-set fields).
// All Plat structs are empty on Metal -- the api_view/api_image/api_pso
// pointers in the Hot structs already carry the full Metal handle.

#pragma once

namespace MTL {
class Texture;
class SamplerState;
class RenderPipelineState;
class ComputePipelineState;
class Buffer;
class Device;
class CommandQueue;
class Fence;
}

namespace cairns::rhi {

using ApiTextureHandle = MTL::Texture*;
using ApiSamplerHandle = MTL::SamplerState*;
using ApiPsoHandle = MTL::RenderPipelineState*;
using ApiArgBufferHandle = MTL::Buffer*;
using ApiKernelHandle = MTL::ComputePipelineState*;

// The metal leaf of the graph's per-resource barrier: the writing pass signals
// this fence at EndRenderPass, a later pass that hazards on this texture waits
// it at BeginRenderPass. Lazy-created, reused across frames (the fence persists
// with Texture::Cold) -> cross-frame WAW sync. Driven by the graph's computed
// barriers, NOT ad-hoc (Granite physical_events leaf).
struct TextureColdPlat {
    MTL::Fence* sync_fence_ = nullptr;
};
struct ShaderHotPlat {};
struct KernelHotPlat {};
// Metal has no descriptor objects; DynamicBuffers plat is
// empty. Recorder reads bindings from Cold's layout vector per draw.
struct DynamicBuffersHotPlat {};

struct BackendInitParams {
    MTL::Device* device = nullptr;
    MTL::CommandQueue* queue = nullptr;
};

}  // namespace cairns::rhi
