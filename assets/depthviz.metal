#include <metal_stdlib>
using namespace metal;

namespace depthvizfx {

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut depthviz_vertex(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    VertexOut out;
    out.uv = p;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

fragment float4 depthviz_fragment(VertexOut in [[stage_in]],
                                  texture2d<float> depth_tex [[texture(0)]],
                                  sampler s [[sampler(0)]]) {
    float d = depth_tex.sample(s, float2(in.uv.x, 1.0 - in.uv.y)).r;
    float g = pow(clamp((1.0 - d) * 12.0, 0.0, 1.0), 0.6);
    return float4(g * 0.35, g * 0.65, g, 1.0);
}

}  // namespace depthvizfx
