#version 450

// Watercolor 1-2/4 (Luft & Deussen 2006): separable 9-tap gaussian, run
// twice (p2.xy = (1,0) then (0,1)). The blurred color is the wash base.

layout(set = 0, binding = 0) uniform sampler2D u_color;

layout(set = 1, binding = 0) uniform PostFxParams {
    vec4 screen;  // x = 1/w, y = 1/h of the SOURCE
    vec4 p0;      // x = blur step scale (texels per tap)
    vec4 p1;
    vec4 p2;      // xy = blur direction
};

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 s = vec2(inUv.x, 1.0 - inUv.y);
    vec2 step = p2.xy * screen.xy * max(p0.x, 0.5);
    const float w[5] = float[](0.2270270270, 0.1945945946, 0.1216216216,
                               0.0540540541, 0.0162162162);
    vec3 c = texture(u_color, s).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        c += texture(u_color, s + step * float(i)).rgb * w[i];
        c += texture(u_color, s - step * float(i)).rgb * w[i];
    }
    outColor = vec4(c, 1.0);
}
