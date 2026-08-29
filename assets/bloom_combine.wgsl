// Bloom 4/4: additive combine (color + intensity * bloom). Mirrors
// bloom_combine.frag.

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
    screen: vec4<f32>,
    p0: vec4<f32>,      // z = intensity
    p1: vec4<f32>,
    p2: vec4<f32>,
};

@group(0) @binding(0) var u_color: texture_2d<f32>;
@group(0) @binding(1) var u_bloom: texture_2d<f32>;
@group(0) @binding(2) var u_smp: sampler;
@group(0) @binding(3) var<uniform> params: PostFxParams;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let s = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    let c = textureSample(u_color, u_smp, s).rgb;
    let b = textureSample(u_bloom, u_smp, s).rgb;
    return vec4<f32>(c + params.p0.z * b, 1.0);
}
