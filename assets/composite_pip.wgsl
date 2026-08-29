// Full-screen composite. Mirrors composite_pip.{vert,frag,metal}: a single
// vertex-index triangle covering the screen, samples one color texture (the
// resolved forward color) with a V-flip and writes the swap target. The GLSL
// combined sampler2D splits into a texture (binding 0) + sampler (binding 1)
// under WGSL; DrawFullscreen binds N textures at 0..N-1 + the sampler at N.

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

@group(0) @binding(0) var u_color_tex: texture_2d<f32>;
@group(0) @binding(1) var u_color_smp: sampler;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let s = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    return textureSample(u_color_tex, u_color_smp, s);
}
