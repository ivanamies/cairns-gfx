#include <metal_stdlib>
using namespace metal;

namespace depthonly {

struct VertexInput {
    float4 pos [[attribute(0)]];
};

struct RenderPassGlobals {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4 camera_pos;
    float4 camera_dir;
    float4 screen_params;
};

struct MaterialGpu {
    uint32_t tex_color_id;
    uint32_t wip1;
    uint32_t wip2;
    uint32_t wip3;
    uint32_t wip4;
    uint32_t sampler_id;
};

struct DrawTmp {
    float4x4 model_matrix;
    uint mesh_id;
    uint tex_id;
    uint sampler_id;
    uint yolo_padding;
};

struct VertexOut {
    float4 position [[position]];
};

vertex VertexOut depth_only_vertex(VertexInput in [[stage_in]],
                                   constant RenderPassGlobals& globals [[buffer(1)]],
                                   constant MaterialGpu& material [[buffer(2)]],
                                   constant DrawTmp& draw_tmp [[buffer(4)]]) {
    VertexOut out;
    out.position = globals.view_proj * draw_tmp.model_matrix * in.pos;
    return out;
}

fragment void depth_only_fragment() {
}

}  // namespace depthonly
