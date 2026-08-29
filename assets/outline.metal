// #207 outline (plan B fullscreen post-process). Mirror of outline.vert/
// outline.frag.spv on the Metal backend.

#include <metal_stdlib>
using namespace metal;

namespace cube {

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertexShader(uint vid [[vertex_id]]) {
    VertexOut out;
    out.uv = float2(float((vid << 1) & 2), float(vid & 2));
    out.position = float4(out.uv * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

struct Highlights {
    uint count;
    uint ids[64];
};

fragment float4 fragmentShader(
        VertexOut in [[stage_in]],
        texture2d<float> color_tex [[texture(0)]],
        texture2d<uint>  id_tex    [[texture(1)]],
        sampler color_samp [[sampler(0)]],
        constant Highlights& highlights [[buffer(0)]]) {
    float4 colour = color_tex.sample(color_samp, in.uv);
    uint centre = id_tex.sample(color_samp, in.uv).r;
    bool centre_in = false;
    if (centre != 0u) {
        for (uint i = 0u; i < highlights.count; ++i) {
            if (highlights.ids[i] == centre) {
                centre_in = true;
                break;
            }
        }
    }
    if (!centre_in) {
        return colour;
    }
    float2 texel = 1.0 / float2(id_tex.get_width(), id_tex.get_height());
    uint n = id_tex.sample(color_samp, in.uv + float2(0.0,  texel.y)).r;
    uint s = id_tex.sample(color_samp, in.uv + float2(0.0, -texel.y)).r;
    uint e = id_tex.sample(color_samp, in.uv + float2( texel.x, 0.0)).r;
    uint w = id_tex.sample(color_samp, in.uv + float2(-texel.x, 0.0)).r;
    if (n != centre || s != centre || e != centre || w != centre) {
        return float4(1.0, 0.95, 0.2, 1.0);
    }
    return colour;
}

} // namespace cube
