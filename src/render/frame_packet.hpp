#pragma once

// FramePacket -- the contract between the game thread (producer) and the
// render thread (consumer). Game thread builds one packet per frame, pushes
// it on the SPSC queue, and moves on; render thread pops, encodes per-draw
// UBOs (the only allowed BumpAllocate path), runs the render graph, submits,
// presents. Pipeline depth = kFramesInFlight (2).
//
// Lifetime: a packet's storage lives until the slot `frame_idx %
// kFramesInFlight` is reused two frames later. Spans inside the packet are
// backed either by the per-frame FrameArena (game-thread-allocated; render
// thread reads) or by hot_arena (Engine-stable, e.g. material_bind_groups_).
//
// Unused fields stay default-initialised so a packet built before all engine
// state is wired can flow through harmlessly. Once the render-thread loop
// exists, the consumer asserts on the fields it actually needs.

#include "render/render_graph.hpp"
#include "render/render_proxy_arrays.hpp"
#include "rhi/resource_manager.hpp"
#include "util/draw.hpp"
#include "util/draw_key.hpp"
#include "util/render_pass_globals.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>

struct ImDrawData;

namespace cairns {

struct FramePacket {
    // Monotonic frame ID. `frame_idx % kFramesInFlight` selects the FrameArena
    // slot the producer drew this packet's storage from. The consumer must
    // not retain pointers past the next time that slot is reused.
    uint32_t frame_idx = 0;

    // Sim-side bookkeeping; render thread reads to gate the golden dump.
    uint32_t sim_frame = 0;
    uint32_t sim_steps_this_frame = 0;
    uint32_t particle_parity_in = 0;

    // Camera + projection state (already evaluated CPU-side by the game
    // thread; render thread uses these for any per-frame uploads).
    glm::mat4 view_matrix{1.0f};
    glm::mat4 proj_matrix{1.0f};
    glm::vec3 camera_pos{0.0f};
    glm::vec3 camera_dir{0.0f, 0.0f, -1.0f};
    float near_z = 0.1f;
    float far_z = 100.0f;
    rhi::RenderPassGlobals globals{};

    // Draw payload. Spans point at FrameArena-backed storage owned by the
    // packet's slot. `draws` carries dynamic_buffer_offsets = UINT32_MAX
    // until the render thread bumps the per-draw UBOs and patches them.
    std::span<cairns::Draw> draws;
    std::span<const std::pair<cairns::DrawKey, uint32_t>> sorted;
    std::span<const glm::mat4> draw_models;
    std::span<const rhi::Handle<rhi::Texture>> resident_textures;
    std::span<const rhi::Handle<rhi::Buffer>> resident_buffers;

    // Render graph + the per-frame graph-local handles formerly captured as
    // `[&]` locals in draw(). The render thread reads these from the packet
    // when executing the graph, so the values must outlive draw()'s stack.
    rhi::RenderGraph* graph = nullptr;
    rhi::GraphTexture swap_target{};
    rhi::GraphTexture color_tex{};
    rhi::GraphTexture depth_tex{};
    rhi::GraphTexture fwd_depth{};
    rhi::GraphTexture swap_tex{};
    rhi::GraphBuffer sim_ssbo{};
    uint32_t fb_w = 0;
    uint32_t fb_h = 0;
    float clear_color[4] = {0, 0, 0, 1};
    bool draw_imgui = false;

    // Render-thread outputs:
    //   globals_offset: set by RecordFrame after the per-frame globals bump.
    //   particle_parity_out: written by the particle_sim execute lambda;
    //     drained by the game thread before producing the next packet.
    uint32_t globals_offset = 0;
    uint32_t particle_parity_out = 0;

    // ImGui draw data, deep-copied by CloneImGuiDrawData() on the game thread
    // so the render thread reads stable memory while game thread builds frame
    // N+2. Null under CAIRNS_DUMP (golden mode skips ImGui).
    ImDrawData* imgui = nullptr;

    // Golden-capture handshake. When set, the render thread routes Frames'
    // dump_path through the swapchain capture and signals dump_done so the
    // game thread can exit(0) deterministically.
    bool request_dump = false;
    std::filesystem::path dump_path;
};

}  // namespace cairns
