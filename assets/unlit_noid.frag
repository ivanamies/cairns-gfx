#version 450

// #222 Phase A.1: id-less variant of unlit.frag. Selected by RecordFrame
// when outline_on || pick_pending is false, sparing the GPU the R32U store.
// Same set/binding layout as unlit.frag for shared set-1.
//
// inEntityId at location 1 is declared (matches unlit.vert's output) but
// not used -- without it, vk validation warns "vertex output not consumed
// by fragment". flat-interpolating a uint that's never read is free; the
// GPU still skips the location-1 fragment store path because the pipeline
// has color_count=1.

layout(set = 1, binding = 0) uniform sampler2D u_material_tex;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) flat in uint inUnusedEntityId;

layout(location = 0) out vec4 outColor;

void main() {
    // touch the entity-id input so the compiler keeps it bound; the alpha
    // bit-shuffle is a 0.0 no-op since 0u * 0.0 == 0.0 and color.a stays.
    outColor = texture(u_material_tex, inTexCoord);
    outColor.a += float(inUnusedEntityId & 0u) * 0.0;
}
