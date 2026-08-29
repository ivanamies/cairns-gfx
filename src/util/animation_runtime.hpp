// Animation runtime helpers: TRS compose/decompose + walk-clip selection.
// Pure functions over flat arrays; no classes, no virtuals.

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

// Pick the "walking" clip in a prefab: case-insensitive substring match,
// preferring "walk", then "run", then "idle", then the clip with the most
// channels. Returns -1 only when clips is empty. Allocator-templated so
// std and ChunkStdAllocator clip vectors both work; takes the prefab arena
// to resolve the interned NameRef clip names.
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
