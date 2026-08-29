#version 450

layout(set = 0, binding = 0) uniform sampler2D uMain;    // FT3 (5 glbs)
layout(set = 0, binding = 1) uniform sampler2D uInset0;  // FT1 (blur glb1)
layout(set = 0, binding = 2) uniform sampler2D uInset1;  // FT2 (depth glb2)

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 s = vec2(inUV.x, 1.0 - inUV.y);  // top-left-origin screen coords
    vec3 col = texture(uMain, s).rgb;
    const float iw = 0.28;
    const float ih = 0.28;
    const float m = 0.02;
    const float b = 0.004;  // border
    if (s.x >= m - b && s.x <= m + iw + b && s.y >= m - b && s.y <= m + ih + b) {
        if (s.x >= m && s.x <= m + iw && s.y >= m && s.y <= m + ih) {
            vec2 luv = (s - vec2(m, m)) / vec2(iw, ih);
            col = texture(uInset0, luv).rgb;
        } else {
            col = vec3(0.9);
        }
    } else if (s.x >= 1.0 - m - iw - b && s.x <= 1.0 - m + b && s.y >= m - b &&
               s.y <= m + ih + b) {
        if (s.x >= 1.0 - m - iw && s.x <= 1.0 - m && s.y >= m && s.y <= m + ih) {
            vec2 luv = (s - vec2(1.0 - m - iw, m)) / vec2(iw, ih);
            col = texture(uInset1, luv).rgb;
        } else {
            col = vec3(0.9);
        }
    }
    outColor = vec4(col, 1.0);
}
