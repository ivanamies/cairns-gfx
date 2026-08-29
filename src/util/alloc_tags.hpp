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

// ResourceManager<T> pool internals ────────────────────────────────────
// Tagged via Owner-prefixed names since ResourceManager is templated;
// every typed pool (Buffer/Texture/Sampler/BindGroup/DynamicBuffers/
// Shader/Kernel + Mesh/Prefab/Material) gets one set per dimension.
struct RmHot {
    static constexpr const char* name() { return "ResourceManager::hot_"; }
};
struct RmCold {
    static constexpr const char* name() { return "ResourceManager::cold_"; }
};
struct RmGeneration {
    static constexpr const char* name() {
        return "ResourceManager::generation_";
    }
};
struct RmFreelist {
    static constexpr const char* name() {
        return "ResourceManager::freelist_";
    }
};
struct DynamicBuffersLayout {
    static constexpr const char* name() {
        return "DynamicBuffers::Cold::layout";
    }
};

// util/scene_gpu ───────────────────────────────────────────────────────
struct SceneGpuPosBatch {
    static constexpr const char* name() {
        return "scene_gpu::pos_batch";
    }
};
struct SceneGpuAttrBatch {
    static constexpr const char* name() {
        return "scene_gpu::attr_batch";
    }
};
struct SceneGpuIdxBatch {
    static constexpr const char* name() {
        return "scene_gpu::idx_batch";
    }
};
struct SceneGpuSkinBatch {
    static constexpr const char* name() {
        return "scene_gpu::skin_batch";
    }
};
struct SceneGpuSkinRaw {
    static constexpr const char* name() {
        return "scene_gpu::skin_raw";
    }
};

// util/chunk_allocator ─────────────────────────────────────────────────
struct ChunkAllocatorChunks {
    static constexpr const char* name() {
        return "ChunkAllocator::chunks_";
    }
};

// scene/asset_registry ─────────────────────────────────────────────────
struct AssetRegistryByKey {
    static constexpr const char* name() {
        return "AssetRegistry::by_key_";
    }
};

// control/agent_stdin_drain ────────────────────────────────────────────
struct StdinDrainQueue {
    static constexpr const char* name() {
        return "AgentStdinDrain::queue_";
    }
};
struct StdinDrainQueueItem {
    static constexpr const char* name() {
        return "AgentStdinDrain::queue_item";
    }
};

// control/command_registry ─────────────────────────────────────────────
struct CmdName {
    static constexpr const char* name() { return "Command::name"; }
};
struct CmdDoc {
    static constexpr const char* name() { return "Command::doc"; }
};
struct CmdAliasedFor {
    static constexpr const char* name() {
        return "Command::aliased_for";
    }
};
struct CmdIndexName {
    static constexpr const char* name() { return "CommandIndex::name"; }
};
struct RegistryCommands {
    static constexpr const char* name() {
        return "CommandRegistry::commands_";
    }
};
struct RegistrySortedNames {
    static constexpr const char* name() {
        return "CommandRegistry::sorted_names_";
    }
};
struct RegistryEvents {
    static constexpr const char* name() {
        return "CommandRegistry::events_";
    }
};

// engine_headless ──────────────────────────────────────────────────────
struct EngineHeadlessMessages {
    static constexpr const char* name() {
        return "engine_headless::messages";
    }
};

// render/render_scene + render_proxy_arrays ────────────────────────────
struct RenderSceneResidentTextures {
    static constexpr const char* name() {
        return "RenderScene::resident_textures";
    }
};
struct RenderProxyArrayData {
    static constexpr const char* name() {
        return "RenderProxyArray::data";
    }
};

// render/render_proxy ─────────────────────────────────────────────────
struct SkinnedAttachmentColdName {
    static constexpr const char* name() {
        return "SkinnedAttachment::Cold::name";
    }
};

