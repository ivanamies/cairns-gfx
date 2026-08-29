#pragma once

#include "util/define.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <filesystem>
#include <thread>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numbers>
#include <variant>

#include <condition_variable>
#include <mutex>

#include <stb_image_write.h>

#include "gfx_api.hpp"
#include "rhi/init_config.hpp"
#include "rhi/swap_chain.hpp"
#include "gpu_scene_registry.hpp"
#include "util/misc.hpp"
#include "util/std_allocator.hpp"
#include "util/render_pass_globals.hpp"
#include "util/offset_allocator.hpp"
#include "util/gltf_loader.hpp"
#include "util/debug_asset.hpp"
#include "util/draw.hpp"
#include "util/draw_key.hpp"
#include "util/material_gpu.hpp"
#include "util/scene_gpu.hpp"
#include "util/timer.hpp"
#include "util/imgui_snapshot.hpp"
#include "util/frame_clock.hpp"
#include "util/log.hpp"
#include "scene/asset_registry.hpp"
#include "scene/components.hpp"
#include "scene/world.hpp"
#include "scene/viewport.hpp"
#include "scene/selection.hpp"
#include "render/frame_packet.hpp"
#include "render/render_extract.hpp"
#include "render/render_graph.hpp"
#include "render/render_scene.hpp"
#include "render/render_thread.hpp"
#include "scene/transform_propagation.hpp"

#include <memory>
#include "rhi/rhi.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/command_recorder.hpp"
#include "util/task_guard.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"

namespace cairns {

inline static constexpr uint32_t kHotArenaMemorySize = 1 << 29;
inline static constexpr uint32_t kUboAlign = 32;
inline static constexpr uint32_t kMeshPosBindSlot = 0;

// TODO: these four DrawKey fields are mocked to 0; hook them into the RDG
// (render dependency graph) later.
inline static constexpr uint32_t kMockTranslucency = 0;
inline static constexpr uint32_t kMockViewport = 0;
inline static constexpr uint32_t kMockViewportLayer = 0;
inline static constexpr uint32_t kMockFullscreenLayer = 0;

class Engine {
public:

    using TexHandle = rhi::Handle<rhi::Texture>;
    using BufHandle = rhi::Handle<rhi::Buffer>;
    using DynBufId = uint32_t;
    using ShaderHandle = rhi::Handle<rhi::Shader>;
    using MatId = uint32_t;
    using SamplerHandle = rhi::Handle<rhi::Sampler>;
    using BindGroupId = uint32_t;

    static constexpr uint32_t kFramesInFlight = 2;

    // Per-slot storage. drawList / drawListSorted / proxies / resident_textures
    // / draw_world_matrices live here so the game thread can fill slot S while
    // the render thread reads slot ~S. Capacity grows on demand; .clear()/
    // .resize() preserve buffers across frame reuse. pending_globals etc. are
    // staged by Build and consumed by EncodeDraws.
    static constexpr int kNumViewportsPerSlot = 2;
    struct PerSlot {
        cairns::RenderProxyArrays proxies;
        std::vector<cairns::Draw, cairns::Allocator<cairns::Draw>> drawList;
        std::vector<std::pair<cairns::DrawKey, uint32_t>,
                    cairns::Allocator<std::pair<cairns::DrawKey, uint32_t>>>
            drawListSorted;
        std::vector<rhi::Handle<rhi::Texture>> resident_textures;
        std::vector<glm::mat4> draw_world_matrices;
        // Per-viewport camera state. One RenderPassGlobals upload per
        // viewport at distinct globals_offset; RecordFrame issues one
        // forward pass per viewport with the matching offset.
        std::array<cairns::rhi::RenderPassGlobals, kNumViewportsPerSlot> pending_globals{};
        std::array<glm::mat4, kNumViewportsPerSlot> pending_view_matrix{};
        std::array<float, kNumViewportsPerSlot> pending_near_z{};
        std::array<float, kNumViewportsPerSlot> pending_far_z{};
        std::array<uint32_t, kNumViewportsPerSlot> globals_offset{};
        uint32_t dt_off = 0;
        cairns::ImDrawDataSnapshot imgui_snapshot;
        cairns::FramePacket pkt{};

        explicit PerSlot(cairns::Arena& a)
            : drawList(cairns::Allocator<cairns::Draw>(a)),
              drawListSorted(
                  cairns::Allocator<std::pair<cairns::DrawKey, uint32_t>>(a)) {
            pending_view_matrix.fill(glm::mat4(1.0f));
            pending_near_z.fill(0.1f);
            pending_far_z.fill(100.0f);
        }
    };

    Engine() :
    hot_arena_mem_(malloc(kHotArenaMemorySize)),
    hot_arena_(hot_arena_mem_, kHotArenaMemorySize),
    scenes_(cairns::Allocator<cairns::Scene>(hot_arena_)),
    root_nodes_stack_cache_(cairns::Allocator<int32_t>(hot_arena_))
    {
        slots_.reserve(kFramesInFlight);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            slots_.emplace_back(hot_arena_);
        }
    }
    
    bool initSwapChain(const rhi::InitConfig& cfg) {
        if ( !rhi_.device.InitSwapChain(swapchain_, cfg)) {
            return false;
        }

        return true;
    }
    
    // SDL fires SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED on the event thread.
    // We don't synchronously touch any GPU state here -- ApplyPendingResize
    // (top of draw()) drains the render thread first.
    bool requestResizeFrameBuffer(uint32_t width, uint32_t height) {
        if (width == 0 || height == 0) {
            return true;
        }
        resize_pending_w_ = width;
        resize_pending_h_ = height;
        resize_pending_ = true;
        return true;
    }

    bool RequestViewportDump(const std::filesystem::path& path) {
        rhi_.frames.SetDumpPath(path);
        return true;
    }

    // P1 input surface for main.cpp. Both no-op under CAIRNS_CAM_POSE so a
    // byte-gate dump can't be perturbed by an event that snuck through.

    // move_input.x = right(+) / left(-), .y = up(+) / down(-),
    // .z = forward(+) / back(-). Caller multiplies by dt + speed.
    void ApplyFlyMovement(const glm::vec3& move_input) {
        if (cam_pose_override_) {
            return;
        }
        cairns::FlyController& fc = fly_[active_viewport_];
        const float cy = std::cos(fc.yaw);
        const float sy = std::sin(fc.yaw);
        const float cp = std::cos(fc.pitch);
        const float sp = std::sin(fc.pitch);
        const glm::vec3 forward(-cp * sy, sp, -cp * cy);
        // right = normalize(cross(forward, world_up)). Cheaper closed-form:
        // when world_up is (0,1,0), right = (-cy, 0, sy) (independent of pitch).
        const glm::vec3 right(-cy, 0.0f, sy);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        fc.position += right * move_input.x + up * move_input.y +
                        forward * move_input.z;
    }

    void ApplyMouseLook(float dyaw, float dpitch) {
        if (cam_pose_override_) {
            return;
        }
        cairns::FlyController& fc = fly_[active_viewport_];
        fc.yaw += dyaw;
        // Clamp pitch just inside +/-pi/2 so forward never becomes degenerate.
        constexpr float kPitchLimit = 1.55334f;
        fc.pitch = std::clamp(fc.pitch + dpitch, -kPitchLimit, kPitchLimit);
    }

    bool CamPoseOverridden() const { return cam_pose_override_; }

    // ===== P4 selection / highlight / pick. Document-side state -- the
    // selection set names "what the user (human or VLM) cares about right
    // now"; the highlight set names "what should glow". Both live on Engine
    // today; multi-world will move them onto World once #195 lands. The GPU
    // ID buffer + outline shader + the actual pick readback are a separate
    // follow-up (#204..#206); RequestPick just records the click coordinate
    // so the handler can resolve it once the GPU side is wired.

    const std::vector<cairns::SelectionTarget>& Selection() const { return selection_; }
    const std::vector<cairns::SelectionTarget>& Highlights() const { return highlights_; }
    uint32_t SelectionRevision() const { return selection_rev_; }

    void ClearSelection() {
        if (!selection_.empty()) {
            selection_.clear();
            ++selection_rev_;
        }
    }
    void SetSelection(std::vector<cairns::SelectionTarget>&& targets) {
        selection_ = std::move(targets);
        ++selection_rev_;
    }
    void AddSelection(const cairns::SelectionTarget& t) {
        for (const auto& s : selection_) {
            if (s == t) {
                return;
            }
        }
        selection_.push_back(t);
        ++selection_rev_;
    }
    void RemoveSelection(const cairns::SelectionTarget& t) {
        for (size_t i = 0; i < selection_.size(); ++i) {
            if (selection_[i] == t) {
                selection_.erase(selection_.begin() + static_cast<long>(i));
                ++selection_rev_;
                return;
            }
        }
    }

    void ClearHighlights() {
        if (!highlights_.empty()) {
            highlights_.clear();
            ++highlights_rev_;
        }
    }
    void SetHighlights(std::vector<cairns::SelectionTarget>&& targets) {
        highlights_ = std::move(targets);
        ++highlights_rev_;
    }

    // Window-pixel coords. Engine doesn't resolve the pick yet -- the GPU
    // ID buffer + readback land in a follow-up; this just records the
    // request so a future RecordFrame can copy the texel out and a future
    // tick can deliver the resolved entity.
    void RequestPick(int viewport, uint32_t x, uint32_t y) {
        pick_pending_ = true;
        pick_viewport_ = viewport;
        pick_x_ = x;
        pick_y_ = y;
    }
    bool PickPending() const { return pick_pending_; }
    int PendingPickViewport() const { return pick_viewport_; }
    uint32_t PendingPickX() const { return pick_x_; }
    uint32_t PendingPickY() const { return pick_y_; }

    // Click-to-focus: caller passes the window-x of the LMB click. Engine
    // picks the half of the swap target the click lands in. fly_/keyboard
    // input is then routed to that viewport on subsequent iterates.
    void SetActiveViewportFromClickX(float window_x) {
        const float half = static_cast<float>(FrameWidth()) /
                            static_cast<float>(kNumViewports);
        active_viewport_ = (window_x < half) ? 0 : 1;
    }
    int ActiveViewport() const { return active_viewport_; }

    // Override the deterministic-particles seed (default kept at 42 to match
    // the existing CAIRNS_DUMP byte-gate). Must be called before
    // GreaterInit's initParticles for the change to take effect.
    void SetRandomSeed(uint32_t seed) { random_seed_ = seed; }
    uint32_t GetRandomSeed() const { return random_seed_; }

    uint32_t GetFinalTargetWidth() const { return final_target_w_; }
    uint32_t GetFinalTargetHeight() const { return final_target_h_; }

