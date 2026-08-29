#include <metal_stdlib>
using namespace metal;

// A.3: single red triangle for L1. Mirrors red_triangle.{vert,frag}.glsl;
// no vertex buffer, no textures, no UBO. Centered ~half-screen NDC.

namespace red_trianglefx {

struct VertexOut {
    float4 position [[position]];
};

vertex VertexOut red_triangle_vertex(uint vid [[vertex_id]]) {
    float2 p;
    if (vid == 0) {
        p = float2(-0.5, -0.5);
    } else if (vid == 1) {
        p = float2( 0.5, -0.5);
    } else {
        p = float2( 0.0,  0.5);
    }
    VertexOut out;
    out.position = float4(p, 0.0, 1.0);
    return out;
}

fragment float4 red_triangle_fragment(VertexOut in [[stage_in]]) {
    return float4(1.0, 0.0, 0.0, 1.0);
}

}  // namespace red_trianglefx