// render/render_graph ──────────────────────────────────────────────────
struct RGPasses {
    static constexpr const char* name() { return "RenderGraph::passes_"; }
};
struct RGTextures {
    static constexpr const char* name() {
        return "RenderGraph::textures_";
    }
};
struct RGBuffers {
    static constexpr const char* name() {
        return "RenderGraph::buffers_";
    }
};
struct RGTopo {
    static constexpr const char* name() {
        return "RenderGraph::topo_order_";
    }
};
struct RGResolvedTex {
    static constexpr const char* name() {
        return "RenderGraph::resolved_tex_";
    }
};
struct RGResolvedBuf {
    static constexpr const char* name() {
        return "RenderGraph::resolved_buf_";
    }
};
struct RGTexPool {
    static constexpr const char* name() {
        return "RenderGraph::tex_pool_";
    }
};
struct RGBufPool {
    static constexpr const char* name() {
        return "RenderGraph::buf_pool_";
    }
};

// rhi/metal + rhi/vulkan memory_allocator ──────────────────────────────
struct MemAllocBlocks {
    static constexpr const char* name() {
        return "MemoryAllocator::blocks_";
    }
};
struct MemAllocBufferPool {
    static constexpr const char* name() {
        return "MemoryAllocator::buffer_pools_";
    }
};
struct MemAllocImagePool {
    static constexpr const char* name() {
        return "MemoryAllocator::image_pools_";
    }
};
struct MemAllocPendingFree {
    static constexpr const char* name() {
        return "MemoryAllocator::pending_frees_";
    }
};

// rhi/vulkan frames_plat ───────────────────────────────────────────────
struct VkFramesGraphicsCmds {
    static constexpr const char* name() {
        return "vk::FramesPlat::graphics_cmds_";
    }
};
struct VkFramesComputeCmds {
    static constexpr const char* name() {
        return "vk::FramesPlat::compute_cmds_";
    }
};
struct VkFramesImageAvail {
    static constexpr const char* name() {
        return "vk::FramesPlat::image_available_";
    }
};
struct VkFramesRenderFin {
    static constexpr const char* name() {
        return "vk::FramesPlat::render_finished_";
    }
};
struct VkFramesComputeFin {
    static constexpr const char* name() {
        return "vk::FramesPlat::compute_finished_";
    }
};
struct VkFramesInFlight {
    static constexpr const char* name() {
        return "vk::FramesPlat::in_flight_";
    }
};
struct VkFramesComputeInFlight {
    static constexpr const char* name() {
        return "vk::FramesPlat::compute_in_flight_";
    }
};
struct VkFramesGlobalsSets {
    static constexpr const char* name() {
        return "vk::FramesPlat::globals_sets_";
    }
};
struct VkFramesDrawtmpSets {
    static constexpr const char* name() {
        return "vk::FramesPlat::drawtmp_sets_";
    }
};
struct VkFramesCompositeSets {
    static constexpr const char* name() {
        return "vk::FramesPlat::composite_sets_";
    }
};

// rhi/vulkan swap_chain_plat ───────────────────────────────────────────
struct VkSwapImages {
    static constexpr const char* name() {
        return "vk::SwapChainPlat::swapChainImages";
    }
};
struct VkSwapImageViews {
    static constexpr const char* name() {
        return "vk::SwapChainPlat::swapChainImageViews";
    }
};
struct VkSwapFramebuffers {
    static constexpr const char* name() {
        return "vk::SwapChainPlat::swapChainFramebuffers";
    }
};

// rhi/vulkan command_recorder_plat ─────────────────────────────────────
struct VkCmdRecorderRps {
    static constexpr const char* name() {
        return "vk::CommandRecorderPlat::rps";
    }
};
struct VkCmdRecorderFbs {
    static constexpr const char* name() {
        return "vk::CommandRecorderPlat::fbs";
    }
};

