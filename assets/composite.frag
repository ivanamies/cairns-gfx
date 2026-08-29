#version 450

layout(set = 0, binding = 0) uniform sampler2D uColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
    if (inUV.x < 0.5) {
        outColor = texture(uColor, vec2(inUV.x * 2.0, inUV.y));
    } else {
        float d = texture(uDepth, vec2((inUV.x - 0.5) * 2.0, inUV.y)).r;
        float g = pow(clamp((1.0 - d) * 12.0, 0.0, 1.0), 0.6);
        outColor = vec4(g * 0.35, g * 0.65, g, 1.0);
    }
}
