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

// Sample one AnimationSampler at time t. Returns the interpolated value as
// vec4 (caller branches on the channel's path: T/S in .xyz, R quat in
// .xyzw, weights scalar in .x). Time outside [0, last] clamps to endpoints
// (glTF semantics for non-looping sample; looping is a caller concern).
// CubicSpline interpolation is intentionally not supported -- v7 plan
// punts it; callers should assert before sampling.
inline glm::vec4 SampleSampler(const AnimationSampler& s,
                               AnimationPath path, float t) {
    if (s.times.empty()) {
        return glm::vec4(0.0f);
    }
    if (s.interp == AnimationInterpolation::kCubicSpline) {
        assert(false && "CubicSpline interpolation not implemented");
        return s.values[0];
    }
    if (t <= s.times.front()) {
        return s.values.front();
    }
    if (t >= s.times.back()) {
        return s.values.back();
    }
    // Binary search for the upper key.
    uint32_t lo = 0;
    uint32_t hi = static_cast<uint32_t>(s.times.size()) - 1;
    while (lo + 1 < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (s.times[mid] <= t) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    if (s.interp == AnimationInterpolation::kStep) {
        return s.values[lo];
    }
    // Linear (default).
    const float t0 = s.times[lo];
    const float t1 = s.times[hi];
    const float u = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
    if (path == AnimationPath::kRotation) {
        // Quaternion slerp on the .xyzw payload.
        const glm::quat q0(s.values[lo].w, s.values[lo].x,
                           s.values[lo].y, s.values[lo].z);
        const glm::quat q1(s.values[hi].w, s.values[hi].x,
                           s.values[hi].y, s.values[hi].z);
        const glm::quat qr = glm::slerp(q0, q1, u);
        return glm::vec4(qr.x, qr.y, qr.z, qr.w);
    }
    return glm::mix(s.values[lo], s.values[hi], u);
}

// Evaluate a clip at time t, writing into per-node TRS overrides. The
// caller pre-populates `trs_out[i]` from each Node's bind-pose
// (DecomposeNodeLocal). SampleClip overrides T/R/S for nodes that the
// clip's channels target. Channel weights paths are skipped (handled by
// morph-target code if/when added).
inline void SampleClip(const Clip& clip, float t,
                       std::span<AnimatedTRS> trs_out) {
    // Wrap t to clip duration so loops just work; SampleSampler still
    // clamps internally to its own keyframes.
    const float dur = clip.duration > 0.0f ? clip.duration : 1.0f;
    const float wrapped = t - dur * std::floor(t / dur);
    for (const AnimationChannel& ch : clip.channels) {
        if (ch.samplerIndex < 0 ||
            ch.samplerIndex >= static_cast<int32_t>(clip.samplers.size())) {
            continue;
        }
        if (ch.nodeIndex < 0 ||
            ch.nodeIndex >= static_cast<int32_t>(trs_out.size())) {
            continue;
        }
        const glm::vec4 v = SampleSampler(clip.samplers[ch.samplerIndex],
                                          ch.path, wrapped);
        AnimatedTRS& trs = trs_out[ch.nodeIndex];
        switch (ch.path) {
            case AnimationPath::kTranslation:
                trs.T = glm::vec3(v);
                break;
            case AnimationPath::kRotation:
                trs.R = glm::quat(v.w, v.x, v.y, v.z);
                break;
            case AnimationPath::kScale:
                trs.S = glm::vec3(v);
                break;
            case AnimationPath::kWeights:
                // Morph weights: not handled here.
                break;
        }
    }
}

// Walk the Node tree parent-before-child, writing each node's world matrix
// into world_out. local_xforms[i] is the local matrix of node i (caller
// pre-populates from Node::localTransform or from a SampleClip + ComposeTRS
// override). roots gives top-level node indices.
//
// Scratch (DFS stack) comes from the caller's BumpArena; size = n_nodes
// (worst case). Stack is rewound on exit via a Mark/Rewind. The walk
// preserves Node::localTransform / Node::globalTransform untouched (output
// is to world_out only).
inline void ComputeNodeWorldMatrices(const std::vector<Node>& nodes,
                                     std::span<const int32_t> roots,
                                     std::span<const glm::mat4> local_xforms,
                                     std::span<glm::mat4> world_out,
                                     BumpArena& arena) {
    const uint32_t n = static_cast<uint32_t>(nodes.size());
    if (n == 0) {
        return;
    }
    assert(local_xforms.size() == n);
    assert(world_out.size() == n);
    const auto mark = arena.Mark();
    // (node_idx, parent_world_idx_or_negative_for_root) pairs walked DFS.
    // Pack as two parallel int arrays so we can ArenaList over them.
    int32_t* stack_node = arena.AllocateArray<int32_t>(n);
    int32_t* stack_parent = arena.AllocateArray<int32_t>(n);
    uint32_t top = 0;
    for (size_t i = 0; i < roots.size(); ++i) {
        if (top >= n) {
            break;
        }
        stack_node[top] = roots[i];
        stack_parent[top] = -1;
        ++top;
    }
    while (top > 0) {
        --top;
        const int32_t ni = stack_node[top];
        const int32_t pi = stack_parent[top];
        if (ni < 0 || ni >= static_cast<int32_t>(n)) {
            continue;
        }
        const glm::mat4 parent_world =
            (pi >= 0) ? world_out[pi] : glm::mat4(1.0f);
        world_out[ni] = parent_world * local_xforms[ni];
        const Node& node = nodes[ni];
        for (int32_t child : node.children) {
            if (top >= n) {
                break;
            }
            stack_node[top] = child;
            stack_parent[top] = ni;
            ++top;
        }
    }
    arena.Rewind(mark);
}

// Build the per-joint palette for one skin. For each joint j:
//   palette[j] = inverse(mesh_node_world) * joint_world * skin.inverseBinds[j]
// where joint_world = node_world[skin.jointNodes[j]]. The
// inverse(mesh_node_world) prefactor is per the glTF skin spec -- it
// converts mesh-local positions into joint-local space before the joint
// transform is applied. Pass mesh_node_world_inv pre-computed (caller can
// share it across multiple skins binding the same mesh node).
// #221 Phase 9: pick the "walking" clip in a Scene. Case-insensitive
// substring match against common animation names; falls back to clip 0
// when no match. Returns -1 only when clips is empty. The user-curated
// rule: prefer "walk", then "run", then first.
inline int SelectWalkingClip(const std::vector<Clip>& clips) {
    if (clips.empty()) {
        return -1;
    }
    auto contains_ci = [](const std::string& s, const char* needle) {
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
    for (size_t i = 0; i < clips.size(); ++i) {
        if (best_walk < 0 && contains_ci(clips[i].name, "walk")) {
            best_walk = static_cast<int>(i);
        } else if (best_run < 0 && contains_ci(clips[i].name, "run")) {
            best_run = static_cast<int>(i);
        }
    }
    if (best_walk >= 0) {
        return best_walk;
    }
    if (best_run >= 0) {
        return best_run;
    }
    return 0;
}

inline void ComputeSkinningPalette(const Skin& skin,
                                   std::span<const glm::mat4> node_world,
                                   const glm::mat4& mesh_node_world_inv,
                                   std::span<glm::mat4> palette_out) {
    const uint32_t n = static_cast<uint32_t>(skin.jointNodes.size());
    assert(palette_out.size() >= n);
    assert(skin.inverseBinds.size() == skin.jointNodes.size());
    for (uint32_t j = 0; j < n; ++j) {
        const int32_t ni = skin.jointNodes[j];
        const glm::mat4 jw = (ni >= 0 && static_cast<size_t>(ni) <
                              node_world.size())
                                  ? node_world[ni]
                                  : glm::mat4(1.0f);
        palette_out[j] = mesh_node_world_inv * jw * skin.inverseBinds[j];
    }
}

}  // namespace cairns
