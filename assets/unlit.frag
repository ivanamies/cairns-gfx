#version 450

// set 2: per-material combined image+sampler (bound on material change).
layout(set = 1, binding = 0) uniform sampler2D u_material_tex;

layout(location = 0) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;
// #206 R32U id buffer (MRT). Per-draw encoded {type<<24 | id}; today
// the encoding lives in a future push_constant / instance-buffer
// channel; stubbed to 0 for now so the buffer + pipeline plumbing
// works without flagging vk validation for "shader output mismatch".
layout(location = 1) out uint outId;

void main() {
    outColor = texture(u_material_tex, inTexCoord);
    outId = 0u;
}
