// #221 Skinning Phase 5b: Metal mirror of assets/anim_eval.comp.glsl.
// Compiled at runtime via Pipelines::CreateComputePipeline's
// compile_metal_library path. Buffer indices follow the GLSL binding numbers
// 1:1 (Metal has no descriptor sets; each binding is one MTLBuffer).
//
// #231 SSBO pack: 12 read-only tables folded into 3 packed buffers by element
// type. Offsets in SceneHeader are now element offsets into the packed buffer.
//   buffer 0  : ActorRecord[]   (per-frame, kDynamic)
//   buffer 1  : int   ae_i32[]    parent | topo | joint_nodes | times(bits)
//   buffer 2  : float4 ae_vec4[]  bind_pose(3/joint T,R,S) | values | invbind(4/j)
//   buffer 3  : uint4 ae_word16[] channels | samplers (each 16B = 1 uint4)
//   buffer 4  : SceneHeader[]
//   buffer 5  : float4x4 world_scratch[]  (RW)
//   buffer 6  : float4x4 palette_out[]    (W)
//
// Dispatch: dispatchThreadgroups(MTLSize(actor_count, 1, 1),
//                                threadsPerThreadgroup: MTLSize(64, 1, 1))

#include <metal_stdlib>
using namespace metal;

constant uint kPathTranslation = 0u;
constant uint kPathRotation    = 1u;
constant uint kPathScale       = 2u;

constant uint kInterpStep = 0u;

struct GpuTRS {
    float4 T;
    float4 R;
    float4 S;
};

struct SceneHeader {
    uint node_count;
    uint joint_count;
    uint channel_count;
    uint sampler_count;

    uint parent_off;
    uint topo_off;
    uint bind_pose_off;
    uint channel_off;

    uint sampler_off;
    uint times_off;
    uint values_off;
    uint joint_nodes_off;

    uint inverse_binds_off;
    int  mesh_node;
    float duration;
    float _pad0;
};

struct ActorRecord {
    uint scene_idx;
    uint world_scratch_base;
    uint palette_out_base;
    float time;
};

struct GpuChannel {
    int  node_idx;
    uint path;
    int  sampler_idx;
    uint _pad0;
};

struct GpuSampler {
    uint times_off;
    uint values_off;
    uint count;
    uint interp;
};

// #231 times live in ae_i32 as float-bit ints; bitcast on read.
static float ae_time(const device int* i32buf, uint idx) {
    return as_type<float>(i32buf[idx]);
}

static uint upper_key(const device int* i32buf, uint times_off, uint count, float t) {
    uint lo = 0u;
    uint hi = count - 1u;
    while (lo + 1u < hi) {
        uint mid = (lo + hi) >> 1u;
        if (ae_time(i32buf, times_off + mid) <= t) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return hi;
}

static float4 sample_sampler(GpuSampler s,
                             uint path,
                             float t,
                             const device int* i32buf,
                             const device float4* values) {
    if (s.count == 0u) {
        return float4(0.0);
    }
    if (t <= ae_time(i32buf, s.times_off)) {
        return values[s.values_off];
    }
    if (t >= ae_time(i32buf, s.times_off + s.count - 1u)) {
        return values[s.values_off + s.count - 1u];
    }
    uint hi = upper_key(i32buf, s.times_off, s.count, t);
    uint lo = hi - 1u;
    if (s.interp == kInterpStep) {
        return values[s.values_off + lo];
    }
    float t0 = ae_time(i32buf, s.times_off + lo);
    float t1 = ae_time(i32buf, s.times_off + hi);
    float u = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0;
    float4 v0 = values[s.values_off + lo];
    float4 v1 = values[s.values_off + hi];
    if (path == kPathRotation) {
        float cosTheta = dot(v0, v1);
        if (cosTheta < 0.0) {
            v1 = -v1;
            cosTheta = -cosTheta;
        }
        if (cosTheta > 0.9995) {
            return normalize(mix(v0, v1, u));
        }
        float theta0 = acos(cosTheta);
        float theta = theta0 * u;
        float sinTheta0 = sin(theta0);
        float s0 = sin(theta0 - theta) / sinTheta0;
        float s1 = sin(theta) / sinTheta0;
        return s0 * v0 + s1 * v1;
    }
    return mix(v0, v1, u);
}

static float4x4 quat_to_mat4(float4 q) {
    float xx = q.x * q.x;
    float yy = q.y * q.y;
    float zz = q.z * q.z;
    float xy = q.x * q.y;
    float xz = q.x * q.z;
    float yz = q.y * q.z;
    float wx = q.w * q.x;
    float wy = q.w * q.y;
    float wz = q.w * q.z;
    return float4x4(
        float4(1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz),       2.0 * (xz - wy),       0.0),
        float4(2.0 * (xy - wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx),       0.0),
        float4(2.0 * (xz + wy),       2.0 * (yz - wx),       1.0 - 2.0 * (xx + yy), 0.0),
        float4(0.0,                   0.0,                   0.0,                   1.0));
}

