// #221 Skinning Phase 4: Metal skin compute kernel (mirror of skin.comp.glsl).
// Compiled by `xcrun metal` at runtime via Pipelines::CreateComputePipeline's
// compile_metal_library path.
//
// Buffer indices follow the GLSL Group A/B slot numbering 1:1; the Metal
// path doesn't use descriptor sets so each binding is one MTLBuffer
// at the index below.
//   buffer 0: Params { instance_count, vertex_count, joint_count, pad }
//   buffer 1: Palettes (float4x4[])
//   buffer 2: InstanceMeta (uint2[]; .x = palette_off_mat4s, .y = output_off_vec4s)
//   buffer 3: OutputPool (float4[])
//   buffer 4: Positions slice (float4[])
//   buffer 5: SkinAttrs slice (uint4 joints + float4 weights, interleaved)

#include <metal_stdlib>
using namespace metal;

struct SkinParams {
    uint instance_count;
    uint vertex_count;
    uint joint_count;
    uint _pad0;
};

kernel void skin_compute(uint gid [[thread_position_in_grid]],
                          constant SkinParams& params [[buffer(0)]],
                          const device float4x4* palette [[buffer(1)]],
                          const device uint2* inst_meta [[buffer(2)]],
                          device float4* out_pos [[buffer(3)]],
                          const device float4* mesh_pos [[buffer(4)]],
                          const device uint4* skin_joints_then_weights [[buffer(5)]]) {
    uint per_instance = params.vertex_count;
    if (per_instance == 0u) {
        return;
    }
    uint inst = gid / per_instance;
    uint vid = gid - inst * per_instance;
    if (inst >= params.instance_count) {
        return;
    }
    uint2 meta = inst_meta[inst];
    uint palette_off = meta.x;
    uint output_off = meta.y;

    uint4 j = skin_joints_then_weights[2u * vid];
    uint4 wraw = skin_joints_then_weights[2u * vid + 1u];
    float4 w = float4(as_type<float>(wraw.x), as_type<float>(wraw.y),
                      as_type<float>(wraw.z), as_type<float>(wraw.w));
    float4x4 m = palette[palette_off + j.x] * w.x
               + palette[palette_off + j.y] * w.y
               + palette[palette_off + j.z] * w.z
               + palette[palette_off + j.w] * w.w;

    float4 p = mesh_pos[vid];
    out_pos[output_off + vid] = m * float4(p.xyz, 1.0);
}
