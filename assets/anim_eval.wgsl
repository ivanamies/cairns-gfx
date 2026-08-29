// WebGPU port of assets/anim_eval.comp.glsl / assets/anim_eval.metal. One
// workgroup per visible skinned actor; 64 threads; dispatch (actor_count,1,1).
// Entry cs_main (the pipeline hardcodes it). Every storage binding is
// read_write because MakeComputeSetLayout declares every kStorage as Storage
// (RW) -- read-only access through them is still read-only at runtime, matching
// vk/metal. The 4x4 inverse mirrors metal's explicit cofactor form (NOT WGSL
// inverse()) to keep FP-bit parity with the metal backend's deterministic hash.

const kPathTranslation: u32 = 0u;
const kPathRotation: u32 = 1u;
const kPathScale: u32 = 2u;
const kInterpStep: u32 = 0u;

struct GpuTRS { T: vec4<f32>, R: vec4<f32>, S: vec4<f32>, };

struct SceneHeader {
  node_count: u32, joint_count: u32, channel_count: u32, sampler_count: u32,
  parent_off: u32, topo_off: u32, bind_pose_off: u32, channel_off: u32,
  sampler_off: u32, times_off: u32, values_off: u32, joint_nodes_off: u32,
  inverse_binds_off: u32, mesh_node: i32, duration: f32, _pad0: f32,
};

struct ActorRecord {
  scene_idx: u32, world_scratch_base: u32, palette_out_base: u32, time: f32,
};

struct GpuChannel { node_idx: i32, path: u32, sampler_idx: i32, _pad0: u32, };
struct GpuSampler { times_off: u32, values_off: u32, count: u32, interp: u32, };

@group(0) @binding(0)  var<uniform> records: array<ActorRecord, 1024>;
@group(0) @binding(1)  var<storage, read_write> headers: array<SceneHeader>;
@group(0) @binding(2)  var<storage, read_write> parent: array<i32>;
@group(0) @binding(3)  var<storage, read_write> topo: array<i32>;
@group(0) @binding(4)  var<storage, read_write> bind_pose: array<GpuTRS>;
@group(0) @binding(5)  var<storage, read_write> channels: array<GpuChannel>;
@group(0) @binding(6)  var<storage, read_write> samplers: array<GpuSampler>;
@group(0) @binding(7)  var<storage, read_write> times: array<f32>;
@group(0) @binding(8)  var<storage, read_write> values: array<vec4<f32>>;
@group(0) @binding(9)  var<storage, read_write> joint_nodes: array<i32>;
@group(0) @binding(10) var<storage, read_write> inverse_binds: array<mat4x4<f32>>;
@group(0) @binding(11) var<storage, read_write> world_scratch: array<mat4x4<f32>>;
@group(0) @binding(12) var<storage, read_write> palette_out: array<mat4x4<f32>>;

var<workgroup> s_trs: array<GpuTRS, 256>;
var<workgroup> s_mw_inv_c0: vec4<f32>;
var<workgroup> s_mw_inv_c1: vec4<f32>;
var<workgroup> s_mw_inv_c2: vec4<f32>;
var<workgroup> s_mw_inv_c3: vec4<f32>;

fn upper_key(times_off: u32, count: u32, t: f32) -> u32 {
  var lo: u32 = 0u;
  var hi: u32 = count - 1u;
  loop {
    if (!(lo + 1u < hi)) { break; }
    let mid: u32 = (lo + hi) >> 1u;
    if (times[times_off + mid] <= t) { lo = mid; } else { hi = mid; }
  }
  return hi;
}

