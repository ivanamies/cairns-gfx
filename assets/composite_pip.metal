#include <metal_stdlib>
using namespace metal;

namespace composite_pipfx {

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut composite_pip_vertex(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    VertexOut out;
    out.uv = p;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

fragment float4 composite_pip_fragment(VertexOut in [[stage_in]],
                                       texture2d<float> color_tex [[texture(0)]],
                                       sampler s [[sampler(0)]]) {
    float2 sc = float2(in.uv.x, 1.0 - in.uv.y);
    return color_tex.sample(s, sc);
}

}  // namespace composite_pipfx
