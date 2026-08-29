// WebGPU port of skin.comp.glsl / skin.metal. Entry cs_main, workgroup (64,1,1),
// dispatch (ceil(vertex_count/64), instance_count, 1).
//
// WebGPU's compute pipeline has a single descriptor set, so vk's group A (mesh
// positions + packed skin attrs) folds into group 0 at bindings 4/5. The
// per-mesh sub-buffer byte offsets metal passes via setBuffer are NOT all
// 256-aligned (palette especially: e.g. 12736 % 256 = 192), and webgpu requires
// 256-aligned buffer-binding offsets. So the dedicated mesh/palette/pool buffers
// are bound WHOLE at offset 0 and their element bases ride in binding 6 (a
// webgpu-only uniform); params + inst_meta stay in the kDynamic master at their
// 256-aligned offsets (baked into the bind group). All storage is read_write per
// the shared compute-set layout convention (read-only access is still read-only).

struct SkinParams {
  instance_count: u32, vertex_count: u32, joint_count: u32, mode: u32,
};

// WebGPU-only: element bases metal/vk carry as per-mesh setBuffer byte offsets.
struct WebBases {
  pos_base: u32, skin_base: u32, palette_base: u32, _pad: u32,
};

const kSkinModeFull: u32 = 0u;
const kSkinModeOnePalette: u32 = 1u;
const kSkinModeNoSkinAttrs: u32 = 2u;
const kSkinModePassthrough: u32 = 3u;

// inst_meta is read-only storage so the kDynamic master (also bound as the
// params + bases UNIFORMs) stays all-reads in the dispatch -- a read_write
// storage on it would be an exclusive-usage conflict. The other read-only
// storages are `read` for the same reason / correctness; only out_pos is written.
@group(0) @binding(0) var<uniform> params: SkinParams;
@group(0) @binding(1) var<storage, read> palette: array<mat4x4<f32>>;
@group(0) @binding(2) var<storage, read> inst_meta: array<vec2<u32>>;
@group(0) @binding(3) var<storage, read_write> out_pos: array<vec4<f32>>;
@group(0) @binding(4) var<storage, read> mesh_pos: array<vec4<f32>>;
@group(0) @binding(5) var<storage, read> skin_packed: array<vec2<u32>>;
@group(0) @binding(6) var<uniform> bases: WebBases;

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>,
           @builtin(workgroup_id) wid: vec3<u32>) {
  let inst: u32 = wid.y;
  let vid: u32 = gid.x;
  if (vid >= params.vertex_count) { return; }
  let im: vec2<u32> = inst_meta[inst];
  let palette_off: u32 = im.x;
  let output_off: u32 = im.y;

  let p: vec4<f32> = mesh_pos[bases.pos_base + vid];
  var out_p: vec4<f32>;

  if (params.mode == kSkinModePassthrough) {
    out_p = p;
  } else {
    var j: vec4<u32>;
    var w: vec4<f32>;
    if (params.mode == kSkinModeNoSkinAttrs) {
      j = vec4<u32>(0u, 0u, 0u, 0u);
      w = vec4<f32>(1.0, 0.0, 0.0, 0.0);
    } else {
      let jp: u32 = skin_packed[bases.skin_base + vid].x;
      let wp: u32 = skin_packed[bases.skin_base + vid].y;
      j = vec4<u32>(jp & 0xFFu, (jp >> 8u) & 0xFFu,
                    (jp >> 16u) & 0xFFu, (jp >> 24u) & 0xFFu);
      w = vec4<f32>(f32(wp & 0xFFu), f32((wp >> 8u) & 0xFFu),
                    f32((wp >> 16u) & 0xFFu), f32((wp >> 24u) & 0xFFu))
          * (1.0 / 255.0);
    }
    let pbase: u32 = bases.palette_base + palette_off;
    var m: mat4x4<f32>;
    if (params.mode == kSkinModeOnePalette) {
      m = palette[pbase];
    } else {
      m = palette[pbase + j.x] * w.x
        + palette[pbase + j.y] * w.y
        + palette[pbase + j.z] * w.z
        + palette[pbase + j.w] * w.w;
    }
    out_p = m * vec4<f32>(p.xyz, 1.0);
  }
  out_pos[output_off + vid] = out_p;
}
