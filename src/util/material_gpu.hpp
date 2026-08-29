#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>

namespace cairns::rhi {

// Per-MATERIAL dynamic-offset UBO (slot 2, Draw::dynamic_buffer_offsets[0]).
// std140-safe 64 B: four vec4-aligned rows. One block per referenced material
// per frame, offset shared across its draws. ids.x/.y keep the old
// tex_color_id/sampler_id semantics.
struct MaterialGpu {
    glm::vec4 base_color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    glm::vec4 params0 = glm::vec4(0.0f);
    glm::vec4 params1 = glm::vec4(0.0f);
    glm::uvec4 ids = glm::uvec4(std::numeric_limits<uint32_t>::max());
};
static_assert(sizeof(MaterialGpu) == 64, "std140 block layout");

struct DrawTmp {
    glm::mat4 model_matrix;
    uint32_t mesh_id = std::numeric_limits<uint32_t>::max();
    uint32_t tex_id = std::numeric_limits<uint32_t>::max();
    uint32_t sampler_id = std::numeric_limits<uint32_t>::max();
    // Per-draw entity id (== MeshProxy::entity_id; +1 over the entt::entity
    // index so 0 means "background"). Written to the R32U id_off attachment
    // by unlit.frag; sampled by outline.frag.
    uint32_t entity_id = 0;
};

}  // namespace cairns::rhi
