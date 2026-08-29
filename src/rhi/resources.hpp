// rhi/resources.hpp
//
// The resource pools (Aaltonen "arrays you walk") + their create/destroy/get +
// pipeline creation. Depends on Device + Allocator. The 7 typed ResourceManager<T> are
// public members; convenience methods delegate to them. Handle-resolving helpers
// for backend-typed handles (VkBuffer / MTL::Buffer*) live on ResourcesPlat so
// the common header has no #if-gated public surface; the plat holds a
// back-pointer to its owning Resources for pool access. BufferBaseOffset stays
// on the common surface (backend-neutral signature).

#pragma once

#include "util/define.hpp"

#include <cstdint>
#include <span>
#include <vector>

#include "rhi/resource_manager.hpp"  // ResourceManager<T>, Handle<>, resource types, Descs
#include "rhi/swap_resolve_target.hpp"
#if CAIRNS_METAL
#include "rhi/metal/resources_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/resources_plat.hpp"
#elif CAIRNS_WEBGPU
#include "rhi/webgpu/resources_plat.hpp"
#endif

namespace cairns::rhi {

class Device;
class Allocator;
class Frames;
class Pipelines;

// Compile-time backend capability flag: surfaceless full-scene render into
// final_target_. Consult this instead of #if CAIRNS_METAL.
inline constexpr bool kSupportsSurfacelessRender = true;

class Resources {
public:
    Resources() = default;
    ~Resources();
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;

    [[nodiscard]] bool Init(Device& device, cairns::ChunkAllocator& chunk);
    void Deinit();

    Handle<Buffer> CreateBuffer(Allocator& alloc, const BufferDesc& desc);
    void UploadBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t dst_offset,
                      std::span<const uint8_t> data);
    Handle<Texture> CreateTexture(Allocator& alloc, const TextureDesc& desc);
    Handle<Sampler> CreateSampler(const SamplerDesc& desc);
    Handle<BindGroup> CreateBindGroup(const BindGroupDesc& desc);
    // Per-skinned-mesh Group A bind group. desc.buffers must carry exactly
    // two BufferBindings (slot 0 = positions slice w/ mesh-local
    // element-aligned byte offset + range, slot 1 = skin-attrs slice).
    // Vulkan: allocates from the descriptor_pool + writes both SSBO
    // descriptors. Metal: returns Null (Metal compute binds buffers
    // directly per-batch via setBuffer:offset:atIndex:). Takes Pipelines&
    // for the layout (global to PSOs), Frames& for the per-FIF pool.
    Handle<BindGroup> CreateSkinGroupA(Allocator& alloc, Frames& frames,
                                        Pipelines& pipelines,
                                        const BindGroupDesc& desc);
    Handle<DynamicBuffers> CreateDynamicBuffers(const DynamicBuffersDesc& desc);
    // Full variant. Vulkan builds a VkDescriptorSetLayout from bindings,
    // allocates one VkDescriptorSet per FIF from frames.plat.
    // descriptor_pool_, writes each against the kDynamic master at
    // offset 0 + max_range. Metal stores only Cold's layout (same as the
    // overload above).
    Handle<DynamicBuffers> CreateDynamicBuffers(Allocator& alloc, Frames& frames,
                                                  const DynamicBuffersDesc& desc);

    // Typed generational pools — public; walk them directly for debug/iteration.
    ResourceManager<Buffer> buffers;
    ResourceManager<Texture> textures;
    ResourceManager<Sampler> samplers;
    ResourceManager<BindGroup> bind_groups;
    ResourceManager<DynamicBuffers> dynamic_buffers;
    ResourceManager<Shader> shaders;
    ResourceManager<Kernel> kernels;

    // Chunk-backed fixed-capacity pools (no malloc, no realloc). Caps cover the
    // 600-GLB residency ceiling (kPrefabResidencyCap) at measured per-GLB ratios
    // with >=2x margin; over-cap aborts in ResourceManager::Acquire. Called from
    // each backend's Init.
    void ReservePools(cairns::ChunkAllocator& chunk) {
        buffers.Reserve(chunk, 8192);
        textures.Reserve(chunk, 4096);
        samplers.Reserve(chunk, 2048);
        bind_groups.Reserve(chunk, 8192);
        dynamic_buffers.Reserve(chunk, 512);
        shaders.Reserve(chunk, 128);
        kernels.Reserve(chunk, 64);
    }

    void Destroy(Allocator& alloc, Handle<Buffer> h);
    void Destroy(Allocator& alloc, Handle<Texture> h);
    void Destroy(Handle<Sampler> h);
    void Destroy(Handle<BindGroup> h);
    void Destroy(Handle<DynamicBuffers> h);
    void Destroy(Handle<Shader> h);
    void Destroy(Handle<Kernel> h);

