// Forward scene pass (de-bindless, W1 model). Mirrors unlit.vert +
// unlit_noid.frag: group 0 = RenderPassGlobals (dynamic UBO), group 1 = one
// material texture+sampler (the GLSL combined sampler2D split into tex@0 +
// sampler@1), group 2 = DrawTmp (dynamic UBO). Position stream @location(0),
// uv @location(1, offset 48 in the 64-byte attribute stream).

struct Globals {
    view_proj: mat4x4<f32>,
    inv_view_proj: mat4x4<f32>,
    camera_pos: vec4<f32>,
    camera_dir: vec4<f32>,
    screen_params: vec4<f32>,
};

struct DrawTmp {
    model_matrix: mat4x4<f32>,
    mesh_id: u32,
    tex_id: u32,
    sampler_id: u32,
    entity_id: u32,
};

@group(0) @binding(0) var<uniform> globals: Globals;
@group(1) @binding(0) var u_material_tex: texture_2d<f32>;
@group(1) @binding(1) var u_material_smp: sampler;
@group(2) @binding(0) var<uniform> draw_tmp: DrawTmp;

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@location(0) in_pos: vec4<f32>, @location(1) in_uv: vec2<f32>) -> VsOut {
    var out: VsOut;
    out.pos = globals.view_proj * draw_tmp.model_matrix * in_pos;
    out.uv = in_uv;
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    return textureSample(u_material_tex, u_material_smp, in.uv);
}