    // Current logical frame dims. Windowed: tracks the swapchain. Surfaceless:
    // tracks final_target_. Single source of truth for aspect / screen_params /
    // ImGui DPI scaling -- all of which used to read swapchain_ directly and
    // were wrong by construction in surfaceless mode.
    uint32_t FrameWidth() const {
        return final_target_.IsNull() ? swapchain_.Width() : final_target_w_;
    }
    uint32_t FrameHeight() const {
        return final_target_.IsNull() ? swapchain_.Height() : final_target_h_;
    }

    // Reallocate final_target_ at the new dimensions. Surfaceless mode only.
    bool ResizeFinalTarget(uint32_t w, uint32_t h) {
        if (final_target_.IsNull()) {
            return false;
        }
        rhi_.resources.Destroy(rhi_.alloc, final_target_);
        final_target_ = rhi::Handle<rhi::Texture>::Null;
        final_target_w_ = w;
        final_target_h_ = h;
        rhi::TextureDesc td{};
        td.debug_name = "final_target";
        td.dimensions = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
        td.format = rhi::Format::kBgra8Unorm;
        td.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled |
                   rhi::kTexUsageTransferSrc;
        td.memory = rhi::Memory::kDefault;
        final_target_ = rhi_.resources.CreateTexture(rhi_.alloc, td);
        return !final_target_.IsNull();
    }

