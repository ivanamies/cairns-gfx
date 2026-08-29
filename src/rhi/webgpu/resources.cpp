// rhi/webgpu/resources.cpp -- WebGPU backend: buffer/texture create + upload +
// readback.
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include <cstring>
#include <vector>

#include "rhi/resources.hpp"
#include "rhi/device.hpp"
#include "rhi/allocator.hpp"
#include "rhi/frames.hpp"
#include "rhi/pipelines.hpp"
#include "rhi/webgpu/layouts_plat.hpp"

#include <webgpu/webgpu.h>
#include <cstdio>

#include "rhi/webgpu/native_compat.hpp"  // DrainGpu (wgpuDevicePoll wrapper)

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
uint32_t BppForFormat(Format f) {
    switch (f) {
        case Format::kR8Unorm: return 1;
        case Format::kRg8Unorm: return 2;
        case Format::kR16F: return 2;
        case Format::kRgba8Unorm:
        case Format::kRgba8Srgb:
        case Format::kBgra8Unorm:
        case Format::kBgra8Srgb:
        case Format::kR32F:
        case Format::kR32Uint: return 4;
        case Format::kRgba16F:
        case Format::kRg32F: return 8;
        case Format::kRgba32F: return 16;
        default: return 0;  // compressed / unsupported: skip pixel upload
    }
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

bool Resources::Init(Device& device, cairns::ChunkAllocator& chunk) {
    ReservePools(chunk);
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
    const uint32_t bpp = BppForFormat(d.format);
    const uint32_t bw = static_cast<uint32_t>(d.dimensions.x);
    const uint32_t bh = static_cast<uint32_t>(d.dimensions.y);
    // How many mip levels does initial_data actually cover? metal/vk generate
    // the rest from mip 0; WebGPU has no auto-mipgen, so we create only the
    // filled levels -- otherwise minified surfaces sample empty (white) mips.
    uint32_t fill_levels = d.mip_levels;
    const bool upload = !d.initial_data.empty() && bpp > 0 && d.array_layers == 1;
    if (upload) {
        fill_levels = 0;
        size_t off = 0;
        for (uint32_t level = 0; level < d.mip_levels; ++level) {
            const uint32_t lw = bw >> level ? bw >> level : 1u;
            const uint32_t lh = bh >> level ? bh >> level : 1u;
            const size_t ls = static_cast<size_t>(lw * bpp) * lh;
            if (off + ls > d.initial_data.size()) { break; }
            off += ls;
            ++fill_levels;
        }
        if (fill_levels == 0) { fill_levels = 1; }
    }

    WGPUTextureDescriptor td = {};
    td.usage = ToWgpuTexUsage(d.usage);
    td.dimension = WGPUTextureDimension_2D;
    td.size = {bw, bh, d.array_layers};
    td.format = ToWgpuFormat(d.format);
    td.mipLevelCount = fill_levels;
    td.sampleCount = d.sample_count;
    WGPUTexture tex = wgpuDeviceCreateTexture(plat.device_, &td);
    if (!tex) { return Handle<Texture>::Null; }
    if (upload) {
        size_t offset = 0;
        for (uint32_t level = 0; level < fill_levels; ++level) {
            const uint32_t lw = bw >> level ? bw >> level : 1u;
            const uint32_t lh = bh >> level ? bh >> level : 1u;
            const uint32_t bytes_per_row = lw * bpp;
            const size_t level_size = static_cast<size_t>(bytes_per_row) * lh;
            WGPUTexelCopyTextureInfo dst = {};
            dst.texture = tex;
            dst.mipLevel = level;
            dst.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyBufferLayout layout = {};
            layout.bytesPerRow = bytes_per_row;
            layout.rowsPerImage = lh;
            WGPUExtent3D ext = {lw, lh, 1};
            wgpuQueueWriteTexture(plat.queue_, &dst, d.initial_data.data() + offset,
                                  level_size, &layout, &ext);
            offset += level_size;
        }
    }
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
Handle<BindGroup> Resources::CreateBindGroup(const BindGroupDesc& d) {
    // Material set (group 1): one texture+sampler. Layout is group-equivalent
    // to the unlit pipeline's group-1 layout (wgpu dedups identical descriptors).
    // depth_sample = the shadow map: depth sampleType + non-filtering sampler.
    WGPUBindGroupLayout layout =
        d.depth_sample ? webgpu::MakeDepthSampleLayout(plat.device_)
                       : webgpu::MakeMaterialLayout(plat.device_);
    WGPUBindGroupEntry entries[2] = {};
    entries[0].binding = 0;
    if (!d.textures.empty()) {
        Texture::Hot* th = textures.GetHot(d.textures[0].texture);
        if (th) { entries[0].textureView = static_cast<WGPUTextureView>(th->api_view); }
    }
    entries[1].binding = 1;
    if (!d.samplers.empty()) {
        Sampler::Hot* sh = samplers.GetHot(d.samplers[0].sampler);
        if (sh) { entries[1].sampler = static_cast<WGPUSampler>(sh->api_sampler); }
    }
    WGPUBindGroupDescriptor bgd = {};
    bgd.layout = layout;
    bgd.entryCount = 2;
    bgd.entries = entries;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(plat.device_, &bgd);
    wgpuBindGroupLayoutRelease(layout);  // bind group retains its own ref
    if (!bg) { return Handle<BindGroup>::Null; }
    Handle<BindGroup> h = bind_groups.Acquire();
    bind_groups.GetHot(h)->api_descriptor_set = static_cast<void*>(bg);
    bind_groups.GetCold(h)->debug_name = d.debug_name;
    return h;
}
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
    (void)f;
    Handle<DynamicBuffers> h = CreateDynamicBuffers(d);
    // Real bind group for the single dynamic-offset uniform case (globals +
    // drawtmp, both backed by the kDynamic bump master).
    const bool simple = d.bindings.size() == 1 &&
                        d.bindings[0].kind == BufferKind::kUniform &&
                        d.bindings[0].has_dynamic_offset;
    if (!simple) {
        // Multi-binding compute set (particle/skin/anim-eval): build the bind
        // group once -- storages over their persistent backing, the dynamic
        // uniform over the kDynamic bump master (dynamic offset supplied per
        // dispatch). Mirrors the metal per-slot setBuffer walk.
        WGPUBindGroupLayout layout = webgpu::MakeComputeSetLayout(
            plat.device_, d.bindings.data(), d.bindings.size());
        WGPUBindGroupEntry entries[webgpu::kMaxComputeBindings] = {};
        const size_t n = d.bindings.size() < webgpu::kMaxComputeBindings
                             ? d.bindings.size()
                             : webgpu::kMaxComputeBindings;
        bool ok = true;
        for (size_t i = 0; i < n; ++i) {
            const DynamicBinding& b = d.bindings[i];
            entries[i].binding = b.slot;
            if (b.backing.IsNull()) {
                entries[i].buffer = plat.GetBumpMasterBuffer(a, Memory::kDynamic);
                entries[i].offset = 0;
                entries[i].size = b.max_range ? b.max_range : 16u;
            } else {
                uint32_t off = 0;
                entries[i].buffer = plat.GetWgpuBuffer(a, b.backing, &off);
                entries[i].offset = off;
                Buffer::Cold* bc = buffers.GetCold(b.backing);
                entries[i].size =
                    b.max_range ? b.max_range : (bc ? bc->size_bytes : 0u);
            }
            if (!entries[i].buffer) { ok = false; break; }
        }
        if (!ok) { return h; }
        WGPUBindGroupDescriptor cbgd = {};
        cbgd.layout = layout;
        cbgd.entryCount = n;
        cbgd.entries = entries;
        DynamicBuffers::Hot* chot = dynamic_buffers.GetHot(h);
        chot->plat.layout = layout;
        chot->plat.sets[0] = wgpuDeviceCreateBindGroup(plat.device_, &cbgd);
        return h;
    }
    WGPUBuffer buf = d.bindings[0].backing.IsNull()
                         ? plat.GetBumpMasterBuffer(a, Memory::kDynamic)
                         : plat.GetWgpuBuffer(a, d.bindings[0].backing, nullptr);
    if (!buf) { return h; }
    WGPUBindGroupLayout layout = webgpu::MakeDynUboLayout(plat.device_);
    WGPUBindGroupEntry e = {};
    e.binding = 0;
    e.buffer = buf;
    e.offset = 0;
    e.size = d.bindings[0].max_range;
    WGPUBindGroupDescriptor bgd = {};
    bgd.layout = layout;
    bgd.entryCount = 1;
    bgd.entries = &e;
    DynamicBuffers::Hot* hot = dynamic_buffers.GetHot(h);
    hot->plat.layout = layout;  // kept alive for the bind group's lifetime
    hot->plat.sets[0] = wgpuDeviceCreateBindGroup(plat.device_, &bgd);
    return h;
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
// No fixed material descriptor pool on WebGPU -> nothing to reset.
void Resources::ResetMaterialBindGroups() {}

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
    // Desktop: DrainGpu blocks until the map resolves. Browser: DrainGpu is a
    // no-op (in-process readback is unsupported there -- the windowed path
    // presents to the canvas; capture is via CDP screenshot), so `done` stays
    // false and this returns false.
    for (int i = 0; i < 4000 && !done; ++i) { webgpu::DrainGpu(plat.device_); }
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

bool Resources::ReadBackTextureR32UTexel(Handle<Texture> h, uint32_t x,
                                         uint32_t y, uint32_t& out) {
    Texture::Cold* cold = textures.GetCold(h);
    if (!cold || !cold->api_image) { return false; }
    if (x >= cold->width || y >= cold->height) { return false; }
    WGPUTexture tex = static_cast<WGPUTexture>(cold->api_image);
    const uint32_t padded = 256u;  // copyTextureToBuffer bytesPerRow alignment

    WGPUBufferDescriptor bd = {};
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    bd.size = padded;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(plat.device_, &bd);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(plat.device_, nullptr);
    WGPUTexelCopyTextureInfo src = {};
    src.texture = tex;
    src.aspect = WGPUTextureAspect_All;
    src.origin.x = x;
    src.origin.y = y;
    WGPUTexelCopyBufferInfo dst = {};
    dst.buffer = buf;
    dst.layout.bytesPerRow = padded;
    dst.layout.rowsPerImage = 1;
    WGPUExtent3D ext = {1, 1, 1};
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(plat.queue_, 1, &cmd);

    bool done = false;
    WGPUBufferMapCallbackInfo mcb = {};
    mcb.mode = WGPUCallbackMode_AllowProcessEvents;
    mcb.callback = [](WGPUMapAsyncStatus, WGPUStringView, void* u1, void*) {
        *static_cast<bool*>(u1) = true;
    };
    mcb.userdata1 = &done;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size, mcb);
    // Desktop wgpu-native: DrainGpu blocks until the map resolves. Browser:
    // DrainGpu is a no-op (no in-process sync readback), so this returns false --
    // a click-pick in Chrome needs an async path (the highlight SET can still be
    // populated by dispatch, which drives the outline pass without a readback).
    for (int i = 0; i < 4000 && !done; ++i) { webgpu::DrainGpu(plat.device_); }
    const uint8_t* data =
        static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(buf, 0, bd.size));
    if (!data) { wgpuBufferRelease(buf); return false; }
    out = *reinterpret_cast<const uint32_t*>(data);
    wgpuBufferUnmap(buf);
    wgpuBufferRelease(buf);
    return true;
}
bool Resources::ReadBackBuffer(Allocator& a, Handle<Buffer> h, uint32_t bytes, std::vector<uint8_t>& out) {
    if (!bytes) { return false; }
    uint32_t src_off = 0;
    WGPUBuffer src = plat.GetWgpuBuffer(a, h, &src_off);
    if (!src) { return false; }
    const uint32_t copy_bytes = (bytes + 3u) & ~3u;  // copy size must be 4-aligned
    WGPUBufferDescriptor bd = {};
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    bd.size = copy_bytes;
    WGPUBuffer staging = wgpuDeviceCreateBuffer(plat.device_, &bd);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(plat.device_, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, src, src_off, staging, 0, copy_bytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(plat.queue_, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);

    bool done = false;
    WGPUBufferMapCallbackInfo mcb = {};
    mcb.mode = WGPUCallbackMode_AllowProcessEvents;
    mcb.callback = [](WGPUMapAsyncStatus, WGPUStringView, void* u1, void*) { *static_cast<bool*>(u1) = true; };
    mcb.userdata1 = &done;
    wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, copy_bytes, mcb);
    for (int i = 0; i < 4000 && !done; ++i) { webgpu::DrainGpu(plat.device_); }
    const uint8_t* data = static_cast<const uint8_t*>(
        wgpuBufferGetConstMappedRange(staging, 0, copy_bytes));
    if (!data) { wgpuBufferRelease(staging); return false; }
    out.resize(bytes);
    std::memcpy(out.data(), data, bytes);
    wgpuBufferUnmap(staging);
    wgpuBufferRelease(staging);
    return true;
}
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

void Resources::AdvanceFrame(Allocator& a) {
    // Mirror metal/vulkan: advancing the frame MUST reset the per-frame bump
    // ring (alloc.AdvanceFrame -> MemoryAllocator::BeginFrame). Dropping it let
    // the kDynamic cursor climb ~38KB/frame until BumpAllocate overflowed its
    // slot, returned nullptr, and (release: assert compiled out) memcpy'd to
    // address zero -> "corrupted heap" abort at ~frame 450. Goldens stop at
    // frame 55 so they never tripped it.
    plat.frame_index_++;
    a.AdvanceFrame(plat.frame_index_);
}
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
