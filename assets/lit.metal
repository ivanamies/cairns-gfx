// Lit forward shaders: unlit.metal + world normal + half-lambert directional
// shading. Light params ride flat varyings from the vertex stage so the
// fragment needs no extra buffer binding.
#include <metal_stdlib>
using namespace metal;

#define LIT_GLOBALS_BUFFER_SLOT 1
#define LIT_MATERIAL_BUFFER_SLOT 2
#define LIT_DRAW_TMP_BUFFER_SLOT 4

namespace lit {

struct VertexInput {
    float4 pos [[attribute(0)]];
    float2 uv [[attribute(1)]];      // stream 1: offset 48, stride 64
    float4 normal [[attribute(2)]];  // stream 1: offset 32, stride 64
};

struct VertexOut {
    float4 position [[position]];
    float2 textureCoordinate;
    float3 worldNormal;
    float4 lightDir [[flat]];
    float4 lightColor [[flat]];
    float4 ambient [[flat]];
    uint entity_id [[flat]];
};

struct RenderPassGlobals {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4 camera_pos;
    float4 camera_dir;
    float4 screen_params;
    float4x4 light_view_proj;
    float4 light_dir;    // xyz normalized; w = shadow_strength
    float4 light_color;  // rgb; w = intensity
    float4 ambient;
};

struct DrawTmp {
    float4x4 model_matrix;
    uint mesh_id;
    uint tex_id;
    uint sampler_id;
    uint entity_id;
};

struct MaterialArg {
    texture2d<float> tex [[id(0)]];
    sampler samp [[id(1)]];
};

vertex VertexOut vertexShader(
    VertexInput in [[stage_in]],
    constant RenderPassGlobals& globals [[buffer(LIT_GLOBALS_BUFFER_SLOT)]],
    constant DrawTmp& draw_tmp [[buffer(LIT_DRAW_TMP_BUFFER_SLOT)]]) {
    VertexOut out;
    out.position = globals.view_proj * draw_tmp.model_matrix * in.pos;
    out.textureCoordinate = in.uv;
    // Uniform-scale assumption: model 3x3 rotates+scales; frag renormalizes.
    const float3x3 m3 = float3x3(draw_tmp.model_matrix[0].xyz,
                                 draw_tmp.model_matrix[1].xyz,
                                 draw_tmp.model_matrix[2].xyz);
    out.worldNormal = m3 * in.normal.xyz;
    out.lightDir = globals.light_dir;
    out.lightColor = globals.light_color;
    out.ambient = globals.ambient;
    out.entity_id = draw_tmp.entity_id;
    return out;
}

static inline float4 shade(VertexOut in, MaterialArg material) {
    const float3 n = normalize(in.worldNormal);
    // Half-lambert (pow 2): keeps backsides readable without a fill light.
    const float hl = 0.5f + 0.5f * dot(n, -in.lightDir.xyz);
    const float3 lighting =
        in.ambient.rgb + in.lightColor.rgb * in.lightColor.w * hl * hl;
    const float4 tex =
        material.tex.sample(material.samp, in.textureCoordinate);
    return float4(tex.rgb * lighting, tex.a);
}

struct FragmentOut {
    float4 color [[color(0)]];
    uint id [[color(1)]];
};

fragment FragmentOut fragmentShader(
    VertexOut in [[stage_in]],
    constant MaterialArg& material [[buffer(LIT_MATERIAL_BUFFER_SLOT)]]) {
    FragmentOut out;
    out.color = shade(in, material);
    out.id = in.entity_id;
    return out;
}

fragment float4 fragmentShader_noid(
    VertexOut in [[stage_in]],
    constant MaterialArg& material [[buffer(LIT_MATERIAL_BUFFER_SLOT)]]) {
    return shade(in, material);
}

}  // namespace lit
