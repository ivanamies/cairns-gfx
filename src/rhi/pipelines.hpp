// rhi/pipelines.hpp
//
// Graphics + compute pipeline (PSO) creation. Depends on Device + Resources
// (stores the compiled Shader/Kernel in their pools) + Bindless + Frames
// (Vulkan pipeline layouts reference their descriptor-set layouts). Owns the
// pipeline teardown for live Shader/Kernel pool entries at shutdown.

#pragma once

#include "util/define.hpp"

#include "rhi/resource_manager.hpp"  // Handle<>, Shader, Kernel, *PipelineDesc

namespace cairns::rhi {

class Device;
class Resources;
class Bindless;
class Frames;

class Pipelines {
public:
    Pipelines() = default;
    ~Pipelines() = default;
    Pipelines(const Pipelines&) = delete;
    Pipelines& operator=(const Pipelines&) = delete;

    [[nodiscard]] bool Init(Device& device);
    void Deinit(Resources& resources);

    Handle<Shader> CreateGraphicsPipeline(Resources& resources, Bindless& bindless,
                                          Frames& frames, const GraphicsPipelineDesc& desc);
    Handle<Kernel> CreateComputePipeline(Resources& resources, Frames& frames,
                                         const ComputePipelineDesc& desc);

private:
    // Internal state — self-only (nothing reaches into Pipelines).
#if CAIRNS_VULKAN
    VkDevice device_ = VK_NULL_HANDLE;  // mirrored from Device
#elif CAIRNS_METAL
    MTL::Device* device_ = nullptr;     // mirrored from Device
#endif
    bool inited_ = false;
};

}  // namespace cairns::rhi
