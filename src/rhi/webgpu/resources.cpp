// rhi/webgpu/resources.cpp -- WebGPU backend (W2 stubs; real create/upload/
// readback in W3).
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/resources.hpp"
#include "rhi/device.hpp"
#include "rhi/allocator.hpp"
#include "rhi/frames.hpp"
#include "rhi/pipelines.hpp"

#include <webgpu/webgpu.h>

namespace cairns::rhi {

Resources::~Resources() {}

bool Resources::Init(Device& device) {
    plat.device_ = device.plat.device;
    plat.queue_ = device.plat.queue;
    plat.resources_ = this;
    plat.frame_index_ = 1;
    inited_ = true;
    return true;
}
void Resources::Deinit() { inited_ = false; }

Handle<Buffer> Resources::CreateBuffer(Allocator& a, const BufferDesc& d) { (void)a; (void)d; return Handle<Buffer>::Null; }
void Resources::UploadBuffer(Allocator& a, Handle<Buffer> h, uint32_t off, std::span<const uint8_t> data) { (void)a; (void)h; (void)off; (void)data; }
Handle<Texture> Resources::CreateTexture(Allocator& a, const TextureDesc& d) { (void)a; (void)d; return Handle<Texture>::Null; }
Handle<Sampler> Resources::CreateSampler(const SamplerDesc& d) { (void)d; return Handle<Sampler>::Null; }
Handle<BindGroup> Resources::CreateBindGroup(const BindGroupDesc& d) { (void)d; return Handle<BindGroup>::Null; }
Handle<BindGroup> Resources::CreateSkinGroupA(Allocator& a, Frames& f, Pipelines& p, const BindGroupDesc& d) { (void)a; (void)f; (void)p; (void)d; return Handle<BindGroup>::Null; }
Handle<DynamicBuffers> Resources::CreateDynamicBuffers(const DynamicBuffersDesc& d) { (void)d; return Handle<DynamicBuffers>::Null; }
Handle<DynamicBuffers> Resources::CreateDynamicBuffers(Allocator& a, Frames& f, const DynamicBuffersDesc& d) { (void)a; (void)f; (void)d; return Handle<DynamicBuffers>::Null; }

void Resources::Destroy(Allocator& a, Handle<Buffer> h) { (void)a; (void)h; }
void Resources::Destroy(Allocator& a, Handle<Texture> h) { (void)a; (void)h; }
void Resources::Destroy(Handle<Sampler> h) { (void)h; }
void Resources::Destroy(Handle<BindGroup> h) { (void)h; }
void Resources::Destroy(Handle<DynamicBuffers> h) { (void)h; }
void Resources::Destroy(Handle<Shader> h) { (void)h; }
void Resources::Destroy(Handle<Kernel> h) { (void)h; }

void Resources::DeferFree(Allocator& a, Handle<Buffer> h) { (void)a; (void)h; }
void Resources::DeferFree(Allocator& a, Handle<Texture> h) { (void)a; (void)h; }
void Resources::DeferFree(Handle<Sampler> h) { (void)h; }
void Resources::DeferFree(Handle<BindGroup> h) { (void)h; }
void Resources::DeferFree(Handle<DynamicBuffers> h) { (void)h; }
void Resources::DeferFree(Handle<Shader> h) { (void)h; }
void Resources::DeferFree(Handle<Kernel> h) { (void)h; }
void Resources::DrainDeferredFrees(Allocator& a, uint32_t cur_frame) { (void)a; (void)cur_frame; }

Buffer::Hot* Resources::GetHot(Handle<Buffer> h) { (void)h; return nullptr; }
Texture::Hot* Resources::GetHot(Handle<Texture> h) { (void)h; return nullptr; }
Sampler::Hot* Resources::GetHot(Handle<Sampler> h) { (void)h; return nullptr; }
BindGroup::Hot* Resources::GetHot(Handle<BindGroup> h) { (void)h; return nullptr; }
DynamicBuffers::Hot* Resources::GetHot(Handle<DynamicBuffers> h) { (void)h; return nullptr; }
Shader::Hot* Resources::GetHot(Handle<Shader> h) { (void)h; return nullptr; }
Kernel::Hot* Resources::GetHot(Handle<Kernel> h) { (void)h; return nullptr; }

uint32_t Resources::GetBufferByteSize(Handle<Buffer> h) { (void)h; return 0; }
uint32_t Resources::BufferBaseOffset(Allocator& a, Handle<Buffer> h) { (void)a; (void)h; return 0; }

bool Resources::ReadBackTextureRgba(Handle<Texture> h, std::vector<uint8_t>& out, uint32_t& w, uint32_t& hh) { (void)h; (void)out; (void)w; (void)hh; return false; }
bool Resources::ReadBackTextureR32UTexel(Handle<Texture> h, uint32_t x, uint32_t y, uint32_t& out) { (void)h; (void)x; (void)y; (void)out; return false; }
bool Resources::ReadBackBuffer(Allocator& a, Handle<Buffer> h, uint32_t bytes, std::vector<uint8_t>& out) { (void)a; (void)h; (void)bytes; (void)out; return false; }
bool Resources::ClearColorTexture(Handle<Texture> h, const float color[4]) { (void)h; (void)color; return false; }
SwapResolveTarget Resources::MakeSurfacelessSwapResolveTarget(Handle<Texture> h, uint32_t w, uint32_t h_px) { (void)h; (void)w; (void)h_px; return SwapResolveTarget{}; }

void Resources::AdvanceFrame(Allocator& a) { (void)a; ++plat.frame_index_; }
uint32_t Resources::FrameIndex() const { return plat.frame_index_; }

// ResourcesPlat native-handle resolution.
WGPUBuffer ResourcesPlat::GetWgpuBuffer(Allocator& a, Handle<Buffer> h, uint32_t* out_offset) { (void)a; (void)h; if (out_offset) { *out_offset = 0; } return nullptr; }
uint8_t* ResourcesPlat::MappedPtr(Allocator& a, Handle<Buffer> h) { (void)a; (void)h; return nullptr; }
WGPUBuffer ResourcesPlat::GetBumpMasterBuffer(Allocator& a, Memory mem) const { (void)a; (void)mem; return nullptr; }

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
