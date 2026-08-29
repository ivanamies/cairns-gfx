#include <metal_stdlib>
using namespace metal;

namespace composite3fx {

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut composite3_vertex(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    VertexOut out;
    out.uv = p;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

fragment float4 composite3_fragment(VertexOut in [[stage_in]],
                                    texture2d<float> main_tex [[texture(0)]],
                                    texture2d<float> inset0 [[texture(1)]],
                                    texture2d<float> inset1 [[texture(2)]],
                                    sampler s [[sampler(0)]]) {
    float2 sc = float2(in.uv.x, 1.0 - in.uv.y);
    float3 col = main_tex.sample(s, sc).rgb;
    const float iw = 0.28;
    const float ih = 0.28;
    const float m = 0.02;
    const float b = 0.004;
    if (sc.x >= m - b && sc.x <= m + iw + b && sc.y >= m - b && sc.y <= m + ih + b) {
        if (sc.x >= m && sc.x <= m + iw && sc.y >= m && sc.y <= m + ih) {
            float2 luv = (sc - float2(m, m)) / float2(iw, ih);
            col = inset0.sample(s, luv).rgb;
        } else {
            col = float3(0.9);
        }
    } else if (sc.x >= 1.0 - m - iw - b && sc.x <= 1.0 - m + b && sc.y >= m - b &&
               sc.y <= m + ih + b) {
        if (sc.x >= 1.0 - m - iw && sc.x <= 1.0 - m && sc.y >= m && sc.y <= m + ih) {
            float2 luv = (sc - float2(1.0 - m - iw, m)) / float2(iw, ih);
            col = inset1.sample(s, luv).rgb;
        } else {
            col = float3(0.9);
        }
    }
    return float4(col, 1.0);
}

}  // namespace composite3fx
