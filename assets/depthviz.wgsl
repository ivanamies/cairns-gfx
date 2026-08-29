// Depth-strip visualization (nested-graph PIP), WebGPU port of depthviz.metal.
// Fullscreen triangle (vertex_index trick, no vertex buffers); samples the
// scene depth attachment and maps near surfaces to a blue silhouette. The
// depth target is Depth32Float -> texture_depth_2d + sampleType "depth"; the
// bind group layout (pipelines.cpp depthviz branch) matches.

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

@group(0) @binding(0) var depth_tex: texture_depth_2d;
@group(0) @binding(1) var depth_smp: sampler;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let sc = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    let d = textureSample(depth_tex, depth_smp, sc);
    let g = pow(clamp((1.0 - d) * 12.0, 0.0, 1.0), 0.6);
    return vec4<f32>(g * 0.35, g * 0.65, g, 1.0);
}
