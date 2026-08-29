#version 450

// A.3: single red triangle for the L1 ladder rung.
// Three NDC vertices emitted from gl_VertexIndex; no vertex buffer, no
// bind groups, no textures. Centered, ~half-screen, deterministic.
// Combined with red_triangle.frag this is the "pipeline + clear + one draw"
// tripwire L1 was supposed to be.

void main() {
    vec2 p;
    if (gl_VertexIndex == 0) {
        p = vec2(-0.5, -0.5);
    } else if (gl_VertexIndex == 1) {
        p = vec2( 0.5, -0.5);
    } else {
        p = vec2( 0.0,  0.5);
    }
    gl_Position = vec4(p, 0.0, 1.0);
}