fn sample_sampler(s: GpuSampler, path: u32, t: f32) -> vec4<f32> {
  if (s.count == 0u) { return vec4<f32>(0.0); }
  if (t <= times[s.times_off]) { return values[s.values_off]; }
  if (t >= times[s.times_off + s.count - 1u]) {
    return values[s.values_off + s.count - 1u];
  }
  let hi: u32 = upper_key(s.times_off, s.count, t);
  let lo: u32 = hi - 1u;
  if (s.interp == kInterpStep) { return values[s.values_off + lo]; }
  let t0: f32 = times[s.times_off + lo];
  let t1: f32 = times[s.times_off + hi];
  var u: f32 = 0.0;
  if (t1 > t0) { u = (t - t0) / (t1 - t0); }
  var v0: vec4<f32> = values[s.values_off + lo];
  var v1: vec4<f32> = values[s.values_off + hi];
  if (path == kPathRotation) {
    var cosTheta: f32 = dot(v0, v1);
    if (cosTheta < 0.0) { v1 = -v1; cosTheta = -cosTheta; }
    if (cosTheta > 0.9995) { return normalize(mix(v0, v1, vec4<f32>(u))); }
    let theta0: f32 = acos(cosTheta);
    let theta: f32 = theta0 * u;
    let sinTheta0: f32 = sin(theta0);
    let s0: f32 = sin(theta0 - theta) / sinTheta0;
    let s1: f32 = sin(theta) / sinTheta0;
    return s0 * v0 + s1 * v1;
  }
  return mix(v0, v1, vec4<f32>(u));
}

fn quat_to_mat4(q: vec4<f32>) -> mat4x4<f32> {
  let xx = q.x * q.x; let yy = q.y * q.y; let zz = q.z * q.z;
  let xy = q.x * q.y; let xz = q.x * q.z; let yz = q.y * q.z;
  let wx = q.w * q.x; let wy = q.w * q.y; let wz = q.w * q.z;
  return mat4x4<f32>(
    vec4<f32>(1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz),       2.0 * (xz - wy),       0.0),
    vec4<f32>(2.0 * (xy - wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx),       0.0),
    vec4<f32>(2.0 * (xz + wy),       2.0 * (yz - wx),       1.0 - 2.0 * (xx + yy), 0.0),
    vec4<f32>(0.0,                   0.0,                   0.0,                   1.0));
}

