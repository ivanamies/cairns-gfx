#version 450

// Kuwahara 2/3: 5x5 gaussian (sigma 2) of the structure tensor, then
// eigen-analysis -> (minor eigenvector t, phi, anisotropy A) into RGBA16F.

layout(set = 0, binding = 0) uniform sampler2D u_tensor;

layout(set = 1, binding = 0) uniform PostFxParams {
    vec4 screen;  // x = 1/w, y = 1/h, z = w, w = h
    vec4 p0;
    vec4 p1;
    vec4 p2;
};

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 s = vec2(inUv.x, 1.0 - inUv.y);
    vec2 d = screen.xy;
    vec3 g = vec3(0.0);
    float total = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            float w = exp(-float(i * i + j * j) / 8.0);
            g += w * texture(u_tensor,
                             s + vec2(float(i) * d.x, float(j) * d.y)).xyz;
            total += w;
        }
    }
    g /= total;
    float E = g.x;
    float F = g.y;
    float G = g.z;
    float root = sqrt(max((E - G) * (E - G) + 4.0 * F * F, 0.0));
    float lambda1 = 0.5 * (E + G + root);
    float lambda2 = 0.5 * (E + G - root);
    vec2 t = vec2(lambda1 - E, -F);
    t = (length(t) > 1e-8) ? normalize(t) : vec2(0.0, 1.0);
    float phi = atan(t.y, t.x);
    float denom = lambda1 + lambda2;
    float A = (denom > 1e-8) ? (lambda1 - lambda2) / denom : 0.0;
    outColor = vec4(t, phi, A);
}
