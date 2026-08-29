#version 450

// Bloom 3/4: 9-tap tent upsample of the lower mip + in-shader add of the
// same-res skip texture (no blend state -- deterministic). screen.xy =
// texel of the LOWER (smaller) source.

layout(set = 0, binding = 0) uniform sampler2D u_lower;
layout(set = 0, binding = 1) uniform sampler2D u_skip;

layout(set = 1, binding = 0) uniform PostFxParams {
    vec4 screen;  // x = 1/w, y = 1/h of u_lower
    vec4 p0;
    vec4 p1;
    vec4 p2;
};

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 s = vec2(inUv.x, 1.0 - inUv.y);
    vec2 d = screen.xy;
    vec3 o = texture(u_lower, s + vec2(-d.x, -d.y)).rgb;
    o += texture(u_lower, s + vec2(0.0, -d.y)).rgb * 2.0;
    o += texture(u_lower, s + vec2(d.x, -d.y)).rgb;
    o += texture(u_lower, s + vec2(-d.x, 0.0)).rgb * 2.0;
    o += texture(u_lower, s).rgb * 4.0;
    o += texture(u_lower, s + vec2(d.x, 0.0)).rgb * 2.0;
    o += texture(u_lower, s + vec2(-d.x, d.y)).rgb;
    o += texture(u_lower, s + vec2(0.0, d.y)).rgb * 2.0;
    o += texture(u_lower, s + vec2(d.x, d.y)).rgb;
    o /= 16.0;
    outColor = vec4(o + texture(u_skip, s).rgb, 1.0);
}
