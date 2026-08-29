#version 450

// #207 outline (plan B): fullscreen post-process. Samples the R32U id buffer
// + the BGRA color buffer. MVP: treat any non-zero id as a "highlightable"
// fragment; tint pixels that sit on a 4-neighbour id discontinuity.
//
// When per-draw {type|id} encoding lands (unlit.frag emits a real id, not 0),
// this pass produces edge silhouettes around every object with zero extra
// geometry work. A future commit replaces the "id != 0" filter with a real
// highlight set (texture-encoded count + ids) so the pass paints only the
// selected entities.

layout(set = 0, binding = 0) uniform sampler2D u_color_tex;
layout(set = 0, binding = 1) uniform usampler2D u_id_tex;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 colour = texture(u_color_tex, inUv);
    uint centre_id = texture(u_id_tex, inUv).r;
    if (centre_id == 0u) {
        outColor = colour;
        return;
    }
    // 4-neighbour edge detect on the id channel.
    vec2 texel = 1.0 / vec2(textureSize(u_id_tex, 0));
    uint n  = texture(u_id_tex, inUv + vec2(0.0,  texel.y)).r;
    uint s  = texture(u_id_tex, inUv + vec2(0.0, -texel.y)).r;
    uint e  = texture(u_id_tex, inUv + vec2( texel.x, 0.0)).r;
    uint w  = texture(u_id_tex, inUv + vec2(-texel.x, 0.0)).r;
    bool on_edge =
        (n != centre_id) || (s != centre_id) ||
        (e != centre_id) || (w != centre_id);
    if (on_edge) {
        outColor = vec4(1.0, 0.95, 0.2, 1.0);  // yellow outline
    } else {
        outColor = colour;
    }
}
