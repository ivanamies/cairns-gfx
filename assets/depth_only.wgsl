// Depth-only shadow pass: position stream only, no color targets. The
// fragment stage exists but writes nothing (webgpu requires an entry when
// the builder supplies a fragment state; a zero-target fragment is legal).

struct Globals {
    view_proj: mat4x4<f32>,
    inv_view_proj: mat4x4<f32>,
    camera_pos: vec4<f32>,
    camera_dir: vec4<f32>,
    screen_params: vec4<f32>,
    light_view_proj: mat4x4<f32>,
    light_dir: vec4<f32>,
    light_color: vec4<f32>,
    ambient: vec4<f32>,
};

struct DrawTmp {
    model_matrix: mat4x4<f32>,
    mesh_id: u32,
    tex_id: u32,
    sampler_id: u32,
    entity_id: u32,
};

@group(0) @binding(0) var<uniform> globals: Globals;
@group(2) @binding(0) var<uniform> draw_tmp: DrawTmp;

@vertex
fn vs_main(@location(0) in_pos: vec4<f32>) -> @builtin(position) vec4<f32> {
    // The shadow pass uploads its globals with the light VP in the view_proj
    // slot -- this shader never knows it is a light.
    return globals.view_proj * draw_tmp.model_matrix * in_pos;
}

@fragment
fn fs_main() {
}