    // Surfaceless (cairns_serve) one-frame render: drives the windowed draw()
    // path once. Swap pass writes into final_target_; the engine builds a
    // SwapResolveTarget with no drawable so Frames neither acquires a
    // drawable nor presents one. Synchronous: render thread (if used)
    // drained before return; the metal/vulkan Frames::End waitUntilCompleted's
    // the render-to-texture path. Returns false if not surfaceless.
    bool RenderHeadlessFrame() {
        if (final_target_.IsNull()) {
            return false;
        }
#if CAIRNS_METAL
        return draw();
#elif CAIRNS_VULKAN
        // vk headless full-scene render not yet wired (see GreaterInit
        // comment). Fall back to a one-shot clear so io.dumpTexture sees
        // the engine's clear color until vk Frames learns the headless
        // path.
        VkImage img = static_cast<VkImage>(
            rhi_.resources.textures.GetCold(final_target_)->api_image);
        if (img == VK_NULL_HANDLE) {
            return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = rhi_.device.command_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(rhi_.device.device_, &cai, &cb) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier to_dst{};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = img;
        to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_dst.srcAccessMask = 0;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                              0, nullptr, 1, &to_dst);
        VkClearColorValue cc{};
        cc.float32[0] = 41.0f / 255.0f;
        cc.float32[1] = 42.0f / 255.0f;
        cc.float32[2] = 48.0f / 255.0f;
        cc.float32[3] = 1.0f;
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              &cc, 1, &r);
        VkImageMemoryBarrier to_shader = to_dst;
        to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                              0, nullptr, 0, nullptr, 1, &to_shader);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(rhi_.device.graphics_queue_, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(rhi_.device.graphics_queue_);
        vkFreeCommandBuffers(rhi_.device.device_, rhi_.device.command_pool_,
                              1, &cb);
        return true;
#else
        return false;
#endif
    }

    // Headless texture readback: blit final_target_ -> Shared buffer ->
    // PNG. Mirrors the windowed dump in metal/frames.cpp::End() but reads
    // from the offscreen target instead of the swapchain drawable. Apple
    // origin is top-left so no Y-flip needed (matches the windowed dump's
    // contract). BGRA -> RGBA swizzle on the host side.
    bool DumpFinalTarget(const std::filesystem::path& path) {
        if (final_target_.IsNull()) {
            return false;
        }
#if CAIRNS_METAL
        MTL::Texture* tex =
            rhi_.resources.GetHot(final_target_)->api_view;
        if (!tex) {
            return false;
        }
        const NS::UInteger w = tex->width();
        const NS::UInteger h = tex->height();
        const NS::UInteger bpr = w * 4;
        const NS::UInteger bufSize = bpr * h;
        MTL::Buffer* readback = rhi_.device.device_->newBuffer(
            bufSize, MTL::ResourceStorageModeShared);
        if (!readback) {
            return false;
        }
        MTL::CommandBuffer* cb = rhi_.device.queue_->commandBuffer();
        MTL::BlitCommandEncoder* blit = cb->blitCommandEncoder();
        blit->copyFromTexture(tex, 0, 0, MTL::Origin{0, 0, 0},
                              MTL::Size{w, h, 1}, readback, 0, bpr, 0);
        blit->endEncoding();
        cb->commit();
        cb->waitUntilCompleted();
        std::vector<uint8_t> rgba(bufSize);
        const uint8_t* bgra =
            static_cast<const uint8_t*>(readback->contents());
        for (NS::UInteger i = 0; i < w * h; ++i) {
            rgba[i * 4 + 0] = bgra[i * 4 + 2];
            rgba[i * 4 + 1] = bgra[i * 4 + 1];
            rgba[i * 4 + 2] = bgra[i * 4 + 0];
            rgba[i * 4 + 3] = bgra[i * 4 + 3];
        }
        const bool ok = stbi_write_png(
            path.string().c_str(), static_cast<int>(w),
            static_cast<int>(h), 4, rgba.data(),
            static_cast<int>(bpr)) != 0;
        readback->release();
        return ok;
#elif CAIRNS_VULKAN
        VkImage img = static_cast<VkImage>(
            rhi_.resources.textures.GetCold(final_target_)->api_image);
        if (img == VK_NULL_HANDLE) {
            return false;
        }
        const uint32_t w = final_target_w_;
        const uint32_t h = final_target_h_;
        const VkDeviceSize buf_size =
            static_cast<VkDeviceSize>(w) * h * 4;

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = buf_size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buf = VK_NULL_HANDLE;
        if (vkCreateBuffer(rhi_.device.device_, &bci, nullptr, &buf) !=
            VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(rhi_.device.device_, buf, &mr);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(rhi_.device.physical_, &mp);
        uint32_t type_idx = 0;
        bool found_type = false;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            const auto need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((mr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & need) == need) {
                type_idx = i;
                found_type = true;
                break;
            }
        }
        if (!found_type) {
            vkDestroyBuffer(rhi_.device.device_, buf, nullptr);
            return false;
        }
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = type_idx;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        vkAllocateMemory(rhi_.device.device_, &mai, nullptr, &mem);
        vkBindBufferMemory(rhi_.device.device_, buf, mem, 0);

        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = rhi_.device.command_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(rhi_.device.device_, &cai, &cb);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);

        VkImageMemoryBarrier to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_src.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = img;
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_src.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                              0, nullptr, 1, &to_src);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                buf, 1, &region);

        VkImageMemoryBarrier to_shader = to_src;
        to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_shader.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                              0, nullptr, 0, nullptr, 1, &to_shader);

        vkEndCommandBuffer(cb);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(rhi_.device.graphics_queue_, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(rhi_.device.graphics_queue_);

        void* mapped = nullptr;
        vkMapMemory(rhi_.device.device_, mem, 0, buf_size, 0, &mapped);
        std::vector<uint8_t> rgba(static_cast<size_t>(buf_size));
        const uint8_t* src = static_cast<const uint8_t*>(mapped);
        // final_target_ format is BGRA8Unorm (matches the metal path).
        for (uint32_t i = 0; i < w * h; ++i) {
            rgba[i * 4 + 0] = src[i * 4 + 2];
            rgba[i * 4 + 1] = src[i * 4 + 1];
            rgba[i * 4 + 2] = src[i * 4 + 0];
            rgba[i * 4 + 3] = src[i * 4 + 3];
        }
        vkUnmapMemory(rhi_.device.device_, mem);

        const bool ok = stbi_write_png(
            path.string().c_str(), static_cast<int>(w),
            static_cast<int>(h), 4, rgba.data(),
            static_cast<int>(w * 4)) != 0;

        vkFreeCommandBuffers(rhi_.device.device_, rhi_.device.command_pool_,
                              1, &cb);
        vkDestroyBuffer(rhi_.device.device_, buf, nullptr);
        vkFreeMemory(rhi_.device.device_, mem, nullptr);
        return ok;
#else
        (void)path;
        return false;
#endif
    }
    
    bool initCpuAllocators() {
        return true;
    }
    
    bool initResourceManagers() {
        using namespace cairns;
        using namespace cairns::rhi;
        // 4 because we're only pretending to be a real UGC engine at this point
        return true;
    }
    
    bool GreaterInit(const rhi::InitConfig& cfg) {
        // Clock selection: CAIRNS_DUMP => FixedClock (golden); else WallClock.
        golden_ = (std::getenv("CAIRNS_DUMP") != nullptr);
        tiny_quad_test_ = (std::getenv("CAIRNS_TINY_QUAD") != nullptr);

        // CAIRNS_CAM_POSE=x,y,z,yaw_rad,pitch_rad pins fly_[0] to a fixed
        // pose so byte-gate dumps are deterministic. The pre-P1 reference
        // pose -- origin looking down -Z -- is CAIRNS_CAM_POSE=0,0,0,0,0.
        if (const char* p = std::getenv("CAIRNS_CAM_POSE")) {
            float v[5] = {0};
            int n = std::sscanf(p, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2],
                                 &v[3], &v[4]);
            if (n == 5) {
                // Pin BOTH viewports' controllers to the same pose so the
                // side-by-side composite is deterministic regardless of
                // which viewport ends up active. Diverging the second
                // viewport for a multi-pose byte-gate is the P3 follow-up.
                for (int vi = 0; vi < kNumViewports; ++vi) {
                    fly_[vi].position = glm::vec3(v[0], v[1], v[2]);
                    fly_[vi].yaw = v[3];
                    fly_[vi].pitch = v[4];
                }
                cam_pose_override_ = true;
            }
        }
        if (golden_) {
            clock_ = std::make_unique<cairns::FixedClock>(cairns::kFixedDt);
        } else {
            clock_ = std::make_unique<cairns::WallClock>();
        }

        // these initializations are wrong.
        // there is a dependency graph
        // alloc gpu mem -> upload cpu to gpu mem -> draw on gpu
        // but it should be:
        // generate commands to alloc gpu mem -> upload cpu to gpu mem -> generate draw commands
        // and this can be parallelized:
        // thread 1: generate commands to alloc gpu mem -> signal fence1 -> generate draw commands -> wait for fence2 -> execute draw commands
        // thread 2: wait for fence1 -> upload cpu to gpu mem -> signal fence2
        
        if ( !initCpuAllocators() ) {
            CAIRNS_PRINT("GreaterInit: initCpuAllocators failed\n");
            return false;
        }
        if ( !initResourceManagers() ) {
            CAIRNS_PRINT("GreaterInit: initResourceManagers failed\n");
            return false;
        }
        if (!rhi_.device.Init(cfg)) {
            CAIRNS_PRINT("GreaterInit: device.Init failed\n");
            return false;
        }
        if (!rhi_.alloc.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: alloc.Init failed\n");
            return false;
        }
        if (!rhi_.resources.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: resources.Init failed\n");
            return false;
        }
        if (!rhi_.frames.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: frames.Init failed\n");
            return false;
        }
        if (!rhi_.pipelines.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: pipelines.Init failed\n");
            return false;
        }
        if (!cfg.surfaceless) {
            if ( !initSwapChain(cfg)) {
                CAIRNS_PRINT("GreaterInit: initSwapChain failed\n");
                return false;
            }
        }
        { // init debug assets
            std::vector<std::filesystem::path> glb_paths;
            if (const char* env_glb = std::getenv("CAIRNS_GLB")) {
                std::string spec(env_glb);
                size_t start = 0;
                while (start <= spec.size()) {
                    size_t comma = spec.find(',', start);
                    std::string tok = spec.substr(
                        start, comma == std::string::npos ? std::string::npos
                                                          : comma - start);
                    if (!tok.empty()) {
                        std::filesystem::path p(tok);
                        if (p.is_absolute()) {
                            glb_paths.push_back(p);
                        } else {
                            std::filesystem::path resolved;
                            if (!cairns::GetStaticResourceFilepath(tok, resolved)) {
                                return false;
                            }
                            glb_paths.push_back(resolved);
                        }
                    }
                    if (comma == std::string::npos) {
                        break;
                    }
                    start = comma + 1;
                }
            } else {
                for (size_t glb_idx = cairns::kDebugGlbsToParseStart;
                     glb_idx < cairns::kDebugGlbsToParseStart + cairns::kDebugGlbsToParse;
                     ++glb_idx) {
                    std::filesystem::path filepath;
                    if (!cairns::GetStaticResourceFilepath(cairns::kDebugGlbs[glb_idx],
                                                           filepath)) {
                        fprintf(stderr, "file missing %s\n", cairns::kDebugGlbs[glb_idx]);
                        continue;
                    }
                    glb_paths.push_back(filepath);
                }
            }

            const int kHeroSlices = 33;
            const int loaded_heroes = static_cast<int>(glb_paths.size());
            const int instance_count =
                std::getenv("CAIRNS_N") ? std::atoi(std::getenv("CAIRNS_N"))
                                        : loaded_heroes * kHeroSlices;
            const int grid_n = std::max(
                1, static_cast<int>(std::ceil(std::sqrt(
                       static_cast<float>(instance_count)))));
            const float spacing = 4.0f / static_cast<float>(grid_n);
            const float scale =
                std::getenv("CAIRNS_SCALE")
                    ? static_cast<float>(std::atof(std::getenv("CAIRNS_SCALE")))
                    : 0.013f / static_cast<float>(grid_n);
            const float start = -spacing * static_cast<float>(grid_n - 1) * 0.5f;
            debugSceneXforms_ = cairns::GenerateDebugGridTransforms(
                glm::vec3(start, start, -3), grid_n, spacing, spacing, 1.0f, scale,
                instance_count);

            for (const std::filesystem::path& filepath : glb_paths) {
                scenes_.push_back(cairns::Scene(hot_arena_));
                cairns::Scene& scene = scenes_.back();
                if (!cairns::LoadSceneFromGltf(filepath, scene)) {
                    return false;
                }
                cairns::PrepareSceneResources(scene, rhi_.resources, rhi_.alloc, materials_);
            }

            if (!cairns::rhi::LoadScenesGpu(
                    std::span<cairns::Scene>(scenes_.data(), scenes_.size()),
                    rhi_.resources, rhi_.alloc)) {
                return false;
            }

            for (cairns::Scene& scene : scenes_) {
                scene.CleanupTmps();
            }
        }
        if (!scenes_.empty() && !scenes_[0].meshes.empty()) {
            mesh_master_handle_ = scenes_[0].meshes[0].posHandle;
        }

        // EnTT scene-layer path. Register each loaded Scene with the
        // AssetRegistry, then create one entity per debug-grid xform in
        // the active world's registry. SceneEntity / SceneWorld are
        // gone -- the entt::registry IS the source of truth.
        if (!scenes_.empty()) {
            // Pre-allocate hot/cold cells up to kMaxWorlds so Acquire
            // doesn't trigger a vector growth that would move
            // World::Cold and invalidate any cached pointers. The
            // unique_ptr<entt::registry> inside Cold is the second
            // safety layer.
            for (uint32_t w = 0; w < kMaxWorlds; ++w) {
                cairns::WorldId tmp = worlds_.Acquire();
                worlds_.Release(tmp);
            }

            // Shared GPU buffer handles -- all GLBs alias the same
            // packed buffer-set (see scene_gpu.hpp).
            const auto pos_handle = scenes_[0].meshes[0].posHandle;
            const auto attr_handle = scenes_[0].meshes[0].attrHandle;
            const auto idx_handle = scenes_[0].meshes[0].indexHandle;

            std::vector<cairns::AssetId> per_scene_asset;
            per_scene_asset.reserve(scenes_.size());
            for (size_t s_idx = 0; s_idx < scenes_.size(); ++s_idx) {
                per_scene_asset.push_back(
                    assets_.RegisterExistingScene(
                        static_cast<uint32_t>(s_idx), &scenes_[s_idx],
                        pos_handle, attr_handle, idx_handle));
            }

            active_world_ = worlds_.Acquire();
            cairns::World::Hot* wh = worlds_.GetHot(active_world_);
            cairns::World::Cold* wc = worlds_.GetCold(active_world_);
            if (wh && wc) {
                // Reused-slot trap: fresh re-init in case this slot was
                // recycled. unique_ptr<registry> + dirty get reset.
                *wc = cairns::World::Cold{};
                wh->proxy_slot = 0;
                wh->dirty = true;

                auto& reg = wc->registry;
                for (size_t i = 0; i < debugSceneXforms_.size(); ++i) {
                    const uint32_t scene_idx =
                        static_cast<uint32_t>(i % scenes_.size());
                    const entt::entity e = reg.create();
                    cairns::WorldTransform wt;
                    wt.world = debugSceneXforms_[i];
                    reg.emplace<cairns::WorldTransform>(e, wt);
                    cairns::AssetRef ar;
                    ar.asset = per_scene_asset[scene_idx];
                    reg.emplace<cairns::AssetRef>(e, ar);
                    cairns::Renderable rdr;
                    rdr.layer_mask = 0xFFFFFFFFu;
                    rdr.flags = cairns::kProxyVisible;
                    reg.emplace<cairns::Renderable>(e, rdr);
                }
            }
            world_proxies_.resize(1);  // active_world_ uses slot 0

            // P6: open a SECOND world to flush single-world assumptions
            // in the type system + pool plumbing. Populated with half
            // the debug-grid for visible distinctness if anyone wires a
            // second view to it. NOT rendered yet -- the active path
            // still draws only active_world_. P6's isolation gate is
            // satisfied by "world 0 pixels unchanged when world 1
            // exists." Full side-by-side rendering (per-view targets +
            // tiled composite + composite_pip variant) is the next
            // commit on top of this seam.
            secondary_world_ = worlds_.Acquire();
            if (auto* wh2 = worlds_.GetHot(secondary_world_)) {
                if (auto* wc2 = worlds_.GetCold(secondary_world_)) {
                    *wc2 = cairns::World::Cold{};
                    wh2->proxy_slot = 1;
                    wh2->dirty = true;
                    auto& reg2 = wc2->registry;
                    const size_t half = debugSceneXforms_.size() / 2;
                    for (size_t i = 0; i < half; ++i) {
                        const uint32_t scene_idx = static_cast<uint32_t>(
                            i % scenes_.size());
                        const entt::entity e = reg2.create();
                        cairns::WorldTransform wt;
                        wt.world = debugSceneXforms_[i];
                        reg2.emplace<cairns::WorldTransform>(e, wt);
                        cairns::AssetRef ar;
                        ar.asset = per_scene_asset[scene_idx];
                        reg2.emplace<cairns::AssetRef>(e, ar);
                        cairns::Renderable rdr;
                        rdr.layer_mask = 0xFFFFFFFFu;
                        rdr.flags = cairns::kProxyVisible;
                        reg2.emplace<cairns::Renderable>(e, rdr);
                    }
                }
            }
            world_proxies_.resize(2);  // secondary_world_ uses slot 1
        }
        // Surfaceless mode: allocate the offscreen final_target_ and CONTINUE
        // through normal init. The engine -- not the RHI -- is the one that
        // decides which texture the swap pass writes into each frame: in
        // surfaceless mode it builds a SwapResolveTarget pointing at
        // final_target_; in windowed mode it pulls one out of swapchain_.
        // SwapChain and Frames have no notion of "headless" mode.
        if (cfg.surfaceless) {
            final_target_w_ = cfg.width;
            final_target_h_ = cfg.height;
            rhi::TextureDesc td{};
            td.debug_name = "final_target";
            td.dimensions = {static_cast<int32_t>(cfg.width),
                             static_cast<int32_t>(cfg.height), 1};
            td.format = rhi::Format::kBgra8Unorm;
            td.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled |
                       rhi::kTexUsageTransferSrc;
            td.memory = rhi::Memory::kDefault;
            final_target_ = rhi_.resources.CreateTexture(rhi_.alloc, td);
            if (final_target_.IsNull()) {
                CAIRNS_PRINT("GreaterInit: final_target_ create failed\n");
                return false;
            }
#if !CAIRNS_METAL
            // vk render-to-texture not yet wired (needs a render_pass +
            // framebuffer over final_target_'s VkImageView, replacing the
            // SwapChain ones). RenderHeadlessFrame falls back to a clear.
            return true;
#endif
        }
        if ( !initRenderPipeline() ) {
            CAIRNS_PRINT("GreaterInit: initRenderPipeline failed\n");
            return false;
        }
        const uint32_t init_w = cfg.surfaceless ? cfg.width : swapchain_.Width();
        const uint32_t init_h = cfg.surfaceless ? cfg.height : swapchain_.Height();
        if ( !rhi_.frames.InitTargets(rhi_.resources, rhi_.alloc, init_w, init_h) ) {
            CAIRNS_PRINT("GreaterInit: frames.InitTargets failed\n");
            return false;
        }
        if ( !initParticles() ) {
            CAIRNS_PRINT("GreaterInit: initParticles failed\n");
            return false;
        }
        render_thread_ = std::make_unique<cairns::RenderThread>(
            [this](cairns::FramePacket& pkt) { this->RecordFrame(pkt); });
        return true;
    }

    bool BuildMeshOpaqueDraws(uint32_t slot) {
        PerSlot& s = slots_[slot];
        // CPU-side scene build only. NO bump allocations -- those happen in
        // EncodeDraws() on the render-thread side post-split. Stable-index
        // writes (resize + index assignment) so the output is independent of
        // walk/execution order.
        // Rotation reads sim, not wall: render_angle_deg_ is sim_angle_deg_
        // plus an interpolation in [0, kFixedDt) toward the next sim step.
        const float angle_degs = render_angle_deg_;
        const float angle_rads = angle_degs * std::numbers::pi / 180.0f;
        const glm::mat4 rot_matrix = glm::rotate(glm::mat4(1.0f), angle_rads, glm::vec3(0, 1.0, 0));

        // Per-viewport camera resolve. Each viewport gets its own
        // RenderPassGlobals (uploaded at a distinct bump offset by
        // EncodeDraws); RecordFrame issues one forward pass per viewport
        // bound against the matching offset. Aspect is (vp_w / vp_h) where
        // vp_w = FrameWidth() / kNumViewportsPerSlot (side-by-side split).
        const float vp_w = static_cast<float>(FrameWidth()) /
                            static_cast<float>(kNumViewportsPerSlot);
        const float vp_h = static_cast<float>(FrameHeight());
        const float aspect_ratio = vp_w / vp_h;
        const float fov = 90 * (std::numbers::pi / 180.0f);
        const float near_z = 0.1f;
        const float far_z = 100.0f;
        const glm::mat4 proj_matrix = glm::perspectiveRH_ZO(fov, aspect_ratio, near_z, far_z);
        for (int v = 0; v < kNumViewports; ++v) {
            cairns::FlyController& fc = fly_[v];
            const float cy = std::cos(fc.yaw);
            const float sy = std::sin(fc.yaw);
            const float cp = std::cos(fc.pitch);
            const float sp = std::sin(fc.pitch);
            const glm::vec3 camera_pos = fc.position;
            const glm::vec3 camera_dir(-cp * sy, sp, -cp * cy);
            const glm::vec3 world_up(0, 1, 0);
            const glm::mat4 view_matrix = glm::lookAtRH(camera_pos,
                                                          camera_pos + camera_dir,
                                                          world_up);
            const glm::mat4 view_proj = proj_matrix * view_matrix;
            s.pending_globals[v] = cairns::rhi::RenderPassGlobals {
                .view_proj = view_proj,
                .inv_view_proj = glm::inverse(view_proj),
                .camera_pos = glm::vec4(camera_pos, 1.0f /*exposure */),
                .camera_dir = glm::vec4(camera_dir, near_z),
                .screen_params = glm::vec4(vp_w, vp_h, 1.0f / vp_w, 1.0f / vp_h)
            };
            s.pending_view_matrix[v] = view_matrix;
            s.pending_near_z[v] = near_z;
            s.pending_far_z[v] = far_z;
        }

        // Set the active world's root_transform, run TRS hierarchy
        // propagation (no-op when no entity carries a Transform; the
        // current scene-load emplaces WorldTransform directly), then
        // extract. Extract composes node.globalTransform * (world *
        // root_transform).
        {
            cairns::World::Hot* wh = worlds_.GetHot(active_world_);
            cairns::World::Cold* wc = worlds_.GetCold(active_world_);
            if (wh && wc) {
                wh->root_transform = rot_matrix;
                cairns::PropagateTransforms(*wc, glm::mat4(1.0f));
                cairns::ExtractFromWorld(*wc, wh->root_transform, assets_,
                                         s.proxies);
            }
        }

        // Counting pass -> total_draws.
        uint32_t total_draws = 0;
        for (const cairns::MeshProxy& mp : s.proxies.meshes.data) {
            total_draws += mp.primitive_count;
        }
        s.drawList.resize(total_draws);
        s.drawListSorted.resize(total_draws);
        s.draw_world_matrices.resize(total_draws);

        // Fill pass -- stable_idx assigned by prefix sum over the proxy walk
        // (deterministic of input order, independent of execution order so a
        // future parallel_for is a drop-in).
        uint32_t stable_idx = 0;
        for (const cairns::MeshProxy& mp : s.proxies.meshes.data) {
            const BufHandle pos = mp.pos;
            [[maybe_unused]] const BufHandle attr = mp.attr;
            const BufHandle index = mp.index;
            const glm::mat4& world_mat = mp.world_matrix;
            const uint32_t index_base_off = rhi_.resources.BufferBaseOffset(rhi_.alloc, index);
            for (uint32_t p = 0; p < mp.primitive_count; ++p) {
                const cairns::PrimitiveProxy& prim = s.proxies.primitives[mp.first_primitive + p];
                const MatId mat_id = prim.material_id;

                cairns::Draw draw{};
                draw.bind_groups[1] = material_bind_groups_[mat_id];
                draw.index_buffer = index;
                draw.index_offset = index_base_off + (prim.first_index * sizeof(uint32_t));
                draw.vertex_offset = prim.vertex_offset;
                draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot] = pos;
                draw.vertex_buffers[cairns::Draw::kVertexBufferAttrSlot] = attr;
                draw.instance_offset = 0;
                draw.instance_count = 1;
                draw.dynamic_buffer_offsets[0] = UINT32_MAX;  // material   - filled by EncodeDraws
                draw.dynamic_buffer_offsets[1] = UINT32_MAX;  // draw_tmp   - filled by EncodeDraws
                assert(prim.index_count % 3 == 0);
                draw.triangle_count =
                    tiny_quad_test_ ? 2 : prim.index_count / 3;

                // P2: depth_q dropped from the sort key (pass 0). Including
                // it would make the sort camera-dependent and force a per-
                // viewport re-sort. Material + pipeline ordering still
                // preserves batching across both viewports.
                s.drawListSorted[stable_idx] = std::make_pair(
                    cairns::BuildDrawKey(mat_id & 0x3FFFFFFFu, /*depth=*/0,
                                         kMockTranslucency, kMockViewport,
                                         kMockViewportLayer, kMockFullscreenLayer),
                    stable_idx);
                s.drawList[stable_idx] = draw;
                s.draw_world_matrices[stable_idx] = world_mat;
                ++stable_idx;
            }
        }
        assert(stable_idx == total_draws);

        return true;
    }

    // Render-side bump of all per-frame UBOs. Order matters: globals first,
    // per-draw (material, draw_tmp) in stable_idx order, fixed_dt last.
    // Writes s.globals_offset, s.drawList[*].dynamic_buffer_offsets[0..1],
    // s.dt_off. Compute kernel sees pkt.fixed_dt (constant sim dt), not wall.
    void EncodeDraws(const FramePacket& pkt) {
        PerSlot& s = slots_[pkt.slot];
        // 1. globals UBO -- one per viewport, distinct bump offsets. The
        // forward pass for viewport v binds s.globals_offset[v].
        for (int v = 0; v < kNumViewports; ++v) {
            void* gptr = rhi_.alloc.BumpAllocate(
                sizeof(cairns::rhi::RenderPassGlobals), rhi_.alloc.UboAlign(),
                rhi::Memory::kDynamic, &s.globals_offset[v]);
            assert(gptr && "bump alloc failed: render pass globals");
            memcpy(gptr, &s.pending_globals[v], sizeof(cairns::rhi::RenderPassGlobals));
        }
        if (frame_ <= 6) {
            const glm::mat4& vp = s.pending_globals[active_viewport_].view_proj;
            const float vp_w = static_cast<float>(FrameWidth()) /
                                static_cast<float>(kNumViewports);
            const float aspect_ratio = vp_w / static_cast<float>(FrameHeight());
            size_t entity_count = 0;
            if (auto* wc = worlds_.GetCold(active_world_)) {
                entity_count = wc->registry.storage<entt::entity>().size();
            }
            fprintf(stderr,
                    "[FLAKE] frame=%u w=%u h=%u aspect=%.9f vp00=%.9f vp11=%.9f "
                    "vp22=%.9f vp32=%.9f goff0=%u goff1=%u par_in=%u par_out=%u "
                    "entities=%zu meshes=%zu prims=%zu\n",
                    frame_, FrameWidth(), FrameHeight(), aspect_ratio,
                    vp[0][0], vp[1][1], vp[2][2], vp[3][2],
                    s.globals_offset[0], s.globals_offset[1],
                    pkt.particle_parity_in, pkt.particle_parity_out,
                    entity_count, s.proxies.meshes.size(),
                    s.proxies.primitives.size());
        }

        // 2. Per-draw material + draw_tmp UBOs in stable_idx order.
        const cairns::rhi::MaterialGpu material_gpu {};
        for (size_t i = 0; i < s.drawList.size(); ++i) {
            uint32_t material_offset = 0;
            void* mptr = rhi_.alloc.BumpAllocate(
                sizeof(cairns::rhi::MaterialGpu), rhi_.alloc.UboAlign(),
                rhi::Memory::kDynamic, &material_offset);
            assert(mptr && "bump alloc failed: material");
            memcpy(mptr, &material_gpu, sizeof(material_gpu));

            const cairns::rhi::DrawTmp draw_tmp { .model_matrix = s.draw_world_matrices[i] };
            uint32_t drawtmp_offset = 0;
            void* tptr = rhi_.alloc.BumpAllocate(
                sizeof(cairns::rhi::DrawTmp), rhi_.alloc.UboAlign(),
                rhi::Memory::kDynamic, &drawtmp_offset);
            assert(tptr && "bump alloc failed: draw tmp");
            memcpy(tptr, &draw_tmp, sizeof(draw_tmp));

            s.drawList[i].dynamic_buffer_offsets[0] = material_offset;
            s.drawList[i].dynamic_buffer_offsets[1] = drawtmp_offset;
        }

        // 3. delta_time UBO.
        float* dt_ptr = static_cast<float*>(
            rhi_.alloc.BumpAllocate(sizeof(float), rhi_.alloc.UboAlign(),
                                    rhi::Memory::kDynamic, &s.dt_off));
        assert(dt_ptr && "bump alloc failed: delta time");
        // Compute kernel sees the fixed sim dt, NOT wall dt -- particles step
        // at a constant rate regardless of frame timing.
        *dt_ptr = pkt.fixed_dt;
    }

    // Drain in-flight work, settle GPU, invalidate any caches keyed on the
    // old swap dims, then resize per-viewport / final_target state. Called
    // at the top of draw(); a no-op when no SDL resize is pending and the
    // observed swap dims haven't drifted (Vk's WSI may auto-recreate the
    // swapchain on OUT_OF_DATE without ever calling this path).
    void ApplyPendingResize() {
        const uint32_t cur_w = final_target_.IsNull() ? swapchain_.Width()
                                                       : final_target_w_;
        const uint32_t cur_h = final_target_.IsNull() ? swapchain_.Height()
                                                       : final_target_h_;
        const bool dims_drifted = (cur_w != last_seen_swap_w_) ||
                                   (cur_h != last_seen_swap_h_);
        if (!resize_pending_ && !dims_drifted) {
            return;
        }
        if (render_thread_) {
            render_thread_->Drain();
        }
#if CAIRNS_VULKAN
        if (rhi_.device.device_) {
            vkDeviceWaitIdle(rhi_.device.device_);
        }
        // Framebuffers in the offscreen cache are sized at create-time
        // against the prior swap dims; the (w, h) check inside
        // get_offscreen_fb wouldn't match the new dims so they'd grow
        // unboundedly. Wipe them on resize; render passes (keyed on format,
        // not dims) survive.
        rhi_.frames.offscreen_target_cache_.FlushFramebuffers();
#endif
        if (!final_target_.IsNull() &&
            (resize_pending_w_ != final_target_w_ ||
             resize_pending_h_ != final_target_h_) &&
            resize_pending_w_ != 0 && resize_pending_h_ != 0) {
            ResizeFinalTarget(resize_pending_w_, resize_pending_h_);
        }
        last_seen_swap_w_ = final_target_.IsNull() ? swapchain_.Width()
                                                    : final_target_w_;
        last_seen_swap_h_ = final_target_.IsNull() ? swapchain_.Height()
                                                    : final_target_h_;
        resize_pending_ = false;
    }

    bool draw() {
        // Settle any pending SDL resize BEFORE the next render thread acquire
        // -- ApplyPendingResize drains the render thread and waits the device
        // idle so destroyed targets aren't dereferenced by an in-flight frame.
        // Minimized window: swap dims are 0 in windowed mode; bail with a
        // drain so we don't try to render to a zero-extent target.
        ApplyPendingResize();
        if (final_target_.IsNull() &&
            (swapchain_.Width() == 0 || swapchain_.Height() == 0)) {
            if (render_thread_) {
                render_thread_->Drain();
            }
            return false;
        }

        frame_++;
        const uint32_t slot = (frame_ - 1) % kFramesInFlight;

        // Acquire BEFORE touching slot storage -- this is the backpressure
        // gate, blocks if the render thread is still holding slot S.
        render_thread_->Acquire(slot);

        PerSlot& s = slots_[slot];

        const uint64_t cpu_now_ns = cairns::timestamp_ns();
        if (cpu_last_frame_ns_ != 0) {
            cpu_ms_last_ = static_cast<float>(cpu_now_ns - cpu_last_frame_ns_) / 1.0e6f;
            cpu_ms_history_[cpu_ms_head_] = cpu_ms_last_;
            cpu_ms_head_ = (cpu_ms_head_ + 1) % kCpuMsHistory;
        }
        cpu_last_frame_ns_ = cpu_now_ns;
        s.pkt.request_dump = false;
        s.pkt.dump_path.clear();
        if (golden_ && !dump_emitted_ && sim_frame_ >= cairns::kGoldenDumpFrame) {
            const char* dump = std::getenv("CAIRNS_DUMP");
            s.pkt.request_dump = true;
            s.pkt.dump_path = dump ? dump : "/tmp/cairns_dump.png";
            dump_emitted_ = true;
            dump_emit_frame_ = frame_;
        }
        if (dump_emitted_ && frame_ >= dump_emit_frame_ + 2) {
            std::exit(0);  // headless byte-gate: dump frame flushed, now quit
        }

        cairns::Timer t_frame("frame", 0);

        // Fiedler fixed-timestep accumulator. clock_ is FixedClock under
        // CAIRNS_DUMP (1 step/frame, alpha=0) or WallClock live. wall_dt is
        // clamped to kMaxFrameDt to avoid spiral-of-death on big stalls.
        const double wall_dt = clock_->Tick();
        accumulator_ += std::min(wall_dt, cairns::kMaxFrameDt);
        sim_steps_this_frame_ = 0;
        while (accumulator_ >= cairns::kFixedDt &&
               sim_steps_this_frame_ < cairns::kMaxStepsPerFrame) {
            sim_angle_deg_ += cairns::kRotDegPerSec * static_cast<float>(cairns::kFixedDt);
            ++sim_frame_;
            accumulator_ -= cairns::kFixedDt;
            ++sim_steps_this_frame_;
        }
        const float alpha = static_cast<float>(accumulator_ / cairns::kFixedDt);
        render_angle_deg_ = sim_angle_deg_ +
                            alpha * cairns::kRotDegPerSec *
                                static_cast<float>(cairns::kFixedDt);

        if (frame_ <= 5) {
            fprintf(stderr,
                    "[FCLK] frame=%u wall_dt=%.4f acc=%.4f steps=%u alpha=%.3f "
                    "sim_frame=%llu sim_deg=%.3f\n",
                    frame_, wall_dt, accumulator_, sim_steps_this_frame_,
                    alpha, static_cast<unsigned long long>(sim_frame_),
                    sim_angle_deg_);
        }


        cairns::Timer t_build("build_draws", 1);
        if (!BuildMeshOpaqueDraws(slot)) {
            return false;
        }
        t_build.End();
        std::sort(s.drawListSorted.begin(), s.drawListSorted.end());

        s.resident_textures.clear();
        for (auto& scene : scenes_) {
            for (const auto th : scene.textureHandles) {
                s.resident_textures.push_back(th);
            }
        }

        // Fill packet header (the view into per-slot storage).
        s.pkt.frame_idx = frame_;
        s.pkt.slot = slot;
        s.pkt.view = s.pending_view_matrix[active_viewport_];
        s.pkt.proj = glm::mat4(1.0f);  // not used downstream; view_proj baked into pending_globals
        s.pkt.near_z = s.pending_near_z[active_viewport_];
        s.pkt.far_z = s.pending_far_z[active_viewport_];
        s.pkt.sim_steps_this_frame = sim_steps_this_frame_;
        s.pkt.fixed_dt = static_cast<float>(cairns::kFixedDt);
        // Wait for the previous frame's render-thread-published parity. In
        // steady state Acquire(slot) already established the happens-after,
        // so this rarely actually blocks.
        {
            std::unique_lock<std::mutex> lk(parity_m_);
            parity_cv_.wait(lk, [&] {
                return latest_parity_frame_ + 1 >= frame_;
            });
            s.pkt.particle_parity_in = latest_parity_out_;
        }
        s.pkt.draws = std::span<const cairns::Draw>(s.drawList.data(), s.drawList.size());
        s.pkt.sorted = std::span<const std::pair<cairns::DrawKey, uint32_t>>(
            s.drawListSorted.data(), s.drawListSorted.size());
        s.pkt.resident_textures = std::span<const rhi::Handle<rhi::Texture>>(
            s.resident_textures.data(), s.resident_textures.size());

        // Skip ImGui in golden-dump mode (windowed CAIRNS_DUMP, no overlay
        // in the byte-gate) AND in surfaceless mode (cairns_serve has no
        // SDL3 platform backend init'd; ImGui_ImplSDL3_NewFrame would
        // assert. final_target_ being non-null is the surfaceless marker).
        const bool draw_imgui = !golden_ && final_target_.IsNull();
        if (draw_imgui) {
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
            ImGui::Begin("cairns", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            const float fps = cpu_ms_last_ > 0.0f ? 1000.0f / cpu_ms_last_ : 0.0f;
            float ms_max = 1.0f;
            float ms_avg = 0.0f;
            for (int i = 0; i < kCpuMsHistory; ++i) {
                ms_max = cpu_ms_history_[i] > ms_max ? cpu_ms_history_[i] : ms_max;
                ms_avg += cpu_ms_history_[i];
            }
            ms_avg /= static_cast<float>(kCpuMsHistory);
            ImGui::Text("CPU %.2f ms   |   %.0f FPS", cpu_ms_last_, fps);
            ImGui::Text("avg %.2f ms   |   peak %.2f ms", ms_avg, ms_max);
            auto slot_avg_ms = [](int s) -> float {
                const uint64_t n = cairns::Timer::accum_itrs_[s];
                if (n == 0) {
                    return 0.0f;
                }
                return static_cast<float>(
                    cairns::Timer::accum_times_[s] / static_cast<double>(n) /
                    1000.0);
            };
            const uint32_t gpu_mask = cairns::TimerStorage::GpuSlotMask();
            float gpu_frame_ms = 0.0f;
            for (uint32_t s = 0; s < cairns::Timer::kMaxSlots; ++s) {
                if (gpu_mask & (1u << s)) {
                    gpu_frame_ms += slot_avg_ms(s);
                }
            }
            ImGui::Text("%-12s %5.2f ms", "gpu_frame", gpu_frame_ms);
            for (uint32_t s = 0; s < cairns::Timer::kMaxSlots; ++s) {
                if (cairns::Timer::accum_itrs_[s] == 0) {
                    continue;
                }
                const char* nm = cairns::Timer::slot_names_[s]
                                     ? cairns::Timer::slot_names_[s]
                                     : "?";
                if (std::strcmp(nm, "set up render pass globals") == 0 ||
                    std::strcmp(nm, "build opaque draw list") == 0 ||
                    std::strcmp(nm, "particle_sim") == 0 ||
                    std::strcmp(nm, "forward") == 0) {
                    continue;
                }
                ImGui::Text("%-12s %5.2f ms", nm, slot_avg_ms(s));
            }
            char overlay[32];
            std::snprintf(overlay, sizeof(overlay), "%.2f ms", cpu_ms_last_);
            ImGui::PlotLines("##cpuhist", cpu_ms_history_, kCpuMsHistory,
                             cpu_ms_head_, overlay, 0.0f, ms_max * 1.15f,
                             ImVec2(300.0f, 110.0f));
            ImGui::End();
            ImGui::PopStyleColor(4);
            ImGui::Render();
            cairns::SnapshotImDrawData(ImGui::GetDrawData(), s.imgui_snapshot);
            s.pkt.imgui_snapshot = &s.imgui_snapshot.data;
        } else {
            s.imgui_snapshot.Clear();
            s.pkt.imgui_snapshot = nullptr;
        }

        render_thread_->Submit(slot, &s.pkt);

        // Under CAIRNS_DUMP, collapse to depth-1 pipelining: wait for the
        // render thread to fully complete this frame before the next iteration
        // queues another. Keeps frame 5's dump output byte-identical regardless
        // of threading (Drain forces same parity sequence as single-threaded).
        //
        // Also drain in surfaceless mode (cairns_serve) so the next
        // io.dumpTexture op sees the rendered pixels rather than reading
        // final_target_ while the render thread is still working on it.
        if (std::getenv("CAIRNS_DUMP") || !final_target_.IsNull()) {
            render_thread_->Drain();
        }

        t_frame.End();
        if (frame_ % 120 == 0) {
            const size_t loaded = scenes_.size();
            size_t entities = 0;
            if (auto* wc = worlds_.GetCold(active_world_)) {
                entities = wc->registry.storage<entt::entity>().size();
            }
            const size_t slices = loaded > 0 ? entities / loaded : 0;
            CAIRNS_PRINT("============\n");
            CAIRNS_PRINT("draws %zu | %zu GLBs x %zu slices = %zu entities | resolution %u x %u\n",
                         s.drawList.size(), loaded, slices, entities,
                         FrameWidth(), FrameHeight());
            cairns::Timer::PrintReport();
            cairns::Timer::Reset();
        }
        return true;
    }

    // Pick the per-frame swap target. SwapChain and Frames are
    // app-mode-agnostic -- the engine is the one place that knows which
    // texture the swap pass writes into this frame.
    rhi::SwapResolveTarget AcquireFrameSwapTarget() {
        if (final_target_.IsNull()) {
            return swapchain_.AcquireForFrame();
        }
#if CAIRNS_METAL
        MTL::Texture* tex = rhi_.resources.GetHot(final_target_)->api_view;
        return rhi::MakeSwapResolveTargetFromTexture(tex, final_target_w_,
                                                       final_target_h_);
#else
        // vk render-to-texture not yet wired (#199); surfaceless mode bails
        // before render_thread_ is created so this path isn't reached.
        return rhi::SwapResolveTarget{};
#endif
    }

    // Render-thread entry point (post commit 6). Today called synchronously
    // from draw(). Owns: rhi_.frames.Begin/End, the bump-ring EncodeDraws,
    // the compute + render-pass encode. Reads pkt + slots_[pkt.slot].
    void RecordFrame(FramePacket& pkt) {
        [[maybe_unused]] cairns::TaskGuard task_guard;

        PerSlot& s = slots_[pkt.slot];

        // Publish parity early -- a pure function of pkt fields (no GPU
        // dependency) so the game thread's parity_cv wait clears immediately.
        // With N steps per frame, parity_out = parity_in ^ (N & 1).
        pkt.particle_parity_out =
            pkt.particle_parity_in ^ (pkt.sim_steps_this_frame & 1u);
        {
            std::lock_guard<std::mutex> lk(parity_m_);
            latest_parity_out_ = pkt.particle_parity_out;
            latest_parity_frame_ = pkt.frame_idx;
        }
        parity_cv_.notify_all();

        if (pkt.request_dump) {
            rhi_.frames.SetDumpPath(pkt.dump_path);
        }

        // Engine -- not the RHI -- picks the per-frame swap target. Windowed:
        // pull the next drawable from the SwapChain. Surfaceless: hand Frames
        // the engine-owned offscreen, with no drawable so it doesn't present.
        rhi::SwapResolveTarget swap_target = AcquireFrameSwapTarget();
        rhi::FrameContext fc = rhi_.frames.Begin(rhi_.resources, rhi_.alloc, swap_target);

        cairns::Timer t_record("record", 2);
        EncodeDraws(pkt);

        if (!graph_) {
            graph_ = std::make_unique<rhi::RenderGraph>(rhi_.resources, rhi_.alloc);
        }
        graph_->Reset();

        // Per-viewport MeshDrawList: same draws, distinct globals_offset.
        // (Same world for both viewports this commit; multi-world content
        // lands in #195.)
        std::array<rhi::MeshDrawList, kNumViewports> mls{};
        for (int v = 0; v < kNumViewports; ++v) {
            mls[v].draws = pkt.draws;
            mls[v].sorted_draws = pkt.sorted;
            mls[v].pipeline = unlit_offscreen_;
            mls[v].globals_offset = s.globals_offset[v];
            mls[v].resident_textures = pkt.resident_textures;
            mls[v].resident_buffers =
                std::span<const rhi::Handle<rhi::Buffer>>(&mesh_master_handle_, 1);
        }

        rhi::PointDraw pd{};
        pd.pipeline = particle_render_offscreen_;
        pd.vertex_buffer = particle_ssbo_[pkt.particle_parity_out];
        pd.vertex_offset = 0;
        pd.vertex_count = kParticleCount;

        const float clear[4] = {41.0f / 255.0f, 42.0f / 255.0f, 48.0f / 255.0f, 1.0f};
        const uint32_t fb_w = swap_target.width;
        const uint32_t fb_h = swap_target.height;
        const uint32_t vp_w = fb_w / kNumViewports;
        const uint32_t vp_h = fb_h;

        // pass 1: particle_sim kCompute. import the writer ssbo so prune keeps
        // it (external side effect -- game thread reads particle_parity_out).
        rhi::GraphBuffer sim_out;
        graph_->AddPass(
            "particle_sim", rhi::PassType::kCompute,
            [&](rhi::PassBuilder& b) {
                rhi::GraphBufferDesc bd{};
                bd.usage = rhi::kUsageStorage;
                sim_out = b.ImportBuffer(
                    particle_ssbo_[pkt.particle_parity_out], bd);
                b.WriteBuffer(sim_out);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                rhi::ComputeDispatch cd{};
                cd.kernel = particle_kernel_;
                cd.groups_x = kParticleCount / 256;
                cd.local_x = 256;
                // Metal: sequential ComputeCommandEncoders self-hazard on R/W
                // ordering (MTLHazardTrackingModeTracked). Vulkan: recorder
                // emits a compute->compute pipeline barrier between dispatches.
                for (uint32_t k = 0; k < pkt.sim_steps_this_frame; ++k) {
                    const uint32_t step_src =
                        pkt.particle_parity_in ^ (k & 1u);
                    const uint32_t step_dst = step_src ^ 1u;
                    rhi::BoundBuffer cbufs[3] = {
                        {0, rhi_.alloc.BumpMasterBuffer(rhi::Memory::kDynamic),
                         s.dt_off},
                        {1, particle_ssbo_[step_src], 0},
                        {2, particle_ssbo_[step_dst], 0},
                    };
                    cd.buffers = std::span<const rhi::BoundBuffer>(cbufs, 3);
                    cd.step_index = k;
                    cmd.Dispatch(rhi_.resources, rhi_.alloc, cd);
                }
            });

        // pass 2: forward, ONCE PER VIEWPORT. Each pass writes to a private
        // half-width color+depth target. Particles render into both viewports
        // (compute step ran once above; particle render is a graphics
        // submission that draws into each forward pass's encoder).
        std::array<rhi::GraphTexture, kNumViewports> color_off{};
        std::array<rhi::GraphTexture, kNumViewports> depth_off{};
        for (int v = 0; v < kNumViewports; ++v) {
            const int vp_idx = v;
            const char* pass_name = (vp_idx == 0) ? "forward_vp0" : "forward_vp1";
            graph_->AddPass(
                pass_name, rhi::PassType::kGraphics,
                [&, vp_idx](rhi::PassBuilder& b) {
                    rhi::GraphTextureDesc cd{};
                    cd.width = vp_w;
                    cd.height = vp_h;
                    cd.format = rhi::Format::kBgra8Unorm;
                    cd.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled;
                    color_off[vp_idx] = b.CreateColorTarget(cd);
                    rhi::GraphTextureDesc dd{};
                    dd.width = vp_w;
                    dd.height = vp_h;
                    dd.format = rhi::Format::kD32F;
                    dd.usage = rhi::kTexUsageDepthTarget | rhi::kTexUsageSampled;
                    depth_off[vp_idx] = b.CreateDepthTarget(dd);
                    b.AddColorOutput("color", color_off[vp_idx], rhi::LoadOp::kClear, clear);
                    b.AddDepthOutput("fwd_depth", depth_off[vp_idx], rhi::LoadOp::kClear, 1.0f);
                },
                [&, vp_idx](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                    cmd.DrawMeshes(rhi_.resources, rhi_.alloc, mls[vp_idx]);
                    cmd.DrawPoints(rhi_.resources, rhi_.alloc, pd);
                });
        }

        // pass 3: composite + ui kGraphics. Composite samples color_off full-
        // screen, depth_off PIP in the bottom-right; ImGui draws on top into the
        // same encoder. Both backends use MSAA swapchain renderpasses where the
        // MSAA color attachment storeOp is "resolve + don't-keep-MSAA" (Adreno
        // tile-residency optimization). Two back-to-back render passes targeting
        // the same swap framebuffer would either clear or load undefined MSAA
        // between passes, so we keep them under one encoder. The graph still
        // expresses the dependencies (this pass reads color_off + depth_off
        // produced by forward) -- "ui" is conceptually a separate phase that
        // physically shares the swap encoder for MSAA reasons.
        rhi::GraphTexture swap_tex;
        graph_->AddPass(
            "swap", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                rhi::GraphTextureDesc td{};
                td.width = fb_w;
                td.height = fb_h;
                swap_tex = b.ImportTexture(rhi::Handle<rhi::Texture>::Null, td);
                b.AddColorOutput("swapchain", swap_tex, rhi::LoadOp::kClear, clear);
                for (int v = 0; v < kNumViewports; ++v) {
                    b.AddAttachmentInput(color_off[v]);
                    b.AddAttachmentInput(depth_off[v]);
                }
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources& res) {
                std::array<rhi::Handle<rhi::Texture>, kNumViewports> vp_color{};
                std::array<rhi::Handle<rhi::Texture>, kNumViewports> vp_depth{};
                for (int v = 0; v < kNumViewports; ++v) {
                    vp_color[v] = res.Resolve(color_off[v]);
                    vp_depth[v] = res.Resolve(depth_off[v]);
                }
                // Composite each viewport's color into its half of the swap.
                const float vp_fw = static_cast<float>(vp_w);
                const float vp_fh = static_cast<float>(vp_h);
                for (int v = 0; v < kNumViewports; ++v) {
                    const float x = vp_fw * static_cast<float>(v);
                    cmd.SetViewport(x, 0.0f, vp_fw, vp_fh);
                    cmd.SetScissor(static_cast<int32_t>(x), 0,
                                   vp_w, vp_h);
                    cmd.DrawFullscreen(rhi_.resources, composite_pip_,
                                       std::span<const rhi::Handle<rhi::Texture>>(&vp_color[v], 1),
                                       composite_sampler_);
                }
                // Active viewport's depth PIP (bottom-right 25% of the active
                // viewport's half).
                {
                    const float vp_x0 = vp_fw * static_cast<float>(active_viewport_);
                    const float pip_x = vp_x0 + 0.75f * vp_fw;
                    const float pip_y = 0.75f * vp_fh;
                    const float pip_w = 0.25f * vp_fw;
                    const float pip_h = 0.25f * vp_fh;
                    cmd.SetViewport(pip_x, pip_y, pip_w, pip_h);
                    cmd.SetScissor(static_cast<int32_t>(pip_x),
                                   static_cast<int32_t>(pip_y),
                                   static_cast<uint32_t>(pip_w),
                                   static_cast<uint32_t>(pip_h));
                    cmd.DrawFullscreen(rhi_.resources, depthviz_,
                                       std::span<const rhi::Handle<rhi::Texture>>(&vp_depth[active_viewport_], 1),
                                       composite_sampler_);
                }
                // Restore full extent before the ui draw.
                cmd.SetViewport(0.0f, 0.0f, static_cast<float>(fb_w),
                                static_cast<float>(fb_h));
                cmd.SetScissor(0, 0, fb_w, fb_h);
                if (pkt.imgui_snapshot) {
                    cmd.DrawImGui(rhi_.resources, rhi_.alloc, imgui_, imgui_font_,
                                  imgui_sampler_, pkt.imgui_snapshot);
                }
            });

        graph_->SetOutput(swap_tex);
        if (!graph_->Bake() || !graph_->Execute(fc, swap_target)) {
            t_record.End();
            rhi_.frames.End(swap_target, fc);
            return;
        }
        if (frame_ <= 6) {
            const rhi::Handle<rhi::Texture> coff0 = graph_->ResolveTexture(color_off[0]);
            const rhi::Handle<rhi::Texture> doff0 = graph_->ResolveTexture(depth_off[0]);
            const rhi::Handle<rhi::Texture> coff1 = graph_->ResolveTexture(color_off[1]);
            const rhi::Handle<rhi::Texture> doff1 = graph_->ResolveTexture(depth_off[1]);
            fprintf(stderr,
                    "[FLAKE-R] frame=%u slot=%u img=%u "
                    "vp0_color=%u/%u vp0_depth=%u/%u vp1_color=%u/%u vp1_depth=%u/%u "
                    "steps=%u\n",
                    frame_, pkt.slot, fc.swapchain_image_index,
                    coff0.index, coff0.generation, doff0.index, doff0.generation,
                    coff1.index, coff1.generation, doff1.index, doff1.generation,
                    pkt.sim_steps_this_frame);
        }
        t_record.End();
        rhi_.frames.End(swap_target, fc);
    }
    
    bool initRenderPipeline() {
        {
            // set-2 per-material bind groups (portable path). Dense, indexed by
            // MatId. Texture+sampler arrive via this group, not the global table.
            material_bind_groups_.assign(materials_.size(),
                                         rhi::Handle<rhi::BindGroup>::Null);
            for (size_t m = 0; m < materials_.size(); ++m) {
                const rhi::TextureBinding tb{0, materials_[m].color};
                const rhi::SamplerBinding sb{0, materials_[m].sampler};
                rhi::BindGroupDesc bgd{};
                bgd.textures = std::span<const rhi::TextureBinding>(&tb, 1);
                bgd.samplers = std::span<const rhi::SamplerBinding>(&sb, 1);
                material_bind_groups_[m] = rhi_.resources.CreateBindGroup(bgd);
            }
        }

        {  // unlit graphics pipeline via rhi
            const char* base = SDL_GetBasePath();
            const std::string shader_dir = base ? base : "";
            const rhi::VertexInputAttribute vtx_attrs[2] = {
                {0, cairns::kMeshPosBindSlot, rhi::Format::kRgba32F, 0},
                // stream 1: uv at offset 48 in the 64-byte VertexAttribute.
                {1, cairns::kMeshAttrVertexBindSlot, rhi::Format::kRg32F, 48},
            };
            const rhi::VertexBufferLayout vtx_layouts[2] = {
                {cairns::kMeshPosBindSlot, static_cast<uint32_t>(sizeof(glm::vec4))},
                {cairns::kMeshAttrVertexBindSlot, 64},
            };
            rhi::GraphicsPipelineDesc desc{};
            desc.logical_shader = "unlit";
            desc.shader_dir = shader_dir.c_str();
            desc.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(vtx_attrs, 2);
            desc.vertex_buffers =
                std::span<const rhi::VertexBufferLayout>(vtx_layouts, 2);
            desc.topology = rhi::PrimitiveTopology::kTriangleList;
            desc.cull = rhi::CullMode::kBack;
            desc.front_face = rhi::FrontFace::kCounterClockwise;
            desc.depth_test = true;
            desc.depth_write = true;
            desc.depth_compare = rhi::CompareOp::kLess;
            desc.color_format = rhi::Format::kBgra8Unorm;
            desc.depth_format = rhi::Format::kD32F;
            desc.sample_count = sampleCount;
            desc.push_constant_bytes = 0;  // base_vertex no longer needed (attrs are a vertex stream)
            desc.debug_name = "unlit";
            desc.swap_chain = &swapchain_;
            unlit_ = rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.frames, desc);
            if (unlit_.IsNull()) {
                std::exit(0);
            }

            // Offscreen variant: single-sample, no swapchain compat. Same shaders
            // + vertex layout as unlit; targets a render-graph color_off+depth_off.
            rhi::GraphicsPipelineDesc ofd = desc;
            ofd.logical_shader = "unlit_offscreen";
            ofd.sample_count = 1;
            ofd.swap_chain = nullptr;
            ofd.debug_name = "unlit_offscreen";
            unlit_offscreen_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, ofd);
            if (unlit_offscreen_.IsNull()) {
                std::exit(0);
            }

            // composite_pip: full-screen tri, samples 1 color tex, writes MSAA swap.
            rhi::GraphicsPipelineDesc cpd{};
            cpd.logical_shader = "composite_pip";
            cpd.shader_dir = shader_dir.c_str();
            cpd.topology = rhi::PrimitiveTopology::kTriangleList;
            cpd.cull = rhi::CullMode::kNone;
            cpd.depth_test = false;
            cpd.depth_write = false;
            cpd.color_format = rhi::Format::kBgra8Unorm;
            cpd.depth_format = rhi::Format::kD32F;
            cpd.sample_count = sampleCount;
            cpd.debug_name = "composite_pip";
            cpd.swap_chain = &swapchain_;
            composite_pip_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, cpd);

            // depthviz: same shape, samples 1 depth tex, blue silhouette.
            rhi::GraphicsPipelineDesc dvd = cpd;
            dvd.logical_shader = "depthviz";
            dvd.debug_name = "depthviz";
            depthviz_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, dvd);

            if (composite_pip_.IsNull() || depthviz_.IsNull()) {
                std::exit(0);
            }

            rhi::SamplerDesc sd{};
            sd.min_filter = rhi::Filter::kLinear;
            sd.mag_filter = rhi::Filter::kLinear;
            sd.mip_filter = rhi::Filter::kLinear;
            sd.address_mode = rhi::AddressMode::kClampToEdge;
            composite_sampler_ = rhi_.resources.CreateSampler(sd);
        }

        return true;
    }

    struct Particle {
        float position[2];
        float velocity[2];
        float color[4];
    };

    bool initParticles() {
        const char* base = SDL_GetBasePath();
        const std::string shader_dir = base ? base : "";
        {  // particle compute kernel via rhi
            rhi::ComputePipelineDesc desc{};
            desc.logical_shader = "particle";
            desc.shader_dir = shader_dir.c_str();
            desc.debug_name = "particle_compute";
            particle_kernel_ = rhi_.pipelines.CreateComputePipeline(rhi_.resources, rhi_.frames, desc);
            if (particle_kernel_.IsNull()) {
                return false;
            }
        }
        {  // particle render pipeline via rhi
            const rhi::VertexInputAttribute attrs[2] = {
                {0, 0, rhi::Format::kRg32F,
                 static_cast<uint32_t>(offsetof(Particle, position))},
                {1, 0, rhi::Format::kRgba32F,
                 static_cast<uint32_t>(offsetof(Particle, color))},
            };
            const rhi::VertexBufferLayout layout{
                0, static_cast<uint32_t>(sizeof(Particle))};
            rhi::GraphicsPipelineDesc desc{};
            desc.logical_shader = "particle";
            desc.shader_dir = shader_dir.c_str();
            desc.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(attrs, 2);
            desc.vertex_buffers =
                std::span<const rhi::VertexBufferLayout>(&layout, 1);
            desc.topology = rhi::PrimitiveTopology::kPointList;
            desc.cull = rhi::CullMode::kBack;
            desc.front_face = rhi::FrontFace::kCounterClockwise;
            desc.depth_test = true;
            desc.depth_write = true;
            desc.depth_compare = rhi::CompareOp::kLess;
            desc.blend.enable = true;
            desc.blend.src_color = rhi::BlendFactor::kSrcAlpha;
            desc.blend.dst_color = rhi::BlendFactor::kOneMinusSrcAlpha;
            desc.blend.src_alpha = rhi::BlendFactor::kOne;
            desc.blend.dst_alpha = rhi::BlendFactor::kZero;
            desc.color_format = rhi::Format::kBgra8Unorm;
            desc.depth_format = rhi::Format::kD32F;
            desc.sample_count = sampleCount;
            desc.push_constant_bytes = 0;
            desc.debug_name = "particle_render";
            desc.swap_chain = &swapchain_;
            particle_render_shader_ = rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.frames, desc);
            if (particle_render_shader_.IsNull()) {
                return false;
            }
            // Offscreen variant for the render-graph forward pass.
            rhi::GraphicsPipelineDesc opd = desc;
            opd.sample_count = 1;
            opd.swap_chain = nullptr;
            opd.debug_name = "particle_render_offscreen";
            particle_render_offscreen_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, opd);
            if (particle_render_offscreen_.IsNull()) {
                return false;
            }
        }

        {
            // imgui pipeline (swapchain MSAA, alpha blend, no depth).
            const char* base = SDL_GetBasePath();
            const std::string shader_dir = base ? base : "";
            const rhi::VertexInputAttribute ia[3] = {
                {0, 0, rhi::Format::kRg32F, offsetof(ImDrawVert, pos)},
                {1, 0, rhi::Format::kRg32F, offsetof(ImDrawVert, uv)},
                {2, 0, rhi::Format::kRgba8Unorm, offsetof(ImDrawVert, col)},
            };
            const rhi::VertexBufferLayout il{
                0, static_cast<uint32_t>(sizeof(ImDrawVert))};
            rhi::GraphicsPipelineDesc id{};
            id.logical_shader = "imgui";
            id.shader_dir = shader_dir.c_str();
            id.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(ia, 3);
            id.vertex_buffers = std::span<const rhi::VertexBufferLayout>(&il, 1);
            id.topology = rhi::PrimitiveTopology::kTriangleList;
            id.cull = rhi::CullMode::kNone;
            id.depth_test = false;
            id.depth_write = false;
            id.blend.enable = true;
            id.blend.src_color = rhi::BlendFactor::kSrcAlpha;
            id.blend.dst_color = rhi::BlendFactor::kOneMinusSrcAlpha;
            id.blend.src_alpha = rhi::BlendFactor::kOne;
            id.blend.dst_alpha = rhi::BlendFactor::kOneMinusSrcAlpha;
            id.color_format = rhi::Format::kBgra8Unorm;
            id.depth_format = rhi::Format::kD32F;
            id.sample_count = sampleCount;
            id.push_constant_bytes = 16;
            id.debug_name = "imgui";
            id.swap_chain = &swapchain_;
            imgui_ = rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources,
                                                          rhi_.frames, id);
            if (imgui_.IsNull()) {
                return false;
            }

            // imgui font atlas -> Texture. Scale font + style by surface
            // width so HiDPI / mobile displays don't render a postage-stamp
            // overlay. 1280px is the desktop reference width.
            ImGuiIO& io = ImGui::GetIO();
            const float kRefWidth = 900.0f;
            const float raw_scale =
                static_cast<float>(FrameWidth()) / kRefWidth;
