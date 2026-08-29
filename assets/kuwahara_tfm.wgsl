// Kuwahara 2/3: 5x5 gaussian (sigma 2) of the structure tensor, then
// eigen-analysis -> (minor eigenvector t, phi, anisotropy A). Mirrors
// kuwahara_tfm.frag.

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
    p0: vec4<f32>,
    p1: vec4<f32>,
    p2: vec4<f32>,
};

@group(0) @binding(0) var u_tensor: texture_2d<f32>;
@group(0) @binding(1) var u_smp: sampler;
@group(0) @binding(2) var<uniform> params: PostFxParams;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let s = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    let d = params.screen.xy;
    var g = vec3<f32>(0.0);
    var total = 0.0;
    for (var j = -2; j <= 2; j = j + 1) {
        for (var i = -2; i <= 2; i = i + 1) {
            let w = exp(-f32(i * i + j * j) / 8.0);
            g = g + w * textureSampleLevel(
                            u_tensor, u_smp,
                            s + vec2<f32>(f32(i) * d.x, f32(j) * d.y),
                            0.0).xyz;
            total = total + w;
        }
    }
    g = g / total;
    let E = g.x;
    let F = g.y;
    let G = g.z;
    let root = sqrt(max((E - G) * (E - G) + 4.0 * F * F, 0.0));
    let lambda1 = 0.5 * (E + G + root);
    let lambda2 = 0.5 * (E + G - root);
    var t = vec2<f32>(lambda1 - E, -F);
    if (length(t) > 1e-8) {
        t = normalize(t);
    } else {
        t = vec2<f32>(0.0, 1.0);
    }
    let phi = atan2(t.y, t.x);
    let denom = lambda1 + lambda2;
    var a = 0.0;
    if (denom > 1e-8) {
        a = (lambda1 - lambda2) / denom;
    }
    return vec4<f32>(t, phi, a);
}