// rhi/vulkan gpu_profiler_plat ─────────────────────────────────────────
struct VkProfilerPassNames {
    static constexpr const char* name() {
        return "vk::GpuProfilerPlat::pass_names_";
    }
};
struct VkProfilerPassCount {
    static constexpr const char* name() {
        return "vk::GpuProfilerPlat::pass_count_";
    }
};
struct VkProfilerComputePassCount {
    static constexpr const char* name() {
        return "vk::GpuProfilerPlat::compute_pass_count_";
    }
};

// rhi/metal/frames locals ──────────────────────────────────────────────
struct MtlFramesReadbackRgba {
    static constexpr const char* name() {
        return "metal::frames::readback_rgba";
    }
};

// rhi/vulkan/device locals ─────────────────────────────────────────────
struct VkDeviceValidationLayers {
    static constexpr const char* name() {
        return "vk::device::kValidationLayers";
    }
};
struct VkDeviceExtensionsConst {
    static constexpr const char* name() {
        return "vk::device::kDeviceExtensions";
    }
};
struct VkDeviceLayerProps {
    static constexpr const char* name() {
        return "vk::device::available_layers";
    }
};
struct VkDeviceQueueFamilies {
    static constexpr const char* name() {
        return "vk::device::queue_families";
    }
};
struct VkDeviceExtProps {
    static constexpr const char* name() {
        return "vk::device::available_extensions";
    }
};
struct VkDeviceRequiredExtSet {
    static constexpr const char* name() {
        return "vk::device::required_ext_set";
    }
};
struct VkDevicePhysicalDevices {
    static constexpr const char* name() {
        return "vk::device::physical_devices";
    }
};
struct VkDeviceInstanceExts {
    static constexpr const char* name() {
        return "vk::device::instance_extensions";
    }
};
struct VkDeviceQueueCreateInfos {
    static constexpr const char* name() {
        return "vk::device::queue_create_infos";
    }
};
struct VkDeviceDeviceExts {
    static constexpr const char* name() {
        return "vk::device::device_extensions";
    }
};

// rhi/vulkan/frames locals ─────────────────────────────────────────────
struct VkFramesDumpRgba {
    static constexpr const char* name() {
        return "vk::frames::dump_rgba";
    }
};
struct VkFramesAllocSetLayouts {
    static constexpr const char* name() {
        return "vk::frames::alloc_sets_layouts";
    }
};
struct VkFramesCompositeLayouts {
    static constexpr const char* name() {
        return "vk::frames::composite_layouts";
    }
};
struct VkFramesCompositeFlat {
    static constexpr const char* name() {
        return "vk::frames::composite_flat";
    }
};

// rhi/vulkan/pipelines locals ──────────────────────────────────────────
struct VkPipelinesVertCode {
    static constexpr const char* name() {
        return "vk::pipelines::vert_code";
    }
};
struct VkPipelinesFragCode {
    static constexpr const char* name() {
        return "vk::pipelines::frag_code";
    }
};
struct VkPipelinesCompCode {
    static constexpr const char* name() {
        return "vk::pipelines::comp_code";
    }
};
struct VkPipelinesVtxBindings {
    static constexpr const char* name() {
        return "vk::pipelines::vertex_bindings";
    }
};
struct VkPipelinesVtxAttrs {
    static constexpr const char* name() {
        return "vk::pipelines::vertex_attrs";
    }
};
struct VkPipelinesSetLayouts {
    static constexpr const char* name() {
        return "vk::pipelines::set_layouts";
    }
};

// rhi/vulkan/resources locals ──────────────────────────────────────────
struct VkResourcesDynBindings {
    static constexpr const char* name() {
        return "vk::resources::dyn_vk_bindings";
    }
};
struct VkResourcesDynLayouts {
    static constexpr const char* name() {
        return "vk::resources::dyn_layouts";
    }
};
struct VkResourcesDynBufferInfo {
    static constexpr const char* name() {
        return "vk::resources::dyn_buffer_info";
    }
};
struct VkResourcesDynWrites {
    static constexpr const char* name() {
        return "vk::resources::dyn_writes";
    }
};

}  // namespace cairns::tags
