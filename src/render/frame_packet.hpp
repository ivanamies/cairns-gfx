// src/render/frame_packet.hpp
//
// Per-frame producer/consumer handoff between the game thread and the render
// thread. Owns no GPU data: spans point into per-slot storage on Engine, plain
// scalars are by value. See the "Architecture -- Acquire the SLOT before
// writing a byte" section of the threading plan.

#pragma once

#include "rhi/resources.hpp"
#include "util/draw.hpp"
#include "util/draw_key.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>

#include <glm/glm.hpp>

struct ImDrawData;

namespace cairns {

struct FramePacket {
    uint32_t frame_idx = 0;
    uint32_t slot = 0;

    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::vec3 camera_pos{0.0f};
    glm::vec3 camera_dir{0.0f, 0.0f, -1.0f};
    float near_z = 0.1f;
    float far_z = 100.0f;

    std::span<const Draw> draws;
    std::span<const std::pair<DrawKey, uint32_t>> sorted;
    std::span<const rhi::Handle<rhi::Texture>> resident_textures;

    uint32_t sim_steps_this_frame = 0;
    float fixed_dt = 1.0f / 60.0f;

    uint32_t particle_parity_in = 0;
    uint32_t particle_parity_out = 0;

    ImDrawData* imgui_snapshot = nullptr;

    bool request_dump = false;
    std::filesystem::path dump_path;
};

}  // namespace cairns