static float4x4 compose_trs(GpuTRS trs) {
    float4x4 r = quat_to_mat4(trs.R);
    float4x4 m;
    m[0] = r[0] * trs.S.x;
    m[1] = r[1] * trs.S.y;
    m[2] = r[2] * trs.S.z;
    m[3] = float4(trs.T.xyz, 1.0);
    return m;
}

// #231 reconstruct a packed GpuChannel / GpuSampler from one uint4.
static GpuChannel load_channel(const device uint4* word16, uint idx) {
    uint4 w = word16[idx];
    GpuChannel ch;
    ch.node_idx = int(w.x);
    ch.path = w.y;
    ch.sampler_idx = int(w.z);
    ch._pad0 = w.w;
    return ch;
}

static GpuSampler load_sampler(const device uint4* word16, uint idx) {
    uint4 w = word16[idx];
    GpuSampler s;
    s.times_off = w.x;
    s.values_off = w.y;
    s.count = w.z;
    s.interp = w.w;
    return s;
}

kernel void anim_eval(uint3 gid [[thread_position_in_grid]],
                      uint3 wid [[threadgroup_position_in_grid]],
                      uint3 lid [[thread_position_in_threadgroup]],
                      const device ActorRecord* records  [[buffer(0)]],
                      const device int* ae_i32           [[buffer(1)]],
                      const device float4* ae_vec4       [[buffer(2)]],
                      const device uint4* ae_word16      [[buffer(3)]],
                      const device SceneHeader* headers  [[buffer(4)]],
                      device float4x4* world_scratch     [[buffer(5)]],
                      device float4x4* palette_out       [[buffer(6)]],
                      threadgroup GpuTRS* s_trs          [[threadgroup(0)]]) {
    uint actor = wid.x;
    uint tid = lid.x;
    ActorRecord rec = records[actor];
    SceneHeader sh = headers[rec.scene_idx];

    for (uint i = tid; i < sh.node_count; i += 64u) {
        uint bo = sh.bind_pose_off + i * 3u;
        GpuTRS trs;
        trs.T = ae_vec4[bo + 0u];
        trs.R = ae_vec4[bo + 1u];
        trs.S = ae_vec4[bo + 2u];
        s_trs[i] = trs;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    float dur = (sh.duration > 0.0) ? sh.duration : 1.0;
    float wrapped = rec.time - dur * floor(rec.time / dur);
    for (uint c = tid; c < sh.channel_count; c += 64u) {
        GpuChannel ch = load_channel(ae_word16, sh.channel_off + c);
        if (ch.sampler_idx < 0 || ch.node_idx < 0) {
            continue;
        }
        if (uint(ch.node_idx) >= sh.node_count) {
            continue;
        }
        GpuSampler s =
            load_sampler(ae_word16, sh.sampler_off + uint(ch.sampler_idx));
        float4 v = sample_sampler(s, ch.path, wrapped, ae_i32, ae_vec4);
        if (ch.path == kPathTranslation) {
            s_trs[ch.node_idx].T = float4(v.xyz, 0.0);
        } else if (ch.path == kPathRotation) {
            s_trs[ch.node_idx].R = v;
        } else if (ch.path == kPathScale) {
            s_trs[ch.node_idx].S = float4(v.xyz, 0.0);
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (tid == 0u) {
        for (uint i = 0u; i < sh.node_count; ++i) {
            int ni = ae_i32[sh.topo_off + i];
            if (ni < 0 || uint(ni) >= sh.node_count) {
                continue;
            }
            int pi = ae_i32[sh.parent_off + uint(ni)];
            float4x4 local = compose_trs(s_trs[ni]);
            float4x4 world;
            if (pi >= 0) {
                world = world_scratch[rec.world_scratch_base + uint(pi)] * local;
            } else {
                world = local;
            }
            world_scratch[rec.world_scratch_base + uint(ni)] = world;
        }
    }
    threadgroup_barrier(mem_flags::mem_device);

    if (sh.mesh_node < 0 || uint(sh.mesh_node) >= sh.node_count) {
        return;
    }
    // #222 Phase 0.4: hoist 4x4 cofactor-inverse of mesh_world to thread 0
    // + threadgroup memory. Was 64 redundant inverses per workgroup. MSL
    // function-scope threadgroup vars can't default-construct float4x4 -
    // store 4 columns separately and rebuild on read.
    threadgroup float4 s_mw_inv_c0;
    threadgroup float4 s_mw_inv_c1;
    threadgroup float4 s_mw_inv_c2;
    threadgroup float4 s_mw_inv_c3;
    if (tid == 0u) {
        float4x4 mw =
            world_scratch[rec.world_scratch_base + uint(sh.mesh_node)];
        float a00 = mw[0].x, a01 = mw[1].x, a02 = mw[2].x, a03 = mw[3].x;
        float a10 = mw[0].y, a11 = mw[1].y, a12 = mw[2].y, a13 = mw[3].y;
        float a20 = mw[0].z, a21 = mw[1].z, a22 = mw[2].z, a23 = mw[3].z;
        float a30 = mw[0].w, a31 = mw[1].w, a32 = mw[2].w, a33 = mw[3].w;
        float b00 = a00*a11 - a01*a10;
        float b01 = a00*a12 - a02*a10;
        float b02 = a00*a13 - a03*a10;
        float b03 = a01*a12 - a02*a11;
        float b04 = a01*a13 - a03*a11;
        float b05 = a02*a13 - a03*a12;
        float b06 = a20*a31 - a21*a30;
        float b07 = a20*a32 - a22*a30;
        float b08 = a20*a33 - a23*a30;
        float b09 = a21*a32 - a22*a31;
        float b10 = a21*a33 - a23*a31;
        float b11 = a22*a33 - a23*a32;
        float det = b00*b11 - b01*b10 + b02*b09 + b03*b08 - b04*b07 + b05*b06;
        float inv_det = (det != 0.0) ? 1.0 / det : 0.0;
        s_mw_inv_c0 =
            float4( a11*b11 - a12*b10 + a13*b09,
                    -a10*b11 + a12*b08 - a13*b07,
                     a10*b10 - a11*b08 + a13*b06,
                    -a10*b09 + a11*b07 - a12*b06) * inv_det;
        s_mw_inv_c1 =
            float4(-a01*b11 + a02*b10 - a03*b09,
                     a00*b11 - a02*b08 + a03*b07,
                    -a00*b10 + a01*b08 - a03*b06,
                     a00*b09 - a01*b07 + a02*b06) * inv_det;
        s_mw_inv_c2 =
            float4( a31*b05 - a32*b04 + a33*b03,
                    -a30*b05 + a32*b02 - a33*b01,
                     a30*b04 - a31*b02 + a33*b00,
                    -a30*b03 + a31*b01 - a32*b00) * inv_det;
        s_mw_inv_c3 =
            float4(-a21*b05 + a22*b04 - a23*b03,
                     a20*b05 - a22*b02 + a23*b01,
                    -a20*b04 + a21*b02 - a23*b00,
                     a20*b03 - a21*b01 + a22*b00) * inv_det;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const float4x4 mesh_world_inv = float4x4(s_mw_inv_c0, s_mw_inv_c1,
                                              s_mw_inv_c2, s_mw_inv_c3);

    for (uint j = tid; j < sh.joint_count; j += 64u) {
        int jn = ae_i32[sh.joint_nodes_off + j];
        if (jn < 0 || uint(jn) >= sh.node_count) {
            palette_out[rec.palette_out_base + j] = float4x4(1.0);
            continue;
        }
        float4x4 jw = world_scratch[rec.world_scratch_base + uint(jn)];
        uint ibo = sh.inverse_binds_off + j * 4u;
        float4x4 ib = float4x4(ae_vec4[ibo + 0u], ae_vec4[ibo + 1u],
                               ae_vec4[ibo + 2u], ae_vec4[ibo + 3u]);
        palette_out[rec.palette_out_base + j] = mesh_world_inv * jw * ib;
    }
}