fn compose_trs(trs: GpuTRS) -> mat4x4<f32> {
  let r: mat4x4<f32> = quat_to_mat4(trs.R);
  return mat4x4<f32>(
    r[0] * trs.S.x,
    r[1] * trs.S.y,
    r[2] * trs.S.z,
    vec4<f32>(trs.T.xyz, 1.0));
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(workgroup_id) wid: vec3<u32>,
           @builtin(local_invocation_id) lid: vec3<u32>) {
  let actor: u32 = wid.x;
  let tid: u32 = lid.x;
  let rec: ActorRecord = records[actor];
  let sh: SceneHeader = headers[rec.scene_idx];

  // Stage 1: seed shared TRS from bind pose.
  var i: u32 = tid;
  loop {
    if (!(i < sh.node_count)) { break; }
    s_trs[i] = bind_pose[sh.bind_pose_off + i];
    i = i + 64u;
  }
  workgroupBarrier();

  // Stage 2: sample clip channels, override shared TRS.
  var dur: f32 = 1.0;
  if (sh.duration > 0.0) { dur = sh.duration; }
  let wrapped: f32 = rec.time - dur * floor(rec.time / dur);
  var c: u32 = tid;
  loop {
    if (!(c < sh.channel_count)) { break; }
    let ch: GpuChannel = channels[sh.channel_off + c];
    if (ch.sampler_idx >= 0 && ch.node_idx >= 0 &&
        u32(ch.node_idx) < sh.node_count) {
      let s: GpuSampler = samplers[sh.sampler_off + u32(ch.sampler_idx)];
      let v: vec4<f32> = sample_sampler(s, ch.path, wrapped);
      if (ch.path == kPathTranslation) {
        s_trs[u32(ch.node_idx)].T = vec4<f32>(v.xyz, 0.0);
      } else if (ch.path == kPathRotation) {
        s_trs[u32(ch.node_idx)].R = v;
      } else if (ch.path == kPathScale) {
        s_trs[u32(ch.node_idx)].S = vec4<f32>(v.xyz, 0.0);
      }
    }
    c = c + 64u;
  }
  workgroupBarrier();

  // Stage 3: thread 0 walks topo composing worlds.
  if (tid == 0u) {
    var k: u32 = 0u;
    loop {
      if (!(k < sh.node_count)) { break; }
      let ni: i32 = topo[sh.topo_off + k];
      if (ni >= 0 && u32(ni) < sh.node_count) {
        let pi: i32 = parent[sh.parent_off + u32(ni)];
        let local: mat4x4<f32> = compose_trs(s_trs[u32(ni)]);
        var world: mat4x4<f32>;
        if (pi >= 0) {
          world = world_scratch[rec.world_scratch_base + u32(pi)] * local;
        } else {
          world = local;
        }
        world_scratch[rec.world_scratch_base + u32(ni)] = world;
      }
      k = k + 1u;
    }
  }
  storageBarrier();
  workgroupBarrier();

  // Stage 4: palette build.
  if (sh.mesh_node < 0 || u32(sh.mesh_node) >= sh.node_count) { return; }
  if (tid == 0u) {
    let mw: mat4x4<f32> =
        world_scratch[rec.world_scratch_base + u32(sh.mesh_node)];
    let a00 = mw[0].x; let a01 = mw[1].x; let a02 = mw[2].x; let a03 = mw[3].x;
    let a10 = mw[0].y; let a11 = mw[1].y; let a12 = mw[2].y; let a13 = mw[3].y;
    let a20 = mw[0].z; let a21 = mw[1].z; let a22 = mw[2].z; let a23 = mw[3].z;
    let a30 = mw[0].w; let a31 = mw[1].w; let a32 = mw[2].w; let a33 = mw[3].w;
    let b00 = a00*a11 - a01*a10; let b01 = a00*a12 - a02*a10;
    let b02 = a00*a13 - a03*a10; let b03 = a01*a12 - a02*a11;
    let b04 = a01*a13 - a03*a11; let b05 = a02*a13 - a03*a12;
    let b06 = a20*a31 - a21*a30; let b07 = a20*a32 - a22*a30;
    let b08 = a20*a33 - a23*a30; let b09 = a21*a32 - a22*a31;
    let b10 = a21*a33 - a23*a31; let b11 = a22*a33 - a23*a32;
    let det = b00*b11 - b01*b10 + b02*b09 + b03*b08 - b04*b07 + b05*b06;
    var inv_det: f32 = 0.0;
    if (det != 0.0) { inv_det = 1.0 / det; }
    s_mw_inv_c0 = vec4<f32>( a11*b11 - a12*b10 + a13*b09,
                            -a10*b11 + a12*b08 - a13*b07,
                             a10*b10 - a11*b08 + a13*b06,
                            -a10*b09 + a11*b07 - a12*b06) * inv_det;
    s_mw_inv_c1 = vec4<f32>(-a01*b11 + a02*b10 - a03*b09,
                             a00*b11 - a02*b08 + a03*b07,
                            -a00*b10 + a01*b08 - a03*b06,
                             a00*b09 - a01*b07 + a02*b06) * inv_det;
    s_mw_inv_c2 = vec4<f32>( a31*b05 - a32*b04 + a33*b03,
                            -a30*b05 + a32*b02 - a33*b01,
                             a30*b04 - a31*b02 + a33*b00,
                            -a30*b03 + a31*b01 - a32*b00) * inv_det;
    s_mw_inv_c3 = vec4<f32>(-a21*b05 + a22*b04 - a23*b03,
                             a20*b05 - a22*b02 + a23*b01,
                            -a20*b04 + a21*b02 - a23*b00,
                             a20*b03 - a21*b01 + a22*b00) * inv_det;
  }
  workgroupBarrier();
  let mesh_world_inv: mat4x4<f32> =
      mat4x4<f32>(s_mw_inv_c0, s_mw_inv_c1, s_mw_inv_c2, s_mw_inv_c3);

  var j: u32 = tid;
  loop {
    if (!(j < sh.joint_count)) { break; }
    let jn: i32 = joint_nodes[sh.joint_nodes_off + j];
    if (jn < 0 || u32(jn) >= sh.node_count) {
      palette_out[rec.palette_out_base + j] = mat4x4<f32>(
        vec4<f32>(1.0,0.0,0.0,0.0), vec4<f32>(0.0,1.0,0.0,0.0),
        vec4<f32>(0.0,0.0,1.0,0.0), vec4<f32>(0.0,0.0,0.0,1.0));
    } else {
      let jw: mat4x4<f32> = world_scratch[rec.world_scratch_base + u32(jn)];
      let ib: mat4x4<f32> = inverse_binds[sh.inverse_binds_off + j];
      palette_out[rec.palette_out_base + j] = mesh_world_inv * jw * ib;
    }
    j = j + 64u;
  }
}
