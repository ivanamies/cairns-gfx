// #207 outline (plan B fullscreen post-process). Mirror of outline.vert/
// outline.frag.spv on the Metal backend. MVP: treat any non-zero id as
// highlightable; tint 4-neighbour edge.

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

static bool id_in_highlights(texture2d<uint> highlights, sampler samp,
                              uint id) {
    if (id == 0u) {
        return false;
    }
    uint count = highlights.read(uint2(0u, 0u)).r;
    for (uint i = 0u; i < count; ++i) {
        if (highlights.read(uint2(i + 1u, 0u)).r == id) {
            return true;
        }
    }
    return false;
}

fragment float4 fragmentShader(
        VertexOut in [[stage_in]],
        texture2d<float> color_tex [[texture(0)]],
        texture2d<uint>  id_tex    [[texture(1)]],
        texture2d<uint>  highlights [[texture(2)]],
        sampler color_samp [[sampler(0)]]) {
    // Y-flip on sample (matches composite_pip.metal); see outline.frag.
    float2 s = float2(in.uv.x, 1.0 - in.uv.y);
    float4 colour = color_tex.sample(color_samp, s);
    uint centre = id_tex.sample(color_samp, s).r;
    if (!id_in_highlights(highlights, color_samp, centre)) {
        return colour;
    }
    float2 texel = 1.0 / float2(id_tex.get_width(), id_tex.get_height());
    uint nn = id_tex.sample(color_samp, s + float2(0.0,  texel.y)).r;
    uint ss = id_tex.sample(color_samp, s + float2(0.0, -texel.y)).r;
    uint ee = id_tex.sample(color_samp, s + float2( texel.x, 0.0)).r;
    uint ww = id_tex.sample(color_samp, s + float2(-texel.x, 0.0)).r;
    if (nn != centre || ss != centre || ee != centre || ww != centre) {
        return float4(1.0, 0.95, 0.2, 1.0);
    }
    return colour;
}

} // namespace cube
