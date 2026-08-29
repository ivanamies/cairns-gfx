#version 450

// Bloom 1/4 (Jimenez SIGGRAPH 2014): soft-threshold bright extraction into
// the half-res RGBA16F ladder base.

layout(set = 0, binding = 0) uniform sampler2D u_color;

layout(set = 1, binding = 0) uniform PostFxParams {
    vec4 screen;  // x = 1/w, y = 1/h of the SOURCE
    vec4 p0;      // x = threshold, y = knee
    vec4 p1;
    vec4 p2;
};

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    // Y-flip on sample (matches outline.frag): keeps the downstream
    // composite flip correct through every chain link.
    vec2 s = vec2(inUv.x, 1.0 - inUv.y);
    vec3 c = texture(u_color, s).rgb;
    float threshold = p0.x;
    float knee = max(p0.y, 1e-4);
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contrib = max(soft, br - threshold) / max(br, 1e-4);
    outColor = vec4(c * max(contrib, 0.0), 1.0);
}
