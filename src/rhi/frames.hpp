// rhi/frames.hpp
//
// Fork C frame lifecycle: owns per-frame sync + command buffers + the per-frame
// descriptor sets (Vulkan) / render-pass + MSAA/depth targets (Metal) + the
// one-shot swapchain dump. Begin() returns a FrameContext whose CommandRecorder
// records into the frame; End() submits + presents. Depends on Device + Resources.

#pragma once

#include "util/define.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "rhi/command_recorder.hpp"  // FrameContext
#if CAIRNS_VULKAN
#include <vulkan/vulkan.h>
#elif CAIRNS_METAL
#include <Metal/Metal.hpp>
#endif

namespace cairns::rhi {

class Device;
class Resources;
class Allocator;
struct SwapChain;

class Frames {
public:
    Frames() = default;
    ~Frames();
    Frames(const Frames&) = delete;
    Frames& operator=(const Frames&) = delete;

    // CALLER: ENGINE.
    [[nodiscard]] bool Init(Device& device);
    // CALLER: ENGINE.
    void Deinit();

    // Metal: create MSAA/depth render targets + render-pass descriptor (called
    // after scene textures load). Vulkan: no-op (targets created in SwapChain).
    // CALLER: ENGINE.
    [[nodiscard]] bool InitTargets(Resources& resources, Allocator& alloc, SwapChain& sc);

    // Headless mode: the swap-pass resolve target is final_target_ rather
    // than the swapchain drawable. Engine calls this once after final_target_
    // is allocated. Subsequent Begin/End skip drawable acquisition + present.
#if CAIRNS_METAL
    void SetHeadlessSwapTarget(MTL::Texture* tex) { headless_swap_target_ = tex; }
#elif CAIRNS_VULKAN
    void SetHeadlessSwapTarget(VkImage img, VkImageView view, VkFormat fmt,
                                uint32_t w, uint32_t h) {
        headless_swap_image_ = img;
        headless_swap_view_ = view;
        headless_swap_format_ = fmt;
        headless_swap_w_ = w;
        headless_swap_h_ = h;
    }
#endif
    bool IsHeadless() const {
#if CAIRNS_METAL
        return headless_swap_target_ != nullptr;
#elif CAIRNS_VULKAN
        return headless_swap_image_ != VK_NULL_HANDLE;
#else
        return false;
#endif
    }

    // CALLER: ENGINE (per-frame draw loop).
    FrameContext Begin(Resources& resources, Allocator& alloc, SwapChain& sc);
    void End(SwapChain& sc, FrameContext& fc);

    // Request a one-shot swapchain dump on the next End(). CALLER: ENGINE.
    void SetDumpPath(const std::filesystem::path& path);

    // Frame state; Pipelines reads the *_layout_ set layouts (vk pipeline layouts).
#if CAIRNS_VULKAN
    VkDevice device_ = VK_NULL_HANDLE;          // mirrored from Device
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue compute_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    uint32_t frames_in_flight_ = 0;
    uint32_t recorder_frame_ = 0;
    std::vector<VkCommandBuffer> graphics_cmds_;
    std::vector<VkCommandBuffer> compute_cmds_;
    std::vector<VkSemaphore> image_available_;
    std::vector<VkSemaphore> render_finished_;
    std::vector<VkSemaphore> compute_finished_;
    std::vector<VkFence> in_flight_;
    std::vector<VkFence> compute_in_flight_;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout globals_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout drawtmp_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout compute_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout point_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> globals_sets_;
    std::vector<VkDescriptorSet> drawtmp_sets_;
    // One DescriptorSet per in-flight slot per sim step. Indexed
    // [frame_in_flight][step_index]. Multi-step compute needs distinct sets
    // because vkUpdateDescriptorSets on an in-use set is UB.
    std::vector<std::array<VkDescriptorSet, kMaxStepsPerFrame>> compute_sets_;
    std::vector<VkDescriptorSet> point_sets_;
    // Per-frame ring of composite descriptor sets for DrawFullscreen. Lets one
    // pass issue multiple fullscreen draws with distinct textures (the 997af20
    // last-bound-wins fix).
    std::vector<std::array<VkDescriptorSet, kCompositeRingSize>> composite_sets_;
    OffscreenTargetCache offscreen_target_cache_;
    VkQueryPool ts_pool_ = VK_NULL_HANDLE;
    float ts_period_ns_ = 0.0f;
    bool host_query_reset_ = false;
    PFN_vkResetQueryPool vk_reset_query_pool_ = nullptr;
    std::vector<std::array<const char*, kMaxPasses>> pass_names_;
    std::vector<uint32_t> pass_count_;
#elif CAIRNS_METAL
    MTL::Device* device_ = nullptr;             // mirrored from Device
    MTL::CommandQueue* queue_ = nullptr;        // mirrored from Device
    void* frame_semaphore_ = nullptr;           // dispatch_semaphore_t
    MTL::RenderPassDescriptor* render_pass_desc_ = nullptr;
    MTL::DepthStencilState* depth_stencil_ = nullptr;
    Handle<Texture> msaa_handle_ = Handle<Texture>::Null;
    Handle<Texture> depth_handle_ = Handle<Texture>::Null;
    MTL::Texture* headless_swap_target_ = nullptr;  // null in windowed mode
#endif
#if CAIRNS_VULKAN
    VkImage headless_swap_image_ = VK_NULL_HANDLE;
    VkImageView headless_swap_view_ = VK_NULL_HANDLE;
    VkFormat headless_swap_format_ = VK_FORMAT_UNDEFINED;
    uint32_t headless_swap_w_ = 0;
    uint32_t headless_swap_h_ = 0;
#endif
    std::filesystem::path dump_path_;

private:
    bool inited_ = false;
};

}  // namespace cairns::rhi
