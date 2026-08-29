// A.3: single red triangle for the L1 ladder rung. Mirrors
// red_triangle.{vert,frag,metal} -- three NDC verts from the vertex index, no
// vertex buffer, no bind groups, no UBO. Centered, ~half-screen, deterministic.

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> @builtin(position) vec4<f32> {
    var p: vec2<f32>;
    if (vid == 0u) {
        p = vec2<f32>(-0.5, -0.5);
    } else if (vid == 1u) {
        p = vec2<f32>(0.5, -0.5);
    } else {
        p = vec2<f32>(0.0, 0.5);
    }
    return vec4<f32>(p, 0.0, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4<f32> {
    return vec4<f32>(1.0, 0.0, 0.0, 1.0);
}