    // Fenced deferred deletion. Enqueue a handle to be Destroy()'d
    // kFramesInFlight frames from now -- safely past the in-flight window
    // that might still reference the underlying GPU object. The drain
    // happens in Frames::Begin at the existing vkWaitForFences /
    // cmd-buffer-completion sync point; no new fence is introduced. Use
    // DeferFree() instead of Destroy() when the resource may be bound by
    // the current or last-kFIF frames (e.g. swapping a pipeline behind
    // its handle, replacing a descriptor set, evicting a prefab batch).
    // Use Destroy() directly only for one-shot teardown after Device::
    // WaitIdle() or during shutdown.
    void DeferFree(Allocator& alloc, Handle<Buffer> h);
    void DeferFree(Allocator& alloc, Handle<Texture> h);
    void DeferFree(Handle<Sampler> h);
    void DeferFree(Handle<BindGroup> h);
    void DeferFree(Handle<DynamicBuffers> h);
    void DeferFree(Handle<Shader> h);
    void DeferFree(Handle<Kernel> h);
    // Pop every queued free whose retire_frame <= |cur_frame|. The
    // caller (Frames::Begin) passes the current frame index AFTER its
    // fence/cmd-buffer wait has proven that frame (cur_frame - kFIF)
    // is GPU-done. Items pushed at frame N stamped retire_frame = N +
    // kFIF, so they retire exactly when cur_frame catches up.
    void DrainDeferredFrees(Allocator& alloc, uint32_t cur_frame);

    // Bulk-free every per-material set-2 descriptor set. Destroy(BindGroup)
    // only recycles the handle slot, never vkFreeDescriptorSets, so the
    // fixed material descriptor pool leaks a set per material across full
    // reloads until allocation fails (vk-only; Metal/WebGPU have no fixed
    // pool). Call at a full-unload point (render thread drained, all
    // materials released) so no live set2 remains.
    void ResetMaterialBindGroups();

    Buffer::Hot* GetHot(Handle<Buffer> h);
    Texture::Hot* GetHot(Handle<Texture> h);
    Sampler::Hot* GetHot(Handle<Sampler> h);
    BindGroup::Hot* GetHot(Handle<BindGroup> h);
    DynamicBuffers::Hot* GetHot(Handle<DynamicBuffers> h);
    Shader::Hot* GetHot(Handle<Shader> h);
    Kernel::Hot* GetHot(Handle<Kernel> h);

    uint32_t GetBufferByteSize(Handle<Buffer> h);

    // Byte offset of a buffer within its backing master allocation.
    uint32_t BufferBaseOffset(Allocator& alloc, Handle<Buffer> h);

    // Backend-neutral helpers used by the engine's surfaceless / dump path.
    //
    // ReadBackTextureRgba: blit |h| (BGRA8Unorm) into a host-visible buffer,
    // wait, swizzle to RGBA8, return the pixel bytes + dims. Caller writes
    // to disk (so stb stays out of rhi/).
    //
    // ClearColorTexture: one-shot clear of |h| to |color| and transition into
    // shader-read. Used by RenderHeadlessFrame's vk fallback.
    //
    // MakeSurfacelessSwapResolveTarget: build a SwapResolveTarget that writes
    // into |h| instead of a swapchain drawable. Returns a null target on vk
    // (surfaceless render-to-texture not yet wired).
    bool ReadBackTextureRgba(Handle<Texture> h, std::vector<uint8_t>& out_rgba,
                              uint32_t& out_w, uint32_t& out_h);
    // Pick readback: read one R32U texel from |h| at (x, y). Returns false if
    // the texture is invalid, the coord is out of range, or the backend
    // failed to map the readback buffer. Caller is expected to have
    // drained in-flight work targeting |h| before calling -- this helper
    // does NOT add any cross-frame synchronization beyond an immediate
    // waitUntilCompleted on its own blit command buffer.
    bool ReadBackTextureR32UTexel(Handle<Texture> h, uint32_t x, uint32_t y,
                                  uint32_t& out_value);
    // Blit |byte_size| bytes of |h| (from its base offset) into a host-visible
    // buffer, wait, copy out. Caller drains in-flight work targeting |h| first.
    // Backs CPU-side particle/skin state hashing (no render output).
    bool ReadBackBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t byte_size,
                        std::vector<uint8_t>& out);
    bool ClearColorTexture(Handle<Texture> h, const float color[4]);
    SwapResolveTarget MakeSurfacelessSwapResolveTarget(Handle<Texture> h,
                                                        uint32_t w, uint32_t h_px);

    // Native-handle resolution + MaterialSetLayout (vk-only) live on plat.
    // External callers go `res.plat.GetVkBuffer(...)` / `res.plat.GetMtlBuffer(...)`
    // / `res.plat.MaterialSetLayout()` etc. Plat holds a back-pointer to its
    // owning Resources for pool access.
    ResourcesPlat plat;

    // Advance the resource frame counter (drives deferred-free + the bump ring).
    void AdvanceFrame(Allocator& alloc);
    uint32_t FrameIndex() const;

private:
    bool inited_ = false;

    // Per-resource retire-frame FIFO (Granite / Themaister pattern) --
    // each enqueued handle carries the frame index at which it becomes
    // safe to destroy (FrameIndex at-time-of-push + kFIF).
    // DrainDeferredFrees pops from the front while the front's
    // retire_frame <= the cutoff frame the caller passes in (driven by
    // the existing fence / semaphore wait that proves frame N-kFIF is
    // done). Retire by frame index, not per-slot buckets: frees pushed
    // BETWEEN frames (reload) belong to no slot's frame.
    enum DeferKind : uint8_t {
        kDeferBuffer,
        kDeferTexture,
        kDeferSampler,
        kDeferBindGroup,
        kDeferDynamicBuffers,
        kDeferShader,
        kDeferKernel,
    };
    struct DeferEntry {
        uint16_t index = 0;
        uint16_t generation = 0;
        uint8_t kind = 0;
        uint32_t retire_frame = 0;
    };
    std::vector<DeferEntry> deferred_;

    void DeferPushRaw(uint16_t index, uint16_t generation, uint8_t kind);
};

}  // namespace cairns::rhi
