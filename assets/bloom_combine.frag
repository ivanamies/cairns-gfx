#version 450

// Bloom 4/4: additive combine of the scene color and the bloom ladder's
// full contribution at half res (bilinear stretch).

layout(set = 0, binding = 0) uniform sampler2D u_color;
layout(set = 0, binding = 1) uniform sampler2D u_bloom;

layout(set = 1, binding = 0) uniform PostFxParams {
    vec4 screen;
    vec4 p0;      // z = intensity
    vec4 p1;
    vec4 p2;
};

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 s = vec2(inUv.x, 1.0 - inUv.y);
    vec3 c = texture(u_color, s).rgb;
    vec3 b = texture(u_bloom, s).rgb;
    outColor = vec4(c + p0.z * b, 1.0);
}
