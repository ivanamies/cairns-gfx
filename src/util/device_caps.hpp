// src/util/device_caps.hpp
//
// PURE device-fit arithmetic. No Vulkan, no Metal, no GPU. Plain integers in,
// bool out. This is the CPU-side root of both failure modes from the
// 2026-06-17 S22 bisect:
//
//   * GARBLE  -- a storage-buffer binding addressed past the device's
//                maxStorageBufferRange. On Adreno 730 that range is 256 MB;
//                writes past it silently no-op (validation layers stay quiet).
//                kSkinOutputBytes was 1 GB, so every slice whose byte offset
//                landed >= 256 MB was dropped -> "half the grid garbled."
//
//   * OOM     -- total resident GPU+CPU memory exceeded the S22 lmkd budget.
//                kFramesInFlight=3 replicated the 104 MB GPU bump ring three
//                times; dropping to 2 freed ~104 MB.
//
// WHY THIS FILE EXISTS (and why it is a free function, not a method on Device):
//
//   Dependency Inversion onto *data*, not onto a vtable. SkinPoolFitsDevice
//   depends on a POD `DeviceCaps`, never on VkPhysicalDevice or MTLDevice. That
//   buys two concrete engineering wins:
//     1. It runs in a unit test with zero GPU. The 4 hours of speculation in
//        the bisect were spent re-deriving, by hand, the inequality on line
//        `SkinPoolFitsDevice` below. Now it is one assert.
//     2. All four platforms (macOS Metal, macOS MoltenVK, Android Adreno vk,
//        iOS Metal) feed the SAME function. The arithmetic therefore *cannot*
//        diverge across platforms -- only the queried cap values can. That
//        collapses the cross-platform-divergence surface down to "did each
//        backend fill DeviceCaps correctly," which is itself checkable.

#ifndef CAIRNS_UTIL_DEVICE_CAPS_HPP
#define CAIRNS_UTIL_DEVICE_CAPS_HPP

#include <cstdint>

namespace cairns {

// vec4 (16 B) is the skin-output element. The pool measures capacity in these
// units (kSkinOutputBytes / 16) and the engine binds at slice.offset * 16.
inline constexpr uint32_t kSkinVertexStride = 16u;

// The four-to-six limits the bisect taught us to care about. Backends fill it;
// the predicates below consume it.
struct DeviceCaps {
    // VkPhysicalDeviceLimits::maxStorageBufferRange (vk) / the MTL buffer-length
    // ceiling (metal). The 256 MB Adreno floor lives here.
    uint32_t max_storage_buffer_range = 0;
    // VkPhysicalDeviceLimits::maxUniformBufferRange. UBO-bound data must fit this.
    uint32_t max_uniform_buffer_range = 0;
    // Storage buffers bindable in ONE shader stage. vk
    // maxPerStageDescriptorStorageBuffers / metal buffer-arg table /
    // webgpu maxStorageBuffersPerShaderStage. WebGPU's spec FLOOR is 8 and there
    // is no portable tier at 12 (next is 16 @ ~80% of devices), so the anim_eval
    // kernel is packed to <=4; we still demand a 10-SSBO floor as headroom and
    // refuse to boot below it (see DeviceMeetsComputeRequirements).
    uint32_t max_storage_buffers_per_stage = 0;
    // How much device-local memory we are allowed to occupy before the OS
    // reclaims us. On Android this is informed by the lmkd budget, NOT the
    // physical heap size -- a 12 GB phone will still kill a 1.6 GB app.
    uint64_t resident_budget_bytes = 0;
};

// Boot floors. A device below either of these is refused at GreaterInit rather
// than left to garble (the 256 MB range is the Adreno-730 storage-buffer floor;
// the 10-SSBO count is the WebGPU-driven minimum we standardize on across all
// backends so the skin pool binds whole + the packed anim set always fits).
inline constexpr uint32_t kMinStorageBuffersPerStage = 10u;
inline constexpr uint32_t kMinStorageBufferRangeBytes = 256u * 1024u * 1024u;

constexpr bool DeviceMeetsComputeRequirements(const DeviceCaps& caps) {
    return caps.max_storage_buffers_per_stage >= kMinStorageBuffersPerStage &&
           caps.max_storage_buffer_range >= kMinStorageBufferRangeBytes;
}

// THE garble invariant. A storage buffer must fit entirely within the
// addressable range, because the engine binds the whole pool and indexes into
// it on the GPU; the largest byte offset it will dereference is
// (capacity_units * stride) == skin_output_bytes. Past that, Adreno no-ops.
constexpr bool SkinPoolFitsDevice(uint32_t skin_output_bytes,
                                  const DeviceCaps& caps) {
    return skin_output_bytes <= caps.max_storage_buffer_range;
}

// Byte offset for the GPU bind of a skin slice. Overflow-safe in uint64.
constexpr uint64_t SkinSliceByteOffset(uint32_t offset_units) {
    return static_cast<uint64_t>(offset_units) * kSkinVertexStride;
}

// The OOM invariant inputs. Everything resident, by tier. FIF-replicated tiers
// are multiplied by frames_in_flight; single-instance tiers are not.
struct MemoryFootprint {
    uint64_t skin_output_bytes  = 0;  // single instance (one persistent pool)
    uint64_t palette_out_bytes  = 0;  // single instance
    uint64_t world_scratch_bytes = 0; // single instance
    uint64_t gpu_bump_per_fif   = 0;  // upload+dynamic+readback slot sizes, per FIF
    uint64_t cpu_arena_per_slot = 0;  // kArenaBytesPerSlot, per FIF slot
    uint32_t frames_in_flight   = 1;
};

// Peak resident bytes. This crossed the S22 lmkd threshold at FIF=3 and fit
// under it at FIF=2.
constexpr uint64_t ResidentBytes(const MemoryFootprint& f) {
    return f.skin_output_bytes + f.palette_out_bytes + f.world_scratch_bytes +
           (f.gpu_bump_per_fif + f.cpu_arena_per_slot) *
               static_cast<uint64_t>(f.frames_in_flight);
}

// THE oom invariant. True iff we fit under what the OS will let us keep.
constexpr bool FitsResidentBudget(const MemoryFootprint& f,
                                  const DeviceCaps& caps) {
    return ResidentBytes(f) <= caps.resident_budget_bytes;
}

}  // namespace cairns

#endif  // CAIRNS_UTIL_DEVICE_CAPS_HPP
