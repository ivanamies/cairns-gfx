#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace cairns {

struct GpuTRS {
    glm::vec4 T{0.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 R{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 S{1.0f, 1.0f, 1.0f, 0.0f};
};

struct GpuSceneHeader {
    uint32_t node_count = 0;
    uint32_t joint_count = 0;
    uint32_t channel_count = 0;
    uint32_t sampler_count = 0;

    // Offsets into the 3 PACKED gpu buffers, in that buffer's ELEMENT units
    // (#231 12->6 SSBO pack). i32-element into ae_i32_buf_: parent/topo/
    // joint_nodes/times. vec4-element into ae_vec4_buf_: bind_pose (3 vec4/
    // joint T,R,S), values (1 vec4/key), inverse_binds (4 vec4/joint = mat4
    // columns). uvec4-element into ae_word16_buf_: channel/sampler (16B each).
    uint32_t parent_off = 0;        // i32-element into ae_i32_buf_
    uint32_t topo_off = 0;          // i32-element into ae_i32_buf_
    uint32_t bind_pose_off = 0;     // vec4-element into ae_vec4_buf_ (i*3)
    uint32_t channel_off = 0;       // uvec4-element into ae_word16_buf_

    uint32_t sampler_off = 0;       // uvec4-element into ae_word16_buf_
    uint32_t times_off = 0;         // i32-element into ae_i32_buf_
    uint32_t values_off = 0;        // vec4-element into ae_vec4_buf_
    uint32_t joint_nodes_off = 0;   // i32-element into ae_i32_buf_

    uint32_t inverse_binds_off = 0; // vec4-element into ae_vec4_buf_ (j*4)
    int32_t  mesh_node = -1;
    float duration = 0.0f;
    float _pad0 = 0.0f;
};

struct GpuChannel {
    int32_t  node_idx = -1;
    uint32_t path = 0;
    int32_t  sampler_idx = -1;
    uint32_t _pad0 = 0;
};

struct GpuSampler {
    uint32_t times_off = 0;   // i32-element into ae_i32_buf_ (after +base)
    uint32_t values_off = 0;  // vec4-element into ae_vec4_buf_ (after +base)
    uint32_t count = 0;
    uint32_t interp = 0;
};

struct GpuActorRecord {
    uint32_t scene_idx = 0;
    uint32_t world_scratch_base = 0;
    uint32_t palette_out_base = 0;
    float time = 0.0f;
};

}  // namespace cairns
