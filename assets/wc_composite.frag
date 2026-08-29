#version 450

// Watercolor 4/4: the wash. Sample blurred color through a noise wobble,
// tone-quantize, darken by the edge map (pigment migration), granulate by
// the paper grain. Paper/noise pack rides one RGBA texture (rgb = wobble
// field, a = grain); tiled via fract() in-shader (the shared sampler is
// clamp-to-edge).

layout(set = 0, binding = 0) uniform sampler2D u_blur;
layout(set = 0, binding = 1) uniform sampler2D u_edge;
layout(set = 0, binding = 2) uniform sampler2D u_pn;

layout(set = 1, binding = 0) uniform PostFxParams {
    vec4 screen;  // x = 1/w, y = 1/h, z = w, w = h
    vec4 p0;      // y = quantize levels, z = wobble texels, w = edge strength
    vec4 p1;      // z = granulation strength
    vec4 p2;
};

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 s = vec2(inUv.x, 1.0 - inUv.y);
    vec2 tile = fract(s * screen.zw / 512.0);
    vec4 pn = texture(u_pn, tile);
    vec2 wob = (pn.rg - 0.5) * p0.z * screen.xy;
    vec3 c = texture(u_blur, s + wob).rgb;
    float levels = max(p0.y, 1.0);
    c = floor(c * levels + 0.5) / levels;
    float e = texture(u_edge, s + wob).r;
    c *= 1.0 - p0.w * e;
    c *= 1.0 - p1.z * (pn.a - 0.5);
    outColor = vec4(c, 1.0);
}
