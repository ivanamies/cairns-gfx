#version 450

// Id-less lit fragment. Unused varyings declared to match lit.vert's
// outputs (vk validation: "vertex output not consumed" warns otherwise).

layout(set = 1, binding = 0) uniform sampler2D u_material_tex;
layout(set = 3, binding = 0) uniform sampler2D u_shadow;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) flat in uint inUnusedEntityId;
layout(location = 2) in vec3 inWorldNormal;
layout(location = 3) flat in vec4 inLightDir;
layout(location = 4) flat in vec4 inLightColor;
layout(location = 5) flat in vec4 inAmbient;
layout(location = 6) in vec4 inShadowCoord;

layout(location = 0) out vec4 outColor;

float shadow_factor(vec3 n) {
    if (inLightDir.w <= 0.0) {
        return 1.0;
    }
    vec3 pc = inShadowCoord.xyz / inShadowCoord.w;
    vec2 uv = pc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        pc.z > 1.0) {
        return 1.0;
    }
    float ndl = max(dot(n, -inLightDir.xyz), 0.0);
    float bias = max(0.0015, 0.004 * (1.0 - ndl));
    float cur = pc.z - bias;
    vec2 texel = 1.0 / vec2(textureSize(u_shadow, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float d = texture(u_shadow, uv + vec2(x, y) * texel).r;
            sum += (cur <= d) ? 1.0 : 0.0;
        }
    }
    return mix(1.0, sum / 9.0, inLightDir.w);
}

void main() {
    vec3 n = normalize(inWorldNormal);
    float hl = 0.5 + 0.5 * dot(n, -inLightDir.xyz);
    float sh = shadow_factor(n);
    vec3 lighting =
        inAmbient.rgb + inLightColor.rgb * inLightColor.w * hl * hl * sh;
    vec4 tex = texture(u_material_tex, inTexCoord);
    outColor = vec4(tex.rgb * lighting, tex.a);
}
