#include <metal_stdlib>
using namespace metal;

namespace blurfx {

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut blur_vertex(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    VertexOut out;
    out.uv = p;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

fragment float4 blur_fragment(VertexOut in [[stage_in]],
                              texture2d<float> tex [[texture(0)]],
                              sampler s [[sampler(0)]]) {
    float2 uv = float2(in.uv.x, 1.0 - in.uv.y);
    float2 texel = 1.0 / float2(tex.get_width(), tex.get_height());
    float w[5] = {1.0, 4.0, 6.0, 4.0, 1.0};
    float spread = 3.0;
    float4 sum = float4(0.0);
    float wsum = 0.0;
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 5; ++x) {
            float wt = w[x] * w[y];
            float2 off = float2(float(x - 2), float(y - 2)) * texel * spread;
            sum += tex.sample(s, uv + off) * wt;
            wsum += wt;
        }
    }
    return sum / wsum;
}

}  // namespace blurfx
