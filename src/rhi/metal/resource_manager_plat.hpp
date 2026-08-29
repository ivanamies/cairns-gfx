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
}

namespace cairns::rhi {

using ApiTextureHandle = MTL::Texture*;
using ApiSamplerHandle = MTL::SamplerState*;
using ApiPsoHandle = MTL::RenderPipelineState*;
using ApiArgBufferHandle = MTL::Buffer*;
using ApiKernelHandle = MTL::ComputePipelineState*;

struct TextureColdPlat {};
struct ShaderHotPlat {};
struct KernelHotPlat {};

struct BackendInitParams {
    MTL::Device* device = nullptr;
    MTL::CommandQueue* queue = nullptr;
};

}  // namespace cairns::rhi
