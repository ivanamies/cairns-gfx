// Kuwahara 1/3 (Kyprianidis 2009): Sobel of RGB -> structure tensor
// (E, F, G). Mirrors kuwahara_tensor.frag; params ride the dynamic-offset
// uniform DrawFullscreenParams binds at tex_count+1.

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

@group(0) @binding(0) var u_color: texture_2d<f32>;
@group(0) @binding(1) var u_smp: sampler;
@group(0) @binding(2) var<uniform> params: PostFxParams;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let s = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    let d = params.screen.xy;
    let c00 = textureSample(u_color, u_smp, s + vec2<f32>(-d.x, -d.y)).rgb;
    let c10 = textureSample(u_color, u_smp, s + vec2<f32>(0.0, -d.y)).rgb;
    let c20 = textureSample(u_color, u_smp, s + vec2<f32>(d.x, -d.y)).rgb;
    let c01 = textureSample(u_color, u_smp, s + vec2<f32>(-d.x, 0.0)).rgb;
    let c21 = textureSample(u_color, u_smp, s + vec2<f32>(d.x, 0.0)).rgb;
    let c02 = textureSample(u_color, u_smp, s + vec2<f32>(-d.x, d.y)).rgb;
    let c12 = textureSample(u_color, u_smp, s + vec2<f32>(0.0, d.y)).rgb;
    let c22 = textureSample(u_color, u_smp, s + vec2<f32>(d.x, d.y)).rgb;
    let gx = (c20 + 2.0 * c21 + c22 - c00 - 2.0 * c01 - c02) / 4.0;
    let gy = (c02 + 2.0 * c12 + c22 - c00 - 2.0 * c10 - c20) / 4.0;
    return vec4<f32>(dot(gx, gx), dot(gx, gy), dot(gy, gy), 1.0);
}
