#version 450

layout(set = 0, binding = 0) uniform sampler2D uColor;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 s = vec2(inUV.x, 1.0 - inUV.y);
    outColor = texture(uColor, s);
}
