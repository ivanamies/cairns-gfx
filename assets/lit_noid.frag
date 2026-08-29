#version 450

// Id-less lit fragment. Unused varyings declared to match lit.vert's
// outputs (vk validation: "vertex output not consumed" warns otherwise).

layout(set = 1, binding = 0) uniform sampler2D u_material_tex;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) flat in uint inUnusedEntityId;
layout(location = 2) in vec3 inWorldNormal;
layout(location = 3) flat in vec4 inLightDir;
layout(location = 4) flat in vec4 inLightColor;
layout(location = 5) flat in vec4 inAmbient;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 n = normalize(inWorldNormal);
    float hl = 0.5 + 0.5 * dot(n, -inLightDir.xyz);
    vec3 lighting =
        inAmbient.rgb + inLightColor.rgb * inLightColor.w * hl * hl;
    vec4 tex = texture(u_material_tex, inTexCoord);
    outColor = vec4(tex.rgb * lighting, tex.a);
}
