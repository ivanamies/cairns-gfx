#version 450

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;   // stream 1: VertexAttribute.uv (offset 48, stride 64)

layout(set = 0, binding = 0) uniform GlobalsUBO {
    mat4 view_proj;
    mat4 inv_view_proj;
    vec4 camera_pos;
    vec4 camera_dir;
    vec4 screen_params;
} globals;

layout(set = 0, binding = 2) uniform DrawTmpUBO {
    mat4 model_matrix;
    uint mesh_id;
    uint tex_id;
    uint sampler_id;
    uint yolo_padding;
} draw_tmp;

layout(location = 0) out vec2 outTexCoord;

void main() {
    gl_Position = globals.view_proj * draw_tmp.model_matrix * inPos;
    outTexCoord = inUV;
}
