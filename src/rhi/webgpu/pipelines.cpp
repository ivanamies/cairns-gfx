// rhi/webgpu/pipelines.cpp -- WebGPU backend (W2 stubs; WGSL pipelines in W4).
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/pipelines.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"
#include "rhi/frames.hpp"

namespace cairns::rhi {

bool Pipelines::Init(Device& device) { plat.device_ = device.plat.device; inited_ = true; return true; }
void Pipelines::Deinit(Resources& resources) { (void)resources; }

Handle<Shader> Pipelines::CreateGraphicsPipeline(Resources& resources, Frames& frames,
                                                 const GraphicsPipelineDesc& desc) {
    (void)resources; (void)frames; (void)desc;
    return Handle<Shader>::Null;
}
Handle<Kernel> Pipelines::CreateComputePipeline(Resources& resources, Frames& frames,
                                                const ComputePipelineDesc& desc) {
    (void)resources; (void)frames; (void)desc;
    return Handle<Kernel>::Null;
}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
