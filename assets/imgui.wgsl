// imgui GPU pass. Mirrors imgui.metal: group 0 = push-constant uniform
// {scale, translate} (dynamic-offset UBO), group 1 = font texture+sampler.
// ImDrawVert: pos float32x2 @location(0), uv float32x2 @location(1),
// col unorm8x4 @location(2). col * font.sample(uv), alpha-blended by the
// pipeline (src_alpha / one_minus_src_alpha).

struct Pc {
    scale: vec2<f32>,
    translate: vec2<f32>,
};

@group(0) @binding(0) var<uniform> pc: Pc;
@group(1) @binding(0) var font_tex: texture_2d<f32>;
@group(1) @binding(1) var font_smp: sampler;

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) col: vec4<f32>,
};

@vertex
fn vs_main(@location(0) in_pos: vec2<f32>,
           @location(1) in_uv: vec2<f32>,
           @location(2) in_col: vec4<f32>) -> VsOut {
    var out: VsOut;
    out.uv = in_uv;
    out.col = in_col;
    out.pos = vec4<f32>(in_pos * pc.scale + pc.translate, 0.0, 1.0);
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    return in.col * textureSample(font_tex, font_smp, in.uv);
}
