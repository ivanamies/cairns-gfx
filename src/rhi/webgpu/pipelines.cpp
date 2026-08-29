// rhi/webgpu/pipelines.cpp -- WebGPU backend (W2 stubs; WGSL pipelines in W4).
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/pipelines.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"
#include "rhi/frames.hpp"

#include <cstdio>

namespace cairns::rhi {

bool Pipelines::Init(Device& device) { plat.device_ = device.plat.device; inited_ = true; return true; }
void Pipelines::Deinit(Resources& resources) { (void)resources; }

Handle<Shader> Pipelines::CreateGraphicsPipeline(Resources& resources, Frames& frames,
                                                 const GraphicsPipelineDesc& desc) {
    (void)frames; (void)desc;
    Handle<Shader> h = resources.shaders.Acquire();
    if (Shader::Hot* hot = resources.shaders.GetHot(h)) { hot->api_pso = nullptr; }
    return h;
}
Handle<Kernel> Pipelines::CreateComputePipeline(Resources& resources, Frames& frames,
                                                const ComputePipelineDesc& desc) {
    (void)frames; (void)desc;
    Handle<Kernel> h = resources.kernels.Acquire();
    if (Kernel::Hot* hot = resources.kernels.GetHot(h)) { hot->api_pso = nullptr; }
    return h;
}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
