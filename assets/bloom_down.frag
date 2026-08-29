#version 450

// Bloom 2/4: 13-tap downsample (Jimenez 2014). screen.xy = SOURCE texel;
// the render target is half the source in each dimension.

layout(set = 0, binding = 0) uniform sampler2D u_src;

layout(set = 1, binding = 0) uniform PostFxParams {
    vec4 screen;  // x = 1/w, y = 1/h of the SOURCE
    vec4 p0;
    vec4 p1;
    vec4 p2;
};

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 s = vec2(inUv.x, 1.0 - inUv.y);
    vec2 d = screen.xy;
    vec3 a = texture(u_src, s + vec2(-2.0 * d.x, -2.0 * d.y)).rgb;
    vec3 b = texture(u_src, s + vec2(0.0, -2.0 * d.y)).rgb;
    vec3 c = texture(u_src, s + vec2(2.0 * d.x, -2.0 * d.y)).rgb;
    vec3 e = texture(u_src, s + vec2(-2.0 * d.x, 0.0)).rgb;
    vec3 f = texture(u_src, s).rgb;
    vec3 g = texture(u_src, s + vec2(2.0 * d.x, 0.0)).rgb;
    vec3 h = texture(u_src, s + vec2(-2.0 * d.x, 2.0 * d.y)).rgb;
    vec3 i = texture(u_src, s + vec2(0.0, 2.0 * d.y)).rgb;
    vec3 j = texture(u_src, s + vec2(2.0 * d.x, 2.0 * d.y)).rgb;
    vec3 k = texture(u_src, s + vec2(-d.x, -d.y)).rgb;
    vec3 l = texture(u_src, s + vec2(d.x, -d.y)).rgb;
    vec3 m = texture(u_src, s + vec2(-d.x, d.y)).rgb;
    vec3 n = texture(u_src, s + vec2(d.x, d.y)).rgb;
    vec3 o = f * 0.125;
    o += (a + c + h + j) * 0.03125;
    o += (b + e + g + i) * 0.0625;
    o += (k + l + m + n) * 0.125;
    outColor = vec4(o, 1.0);
}
