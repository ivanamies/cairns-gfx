// Bloom 1/4 (Jimenez 2014): soft-threshold bright extraction. Mirrors
// bloom_bright.frag; params ride the dynamic-offset uniform at tex_count+1.

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
    p0: vec4<f32>,      // x = threshold, y = knee
    p1: vec4<f32>,
    p2: vec4<f32>,
};

@group(0) @binding(0) var u_color: texture_2d<f32>;
@group(0) @binding(1) var u_smp: sampler;
@group(0) @binding(2) var<uniform> params: PostFxParams;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let s = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    let c = textureSample(u_color, u_smp, s).rgb;
    let threshold = params.p0.x;
    let knee = max(params.p0.y, 1e-4);
    let br = max(c.r, max(c.g, c.b));
    var soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    let contrib = max(soft, br - threshold) / max(br, 1e-4);
    return vec4<f32>(c * max(contrib, 0.0), 1.0);
}
