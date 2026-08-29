#version 450

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 s = vec2(inUV.x, 1.0 - inUV.y);
    vec2 texel = 1.0 / vec2(textureSize(uTex, 0));
    float w[5] = float[](1.0, 4.0, 6.0, 4.0, 1.0);
    float spread = 3.0;
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 5; ++x) {
            float wt = w[x] * w[y];
            vec2 off = vec2(float(x - 2), float(y - 2)) * texel * spread;
            sum += texture(uTex, s + off) * wt;
            wsum += wt;
        }
    }
    outColor = sum / wsum;
}
