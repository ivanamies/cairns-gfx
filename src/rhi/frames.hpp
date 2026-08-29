// rhi/frames.hpp
//
// Fork C frame lifecycle: owns per-frame sync + command buffers + the per-frame
// descriptor sets (Vulkan) / render-pass + MSAA/depth targets (Metal) + the
// one-shot swapchain dump. Begin() returns a FrameContext whose CommandRecorder
// records into the frame; End() submits + presents. Depends on Device + Resources.

#pragma once

#include "util/define.hpp"

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
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;  // 2 sampled tex
    VkDescriptorSetLayout imgui_set_layout_ = VK_NULL_HANDLE;      // 1 font sampler
    std::vector<VkDescriptorSet> globals_sets_;
    std::vector<VkDescriptorSet> drawtmp_sets_;
    std::vector<VkDescriptorSet> compute_sets_;
    std::vector<VkDescriptorSet> point_sets_;
    std::vector<VkDescriptorSet> composite_sets_;
    std::vector<VkDescriptorSet> imgui_sets_;
    OffscreenTargetCache offscreen_cache_;
#elif CAIRNS_METAL
    MTL::Device* device_ = nullptr;             // mirrored from Device
    MTL::CommandQueue* queue_ = nullptr;        // mirrored from Device
    void* frame_semaphore_ = nullptr;           // dispatch_semaphore_t
    MTL::RenderPassDescriptor* render_pass_desc_ = nullptr;
    MTL::DepthStencilState* depth_stencil_ = nullptr;
    Handle<Texture> msaa_handle_ = Handle<Texture>::Null;
    Handle<Texture> depth_handle_ = Handle<Texture>::Null;
#endif
    std::filesystem::path dump_path_;

private:
    bool inited_ = false;
};

}  // namespace cairns::rhi
