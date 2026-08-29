// Lit forward pass WITH the id MRT: half-lambert directional shading.
// Light params ride flat varyings from the vertex stage (no extra binding).

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
@group(1) @binding(0) var u_material_tex: texture_2d<f32>;
@group(1) @binding(1) var u_material_smp: sampler;
@group(2) @binding(0) var<uniform> draw_tmp: DrawTmp;

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) world_normal: vec3<f32>,
    @location(2) @interpolate(flat) light_dir: vec4<f32>,
    @location(3) @interpolate(flat) light_color: vec4<f32>,
    @location(4) @interpolate(flat) ambient: vec4<f32>,
};

@vertex
fn vs_main(@location(0) in_pos: vec4<f32>, @location(1) in_uv: vec2<f32>,
           @location(2) in_normal: vec4<f32>) -> VsOut {
    var out: VsOut;
    out.pos = globals.view_proj * draw_tmp.model_matrix * in_pos;
    out.uv = in_uv;
    let m3 = mat3x3<f32>(draw_tmp.model_matrix[0].xyz,
                         draw_tmp.model_matrix[1].xyz,
                         draw_tmp.model_matrix[2].xyz);
    out.world_normal = m3 * in_normal.xyz;
    out.light_dir = globals.light_dir;
    out.light_color = globals.light_color;
    out.ambient = globals.ambient;
    return out;
}

struct FragOut {
    @location(0) color: vec4<f32>,
    @location(1) id: u32,
};

@fragment
fn fs_main(in: VsOut) -> FragOut {
    var out: FragOut;
    let n = normalize(in.world_normal);
    // Half-lambert (pow 2): keeps backsides readable without a fill light.
    let hl = 0.5 + 0.5 * dot(n, -in.light_dir.xyz);
    let lighting = in.ambient.rgb + in.light_color.rgb * in.light_color.w * hl * hl;
    let tex = textureSample(u_material_tex, u_material_smp, in.uv);
    out.color = vec4<f32>(tex.rgb * lighting, tex.a);
    out.id = draw_tmp.entity_id;
    return out;
}
