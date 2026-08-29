#version 450

// Lit forward vertex: unlit.vert + the world normal + the light params
// forwarded as flat varyings, so the fragment stage needs NO extra
// descriptor (set 0 stays vertex-declared on every backend).

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;      // stream 1: offset 48, stride 64
layout(location = 2) in vec4 inNormal;  // stream 1: offset 32, stride 64

layout(set = 0, binding = 0) uniform GlobalsUBO {
    mat4 view_proj;
    mat4 inv_view_proj;
    vec4 camera_pos;
    vec4 camera_dir;
    vec4 screen_params;
    mat4 light_view_proj;
    vec4 light_dir;    // xyz normalized; w = shadow_strength
    vec4 light_color;  // rgb; w = intensity
    vec4 ambient;
} globals;

layout(set = 2, binding = 0) uniform DrawTmpUBO {
    mat4 model_matrix;
    uint mesh_id;
    uint tex_id;
    uint sampler_id;
    uint entity_id;
} draw_tmp;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) flat out uint outEntityId;
layout(location = 2) out vec3 outWorldNormal;
layout(location = 3) flat out vec4 outLightDir;
layout(location = 4) flat out vec4 outLightColor;
layout(location = 5) flat out vec4 outAmbient;

void main() {
    gl_Position = globals.view_proj * draw_tmp.model_matrix * inPos;
    outTexCoord = inUV;
    outEntityId = draw_tmp.entity_id;
    // Uniform-scale assumption: the model 3x3 rotates+scales the normal;
    // the fragment renormalizes.
    outWorldNormal = mat3(draw_tmp.model_matrix) * inNormal.xyz;
    outLightDir = globals.light_dir;
    outLightColor = globals.light_color;
    outAmbient = globals.ambient;
}
