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
// #207 highlights as R32U texture (65x1). Texel (0,0).r = count, texels
// (i,0).r = ids[i-1] for i in 1..count. Outlined ids are matched against
// this set; non-highlighted entities skip the edge-detect and fall through
// to the unmodified color.
layout(set = 0, binding = 2) uniform usampler2D u_highlights;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

bool id_in_highlights(uint id) {
    if (id == 0u) {
        return false;
    }
    uint count = texelFetch(u_highlights, ivec2(0, 0), 0).r;
    for (uint i = 0u; i < count; ++i) {
        uint hid = texelFetch(u_highlights, ivec2(int(i + 1u), 0), 0).r;
        if (hid == id) {
            return true;
        }
    }
    return false;
}

void main() {
    // Y-flip on sample (matches composite_pip.frag): color_off + id_off were
    // written under a negative-height viewport, and outline_off will be too,
    // so reading flipped here keeps composite's downstream flip correct.
    vec2 s = vec2(inUv.x, 1.0 - inUv.y);
    vec4 colour = texture(u_color_tex, s);
    uint centre_id = texture(u_id_tex, s).r;
    if (!id_in_highlights(centre_id)) {
        outColor = colour;
        return;
    }
    vec2 texel = 1.0 / vec2(textureSize(u_id_tex, 0));
    uint n  = texture(u_id_tex, s + vec2(0.0,  texel.y)).r;
    uint sN = texture(u_id_tex, s + vec2(0.0, -texel.y)).r;
    uint e  = texture(u_id_tex, s + vec2( texel.x, 0.0)).r;
    uint w  = texture(u_id_tex, s + vec2(-texel.x, 0.0)).r;
    bool on_edge =
        (n != centre_id) || (sN != centre_id) ||
        (e != centre_id) || (w  != centre_id);
    if (on_edge) {
        outColor = vec4(1.0, 0.95, 0.2, 1.0);  // yellow outline
    } else {
        outColor = colour;
    }
}
