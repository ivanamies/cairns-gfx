// util/alloc_tags.hpp
//
// Compile-time tag structs for print_allocator. One tag per STL
// container field we want to observe. Convention: tag name matches
// "Owner::field_name" verbatim so [ALLOC] log lines are greppable.
//
// Used by the wide phase-E instrumentation across Engine, Prefab::
// Hot/Cold, Mesh::Hot/Cold, Node, Skin, AnimationClip, etc. Keeping
// the tags in one header means field declarations stay one-liner.
//
// SCOPE: this file is only useful when built with
// -DCAIRNS_ALLOC_TRACE=1. In production builds print_allocator's
// allocate/deallocate compile to pass-through and no [ALLOC] line is
// emitted; the tag types are inert.

#pragma once

namespace cairns::tags {

// Engine direct members ────────────────────────────────────────────────
struct EnginePrefabIds {
    static constexpr const char* name() { return "Engine::prefab_ids_"; }
};
struct EngineGlbPaths {
    static constexpr const char* name() { return "Engine::glb_paths_"; }
};
struct EnginePerPrefabAsset {
    static constexpr const char* name() {
        return "Engine::per_prefab_asset_";
    }
};
struct EngineResidentTextures {
    static constexpr const char* name() {
        return "Engine::resident_textures_";
    }
};
struct EnginePerBatchSharedSkin {
    static constexpr const char* name() {
        return "Engine::per_batch_shared_skin_";
    }
};
struct EngineLastHandleSnapshot {
    static constexpr const char* name() {
        return "Engine::last_handle_snapshot_";
    }
};
struct EngineRootNodesStackCache {
    static constexpr const char* name() {
        return "Engine::root_nodes_stack_cache_";
    }
};
struct EngineSceneProxies {
    static constexpr const char* name() {
        return "Engine::scene_proxies_";
    }
};
struct EngineSelection {
    static constexpr const char* name() { return "Engine::selection_"; }
};
struct EngineHighlights {
    static constexpr const char* name() { return "Engine::highlights_"; }
};
struct EnginePerSlotArena {
    static constexpr const char* name() {
        return "Engine::PerSlot::arena_storage";
    }
};

// Prefab::Hot ──────────────────────────────────────────────────────────
struct PrefabHotMeshes {
    static constexpr const char* name() { return "Prefab::Hot::meshes"; }
};
struct PrefabHotRootNodes {
    static constexpr const char* name() { return "Prefab::Hot::rootNodes"; }
};
struct PrefabHotMaterials {
    static constexpr const char* name() { return "Prefab::Hot::materials"; }
};

// Prefab::Cold ─────────────────────────────────────────────────────────
struct PrefabColdNodes {
    static constexpr const char* name() { return "Prefab::Cold::nodes"; }
};
struct PrefabColdBindPose {
    static constexpr const char* name() {
        return "Prefab::Cold::bind_pose";
    }
};
struct PrefabColdTextureHandles {
    static constexpr const char* name() {
        return "Prefab::Cold::textureHandles";
    }
};
struct PrefabColdSamplerHandles {
    static constexpr const char* name() {
        return "Prefab::Cold::samplerHandles";
    }
};
struct PrefabColdGpuParent {
    static constexpr const char* name() {
        return "Prefab::Cold::gpu_parent";
    }
};
struct PrefabColdGpuTopo {
    static constexpr const char* name() { return "Prefab::Cold::gpu_topo"; }
};
struct PrefabColdGpuBindPose {
    static constexpr const char* name() {
        return "Prefab::Cold::gpu_bind_pose";
    }
};
struct PrefabColdGpuChannels {
    static constexpr const char* name() {
        return "Prefab::Cold::gpu_channels";
    }
};
struct PrefabColdGpuSamplers {
    static constexpr const char* name() {
        return "Prefab::Cold::gpu_samplers";
    }
};
struct PrefabColdGpuTimes {
    static constexpr const char* name() {
        return "Prefab::Cold::gpu_times";
    }
};
struct PrefabColdGpuValues {
    static constexpr const char* name() {
        return "Prefab::Cold::gpu_values";
    }
};
struct PrefabColdGpuJointNodes {
    static constexpr const char* name() {
        return "Prefab::Cold::gpu_joint_nodes";
    }
};
struct PrefabColdGpuInverseBinds {
    static constexpr const char* name() {
        return "Prefab::Cold::gpu_inverse_binds";
    }
};
struct PrefabColdSkins {
    static constexpr const char* name() { return "Prefab::Cold::skins"; }
};
struct PrefabColdClips {
    static constexpr const char* name() { return "Prefab::Cold::clips"; }
};

// Mesh::Hot / Cold ─────────────────────────────────────────────────────
struct MeshHotPrimitives {
    static constexpr const char* name() {
        return "Mesh::Hot::primitives";
    }
};
struct MeshColdName {
    static constexpr const char* name() { return "Mesh::Cold::name"; }
};
struct MeshColdCpuPositions {
    static constexpr const char* name() {
        return "Mesh::Cold::cpuPositions";
    }
};
struct MeshColdCpuAttrs {
    static constexpr const char* name() {
        return "Mesh::Cold::cpuAttrs";
    }
};
struct MeshColdCpuIndices {
    static constexpr const char* name() {
        return "Mesh::Cold::cpuIndices";
    }
};
struct MeshColdCpuSkinAttrs {
    static constexpr const char* name() {
        return "Mesh::Cold::cpuSkinAttrs";
    }
};

// Node ─────────────────────────────────────────────────────────────────
struct NodeName {
    static constexpr const char* name() { return "Node::name"; }
};
struct NodeChildren {
    static constexpr const char* name() { return "Node::children"; }
};

// Skin / AnimationClip / AnimationSampler ──────────────────────────────
struct SkinJointNodes {
    static constexpr const char* name() { return "Skin::jointNodes"; }
};
struct SkinInverseBinds {
    static constexpr const char* name() { return "Skin::inverseBinds"; }
};
struct AnimSamplerTimes {
    static constexpr const char* name() {
        return "AnimationSampler::times";
    }
};
struct AnimSamplerValues {
    static constexpr const char* name() {
        return "AnimationSampler::values";
    }
};
struct AnimClipName {
    static constexpr const char* name() {
        return "AnimationClip::name";
    }
};
struct AnimClipSamplers {
    static constexpr const char* name() {
        return "AnimationClip::samplers";
    }
};
struct AnimClipChannels {
    static constexpr const char* name() {
        return "AnimationClip::channels";
    }
};

// Resources F1 ring ────────────────────────────────────────────────────
struct ResourcesDeferred {
    static constexpr const char* name() {
        return "Resources::deferred_";
    }
};

}  // namespace cairns::tags
