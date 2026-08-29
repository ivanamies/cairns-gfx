#version 450

// Kuwahara 1/3 (Kyprianidis 2009): Sobel of RGB -> structure tensor
// (E, F, G) = (dot(gx,gx), dot(gx,gy), dot(gy,gy)) into RGBA16F.

layout(set = 0, binding = 0) uniform sampler2D u_color;

layout(set = 1, binding = 0) uniform PostFxParams {
    vec4 screen;  // x = 1/w, y = 1/h, z = w, w = h
    vec4 p0;
    vec4 p1;
    vec4 p2;
};

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    // Y-flip on sample (matches outline.frag): keeps the downstream
    // composite flip correct through every chain link.
    vec2 s = vec2(inUv.x, 1.0 - inUv.y);
    vec2 d = screen.xy;
    vec3 c00 = texture(u_color, s + vec2(-d.x, -d.y)).rgb;
    vec3 c10 = texture(u_color, s + vec2( 0.0, -d.y)).rgb;
    vec3 c20 = texture(u_color, s + vec2( d.x, -d.y)).rgb;
    vec3 c01 = texture(u_color, s + vec2(-d.x,  0.0)).rgb;
    vec3 c21 = texture(u_color, s + vec2( d.x,  0.0)).rgb;
    vec3 c02 = texture(u_color, s + vec2(-d.x,  d.y)).rgb;
    vec3 c12 = texture(u_color, s + vec2( 0.0,  d.y)).rgb;
    vec3 c22 = texture(u_color, s + vec2( d.x,  d.y)).rgb;
    vec3 gx = (c20 + 2.0 * c21 + c22 - c00 - 2.0 * c01 - c02) / 4.0;
    vec3 gy = (c02 + 2.0 * c12 + c22 - c00 - 2.0 * c10 - c20) / 4.0;
    outColor = vec4(dot(gx, gx), dot(gx, gy), dot(gy, gy), 1.0);
}
