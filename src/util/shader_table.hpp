// src/util/shader_table.hpp -- material shader selection as data.
//  - Product: a material carries a ShaderKey; the frame's needs (id buffer for
//    picking, normals target for screen-space effects) pick the variant.
//  - Copies: Aaltonen -- permutations are a small static table, not string
//    soup; per-backend file/entry resolution stays in the backend tables
//    (resolve_metal_shader / resolve_vk_shader / Classify).
#pragma once

#include <cstdint>
#include <cstring>

namespace cairns {

enum class ShaderKey : uint8_t {
    kUnlit,
    kLit,
    kTamHatch,
    kStrokeSprite,
};

enum VariantBits : uint8_t {
    kVariantId = 1,       // writes the R32U entity-id MRT (pick/outline)
    kVariantNormals = 2,  // writes the world-normal MRT (screen-space effects)
};

// Logical shader name for a key+variant. Names must match the backend tables;
// today only the unlit pair is wired -- lit/tam/stroke names are reserved so
// the matrix changes once.
inline const char* LogicalShaderName(ShaderKey key, uint8_t variant) {
    const bool id = (variant & kVariantId) != 0;
    const bool norm = (variant & kVariantNormals) != 0;
    switch (key) {
        case ShaderKey::kUnlit:
            if (id) {
                return norm ? "unlit_offscreen_norm" : "unlit_offscreen";
            }
            return norm ? "unlit_offscreen_noid_norm" : "unlit_offscreen_noid";
        case ShaderKey::kLit:
            if (id) {
                return norm ? "lit_offscreen_norm" : "lit_offscreen";
            }
            return norm ? "lit_offscreen_noid_norm" : "lit_offscreen_noid";
        case ShaderKey::kTamHatch:
            if (id) {
                return norm ? "tam_hatch_norm" : "tam_hatch";
            }
            return norm ? "tam_hatch_noid_norm" : "tam_hatch_noid";
        case ShaderKey::kStrokeSprite:
            return "stroke_splat";
    }
    return "unlit_offscreen_noid";
}

// Wire-name -> key for ops ("lit" in cairns.scene.setMaterialShaderAll).
// Returns false on an unknown name (caller reports; no silent default).
inline bool StringToShaderKey(const char* name, ShaderKey* out) {
    struct Row {
        const char* name;
        ShaderKey key;
    };
    static constexpr Row kRows[] = {
        {"lit", ShaderKey::kLit},
        {"stroke_splat", ShaderKey::kStrokeSprite},
        {"tam_hatch", ShaderKey::kTamHatch},
        {"unlit", ShaderKey::kUnlit},
    };
    for (const Row& r : kRows) {
        if (std::strcmp(name, r.name) == 0) {
            *out = r.key;
            return true;
        }
    }
    return false;
}

}  // namespace cairns
