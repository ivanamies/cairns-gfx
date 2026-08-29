#version 450

// #222 Phase A.1: id-less variant of unlit.frag. Selected by RecordFrame
// when outline_on || pick_pending is false, sparing the GPU the inEntityId
// flat-interpolation + the R32U store. Same set/binding layout as unlit.frag
// for shared set-1.

layout(set = 1, binding = 0) uniform sampler2D u_material_tex;

layout(location = 0) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(u_material_tex, inTexCoord);
}
