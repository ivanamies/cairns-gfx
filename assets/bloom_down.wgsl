// Bloom 2/4: 13-tap downsample (Jimenez 2014). Mirrors bloom_down.frag;
// screen.xy = SOURCE texel.

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
    screen: vec4<f32>,  // x = 1/w, y = 1/h of the SOURCE
    p0: vec4<f32>,
    p1: vec4<f32>,
    p2: vec4<f32>,
};

@group(0) @binding(0) var u_src: texture_2d<f32>;
@group(0) @binding(1) var u_smp: sampler;
@group(0) @binding(2) var<uniform> params: PostFxParams;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let s = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    let d = params.screen.xy;
    let a = textureSample(u_src, u_smp, s + vec2<f32>(-2.0 * d.x, -2.0 * d.y)).rgb;
    let b = textureSample(u_src, u_smp, s + vec2<f32>(0.0, -2.0 * d.y)).rgb;
    let c = textureSample(u_src, u_smp, s + vec2<f32>(2.0 * d.x, -2.0 * d.y)).rgb;
    let e = textureSample(u_src, u_smp, s + vec2<f32>(-2.0 * d.x, 0.0)).rgb;
    let f = textureSample(u_src, u_smp, s).rgb;
    let g = textureSample(u_src, u_smp, s + vec2<f32>(2.0 * d.x, 0.0)).rgb;
    let h = textureSample(u_src, u_smp, s + vec2<f32>(-2.0 * d.x, 2.0 * d.y)).rgb;
    let i = textureSample(u_src, u_smp, s + vec2<f32>(0.0, 2.0 * d.y)).rgb;
    let j = textureSample(u_src, u_smp, s + vec2<f32>(2.0 * d.x, 2.0 * d.y)).rgb;
    let k = textureSample(u_src, u_smp, s + vec2<f32>(-d.x, -d.y)).rgb;
    let l = textureSample(u_src, u_smp, s + vec2<f32>(d.x, -d.y)).rgb;
    let m = textureSample(u_src, u_smp, s + vec2<f32>(-d.x, d.y)).rgb;
    let n = textureSample(u_src, u_smp, s + vec2<f32>(d.x, d.y)).rgb;
    var o = f * 0.125;
    o = o + (a + c + h + j) * 0.03125;
    o = o + (b + e + g + i) * 0.0625;
    o = o + (k + l + m + n) * 0.125;
    return vec4<f32>(o, 1.0);
}
