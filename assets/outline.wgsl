// #207 outline / selection highlight -- WebGPU port of outline.frag + outline.metal.
// Fullscreen triangle (vertex_index, no vertex buffers). Samples the scene color
// (BGRA float), the R32U entity-id buffer, and the R32U highlights set
// (texel 0 = count, texels 1..count = highlighted ids). Paints a yellow edge
// around highlighted entities. id + highlights are INTEGER textures, so they are
// read with textureLoad -- WGSL/Dawn forbids sampling (filtering) integer textures.

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VsOut {
    let p = vec2<f32>(f32((vid << 1u) & 2u), f32(vid & 2u));
    var out: VsOut;
    out.uv = p;
    out.pos = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

@group(0) @binding(0) var color_tex: texture_2d<f32>;
@group(0) @binding(1) var id_tex: texture_2d<u32>;
@group(0) @binding(2) var highlights_tex: texture_2d<u32>;
@group(0) @binding(3) var color_smp: sampler;

fn id_in_highlights(id: u32) -> bool {
    if (id == 0u) { return false; }
    let count = textureLoad(highlights_tex, vec2<i32>(0, 0), 0).r;
    for (var i: u32 = 0u; i < count; i = i + 1u) {
        if (textureLoad(highlights_tex, vec2<i32>(i32(i) + 1, 0), 0).r == id) {
            return true;
        }
    }
    return false;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    // Y-flip on sample (matches composite_pip/outline.frag): color_off + id_off
    // were written under a negative-height viewport.
    let s = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    let colour = textureSample(color_tex, color_smp, s);
    let dims = vec2<i32>(textureDimensions(id_tex));
    let pix = vec2<i32>(s * vec2<f32>(dims));
    let centre = textureLoad(id_tex, pix, 0).r;
    if (!id_in_highlights(centre)) {
        return colour;
    }
    // 4-neighbour id discontinuity -> on the silhouette edge. textureLoad clamps
    // out-of-bounds to 0, which is fine at the screen border.
    let n = textureLoad(id_tex, pix + vec2<i32>(0, 1), 0).r;
    let s2 = textureLoad(id_tex, pix + vec2<i32>(0, -1), 0).r;
    let e = textureLoad(id_tex, pix + vec2<i32>(1, 0), 0).r;
    let w = textureLoad(id_tex, pix + vec2<i32>(-1, 0), 0).r;
    if (n != centre || s2 != centre || e != centre || w != centre) {
        return vec4<f32>(1.0, 0.95, 0.2, 1.0);  // yellow outline
    }
    return colour;
}
