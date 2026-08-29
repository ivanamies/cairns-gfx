// Kuwahara 3/3: 8-sector anisotropic filter (Kyprianidis GPU Pro 2010
// polynomial weights). Mirrors kuwahara_filter.frag. textureSampleLevel
// throughout: the loop bounds are per-pixel (non-uniform control flow), where
// WGSL forbids implicit-lod sampling; the inputs are single-mip transients so
// level 0 is exact.

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VsOut {
    let p = vec2<f32>(f32((vid << 1u) & 2u), f32(vid & 2u));
    var out: VsOut;
    out.uv = p;
    out.pos = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

struct PostFxParams {
    screen: vec4<f32>,  // x = 1/w, y = 1/h, z = w, w = h
    p0: vec4<f32>,      // x = radius, y = q, z = alpha
    p1: vec4<f32>,
    p2: vec4<f32>,
};

@group(0) @binding(0) var u_color: texture_2d<f32>;
@group(0) @binding(1) var u_tfm: texture_2d<f32>;
@group(0) @binding(2) var u_smp: sampler;
@group(0) @binding(3) var<uniform> params: PostFxParams;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let s = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    let radius = max(params.p0.x, 1.0);
    let q = params.p0.y;
    let alpha = max(params.p0.z, 0.001);

    let t = textureSampleLevel(u_tfm, u_smp, s, 0.0);
    let a = radius * clamp((alpha + t.w) / alpha, 0.1, 2.0);
    let b = radius * clamp(alpha / (alpha + t.w), 0.1, 2.0);
    let cos_phi = cos(t.z);
    let sin_phi = sin(t.z);
    let r_mat = mat2x2<f32>(vec2<f32>(cos_phi, -sin_phi),
                            vec2<f32>(sin_phi, cos_phi));
    let s_mat = mat2x2<f32>(vec2<f32>(0.5 / a, 0.0), vec2<f32>(0.0, 0.5 / b));
    let sr = s_mat * r_mat;
    let max_x = i32(sqrt(a * a * cos_phi * cos_phi +
                         b * b * sin_phi * sin_phi));
    let max_y = i32(sqrt(a * a * sin_phi * sin_phi +
                         b * b * cos_phi * cos_phi));

    var m: array<vec4<f32>, 8>;
    var sq: array<vec3<f32>, 8>;
    for (var k = 0; k < 8; k = k + 1) {
        m[k] = vec4<f32>(0.0);
        sq[k] = vec3<f32>(0.0);
    }
    for (var j = -max_y; j <= max_y; j = j + 1) {
        for (var i = -max_x; i <= max_x; i = i + 1) {
            let v = sr * vec2<f32>(f32(i), f32(j));
            if (dot(v, v) > 0.25) {
                continue;
            }
            let c = textureSampleLevel(
                        u_color, u_smp,
                        s + vec2<f32>(f32(i) * params.screen.x,
                                      f32(j) * params.screen.y), 0.0).rgb;
            var w: array<f32, 8>;
            var z: f32;
            var vxx = 0.33 - 3.77 * v.x * v.x;
            var vyy = 0.33 - 3.77 * v.y * v.y;
            z = max(0.0, v.y + vxx);  w[0] = z * z;
            z = max(0.0, -v.x + vyy); w[2] = z * z;
            z = max(0.0, -v.y + vxx); w[4] = z * z;
            z = max(0.0, v.x + vyy);  w[6] = z * z;
            let vr = 0.7071067812 * vec2<f32>(v.x - v.y, v.x + v.y);
            vxx = 0.33 - 3.77 * vr.x * vr.x;
            vyy = 0.33 - 3.77 * vr.y * vr.y;
            z = max(0.0, vr.y + vxx);  w[1] = z * z;
            z = max(0.0, -vr.x + vyy); w[3] = z * z;
            z = max(0.0, -vr.y + vxx); w[5] = z * z;
            z = max(0.0, vr.x + vyy);  w[7] = z * z;
            let sum = w[0] + w[1] + w[2] + w[3] + w[4] + w[5] + w[6] + w[7];
            if (sum < 1e-8) {
                continue;
            }
            let g = exp(-3.125 * dot(v, v)) / sum;
            for (var k = 0; k < 8; k = k + 1) {
                let wk = w[k] * g;
                m[k] = m[k] + vec4<f32>(c * wk, wk);
                sq[k] = sq[k] + c * c * wk;
            }
        }
    }
    var o = vec4<f32>(0.0);
    for (var k = 0; k < 8; k = k + 1) {
        if (m[k].w < 1e-8) {
            continue;
        }
        let mean = m[k].rgb / m[k].w;
        let variance = abs(sq[k] / m[k].w - mean * mean);
        let sigma2 = variance.r + variance.g + variance.b;
        let w = 1.0 / (1.0 + pow(255.0 * sigma2, 0.5 * q));
        o = o + vec4<f32>(mean * w, w);
    }
    if (o.w > 1e-8) {
        return vec4<f32>(o.rgb / o.w, 1.0);
    }
    return vec4<f32>(textureSampleLevel(u_color, u_smp, s, 0.0).rgb, 1.0);
}
