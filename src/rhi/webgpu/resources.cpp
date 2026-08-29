// rhi/webgpu/resources.cpp -- WebGPU backend (W3: real buffer/texture create +
// upload + readback; the rest stubbed pending W4/W5).
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include <cstring>
#include <vector>

#include "rhi/resources.hpp"
#include "rhi/device.hpp"
#include "rhi/allocator.hpp"
#include "rhi/frames.hpp"
#include "rhi/pipelines.hpp"

#include <webgpu/webgpu.h>
#include <cstdio>

#include <webgpu/wgpu.h>

namespace cairns::rhi {

namespace {
WGPUTextureFormat ToWgpuFormat(Format f) {
    switch (f) {
        case Format::kR8Unorm: return WGPUTextureFormat_R8Unorm;
        case Format::kRgba8Unorm: return WGPUTextureFormat_RGBA8Unorm;
        case Format::kRgba8Srgb: return WGPUTextureFormat_RGBA8UnormSrgb;
        case Format::kBgra8Unorm: return WGPUTextureFormat_BGRA8Unorm;
        case Format::kBgra8Srgb: return WGPUTextureFormat_BGRA8UnormSrgb;
        case Format::kR16F: return WGPUTextureFormat_R16Float;
        case Format::kRgba16F: return WGPUTextureFormat_RGBA16Float;
        case Format::kR32F: return WGPUTextureFormat_R32Float;
        case Format::kRgba32F: return WGPUTextureFormat_RGBA32Float;
        case Format::kRg32F: return WGPUTextureFormat_RG32Float;
        case Format::kD32F: return WGPUTextureFormat_Depth32Float;
        case Format::kD24S8: return WGPUTextureFormat_Depth24PlusStencil8;
        case Format::kR32Uint: return WGPUTextureFormat_R32Uint;
        default: return WGPUTextureFormat_RGBA8Unorm;
    }
}
WGPUTextureUsage ToWgpuTexUsage(TextureUsage u) {
    uint64_t out = WGPUTextureUsage_CopySrc;  // always allow readback
    if (u & kTexUsageSampled) out |= WGPUTextureUsage_TextureBinding;
    if (u & kTexUsageStorage) out |= WGPUTextureUsage_StorageBinding;
    if (u & kTexUsageColorTarget) out |= WGPUTextureUsage_RenderAttachment;
    if (u & kTexUsageDepthTarget) out |= WGPUTextureUsage_RenderAttachment;
    if (u & kTexUsageTransferDst) out |= WGPUTextureUsage_CopyDst;
    return static_cast<WGPUTextureUsage>(out);
}
WGPUFilterMode ToWgpuFilter(Filter f) {
    return f == Filter::kNearest ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
}
WGPUMipmapFilterMode ToWgpuMipFilter(Filter f) {
    return f == Filter::kNearest ? WGPUMipmapFilterMode_Nearest
                                 : WGPUMipmapFilterMode_Linear;
}
WGPUAddressMode ToWgpuAddress(AddressMode m) {
    switch (m) {
        case AddressMode::kRepeat: return WGPUAddressMode_Repeat;
        case AddressMode::kMirroredRepeat: return WGPUAddressMode_MirrorRepeat;
        case AddressMode::kClampToEdge: return WGPUAddressMode_ClampToEdge;
        case AddressMode::kClampToBorder: return WGPUAddressMode_ClampToEdge;
        default: return WGPUAddressMode_Repeat;
    }
}
}  // namespace

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

Handle<Buffer> Resources::CreateBuffer(Allocator& alloc, const BufferDesc& d) {
    webgpu::AllocResult r = alloc.plat.memory_.AllocBuffer(d.byte_size, d.usage, d.memory, 16);
    if (!r.ok) { return Handle<Buffer>::Null; }
    Handle<Buffer> h = buffers.Acquire();
    Buffer::Hot* hot = buffers.GetHot(h);
    hot->heap_buffer_index = static_cast<uint16_t>(r.heap_index);
    hot->pad = 0;
    hot->offset_in_heap = r.offset;
    Buffer::Cold* cold = buffers.GetCold(h);
    cold->size_bytes = d.byte_size;
    cold->usage = d.usage;
    cold->mem_type = d.memory;
    cold->debug_name = d.debug_name;
    if (!d.initial_data.empty()) { UploadBuffer(alloc, h, 0, d.initial_data); }
    return h;
}

void Resources::UploadBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t dst_offset,
                              std::span<const uint8_t> data) {
    if (data.empty()) { return; }
    Buffer::Hot* hot = GetHot(h);
    if (!hot) { return; }
    WGPUBuffer dst = alloc.plat.memory_.HeapMasterBuffer(hot->heap_buffer_index);
    if (!dst) { return; }
    wgpuQueueWriteBuffer(plat.queue_, dst, hot->offset_in_heap + dst_offset,
                         data.data(), data.size());
}

