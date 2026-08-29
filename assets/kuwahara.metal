#include <metal_stdlib>
using namespace metal;

// Anisotropic Kuwahara (Kyprianidis 2009), 3 fullscreen passes. Mirrors
// kuwahara_{tensor,tfm,filter}.frag; params ride the 64B PostFxParams block
// at fragment buffer 0 (DrawFullscreenParams).

namespace kuwahara {

struct PostFxParams {
    float4 screen;  // x = 1/w, y = 1/h, z = w, w = h
    float4 p0;      // filter: x = radius, y = q, z = alpha
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

fragment float4 tensor_fragment(VertexOut in [[stage_in]],
                                texture2d<float> color_tex [[texture(0)]],
                                sampler smp [[sampler(0)]],
                                constant PostFxParams& params [[buffer(0)]]) {
    float2 s = float2(in.uv.x, 1.0 - in.uv.y);
    float2 d = params.screen.xy;
    float3 c00 = color_tex.sample(smp, s + float2(-d.x, -d.y)).rgb;
    float3 c10 = color_tex.sample(smp, s + float2( 0.0, -d.y)).rgb;
    float3 c20 = color_tex.sample(smp, s + float2( d.x, -d.y)).rgb;
    float3 c01 = color_tex.sample(smp, s + float2(-d.x,  0.0)).rgb;
    float3 c21 = color_tex.sample(smp, s + float2( d.x,  0.0)).rgb;
    float3 c02 = color_tex.sample(smp, s + float2(-d.x,  d.y)).rgb;
    float3 c12 = color_tex.sample(smp, s + float2( 0.0,  d.y)).rgb;
    float3 c22 = color_tex.sample(smp, s + float2( d.x,  d.y)).rgb;
    float3 gx = (c20 + 2.0 * c21 + c22 - c00 - 2.0 * c01 - c02) / 4.0;
    float3 gy = (c02 + 2.0 * c12 + c22 - c00 - 2.0 * c10 - c20) / 4.0;
    return float4(dot(gx, gx), dot(gx, gy), dot(gy, gy), 1.0);
}

fragment float4 tfm_fragment(VertexOut in [[stage_in]],
                             texture2d<float> tensor_tex [[texture(0)]],
                             sampler smp [[sampler(0)]],
                             constant PostFxParams& params [[buffer(0)]]) {
    float2 s = float2(in.uv.x, 1.0 - in.uv.y);
    float2 d = params.screen.xy;
    float3 g = float3(0.0);
    float total = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            float w = exp(-float(i * i + j * j) / 8.0);
            g += w * tensor_tex.sample(
                         smp, s + float2(float(i) * d.x, float(j) * d.y)).xyz;
            total += w;
        }
    }
    g /= total;
    float E = g.x;
    float F = g.y;
    float G = g.z;
    float root = sqrt(max((E - G) * (E - G) + 4.0 * F * F, 0.0));
    float lambda1 = 0.5 * (E + G + root);
    float lambda2 = 0.5 * (E + G - root);
    float2 t = float2(lambda1 - E, -F);
    t = (length(t) > 1e-8) ? normalize(t) : float2(0.0, 1.0);
    float phi = atan2(t.y, t.x);
    float denom = lambda1 + lambda2;
    float A = (denom > 1e-8) ? (lambda1 - lambda2) / denom : 0.0;
    return float4(t, phi, A);
}

fragment float4 filter_fragment(VertexOut in [[stage_in]],
                                texture2d<float> color_tex [[texture(0)]],
                                texture2d<float> tfm_tex [[texture(1)]],
                                sampler smp [[sampler(0)]],
                                constant PostFxParams& params [[buffer(0)]]) {
    float2 s = float2(in.uv.x, 1.0 - in.uv.y);
    float radius = max(params.p0.x, 1.0);
    float q = params.p0.y;
    float alpha = max(params.p0.z, 0.001);

    float4 t = tfm_tex.sample(smp, s);
    float a = radius * clamp((alpha + t.w) / alpha, 0.1, 2.0);
    float b = radius * clamp(alpha / (alpha + t.w), 0.1, 2.0);
    float cos_phi = cos(t.z);
    float sin_phi = sin(t.z);
    // Column-major, same as the GLSL mat2(cos,-sin,sin,cos) constructor.
    float2x2 R = float2x2(float2(cos_phi, -sin_phi), float2(sin_phi, cos_phi));
    float2x2 S = float2x2(float2(0.5 / a, 0.0), float2(0.0, 0.5 / b));
    float2x2 SR = S * R;
    int max_x = int(sqrt(a * a * cos_phi * cos_phi +
                         b * b * sin_phi * sin_phi));
    int max_y = int(sqrt(a * a * sin_phi * sin_phi +
                         b * b * cos_phi * cos_phi));

    float4 m[8];
    float3 sq[8];
    for (int k = 0; k < 8; ++k) {
        m[k] = float4(0.0);
        sq[k] = float3(0.0);
    }
    for (int j = -max_y; j <= max_y; ++j) {
        for (int i = -max_x; i <= max_x; ++i) {
            float2 v = SR * float2(float(i), float(j));
            if (dot(v, v) > 0.25) {
                continue;
            }
            float3 c = color_tex.sample(
                           smp, s + float2(float(i) * params.screen.x,
                                           float(j) * params.screen.y)).rgb;
            float w[8];
            float z;
            float vxx = 0.33 - 3.77 * v.x * v.x;
            float vyy = 0.33 - 3.77 * v.y * v.y;
            z = max(0.0, v.y + vxx);  w[0] = z * z;
            z = max(0.0, -v.x + vyy); w[2] = z * z;
            z = max(0.0, -v.y + vxx); w[4] = z * z;
            z = max(0.0, v.x + vyy);  w[6] = z * z;
            float2 vr = 0.7071067812 * float2(v.x - v.y, v.x + v.y);
            vxx = 0.33 - 3.77 * vr.x * vr.x;
            vyy = 0.33 - 3.77 * vr.y * vr.y;
            z = max(0.0, vr.y + vxx);  w[1] = z * z;
            z = max(0.0, -vr.x + vyy); w[3] = z * z;
            z = max(0.0, -vr.y + vxx); w[5] = z * z;
            z = max(0.0, vr.x + vyy);  w[7] = z * z;
            float sum = w[0] + w[1] + w[2] + w[3] +
                        w[4] + w[5] + w[6] + w[7];
            if (sum < 1e-8) {
                continue;
            }
            float g = exp(-3.125 * dot(v, v)) / sum;
            for (int k = 0; k < 8; ++k) {
                float wk = w[k] * g;
                m[k] += float4(c * wk, wk);
                sq[k] += c * c * wk;
            }
        }
    }
    float4 o = float4(0.0);
    for (int k = 0; k < 8; ++k) {
        if (m[k].w < 1e-8) {
            continue;
        }
        float3 mean = m[k].rgb / m[k].w;
        float3 var = abs(sq[k] / m[k].w - mean * mean);
        float sigma2 = var.r + var.g + var.b;
        float w = 1.0 / (1.0 + pow(255.0 * sigma2, 0.5 * q));
        o += float4(mean * w, w);
    }
    return (o.w > 1e-8) ? float4(o.rgb / o.w, 1.0)
                        : float4(color_tex.sample(smp, s).rgb, 1.0);
}

}  // namespace kuwahara
