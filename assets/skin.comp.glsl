// #221 Skinning Phase 4: instanced compute pre-skin kernel.
// Source for assets/skin.comp.spv. Compile with:
//   glslc -fshader-stage=compute assets/skin.comp.glsl -o assets/skin.comp.spv
//
// Layout (matches Frames::Init skin_group_b_layout_ + skin_group_a_layout_
// in src/rhi/vulkan/frames.cpp):
//   set 0 (Group B, frame-global, rotated per-frame-in-flight)
//     binding 0: UBO_DYNAMIC Params (instance_count, vertex_count, ...)
//     binding 1: SSBO_DYNAMIC Palettes (mat4[]; per-instance joint matrices)
//     binding 2: SSBO_DYNAMIC InstanceMeta (uvec2 per instance: palette_off, output_off; both in mat4/vec4 element units)
//     binding 3: SSBO          OutputPool (vec4[]; whole skin_output_pool_)
//   set 1 (Group A, per-mesh, baked at load)
//     binding 0: SSBO  positions slice (vec4[]; mesh-local mesh vertex positions)
//     binding 1: SSBO  skin attrs slice (uvec4 joints + vec4 weights per vertex)
//
// Dispatch (post #221 P1): vkCmdDispatch(ceil(vertex_count/64),
// instance_count, 1). Each workgroup is one instance; gl_WorkGroupID.y
// selects the instance, gl_GlobalInvocationID.x selects the vid. The old
// 1D integer-divide mapping was retired by Phase 1's instanced batching.

#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform Params {
    uint instance_count;
    uint vertex_count;
    uint joint_count;
    uint mode;
} params;

const uint kSkinModeFull = 0u;
const uint kSkinModeOnePalette = 1u;
const uint kSkinModeNoSkinAttrs = 2u;
const uint kSkinModePassthrough = 3u;


layout(set = 0, binding = 1) readonly buffer Palettes {
    mat4 palette[];
};

layout(set = 0, binding = 2) readonly buffer InstanceMetaSSBO {
    // .x = palette_off_mat4s (element index into palette[])
    // .y = output_off_vec4s (element index into out_pos[])
    uvec2 inst_meta[];
};

layout(set = 0, binding = 3) writeonly buffer OutputPool {
    vec4 out_pos[];
};

layout(set = 1, binding = 0) readonly buffer Positions {
    vec4 mesh_pos[];
};

layout(set = 1, binding = 1) readonly buffer SkinAttrs {
    uvec4 skin_joints_then_weights[];
};

vec4 skin_weights_at(uint vid) {
    uvec4 raw = skin_joints_then_weights[2u * vid + 1u];
    return vec4(uintBitsToFloat(raw.x), uintBitsToFloat(raw.y),
                uintBitsToFloat(raw.z), uintBitsToFloat(raw.w));
}

uvec4 skin_joints_at(uint vid) {
    return skin_joints_then_weights[2u * vid];
}

void main() {
    uint inst = gl_WorkGroupID.y;
    uint vid = gl_GlobalInvocationID.x;
    if (vid >= params.vertex_count) {
        return;
    }
    uvec2 meta = inst_meta[inst];
    uint palette_off = meta.x;
    uint output_off = meta.y;

    vec4 p = mesh_pos[vid];
    vec4 out_p;

    if (params.mode == kSkinModePassthrough) {
        out_p = p;
    } else {
        uvec4 j;
        vec4 w;
        if (params.mode == kSkinModeNoSkinAttrs) {
            j = uvec4(0u, 0u, 0u, 0u);
            w = vec4(1.0, 0.0, 0.0, 0.0);
        } else {
            j = skin_joints_at(vid);
            w = skin_weights_at(vid);
        }
        mat4 m;
        if (params.mode == kSkinModeOnePalette) {
            m = palette[palette_off];
        } else {
            m = palette[palette_off + j.x] * w.x
              + palette[palette_off + j.y] * w.y
              + palette[palette_off + j.z] * w.z
              + palette[palette_off + j.w] * w.w;
        }
        out_p = m * vec4(p.xyz, 1.0);
    }
    out_pos[output_off + vid] = out_p;
}
