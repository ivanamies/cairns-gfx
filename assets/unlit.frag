#version 450

// set 2: per-material combined image+sampler (bound on material change).
layout(set = 2, binding = 0) uniform sampler2D u_material_tex;

layout(location = 0) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(u_material_tex, inTexCoord);
}
