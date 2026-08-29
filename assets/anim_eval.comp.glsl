// #221 Skinning Phase 5b: GPU palette evaluation kernel.
// Source for assets/anim_eval.comp.spv. Compile with:
//   glslc -fshader-stage=compute assets/anim_eval.comp.glsl -o assets/anim_eval.comp.spv
//
// One workgroup per visible skinned actor; 64 threads per workgroup.
// Dispatch: vkCmdDispatch(actor_count, 1, 1).
//
// Stages inside the workgroup:
//   1. Cooperative seed of shared TRS from bind_pose for this actor's scene.
//   2. Cooperative sample over the scene's walking-clip channels, overriding
//      shared TRS slots that the clip targets.
//   3. Thread 0 walks topo_order composing world = world[parent] * compose(trs),
//      writing into per-actor world_scratch slab.
//   4. Cooperative palette build: palette[j] = inv(mesh_world) * world[joint_node[j]] * inverse_bind[j],
//      writing into the persistent palette buffer.
//
// Caps (ratified over plan v7 baseline of 96/128, after Phase 0a `3692803`
// measured max_joints=254 / max_nodes=256 on the LoL champion set):
//   kMaxJointsPerSkin   = 256
//   kMaxNodesPerScene   = 256
//   kMaxClipChannels    = 512  (one channel per (node, path) at worst)
//
// Bindings (single descriptor set). #231 SSBO pack: 12 read-only tables folded
// into 3 packed buffers by element type; SceneHeader offsets are element
// offsets into the packed buffer.
//   0  : UBO  ActorRecord[]            -- per-frame, kDynamic
//   1  : SSBO int ae_i32[]             -- parent | topo | joint_nodes | times(bits)
//   2  : SSBO vec4 ae_vec4[]           -- bind_pose(3/joint) | values | invbind(4/j)
//   3  : SSBO uvec4 ae_word16[]        -- channels | samplers (each 16B = 1 uvec4)
//   4  : SSBO SceneHeader[]            -- per-scene, kDefault
//   5  : SSBO mat4 world_scratch[]     -- RW; 1024 actors * 256 nodes = 16 MB kDefault
//   6  : SSBO mat4 palette_out[]       -- W;  1024 actors * 256 joints = 16 MB kDefault
//
// AnimationPath enum (matches src/util/gltf_loader.hpp):
//   0 = kTranslation, 1 = kRotation, 2 = kScale, 3 = kWeights (skipped)
// AnimationInterpolation enum:
//   0 = kStep, 1 = kLinear, 2 = kCubicSpline (asserts disabled; treated as linear)

#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#define kMaxNodesPerScene 256u
#define kMaxJointsPerSkin 256u

#define kPathTranslation 0u
#define kPathRotation    1u
#define kPathScale       2u
#define kPathWeights     3u

#define kInterpStep   0u
#define kInterpLinear 1u

struct GpuTRS {
    vec4 T;  // .xyz = translation, .w pad
    vec4 R;  // quaternion (x, y, z, w)
    vec4 S;  // .xyz = scale,       .w pad
};

