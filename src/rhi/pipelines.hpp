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
    ~Pipelines();
    Pipelines(const Pipelines&) = delete;
    Pipelines& operator=(const Pipelines&) = delete;

    [[nodiscard]] bool Init(Device& device, Resources& resources,
                            Bindless& bindless, Frames& frames);
    void Deinit();

    Handle<Shader> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc);
    Handle<Kernel> CreateComputePipeline(const ComputePipelineDesc& desc);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace cairns::rhi
