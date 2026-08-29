// util/material_map.hpp
//
// #229 loader: the material->texture index mapping, extracted as a pure
// function so it is unit-testable without a GPU / a .glb fixture. The invariant
// it enforces is subtle and was a real bug: the output arrays must stay 1:1
// with glTF material indices. The draw path binds hot.materials[prim
// .materialIndex]; if a material with no resolvable baseColorTexture is SKIPPED
// (the old behaviour) the list compacts and every later material's slot shifts
// -> off-by-N texture binds. So an unresolvable material emits kNoMaterialTexture
// in place (PreparePrefabResources maps that to a placeholder), never a skip.

#pragma once

#include <cstdint>
#include <vector>

namespace cairns {

// Sentinel in materialToTextureIndex/materialToSamplerIndex for a glTF material
// with no resolvable baseColorTexture (or a texture missing its image/sampler).
inline constexpr uint32_t kNoMaterialTexture = UINT32_MAX;

// One glTF material's baseColorTexture reference, reduced to POD.
struct MaterialTexRef {
    bool has_base_color = false;
    uint32_t texture_index = 0;  // index into the textures[] below
};

// One glTF texture's image + sampler slots, already resolved to kNoMaterialTexture
// when the source texture lacks that index.
struct TextureSlots {
    uint32_t image = kNoMaterialTexture;
    uint32_t sampler = kNoMaterialTexture;
};

// Emit exactly one (image, sampler) pair PER material, in material-index order.
// Unresolvable -> kNoMaterialTexture. Never skips: out arrays end 1:1 with
// `materials`. Output container is templated so the loader can pass its
// chunk-backed (custom-allocator) vectors and tests can pass std::vector.
template <typename OutVec>
inline void MapMaterialTextures(const std::vector<MaterialTexRef>& materials,
                                const std::vector<TextureSlots>& textures,
                                OutVec& out_image, OutVec& out_sampler) {
    for (const MaterialTexRef& m : materials) {
        uint32_t image = kNoMaterialTexture;
        uint32_t sampler = kNoMaterialTexture;
        if (m.has_base_color && m.texture_index < textures.size()) {
            image = textures[m.texture_index].image;
            sampler = textures[m.texture_index].sampler;
        }
        out_image.push_back(image);
        out_sampler.push_back(sampler);
    }
}

}  // namespace cairns
