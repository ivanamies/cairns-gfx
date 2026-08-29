#pragma once

namespace cairns::rhi {
    
struct Buffer { };
struct Texture { };
struct Material {
    enum class Type : uint32_t {
        kUnknown = 0,
        kUnlit = 1,
        kPbr = 2,
        BlinnPhong = 3,
    };
};
struct BindGroup {
    enum class Type {
        kUnknown = 0,
        kRenderPassGlobal = 1,
        kMaterial = 2,
        kShaderSpecific = 3,
    };
};
struct Sampler { };
// Hypehype Modern Mobile Rendering Architecture sl22/pg36 "PSO with all render state"
struct Shader { };
// Hypehype Modern Mobile Rendering Architecture sl22/pg36 "dynamic buffers (for temp allocated offset bound data". This means UBOS.
struct DynamicBuffers {
    enum class Type {
        kUnknown = 0,
        kTmp = 1, // bind slot 4 temps
    };
};
struct RenderPass { };

} // cairns::rhi
