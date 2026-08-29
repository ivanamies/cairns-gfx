#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec4 inPos;

struct VertexAttribute {
    vec4 color;
    vec4 tangent;
    vec4 normal;
    vec2 uv;
    vec2 pad1;
};

layout(set = 0, binding = 1) readonly buffer AttrBuf {
    VertexAttribute verts[];
} attrs_buf[];

layout(set = 1, binding = 0) uniform GlobalsUBO {
    mat4 view_proj;
    mat4 inv_view_proj;
    vec4 camera_pos;
    vec4 camera_dir;
    vec4 screen_params;
} globals;

layout(set = 1, binding = 1) uniform MaterialUBO {
    uint tex_color_id;
    uint wip1;
    uint wip2;
    uint wip3;
    uint wip4;
    uint sampler_id;
} material;

layout(set = 1, binding = 2) uniform DrawTmpUBO {
    mat4 model_matrix;
    uint mesh_id;
    uint tex_id;
    uint sampler_id;
    uint yolo_padding;
} draw_tmp;

layout(push_constant) uniform PushConstants {
    uint base_vertex;
} pc;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) flat out uint outTexId;
layout(location = 2) flat out uint outSamplerId;

void main() {
    gl_Position = globals.view_proj * draw_tmp.model_matrix * inPos;
    outTexCoord = attrs_buf[draw_tmp.mesh_id].verts[pc.base_vertex + gl_VertexIndex].uv;
    outTexId = material.tex_color_id;
    outSamplerId = material.sampler_id;
}
