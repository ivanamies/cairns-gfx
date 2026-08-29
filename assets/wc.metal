#include <metal_stdlib>
using namespace metal;

// Watercolor (Luft & Deussen 2006), 3 fullscreen entry points (blur runs
// twice, h then v). Mirrors wc_{blur,edge,composite}.frag; params ride the
// 64B PostFxParams block at fragment buffer 0 (DrawFullscreenParams).

namespace wc {

struct PostFxParams {
    float4 screen;  // x = 1/w, y = 1/h, z = w, w = h (SOURCE dims)
    float4 p0;      // x = blur step, y = levels, z = wobble, w = edge str
    float4 p1;      // x = color-edge gain, y = depth-edge gain, z = grain
    float4 p2;      // xy = blur dir; z = near, w = far
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

fragment float4 blur_fragment(VertexOut in [[stage_in]],
                              texture2d<float> color_tex [[texture(0)]],
                              sampler smp [[sampler(0)]],
                              constant PostFxParams& params [[buffer(0)]]) {
    float2 s = float2(in.uv.x, 1.0 - in.uv.y);
    float2 step = params.p2.xy * params.screen.xy * max(params.p0.x, 0.5);
    const float w[5] = {0.2270270270, 0.1945945946, 0.1216216216,
                        0.0540540541, 0.0162162162};
    float3 c = color_tex.sample(smp, s).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        c += color_tex.sample(smp, s + step * float(i)).rgb * w[i];
        c += color_tex.sample(smp, s - step * float(i)).rgb * w[i];
    }
    return float4(c, 1.0);
}

static float wc_lum(texture2d<float> t, sampler smp, float2 uv) {
    float3 c = t.sample(smp, uv).rgb;
    return dot(c, float3(0.299, 0.587, 0.114));
}

static float wc_lin_depth(texture2d<float> t, sampler smp, float2 uv,
                          float near, float far) {
    float d = t.sample(smp, uv).r;
    float lin = near * far / max(far - d * (far - near), 1e-6);
    return clamp((lin - near) / max(far - near, 1e-6), 0.0, 1.0);
}

fragment float4 edge_fragment(VertexOut in [[stage_in]],
                              texture2d<float> color_tex [[texture(0)]],
                              texture2d<float> depth_tex [[texture(1)]],
                              sampler smp [[sampler(0)]],
                              constant PostFxParams& params [[buffer(0)]]) {
    float2 s = float2(in.uv.x, 1.0 - in.uv.y);
    float2 d = params.screen.xy;
    float near = params.p2.z;
    float far = params.p2.w;
    float l00 = wc_lum(color_tex, smp, s + float2(-d.x, -d.y));
    float l10 = wc_lum(color_tex, smp, s + float2(0.0, -d.y));
    float l20 = wc_lum(color_tex, smp, s + float2(d.x, -d.y));
    float l01 = wc_lum(color_tex, smp, s + float2(-d.x, 0.0));
    float l21 = wc_lum(color_tex, smp, s + float2(d.x, 0.0));
    float l02 = wc_lum(color_tex, smp, s + float2(-d.x, d.y));
    float l12 = wc_lum(color_tex, smp, s + float2(0.0, d.y));
    float l22 = wc_lum(color_tex, smp, s + float2(d.x, d.y));
    float lgx = l20 + 2.0 * l21 + l22 - l00 - 2.0 * l01 - l02;
    float lgy = l02 + 2.0 * l12 + l22 - l00 - 2.0 * l10 - l20;
    float ce = length(float2(lgx, lgy));
    float z00 = wc_lin_depth(depth_tex, smp, s + float2(-d.x, -d.y), near, far);
    float z10 = wc_lin_depth(depth_tex, smp, s + float2(0.0, -d.y), near, far);
    float z20 = wc_lin_depth(depth_tex, smp, s + float2(d.x, -d.y), near, far);
    float z01 = wc_lin_depth(depth_tex, smp, s + float2(-d.x, 0.0), near, far);
    float z21 = wc_lin_depth(depth_tex, smp, s + float2(d.x, 0.0), near, far);
    float z02 = wc_lin_depth(depth_tex, smp, s + float2(-d.x, d.y), near, far);
    float z12 = wc_lin_depth(depth_tex, smp, s + float2(0.0, d.y), near, far);
    float z22 = wc_lin_depth(depth_tex, smp, s + float2(d.x, d.y), near, far);
    float zgx = z20 + 2.0 * z21 + z22 - z00 - 2.0 * z01 - z02;
    float zgy = z02 + 2.0 * z12 + z22 - z00 - 2.0 * z10 - z20;
    float de = length(float2(zgx, zgy));
    float e = clamp(ce * params.p1.x + de * params.p1.y, 0.0, 1.0);
    return float4(e, e, e, 1.0);
}

fragment float4 composite_fragment(VertexOut in [[stage_in]],
                                   texture2d<float> blur_tex [[texture(0)]],
                                   texture2d<float> edge_tex [[texture(1)]],
                                   texture2d<float> pn_tex [[texture(2)]],
                                   sampler smp [[sampler(0)]],
                                   constant PostFxParams& params
                                       [[buffer(0)]]) {
    float2 s = float2(in.uv.x, 1.0 - in.uv.y);
    float2 tile = fract(s * params.screen.zw / 512.0);
    float4 pn = pn_tex.sample(smp, tile);
    float2 wob = (pn.rg - 0.5) * params.p0.z * params.screen.xy;
    float3 c = blur_tex.sample(smp, s + wob).rgb;
    float levels = max(params.p0.y, 1.0);
    c = floor(c * levels + 0.5) / levels;
    float e = edge_tex.sample(smp, s + wob).r;
    c *= 1.0 - params.p0.w * e;
    c *= 1.0 - params.p1.z * (pn.a - 0.5);
    return float4(c, 1.0);
}

}  // namespace wc
