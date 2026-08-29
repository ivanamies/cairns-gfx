#version 450

// #207 outline (plan B): fullscreen post-process. Samples the R32U id buffer
// + the BGRA color buffer. If any of the 4-neighbour pixels has a DIFFERENT
// id from the centre AND that centre id is in the highlight set (kept as a
// uniform buffer of uint ids), tint the pixel with the outline colour.
//
// Today the engine emits id = 0 for every fragment (the per-draw {type|id}
// encoding lives in a future instance-buffer channel), so all pixels share
// the same id and this pass is a structural no-op. When unlit.frag starts
// emitting real ids, this pass will produce visible edge silhouettes around
// the highlight set with zero extra geometry work (vs the stencil-grow
// plan A which re-issues the highlighted draws).

layout(set = 0, binding = 0) uniform sampler2D u_color_tex;
layout(set = 0, binding = 1) uniform usampler2D u_id_tex;

// kMaxHighlights matches Engine::kMaxHighlights -- bump both together when
// the highlight set is allowed to grow.
const int kMaxHighlights = 64;
layout(set = 0, binding = 2) uniform Highlights {
    uint count;
    uint ids[kMaxHighlights];
} u_highlights;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

bool id_in_highlight(uint id) {
    if (id == 0u) {
        return false;
    }
    for (int i = 0; i < int(u_highlights.count); ++i) {
        if (u_highlights.ids[i] == id) {
            return true;
        }
    }
    return false;
}

void main() {
    vec4 colour = texture(u_color_tex, inUv);
    uint centre_id = texture(u_id_tex, inUv).r;
    if (!id_in_highlight(centre_id)) {
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
