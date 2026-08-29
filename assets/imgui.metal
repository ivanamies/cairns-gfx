#include <metal_stdlib>
using namespace metal;

namespace imguicairns {

struct VertexIn {
    float2 pos [[attribute(0)]];
    float2 uv [[attribute(1)]];
    float4 col [[attribute(2)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
    float4 col;
};

struct PushConstants {
    float2 scale;
    float2 translate;
};

vertex VertexOut imgui_vertex(VertexIn in [[stage_in]],
                              constant PushConstants& pc [[buffer(1)]]) {
    VertexOut out;
    out.uv = in.uv;
    out.col = in.col;
    out.position = float4(in.pos * pc.scale + pc.translate, 0.0, 1.0);
    return out;
}

fragment float4 imgui_fragment(VertexOut in [[stage_in]],
                               texture2d<float> font [[texture(0)]],
                               sampler s [[sampler(0)]]) {
    return in.col * font.sample(s, in.uv);
}

}  // namespace imguicairns
