// Bloom 3/4: 9-tap tent upsample + in-shader skip add (no blend state).
// Mirrors bloom_up.frag; screen.xy = texel of the LOWER source.

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
    screen: vec4<f32>,  // x = 1/w, y = 1/h of u_lower
    p0: vec4<f32>,
    p1: vec4<f32>,
    p2: vec4<f32>,
};

@group(0) @binding(0) var u_lower: texture_2d<f32>;
@group(0) @binding(1) var u_skip: texture_2d<f32>;
@group(0) @binding(2) var u_smp: sampler;
@group(0) @binding(3) var<uniform> params: PostFxParams;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let s = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    let d = params.screen.xy;
    var o = textureSample(u_lower, u_smp, s + vec2<f32>(-d.x, -d.y)).rgb;
    o = o + textureSample(u_lower, u_smp, s + vec2<f32>(0.0, -d.y)).rgb * 2.0;
    o = o + textureSample(u_lower, u_smp, s + vec2<f32>(d.x, -d.y)).rgb;
    o = o + textureSample(u_lower, u_smp, s + vec2<f32>(-d.x, 0.0)).rgb * 2.0;
    o = o + textureSample(u_lower, u_smp, s).rgb * 4.0;
    o = o + textureSample(u_lower, u_smp, s + vec2<f32>(d.x, 0.0)).rgb * 2.0;
    o = o + textureSample(u_lower, u_smp, s + vec2<f32>(-d.x, d.y)).rgb;
    o = o + textureSample(u_lower, u_smp, s + vec2<f32>(0.0, d.y)).rgb * 2.0;
    o = o + textureSample(u_lower, u_smp, s + vec2<f32>(d.x, d.y)).rgb;
    o = o / 16.0;
    return vec4<f32>(o + textureSample(u_skip, u_smp, s).rgb, 1.0);
}
