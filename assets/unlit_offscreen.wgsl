// Forward scene pass WITH the id MRT (W1 de-bindless model). Identical to
// unlit.wgsl except the fragment writes a SECOND target: the R32U entity id
// (DrawTmp.entity_id), consumed by outline.wgsl for the selection highlight +
// the pick readback. The engine selects this vs unlit_offscreen_noid per frame
// (id MRT only when an outline/pick consumes it -- #222 Phase A.1).

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

struct FragOut {
    @location(0) color: vec4<f32>,
    @location(1) id: u32,
};

@fragment
fn fs_main(in: VsOut) -> FragOut {
    var out: FragOut;
    out.color = textureSample(u_material_tex, u_material_smp, in.uv);
    out.id = draw_tmp.entity_id;  // flat per-draw; outline.wgsl edge-detects it
    return out;
}