struct SceneHeader {
    uint node_count;
    uint joint_count;
    uint channel_count;     // for the scene's selected walking clip
    uint sampler_count;     // for the scene's selected walking clip

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
    uint world_scratch_base;   // in mat4 units
    uint palette_out_base;     // in mat4 units
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

layout(set = 0, binding = 0, std140) uniform ActorRecords {
    // UBO array stride per std140 = 16 B; ActorRecord is 16 B exactly.
    ActorRecord records[1024];
} ar;

// #231 packed read-only tables (element offsets stamped in SceneHeader).
layout(set = 0, binding = 1, std430) readonly buffer AeI32   { int  ae_i32[]; };
layout(set = 0, binding = 2, std430) readonly buffer AeVec4  { vec4 ae_vec4[]; };
layout(set = 0, binding = 3, std430) readonly buffer AeWord16 {
    uvec4 ae_word16[];
};
layout(set = 0, binding = 4, std430) readonly buffer SceneHeaders {
    SceneHeader headers[];
};
layout(set = 0, binding = 5, std430) buffer WorldScratch {
    mat4 world_scratch[];
};
layout(set = 0, binding = 6, std430) writeonly buffer PaletteOut {
    mat4 palette_out[];
};

// #231 times live in ae_i32 as float-bit ints; bitcast on read.
float ae_time(uint idx) { return intBitsToFloat(ae_i32[idx]); }

GpuChannel load_channel(uint idx) {
    uvec4 w = ae_word16[idx];
    GpuChannel ch;
    ch.node_idx = int(w.x);
    ch.path = w.y;
    ch.sampler_idx = int(w.z);
    ch._pad0 = w.w;
    return ch;
}

GpuSampler load_sampler(uint idx) {
    uvec4 w = ae_word16[idx];
    GpuSampler s;
    s.times_off = w.x;
    s.values_off = w.y;
    s.count = w.z;
    s.interp = w.w;
    return s;
}

shared GpuTRS s_trs[kMaxNodesPerScene];
shared mat4 s_mesh_world_inv;

// Binary search the upper key. Mirror of CPU SampleSampler.
uint upper_key(uint times_off, uint count, float t) {
    uint lo = 0u;
    uint hi = count - 1u;
    while (lo + 1u < hi) {
        uint mid = (lo + hi) >> 1u;
        if (ae_time(times_off + mid) <= t) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return hi;
}

vec4 sample_sampler(GpuSampler s, uint path, float t) {
    if (s.count == 0u) {
        return vec4(0.0);
    }
    if (t <= ae_time(s.times_off)) {
        return ae_vec4[s.values_off];
    }
    if (t >= ae_time(s.times_off + s.count - 1u)) {
        return ae_vec4[s.values_off + s.count - 1u];
    }
    uint hi = upper_key(s.times_off, s.count, t);
    uint lo = hi - 1u;
    if (s.interp == kInterpStep) {
        return ae_vec4[s.values_off + lo];
    }
    float t0 = ae_time(s.times_off + lo);
    float t1 = ae_time(s.times_off + hi);
    float u = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0;
    vec4 v0 = ae_vec4[s.values_off + lo];
    vec4 v1 = ae_vec4[s.values_off + hi];
    if (path == kPathRotation) {
        // slerp on quaternions
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

mat4 quat_to_mat4(vec4 q) {
    float xx = q.x * q.x;
    float yy = q.y * q.y;
    float zz = q.z * q.z;
    float xy = q.x * q.y;
    float xz = q.x * q.z;
    float yz = q.y * q.z;
    float wx = q.w * q.x;
    float wy = q.w * q.y;
    float wz = q.w * q.z;
    return mat4(
        vec4(1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz),       2.0 * (xz - wy),       0.0),
        vec4(2.0 * (xy - wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx),       0.0),
        vec4(2.0 * (xz + wy),       2.0 * (yz - wx),       1.0 - 2.0 * (xx + yy), 0.0),
        vec4(0.0,                   0.0,                   0.0,                   1.0));
}

mat4 compose_trs(GpuTRS trs) {
    mat4 r = quat_to_mat4(trs.R);
    mat4 m;
    m[0] = r[0] * trs.S.x;
    m[1] = r[1] * trs.S.y;
    m[2] = r[2] * trs.S.z;
    m[3] = vec4(trs.T.xyz, 1.0);
    return m;
}

void main() {
    uint actor = gl_WorkGroupID.x;
    uint lid = gl_LocalInvocationID.x;
    ActorRecord rec = ar.records[actor];
    SceneHeader sh = headers[rec.scene_idx];

    // Stage 1: cooperative seed of shared TRS from bind pose.
    for (uint i = lid; i < sh.node_count; i += 64u) {
        uint bo = sh.bind_pose_off + i * 3u;
        s_trs[i].T = ae_vec4[bo + 0u];
        s_trs[i].R = ae_vec4[bo + 1u];
        s_trs[i].S = ae_vec4[bo + 2u];
    }
    barrier();

    // Stage 2: sample clip channels, override shared TRS.
    float dur = (sh.duration > 0.0) ? sh.duration : 1.0;
    float wrapped = rec.time - dur * floor(rec.time / dur);
    for (uint c = lid; c < sh.channel_count; c += 64u) {
        GpuChannel ch = load_channel(sh.channel_off + c);
        if (ch.sampler_idx < 0 || ch.node_idx < 0) {
            continue;
        }
        if (uint(ch.node_idx) >= sh.node_count) {
            continue;
        }
        GpuSampler s = load_sampler(sh.sampler_off + uint(ch.sampler_idx));
        vec4 v = sample_sampler(s, ch.path, wrapped);
        if (ch.path == kPathTranslation) {
            s_trs[ch.node_idx].T = vec4(v.xyz, 0.0);
        } else if (ch.path == kPathRotation) {
            s_trs[ch.node_idx].R = v;
        } else if (ch.path == kPathScale) {
            s_trs[ch.node_idx].S = vec4(v.xyz, 0.0);
        }
        // kPathWeights: skipped.
    }
    barrier();

    // Stage 3: thread 0 walks topo composing worlds.
    if (lid == 0u) {
        for (uint i = 0u; i < sh.node_count; ++i) {
            int ni = ae_i32[sh.topo_off + i];
            if (ni < 0 || uint(ni) >= sh.node_count) {
                continue;
            }
            int pi = ae_i32[sh.parent_off + uint(ni)];
            mat4 local = compose_trs(s_trs[ni]);
            mat4 world;
            if (pi >= 0) {
                world = world_scratch[rec.world_scratch_base + uint(pi)] * local;
            } else {
                world = local;
            }
            world_scratch[rec.world_scratch_base + uint(ni)] = world;
        }
    }
    memoryBarrierBuffer();
    barrier();

    // Stage 4: cooperative palette build.
    if (sh.mesh_node < 0 || uint(sh.mesh_node) >= sh.node_count) {
        return;
    }
    // #222 Phase 0.4: hoist inverse(mesh_world) to thread 0 + LDS. Was
    // 64 redundant inverses per workgroup.
    if (lid == 0u) {
        mat4 mesh_world =
            world_scratch[rec.world_scratch_base + uint(sh.mesh_node)];
        s_mesh_world_inv = inverse(mesh_world);
    }
    barrier();
    for (uint j = lid; j < sh.joint_count; j += 64u) {
        int jn = ae_i32[sh.joint_nodes_off + j];
        if (jn < 0 || uint(jn) >= sh.node_count) {
            palette_out[rec.palette_out_base + j] = mat4(1.0);
            continue;
        }
        mat4 jw = world_scratch[rec.world_scratch_base + uint(jn)];
        uint ibo = sh.inverse_binds_off + j * 4u;
        mat4 ib = mat4(ae_vec4[ibo + 0u], ae_vec4[ibo + 1u],
                       ae_vec4[ibo + 2u], ae_vec4[ibo + 3u]);
        palette_out[rec.palette_out_base + j] = s_mesh_world_inv * jw * ib;
    }
}
