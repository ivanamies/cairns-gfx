// src/util/device_caps.hpp
//
// PURE device-fit arithmetic. No Vulkan, no Metal, no GPU. Plain integers in,
// bool out. Guards the two mobile failure modes:
//
//   * GARBLE -- a storage-buffer binding addressed past the device's
//               maxStorageBufferRange: writes past it silently no-op on
//               Adreno (validation layers stay quiet).
//   * OOM    -- total resident GPU+CPU memory exceeding the OS (lmkd)
//               budget; FIF-replicated tiers multiply fast.
//
// Free functions over a POD DeviceCaps, never VkPhysicalDevice/MTLDevice:
// they run in unit tests with zero GPU, and every platform (macOS Metal,
// MoltenVK, Android Adreno vk, iOS Metal) feeds the SAME arithmetic -- only
// the queried cap values can diverge, and filling DeviceCaps correctly is
// itself checkable.

#ifndef CAIRNS_UTIL_DEVICE_CAPS_HPP
#define CAIRNS_UTIL_DEVICE_CAPS_HPP

#include <cstdint>

namespace cairns {

// vec4 (16 B) is the skin-output element. The pool measures capacity in these
// units (kSkinOutputBytes / 16) and the engine binds at slice.offset * 16.
inline constexpr uint32_t kSkinVertexStride = 16u;

// The device limits the fit predicates consume. Backends fill it.
struct DeviceCaps {
    // VkPhysicalDeviceLimits::maxStorageBufferRange (vk) / the MTL
    // buffer-length ceiling (metal).
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

// Boot floor for the per-stage storage-buffer COUNT only. The packed anim_eval
// set uses 6 SSBOs; we require 10 as cross-backend headroom (WebGPU's spec floor
// is 8; native Vulkan reports far more -- the measured S22 / Adreno-730 gives
// 524288, so it clears this trivially).
//
// The storage-buffer-SIZE floor is per-platform (the skin-pool size in
// MemoryBudget, enforced by SkinPoolFitsDevice against max_storage_buffer_range):
// 128 MB on mobile, 256 MB on desktop + webgpu. 128 MB is the WebGPU spec floor
// AND the measured Adreno-730 / S22 maxStorageBufferRange (on-device 2026-06-22 --
// the old "256 MB Adreno" comment was wrong; a fixed 256 MB floor rejected the
// real S22). Desktop runs 5x the mobile actor count, whose skinned output needs
// 256 MB; desktop/webgpu ranges are GBs so the bind fits.
inline constexpr uint32_t kMinStorageBuffersPerStage = 10u;

constexpr bool DeviceMeetsComputeRequirements(const DeviceCaps& caps) {
    return caps.max_storage_buffers_per_stage >= kMinStorageBuffersPerStage;
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
