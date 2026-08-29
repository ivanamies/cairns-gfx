// Simple vertex and fragment shaders.
#include <metal_stdlib>
using namespace metal;

#define CUBE_MAX_SCENE_REGISTRY_TEXTURES 1024
#define CUBE_MAX_SCENE_REGISTRY_MESHES 1024
#define CUBE_MAX_SCENE_REGISTRY_SAMPLERS 128
#define CUBE_TEXTURES_START_OFFSET 0
#define CUBE_VERTEX_ATTR_START_OFFSET (CUBE_TEXTURES_START_OFFSET + CUBE_MAX_SCENE_REGISTRY_TEXTURES)
#define CUBE_VERTEX_SAMPLERS_START_OFFSET (CUBE_VERTEX_ATTR_START_OFFSET + CUBE_MAX_SCENE_REGISTRY_MESHES)

#define CUBE_GLOBALS_BUFFER_SLOT 1
#define CUBE_MATERIAL_BUFFER_SLOT 2
#define CUBE_SHADER_SPECIFIC_BUFFER_SLOT 3
#define CUBE_DRAW_TMP_BUFFER_SLOT 4
#define CUBE_SCENE_REGISTRY_BUFFER_SLOT 5

namespace cube {

struct VertexAttribute {
    float4 color;
    float4 tangent;
    float4 normal;
    float2 uv;
    float2 pad1;
};
    
struct SceneRegistry {
    texture2d<float> textures [[id(CUBE_TEXTURES_START_OFFSET)]] [CUBE_MAX_SCENE_REGISTRY_TEXTURES];
    device VertexAttribute* vertex_attrs [[id(CUBE_VERTEX_ATTR_START_OFFSET)]] [CUBE_MAX_SCENE_REGISTRY_MESHES];
    sampler samplers [[id(CUBE_VERTEX_SAMPLERS_START_OFFSET)]] [CUBE_MAX_SCENE_REGISTRY_SAMPLERS];
};
    
struct VertexInput {
    float4 pos [[attribute(0)]];
    float2 uv [[attribute(1)]];   // stream 1: VertexAttribute.uv (offset 48, stride 64)
};

struct VertexOut {
    // The [[position]] attribute of this member indicates that this value
    // is the clip space position of the vertex when this structure is
    // returned from the vertex function.
    float4 position [[position]];
    
    // Since this member does not have a special attribute, the rasterizer
    // interpolates its value with the values of the other triangle vertices
    // and then passes the interpolated value to the fragment shader for each
    // fragment in the triangle.
    float2 textureCoordinate;
    
    uint tex_color_id [[flat]];
    uint sampler_id [[flat]];
};
    
struct RenderPassGlobals {
    float4x4 view_proj;
    // the below just aren't used, I have them here so no one complains I have an un-representative workload
    float4x4 inv_view_proj; // get world-pos from depth for like raycasting
    float4 camera_pos; // [x, y, z, exposure];
    float4 camera_dir; // [x, y, z, near plane];
    float4 screen_params; // [width, height, 1/width, 1/height];
};

struct MaterialGpu {
    uint32_t tex_color_id;
    uint32_t wip1;
    uint32_t wip2;
    uint32_t wip3;
    uint32_t wip4;
    uint32_t sampler_id;
};
    
struct DrawTmp {
    float4x4 model_matrix;
    uint mesh_id;
    uint tex_id;
    uint sampler_id;
    uint yolo_padding;
};

// set 2: per-material argument buffer (texture @ id 0, sampler @ id 1),
// bound at CUBE_MATERIAL_BUFFER_SLOT (fragment) on material change.
struct MaterialArg {
    texture2d<float> tex [[id(0)]];
    sampler samp [[id(1)]];
};

vertex VertexOut vertexShader(VertexInput in [[stage_in]],
                              constant RenderPassGlobals& globals [[buffer(CUBE_GLOBALS_BUFFER_SLOT)]],
                              constant MaterialGpu& material [[buffer(CUBE_MATERIAL_BUFFER_SLOT)]],
                              constant DrawTmp& draw_tmp [[buffer(CUBE_DRAW_TMP_BUFFER_SLOT)]]) {
    VertexOut out;
    out.position = globals.view_proj * draw_tmp.model_matrix * in.pos;
    out.textureCoordinate = in.uv;
    out.tex_color_id = material.tex_color_id;
    out.sampler_id = material.sampler_id;
    return out;
}

// #206 MRT: color + R32U id buffer. id is stubbed to 0 until per-draw
// {type<<24 | id} encoding lands via instance buffer.
struct FragmentOut {
    float4 color [[color(0)]];
    uint id [[color(1)]];
};

fragment FragmentOut fragmentShader(VertexOut in [[stage_in]], constant MaterialArg& material [[buffer(CUBE_MATERIAL_BUFFER_SLOT)]]) {
    FragmentOut out;
    out.color = material.tex.sample(material.samp, in.textureCoordinate);
    out.id = 0u;
    return out;
}
    
} // namespace cube