#if CAIRNS_ANDROID
            const float kScaleMax = 2.5f;
#elif CAIRNS_APPLE && TARGET_OS_IPHONE
            const float kScaleMax = 1.0f;
#else
            const float kScaleMax = 1.5f;
#endif
            const float dpi_scale = std::clamp(raw_scale, 1.0f, kScaleMax);
            ImFontConfig fc;
            fc.SizePixels = 13.0f * dpi_scale;
            io.Fonts->Clear();
            io.Fonts->AddFontDefault(&fc);
            ImGui::GetStyle().ScaleAllSizes(dpi_scale);
            unsigned char* pixels = nullptr;
            int fw = 0;
            int fh = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
            rhi::TextureDesc ftd{};
            ftd.dimensions = {fw, fh, 1};
            ftd.format = rhi::Format::kRgba8Unorm;
            ftd.mip_levels = 1;
            ftd.array_layers = 1;
            ftd.usage = rhi::kTexUsageSampled | rhi::kTexUsageTransferDst;
            ftd.memory = rhi::Memory::kDefault;
            ftd.initial_data = std::span<const uint8_t>(
                pixels, static_cast<size_t>(fw) * fh * 4);
            imgui_font_ = rhi_.resources.CreateTexture(rhi_.alloc, ftd);
            rhi::SamplerDesc sd{};
            sd.min_filter = rhi::Filter::kLinear;
            sd.mag_filter = rhi::Filter::kLinear;
            sd.mip_filter = rhi::Filter::kLinear;
            sd.address_mode = rhi::AddressMode::kClampToEdge;
            imgui_sampler_ = rhi_.resources.CreateSampler(sd);
        }

        // Deterministic particle seed -- matches the golden capture path.
        // Override via Engine::SetRandomSeed before GreaterInit if you need
        // a different starting state (e.g. the rng.seed NDJSON op).
        std::srand(random_seed_);
        std::vector<Particle> particles(kParticleCount);
        for (uint32_t i = 0; i < kParticleCount; ++i) {
            const float r = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
            const float theta = r * 2.0f * static_cast<float>(std::numbers::pi);
            const float radius = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
            particles[i].position[0] = radius * std::cos(theta);
            particles[i].position[1] = radius * std::sin(theta);
            const float vx = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 0.5f;
            const float vy = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 0.5f;
            particles[i].velocity[0] = vx;
            particles[i].velocity[1] = vy;
            const float t = static_cast<float>(i) / static_cast<float>(kParticleCount);
            particles[i].color[0] = t;
            particles[i].color[1] = 1.0f - t;
            particles[i].color[2] = 0.5f;
            particles[i].color[3] = 1.0f;
        }

        const std::span<const uint8_t> init_data(
            reinterpret_cast<const uint8_t*>(particles.data()),
            particles.size() * sizeof(Particle));
        rhi::BufferDesc bd;
        bd.byte_size = static_cast<uint32_t>(particles.size() * sizeof(Particle));
        bd.usage = rhi::kUsageStorage | rhi::kUsageVertex;
        bd.memory = rhi::Memory::kDefault;
        bd.initial_data = init_data;
        particle_ssbo_[0] = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
        bd.initial_data = init_data;
        particle_ssbo_[1] = rhi_.resources.CreateBuffer(rhi_.alloc, bd);

        return !particle_ssbo_[0].IsNull() && !particle_ssbo_[1].IsNull();
    }

    bool deinit() {
        if (render_thread_) {
            render_thread_->Shutdown();
            render_thread_.reset();
        }
        swapchain_.Deinit();
        rhi_.pipelines.Deinit(rhi_.resources);
        rhi_.frames.Deinit();
        rhi_.resources.Deinit();
        rhi_.alloc.Deinit();
        rhi_.device.Deinit();
        return true;
    }
    
