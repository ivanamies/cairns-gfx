// #221 Skinning Phase 2: animation runtime (sampler + node walk + palette).
//
// Three pure functions over flat arrays. No classes, no virtuals. Scratch
// comes from a caller-provided BumpArena. By design these do NOT mutate
// gltf_loader's Node::globalTransform: per plan-v7 decision 1, mutating
// that field would move the static golden, so the world matrices are
// written into caller-provided spans instead. A later "wire-in" commit
// can choose to populate Node::globalTransform from the same routine if
// the golden is re-baked.

#pragma once

#include "util/cpu_arena.hpp"
#include "util/gltf_loader.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

namespace cairns {

inline AnimatedTRS DecomposeNodeLocal(const Node& n) {
    AnimatedTRS out;
    glm::vec3 skew;
    glm::vec4 persp;
    glm::decompose(n.localTransform, out.S, out.R, out.T, skew, persp);
    return out;
}

inline glm::mat4 ComposeTRS(const AnimatedTRS& trs) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), trs.T);
    m *= glm::toMat4(trs.R);
    m = glm::scale(m, trs.S);
    return m;
}

// #229 P3: ComputeNodeWorldMatrices + ComputeSkinningPalette (CPU node walk +
// CPU palette build) were dead -- superseded by the GPU anim_eval/palette path
// (no callers anywhere). Removed here because they read Node::children /
// Skin::{jointNodes,inverseBinds} as std::vector, which are now pointer-free
// ArenaSlice (need the prefab arena to resolve). If a CPU fallback is ever
// needed, restore from git and thread Engine::prefab_arena_ through.

// #221 Phase 9: pick the "walking" clip in a Scene. Case-insensitive
// substring match against common animation names; falls back to clip 0
// when no match. Returns -1 only when clips is empty. The user-curated
// rule: prefer "walk", then "run", then first.
// #229 P3: allocator-templated (std + ChunkStdAllocator clip vectors) and
// takes the prefab arena to resolve the interned NameRef clip names.
template <typename Alloc>
inline int SelectWalkingClip(const std::vector<Clip, Alloc>& clips,
                             cairns::BumpArena& arena) {
    if (clips.empty()) {
        return -1;
    }
    auto contains_ci = [](std::string_view s, const char* needle) {
        auto lower = [](char c) { return static_cast<char>(std::tolower(c)); };
        const size_t n = std::strlen(needle);
        if (s.size() < n) {
            return false;
        }
        for (size_t i = 0; i + n <= s.size(); ++i) {
            bool ok = true;
            for (size_t k = 0; k < n; ++k) {
                if (lower(s[i + k]) != lower(needle[k])) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                return true;
            }
        }
        return false;
    };
    int best_walk = -1;
    int best_run = -1;
    int best_idle = -1;
    int richest_idx = 0;
    size_t richest_channels = 0;
    for (size_t i = 0; i < clips.size(); ++i) {
        const size_t ch = clips[i].channels.size();
        if (ch > richest_channels) {
            richest_channels = ch;
            richest_idx = static_cast<int>(i);
        }
        const std::string_view cn = ResolveName(arena, clips[i].name);
        if (best_walk < 0 && contains_ci(cn, "walk")) {
            best_walk = static_cast<int>(i);
        } else if (best_run < 0 && contains_ci(cn, "run")) {
            best_run = static_cast<int>(i);
        } else if (best_idle < 0 && contains_ci(cn, "idle")) {
            best_idle = static_cast<int>(i);
        }
    }
    if (best_walk >= 0 && clips[best_walk].channels.size() > 0) {
        return best_walk;
    }
    if (best_run >= 0 && clips[best_run].channels.size() > 0) {
        return best_run;
    }
    if (best_idle >= 0 && clips[best_idle].channels.size() > 0) {
        return best_idle;
    }
    return richest_idx;
}


}  // namespace cairns
