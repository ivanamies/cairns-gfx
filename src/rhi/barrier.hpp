// rhi/barrier.hpp
//
// Abstract, Vulkan-shaped barrier vocabulary. The render graph computes
// synchronization in these terms (a direct copy of Themaister's Granite
// invalidate/flush model -- see README "Render graph: where we strayed from
// Granite"); each backend translates the leaf emission:
//   - vk    -> Vk{Access,PipelineStage}Flags + VkImageLayout + vkCmdPipelineBarrier
//   - metal -> MTLFence update(flush)/wait(invalidate); layout is a no-op
// Granite expresses these as Vk* directly because it is vk-only; we are vk+metal
// so the graph holds these neutral enums and the backend maps them.

#pragma once

#include <cstdint>

namespace cairns::rhi {

// Image layout. Buffers ignore it (kUndefined).
enum class BarrierLayout : uint8_t {
    kUndefined,        // discard / first use
    kColorAttachment,
    kDepthAttachment,
    kShaderRead,       // sampled / texture input
    kTransferSrc,
    kTransferDst,
    kPresent,
    kGeneral,          // storage image
};

// Access masks (Granite VkAccessFlags2 analogue). Bit flags.
enum BarrierAccess : uint32_t {
    kAccessNone          = 0,
    kAccessColorWrite    = 1u << 0,
    kAccessDepthWrite    = 1u << 1,
    kAccessShaderRead    = 1u << 2,
    kAccessShaderWrite   = 1u << 3,
    kAccessTransferRead  = 1u << 4,
    kAccessTransferWrite = 1u << 5,
};

// Pipeline stages (Granite VkPipelineStageFlags2 analogue). Bit flags.
// kPipe* (not kStage*) -- kStage* is the existing ShaderStage enum.
enum BarrierStage : uint32_t {
    kPipeNone        = 0,
    kPipeVertex      = 1u << 0,
    kPipeFragment    = 1u << 1,
    kPipeColorOutput = 1u << 2,
    kPipeDepth       = 1u << 3,
    kPipeCompute     = 1u << 4,
    kPipeTransfer    = 1u << 5,
    kPipeAllGraphics = 1u << 6,
    kPipeAllCommands = 1u << 7,
};

// ResourceBarrier (the computed per-resource barrier) lives in
// command_recorder.hpp -- it needs Handle<>, which would otherwise make this
// header circular with resource_manager.hpp.

// Per-physical-resource sync state, PERSISTENT across frames (Granite
// PipelineEvent / physical_events[]). Stored on Texture::Cold / Buffer::Cold so
// it survives the per-frame graph rebuild -> cross-frame hazards are tracked.
// (We track invalidated access/stage coarsely, not Granite's per-stage[64]
// array; the finer scoping is a deferred perf item, see TODO.)
struct PipelineEvent {
    BarrierLayout layout = BarrierLayout::kUndefined;
    uint32_t to_flush_access = kAccessNone;     // pending writes to make available
    uint32_t src_stages = kPipeNone;             // stages that produced them
    uint32_t invalidated_access = kAccessNone;   // accesses already visible
    uint32_t invalidated_stages = kPipeNone;     // stages already invalidated
};

}  // namespace cairns::rhi
