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
    uint mode;
};

constant uint kSkinModeFull = 0u;
constant uint kSkinModeOnePalette = 1u;
constant uint kSkinModeNoSkinAttrs = 2u;
constant uint kSkinModePassthrough = 3u;

// #222 Phase S.1 Metal mirror: cooperative LDS palette. Same uniformity
// invariant: cooperative load + barrier before the vid early-out. Metal's
// function-scope threadgroup-class float4x4 doesn't default-construct;
// store 4 rows per joint and rebuild on read.
// #222 Phase S.2 (metal mirror): packed 8 B/vert.
kernel void skin_compute(uint3 gid [[thread_position_in_grid]],
                          uint3 wid [[threadgroup_position_in_grid]],
                          uint3 lid [[thread_position_in_threadgroup]],
                          constant SkinParams& params [[buffer(0)]],
                          const device float4x4* palette [[buffer(1)]],
                          const device uint2* inst_meta [[buffer(2)]],
                          device float4* out_pos [[buffer(3)]],
                          const device float4* mesh_pos [[buffer(4)]],
                          const device uint2* skin_packed [[buffer(5)]]) {
    threadgroup float4 s_pal[256 * 4];  // 4 rows per mat4
    uint inst = wid.y;
    uint vid = gid.x;
    uint tid = lid.x;
    uint2 meta = inst_meta[inst];
    uint palette_off = meta.x;
    uint output_off = meta.y;

    for (uint j = tid; j < params.joint_count; j += 64u) {
        float4x4 mm = palette[palette_off + j];
        s_pal[j * 4 + 0] = mm[0];
        s_pal[j * 4 + 1] = mm[1];
        s_pal[j * 4 + 2] = mm[2];
        s_pal[j * 4 + 3] = mm[3];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (vid >= params.vertex_count) {
        return;
    }

    float4 p = mesh_pos[vid];
    float4 out_p;

    if (params.mode == kSkinModePassthrough) {
        out_p = p;
    } else {
        uint4 j;
        float4 w;
        if (params.mode == kSkinModeNoSkinAttrs) {
            j = uint4(0u, 0u, 0u, 0u);
            w = float4(1.0, 0.0, 0.0, 0.0);
        } else {
            uint jp = skin_packed[vid].x;
            uint wp = skin_packed[vid].y;
            j = uint4(jp & 0xFFu, (jp >> 8) & 0xFFu,
                      (jp >> 16) & 0xFFu, (jp >> 24) & 0xFFu);
            w = float4(float(wp & 0xFFu), float((wp >> 8) & 0xFFu),
                       float((wp >> 16) & 0xFFu),
                       float((wp >> 24) & 0xFFu)) * (1.0f / 255.0f);
        }
        float4x4 mx = float4x4(s_pal[j.x*4+0], s_pal[j.x*4+1],
                               s_pal[j.x*4+2], s_pal[j.x*4+3]);
        float4x4 my = float4x4(s_pal[j.y*4+0], s_pal[j.y*4+1],
                               s_pal[j.y*4+2], s_pal[j.y*4+3]);
        float4x4 mz = float4x4(s_pal[j.z*4+0], s_pal[j.z*4+1],
                               s_pal[j.z*4+2], s_pal[j.z*4+3]);
        float4x4 mw = float4x4(s_pal[j.w*4+0], s_pal[j.w*4+1],
                               s_pal[j.w*4+2], s_pal[j.w*4+3]);
        float4x4 m;
        if (params.mode == kSkinModeOnePalette) {
            m = float4x4(s_pal[0], s_pal[1], s_pal[2], s_pal[3]);
        } else {
            m = mx * w.x + my * w.y + mz * w.z + mw * w.w;
        }
        out_p = m * float4(p.xyz, 1.0);
    }
    out_pos[output_off + vid] = out_p;
}
