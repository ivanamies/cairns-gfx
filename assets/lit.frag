#version 450

// Lit forward fragment (id MRT variant): half-lambert directional shading.

layout(set = 1, binding = 0) uniform sampler2D u_material_tex;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) flat in uint inEntityId;
layout(location = 2) in vec3 inWorldNormal;
layout(location = 3) flat in vec4 inLightDir;
layout(location = 4) flat in vec4 inLightColor;
layout(location = 5) flat in vec4 inAmbient;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outId;

void main() {
    vec3 n = normalize(inWorldNormal);
    // Half-lambert (pow 2): keeps backsides readable without a fill light.
    float hl = 0.5 + 0.5 * dot(n, -inLightDir.xyz);
    vec3 lighting =
        inAmbient.rgb + inLightColor.rgb * inLightColor.w * hl * hl;
    vec4 tex = texture(u_material_tex, inTexCoord);
    outColor = vec4(tex.rgb * lighting, tex.a);
    outId = inEntityId;
}