private:
    // todo @iamies
    // make an engine dtor and delete this
    void* hot_arena_mem_;
    cairns::Arena hot_arena_;
    
    ////////// DO NOT MOVE ARENA BELOW THIS LINE. because c++.
    
    uint32_t frame_ = 0;

    std::vector<cairns::Scene, cairns::Allocator<cairns::Scene>> scenes_;
    std::vector<int32_t, cairns::Allocator<int32_t>> root_nodes_stack_cache_;

    std::vector<glm::mat4> debugSceneXforms_;

    std::vector<cairns::LoadedMaterial> materials_;
    // set-2 per-material bind groups, dense by MatId (NO hash). Built once at load.
    std::vector<rhi::Handle<rhi::BindGroup>> material_bind_groups_;

    // Per-slot frame buffers (drawList / drawListSorted / proxies /
    // resident_textures / draw_world_matrices / pending_globals / globals_offset
    // / dt_off / FramePacket). See PerSlot above.
    std::vector<PerSlot> slots_;

    // EnTT scene-layer path. worlds_ pre-reserved at startup
    // (kMaxWorlds Acquire+Release cycle) to keep World::Cold* pointer
    // stable across real Acquire later.
    static constexpr uint32_t kMaxWorlds = 8;
    cairns::AssetRegistry assets_;
    cairns::ResourceManager<cairns::World> worlds_;
    std::vector<cairns::RenderProxyArrays> world_proxies_;
    cairns::WorldId active_world_;
    cairns::WorldId secondary_world_;  // P6 multi-world coexistence test

    // P2: two viewports side-by-side. fly_ is parallel (yaw/pitch/position)
    // to avoid reshaping Viewport every time the camera implementation grows.
    // cam_pose_override_ pins both controllers to a fixed (pos, yaw, pitch)
    // from CAIRNS_CAM_POSE so byte-gate dumps are deterministic regardless of
    // any keyboard/mouse input on this run.
    static constexpr int kNumViewports = 2;
    std::array<cairns::Viewport, kNumViewports> viewports_{};
    std::array<cairns::FlyController, kNumViewports> fly_{};
    int active_viewport_ = 0;
    bool cam_pose_override_ = false;

    rhi::Rhi rhi_;
    rhi::Handle<rhi::Buffer> mesh_master_handle_ = rhi::Handle<rhi::Buffer>::Null;
    // Per-frame render graph. Reused via Reset() across frames (vector storage
    // for passes/textures is preserved). Constructed lazily on first RecordFrame
    // because Resources& / Allocator& must already be initialized.
    std::unique_ptr<rhi::RenderGraph> graph_;

    cairns::rhi::SwapChain swapchain_;
    // shaders
    ShaderHandle unlit_ = ShaderHandle::Null;
    ShaderHandle unlit_offscreen_ = ShaderHandle::Null;
    ShaderHandle composite_pip_ = ShaderHandle::Null;
    ShaderHandle depthviz_ = ShaderHandle::Null;
    rhi::Handle<rhi::Sampler> composite_sampler_ = rhi::Handle<rhi::Sampler>::Null;
    // imgui
    rhi::Handle<rhi::Shader> imgui_ = rhi::Handle<rhi::Shader>::Null;
    rhi::Handle<rhi::Texture> imgui_font_ = rhi::Handle<rhi::Texture>::Null;
    rhi::Handle<rhi::Sampler> imgui_sampler_ = rhi::Handle<rhi::Sampler>::Null;
    // particles
    static constexpr uint32_t kParticleCount = 512;
    rhi::Handle<rhi::Kernel> particle_kernel_;
    rhi::Handle<rhi::Shader> particle_render_shader_;
    rhi::Handle<rhi::Shader> particle_render_offscreen_;
    rhi::Handle<rhi::Buffer> particle_ssbo_[2];
    std::unique_ptr<cairns::RenderThread> render_thread_;
    // Render thread writes back particle_parity_out under parity_m_ so the
    // game thread can chain frame N+1's parity_in from frame N's parity_out
    // without sharing a mutable counter. In steady state Acquire(slot N+1)
    // already happens-after Release of slot N, so the cv wait is a no-op.
    std::mutex parity_m_;
    std::condition_variable parity_cv_;
    uint32_t latest_parity_out_ = 0;
    uint64_t latest_parity_frame_ = 0;
    bool dump_emitted_ = false;
    uint32_t dump_emit_frame_ = 0;

    // Headless / surfaceless mode (cairns_serve): swap pass writes into this
    // offscreen target instead of the swapchain drawable. Allocated when
    // cfg.surfaceless == true in GreaterInit; null otherwise. P1C uses a
    // minimal clear-only render path; P2+ wires the full scene path through it.
    rhi::Handle<rhi::Texture> final_target_ = rhi::Handle<rhi::Texture>::Null;
    uint32_t final_target_w_ = 0;
    uint32_t final_target_h_ = 0;

    // P4 selection / highlight / pick. Selection + highlight are
    // document-side state; rev counters let the protocol's
    // cairns.selection.changed event know when to emit. Pick state holds
    // the most recent unresolved (viewport, x, y) click intent until the
    // GPU ID buffer + readback path lands.
    std::vector<cairns::SelectionTarget> selection_;
    std::vector<cairns::SelectionTarget> highlights_;
    uint32_t selection_rev_ = 0;
    uint32_t highlights_rev_ = 0;
    bool pick_pending_ = false;
    int pick_viewport_ = 0;
    uint32_t pick_x_ = 0;
    uint32_t pick_y_ = 0;

    // P3 resize lifecycle. SDL fires WINDOW_PIXEL_SIZE_CHANGED on the event
    // thread; we record intent + dims and settle on the next draw() call so
    // GPU teardown happens with no in-flight frames. last_seen_swap_w_/h_
    // also catches drift from Vulkan WSI auto-recreating the swapchain on
    // OUT_OF_DATE without our resize intent path firing.
    bool resize_pending_ = false;
    uint32_t resize_pending_w_ = 0;
    uint32_t resize_pending_h_ = 0;
    uint32_t last_seen_swap_w_ = 0;
    uint32_t last_seen_swap_h_ = 0;

    // Seed used by initParticles. Default 42 preserves the existing golden;
    // the rng.seed NDJSON op writes through SetRandomSeed before init.
    uint32_t random_seed_ = 42;
    // Fiedler fixed-timestep accumulator state. Game-thread only -- never
    // touched by the render thread. clock_ is WallClock in live mode,
    // FixedClock under CAIRNS_DUMP.
    std::unique_ptr<cairns::FrameClock> clock_;
    double accumulator_ = 0.0;
    uint64_t sim_frame_ = 0;
    float sim_angle_deg_ = 0.0f;
    float render_angle_deg_ = 0.0f;
    uint32_t sim_steps_this_frame_ = 0;
    bool golden_ = false;
    // Diagnostic: pin every draw to 2 triangles. Draw count + submission
    // identical, geometry throughput ~700x smaller. Isolates draw-submission
    // overhead vs geometry-throughput in the forward pass cost.
    bool tiny_quad_test_ = false;
    // cpu frame-time history (wall-clock between draw() calls) for the imgui graph
    static constexpr int kCpuMsHistory = 128;
    float cpu_ms_history_[kCpuMsHistory] = {};
    int cpu_ms_head_ = 0;
    float cpu_ms_last_ = 0.0f;
    uint64_t cpu_last_frame_ns_ = 0;
    // render pass
    static constexpr size_t sampleCount = 4;
};

} // namespace cairns

