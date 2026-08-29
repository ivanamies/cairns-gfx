#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>

namespace cairns::rhi {

struct MaterialGpu {
    uint32_t tex_color_id = std::numeric_limits<uint32_t>::max();
    uint32_t wip1 = std::numeric_limits<uint32_t>::max();
    uint32_t wip2 = std::numeric_limits<uint32_t>::max();
    uint32_t wip3 = std::numeric_limits<uint32_t>::max();
    uint32_t wip4 = std::numeric_limits<uint32_t>::max();
    uint32_t sampler_id = std::numeric_limits<uint32_t>::max();
};

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