Handle<Texture> Resources::CreateTexture(Allocator& alloc, const TextureDesc& d) {
    (void)alloc;
    WGPUTextureDescriptor td = {};
    td.usage = ToWgpuTexUsage(d.usage);
    td.dimension = WGPUTextureDimension_2D;
    td.size = {static_cast<uint32_t>(d.dimensions.x), static_cast<uint32_t>(d.dimensions.y),
               d.array_layers};
    td.format = ToWgpuFormat(d.format);
    td.mipLevelCount = d.mip_levels;
    td.sampleCount = d.sample_count;
    WGPUTexture tex = wgpuDeviceCreateTexture(plat.device_, &td);
    if (!tex) { return Handle<Texture>::Null; }
    WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
    Handle<Texture> h = textures.Acquire();
    Texture::Hot* hot = textures.GetHot(h);
    hot->api_view = static_cast<void*>(view);
    Texture::Cold* cold = textures.GetCold(h);
    cold->api_image = static_cast<void*>(tex);
    cold->width = static_cast<uint32_t>(d.dimensions.x);
    cold->height = static_cast<uint32_t>(d.dimensions.y);
    return h;
}

Handle<Sampler> Resources::CreateSampler(const SamplerDesc& d) {
    WGPUSamplerDescriptor sd = {};
    const WGPUAddressMode addr = ToWgpuAddress(d.address_mode);
    sd.addressModeU = addr;
    sd.addressModeV = addr;
    sd.addressModeW = addr;
    sd.magFilter = ToWgpuFilter(d.mag_filter);
    sd.minFilter = ToWgpuFilter(d.min_filter);
    sd.mipmapFilter = ToWgpuMipFilter(d.mip_filter);
    sd.lodMinClamp = 0.0f;
    sd.lodMaxClamp = d.max_lod > 0.0f ? d.max_lod : 32.0f;
    // WebGPU only permits anisotropy > 1 when min/mag/mip are all Linear; metal/
    // vk silently ignore it for nearest filters, so clamp to 1 to match them.
    const bool all_linear = d.mag_filter == Filter::kLinear &&
                            d.min_filter == Filter::kLinear &&
                            d.mip_filter == Filter::kLinear;
    sd.maxAnisotropy = (all_linear && d.max_anisotropy > 1.0f)
                           ? static_cast<uint16_t>(d.max_anisotropy) : 1;
    sd.compare = WGPUCompareFunction_Undefined;
    WGPUSampler s = wgpuDeviceCreateSampler(plat.device_, &sd);
    if (!s) { return Handle<Sampler>::Null; }
    Handle<Sampler> h = samplers.Acquire();
    samplers.GetHot(h)->api_sampler = static_cast<void*>(s);
    samplers.GetCold(h)->debug_name = d.debug_name;
    return h;
}
Handle<BindGroup> Resources::CreateBindGroup(const BindGroupDesc& d) { (void)d; return Handle<BindGroup>::Null; }
Handle<BindGroup> Resources::CreateSkinGroupA(Allocator& a, Frames& f, Pipelines& p, const BindGroupDesc& d) { (void)a; (void)f; (void)p; (void)d; return Handle<BindGroup>::Null; }
Handle<DynamicBuffers> Resources::CreateDynamicBuffers(const DynamicBuffersDesc& d) {
    Handle<DynamicBuffers> h = dynamic_buffers.Acquire();
    DynamicBuffers::Cold* cold = dynamic_buffers.GetCold(h);
    cold->layout.assign(d.bindings.begin(), d.bindings.end());
    cold->debug_name = d.debug_name;
    DynamicBuffers::Hot* hot = dynamic_buffers.GetHot(h);
    hot->binding_count = static_cast<uint8_t>(d.bindings.size());
    return h;
}
Handle<DynamicBuffers> Resources::CreateDynamicBuffers(Allocator& a, Frames& f, const DynamicBuffersDesc& d) {
    (void)a; (void)f;
    return CreateDynamicBuffers(d);
}

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

Buffer::Hot* Resources::GetHot(Handle<Buffer> h) { return buffers.GetHot(h); }
Texture::Hot* Resources::GetHot(Handle<Texture> h) { return textures.GetHot(h); }
Sampler::Hot* Resources::GetHot(Handle<Sampler> h) { return samplers.GetHot(h); }
BindGroup::Hot* Resources::GetHot(Handle<BindGroup> h) { return bind_groups.GetHot(h); }
DynamicBuffers::Hot* Resources::GetHot(Handle<DynamicBuffers> h) { return dynamic_buffers.GetHot(h); }
Shader::Hot* Resources::GetHot(Handle<Shader> h) { return shaders.GetHot(h); }
Kernel::Hot* Resources::GetHot(Handle<Kernel> h) { return kernels.GetHot(h); }

uint32_t Resources::GetBufferByteSize(Handle<Buffer> h) {
    Buffer::Cold* c = buffers.GetCold(h);
    return c ? c->size_bytes : 0;
}
uint32_t Resources::BufferBaseOffset(Allocator& a, Handle<Buffer> h) {
    (void)a;
    Buffer::Hot* hot = buffers.GetHot(h);
    return hot ? hot->offset_in_heap : 0;
}

