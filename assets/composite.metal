#include <metal_stdlib>
using namespace metal;

namespace composite {

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut composite_vertex(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    VertexOut out;
    out.uv = p;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

fragment float4 composite_fragment(VertexOut in [[stage_in]],
                                   texture2d<float> color_tex [[texture(0)]],
                                   texture2d<float> depth_tex [[texture(1)]],
                                   sampler s [[sampler(0)]]) {
    if (in.uv.x < 0.5) {
        return color_tex.sample(s, float2(in.uv.x * 2.0, 1.0 - in.uv.y));
    }
    float d = depth_tex.sample(s, float2((in.uv.x - 0.5) * 2.0, 1.0 - in.uv.y)).r;
    float g = pow(clamp((1.0 - d) * 12.0, 0.0, 1.0), 0.6);
    return float4(g * 0.35, g * 0.65, g, 1.0);
}

}  // namespace composite
