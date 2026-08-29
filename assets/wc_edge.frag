#version 450

// Watercolor 3/4: Sobel on the blurred color AND linearized depth ->
// edge intensity (pigment migrates to edges in the composite).

layout(set = 0, binding = 0) uniform sampler2D u_color;
layout(set = 0, binding = 1) uniform sampler2D u_depth;

layout(set = 1, binding = 0) uniform PostFxParams {
    vec4 screen;  // x = 1/w, y = 1/h
    vec4 p0;
    vec4 p1;      // x = color-edge gain, y = depth-edge gain
    vec4 p2;      // z = near, w = far
};

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

float lum(vec2 uv) {
    vec3 c = texture(u_color, uv).rgb;
    return dot(c, vec3(0.299, 0.587, 0.114));
}

float lin_depth(vec2 uv) {
    float d = texture(u_depth, uv).r;
    float near = p2.z;
    float far = p2.w;
    float lin = near * far / max(far - d * (far - near), 1e-6);
    return clamp((lin - near) / max(far - near, 1e-6), 0.0, 1.0);
}

void main() {
    vec2 s = vec2(inUv.x, 1.0 - inUv.y);
    vec2 d = screen.xy;
    float l00 = lum(s + vec2(-d.x, -d.y));
    float l10 = lum(s + vec2(0.0, -d.y));
    float l20 = lum(s + vec2(d.x, -d.y));
    float l01 = lum(s + vec2(-d.x, 0.0));
    float l21 = lum(s + vec2(d.x, 0.0));
    float l02 = lum(s + vec2(-d.x, d.y));
    float l12 = lum(s + vec2(0.0, d.y));
    float l22 = lum(s + vec2(d.x, d.y));
    float lgx = l20 + 2.0 * l21 + l22 - l00 - 2.0 * l01 - l02;
    float lgy = l02 + 2.0 * l12 + l22 - l00 - 2.0 * l10 - l20;
    float ce = length(vec2(lgx, lgy));
    float z00 = lin_depth(s + vec2(-d.x, -d.y));
    float z10 = lin_depth(s + vec2(0.0, -d.y));
    float z20 = lin_depth(s + vec2(d.x, -d.y));
    float z01 = lin_depth(s + vec2(-d.x, 0.0));
    float z21 = lin_depth(s + vec2(d.x, 0.0));
    float z02 = lin_depth(s + vec2(-d.x, d.y));
    float z12 = lin_depth(s + vec2(0.0, d.y));
    float z22 = lin_depth(s + vec2(d.x, d.y));
    float zgx = z20 + 2.0 * z21 + z22 - z00 - 2.0 * z01 - z02;
    float zgy = z02 + 2.0 * z12 + z22 - z00 - 2.0 * z10 - z20;
    float de = length(vec2(zgx, zgy));
    float e = clamp(ce * p1.x + de * p1.y, 0.0, 1.0);
    outColor = vec4(e, e, e, 1.0);
}