bool Resources::ReadBackTextureRgba(Handle<Texture> h, std::vector<uint8_t>& out,
                                    uint32_t& ow, uint32_t& oh) {
    Texture::Cold* cold = textures.GetCold(h);
    if (!cold || !cold->api_image) { return false; }
    WGPUTexture tex = static_cast<WGPUTexture>(cold->api_image);
    const uint32_t w = cold->width;
    const uint32_t ht = cold->height;
    if (!w || !ht) { return false; }
    const uint32_t unpadded = w * 4u;
    const uint32_t padded = (unpadded + 255u) & ~255u;

    WGPUBufferDescriptor bd = {};
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    bd.size = static_cast<uint64_t>(padded) * ht;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(plat.device_, &bd);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(plat.device_, nullptr);
    WGPUTexelCopyTextureInfo src = {};
    src.texture = tex;
    src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst = {};
    dst.buffer = buf;
    dst.layout.bytesPerRow = padded;
    dst.layout.rowsPerImage = ht;
    WGPUExtent3D ext = {w, ht, 1};
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(plat.queue_, 1, &cmd);

    bool done = false;
    WGPUBufferMapCallbackInfo mcb = {};
    mcb.mode = WGPUCallbackMode_AllowProcessEvents;
    mcb.callback = [](WGPUMapAsyncStatus, WGPUStringView, void* u1, void*) { *static_cast<bool*>(u1) = true; };
    mcb.userdata1 = &done;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size, mcb);
    for (int i = 0; i < 4000 && !done; ++i) { wgpuDevicePoll(plat.device_, true, nullptr); }
    const uint8_t* data = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(buf, 0, bd.size));
    if (!data) { wgpuBufferRelease(buf); return false; }
    out.resize(static_cast<size_t>(unpadded) * ht);
    // final_target_ is BGRA8Unorm; swizzle to RGBA8 (matches metal/vk readback
    // so the macos-webgpu goldens are comparable).
    for (uint32_t y = 0; y < ht; ++y) {
        const uint8_t* srow = data + static_cast<size_t>(y) * padded;
        uint8_t* drow = out.data() + static_cast<size_t>(y) * unpadded;
        for (uint32_t x = 0; x < w; ++x) {
            drow[x * 4 + 0] = srow[x * 4 + 2];
            drow[x * 4 + 1] = srow[x * 4 + 1];
            drow[x * 4 + 2] = srow[x * 4 + 0];
            drow[x * 4 + 3] = srow[x * 4 + 3];
        }
    }
    wgpuBufferUnmap(buf);
    wgpuBufferRelease(buf);
    ow = w;
    oh = ht;
    return true;
}

bool Resources::ReadBackTextureR32UTexel(Handle<Texture> h, uint32_t x, uint32_t y, uint32_t& out) { (void)h; (void)x; (void)y; (void)out; return false; }
bool Resources::ReadBackBuffer(Allocator& a, Handle<Buffer> h, uint32_t bytes, std::vector<uint8_t>& out) { (void)a; (void)h; (void)bytes; (void)out; return false; }
bool Resources::ClearColorTexture(Handle<Texture> h, const float color[4]) { (void)h; (void)color; return false; }

SwapResolveTarget Resources::MakeSurfacelessSwapResolveTarget(Handle<Texture> h, uint32_t w, uint32_t h_px) {
    SwapResolveTarget t{};
    t.width = w;
    t.height = h_px;
    Texture::Hot* hot = textures.GetHot(h);
    Texture::Cold* cold = textures.GetCold(h);
    if (hot) { t.plat.view = static_cast<WGPUTextureView>(hot->api_view); }
    if (cold) { t.plat.texture = static_cast<WGPUTexture>(cold->api_image); }
    return t;
}

void Resources::AdvanceFrame(Allocator& a) { (void)a; ++plat.frame_index_; }
uint32_t Resources::FrameIndex() const { return plat.frame_index_; }

WGPUBuffer ResourcesPlat::GetWgpuBuffer(Allocator& a, Handle<Buffer> h, uint32_t* out_offset) {
    Buffer::Hot* hot = resources_ ? resources_->GetHot(h) : nullptr;
    if (!hot) { if (out_offset) *out_offset = 0; return nullptr; }
    if (out_offset) { *out_offset = hot->offset_in_heap; }
    return a.plat.memory_.HeapMasterBuffer(hot->heap_buffer_index);
}
uint8_t* ResourcesPlat::MappedPtr(Allocator& a, Handle<Buffer> h) { (void)a; (void)h; return nullptr; }
WGPUBuffer ResourcesPlat::GetBumpMasterBuffer(Allocator& a, Memory mem) const {
    return a.plat.memory_.HeapMasterBuffer(a.plat.memory_.BumpMasterHeapIndex(mem));
}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
