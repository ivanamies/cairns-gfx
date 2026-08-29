#include <metal_stdlib>
using namespace metal;

// Bloom (Jimenez SIGGRAPH 2014 dual filter), 4 fullscreen entry points.
// Mirrors bloom_{bright,down,up,combine}.frag; params ride the 64B
// PostFxParams block at fragment buffer 0 (DrawFullscreenParams).

namespace bloom {

struct PostFxParams {
    float4 screen;  // x = 1/w, y = 1/h of the SOURCE
    float4 p0;      // bright: x = threshold, y = knee; combine: z = intensity
    float4 p1;
    float4 p2;
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut fullscreen_vertex(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    VertexOut out;
    out.uv = p;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

fragment float4 bright_fragment(VertexOut in [[stage_in]],
                                texture2d<float> color_tex [[texture(0)]],
                                sampler smp [[sampler(0)]],
                                constant PostFxParams& params [[buffer(0)]]) {
    float2 s = float2(in.uv.x, 1.0 - in.uv.y);
    float3 c = color_tex.sample(smp, s).rgb;
    float threshold = params.p0.x;
    float knee = max(params.p0.y, 1e-4);
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contrib = max(soft, br - threshold) / max(br, 1e-4);
    return float4(c * max(contrib, 0.0), 1.0);
}

fragment float4 down_fragment(VertexOut in [[stage_in]],
                              texture2d<float> src_tex [[texture(0)]],
                              sampler smp [[sampler(0)]],
                              constant PostFxParams& params [[buffer(0)]]) {
    float2 s = float2(in.uv.x, 1.0 - in.uv.y);
    float2 d = params.screen.xy;
    float3 a = src_tex.sample(smp, s + float2(-2.0 * d.x, -2.0 * d.y)).rgb;
    float3 b = src_tex.sample(smp, s + float2(0.0, -2.0 * d.y)).rgb;
    float3 c = src_tex.sample(smp, s + float2(2.0 * d.x, -2.0 * d.y)).rgb;
    float3 e = src_tex.sample(smp, s + float2(-2.0 * d.x, 0.0)).rgb;
    float3 f = src_tex.sample(smp, s).rgb;
    float3 g = src_tex.sample(smp, s + float2(2.0 * d.x, 0.0)).rgb;
    float3 h = src_tex.sample(smp, s + float2(-2.0 * d.x, 2.0 * d.y)).rgb;
    float3 i = src_tex.sample(smp, s + float2(0.0, 2.0 * d.y)).rgb;
    float3 j = src_tex.sample(smp, s + float2(2.0 * d.x, 2.0 * d.y)).rgb;
    float3 k = src_tex.sample(smp, s + float2(-d.x, -d.y)).rgb;
    float3 l = src_tex.sample(smp, s + float2(d.x, -d.y)).rgb;
    float3 m = src_tex.sample(smp, s + float2(-d.x, d.y)).rgb;
    float3 n = src_tex.sample(smp, s + float2(d.x, d.y)).rgb;
    float3 o = f * 0.125;
    o += (a + c + h + j) * 0.03125;
    o += (b + e + g + i) * 0.0625;
    o += (k + l + m + n) * 0.125;
    return float4(o, 1.0);
}

fragment float4 up_fragment(VertexOut in [[stage_in]],
                            texture2d<float> lower_tex [[texture(0)]],
                            texture2d<float> skip_tex [[texture(1)]],
                            sampler smp [[sampler(0)]],
                            constant PostFxParams& params [[buffer(0)]]) {
    float2 s = float2(in.uv.x, 1.0 - in.uv.y);
    float2 d = params.screen.xy;
    float3 o = lower_tex.sample(smp, s + float2(-d.x, -d.y)).rgb;
    o += lower_tex.sample(smp, s + float2(0.0, -d.y)).rgb * 2.0;
    o += lower_tex.sample(smp, s + float2(d.x, -d.y)).rgb;
    o += lower_tex.sample(smp, s + float2(-d.x, 0.0)).rgb * 2.0;
    o += lower_tex.sample(smp, s).rgb * 4.0;
    o += lower_tex.sample(smp, s + float2(d.x, 0.0)).rgb * 2.0;
    o += lower_tex.sample(smp, s + float2(-d.x, d.y)).rgb;
    o += lower_tex.sample(smp, s + float2(0.0, d.y)).rgb * 2.0;
    o += lower_tex.sample(smp, s + float2(d.x, d.y)).rgb;
    o /= 16.0;
    return float4(o + skip_tex.sample(smp, s).rgb, 1.0);
}

fragment float4 combine_fragment(VertexOut in [[stage_in]],
                                 texture2d<float> color_tex [[texture(0)]],
                                 texture2d<float> bloom_tex [[texture(1)]],
                                 sampler smp [[sampler(0)]],
                                 constant PostFxParams& params [[buffer(0)]]) {
    float2 s = float2(in.uv.x, 1.0 - in.uv.y);
    float3 c = color_tex.sample(smp, s).rgb;
    float3 b = bloom_tex.sample(smp, s).rgb;
    return float4(c + params.p0.z * b, 1.0);
}

}  // namespace bloom
