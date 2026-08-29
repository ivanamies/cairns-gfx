#version 450

// set 2: per-material combined image+sampler (bound on material change).
layout(set = 1, binding = 0) uniform sampler2D u_material_tex;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) flat in uint inEntityId;  // #207

layout(location = 0) out vec4 outColor;
// #206 R32U id buffer (MRT). Per-draw entity id, flat-interpolated from
// the per-draw DrawTmp UBO (set 2 binding 0). Sampled by outline.frag
// for highlight-set lookup + 4-neighbour edge detect.
layout(location = 1) out uint outId;

void main() {
    outColor = texture(u_material_tex, inTexCoord);
    outId = inEntityId;
}
