// Watercolor 4/4: wobble -> quantize -> edge darken -> granulate. Mirrors
// wc_composite.frag; the paper/noise pack tiles via fract() in-shader.

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
    p0: vec4<f32>,      // y = levels, z = wobble texels, w = edge strength
    p1: vec4<f32>,      // z = granulation strength
    p2: vec4<f32>,
};

@group(0) @binding(0) var u_blur: texture_2d<f32>;
@group(0) @binding(1) var u_edge: texture_2d<f32>;
@group(0) @binding(2) var u_pn: texture_2d<f32>;
@group(0) @binding(3) var u_smp: sampler;
@group(0) @binding(4) var<uniform> params: PostFxParams;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let s = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    let tile = fract(s * params.screen.zw / 512.0);
    let pn = textureSampleLevel(u_pn, u_smp, tile, 0.0);
    let wob = (pn.rg - 0.5) * params.p0.z * params.screen.xy;
    var c = textureSampleLevel(u_blur, u_smp, s + wob, 0.0).rgb;
    let levels = max(params.p0.y, 1.0);
    c = floor(c * levels + 0.5) / levels;
    let e = textureSampleLevel(u_edge, u_smp, s + wob, 0.0).r;
    c = c * (1.0 - params.p0.w * e);
    c = c * (1.0 - params.p1.z * (pn.a - 0.5));
    return vec4<f32>(c, 1.0);
}
