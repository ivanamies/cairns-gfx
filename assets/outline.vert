#version 450

// #207 outline (plan B: fullscreen post-process that samples id_off + color).
// Single-triangle fullscreen draw -- no vertex buffer, gl_VertexIndex is the
// only input. The classic three-vert pattern that paints a triangle covering
// the entire NDC quad with a single backface-cull on the far corners.

layout(location = 0) out vec2 outUv;

void main() {
    outUv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(outUv * 2.0 - 1.0, 0.0, 1.0);
}
