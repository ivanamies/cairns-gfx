// Out-of-line Engine method bodies. Declarations stay in engine.hpp;
// templates + trivial accessors stay inline there.
//  - Product: one Engine hosts the demo scene (100 GLBs x 32 slices across
//    two viewports, click->pick->eval->move), headless or windowed, driven
//    over the NDJSON / script.eval op surface.

#include "engine.hpp"


namespace cairns {

void Engine::uploadAnimTablesGpu() {
        if (skinning_.eval_kernel.IsNull()) {
            return;
        }
        const auto& prefab_ids = prefab_store_.prefab_ids;
        if (prefab_ids.empty()) {
            return;
        }
        // Aaltonen delta path: walk only the trailing window
        // [skinning_.uploaded_prefab_count..end) and upload the new data at
        // an offset past what was uploaded previously -- steady-state cost
        // O(batch). If a batch outgrows a buffer, fall back to a full
        // rebuild that re-uploads every prefab.
        bool full_rebuild = (skinning_.uploaded_prefab_count == 0) ||
                            (skinning_.uploaded_prefab_count > prefab_ids.size());
        // Anim tables packed into 3 flat SSBOs by element type (+ headers)
        // so the anim_eval pipeline fits WebGPU's per-stage storage-buffer
        // limit.
        // i32_flat: parent | topo | joint_nodes | times(int bits).
        // vec4_flat: bind_pose(T,R,S per joint) | values | inverse_binds(cols).
        // word16_flat: channels | samplers (each GpuChannel/GpuSampler is 16B).
        // Storage that survives retry (filled once per attempt).
        std::vector<cairns::GpuSceneHeader> headers;
        std::vector<int32_t> i32_flat;
        std::vector<glm::vec4> vec4_flat;
        std::vector<glm::uvec4> word16_flat;
        AnimCursors base{};
        AnimCursors target{};
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (full_rebuild) {
                skinning_.uploaded_prefab_count = 0;
                skinning_.cur = {};
            }
            base = skinning_.cur;
            headers.clear();
            i32_flat.clear();
            vec4_flat.clear();
            word16_flat.clear();
            const uint32_t start = skinning_.uploaded_prefab_count;
            headers.reserve(prefab_ids.size() - start);
            for (uint32_t i = start; i < prefab_ids.size(); ++i) {
                cairns::PrefabId sid = prefab_ids[i];
                cairns::Prefab::Hot* hot = prefab_store_.prefabs.GetHot(sid);
                cairns::Prefab::Cold* cold = prefab_store_.prefabs.GetCold(sid);
                if (!hot || !cold) {
                    continue;
                }
                if (cold->skins.empty() || cold->clips.empty() ||
                    cold->nodes.empty()) {
                    hot->gpu_prefab_header_idx = UINT32_MAX;
                    continue;
                }
                cairns::GpuSceneHeader sh{};
                sh.node_count = static_cast<uint32_t>(cold->nodes.size());
                sh.joint_count =
                    static_cast<uint32_t>(cold->gpu_joint_nodes.size());
                sh.channel_count =
                    static_cast<uint32_t>(cold->gpu_channels.size());
                sh.sampler_count =
                    static_cast<uint32_t>(cold->gpu_samplers.size());
                // Offsets are in each packed buffer's ELEMENT units.
                // i32 block (parent | topo | joint_nodes | times):
                sh.parent_off =
                    base.i32 + static_cast<uint32_t>(i32_flat.size());
                i32_flat.insert(i32_flat.end(), cold->gpu_parent.begin(),
                                cold->gpu_parent.end());
                sh.topo_off =
                    base.i32 + static_cast<uint32_t>(i32_flat.size());
                i32_flat.insert(i32_flat.end(), cold->gpu_topo.begin(),
                                cold->gpu_topo.end());
                sh.joint_nodes_off =
                    base.i32 + static_cast<uint32_t>(i32_flat.size());
                i32_flat.insert(i32_flat.end(), cold->gpu_joint_nodes.begin(),
                                cold->gpu_joint_nodes.end());
                sh.times_off =
                    base.i32 + static_cast<uint32_t>(i32_flat.size());
                for (float tf : cold->gpu_times) {
                    int32_t bits;
                    std::memcpy(&bits, &tf, sizeof(bits));
                    i32_flat.push_back(bits);
                }
                // vec4 block (bind_pose 3/joint | values | inverse_binds 4/j):
                sh.bind_pose_off =
                    base.vec4 + static_cast<uint32_t>(vec4_flat.size());
                for (const cairns::GpuTRS& trs : cold->gpu_bind_pose) {
                    vec4_flat.push_back(trs.T);
                    vec4_flat.push_back(trs.R);
                    vec4_flat.push_back(trs.S);
                }
                sh.values_off =
                    base.vec4 + static_cast<uint32_t>(vec4_flat.size());
                vec4_flat.insert(vec4_flat.end(), cold->gpu_values.begin(),
                                 cold->gpu_values.end());
                sh.inverse_binds_off =
                    base.vec4 + static_cast<uint32_t>(vec4_flat.size());
                for (const glm::mat4& m : cold->gpu_inverse_binds) {
                    vec4_flat.push_back(m[0]);
                    vec4_flat.push_back(m[1]);
                    vec4_flat.push_back(m[2]);
                    vec4_flat.push_back(m[3]);
                }
                // word16 block (channels | samplers), each 16B = 1 uvec4:
                sh.mesh_node = cold->gpu_mesh_node;
                sh.duration = cold->gpu_clip_duration;
                // Sampler offsets are prefab-local; fold in this prefab's
                // i32/vec4 bases before the reinterpret.
                const uint32_t local_times_base = sh.times_off;
                const uint32_t local_values_base = sh.values_off;
                sh.channel_off =
                    base.word16 + static_cast<uint32_t>(word16_flat.size());
                for (const cairns::GpuChannel& ch : cold->gpu_channels) {
                    glm::uvec4 w;
                    std::memcpy(&w, &ch, sizeof(w));
                    word16_flat.push_back(w);
                }
                sh.sampler_off =
                    base.word16 + static_cast<uint32_t>(word16_flat.size());
                for (cairns::GpuSampler gs : cold->gpu_samplers) {
                    gs.times_off += local_times_base;
                    gs.values_off += local_values_base;
                    glm::uvec4 w;
                    std::memcpy(&w, &gs, sizeof(w));
                    word16_flat.push_back(w);
                }
                hot->gpu_prefab_header_idx =
                    base.headers + static_cast<uint32_t>(headers.size());
                headers.push_back(sh);
            }
            if (headers.empty()) {
                // No new prefabs had anim data. Cursors unchanged; bump
                // uploaded count so we don't re-walk these on the next
                // call.
                skinning_.uploaded_prefab_count =
                    static_cast<uint32_t>(prefab_ids.size());
                return;
            }
            target.headers =
                base.headers + static_cast<uint32_t>(headers.size());
            target.i32 =
                base.i32 + static_cast<uint32_t>(i32_flat.size());
            target.vec4 =
                base.vec4 + static_cast<uint32_t>(vec4_flat.size());
            target.word16 =
                base.word16 + static_cast<uint32_t>(word16_flat.size());
            // For delta attempts: does every buffer already fit the new
            // total? If not, retry as full rebuild (delta-only writes
            // can't span a destroyed-and-recreated buffer).
            auto fits = [&](const rhi::Handle<rhi::Buffer>& b,
                            uint32_t target_entries,
                            size_t entry_size) -> bool {
                if (b.IsNull()) {
                    return false;
                }
                const uint32_t have = rhi_.resources.GetBufferByteSize(b);
                return have >= target_entries * entry_size;
            };
            if (!full_rebuild && (
                    !fits(skinning_.scene_headers_buf, target.headers,
                          sizeof(cairns::GpuSceneHeader)) ||
                    !fits(skinning_.ae_i32_buf, target.i32, sizeof(int32_t)) ||
                    !fits(skinning_.ae_vec4_buf, target.vec4, sizeof(glm::vec4)) ||
                    !fits(skinning_.ae_word16_buf, target.word16,
                          sizeof(glm::uvec4)))) {
                full_rebuild = true;
                continue;
            }
            break;
        }
        // Upload helper: writes `bytes` of `data` into `out` at byte
        // offset `byte_off`. Allocates / grows `out` so it can hold at
        // least `total_bytes`; recycles the existing handle when its
        // capacity suffices.
        auto upload_at = [&](const void* data, size_t bytes,
                             size_t byte_off, size_t total_bytes,
                             rhi::Handle<rhi::Buffer>& out) -> bool {
            if (total_bytes == 0) {
                total_bytes = 16;
            }
            const uint32_t need = static_cast<uint32_t>(total_bytes);
            uint32_t have = 0;
            if (!out.IsNull()) {
                have = rhi_.resources.GetBufferByteSize(out);
            }
            if (out.IsNull() || have < need) {
                if (!out.IsNull()) {
                    rhi_.device.WaitIdle();
                    rhi_.resources.Destroy(rhi_.alloc, out);
                }
                // Growth pad: 4x need (Aaltonen reserve-and-grow), min 4 KB,
                // so steady-state appends stay on the delta path.
                rhi::BufferDesc bd{};
                uint32_t alloc_size = need * 4;
                if (alloc_size < 4096) {
                    alloc_size = 4096;
                }
                bd.byte_size = alloc_size;
                bd.usage = rhi::kUsageStorage;
                bd.memory = rhi::Memory::kDefault;
                out = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
                if (out.IsNull()) {
                    return false;
                }
                // The dyn_anim_eval descriptor set captured the old buffer
                // handle at creation time; mark it for recreation.
                skinning_.dyn_dirty = true;
            }
            if (data && bytes > 0) {
                rhi_.resources.UploadBuffer(
                    rhi_.alloc, out,
                    static_cast<uint32_t>(byte_off),
                    std::span<const uint8_t>(
                        static_cast<const uint8_t*>(data), bytes));
            }
            return true;
        };
        if (!upload_at(headers.data(),
                       headers.size() * sizeof(cairns::GpuSceneHeader),
                       base.headers * sizeof(cairns::GpuSceneHeader),
                       target.headers * sizeof(cairns::GpuSceneHeader),
                       skinning_.scene_headers_buf) ||
            !upload_at(i32_flat.empty() ? nullptr : i32_flat.data(),
                       i32_flat.size() * sizeof(int32_t),
                       base.i32 * sizeof(int32_t),
                       target.i32 * sizeof(int32_t),
                       skinning_.ae_i32_buf) ||
            !upload_at(vec4_flat.empty() ? nullptr : vec4_flat.data(),
                       vec4_flat.size() * sizeof(glm::vec4),
                       base.vec4 * sizeof(glm::vec4),
                       target.vec4 * sizeof(glm::vec4),
                       skinning_.ae_vec4_buf) ||
            !upload_at(word16_flat.empty() ? nullptr : word16_flat.data(),
                       word16_flat.size() * sizeof(glm::uvec4),
                       base.word16 * sizeof(glm::uvec4),
                       target.word16 * sizeof(glm::uvec4),
                       skinning_.ae_word16_buf)) {
            return;
        }
        skinning_.cur = target;
        skinning_.uploaded_prefab_count =
            static_cast<uint32_t>(prefab_ids.size());
        skinning_.eval_tables_uploaded = true;
        // Any destroy-and-recreate above left stale buffer handles in the
        // anim descriptor sets; recreate them. Also covers the first-ever
        // upload, where init left them null (no boot-time bulk load).
        if (skinning_.dyn_dirty) {
            recreateAnimDynBindings();
            recreateSkinGroupB();
            skinning_.dyn_dirty = false;
        }
        CAIRNS_PRINT("uploadAnimTablesGpu: %s | +%zu scenes -> %u total | "
                     "i32 +%zu/%u vec4 +%zu/%u word16 +%zu/%u\n",
                     full_rebuild ? "FULL" : "DELTA",
                     headers.size(), target.headers,
                     i32_flat.size(), target.i32,
                     vec4_flat.size(), target.vec4,
                     word16_flat.size(), target.word16);
        // Packing concatenates tables and upload_at pads 4x; the anim kernel
        // binds these whole (VK_WHOLE_SIZE), so a packed buffer past
        // max_storage_buffer_range silently garbles. Abort instead.
        const uint32_t i32_sz = rhi_.resources.GetBufferByteSize(skinning_.ae_i32_buf);
        const uint32_t vec4_sz = rhi_.resources.GetBufferByteSize(skinning_.ae_vec4_buf);
        const uint32_t w16_sz = rhi_.resources.GetBufferByteSize(skinning_.ae_word16_buf);
        const uint32_t range = rhi_.device.caps.max_storage_buffer_range;
        const double mb = 1024.0 * 1024.0;
        if (std::getenv("CAIRNS_DUMP_CAPS")) {
            CAIRNS_PRINT_ERR(
                "[CAPS] packed anim SSBO alloc: i32=%.1f vec4=%.1f word16=%.1f MB "
                "(largest %.1f MB vs device range %.1f MB)\n",
                i32_sz / mb, vec4_sz / mb, w16_sz / mb,
                std::max({i32_sz, vec4_sz, w16_sz}) / mb, range / mb);
        }
        if (i32_sz > range || vec4_sz > range || w16_sz > range) {
            CAIRNS_PRINT_ERR(
                "[FATAL] packed anim SSBO exceeds device range %u B: "
                "i32=%u vec4=%u word16=%u -- the 12->6 pack overflowed this "
                "device's storage-buffer limit.\n",
                range, i32_sz, vec4_sz, w16_sz);
            std::abort();
        }
    }

}  // namespace cairns

namespace cairns {

bool Engine::deinit() {
        if (render_thread_) {
            render_thread_->Shutdown();
            render_thread_.reset();
        }
        present_.swapchain.Deinit();
        rhi_.pipelines.Deinit(rhi_.resources);
        rhi_.frames.Deinit();
        rhi_.gpu_profiler.Deinit();
        rhi_.offscreen_targets.Deinit();
        rhi_.resources.Deinit();
        rhi_.alloc.Deinit();
        rhi_.device.Deinit();
        // Free the oversize BumpArena slabs carved from cpu_block_ (per-slot
        // frame arenas + prefab interning arena): BumpArena is non-owning and
        // cpu_block_.Deinit reclaims only the chunk pool, not oversize
        // mallocs -- unfreed they leak (Release) and trip the
        // outstanding-balance assert (Debug).
        for (PerSlot& s : slots_) {
            cpu_block_.Free(s.arena.Base());
            s.arena.Init(nullptr, 0);  // idempotent if deinit() runs twice
        }
        cpu_block_.Free(prefab_arena_.Base());
        prefab_arena_.Init(nullptr, 0);
        return true;
    }

}  // namespace cairns

namespace cairns {

bool Engine::initParticleSsbos() {
        // Portable mt19937 + hand-rolled (NextU32()>>8) * (1/16777216)
        // mapping: std::rand differs per libc (libc++/NDK/glibc), so golden
        // hashes would diverge per platform. ParticleRng keeps particle init
        // byte-stable across runs and libc flavors. Seed override:
        // Engine::SetRandomSeed before GreaterInit (the rng.seed NDJSON op).
        cairns::ParticleRng rng(particles_.random_seed);
        std::vector<Particle> particles(ParticleSystem::kParticleCount);
        for (uint32_t i = 0; i < ParticleSystem::kParticleCount; ++i) {
            const float r = rng.NextUnit();
            const float theta = r * 2.0f * static_cast<float>(std::numbers::pi);
            const float radius = rng.NextUnit();
            particles[i].position[0] = radius * std::cos(theta);
            particles[i].position[1] = radius * std::sin(theta);
            const float vx = (rng.NextUnit() - 0.5f) * 0.5f;
            const float vy = (rng.NextUnit() - 0.5f) * 0.5f;
            particles[i].velocity[0] = vx;
            particles[i].velocity[1] = vy;
            const float t = static_cast<float>(i) / static_cast<float>(ParticleSystem::kParticleCount);
            particles[i].color[0] = t;
            particles[i].color[1] = 1.0f - t;
            particles[i].color[2] = 0.5f;
            particles[i].color[3] = 1.0f;
        }

        const std::span<const uint8_t> init_data(
            reinterpret_cast<const uint8_t*>(particles.data()),
            particles.size() * sizeof(Particle));
        rhi::BufferDesc bd;
        bd.byte_size = static_cast<uint32_t>(particles.size() * sizeof(Particle));
        bd.usage = rhi::kUsageStorage | rhi::kUsageVertex;
        bd.memory = rhi::Memory::kDefault;
        bd.initial_data = init_data;
        particles_.ssbo[0] = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
        bd.initial_data = init_data;
        particles_.ssbo[1] = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
        return !particles_.ssbo[0].IsNull() && !particles_.ssbo[1].IsNull();
    }

}  // namespace cairns

namespace cairns {

bool Engine::initParticles() {
        const std::string shader_dir = cairns::GetBasePathSafe();
        // SSBOs created first so the parity DynamicBuffers can write their
        // bindings 1+2 at create time; the particle kernel references the
        // parity layout (set 0).
        if (!initParticleSsbos()) {
            return false;
        }
        {  // Two parity DynamicBuffers, one per (src,dst) ping-pong order.
           // Binding 0 = UBO_DYN dt over kDynamic master (per-dispatch dyn off).
           // Bindings 1+2 = SSBO over particles_.ssbo[A]/[B] no-dyn.
            for (uint32_t p = 0; p < 2; ++p) {
                cairns::rhi::DynamicBinding pb[3]{};
                for (uint32_t i = 0; i < 3; ++i) {
                    pb[i].stages = cairns::rhi::kStageCompute;
                }
                pb[0].slot = 0;
                pb[0].kind = cairns::rhi::BufferKind::kUniform;
                pb[0].max_range = sizeof(float);
                pb[0].has_dynamic_offset = true;
                pb[1].slot = 1;
                pb[1].kind = cairns::rhi::BufferKind::kStorage;
                pb[1].max_range = 0;  // VK_WHOLE_SIZE
                pb[1].has_dynamic_offset = false;
                pb[1].backing = particles_.ssbo[p];          // src
                pb[2].slot = 2;
                pb[2].kind = cairns::rhi::BufferKind::kStorage;
                pb[2].max_range = 0;
                pb[2].has_dynamic_offset = false;
                pb[2].backing = particles_.ssbo[p ^ 1];      // dst
                cairns::rhi::DynamicBuffersDesc pd{};
                pd.debug_name = (p == 0) ? "dyn_particle_parity_0"
                                          : "dyn_particle_parity_1";
                pd.bindings =
                    std::span<const cairns::rhi::DynamicBinding>(pb, 3);
                dyn_particle_parity_[p] =
                    rhi_.resources.CreateDynamicBuffers(rhi_.alloc,
                                                          rhi_.frames, pd);
                if (dyn_particle_parity_[p].IsNull()) {
                    CAIRNS_PRINT("initParticles: dyn_particle_parity create failed\n");
                    return false;
                }
            }
        }
        {  // particle compute kernel via rhi
            rhi::ComputePipelineDesc desc{};
            desc.logical_shader = "particle";
            desc.shader_dir = shader_dir.c_str();
            desc.debug_name = "particle_compute";
            desc.layout = rhi::ComputePipelineLayout::kParticle;
            // Pipeline layout reads from parity[0]'s DynamicBuffers Hot
            // layout (UBO_DYN @0 + 2 SSBO); both parity sets share the
            // same layout shape.
            desc.dyn_set_0 = dyn_particle_parity_[0];
            particles_.kernel = rhi_.pipelines.CreateComputePipeline(rhi_.resources, rhi_.frames, desc);
            if (particles_.kernel.IsNull()) {
                return false;
            }
        }
        {  // particle render pipeline via rhi
            const rhi::VertexInputAttribute attrs[2] = {
                {0, 0, rhi::Format::kRg32F,
                 static_cast<uint32_t>(offsetof(Particle, position))},
                {1, 0, rhi::Format::kRgba32F,
                 static_cast<uint32_t>(offsetof(Particle, color))},
            };
            const rhi::VertexBufferLayout layout{
                0, static_cast<uint32_t>(sizeof(Particle))};
            rhi::GraphicsPipelineDesc desc{};
            desc.logical_shader = "particle";
            desc.shader_dir = shader_dir.c_str();
            desc.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(attrs, 2);
            desc.vertex_buffers =
                std::span<const rhi::VertexBufferLayout>(&layout, 1);
            desc.topology = rhi::PrimitiveTopology::kPointList;
            desc.cull = rhi::CullMode::kBack;
            desc.front_face = rhi::FrontFace::kCounterClockwise;
            desc.depth_test = true;
            desc.depth_write = true;
            desc.depth_compare = rhi::CompareOp::kLess;
            desc.blend.enable = true;
            desc.blend.src_color = rhi::BlendFactor::kSrcAlpha;
            desc.blend.dst_color = rhi::BlendFactor::kOneMinusSrcAlpha;
            desc.blend.src_alpha = rhi::BlendFactor::kOne;
            desc.blend.dst_alpha = rhi::BlendFactor::kZero;
            desc.color_format = rhi::Format::kBgra8Unorm;
            desc.depth_format = rhi::Format::kD32F;
            desc.sample_count = sampleCount;
            desc.push_constant_bytes = 0;
            desc.debug_name = "particle_render";
            desc.swap_chain = &present_.swapchain;
            particles_.render_shader = rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.frames, desc);
            if (particles_.render_shader.IsNull()) {
                return false;
            }
            // Offscreen variant for the render-graph forward pass, which is
            // MRT (BGRA color + R32U id); pipeline declares both attachments
            // so the offscreen-target-cache renderpass matches the
            // pipeline's compat renderpass.
            rhi::GraphicsPipelineDesc opd = desc;
            opd.sample_count = 1;
            opd.swap_chain = nullptr;
            opd.color_formats[0] = rhi::Format::kBgra8Unorm;
            opd.color_formats[1] = rhi::Format::kR32Uint;
            opd.color_count = 2;
            // Particle frag writes only outColor (location 0); the R32U id
            // attachment exists for renderpass-compat but gets
            // colorWriteMask=0 so we don't undef-stomp it.
            opd.frag_color_output_count = 1;
            opd.debug_name = "particle_render_offscreen";
            particles_.render_offscreen = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, opd);
            if (particles_.render_offscreen.IsNull()) {
                return false;
            }
            // Id-less variant for the no-id forward pass. Same shaders; the
            // frag's location-1 write to the id attachment becomes a no-op
            // write to a discarded location.
            rhi::GraphicsPipelineDesc npd = opd;
            npd.color_formats[1] = rhi::Format::kBgra8Unorm;  // unused
            npd.color_count = 1;
            npd.debug_name = "particles_.render_offscreennoid";
            particles_.render_offscreen_noid =
                rhi_.pipelines.CreateGraphicsPipeline(
                    rhi_.resources, rhi_.frames, npd);
            if (particles_.render_offscreen_noid.IsNull()) {
                return false;
            }
        }

        {
            // imgui pipeline (swapchain MSAA, alpha blend, no depth).
            const std::string shader_dir = cairns::GetBasePathSafe();
            const rhi::VertexInputAttribute ia[3] = {
                {0, 0, rhi::Format::kRg32F, offsetof(ImDrawVert, pos)},
                {1, 0, rhi::Format::kRg32F, offsetof(ImDrawVert, uv)},
                {2, 0, rhi::Format::kRgba8Unorm, offsetof(ImDrawVert, col)},
            };
            const rhi::VertexBufferLayout il{
                0, static_cast<uint32_t>(sizeof(ImDrawVert))};
            rhi::GraphicsPipelineDesc id{};
            id.logical_shader = "imgui";
            id.shader_dir = shader_dir.c_str();
            id.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(ia, 3);
            id.vertex_buffers = std::span<const rhi::VertexBufferLayout>(&il, 1);
            id.topology = rhi::PrimitiveTopology::kTriangleList;
            id.cull = rhi::CullMode::kNone;
            id.depth_test = false;
            id.depth_write = false;
            id.blend.enable = true;
            id.blend.src_color = rhi::BlendFactor::kSrcAlpha;
            id.blend.dst_color = rhi::BlendFactor::kOneMinusSrcAlpha;
            id.blend.src_alpha = rhi::BlendFactor::kOne;
            id.blend.dst_alpha = rhi::BlendFactor::kOneMinusSrcAlpha;
            const bool surfaceless_imgui = !present_.final_target.IsNull();
            id.color_format = rhi::Format::kBgra8Unorm;
            id.depth_format = surfaceless_imgui ? rhi::Format::kUndefined
                                                : rhi::Format::kD32F;
            id.sample_count = surfaceless_imgui ? 1u : sampleCount;
            id.push_constant_bytes = 16;
            id.debug_name = "imgui";
            id.swap_chain = surfaceless_imgui ? nullptr : &present_.swapchain;
            imgui_ = rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources,
                                                          rhi_.frames, id);
            if (imgui_.IsNull()) {
                return false;
            }

            // imgui font atlas -> Texture. Scale font + style by surface
            // width so HiDPI / mobile displays don't render a postage-stamp
            // overlay. 1280px is the desktop reference width.
            ImGuiIO& io = ImGui::GetIO();
            const float kRefWidth = 900.0f;
            const float raw_scale =
                static_cast<float>(FrameWidth()) / kRefWidth;
#if CAIRNS_ANDROID
            const float kScaleMax = 2.5f;
#elif CAIRNS_APPLE && TARGET_OS_IPHONE
            const float kScaleMax = 1.0f;
#else
            const float kScaleMax = 1.5f;
#endif
            const float dpi_scale = std::clamp(raw_scale, 1.0f, kScaleMax);
            ImFontConfig fc;
            fc.SizePixels = 13.0f * dpi_scale;
            io.Fonts->Clear();
            io.Fonts->AddFontDefault(&fc);
            ImGui::GetStyle().ScaleAllSizes(dpi_scale);
            unsigned char* pixels = nullptr;
            int fw = 0;
            int fh = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
            rhi::TextureDesc ftd{};
            ftd.dimensions = {fw, fh, 1};
            ftd.format = rhi::Format::kRgba8Unorm;
            ftd.mip_levels = 1;
            ftd.array_layers = 1;
            ftd.usage = rhi::kTexUsageSampled | rhi::kTexUsageTransferDst;
            ftd.memory = rhi::Memory::kDefault;
            ftd.initial_data = std::span<const uint8_t>(
                pixels, static_cast<size_t>(fw) * fh * 4);
            imgui_font_ = rhi_.resources.CreateTexture(rhi_.alloc, ftd);
            rhi::SamplerDesc sd{};
            sd.min_filter = rhi::Filter::kLinear;
            sd.mag_filter = rhi::Filter::kLinear;
            sd.mip_filter = rhi::Filter::kLinear;
            sd.address_mode = rhi::AddressMode::kClampToEdge;
            imgui_sampler_ = rhi_.resources.CreateSampler(sd);
        }

        return !particles_.ssbo[0].IsNull() && !particles_.ssbo[1].IsNull();
    }

}  // namespace cairns

namespace cairns {

void Engine::initSkinKernel() {
        const std::string shader_dir = cairns::GetBasePathSafe();
        rhi::ComputePipelineDesc desc{};
        desc.logical_shader = "skin";
        desc.shader_dir = shader_dir.c_str();
        desc.debug_name = "skin_compute";
        desc.layout = rhi::ComputePipelineLayout::kSkin;
        skinning_.skin_kernel = rhi_.pipelines.CreateComputePipeline(rhi_.resources,
                                                              rhi_.frames, desc);
        if (skinning_.skin_kernel.IsNull()) {
            CAIRNS_PRINT("initSkinKernel: skin kernel load failed (skin.comp.spv / skin.metal missing?) -- skinning disabled\n");
        }
    }

}  // namespace cairns

namespace cairns {

bool Engine::recreateSkinGroupB() {
        if (skinning_.skin_kernel.IsNull() || skinning_.output_pool_buffer.IsNull()) {
            return true;
        }
        cairns::rhi::DynamicBinding gb[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            gb[i].stages = cairns::rhi::kStageCompute;
        }
        gb[0].slot = 0;
        gb[0].kind = cairns::rhi::BufferKind::kUniform;
        gb[0].max_range = 64u;
        gb[0].has_dynamic_offset = true;
        gb[1].slot = 1;
        gb[1].kind = cairns::rhi::BufferKind::kStorage;
        gb[1].max_range = 1u << 20;
        gb[1].has_dynamic_offset = true;
        if (skinning_.eval_tables_uploaded) {
            gb[1].backing = skinning_.palette_out_buf;
        }
        gb[2].slot = 2;
        gb[2].kind = cairns::rhi::BufferKind::kStorage;
        gb[2].max_range = 16384u;
        gb[2].has_dynamic_offset = true;
        gb[3].slot = 3;
        gb[3].kind = cairns::rhi::BufferKind::kStorage;
        gb[3].max_range = 0;  // VK_WHOLE_SIZE
        gb[3].has_dynamic_offset = false;
        gb[3].backing = skinning_.output_pool_buffer;
        cairns::rhi::DynamicBuffersDesc gd{};
        gd.debug_name = "dyn_skin_group_b";
        gd.bindings = std::span<const cairns::rhi::DynamicBinding>(gb, 4);
        if (!skinning_.dyn_skin_group_b.IsNull()) {
            rhi_.resources.DeferFree(skinning_.dyn_skin_group_b);
        }
        skinning_.dyn_skin_group_b =
            rhi_.resources.CreateDynamicBuffers(rhi_.alloc, rhi_.frames, gd);
        if (skinning_.dyn_skin_group_b.IsNull()) {
            CAIRNS_PRINT("recreateSkinGroupB: dyn_skin_group_b create failed\n");
            return false;
        }
        return true;
    }

}  // namespace cairns

namespace cairns {

bool Engine::recreateAnimDynBindings() {
        if (!skinning_.eval_tables_uploaded) {
            return true;
        }
        // Bindings 1-6 = i32 / vec4 / word16 / headers / world_scratch /
        // palette_out (matching the kernel binding numbers).
        const rhi::Handle<rhi::Buffer> ae_ssbo[6] = {
            skinning_.ae_i32_buf, skinning_.ae_vec4_buf, skinning_.ae_word16_buf,
            skinning_.scene_headers_buf, skinning_.world_scratch_buf, skinning_.palette_out_buf,
        };
        cairns::rhi::DynamicBinding ae_b[7]{};
        for (uint32_t i = 0; i < 7; ++i) {
            ae_b[i].stages = cairns::rhi::kStageCompute;
        }
        ae_b[0].slot = 0;
        ae_b[0].kind = cairns::rhi::BufferKind::kUniform;
        ae_b[0].max_range = 16384u;
        ae_b[0].has_dynamic_offset = true;
        for (uint32_t i = 0; i < 6; ++i) {
            ae_b[1 + i].slot = 1 + i;
            ae_b[1 + i].kind = cairns::rhi::BufferKind::kStorage;
            ae_b[1 + i].max_range = 0;  // VK_WHOLE_SIZE
            ae_b[1 + i].has_dynamic_offset = false;
            ae_b[1 + i].backing = ae_ssbo[i];
        }
        cairns::rhi::DynamicBuffersDesc ae_d{};
        ae_d.debug_name = "dyn_anim_eval";
        ae_d.bindings =
            std::span<const cairns::rhi::DynamicBinding>(ae_b, 7);
        // Fenced deferred deletion, not WaitIdle+Destroy: the new set is
        // used immediately; the old one stays alive until its in-flight
        // frame's fence bucket drains. No GPU stall.
        if (!skinning_.dyn_anim_eval.IsNull()) {
            rhi_.resources.DeferFree(skinning_.dyn_anim_eval);
        }
        skinning_.dyn_anim_eval =
            rhi_.resources.CreateDynamicBuffers(rhi_.alloc, rhi_.frames, ae_d);
        if (skinning_.dyn_anim_eval.IsNull()) {
            CAIRNS_PRINT("recreateAnimDynBindings: dyn_anim_eval create failed\n");
            return false;
        }
        return true;
    }

}  // namespace cairns

namespace cairns {

void Engine::initAnimEvalKernel() {
        const std::string shader_dir = cairns::GetBasePathSafe();
        rhi::ComputePipelineDesc desc{};
        desc.logical_shader = "anim_eval";
        desc.shader_dir = shader_dir.c_str();
        desc.debug_name = "anim_eval";
        desc.layout = rhi::ComputePipelineLayout::kAnimEval;
        skinning_.eval_kernel = rhi_.pipelines.CreateComputePipeline(
            rhi_.resources, rhi_.frames, desc);
        if (skinning_.eval_kernel.IsNull()) {
            CAIRNS_PRINT("initAnimEvalKernel: load failed -- gpu palette eval disabled\n");
        }
    }

}  // namespace cairns

namespace cairns {

void Engine::BuildSkinFrame(uint32_t slot) {
        PerSlot& s = slots_[slot];
        s.pkt.skin_batches = std::span<const cairns::SkinBatchGpu>{};
        s.pkt.palettes = std::span<const glm::mat4>{};
        s.pkt.instance_meta = std::span<const glm::uvec2>{};
        s.pkt.actor_records = std::span<const cairns::GpuActorRecord>{};

        if (skinning_.skin_kernel.IsNull() || skinning_.output_pool_buffer.IsNull()) {
            return;
        }
        if (skinning_.eval_kernel.IsNull() || !skinning_.eval_tables_uploaded) {
            return;
        }
        cairns::Scene::Cold* wc = scene_mgr_.pool.GetCold(scene_mgr_.active);
        if (!wc) {
            return;
        }
        // Cull skinned actors against the active viewport frustum: 6 planes
        // from view_proj (row3 +- rowK), bind-pose AABB in world space
        // padded 1.5x for animation motion. A fully-outside actor is
        // skipped in BOTH the anim_eval record and the skin batch.
        const glm::mat4& vp_for_cull =
            s.pending_globals[viewport_mgr_.active_index].view_proj;
        glm::vec4 cull_planes[6];
        {
            const glm::mat4 m = glm::transpose(vp_for_cull);
            cull_planes[0] = m[3] + m[0];  // left
            cull_planes[1] = m[3] - m[0];  // right
            cull_planes[2] = m[3] + m[1];  // bottom
            cull_planes[3] = m[3] - m[1];  // top
            cull_planes[4] = m[3] + m[2];  // near (ZO)
            cull_planes[5] = m[3] - m[2];  // far
            for (int i = 0; i < 6; ++i) {
                const float L = glm::length(glm::vec3(cull_planes[i]));
                if (L > 0.0f) {
                    cull_planes[i] /= L;
                }
            }
        }
        auto aabb_outside =
            [&](const glm::vec3& mn, const glm::vec3& mx) -> bool {
                for (int i = 0; i < 6; ++i) {
                    const glm::vec3 n(cull_planes[i]);
                    const float d = cull_planes[i].w;
                    const glm::vec3 p(
                        n.x >= 0.0f ? mx.x : mn.x,
                        n.y >= 0.0f ? mx.y : mn.y,
                        n.z >= 0.0f ? mx.z : mn.z);
                    if (glm::dot(n, p) + d < 0.0f) {
                        return true;
                    }
                }
                return false;
            };
        auto actor_world_aabb =
            [&](const cairns::Mesh::Hot* mhot, const glm::mat4& world,
                glm::vec3* out_min, glm::vec3* out_max) {
                if (mhot->bind_aabb_min.x > mhot->bind_aabb_max.x) {
                    *out_min = glm::vec3(-1.0e30f);
                    *out_max = glm::vec3( 1.0e30f);
                    return;
                }
                const glm::vec3 c =
                    0.5f * (mhot->bind_aabb_min + mhot->bind_aabb_max);
                const glm::vec3 h =
                    1.5f * 0.5f * (mhot->bind_aabb_max - mhot->bind_aabb_min);
                glm::vec3 wmin( 1.0e30f);
                glm::vec3 wmax(-1.0e30f);
                for (int i = 0; i < 8; ++i) {
                    const glm::vec3 corner(
                        c.x + ((i & 1) ? h.x : -h.x),
                        c.y + ((i & 2) ? h.y : -h.y),
                        c.z + ((i & 4) ? h.z : -h.z));
                    const glm::vec4 wc4 = world * glm::vec4(corner, 1.0f);
                    const glm::vec3 wc(wc4 / wc4.w);
                    wmin = glm::min(wmin, wc);
                    wmax = glm::max(wmax, wc);
                }
                *out_min = wmin;
                *out_max = wmax;
            };
        auto view = wc->registry.view<const cairns::SkinRef,
                                       const cairns::WorldTransform>();

        constexpr uint32_t kBucketCap = cairns::kMaxSkinnedMeshes;
        uint32_t* mesh_actor_count =
            s.arena.AllocateArray<uint32_t>(kBucketCap);
        std::memset(mesh_actor_count, 0,
                    sizeof(uint32_t) * kBucketCap);

        uint32_t total_actors = 0;
        uint32_t dropped_actors = 0;
        entt::entity* kept_entities =
            s.arena.AllocateArray<entt::entity>(kAnimActorsCap);
        for (auto e : view) {
            const cairns::SkinRef& sr = view.get<const cairns::SkinRef>(e);
            auto* sh = skinning_.skins.GetHot(sr.id);
            if (!sh) {
                continue;
            }
            if (sh->mesh.index >= kBucketCap) {
                continue;
            }
            const cairns::Mesh::Hot* mhot_c = prefab_store_.meshes.GetHot(sh->mesh);
            if (mhot_c) {
                const cairns::WorldTransform& wt =
                    view.get<const cairns::WorldTransform>(e);
                glm::vec3 wmn, wmx;
                actor_world_aabb(mhot_c, wt.world, &wmn, &wmx);
                if (aabb_outside(wmn, wmx)) {
                    continue;
                }
            }
            if (total_actors >= kAnimActorsCap) {
                ++dropped_actors;
                continue;
            }
            kept_entities[total_actors] = e;
            ++mesh_actor_count[sh->mesh.index];
            ++total_actors;
        }
        if (dropped_actors > 0) {
            // Loud every-frame report via CAIRNS_PRINT_ERR so Android
            // logcat surfaces it at ERROR level (the silent latch-once
            // hid that a large fraction of actors were dropping to bind
            // pose). Animated + dropped quoted so the shortfall is obvious.
            CAIRNS_PRINT_ERR(
                "[ANIM-CAP] BuildSkinFrame slot=%u: hit kAnimActorsCap=%u; "
                "animated=%u dropped=%u (raise cap or lower hero count)\n",
                slot, kAnimActorsCap, total_actors, dropped_actors);
            skinning_.actors_cap_warned = true;
        }
        if (total_actors == 0) {
            return;
        }

        uint32_t* bucket_remap =
            s.arena.AllocateArray<uint32_t>(kBucketCap);
        std::memset(bucket_remap, 0xFF,
                    sizeof(uint32_t) * kBucketCap);

        cairns::SkinBatchGpu* batches =
            s.arena.AllocateArray<cairns::SkinBatchGpu>(kBucketCap);
        uint32_t* bucket_inst_cursor =
            s.arena.AllocateArray<uint32_t>(kBucketCap);
        std::memset(bucket_inst_cursor, 0,
                    sizeof(uint32_t) * kBucketCap);

        uint32_t bucket_count = 0;
        uint32_t meta_running = 0;
        uint32_t palette_running = 0;
        for (uint32_t k = 0; k < total_actors; ++k) {
            const entt::entity e = kept_entities[k];
            const cairns::SkinRef& sr = view.get<const cairns::SkinRef>(e);
            auto* sh = skinning_.skins.GetHot(sr.id);
            if (!sh) {
                continue;
            }
            if (bucket_remap[sh->mesh.index] != UINT32_MAX) {
                continue;
            }
            const cairns::Mesh::Hot* mhot = prefab_store_.meshes.GetHot(sh->mesh);
            if (!mhot) {
                continue;
            }
            const uint32_t inst = mesh_actor_count[sh->mesh.index];
            const uint32_t joint_count = sh->joint_count;
            const uint32_t vert_count = mhot->vert_count;
            if (inst == 0 || joint_count == 0 || vert_count == 0) {
                continue;
            }
            bucket_remap[sh->mesh.index] = bucket_count;
            cairns::SkinBatchGpu& b = batches[bucket_count];
            b.mesh_set = rhi::Handle<rhi::BindGroup>{};
            b.mesh = sh->mesh;
            b.first_meta = meta_running;
            b.first_palette_mat4 = palette_running;
            b.instance_count = inst;
            b.joint_count = joint_count;
            b.vertex_count = vert_count;
            b.workgroups = (vert_count + 63u) / 64u;
            meta_running += inst;
            palette_running += inst * joint_count;
            ++bucket_count;
        }
        if (bucket_count == 0) {
            return;
        }

        // Vertices the skin kernel actually dispatches this frame (post-cull,
        // post-cap), not the load-time mesh total.
        if (engine_cfg_.anim_vert_report) {
            uint64_t anim_verts = 0;
            uint64_t anim_joints = 0;
            uint32_t total_inst = 0;
            uint32_t max_joints = 0;
            for (uint32_t bi = 0; bi < bucket_count; ++bi) {
                anim_verts += static_cast<uint64_t>(batches[bi].vertex_count) *
                              batches[bi].instance_count;
                anim_joints += static_cast<uint64_t>(batches[bi].joint_count) *
                               batches[bi].instance_count;
                total_inst += batches[bi].instance_count;
                if (batches[bi].joint_count > max_joints) {
                    max_joints = batches[bi].joint_count;
                }
            }
            CAIRNS_PRINT(
                "[ANIM-VERTS] slot=%u actors=%u meshes=%u instances=%u "
                "verts/frame=%llu joints/frame=%llu max_joints=%u\n",
                slot, total_actors, bucket_count, total_inst,
                static_cast<unsigned long long>(anim_verts),
                static_cast<unsigned long long>(anim_joints), max_joints);
        }

        glm::uvec2* instance_meta =
            s.arena.AllocateArray<glm::uvec2>(meta_running);
        cairns::GpuActorRecord* actor_records =
            s.arena.AllocateArray<cairns::GpuActorRecord>(meta_running);

        // Wide-base anim clock: accumulate + wrap in double, narrow once.
        // float sim time loses sub-frame precision after ~17 minutes. The
        // kernel's own wrap (anim_eval.comp.glsl) still runs on the
        // narrowed value as a no-op safety.
        const double anim_t_d =
            static_cast<double>(sim_frame_) * cairns::kFixedDt;
        for (uint32_t k = 0; k < total_actors; ++k) {
            const entt::entity e = kept_entities[k];
            const cairns::SkinRef& sr = view.get<const cairns::SkinRef>(e);
            auto* sh = skinning_.skins.GetHot(sr.id);
            if (!sh) {
                continue;
            }
            if (sh->gpu_prefab_header_idx == UINT32_MAX) {
                continue;
            }
            const uint32_t bi = bucket_remap[sh->mesh.index];
            if (bi == UINT32_MAX) {
                continue;
            }
            cairns::SkinBatchGpu& b = batches[bi];
            const uint32_t cursor = bucket_inst_cursor[bi];
            if (cursor >= b.instance_count) {
                continue;
            }
            const uint32_t actor_idx = b.first_meta + cursor;
            const uint32_t palette_slot_base =
                b.first_palette_mat4 + cursor * b.joint_count;

            instance_meta[actor_idx] =
                glm::uvec2(cursor * b.joint_count, sh->slice_offset);

            // Duration cached on Hot at skin-create; no per-actor Cold
            // fetch on the frame path.
            const double dur = static_cast<double>(sh->gpu_clip_duration);
            const double scaled =
                anim_t_d * static_cast<double>(sh->time_scale) +
                static_cast<double>(sh->time_offset);
            const double wrapped = scaled - dur * std::floor(scaled / dur);

            cairns::GpuActorRecord& rec = actor_records[actor_idx];
            rec.scene_idx = sh->gpu_prefab_header_idx;
            rec.world_scratch_base = actor_idx * kAnimMaxNodes;
            rec.palette_out_base = palette_slot_base;
            rec.time = static_cast<float>(wrapped);

            ++bucket_inst_cursor[bi];
        }

        s.pkt.instance_meta =
            std::span<const glm::uvec2>(instance_meta, meta_running);
        s.pkt.actor_records =
            std::span<const cairns::GpuActorRecord>(actor_records, meta_running);
        s.pkt.skin_batches =
            std::span<const cairns::SkinBatchGpu>(batches, bucket_count);
    }

}  // namespace cairns

namespace cairns {

bool Engine::initRenderPipeline() {
        {
            // Per-material set-2 bind groups live in Material::Hot
            // (resolved per-draw), not a parallel array. Walk every live
            // material; build its BindGroup from Cold's texture+sampler;
            // store into Hot.set2.
            prefab_store_.materials.ForEachLive(
                [&](cairns::Material::Hot& hot,
                    cairns::Material::Cold& cold) {
                    // Skip untextured-material placeholders (null color).
                    if (cold.color.IsNull()) {
                        return;
                    }
                    const rhi::TextureBinding tb{0, cold.color};
                    const rhi::SamplerBinding sb{0, cold.sampler};
                    rhi::BindGroupDesc bgd{};
                    bgd.textures = std::span<const rhi::TextureBinding>(&tb, 1);
                    bgd.samplers = std::span<const rhi::SamplerBinding>(&sb, 1);
                    hot.set2 = rhi_.resources.CreateBindGroup(bgd);
                });
        }

        {  // unlit graphics pipeline via rhi
            const std::string shader_dir = cairns::GetBasePathSafe();
            const rhi::VertexInputAttribute vtx_attrs[2] = {
                {0, cairns::kMeshPosBindSlot, rhi::Format::kRgba32F, 0},
                // stream 1: uv at offset 48 in the 64-byte VertexAttribute.
                {1, cairns::kMeshAttrVertexBindSlot, rhi::Format::kRg32F, 48},
            };
            const rhi::VertexBufferLayout vtx_layouts[2] = {
                {cairns::kMeshPosBindSlot, static_cast<uint32_t>(sizeof(glm::vec4))},
                {cairns::kMeshAttrVertexBindSlot, 64},
            };
            rhi::GraphicsPipelineDesc desc{};
            desc.logical_shader = "unlit";
            desc.shader_dir = shader_dir.c_str();
            desc.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(vtx_attrs, 2);
            desc.vertex_buffers =
                std::span<const rhi::VertexBufferLayout>(vtx_layouts, 2);
            desc.topology = rhi::PrimitiveTopology::kTriangleList;
            desc.cull = rhi::CullMode::kBack;
            desc.front_face = rhi::FrontFace::kCounterClockwise;
            desc.depth_test = true;
            desc.depth_write = true;
            desc.depth_compare = rhi::CompareOp::kLess;
            desc.color_format = rhi::Format::kBgra8Unorm;
            desc.depth_format = rhi::Format::kD32F;
            desc.sample_count = sampleCount;
            desc.push_constant_bytes = 0;  // attrs are a vertex stream; no base_vertex

            // Offscreen variant: single-sample, no swapchain compat. Same shaders
            // + vertex layout as unlit; targets a render-graph color_off+depth_off.
            // MRT: forward pass writes {BGRA color, R32U id} -- pipeline
            // declares both formats so vk renderpass compat matches the
            // 2-attachment offscreen renderpass cache key.
            rhi::GraphicsPipelineDesc ofd = desc;
            ofd.logical_shader = "unlit_offscreen";
            ofd.sample_count = 1;
            ofd.swap_chain = nullptr;
            ofd.color_formats[0] = rhi::Format::kBgra8Unorm;
            ofd.color_formats[1] = rhi::Format::kR32Uint;
            ofd.color_count = 2;
            ofd.debug_name = "unlit_offscreen";
            unlit_offscreen_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, ofd);
            if (unlit_offscreen_.IsNull()) {
                std::exit(0);
            }

            // Id-less variant: single color attachment, no R32U write in
            // the fragment shader. Selected by RecordFrame when no consumer
            // wants the id channel this frame.
            rhi::GraphicsPipelineDesc nid = desc;
            nid.logical_shader = "unlit_offscreen_noid";
            nid.sample_count = 1;
            nid.swap_chain = nullptr;
            nid.color_formats[0] = rhi::Format::kBgra8Unorm;
            nid.color_count = 1;
            nid.debug_name = "unlit_offscreen_noid";
            unlit_offscreen_noid_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, nid);
            if (unlit_offscreen_noid_.IsNull()) {
                std::exit(0);
            }

            // Lit variants: same targets/layout + the world-normal attribute
            // the lit vertex shader consumes (offset 32 in the 64B attr
            // stream; unlit descriptors stay 2-attr so their PSOs are
            // byte-identical to before).
            const rhi::VertexInputAttribute lit_attrs[3] = {
                {0, cairns::kMeshPosBindSlot, rhi::Format::kRgba32F, 0},
                {1, cairns::kMeshAttrVertexBindSlot, rhi::Format::kRg32F, 48},
                {2, cairns::kMeshAttrVertexBindSlot, rhi::Format::kRgba32F,
                 32},
            };
            rhi::GraphicsPipelineDesc lof = ofd;
            lof.logical_shader = "lit_offscreen";
            lof.debug_name = "lit_offscreen";
            lof.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(lit_attrs, 3);
            lit_offscreen_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, lof);
            if (lit_offscreen_.IsNull()) {
                std::exit(0);
            }
            rhi::GraphicsPipelineDesc lnd = nid;
            lnd.logical_shader = "lit_offscreen_noid";
            lnd.debug_name = "lit_offscreen_noid";
            lnd.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(lit_attrs, 3);
            lit_offscreen_noid_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, lnd);
            if (lit_offscreen_noid_.IsNull()) {
                std::exit(0);
            }

            // Shadow machinery: 2048^2 D32F persistent target (fixed size,
            // no resize coupling), nearest non-filtering sampler (webgpu
            // depth-sample constraint; PCF is manual taps), slot-3 bind
            // group, depth-only PSO (position stream only; zero color
            // attachments).
            {
                rhi::TextureDesc sd{};
                sd.debug_name = "shadow_target";
                sd.dimensions = {2048, 2048, 1};
                sd.format = rhi::Format::kD32F;
                sd.usage = rhi::kTexUsageDepthTarget | rhi::kTexUsageSampled;
                sd.memory = rhi::Memory::kDefault;
                shadow_target_ = rhi_.resources.CreateTexture(rhi_.alloc, sd);
                rhi::SamplerDesc smd{};
                smd.min_filter = rhi::Filter::kNearest;
                smd.mag_filter = rhi::Filter::kNearest;
                // webgpu: ALL three filters must be nearest or the sampler
                // counts as filtering and rejects the depth layout.
                smd.mip_filter = rhi::Filter::kNearest;
                shadow_sampler_ = rhi_.resources.CreateSampler(smd);
                if (!shadow_target_.IsNull() && !shadow_sampler_.IsNull()) {
                    const rhi::TextureBinding tb{0, shadow_target_};
                    const rhi::SamplerBinding sb{0, shadow_sampler_};
                    rhi::BindGroupDesc bgd{};
                    bgd.textures =
                        std::span<const rhi::TextureBinding>(&tb, 1);
                    bgd.samplers =
                        std::span<const rhi::SamplerBinding>(&sb, 1);
                    bgd.depth_sample = true;
                    shadow_bind_group_ = rhi_.resources.CreateBindGroup(bgd);
                }
                const rhi::VertexInputAttribute shadow_attrs[1] = {
                    {0, cairns::kMeshPosBindSlot, rhi::Format::kRgba32F, 0},
                };
                const rhi::VertexBufferLayout shadow_layouts[1] = {
                    {cairns::kMeshPosBindSlot,
                     static_cast<uint32_t>(sizeof(glm::vec4))},
                };
                rhi::GraphicsPipelineDesc spd{};
                spd.logical_shader = "shadow_depth";
                spd.shader_dir = shader_dir.c_str();
                spd.debug_name = "shadow_depth";
                spd.vertex_attributes =
                    std::span<const rhi::VertexInputAttribute>(shadow_attrs,
                                                               1);
                spd.vertex_buffers =
                    std::span<const rhi::VertexBufferLayout>(shadow_layouts,
                                                             1);
                spd.topology = rhi::PrimitiveTopology::kTriangleList;
                spd.cull = rhi::CullMode::kBack;
                spd.front_face = rhi::FrontFace::kCounterClockwise;
                spd.depth_test = true;
                spd.depth_write = true;
                spd.depth_compare = rhi::CompareOp::kLess;
                spd.color_format = rhi::Format::kUndefined;
                spd.color_count = 0;
                spd.depth_format = rhi::Format::kD32F;
                spd.sample_count = 1;
                spd.swap_chain = nullptr;
                spd.push_constant_bytes = 0;
                shadow_pso_ = rhi_.pipelines.CreateGraphicsPipeline(
                    rhi_.resources, rhi_.frames, spd);
#if CAIRNS_WEBGPU
                // webgpu Classify stubs shadow_depth: CreateGraphicsPipeline
                // returns a REAL handle with a null internal PSO (never
                // IsNull). Force it Null so every downstream
                // !shadow_pso_.IsNull() gate disables shadows on webgpu (no
                // depth-only pipeline builder yet), honestly.
                shadow_pso_ = ShaderHandle::Null;
#endif
                if (shadow_pso_.IsNull()) {
                    CAIRNS_PRINT_ERR(
                        "[shadow] shadow_depth PSO unavailable -- shadow pass "
                        "disabled on this backend\n");
                }
            }

            // composite_pip: full-screen tri, samples 1 color tex, writes the
            // swap target. Surfaceless: present_.final_target is 1-sample / no-depth,
            // so the pipeline is compat with a 1-sample / no-depth renderpass.
            // Windowed: swapchain renderpass is MSAA + depth attachments.
            const bool surfaceless_pipe = !present_.final_target.IsNull();
            rhi::GraphicsPipelineDesc cpd{};
            cpd.logical_shader = "composite_pip";
            cpd.shader_dir = shader_dir.c_str();
            cpd.topology = rhi::PrimitiveTopology::kTriangleList;
            cpd.cull = rhi::CullMode::kNone;
            cpd.depth_test = false;
            cpd.depth_write = false;
            cpd.color_format = rhi::Format::kBgra8Unorm;
            cpd.depth_format = surfaceless_pipe ? rhi::Format::kUndefined
                                                : rhi::Format::kD32F;
            cpd.sample_count = surfaceless_pipe ? 1u : sampleCount;
            cpd.debug_name = "composite_pip";
            cpd.swap_chain = surfaceless_pipe ? nullptr : &present_.swapchain;
            composite_pip_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, cpd);

            // depthviz: same shape, samples 1 depth tex, blue silhouette.
            rhi::GraphicsPipelineDesc dvd = cpd;
            dvd.logical_shader = "depthviz";
            dvd.debug_name = "depthviz";
            depthviz_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, dvd);

            // Outline: same shape; samples color_off + id_off (2 textures
            // via the shared composite descriptor layout), renders into
            // outline_off (BGRA, same dims as color_off). Surfaceless: targets
            // a 1-sample / no-depth render pass. Windowed: still 1-sample /
            // no-depth because the outline pass writes to a graph color
            // target, NOT directly to the MSAA swapchain renderpass.
            rhi::GraphicsPipelineDesc opd = cpd;
            opd.logical_shader = "outline";
            opd.debug_name = "outline";
            opd.depth_format = rhi::Format::kUndefined;
            opd.sample_count = 1u;
            opd.swap_chain = nullptr;
            outline_pip_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, opd);

            // Kuwahara triplet: outline's shape (1-sample, no depth, graph
            // color target). tensor/tfm write RGBA16F intermediates; filter
            // writes BGRA like the forward color it replaces. Null = chain
            // disabled honestly (shadow_pso_ pattern).
            rhi::GraphicsPipelineDesc ktd = opd;
            ktd.logical_shader = "kuwahara_tensor";
            ktd.debug_name = "kuwahara_tensor";
            ktd.color_format = rhi::Format::kRgba16F;
            kuwahara_tensor_pip_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, ktd);
            rhi::GraphicsPipelineDesc kfd = ktd;
            kfd.logical_shader = "kuwahara_tfm";
            kfd.debug_name = "kuwahara_tfm";
            kuwahara_tfm_pip_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, kfd);
            rhi::GraphicsPipelineDesc kld = opd;
            kld.logical_shader = "kuwahara_filter";
            kld.debug_name = "kuwahara_filter";
            kuwahara_filter_pip_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, kld);
            if (kuwahara_tensor_pip_.IsNull() || kuwahara_tfm_pip_.IsNull() ||
                kuwahara_filter_pip_.IsNull()) {
                CAIRNS_PRINT_ERR(
                    "[postfx] kuwahara PSOs unavailable -- post-effect chain "
                    "disabled on this backend\n");
                kuwahara_tensor_pip_ = ShaderHandle::Null;
                kuwahara_tfm_pip_ = ShaderHandle::Null;
                kuwahara_filter_pip_ = ShaderHandle::Null;
            }

            if (composite_pip_.IsNull() || depthviz_.IsNull() ||
                outline_pip_.IsNull()) {
                std::exit(0);
            }

            rhi::SamplerDesc sd{};
            sd.min_filter = rhi::Filter::kLinear;
            sd.mag_filter = rhi::Filter::kLinear;
            sd.mip_filter = rhi::Filter::kLinear;
            sd.address_mode = rhi::AddressMode::kClampToEdge;
            composite_sampler_ = rhi_.resources.CreateSampler(sd);

            rhi::SamplerDesc nsd{};
            nsd.min_filter = rhi::Filter::kNearest;
            nsd.mag_filter = rhi::Filter::kNearest;
            nsd.mip_filter = rhi::Filter::kNearest;
            nsd.address_mode = rhi::AddressMode::kClampToEdge;
            outline_sampler_ = rhi_.resources.CreateSampler(nsd);
        }

        return true;
    }

}  // namespace cairns

namespace cairns {

void Engine::RecordFrame(FramePacket& pkt) {
        [[maybe_unused]] cairns::TaskGuard task_guard;

        PerSlot& s = slots_[pkt.slot];
        // Render-thread RAII slot lock. Main unlocked the slot's mutex
        // before Submit so this lock_guard takes ownership cleanly.
        // Released on scope exit (after EndSubmit + PresentPacket push).
        std::lock_guard<std::mutex> slot_lock(s.slot_mutex);

        // Publish parity early -- a pure function of pkt fields (no GPU
        // dependency) so the game thread's parity_cv wait clears immediately.
        // With N steps per frame, parity_out = parity_in ^ (N & 1).
        pkt.particle_parity_out =
            pkt.particle_parity_in ^ (pkt.sim_steps_this_frame & 1u);
        {
            std::lock_guard<std::mutex> lk(particles_.parity_m);
            particles_.latest_parity_out = pkt.particle_parity_out;
            particles_.latest_parity_frame = pkt.frame_idx;
        }
        particles_.parity_cv.notify_all();

        if (pkt.request_dump) {
            rhi_.frame_capture.SetDumpPath(pkt.dump_path);
        }

        // Engine -- not the RHI -- picks the per-frame swap target. Windowed:
        // pull the next drawable from the SwapChain. Surfaceless: hand Frames
        // the engine-owned offscreen, with no drawable so it doesn't present.
        rhi::SwapResolveTarget swap_target = AcquireFrameSwapTarget();
        rhi::FrameContext fc = rhi_.frames.Begin(
            rhi_.resources, rhi_.alloc, rhi_.gpu_profiler,
            rhi_.offscreen_targets, swap_target);
        if (fc.skip_frame) {
            std::lock_guard<std::mutex> lk(present_.m);
            s.present_fc = fc;
            s.present_target = swap_target;
            s.present_ready = true;
            present_.cv.notify_all();
            return;
        }

        cairns::Timer t_record("record", 2);
        EncodeDraws(pkt);

        if (!graph_) {
            graph_ = std::make_unique<rhi::RenderGraph>(rhi_.resources, rhi_.alloc);
            // Wire per-slot scratch arenas into the graph's slot table:
            // Bake(slot) resolves slot_arenas_[slot] -> this slot's BumpArena.
            for (uint32_t s = 0; s < kFramesInFlight; ++s) {
                graph_->BindSlotArena(s, slots_[s].arena);
            }
            // Golden mode disables intra-frame transient aliasing: aliased
            // memory is undefined until written, so any read-before-write
            // would be bistable and poison golden hashes. No current graph
            // aliases; hardening for when multi-pass transient graphs grow.
            graph_->SetDisableTransientAliasing(golden_);
        }
        graph_->Reset();

        // Per-viewport MeshDrawList: same draws, distinct globals_offset.
        // Id MRT only when something consumes it (outline
        // overlay or a pending pick this frame). Default path uses the
        // no-id PSO and a single-color forward render pass, saving the
        // R32U store + flat-interp on every visible fragment.
        const bool id_path = !picking_.highlights.empty() || picking_.pending;
        const rhi::Handle<rhi::Shader> forward_pso =
            id_path ? unlit_offscreen_ : unlit_offscreen_noid_;
        std::array<rhi::MeshDrawList, kNumViewports> mls{};
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            // draws stays the FULL list (sorted_draws holds global indices
            // into it); sorted_draws is the sub-span for this viewport's
            // bound scene, so each viewport renders only its own scene's
            // content.
            mls[v].draws = pkt.draws;
            const int sk = s.viewport_scene_idx[v];
            if (sk >= 0 && static_cast<uint32_t>(sk) < s.scene_ranges_count) {
                const PerSlot::SceneDrawRange& r = s.scene_ranges[sk];
                mls[v].sorted_draws =
                    pkt.sorted.subspan(r.draw_lo, r.draw_hi - r.draw_lo);
            } else {
                mls[v].sorted_draws = pkt.sorted;
            }
            mls[v].pipeline = forward_pso;
            // Set 0 (pass globals) routes through dyn_globals_.
            mls[v].dyn_globals = dyn_globals_;
            mls[v].globals_offset = s.globals_offset[v];
            mls[v].resident_textures = pkt.resident_textures;
            mls[v].resident_buffers = {};  // never read by the recorder
        }

        rhi::PointDraw pd{};
        // Particle PSO must match the forward pass's attachment count --
        // mirror the id_path PSO swap for the points pipeline.
        pd.pipeline = id_path ? particles_.render_offscreen
                              : particles_.render_offscreen_noid;
        pd.vertex_buffer = particles_.ssbo[pkt.particle_parity_out];
        pd.vertex_offset = 0;
        pd.vertex_count = ParticleSystem::kParticleCount;

        const float clear[4] = {41.0f / 255.0f, 42.0f / 255.0f, 48.0f / 255.0f, 1.0f};
        const uint32_t fb_w = swap_target.width;
        const uint32_t fb_h = swap_target.height;
        // Uniform horizontal split across the active viewports; layout_rect
        // owns each viewport's region.
        const int n_live = std::max(1, viewport_mgr_.active_count);
        const uint32_t vp_w = fb_w / static_cast<uint32_t>(n_live);
        const uint32_t vp_h = fb_h;
        // Id targets only allocated when this frame writes them (outline
        // overlay or pending pick).
        if (!picking_.highlights.empty() || picking_.pending) {
            EnsureIdTargets(vp_w, vp_h);
        }
        EnsureHighlightsTex();

        // Pre-skin compute pass, BEFORE particle_sim so its output ssbo is
        // ready when the forward pass binds stream 0 as a vertex stream
        // (free vertex-fetch sync via the existing compute -> graphics
        // semaphore @ VERTEX_INPUT on Vulkan; encoder boundary handles it
        // on Metal). Gated on non-empty batches AND a valid skin kernel --
        // absent either, the pass is omitted and the static path is
        // bit-for-bit unchanged.
        // anim_eval is its own graph pass so the palette edge into the skin
        // pass is ordered + barriered by the graph, not by an ad-hoc fence.
        if (!pkt.skin_batches.empty() && !pkt.actor_records.empty() &&
            !skinning_.eval_kernel.IsNull() && skinning_.eval_tables_uploaded &&
            !skinning_.palette_out_buf.IsNull()) {
            graph_->AddPass(
                "anim_eval", rhi::PassType::kCompute,
                [&](rhi::PassBuilder& b) {
                    rhi::GraphBufferDesc bd{};
                    bd.usage = rhi::kUsageStorage;
                    rhi::GraphBuffer pal =
                        b.ImportBuffer(skinning_.palette_out_buf, bd);
                    b.WriteBuffer(pal);
                },
                [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                    const uint32_t n_actors =
                        static_cast<uint32_t>(pkt.actor_records.size());
                    // BuildSkinFrame clamps; this catches any caller that
                    // skips the clamp.
                    assert(n_actors <= kAnimActorsCap);
                    const uint32_t ubo_align = rhi_.alloc.UboAlign();
                    const uint32_t records_bytes = n_actors *
                        static_cast<uint32_t>(sizeof(cairns::GpuActorRecord));
                    uint32_t records_off = 0;
                    void* records_ptr = rhi_.alloc.BumpAllocate(
                        records_bytes, ubo_align,
                        rhi::Memory::kDynamic, &records_off);
                    if (records_ptr == nullptr) {
                        return;
                    }
                    memcpy(records_ptr, pkt.actor_records.data(),
                           records_bytes);
                    rhi::CommandRecorder::AnimEvalArgs ae{};
                    ae.i32_buf = skinning_.ae_i32_buf;
                    ae.vec4_buf = skinning_.ae_vec4_buf;
                    ae.word16_buf = skinning_.ae_word16_buf;
                    ae.scene_headers = skinning_.scene_headers_buf;
                    ae.world_scratch = skinning_.world_scratch_buf;
                    ae.palette_out = skinning_.palette_out_buf;
                    // dyn_set_0 = per-FIF DynamicBuffers set (binding 0 dyn
                    // UBO + 1..6 SSBO over backing). vk reads it; metal
                    // ignores it.
                    ae.dyn_set_0 = skinning_.dyn_anim_eval;
                    ae.records_byte_offset = records_off;
                    ae.actor_count = n_actors;
                    cmd.DispatchAnimEval(rhi_.resources, rhi_.alloc,
                                         skinning_.eval_kernel, ae);
                });
        }
        if (!pkt.skin_batches.empty() && !skinning_.skin_kernel.IsNull() &&
            !skinning_.output_pool_buffer.IsNull()) {
            graph_->AddPass(
                "skinning_compute", rhi::PassType::kCompute,
                [&](rhi::PassBuilder& b) {
                    rhi::GraphBufferDesc bd{};
                    bd.usage = rhi::kUsageStorage;
                    rhi::GraphBuffer pool =
                        b.ImportBuffer(skinning_.output_pool_buffer, bd);
                    b.WriteBuffer(pool);
                    if (!skinning_.palette_out_buf.IsNull()) {
                        rhi::GraphBuffer pal =
                            b.ImportBuffer(skinning_.palette_out_buf, bd);
                        b.ReadBuffer(pal);
                    }
                },
                [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                    // SkinDispatchBatch reads palettes from binding 1 pointing
                    // at skinning_.palette_out_buf (filled by the anim_eval
                    // pass), per-batch dynamic offset =
                    // batch.first_palette_mat4 * sizeof(mat4).
                    PerSlot& s2 = slots_[pkt.slot];
                    const uint32_t n_batches =
                        static_cast<uint32_t>(pkt.skin_batches.size());
                    if (n_batches == 0) {
                        return;
                    }
                    const uint32_t ubo_align = rhi_.alloc.UboAlign();
                    const uint32_t ssbo_align = rhi_.alloc.StorageAlign();
                    rhi::SkinDispatchBatch* dbatches =
                        s2.arena.AllocateArray<rhi::SkinDispatchBatch>(
                            n_batches);
                    for (uint32_t bi = 0; bi < n_batches; ++bi) {
                        const cairns::SkinBatchGpu& sbg =
                            pkt.skin_batches[bi];
                        const cairns::Mesh::Hot* mhot =
                            prefab_store_.meshes.GetHot(sbg.mesh);
                        rhi::SkinDispatchBatch& db = dbatches[bi];
                        db = rhi::SkinDispatchBatch{};
                        const rhi::Handle<rhi::Buffer> mesh_skin_buf =
                            mhot ? ResolvedSharedSkin(*mhot)
                                  : rhi::Handle<rhi::Buffer>::Null;
                        if (!mhot ||
                            mhot->posHandle.IsNull() ||
                            mesh_skin_buf.IsNull()) {
                            continue;
                        }
                        struct SkinParamsCpu {
                            uint32_t instance_count;
                            uint32_t vertex_count;
                            uint32_t joint_count;
                            uint32_t mode;
                        };
                        SkinParamsCpu params{};
                        params.instance_count = sbg.instance_count;
                        params.vertex_count = sbg.vertex_count;
                        params.joint_count = sbg.joint_count;
                        params.mode = 0u;
                        uint32_t params_off = 0;
                        void* params_ptr = rhi_.alloc.BumpAllocate(
                            sizeof(SkinParamsCpu), ubo_align,
                            rhi::Memory::kDynamic, &params_off);
                        if (!params_ptr) {
                            continue;
                        }
                        memcpy(params_ptr, &params, sizeof(SkinParamsCpu));

                        // Palettes live in skinning_.palette_out_buf; the
                        // per-batch dynamic offset selects the bucket window
                        // in mat4 stride. instance_meta.x stays
                        // bucket-relative (cursor * joint_count).
                        const uint32_t pal_off =
                            sbg.first_palette_mat4 *
                            static_cast<uint32_t>(sizeof(glm::mat4));
                        // InstanceMeta window for this batch.
                        const uint32_t meta_start = sbg.first_meta;
                        const uint32_t meta_count = sbg.instance_count;
                        const uint32_t meta_bytes = meta_count *
                            static_cast<uint32_t>(sizeof(glm::uvec2));
                        uint32_t meta_off = 0;
                        void* meta_ptr = rhi_.alloc.BumpAllocate(
                            meta_bytes ? meta_bytes : 8u, ssbo_align,
                            rhi::Memory::kDynamic, &meta_off);
                        if (meta_bytes > 0 && meta_ptr) {
                            memcpy(meta_ptr,
                                   pkt.instance_meta.data() + meta_start,
                                   meta_bytes);
                        }

                        // Group A descriptor set (Vulkan); null on Metal.
                        db.mesh_set = mhot->skin_group_a;
                        db.pos_buffer = mhot->posHandle;
                        db.pos_byte_offset =
                            mhot->global_base_vertex *
                            static_cast<uint32_t>(sizeof(glm::vec4));
                        db.skin_attr_buffer = mesh_skin_buf;
                        db.skin_attr_byte_offset =
                            mhot->skin_attr_base_vertex *
                            static_cast<uint32_t>(
                                sizeof(cairns::PackedSkinVertex));
                        db.params_byte_offset = params_off;
                        db.palettes_byte_offset = pal_off;
                        db.instance_meta_byte_offset = meta_off;
                        db.workgroups = sbg.workgroups;
                        db.instance_count = sbg.instance_count;
                    }
                    // skinning_.dyn_skin_group_b owns the per-FIF set (vk);
                    // metal ignores it.
                    cmd.DispatchSkinBatches(
                        rhi_.resources, rhi_.alloc, skinning_.skin_kernel,
                        skinning_.output_pool_buffer, skinning_.palette_out_buf,
                        skinning_.dyn_skin_group_b,
                        std::span<const rhi::SkinDispatchBatch>(
                            dbatches, n_batches));
                });
        }

        // pass 1: particle_sim kCompute. Import the writer ssbo so prune
        // keeps it (external side effect -- game thread reads
        // particle_parity_out). Emitter-component presence gates the sim:
        // no emitter -> pass omitted, sim_out stays default-null, prune
        // drops it. Computed once; the draw pass reuses the gate.
        const bool particles_active = AnyBoundSceneHasEmitter();
        rhi::GraphBuffer sim_out;
        if (particles_active) {
        graph_->AddPass(
            "particle_sim", rhi::PassType::kCompute,
            [&](rhi::PassBuilder& b) {
                rhi::GraphBufferDesc bd{};
                bd.usage = rhi::kUsageStorage;
                sim_out = b.ImportBuffer(
                    particles_.ssbo[pkt.particle_parity_out], bd);
                b.WriteBuffer(sim_out);
                rhi::GraphBuffer sim_in = b.ImportBuffer(
                    particles_.ssbo[pkt.particle_parity_in], bd);
                b.ReadBuffer(sim_in);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                rhi::ComputeDispatch cd{};
                cd.kernel = particles_.kernel;
                cd.groups_x = ParticleSystem::kParticleCount / 256;
                cd.local_x = 256;
                // Metal: sequential ComputeCommandEncoders self-hazard on R/W
                // ordering (MTLHazardTrackingModeTracked). Vulkan: recorder
                // emits a compute->compute pipeline barrier between dispatches.
                for (uint32_t k = 0; k < pkt.sim_steps_this_frame; ++k) {
                    const uint32_t step_src =
                        pkt.particle_parity_in ^ (k & 1u);
                    // Parity DynamicBuffers holds bindings 1+2 pre-bound;
                    // the dyn offset carries dt only.
                    cd.dyn_set_0 = dyn_particle_parity_[step_src];
                    cd.dyn_offset_0 = s.dt_off;
                    cd.step_index = k;
                    cmd.Dispatch(rhi_.resources, rhi_.alloc, cd);
                }
            });
        }  // particles_active (emitter gate)

        // pass 1.5: shadow map. ONE depth pass from the active scene's light
        // POV, BEFORE forward. Gated on a cast_shadows light + a live PSO
        // (webgpu stubs shadow_pso_ null -> pass absent there, honest until
        // its depth-only builder lands). Renders at the fixed 2048^2 shadow
        // extent; lit fragments sample it via the slot-3 bind group.
        if (s.shadow_active && !shadow_pso_.IsNull() &&
            !s.shadowDrawList.empty()) {
            graph_->AddPass(
                "shadow_vp0", rhi::PassType::kGraphics,
                [&](rhi::PassBuilder& b) {
                    rhi::GraphTextureDesc sdd{};
                    sdd.width = 2048;
                    sdd.height = 2048;
                    sdd.format = rhi::Format::kD32F;
                    sdd.usage = rhi::kTexUsageDepthTarget | rhi::kTexUsageSampled;
                    rhi::GraphTexture sg = b.ImportTexture(shadow_target_, sdd);
                    b.AddDepthOutput("shadow_depth", sg, rhi::LoadOp::kClear,
                                     1.0f);
                    b.SetRenderExtent(2048, 2048);
                    // Skinned casters: shadow vertex fetch reads the skin pool
                    // the skinning pass wrote -> declare it so the graph orders
                    // + barriers the edge.
                    if (!pkt.skin_batches.empty() &&
                        !skinning_.output_pool_buffer.IsNull()) {
                        rhi::GraphBufferDesc sbd{};
                        sbd.usage = rhi::kUsageStorage;
                        rhi::GraphBuffer pool = b.ImportBuffer(
                            skinning_.output_pool_buffer, sbd);
                        b.ReadBuffer(pool);
                    }
                },
                [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                    rhi::MeshDrawList sdl{};
                    sdl.draws = std::span<const cairns::Draw>(
                        s.shadowDrawList.data(), s.shadowDrawList.size());
                    sdl.sorted_draws = pkt.sorted;  // all casters, all scenes
                    sdl.pipeline = shadow_pso_;
                    sdl.dyn_globals = dyn_globals_;
                    sdl.globals_offset = s.shadow_globals_offset;
                    cmd.DrawMeshes(rhi_.resources, rhi_.alloc, sdl);
                });
        }

        // pass 2: forward, ONCE PER VIEWPORT. Each pass writes to a private
        // half-width color+depth target. Particles render into both viewports
        // (compute step ran once above; particle render is a graphics
        // submission that draws into each forward pass's encoder).
        std::array<rhi::GraphTexture, kNumViewports> color_off{};
        std::array<rhi::GraphTexture, kNumViewports> depth_off{};
        std::array<rhi::GraphTexture, kNumViewports> id_off{};
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            const int vp_idx = v;
            const char* pass_name = (vp_idx == 0) ? "forward_vp0" : "forward_vp1";
            graph_->AddPass(
                pass_name, rhi::PassType::kGraphics,
                [&, vp_idx](rhi::PassBuilder& b) {
                    rhi::GraphTextureDesc cd{};
                    cd.width = vp_w;
                    cd.height = vp_h;
                    cd.format = rhi::Format::kBgra8Unorm;
                    cd.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled;
                    color_off[vp_idx] = b.CreateColorTarget(cd);
                    rhi::GraphTextureDesc dd{};
                    dd.width = vp_w;
                    dd.height = vp_h;
                    dd.format = rhi::Format::kD32F;
                    dd.usage = rhi::kTexUsageDepthTarget | rhi::kTexUsageSampled;
                    depth_off[vp_idx] = b.CreateDepthTarget(dd);
                    b.AddColorOutput("color", color_off[vp_idx], rhi::LoadOp::kClear, clear);
                    if (id_path) {
                        // R32U id buffer (MRT). Persistent (engine-owned via
                        // id_target_[vp]) so end-of-frame pick can copyImageToBuffer
                        // a 1x1 region after the render thread drains. Importing
                        // skips the transient pool aliasing race that would
                        // otherwise reuse the texture before readback. Only
                        // attached when outline or pick wants it -- frees
                        // the per-frag R32U store otherwise.
                        rhi::GraphTextureDesc id_desc{};
                        id_desc.width = vp_w;
                        id_desc.height = vp_h;
                        id_desc.format = rhi::Format::kR32Uint;
                        id_desc.usage = rhi::kTexUsageColorTarget |
                                         rhi::kTexUsageSampled |
                                         rhi::kTexUsageTransferSrc;
                        id_off[vp_idx] = b.ImportTexture(id_target_[vp_idx], id_desc);
                        const float id_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        b.AddColorOutput("id", id_off[vp_idx], rhi::LoadOp::kClear, id_clear);
                    }
                    b.AddDepthOutput("fwd_depth", depth_off[vp_idx], rhi::LoadOp::kClear, 1.0f);
                    // Vertex-fetched SSBO reads (skin pool as the stream-0
                    // alias; particle SSBO for DrawPoints) -- declared so the
                    // graph orders + barriers compute -> vertex fetch.
                    rhi::GraphBufferDesc sbd{};
                    sbd.usage = rhi::kUsageStorage;
                    if (!pkt.skin_batches.empty() &&
                        !skinning_.output_pool_buffer.IsNull()) {
                        rhi::GraphBuffer pool = b.ImportBuffer(
                            skinning_.output_pool_buffer, sbd);
                        b.ReadBuffer(pool);
                    }
                    if (particles_active) {
                        rhi::GraphBuffer psb = b.ImportBuffer(
                            particles_.ssbo[pkt.particle_parity_out], sbd);
                        b.ReadBuffer(psb);
                    }
                },
                [&, vp_idx](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                    cmd.DrawMeshes(rhi_.resources, rhi_.alloc, mls[vp_idx]);
                    // Emitter presence gates the compute sim above;
                    // per-viewport Viewport::Cold::particles_enabled gates
                    // each viewport's particle draw.
                    bool vp_particles = true;
                    if (auto* vc = viewport_mgr_.pool.GetCold(viewport_mgr_.ids[vp_idx])) {
                        vp_particles = vc->particles_enabled;
                    }
                    if (particles_active && vp_particles) {
                        cmd.DrawPoints(rhi_.resources, rhi_.alloc, pd);
                    }
                });
        }

        // pass 2.5: outline. Per viewport, fullscreen tri samples color_off
        // + id_off, edge-detects on the id channel, tints on highlight
        // match. Only inserted when the engine carries highlights, so the
        // default cost is zero.
        std::array<rhi::GraphTexture, kNumViewports> outline_off{};
        // Per-viewport "did the outline pass run": the chrome gate is
        // per-viewport, so the swap read can't use one global flag.
        std::array<bool, kNumViewports> outline_ran{};
        // The selection outline IS editor chrome -- meta-UI marking "this
        // entity is selected in the editor". Stylized highlight (rim/toon/
        // ink) is in-canvas art in the material path, unaffected by this
        // gate; cairns.editor.chrome({on:false}) drops the outline before a
        // capture or scroll. Selection STATE (picking_.highlights) is
        // global and preserved -- each viewport draws the outline only when
        // its own Viewport::Cold::chrome_enabled is on.
        const bool have_highlights = !picking_.highlights.empty();
        if (have_highlights) {
            for (int v = 0; v < viewport_mgr_.active_count; ++v) {
                const int vp_idx = v;
                const cairns::Viewport::Cold* vp_cold =
                    viewport_mgr_.pool.GetCold(viewport_mgr_.ids[vp_idx]);
                if (!vp_cold || !vp_cold->chrome_enabled) {
                    continue;
                }
                outline_ran[vp_idx] = true;
                const char* pass_name =
                    (vp_idx == 0) ? "outline_vp0" : "outline_vp1";
                graph_->AddPass(
                    pass_name, rhi::PassType::kGraphics,
                    [&, vp_idx](rhi::PassBuilder& b) {
                        rhi::GraphTextureDesc od{};
                        od.width = vp_w;
                        od.height = vp_h;
                        od.format = rhi::Format::kBgra8Unorm;
                        od.usage = rhi::kTexUsageColorTarget |
                                   rhi::kTexUsageSampled;
                        outline_off[vp_idx] = b.CreateColorTarget(od);
                        b.AddAttachmentInput(color_off[vp_idx]);
                        b.AddAttachmentInput(id_off[vp_idx]);
                        // Import picking_.highlights_tex as a graph input so its
                        // SHADER_READ_ONLY layout transition is emitted by
                        // BeginRenderPass before DrawFullscreen samples it.
                        rhi::GraphTextureDesc hd{};
                        hd.width = kMaxHighlights + 1;
                        hd.height = 1;
                        hd.format = rhi::Format::kR32Uint;
                        hd.usage = rhi::kTexUsageSampled;
                        rhi::GraphTexture hg =
                            b.ImportTexture(picking_.highlights_tex, hd);
                        b.AddAttachmentInput(hg);
                        const float oclear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        b.AddColorOutput("outline_color", outline_off[vp_idx],
                                         rhi::LoadOp::kClear, oclear);
                    },
                    [&, vp_idx](rhi::CommandRecorder& cmd,
                                const rhi::PassResources& res) {
                        const rhi::Handle<rhi::Texture> srcs[3] = {
                            res.Resolve(color_off[vp_idx]),
                            res.Resolve(id_off[vp_idx]),
                            picking_.highlights_tex,
                        };
                        cmd.SetViewport(0.0f, 0.0f, static_cast<float>(vp_w),
                                        static_cast<float>(vp_h));
                        cmd.SetScissor(0, 0, vp_w, vp_h);
                        cmd.DrawFullscreen(
                            rhi_.resources, outline_pip_,
                            std::span<const rhi::Handle<rhi::Texture>>(srcs, 3),
                            outline_sampler_);
                    });
            }
        }

        // pass 2.75: post-effect chain. Extracted PostEffect components
        // append fullscreen passes per viewport (chain input = the outline
        // output when that ran, so chrome sits under the effect); swap
        // samples the chain's final output. Setup lambdas run inside
        // AddPass, so `cur` threads pass N's output into pass N+1's inputs.
        struct PostFxParamsGpu {
            glm::vec4 screen;  // x = 1/w, y = 1/h, z = w, w = h
            glm::vec4 p0;
            glm::vec4 p1;
            glm::vec4 p2;
        };
        static_assert(sizeof(PostFxParamsGpu) == 64,
                      "postfx params block must match the 64B dyn-UBO range");
        std::array<rhi::GraphTexture, kNumViewports> chain_out{};
        std::array<bool, kNumViewports> chain_ran{};
        // Execute lambdas resolve these after the build loop ends -- they
        // must outlive it, hence arrays here and not loop locals.
        std::array<std::array<rhi::GraphTexture, PerSlot::kMaxPostEffects>,
                   kNumViewports> fx_tensor{};
        std::array<std::array<rhi::GraphTexture, PerSlot::kMaxPostEffects>,
                   kNumViewports> fx_tfm{};
        std::array<std::array<rhi::GraphTexture, PerSlot::kMaxPostEffects>,
                   kNumViewports> fx_out{};
        // Declared at pass-build scope (NOT inside the if): the execute
        // lambdas capture this by reference and run at graph Execute.
        auto push_params = [&](rhi::CommandRecorder& cmd,
                               uint32_t fx_idx,
                               rhi::Handle<rhi::Shader> pso,
                               std::span<const rhi::Handle<rhi::Texture>>
                                   texs) {
            PostFxParamsGpu pp{};
            pp.screen = glm::vec4(1.0f / static_cast<float>(vp_w),
                                  1.0f / static_cast<float>(vp_h),
                                  static_cast<float>(vp_w),
                                  static_cast<float>(vp_h));
            pp.p0 = s.post_effects[fx_idx].p0;
            pp.p1 = s.post_effects[fx_idx].p1;
            uint32_t off = 0;
            void* ptr = rhi_.alloc.BumpAllocate(
                sizeof(pp), 256, rhi::Memory::kDynamic, &off);
            if (!ptr) {
                return;
            }
            std::memcpy(ptr, &pp, sizeof(pp));
            cmd.SetViewport(0.0f, 0.0f, static_cast<float>(vp_w),
                            static_cast<float>(vp_h));
            cmd.SetScissor(0, 0, vp_w, vp_h);
            cmd.DrawFullscreenParams(rhi_.resources, rhi_.alloc, pso,
                                     texs, composite_sampler_,
                                     dyn_postfx_, off);
        };
        if (s.post_effect_count > 0 && !kuwahara_filter_pip_.IsNull()) {
            for (int v = 0; v < viewport_mgr_.active_count; ++v) {
                const int vp_idx = v;
                rhi::GraphTexture cur = outline_ran[vp_idx]
                                            ? outline_off[vp_idx]
                                            : color_off[vp_idx];
                for (uint32_t f = 0; f < s.post_effect_count; ++f) {
                    if (s.post_effects[f].type !=
                        cairns::PostEffectType::kKuwahara) {
                        continue;  // other types land with M4+.
                    }
                    const uint32_t fx_idx = f;
                    graph_->AddPass(
                        (vp_idx == 0) ? "kuwahara_tensor_vp0"
                                      : "kuwahara_tensor_vp1",
                        rhi::PassType::kGraphics,
                        [&, vp_idx, fx_idx, cur](rhi::PassBuilder& b) {
                            rhi::GraphTextureDesc td{};
                            td.width = vp_w;
                            td.height = vp_h;
                            td.format = rhi::Format::kRgba16F;
                            td.usage = rhi::kTexUsageColorTarget |
                                       rhi::kTexUsageSampled;
                            fx_tensor[vp_idx][fx_idx] = b.CreateColorTarget(td);
                            b.AddAttachmentInput(cur);
                            const float fc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                            b.AddColorOutput("kuw_tensor",
                                             fx_tensor[vp_idx][fx_idx],
                                             rhi::LoadOp::kClear, fc);
                        },
                        [&, fx_idx, cur](rhi::CommandRecorder& cmd,
                                         const rhi::PassResources& res) {
                            const rhi::Handle<rhi::Texture> srcs[1] = {
                                res.Resolve(cur)};
                            push_params(cmd, fx_idx, kuwahara_tensor_pip_,
                                        std::span<const rhi::Handle<
                                            rhi::Texture>>(srcs, 1));
                        });
                    graph_->AddPass(
                        (vp_idx == 0) ? "kuwahara_tfm_vp0"
                                      : "kuwahara_tfm_vp1",
                        rhi::PassType::kGraphics,
                        [&, vp_idx, fx_idx](rhi::PassBuilder& b) {
                            rhi::GraphTextureDesc td{};
                            td.width = vp_w;
                            td.height = vp_h;
                            td.format = rhi::Format::kRgba16F;
                            td.usage = rhi::kTexUsageColorTarget |
                                       rhi::kTexUsageSampled;
                            fx_tfm[vp_idx][fx_idx] = b.CreateColorTarget(td);
                            b.AddAttachmentInput(fx_tensor[vp_idx][fx_idx]);
                            const float fc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                            b.AddColorOutput("kuw_tfm",
                                             fx_tfm[vp_idx][fx_idx],
                                             rhi::LoadOp::kClear, fc);
                        },
                        [&, vp_idx, fx_idx](rhi::CommandRecorder& cmd,
                                            const rhi::PassResources& res) {
                            const rhi::Handle<rhi::Texture> srcs[1] = {
                                res.Resolve(fx_tensor[vp_idx][fx_idx])};
                            push_params(cmd, fx_idx, kuwahara_tfm_pip_,
                                        std::span<const rhi::Handle<
                                            rhi::Texture>>(srcs, 1));
                        });
                    graph_->AddPass(
                        (vp_idx == 0) ? "kuwahara_filter_vp0"
                                      : "kuwahara_filter_vp1",
                        rhi::PassType::kGraphics,
                        [&, vp_idx, fx_idx, cur](rhi::PassBuilder& b) {
                            rhi::GraphTextureDesc td{};
                            td.width = vp_w;
                            td.height = vp_h;
                            td.format = rhi::Format::kBgra8Unorm;
                            td.usage = rhi::kTexUsageColorTarget |
                                       rhi::kTexUsageSampled;
                            fx_out[vp_idx][fx_idx] = b.CreateColorTarget(td);
                            b.AddAttachmentInput(cur);
                            b.AddAttachmentInput(fx_tfm[vp_idx][fx_idx]);
                            const float fc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                            b.AddColorOutput("kuw_filter",
                                             fx_out[vp_idx][fx_idx],
                                             rhi::LoadOp::kClear, fc);
                        },
                        [&, vp_idx, fx_idx, cur](
                            rhi::CommandRecorder& cmd,
                            const rhi::PassResources& res) {
                            const rhi::Handle<rhi::Texture> srcs[2] = {
                                res.Resolve(cur),
                                res.Resolve(fx_tfm[vp_idx][fx_idx])};
                            push_params(cmd, fx_idx, kuwahara_filter_pip_,
                                        std::span<const rhi::Handle<
                                            rhi::Texture>>(srcs, 2));
                        });
                    cur = fx_out[vp_idx][fx_idx];
                    chain_out[vp_idx] = cur;
                    chain_ran[vp_idx] = true;
                }
            }
        }

        // pass 3: composite + ui kGraphics. Composite samples color_off full-
        // screen, depth_off PIP in the bottom-right; ImGui draws on top into the
        // same encoder. Both backends use MSAA swapchain renderpasses where the
        // MSAA color attachment storeOp is "resolve + don't-keep-MSAA" (Adreno
        // tile-residency optimization). Two back-to-back render passes targeting
        // the same swap framebuffer would either clear or load undefined MSAA
        // between passes, so we keep them under one encoder. The graph still
        // expresses the dependencies (this pass reads color_off + depth_off
        // produced by forward) -- "ui" is conceptually a separate phase that
        // physically shares the swap encoder for MSAA reasons.
        rhi::GraphTexture swap_tex;
        graph_->AddPass(
            "swap", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                rhi::GraphTextureDesc td{};
                td.width = fb_w;
                td.height = fb_h;
                // Surfaceless: import present_.final_target as the swap output so
                // BeginRenderPass routes through the offscreen-target-cache
                // (target.IsNull() == false). Windowed: import null and let
                // BeginRenderPass take the is_swapchain branch.
                const rhi::Handle<rhi::Texture> swap_handle =
                    present_.final_target.IsNull() ? rhi::Handle<rhi::Texture>::Null
                                            : present_.final_target;
                swap_tex = b.ImportTexture(swap_handle, td);
                b.AddColorOutput("swapchain", swap_tex, rhi::LoadOp::kClear, clear);
                for (int v = 0; v < viewport_mgr_.active_count; ++v) {
                    // Swap reads the post-effect chain's final output when
                    // it ran, else outline_off when that ran, else
                    // color_off. All sampled-readonly.
                    b.AddAttachmentInput(
                        chain_ran[v] ? chain_out[v]
                                     : (outline_ran[v] ? outline_off[v]
                                                       : color_off[v]));
                    b.AddAttachmentInput(depth_off[v]);
                }
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources& res) {
                std::array<rhi::Handle<rhi::Texture>, kNumViewports> vp_color{};
                std::array<rhi::Handle<rhi::Texture>, kNumViewports> vp_depth{};
                for (int v = 0; v < viewport_mgr_.active_count; ++v) {
                    vp_color[v] = res.Resolve(
                        chain_ran[v] ? chain_out[v]
                                     : (outline_ran[v] ? outline_off[v]
                                                       : color_off[v]));
                    vp_depth[v] = res.Resolve(depth_off[v]);
                }
                const float fb_fw = static_cast<float>(fb_w);
                const float fb_fh = static_cast<float>(fb_h);
                if (viewport_mgr_.composition_count > 0) {
                  // Draw exactly the installed composition panes. Each pane
                  // blits a source viewport's resolved color or depth into
                  // its NDC rect. The render.nestedGraph op installs the two
                  // canonical entries (color full + depth PIP); the shape is
                  // general (any viewport -> any rect).
                  for (uint8_t ci = 0; ci < viewport_mgr_.composition_count;
                       ++ci) {
                    const cairns::CompositionView& cv =
                        viewport_mgr_.composition[ci];
                    const uint32_t sv = cv.source_viewport;
                    if (sv >= static_cast<uint32_t>(
                                  viewport_mgr_.active_count)) {
                      continue;
                    }
                    const float rx = cv.rect_ndc.x * fb_fw;
                    const float ry = cv.rect_ndc.y * fb_fh;
                    const float rw = cv.rect_ndc.z * fb_fw;
                    const float rh = cv.rect_ndc.w * fb_fh;
                    cmd.SetViewport(rx, ry, rw, rh);
                    cmd.SetScissor(static_cast<int32_t>(rx),
                                   static_cast<int32_t>(ry),
                                   static_cast<uint32_t>(rw),
                                   static_cast<uint32_t>(rh));
                    if (cv.source ==
                        cairns::CompositionView::Source::kResolvedDepth) {
                      // Nearest sampler: depth textures reject filtering
                      // (WebGPU/Dawn); point-sampling depth is correct
                      // everywhere. Shared with the outline pass.
                      cmd.DrawFullscreen(
                          rhi_.resources, depthviz_,
                          std::span<const rhi::Handle<rhi::Texture>>(
                              &vp_depth[sv], 1),
                          outline_sampler_);
                    } else {
                      cmd.DrawFullscreen(
                          rhi_.resources, composite_pip_,
                          std::span<const rhi::Handle<rhi::Texture>>(
                              &vp_color[sv], 1),
                          composite_sampler_);
                    }
                  }
                } else {
                // Composite each LIVE viewport into its layout_rect region
                // of the swap pane. layout_rect = (x,y,w,h) in NDC [0..1].
                // Default for vp 0 is full pane (1,1); follow-up viewports
                // set their own rects via cairns.viewport.setLayout. Skip
                // zero-area rects (uninitialised / disabled).
                for (int v = 0; v < viewport_mgr_.active_count; ++v) {
                    const glm::vec4& rect =
                        viewport_mgr_.pool.GetHot(viewport_mgr_.ids[v])->layout_rect;
                    if (rect.z <= 0.0f || rect.w <= 0.0f) {
                        continue;
                    }
                    const float rx = rect.x * fb_fw;
                    const float ry = rect.y * fb_fh;
                    const float rw = rect.z * fb_fw;
                    const float rh = rect.w * fb_fh;
                    cmd.SetViewport(rx, ry, rw, rh);
                    cmd.SetScissor(static_cast<int32_t>(rx),
                                   static_cast<int32_t>(ry),
                                   static_cast<uint32_t>(rw),
                                   static_cast<uint32_t>(rh));
                    cmd.DrawFullscreen(rhi_.resources, composite_pip_,
                                       std::span<const rhi::Handle<rhi::Texture>>(&vp_color[v], 1),
                                       composite_sampler_);
                }
                // depthviz appears only via composition panes (above);
                // ordinary scenarios render clean color, no depth overlay.
                }  // end else (non-nested multi-viewport composite)
                // Restore full extent before the ui draw.
                cmd.SetViewport(0.0f, 0.0f, static_cast<float>(fb_w),
                                static_cast<float>(fb_h));
                cmd.SetScissor(0, 0, fb_w, fb_h);
                if (pkt.imgui_snapshot) {
                    cmd.DrawImGui(rhi_.resources, rhi_.alloc, imgui_, imgui_font_,
                                  imgui_sampler_, pkt.imgui_snapshot);
                }
            });

        graph_->SetOutput(swap_tex);
        if (!graph_->Bake(pkt.slot) || !graph_->Execute(fc, swap_target)) {
            t_record.End();
            rhi_.frames.EndSubmit(swap_target, rhi_.frame_capture, fc);
            {
                std::lock_guard<std::mutex> lk(present_.m);
                s.present_fc = fc;
                s.present_target = swap_target;
                s.present_ready = true;
            }
            present_.cv.notify_all();
            return;
        }
        if (frame_ <= 6) {
            fprintf(stderr, "[FLAKE-R] frame=%u slot=%u img=%u steps=%u",
                    frame_, pkt.slot, fc.swapchain_image_index,
                    pkt.sim_steps_this_frame);
            for (int v = 0; v < viewport_mgr_.active_count; ++v) {
                const rhi::Handle<rhi::Texture> coff =
                    graph_->ResolveTexture(color_off[v]);
                const rhi::Handle<rhi::Texture> doff =
                    graph_->ResolveTexture(depth_off[v]);
                fprintf(stderr, " vp%d_color=%u/%u vp%d_depth=%u/%u", v,
                        coff.index, coff.generation, v,
                        doff.index, doff.generation);
            }
            fprintf(stderr, "\n");
        }
        t_record.End();
        rhi_.frames.EndSubmit(swap_target, rhi_.frame_capture, fc);
        {
            std::lock_guard<std::mutex> lk(present_.m);
            s.present_fc = fc;
            s.present_target = swap_target;
            s.present_ready = true;
        }
        present_.cv.notify_all();
    }

}  // namespace cairns

namespace cairns {

bool Engine::draw() {
        // Steady-frame marker, throttled to once per 60 frames so the log
        // scanner can see "engine is in steady state" without drowning out
        // the [LOAD]/[RELOAD] markers. Pairs with the Instruments signpost
        // on this scope; the time profiler shows each frame as a 16ms band
        // under "Points of Interest".
        CAIRNS_SIGNPOST_INTERVAL_SCOPED("frame", "draw");
        if ((frame_ % 60) == 0) {
            size_t ec = 0;
            if (auto* wc = scene_mgr_.pool.GetCold(scene_mgr_.active)) {
                ec = wc->registry.storage<entt::entity>().size();
            }
            CAIRNS_PRINT_ERR("[STEADY] frame=%u entities=%zu prefabs=%zu\n",
                              frame_, ec, prefab_store_.prefab_ids.size());
#if CAIRNS_ALLOC_TRACE
            // Allocs over the trailing 60-frame steady window. First print is
            // the window since boot; read the later windows. Includes the
            // command-dispatch path (per-frame NDJSON parse), so this is
            // not a render-thread-only number.
            cairns::alloc_count::PrintDelta("[STEADY-60]", alloc_steady_prev_);
            alloc_steady_prev_ = cairns::alloc_count::Now();
#endif
        }
#if CAIRNS_VULKAN
        if (rhi_.frames.plat.recreate_pending_.load(std::memory_order_acquire)) {
            if (render_thread_) {
                render_thread_->Drain();
            }
            rhi_.device.WaitIdle();
            if (present_.final_target.IsNull()) {
                present_.swapchain.plat.RecreateSwapChain();
                rhi_.offscreen_targets.FlushFramebuffers();
            }
            rhi_.frames.plat.recreate_pending_.store(false, std::memory_order_release);
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                slots_[i].present_ready = false;
            }
            present_.prev_slot = -1;
            present_.queue.clear();
        }
#endif
        ApplyPendingResize();
        if (present_.final_target.IsNull() &&
            (present_.swapchain.Width() == 0 || present_.swapchain.Height() == 0)) {
            if (render_thread_) {
                render_thread_->Drain();
            }
            return false;
        }

        frame_++;
        const uint32_t slot = (frame_ - 1) % kFramesInFlight;

        // Acquire BEFORE touching slot storage -- this is the backpressure
        // gate, blocks if the render thread is still holding slot S.
        PerSlot& s = slots_[slot];
        // std::unique_lock FIRST. The mutex is the actual ownership
        // claim; render_thread_->Acquire then blocks on the state
        // machine. Lock-first means dtor runs cleanly via stack
        // unwinding even if anything between this point and Submit
        // throws.
        std::unique_lock<std::mutex> slot_lock(s.slot_mutex);
        render_thread_->Acquire(slot);
        // Reset this slot's CPU bump arena. Safe here because Acquire
        // blocked until the render thread finished its prior use of slot
        // S -- no reader still inside the bytes.
        s.arena.Reset();

        const uint64_t cpu_now_ns = cairns::timestamp_ns();
        if (cpu_last_frame_ns_ != 0) {
            cpu_ms_last_ = static_cast<float>(cpu_now_ns - cpu_last_frame_ns_) / 1.0e6f;
            cpu_ms_history_[cpu_ms_head_] = cpu_ms_last_;
            cpu_ms_head_ = (cpu_ms_head_ + 1) % kCpuMsHistory;
        }
        cpu_last_frame_ns_ = cpu_now_ns;
        s.pkt.request_dump = false;
        s.pkt.dump_path.clear();
        if (dump_and_exit_ && !present_.dump_emitted &&
            sim_frame_ >= cairns::kGoldenDumpFrame) {
            s.pkt.request_dump = true;
            s.pkt.dump_path = engine_cfg_.dump_path.empty()
                                   ? std::filesystem::path("/tmp/cairns_dump.png")
                                   : engine_cfg_.dump_path;
            present_.dump_emitted = true;
            present_.dump_emit_frame = frame_;
        }
        if (dump_and_exit_ && present_.dump_emitted &&
            frame_ >= present_.dump_emit_frame + 2) {
            std::exit(0);  // headless byte-gate: dump flushed, exit. Tests
                           // set use_fixed_clock without dump_path, so
                           // dump_and_exit_=false here and tests survive.
        }

        cairns::Timer t_frame("frame", 0);

        // Fiedler fixed-timestep accumulator. clock_ is FixedClock under
        // CAIRNS_DUMP (1 step/frame, alpha=0) or WallClock live. wall_dt is
        // clamped to kMaxFrameDt to avoid spiral-of-death on big stalls.
        const double wall_dt = clock_->Tick();
        accumulator_ += std::min(wall_dt, cairns::kMaxFrameDt);
        sim_steps_this_frame_ = 0;
        while (accumulator_ >= cairns::kFixedDt &&
               sim_steps_this_frame_ < cairns::kMaxStepsPerFrame) {
            ++sim_frame_;
            accumulator_ -= cairns::kFixedDt;
            ++sim_steps_this_frame_;
        }
        const float alpha = static_cast<float>(accumulator_ / cairns::kFixedDt);

        if (frame_ <= 5) {
            fprintf(stderr,
                    "[FCLK] frame=%u wall_dt=%.4f acc=%.4f steps=%u alpha=%.3f "
                    "sim_frame=%llu\n",
                    frame_, wall_dt, accumulator_, sim_steps_this_frame_,
                    alpha, static_cast<unsigned long long>(sim_frame_));
        }


        cairns::Timer t_build("build_draws", 1);
        if (!BuildMeshOpaqueDraws(slot)) {
            return false;
        }
        t_build.End();
        // Sort PER-SCENE-RANGE so each viewport's sorted sub-span orders
        // (and indexes) only its own scene's draws.
        for (uint32_t k = 0; k < s.scene_ranges_count; ++k) {
            const PerSlot::SceneDrawRange& r = s.scene_ranges[k];
            std::sort(s.drawListSorted.begin() + r.draw_lo,
                      s.drawListSorted.begin() + r.draw_hi);
        }

        // resident_textures is built once at load (uploadAnimTablesGpu);
        // no per-frame gather.

        // BuildSkinFrame populates the per-frame skin payload (palettes,
        // InstanceMeta, SkinBatchGpu list) on the per-slot arena and
        // publishes spans on s.pkt; with no skinned content it writes
        // empty spans and the static path stays bit-for-bit unchanged.
        {
            cairns::Timer t_skin("skin_eval", 8);
            BuildSkinFrame(slot);
        }

        // Fill packet header (the view into per-slot storage).
        s.pkt.frame_idx = frame_;
        s.pkt.slot = slot;
        s.pkt.view = s.pending_view_matrix[viewport_mgr_.active_index];
        s.pkt.proj = glm::mat4(1.0f);  // not used downstream; view_proj baked into pending_globals
        s.pkt.near_z = s.pending_near_z[viewport_mgr_.active_index];
        s.pkt.far_z = s.pending_far_z[viewport_mgr_.active_index];
        s.pkt.sim_steps_this_frame = sim_steps_this_frame_;
        s.pkt.fixed_dt = static_cast<float>(cairns::kFixedDt);
        // Wait for the previous frame's render-thread-published parity. In
        // steady state Acquire(slot) already established the happens-after,
        // so this rarely actually blocks.
        {
            std::unique_lock<std::mutex> lk(particles_.parity_m);
            particles_.parity_cv.wait(lk, [&] {
                return particles_.latest_parity_frame + 1 >= frame_;
            });
            s.pkt.particle_parity_in = particles_.latest_parity_out;
        }
        s.pkt.draws = std::span<const cairns::Draw>(s.drawList.data(), s.drawList.size());
        s.pkt.sorted = std::span<const std::pair<cairns::DrawKey, uint32_t>>(
            s.drawListSorted.data(), s.drawListSorted.size());
        s.pkt.resident_textures = std::span<const rhi::Handle<rhi::Texture>>(
            prefab_store_.resident_textures.data(), prefab_store_.resident_textures.size());

        // Imgui in golden mode is opt-in (SetImguiInGolden) so the overlay
        // golden can gate it. Surfaceless still skips the SDL3 NewFrame --
        // cairns_serve doesn't init SDL3 -- but ImGui proper runs.
        const bool surfaceless = !present_.final_target.IsNull();
        // Windowed native draws imgui; surfaceless cairns_serve does NOT. The web
        // app is also surfaceless (renders offscreen then copies to the canvas)
        // but DOES want the same imgui HUD + scenario panel as native, so it opts
        // in via SetImguiEnabled -- giving one UI across metal/vk/webgpu.
        const bool draw_imgui =
            (!golden_ && (!surfaceless || imgui_enabled_)) ||
            (golden_ && imgui_in_golden_);
        if (draw_imgui) {
            // SDL-backed shells (windowed native + web) let ImGui_ImplSDL3 own
            // DisplaySize + input; backend-less hosts (cairns_serve, goldens) set
            // DisplaySize manually (NewFrame asserts on the default -1,-1).
            if (ImGui::GetIO().BackendPlatformUserData != nullptr) {
                cairns::platform::ImguiNewFrame();
            } else {
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2(static_cast<float>(FrameWidth()),
                                        static_cast<float>(FrameHeight()));
                io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            }
            ImGui::NewFrame();
            if (hud_visible_) {
            ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
            ImGui::Begin("cairns", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            // Golden mode injects fixed HudStats so the overlay is byte-stable;
            // live cpu_ms_last_ / Timer slots are wall-clock and would flake.
            const bool hud_injected = injected_hud_stats_.has_value();
            const float cpu_ms_disp =
                hud_injected ? injected_hud_stats_->cpu_ms : cpu_ms_last_;
            const float fps =
                hud_injected
                    ? injected_hud_stats_->fps
                    : (cpu_ms_last_ > 0.0f ? 1000.0f / cpu_ms_last_ : 0.0f);
            const float* graph_data = hud_injected
                                          ? injected_hud_stats_->frame_ms.data()
                                          : cpu_ms_history_;
            const int graph_count =
                hud_injected ? static_cast<int>(cairns::HudStats::kGraph)
                             : kCpuMsHistory;
            const int graph_head =
                hud_injected ? static_cast<int>(injected_hud_stats_->graph_head)
                             : cpu_ms_head_;
            float ms_max = 1.0f;
            float ms_avg = 0.0f;
            int ms_n = 0;
            for (int i = 0; i < graph_count; ++i) {
                const float v = graph_data[i];
                if (!std::isfinite(v) || v < 0.0f || v > 1.0e6f) {
                    continue;
                }
                ms_max = v > ms_max ? v : ms_max;
                ms_avg += v;
                ++ms_n;
            }
            ms_avg /= static_cast<float>(ms_n > 0 ? ms_n : 1);
            ImGui::Text("CPU %6.2f ms   |   %3.0f FPS", cpu_ms_disp, fps);
            ImGui::Text("avg %6.2f ms   |   peak %6.2f ms", ms_avg, ms_max);
            if (!hud_injected) {
                auto slot_avg_ms = [this](int s) -> float {
                    const uint64_t n = cairns::Timer::accum_itrs_[s];
                    if (n == 0) {
                        return slot_ms_cache_[s];
                    }
                    const double v =
                        cairns::Timer::accum_times_[s] /
                        static_cast<double>(n) / 1000.0;
                    if (!std::isfinite(v) || v < 0.0 || v > 1.0e6) {
                        return slot_ms_cache_[s];
                    }
                    slot_ms_cache_[s] = static_cast<float>(v);
                    return slot_ms_cache_[s];
                };
                const uint32_t gpu_mask = cairns::TimerStorage::GpuSlotMask();
                float gpu_frame_ms = 0.0f;
                for (uint32_t s = 0; s < cairns::Timer::kMaxSlots; ++s) {
                    if (gpu_mask & (1u << s)) {
                        gpu_frame_ms += slot_avg_ms(s);
                    }
                }
                ImGui::Text("%-12s %5.2f ms", "gpu_frame", gpu_frame_ms);
                for (uint32_t s = 0; s < cairns::Timer::kMaxSlots; ++s) {
                    const char* nm = cairns::Timer::slot_names_[s];
                    if (!nm) {
                        continue;
                    }
                    if (cairns::Timer::accum_itrs_[s] == 0 &&
                        slot_ms_cache_[s] == 0.0f) {
                        continue;
                    }
                    if (std::strcmp(nm, "set up render pass globals") == 0 ||
                        std::strcmp(nm, "build opaque draw list") == 0 ||
                        std::strcmp(nm, "particle_sim") == 0 ||
                        std::strcmp(nm, "forward") == 0) {
                        continue;
                    }
                    ImGui::Text("%-12s %6.2f ms", nm, slot_avg_ms(s));
                }
            }
            char overlay[32];
            std::snprintf(overlay, sizeof(overlay), "%.2f ms", cpu_ms_disp);
            ImGui::PlotLines("##cpuhist", graph_data, graph_count,
                             graph_head, overlay, 0.0f, ms_max * 1.15f,
                             ImVec2(300.0f, 110.0f));
            ImGui::End();
            ImGui::PopStyleColor(4);
            }  // hud_visible_
            // App-provided imgui panel (e.g. the scenario launcher), drawn
            // into the same frame as the HUD. Raw fn ptr + ctx -- no singleton,
            // no std::function alloc; the app owns the panel + does any dispatch.
            if (imgui_panel_fn_) {
                imgui_panel_fn_(imgui_panel_ctx_);
            }
            ImGui::Render();
            auto free_snapshot = [](ImDrawData* d) {
                if (!d) return;
                for (int i = 0; i < d->CmdLists.Size; ++i) {
                    IM_DELETE(d->CmdLists[i]);
                }
                IM_DELETE(d);
            };
            free_snapshot(s.pkt.imgui_snapshot);
            ImDrawData* src = ImGui::GetDrawData();
            ImDrawData* dst = IM_NEW(ImDrawData)();
            dst->Valid = src->Valid;
            dst->DisplayPos = src->DisplayPos;
            dst->DisplaySize = src->DisplaySize;
            dst->FramebufferScale = src->FramebufferScale;
            dst->OwnerViewport = src->OwnerViewport;
            dst->Textures = src->Textures;
            // ImDrawData::AddDrawList asserts _VtxWritePtr == VtxBuffer.Data
            // + VtxBuffer.Size on the input list, but ImDrawList::CloneOutput()
            // does NOT restore _VtxWritePtr / _IdxWritePtr / _VtxCurrentIdx
            // on the freshly-constructed clone, so the assertion fires.
            // Bypass AddDrawList and replicate its bookkeeping ourselves.
            for (int i = 0; i < src->CmdLists.Size; ++i) {
                ImDrawList* cloned = src->CmdLists[i]->CloneOutput();
                dst->CmdLists.push_back(cloned);
                dst->CmdListsCount++;
                dst->TotalVtxCount += cloned->VtxBuffer.Size;
                dst->TotalIdxCount += cloned->IdxBuffer.Size;
            }
            s.pkt.imgui_snapshot = dst;
        } else {
            if (s.pkt.imgui_snapshot != nullptr) {
                for (int i = 0; i < s.pkt.imgui_snapshot->CmdLists.Size; ++i) {
                    IM_DELETE(s.pkt.imgui_snapshot->CmdLists[i]);
                }
                IM_DELETE(s.pkt.imgui_snapshot);
                s.pkt.imgui_snapshot = nullptr;
            }
        }

        // Determinism probe (SIM): hash the per-frame sim input -- the
        // arena's [0,Used) (drawList/sorted/matrices/entity_ids/proxies, all POD
        // in the block) + the sim drivers. Under FixedClock + a static scene this
        // MUST be byte-identical every frame AND run-to-run; a diverging sim hash
        // localizes CPU-side nondeterminism (vs the render/GPU side).
        if (golden_) {
            cairns::Fnv1a sim;
            sim.Write(s.arena.Resolve(0), s.arena.Used());
            sim.WritePod(accumulator_);
            sim.WritePod(sim_frame_);
            last_sim_hash_ = sim.Digest();
            std::fprintf(stderr, "[STATEHASH] frame=%u sim=%016llx used=%zu\n",
                         frame_, (unsigned long long)last_sim_hash_,
                         s.arena.Used());
        }

        // Hand the slot to the render thread BEFORE main-thread Submit.
        // Unlock the mutex first so render thread's RecordFrame can take
        // its own std::lock_guard without blocking on main.
        slot_lock.unlock();
        render_thread_->Submit(slot, &s.pkt);

        present_.queue.push_back(static_cast<int32_t>(slot));
        {
            cairns::Timer t_pw("present_wait", 9);
            while (!present_.queue.empty()) {
                const int32_t head = present_.queue.front();
                PerSlot& ps = slots_[head];
                rhi::FrameContext present_fc{};
                rhi::SwapResolveTarget present_target{};
                bool ready = false;
                {
                    std::unique_lock<std::mutex> lk(present_.m);
                    if (ps.present_ready) {
                        present_fc = ps.present_fc;
                        present_target = ps.present_target;
                        ps.present_ready = false;
                        ready = true;
                    }
                }
                if (!ready) {
                    break;
                }
                present_.queue.pop_front();
                rhi_.frames.Present(present_target, rhi_.frame_capture,
                                    present_fc);
            }
        }
        present_.prev_slot = static_cast<int32_t>(slot);

        // Under CAIRNS_DUMP, collapse to depth-1 pipelining: wait for the
        // render thread to fully complete this frame before the next iteration
        // queues another. Keeps frame 5's dump output byte-identical regardless
        // of threading (Drain forces same parity sequence as single-threaded).
        //
        // Also drain in surfaceless mode (cairns_serve) so the next
        // io.dumpTexture op sees the rendered pixels rather than reading
        // present_.final_target while the render thread is still working on it.
        // Also drain when a pick is pending so the pick resolve sees the
        // just-rendered frame -- windowed sdl-min normally lets the render
        // thread run async, but Shift+LMB stalls one frame to resolve the
        // pick (acceptable cost for an interactive event).
        if (golden_ || !present_.final_target.IsNull() || picking_.pending) {
            render_thread_->Drain();
        }

        // Resolve the pending pick: entity id + 1, 0 = clicked empty space.
        // The result lands in picking_.highlights so the outline pass
        // activates on the next frame.
        if (picking_.pending && picking_.viewport < kNumViewports) {
            // CPU ray-cast pick -- synchronous + identical on metal/vulkan/webgpu,
            // no GPU id-buffer readback (the browser can't read back synchronously).
            // The id buffer stays only for the outline edge-detect (GPU-side).
            const uint32_t entity_plus_one = ResolvePickRaycast(
                picking_.viewport, picking_.x, picking_.y,
                s.pending_globals[picking_.viewport].inv_view_proj);
            const bool ok = true;
            // Resolve hero name + world AABB from the clicked id so the
            // [PICK] line answers "which hero + where" in one printf.
            const char* hero_name = "<none>";
            std::string hero_name_storage;
            uint32_t hero_scene_idx = 0xFFFFFFFFu;
            glm::vec3 hero_min(0.0f);
            glm::vec3 hero_max(0.0f);
            bool hero_has_aabb = false;
            bool hero_animated = false;
            if (ok && entity_plus_one != 0u && !prefab_store_.prefab_ids.empty()) {
                const uint32_t eid = entity_plus_one - 1u;
                hero_scene_idx =
                    eid % static_cast<uint32_t>(prefab_store_.prefab_ids.size());
                if (hero_scene_idx < prefab_store_.glb_paths.size()) {
                    hero_name_storage =
                        prefab_store_.glb_paths[hero_scene_idx].filename().string();
                    hero_name = hero_name_storage.c_str();
                }
                // Resolve world AABB via the entity's WorldTransform + the
                // scene's first mesh bind-pose AABB (matches the cull path).
                if (cairns::Scene::Cold* wcc =
                        scene_mgr_.pool.GetCold(scene_mgr_.active)) {
                    entt::entity ent{eid};
                    if (wcc->registry.valid(ent)) {
                        // An entity gets a SkinRef when TryCreateSkinForScene
                        // succeeded at init. Absence -> static bind pose;
                        // check [SKIN-FAIL] logs at init for the reason.
                        hero_animated =
                            wcc->registry.all_of<cairns::SkinRef>(ent);
                        const auto* wt =
                            wcc->registry.try_get<cairns::WorldTransform>(ent);
                        cairns::Prefab::Hot* sh =
                            prefab_store_.prefabs.GetHot(prefab_store_.prefab_ids[hero_scene_idx]);
                        if (wt && sh && !sh->meshes.empty()) {
                            cairns::Mesh::Hot* mh =
                                prefab_store_.meshes.GetHot(sh->meshes[0]);
                            if (mh && mh->bind_aabb_min.x <=
                                          mh->bind_aabb_max.x) {
                                glm::vec3 wmin(1.0e30f);
                                glm::vec3 wmax(-1.0e30f);
                                const glm::vec3& mn = mh->bind_aabb_min;
                                const glm::vec3& mx = mh->bind_aabb_max;
                                for (int i = 0; i < 8; ++i) {
                                    const glm::vec3 corner(
                                        (i & 1) ? mx.x : mn.x,
                                        (i & 2) ? mx.y : mn.y,
                                        (i & 4) ? mx.z : mn.z);
                                    const glm::vec4 wc4 =
                                        wt->world * glm::vec4(corner, 1.0f);
                                    const glm::vec3 wc(wc4 / wc4.w);
                                    wmin = glm::min(wmin, wc);
                                    wmax = glm::max(wmax, wc);
                                }
                                hero_min = wmin;
                                hero_max = wmax;
                                hero_has_aabb = true;
                            }
                        }
                    }
                }
            }
            CAIRNS_PRINT(
                    "[PICK] vp=%d xy=(%u,%u) tex_dims=(%u,%u) ok=%d "
                    "id+1=%u hero=%s scene_idx=%u animated=%d "
                    "aabb=[%s%.3f,%.3f,%.3f]-[%.3f,%.3f,%.3f]\n",
                    picking_.viewport, picking_.x, picking_.y, id_target_w_,
                    id_target_h_, ok ? 1 : 0, entity_plus_one,
                    hero_name, hero_scene_idx,
                    hero_animated ? 1 : 0,
                    hero_has_aabb ? "" : "n/a:",
                    hero_min.x, hero_min.y, hero_min.z,
                    hero_max.x, hero_max.y, hero_max.z);
            if (ok) {
                picking_.last_result.viewport = picking_.viewport;
                picking_.last_result.x = picking_.x;
                picking_.last_result.y = picking_.y;
                picking_.last_result.type = cairns::SelectionType::kEntity;
                picking_.last_result.id = entity_plus_one;
                picking_.last_result.raw = entity_plus_one;
                picking_.resolved = true;
                if (entity_plus_one != 0u) {
                    std::vector<cairns::SelectionTarget> next;
                    next.push_back({cairns::SelectionType::kEntity,
                                    entity_plus_one, 0u});
                    SetHighlights(std::move(next));
                } else {
                    ClearHighlights();
                }
            }
            picking_.pending = false;
        }

        t_frame.End();
        if (frame_ % 120 == 0) {
            const size_t loaded = prefab_store_.prefab_ids.size();
            size_t entities = 0;
            if (auto* wc = scene_mgr_.pool.GetCold(scene_mgr_.active)) {
                entities = wc->registry.storage<entt::entity>().size();
            }
            const size_t slices = loaded > 0 ? entities / loaded : 0;
            CAIRNS_PRINT("============\n");
            CAIRNS_PRINT("draws %zu | %zu GLBs x %zu slices = %zu entities | resolution %u x %u\n",
                         s.drawList.size(), loaded, slices, entities,
                         FrameWidth(), FrameHeight());
            cairns::Timer::PrintReport();
            cairns::Timer::Reset();
        }
        return true;
    }

}  // namespace cairns

namespace cairns {

void Engine::EncodeDraws(const FramePacket& pkt) {
        PerSlot& s = slots_[pkt.slot];
        // 1. globals UBO -- one per viewport, distinct bump offsets. The
        // forward pass for viewport v binds s.globals_offset[v].
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            void* gptr = rhi_.alloc.BumpAllocate(
                sizeof(cairns::rhi::RenderPassGlobals), rhi_.alloc.UboAlign(),
                rhi::Memory::kDynamic, &s.globals_offset[v]);
            assert(gptr && "bump alloc failed: render pass globals");
            memcpy(gptr, &s.pending_globals[v], sizeof(cairns::rhi::RenderPassGlobals));
        }
        // Shadow globals: a copy of viewport 0's globals with the LIGHT
        // view_proj in the view_proj slot, so depth_only.vert projects from
        // the light's POV without knowing it is a light.
        if (s.shadow_active) {
            cairns::rhi::RenderPassGlobals sg = s.pending_globals[0];
            sg.view_proj = s.light_view_proj;
            void* sgp = rhi_.alloc.BumpAllocate(
                sizeof(cairns::rhi::RenderPassGlobals), rhi_.alloc.UboAlign(),
                rhi::Memory::kDynamic, &s.shadow_globals_offset);
            assert(sgp && "bump alloc failed: shadow globals");
            memcpy(sgp, &sg, sizeof(cairns::rhi::RenderPassGlobals));
        }
        if (frame_ <= 6) {
            const glm::mat4& vp = s.pending_globals[viewport_mgr_.active_index].view_proj;
            const float vp_w = static_cast<float>(FrameWidth()) /
                                static_cast<float>(std::max(1, viewport_mgr_.active_count));
            const float aspect_ratio = vp_w / static_cast<float>(FrameHeight());
            size_t entity_count = 0;
            if (auto* wc = scene_mgr_.pool.GetCold(scene_mgr_.active)) {
                entity_count = wc->registry.storage<entt::entity>().size();
            }
            fprintf(stderr,
                    "[FLAKE] frame=%u w=%u h=%u aspect=%.9f vp00=%.9f vp11=%.9f "
                    "vp22=%.9f vp32=%.9f goff[0]=%u par_in=%u par_out=%u "
                    "entities=%zu meshes=%zu prims=%zu\n",
                    frame_, FrameWidth(), FrameHeight(), aspect_ratio,
                    vp[0][0], vp[1][1], vp[2][2], vp[3][2],
                    s.globals_offset[0],
                    pkt.particle_parity_in, pkt.particle_parity_out,
                    entity_count, s.proxies.meshes.size(),
                    s.proxies.primitives.size());
        }

        // 2. ONE MaterialGpu per referenced material (offset shared across its
        // draws -- the per-material dynamic-offset channel), then the per-draw
        // draw_tmp UBOs in stable_idx order. The offset cache lives on the
        // slot arena, indexed by material pool slot.
        const uint32_t mat_cap = prefab_store_.materials.Capacity();
        uint32_t* mat_offset_cache = s.arena.AllocateArray<uint32_t>(mat_cap);
        memset(mat_offset_cache, 0xFF, mat_cap * sizeof(uint32_t));
        // Same id_path RecordFrame uses for the pass PSO -- the per-draw lit
        // override must match the pass's MRT layout.
        const bool enc_id_path =
            !picking_.highlights.empty() || picking_.pending;
        for (size_t i = 0; i < s.drawList.size(); ++i) {
            const cairns::Handle<cairns::Material> mid = s.draw_material_ids[i];
            const cairns::Material::Cold* mcold =
                mid.IsNull() ? nullptr : prefab_store_.materials.GetCold(mid);
            // Variant resolution: a lit material overrides the pass PSO for
            // its draws (recorders rebind on draw.shader change).
            if (mcold != nullptr &&
                mcold->shader_key == cairns::ShaderKey::kLit) {
                s.drawList[i].shader =
                    enc_id_path ? lit_offscreen_ : lit_offscreen_noid_;
                // ALWAYS bind the shadow map (slot 3) on lit draws where the
                // backend has a shadow PSO (metal/vk; webgpu stubs it null +
                // its WGSL lit shaders declare no group 3). The bind group is
                // a valid depth tex+sampler even when no map rendered this
                // frame; the lit fragment's light_dir.w>0 (== cast_shadows)
                // guard prevents sampling stale/undefined shadow data. This
                // keeps slot 3 bound so metal never hits an unbound arg buffer.
                if (!shadow_pso_.IsNull() && !shadow_bind_group_.IsNull()) {
                    s.drawList[i].bind_groups[2] = shadow_bind_group_;
                }
            }
            uint32_t material_offset = UINT32_MAX;
            if (!mid.IsNull() && mid.index < mat_cap) {
                material_offset = mat_offset_cache[mid.index];
                if (material_offset == UINT32_MAX) {
                    void* mptr = rhi_.alloc.BumpAllocate(
                        sizeof(cairns::rhi::MaterialGpu), rhi_.alloc.UboAlign(),
                        rhi::Memory::kDynamic, &material_offset);
                    assert(mptr && "bump alloc failed: material");
                    const cairns::rhi::MaterialGpu defaults{};
                    memcpy(mptr, mcold != nullptr ? &mcold->params : &defaults,
                           sizeof(cairns::rhi::MaterialGpu));
                    mat_offset_cache[mid.index] = material_offset;
                }
            } else {
                void* mptr = rhi_.alloc.BumpAllocate(
                    sizeof(cairns::rhi::MaterialGpu), rhi_.alloc.UboAlign(),
                    rhi::Memory::kDynamic, &material_offset);
                assert(mptr && "bump alloc failed: material");
                const cairns::rhi::MaterialGpu defaults{};
                memcpy(mptr, &defaults, sizeof(defaults));
            }

            cairns::rhi::DrawTmp draw_tmp { .model_matrix = s.draw_world_matrices[i] };
            draw_tmp.entity_id = s.draw_entity_ids[i];
            uint32_t drawtmp_offset = 0;
            void* tptr = rhi_.alloc.BumpAllocate(
                sizeof(cairns::rhi::DrawTmp), rhi_.alloc.UboAlign(),
                rhi::Memory::kDynamic, &drawtmp_offset);
            assert(tptr && "bump alloc failed: draw tmp");
            memcpy(tptr, &draw_tmp, sizeof(draw_tmp));

            s.drawList[i].dynamic_buffer_offsets[0] = material_offset;
            s.drawList[i].dynamic_buffer_offsets[1] = drawtmp_offset;
        }

        // Shadow draw list: same geometry (index/vertex/drawtmp offset), but
        // .shader nulled so the recorder uses the pass's shadow_pso_, and the
        // material/shadow bind groups cleared (depth_only reads only globals
        // + drawtmp). Built after the forward stamps so drawtmp offsets are
        // final.
        if (s.shadow_active) {
            cairns::Draw* sdl =
                s.arena.AllocateArray<cairns::Draw>(s.drawList.size());
            for (size_t i = 0; i < s.drawList.size(); ++i) {
                cairns::Draw d = s.drawList[i];
                d.shader = rhi::Handle<rhi::Shader>::Null;
                d.bind_groups[0] = rhi::Handle<rhi::BindGroup>::Null;
                d.bind_groups[1] = rhi::Handle<rhi::BindGroup>::Null;
                d.bind_groups[2] = rhi::Handle<rhi::BindGroup>::Null;
                sdl[i] = d;
            }
            s.shadowDrawList =
                std::span<cairns::Draw>(sdl, s.drawList.size());
        } else {
            s.shadowDrawList = {};
        }

        // 3. delta_time UBO.
        float* dt_ptr = static_cast<float*>(
            rhi_.alloc.BumpAllocate(sizeof(float), rhi_.alloc.UboAlign(),
                                    rhi::Memory::kDynamic, &s.dt_off));
        assert(dt_ptr && "bump alloc failed: delta time");
        // Compute kernel sees the fixed sim dt, NOT wall dt -- particles step
        // at a constant rate regardless of frame timing.
        *dt_ptr = pkt.fixed_dt;

        // RENDER hash: the bytes we just encoded for the GPU this frame
        // -- per-viewport globals UBO content + bump offsets, per-draw model
        // matrix + entity id + the two dynamic offsets, and the dt offset. A
        // SEMANTIC hash of the encode outputs (not a raw ring dump), so it is
        // immune to the bump ring's alignment padding and is identical metal/vk.
        // Stable run-to-run => the GPU input is deterministic; any remaining
        // pixel flake is pure GPU execution.
        if (golden_) {
            cairns::Fnv1a render;
            for (int v = 0; v < viewport_mgr_.active_count; ++v) {
                render.Write(&s.pending_globals[v],
                             sizeof(cairns::rhi::RenderPassGlobals));
                render.WritePod(s.globals_offset[v]);
            }
            for (size_t i = 0; i < s.drawList.size(); ++i) {
                render.WritePod(s.drawList[i].dynamic_buffer_offsets[0]);
                render.WritePod(s.drawList[i].dynamic_buffer_offsets[1]);
                render.WritePod(s.draw_world_matrices[i]);
                render.WritePod(s.draw_entity_ids[i]);
            }
            render.WritePod(s.dt_off);
            last_render_hash_ = render.Digest();
            std::fprintf(stderr, "[STATEHASH] frame=%u render=%016llx draws=%zu\n",
                         frame_, (unsigned long long)last_render_hash_,
                         s.drawList.size());
        }
    }

}  // namespace cairns

namespace cairns {

bool Engine::BuildMeshOpaqueDraws(uint32_t slot) {
        PerSlot& s = slots_[slot];
        // CPU-side scene build only. NO bump allocations -- those happen in
        // EncodeDraws() on the render-thread side post-split. Stable-index
        // writes (resize + index assignment) so the output is independent of
        // walk/execution order.
        // Sim-time for per-entity TransformAnim (deterministic; steps at 60Hz).
        const float anim_t =
            static_cast<float>(sim_frame_) * static_cast<float>(cairns::kFixedDt);

        // Per-viewport camera resolve. Each viewport gets its own
        // RenderPassGlobals (uploaded at a distinct bump offset by
        // EncodeDraws); RecordFrame issues one forward pass per viewport
        // bound against the matching offset. Aspect is (vp_w / vp_h) where
        // vp_w = FrameWidth() / kNumViewportsPerSlot (side-by-side split).
        const float vp_w = static_cast<float>(FrameWidth()) /
                            static_cast<float>(std::max(1, viewport_mgr_.active_count));
        const float vp_h = static_cast<float>(FrameHeight());
        const float aspect_ratio = vp_w / vp_h;
        const float fov = 90 * (std::numbers::pi / 180.0f);
        const float near_z = 0.1f;
        const float far_z = 100.0f;
        // proj built per-viewport below since CameraComponent may override
        // fov / near_z / far_z.
        // Per-viewport camera resolve. Camera role #1 (FlyController):
        // viewport.camera_entity == entt::null; FlyController state drives
        // pose. Camera role #2 (placed CameraComponent entity): the entity
        // owns a WorldTransform (pose) + CameraComponent (intrinsics) in
        // the active scene's registry. WorldTransform.world is the camera-
        // to-world matrix; the view matrix is its inverse.
        cairns::Scene::Cold* wc_cam = scene_mgr_.pool.GetCold(scene_mgr_.active);
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            cairns::Viewport::Cold* vpc =
                viewport_mgr_.pool.GetCold(viewport_mgr_.ids[v]);
            const entt::entity vp_cam_entity = vpc->camera_entity;
            const bool entity_cam =
                vp_cam_entity != entt::null && wc_cam &&
                wc_cam->registry.all_of<cairns::WorldTransform,
                                         cairns::CameraComponent>(vp_cam_entity);
            glm::vec3 camera_pos;
            glm::vec3 camera_dir;
            glm::mat4 view_matrix;
            float vp_fov = fov;
            float vp_near = near_z;
            float vp_far = far_z;
            if (entity_cam) {
                const cairns::CameraComponent& cc =
                    wc_cam->registry.get<cairns::CameraComponent>(vp_cam_entity);
                const cairns::WorldTransform& wt =
                    wc_cam->registry.get<cairns::WorldTransform>(vp_cam_entity);
                vp_fov = cc.fov_y_rad;
                vp_near = cc.near_z;
                vp_far = cc.far_z;
                view_matrix = glm::inverse(wt.world);
                camera_pos = glm::vec3(wt.world[3]);
                // -Z in local space is the camera's forward in world space.
                camera_dir = glm::normalize(glm::vec3(-wt.world[2]));
            } else {
                const cairns::FlyController& fc = vpc->fly;
                const float cy = std::cos(fc.yaw);
                const float sy = std::sin(fc.yaw);
                const float cp = std::cos(fc.pitch);
                const float sp = std::sin(fc.pitch);
                camera_pos = fc.position;
                camera_dir = glm::vec3(-cp * sy, sp, -cp * cy);
                view_matrix = glm::lookAtRH(camera_pos,
                                             camera_pos + camera_dir,
                                             glm::vec3(0, 1, 0));
            }
            const glm::mat4 vp_proj =
                glm::perspectiveRH_ZO(vp_fov, aspect_ratio, vp_near, vp_far);
            const glm::mat4 view_proj = vp_proj * view_matrix;
            s.pending_globals[v] = cairns::rhi::RenderPassGlobals {
                .view_proj = view_proj,
                .inv_view_proj = glm::inverse(view_proj),
                .camera_pos = glm::vec4(camera_pos, 1.0f /*exposure */),
                .camera_dir = glm::vec4(camera_dir, vp_near),
                .screen_params = glm::vec4(vp_w, vp_h, 1.0f / vp_w, 1.0f / vp_h)
            };
            // Directional light: first one in the viewport's bound scene
            // (fallback: active). Absent => the designated-init above left
            // the light fields zeroed -- lit materials render ambient-black.
            {
                cairns::Scene::Cold* lwc = wc_cam;
                cairns::Viewport::Hot* vph =
                    viewport_mgr_.pool.GetHot(viewport_mgr_.ids[v]);
                if (vph != nullptr) {
                    if (cairns::Scene::Cold* bc =
                            scene_mgr_.pool.GetCold(vph->scene)) {
                        lwc = bc;
                    }
                }
                if (lwc != nullptr) {
                    auto lights =
                        lwc->registry.view<cairns::DirectionalLight>();
                    for (entt::entity le : lights) {
                        const cairns::DirectionalLight& dl =
                            lights.get<cairns::DirectionalLight>(le);
                        const glm::vec3 nd = glm::normalize(dl.dir);
                        s.pending_globals[v].light_dir =
                            glm::vec4(nd, dl.cast_shadows ? 1.0f : 0.0f);
                        s.pending_globals[v].light_color =
                            glm::vec4(dl.color, dl.intensity);
                        s.pending_globals[v].ambient =
                            glm::vec4(dl.ambient, 0.0f);
                        break;
                    }
                }
            }
            s.pending_view_matrix[v] = view_matrix;
            s.pending_near_z[v] = vp_near;
            s.pending_far_z[v] = vp_far;
        }

        // Directional shadow matrix (ONE map, from the ACTIVE scene's
        // cast_shadows light over the scene's world extent). Deterministic
        // fit: AABB over entity WORLD-TRANSFORM translations + a fixed
        // margin, fixed fallback box when the scene is empty (per-object
        // AABB fit is a refinement). Written into every viewport's globals
        // so the lit fragment can project into shadow space; the shadow pass
        // uploads it as its own globals' view_proj.
        s.shadow_active = false;
        s.light_view_proj = glm::mat4(1.0f);
        // Resolve the caster scene like the per-viewport light extraction:
        // viewport 0's bound scene, falling back to the active scene. (The
        // active scene index can differ from the viewport's bound scene.)
        cairns::Scene::Cold* shadow_wc = wc_cam;
        if (viewport_mgr_.active_count > 0) {
            if (cairns::Viewport::Hot* vph0 =
                    viewport_mgr_.pool.GetHot(viewport_mgr_.ids[0])) {
                if (cairns::Scene::Cold* bc =
                        scene_mgr_.pool.GetCold(vph0->scene)) {
                    shadow_wc = bc;
                }
            }
        }
        if (shadow_wc != nullptr) {
            glm::vec3 sun_dir(0.0f, -1.0f, 0.0f);
            bool casts = false;
            {
                auto lv = shadow_wc->registry.view<cairns::DirectionalLight>();
                for (entt::entity le : lv) {
                    const cairns::DirectionalLight& dl =
                        lv.get<cairns::DirectionalLight>(le);
                    if (dl.cast_shadows) {
                        sun_dir = glm::normalize(dl.dir);
                        casts = true;
                        break;
                    }
                }
            }
            if (casts) {
                glm::vec3 bmin(1.0e30f);
                glm::vec3 bmax(-1.0e30f);
                auto wv = shadow_wc->registry.view<const cairns::WorldTransform>();
                for (entt::entity we : wv) {
                    const glm::vec3 p =
                        glm::vec3(wv.get<const cairns::WorldTransform>(we).world[3]);
                    bmin = glm::min(bmin, p);
                    bmax = glm::max(bmax, p);
                }
                glm::vec3 center(0.0f);
                float radius = 6.0f;  // deterministic fallback box
                if (bmin.x <= bmax.x) {
                    center = 0.5f * (bmin + bmax);
                    radius = glm::length(bmax - center) + 3.0f;
                    radius = std::max(radius, 2.0f);
                }
                const glm::vec3 up =
                    std::abs(sun_dir.y) > 0.99f ? glm::vec3(0, 0, 1)
                                                : glm::vec3(0, 1, 0);
                const glm::vec3 eye = center - sun_dir * (radius * 2.0f);
                const glm::mat4 lview = glm::lookAtRH(eye, center, up);
                const glm::mat4 lproj = glm::orthoRH_ZO(
                    -radius, radius, -radius, radius, 0.05f, radius * 4.0f);
                s.light_view_proj = lproj * lview;
                s.shadow_active = !shadow_pso_.IsNull();
            }
        }
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            s.pending_globals[v].light_view_proj = s.light_view_proj;
        }

        // Post-effect chain: same scene resolution as the shadow light
        // (viewport-0's bound scene, active fallback). Insertion-sorted by
        // order; ties keep registry iteration order.
        s.post_effect_count = 0;
        if (shadow_wc != nullptr) {
            auto fxv = shadow_wc->registry.view<cairns::PostEffect>();
            for (entt::entity fe : fxv) {
                if (s.post_effect_count >= PerSlot::kMaxPostEffects) {
                    break;
                }
                const cairns::PostEffect& fx = fxv.get<cairns::PostEffect>(fe);
                uint32_t i = s.post_effect_count++;
                while (i > 0 && s.post_effects[i - 1].order > fx.order) {
                    s.post_effects[i] = s.post_effects[i - 1];
                    --i;
                }
                s.post_effects[i] = fx;
            }
        }

        // Set each scene's root_transform, run TRS hierarchy propagation
        // (no-op when no entity carries a Transform; scene-load emplaces
        // WorldTransform directly), then extract. Extract composes
        // node.globalTransform * (world * root_transform).
        // Multi-scene fan-out: extract EVERY distinct scene any viewport
        // binds into the ONE s.proxies union (appended), recording each
        // scene's [mesh) range. Viewports on the same scene share its range
        // (dedup). The draw build later carves a per-viewport [draw)
        // sub-range from these, so two viewports on two scenes render
        // different content.
        // s.proxies' meshes + primitives lists live on this slot's
        // BumpArena; pushes beyond cap assert (headroom sized for the
        // 3300-hero benchmark, ~3300 / ~11220).
        s.proxies.Reset(s.arena);
        s.scene_ranges_count = 0;
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            s.viewport_scene_idx[v] = -1;
        }
        auto extract_scene_once = [&](cairns::SceneId sid) -> int {
            for (uint32_t k = 0; k < s.scene_ranges_count; ++k) {
                if (s.scene_ranges[k].scene.index == sid.index) {
                    return static_cast<int>(k);
                }
            }
            cairns::Scene::Hot* wh = scene_mgr_.pool.GetHot(sid);
            cairns::Scene::Cold* wc = scene_mgr_.pool.GetCold(sid);
            if (!wh || !wc) {
                return -1;
            }
            if (s.scene_ranges_count >=
                static_cast<uint32_t>(PerSlot::kMaxScenesPerSlot)) {
                return -1;
            }
            cairns::AnimateTransforms(*wc, anim_t);
            cairns::PropagateTransforms(*wc, glm::mat4(1.0f));
            const uint32_t mesh_lo =
                static_cast<uint32_t>(s.proxies.meshes.size());
            cairns::ExtractFromScene(*wc, wh->root_transform, scene_mgr_.assets,
                                     prefab_store_.prefabs, prefab_store_.meshes, prefab_arena_, s.proxies,
                                     /*append=*/true);
            const uint32_t mesh_hi =
                static_cast<uint32_t>(s.proxies.meshes.size());
            const uint32_t k = s.scene_ranges_count++;
            PerSlot::SceneDrawRange& r = s.scene_ranges[k];
            r.scene = sid;
            r.mesh_lo = mesh_lo;
            r.mesh_hi = mesh_hi;
            r.draw_lo = 0;
            r.draw_hi = 0;
            return static_cast<int>(k);
        };
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            const cairns::SceneId wid =
                viewport_mgr_.pool.GetHot(viewport_mgr_.ids[v])->scene;
            s.viewport_scene_idx[v] = extract_scene_once(wid);
        }

        // Counting pass -> total_draws.
        uint32_t total_draws = 0;
        for (const cairns::MeshProxy& mp : s.proxies.meshes) {
            total_draws += mp.primitive_count;
        }
        // Count-then-allocate on the per-slot BumpArena. The arena was
        // reset at slot Acquire and is exclusively ours until the render
        // thread submits this slot's frame. No std::vector heap; no
        // .resize() pump-and-shrink.
        using DrawKeyPair = std::pair<cairns::DrawKey, uint32_t>;
        s.drawList = {
            s.arena.AllocateArray<cairns::Draw>(total_draws), total_draws};
        s.drawListSorted = {
            s.arena.AllocateArray<DrawKeyPair>(total_draws), total_draws};
        s.draw_world_matrices = {
            s.arena.AllocateArray<glm::mat4>(total_draws), total_draws};
        s.draw_entity_ids = {
            s.arena.AllocateArray<uint32_t>(total_draws), total_draws};
        s.draw_material_ids = {
            s.arena.AllocateArray<cairns::Handle<cairns::Material>>(total_draws),
            total_draws};

        // Per-proxy starting stable_idx via prefix sum over primitive_count.
        // Lives on the per-slot arena so the workers can read it concurrently
        // without further allocation.
        const uint32_t n_proxies =
            static_cast<uint32_t>(s.proxies.meshes.size());
        uint32_t* proxy_first_draw =
            s.arena.AllocateArray<uint32_t>(n_proxies ? n_proxies : 1);
        {
            uint32_t acc = 0;
            for (uint32_t i = 0; i < n_proxies; ++i) {
                proxy_first_draw[i] = acc;
                acc += s.proxies.meshes[i].primitive_count;
            }
        }
        // Convert each scene's [mesh) range into its [draw) range via the
        // shared, spec-tested CarveSceneDrawRanges, so each viewport's sorted
        // sub-span covers only its own scene.
        {
            std::array<cairns::SceneDrawSpan, PerSlot::kMaxScenesPerSlot>
                spans{};
            for (uint32_t k = 0; k < s.scene_ranges_count; ++k) {
                spans[k].mesh_lo = s.scene_ranges[k].mesh_lo;
                spans[k].mesh_hi = s.scene_ranges[k].mesh_hi;
            }
            cairns::CarveSceneDrawRanges(
                std::span<const uint32_t>(proxy_first_draw, n_proxies),
                total_draws,
                std::span<cairns::SceneDrawSpan>(spans.data(),
                                                 s.scene_ranges_count));
            for (uint32_t k = 0; k < s.scene_ranges_count; ++k) {
                s.scene_ranges[k].draw_lo = spans[k].draw_lo;
                s.scene_ranges[k].draw_hi = spans[k].draw_hi;
            }
        }

        // Multithreaded fill. Each worker takes a disjoint proxy range
        // [proxy_lo, proxy_hi) and writes draws starting at
        // proxy_first_draw[proxy_lo]. Subchunks are non-overlapping by
        // construction so there's no shared writer state -- the only shared
        // reads are prefab_store_.materials.GetHot / rhi_.resources.BufferBaseOffset,
        // both pure ResourceManager indexed loads with no internal mutation.
        auto fill_chunk = [&](uint32_t proxy_lo, uint32_t proxy_hi) {
            for (uint32_t i = proxy_lo; i < proxy_hi; ++i) {
                const cairns::MeshProxy& mp = s.proxies.meshes[i];
                const BufHandle pos = mp.pos;
                [[maybe_unused]] const BufHandle attr = mp.attr;
                const BufHandle index = mp.index;
                const glm::mat4& world_mat = mp.world_matrix;
                const uint32_t index_base_off =
                    rhi_.resources.BufferBaseOffset(rhi_.alloc, index);
                // Skinned branch resolves once per proxy. mp.skin packs
                // {generation, index} into uint32; resolve via the
                // skinning_.skins pool. Pos stream rebinds to the per-actor
                // skinning_.output_pool slice (mesh-local), attr stream rebinds
                // to mhot->attr_skinned_alias (pre-offset by
                // global_base_vertex * sizeof(VertexAttribute)), and
                // draw.vertex_offset becomes mesh-local
                // (prim.vertex_offset - global_base_vertex).
                bool skinned = false;
                BufHandle skin_pos_buf{};
                BufHandle skin_attr_buf{};
                uint32_t skin_global_base_vertex = 0;
                if (mp.skin != cairns::kInvalidSkin) {
                    cairns::SkinId sid{
                        static_cast<uint16_t>(mp.skin & 0xFFFFu),
                        static_cast<uint16_t>((mp.skin >> 16) & 0xFFFFu)};
                    const cairns::SkinnedAttachment::Hot* sh =
                        skinning_.skins.GetHot(sid);
                    if (sh) {
                        const cairns::Mesh::Hot* mhot_s =
                            prefab_store_.meshes.GetHot(sh->mesh);
                        if (mhot_s &&
                            !mhot_s->attr_skinned_alias.IsNull() &&
                            !sh->pos_stream.IsNull()) {
                            skinned = true;
                            // pos_stream is the per-actor pre-offset alias
                            // of skinning_.output_pool_buffer.
                            skin_pos_buf = sh->pos_stream;
                            skin_attr_buf = mhot_s->attr_skinned_alias;
                            skin_global_base_vertex =
                                mhot_s->global_base_vertex;
                        }
                    }
                }
                uint32_t stable_idx = proxy_first_draw[i];
                for (uint32_t p = 0; p < mp.primitive_count; ++p) {
                    const cairns::PrimitiveProxy& prim =
                        s.proxies.primitives[mp.first_primitive + p];
                    const MatId mat_id = prim.material_id;

                    cairns::Draw draw{};
                    // Per-material set-2 bind group lives in Material::Hot.
                    draw.bind_groups[1] = prefab_store_.materials.GetHot(mat_id)->set2;
                    // Set 2 (per-draw drawtmp UBO) routes through dyn_drawtmp_.
                    draw.dynamic_buffers = dyn_drawtmp_;
                    draw.index_buffer = index;
                    draw.index_offset =
                        index_base_off + (prim.first_index * sizeof(uint32_t));
                    if (skinned) {
                        draw.vertex_offset =
                            prim.vertex_offset -
                            static_cast<int32_t>(skin_global_base_vertex);
                        // Stream-0 alias is pre-baked; no side-channel
                        // byte offset on Draw.
                        draw.vertex_buffers
                            [cairns::Draw::kVertexBufferPosSlot] =
                            skin_pos_buf;
                        draw.vertex_buffers
                            [cairns::Draw::kVertexBufferAttrSlot] =
                            skin_attr_buf;
                    } else {
                        draw.vertex_offset = prim.vertex_offset;
                        draw.vertex_buffers
                            [cairns::Draw::kVertexBufferPosSlot] = pos;
                        draw.vertex_buffers
                            [cairns::Draw::kVertexBufferAttrSlot] = attr;
                    }
                    draw.instance_offset = 0;
                    draw.instance_count = 1;
                    // filled by EncodeDraws.
                    draw.dynamic_buffer_offsets[0] = UINT32_MAX;
                    draw.dynamic_buffer_offsets[1] = UINT32_MAX;
                    assert(prim.index_count % 3 == 0);
                    draw.triangle_count = prim.index_count / 3;

                    // No depth_q in the sort key: a camera-dependent sort
                    // would force a per-viewport re-sort. Material +
                    // pipeline ordering still preserves batching across
                    // both viewports.
                    s.drawListSorted[stable_idx] = std::make_pair(
                        // BuildDrawKey wants a uint32 material id; feed it
                        // Handle::index (uint16, so the 0x3FFFFFFF mask is
                        // a no-op).
                        cairns::BuildDrawKey(
                            static_cast<uint32_t>(mat_id.index) & 0x3FFFFFFFu,
                            /*depth=*/0, kMockTranslucency, kMockViewport,
                            kMockViewportLayer, kMockFullscreenLayer),
                        stable_idx);
                    s.drawList[stable_idx] = draw;
                    s.draw_world_matrices[stable_idx] = world_mat;
                    // Always emit the real entity id; outline.frag does the
                    // highlight-set filter via the highlights texture so pick
                    // can readback the real id from id_target_ regardless of
                    // outline state.
                    s.draw_entity_ids[stable_idx] = mp.entity_id;
                    s.draw_material_ids[stable_idx] = mat_id;
                    ++stable_idx;
                }
            }
        };

        if (n_proxies > 0) {
            uint32_t n_workers = build_pool_->num_workers();
            if (n_workers > n_proxies) {
                n_workers = n_proxies;
            }
            // Range for worker w is [w*N/W, (w+1)*N/W) -- arithmetic
            // partition, no scratch table, no `per+rem` ceremony.
            const uint32_t n_proxies_local = n_proxies;
            const uint32_t n_workers_local = n_workers;
            build_pool_->RunIndices(n_workers, [&](uint32_t w) {
                const uint32_t lo =
                    (n_proxies_local * w) / n_workers_local;
                const uint32_t hi =
                    (n_proxies_local * (w + 1)) / n_workers_local;
                fill_chunk(lo, hi);
            });
        }

        // Stamp FrameStats at end of build. cull_stage_implemented stays
        // false until a real per-proxy frustum cull lands; the cull-counter
        // test reads this flag and skips honestly when cull isn't real.
        {
            FrameStats fs{};
            fs.submitted = n_proxies;
            fs.draw_calls = static_cast<uint32_t>(s.drawList.size());
            uint64_t verts = 0;
            for (const cairns::Draw& d : s.drawList) {
                verts += static_cast<uint64_t>(d.triangle_count) * 3u;
            }
            fs.verts_processed = verts;
            fs.culled = 0;
            fs.cull_stage_implemented = false;
            last_frame_stats_ = fs;
        }

        return true;
    }

}  // namespace cairns

namespace cairns {

bool Engine::GreaterInit(const rhi::InitConfig& cfg, const EngineConfig& ecfg) {
        engine_cfg_ = ecfg;

        // build_draws worker pool. The thread count is a platform concern:
        // native caps at 4 P-cores; the browser returns 0 -> the pool runs
        // the fan-out inline (web build is single-threaded, no -pthread).
        build_pool_ = std::make_unique<cairns::WorkerPool>(
            cairns::platform::WorkerThreadCount());
        // Clock selection: a dump_path OR an explicit use_fixed_clock => the
        // engine runs with FixedClock + readback enabled (golden_=true). Only
        // a non-empty dump_path also turns on the dump-frame-then-exit(0)
        // path (dump_and_exit_) -- so the test harness can ask for the
        // determinism without being killed.
        dump_and_exit_ = !engine_cfg_.dump_path.empty();
        golden_ = engine_cfg_.use_fixed_clock || dump_and_exit_;
        // particles_enabled config -> emitter component on the active scene,
        // installed after InitInitialViewport() below (the scene must exist).

        if (golden_) {
            clock_ = std::make_unique<cairns::FixedClock>(cairns::kFixedDt);
        } else {
            clock_ = std::make_unique<cairns::WallClock>();
        }

        if ( !initCpuAllocators() ) {
            CAIRNS_PRINT("GreaterInit: initCpuAllocators failed\n");
            return false;
        }
        if ( !initResourceManagers() ) {
            CAIRNS_PRINT("GreaterInit: initResourceManagers failed\n");
            return false;
        }

        // Viewport pool must be set up BEFORE the cam_pose override walks
        // it. InitInitialViewport acquires vp0 and primes its
        // layout / viewport_mgr_.active / name table. Runs AFTER initResourceManagers
        // so viewport_mgr_.pool is already Reserve'd onto cpu_block_ (chunk-backed).
        InitInitialViewport();

        // Config-driven default emitter (CLI/serve `particles_enabled`);
        // the active scene + viewport exist by this point.
        if (engine_cfg_.particles_enabled) {
            EnableParticles(true);
        }

        // Pin every viewport's fly controller to the override pose so
        // byte-gate dumps are deterministic.
        if (engine_cfg_.cam_pose.has_value()) {
            const EngineConfig::CamPose& p = *engine_cfg_.cam_pose;
            for (int vi = 0; vi < viewport_mgr_.active_count; ++vi) {
                cairns::FlyController& fc =
                    viewport_mgr_.pool.GetCold(viewport_mgr_.ids[vi])->fly;
                fc.position = glm::vec3(p.x, p.y, p.z);
                fc.yaw = p.yaw;
                fc.pitch = p.pitch;
            }
            viewport_mgr_.cam_pose_override = true;
        }

        if (!rhi_.device.Init(cfg)) {
            CAIRNS_PRINT("GreaterInit: device.Init failed\n");
            return false;
        }
        // Boot compute floor: refuse devices below the 10-storage-buffer/stage
        // minimum (device_caps.hpp; we use 6, 10 is headroom). The storage SIZE
        // floor is per-platform (128 MB Android / 256 MB else = the skin pool) and
        // is enforced by SkinPoolFitsDevice just below.
        if (!cairns::DeviceMeetsComputeRequirements(rhi_.device.caps)) {
            CAIRNS_PRINT_ERR(
                "[FATAL] device below compute floor: storage_buffers_per_stage=%u "
                "(need >=%u).\n",
                rhi_.device.caps.max_storage_buffers_per_stage,
                cairns::kMinStorageBuffersPerStage);
            std::abort();
        }
        // Boot device-cap invariant: a pool bound past
        // maxStorageBufferRange (256 MB on Adreno 730) is silent no-op
        // writes -- abort loudly instead. The skin-pool size is sourced
        // from MemoryBudget, the single per-platform reservation table.
        const uint32_t kSkinOutputBytes = static_cast<uint32_t>(
            cairns::MemoryBudget::Default().gpu_skin_pool_bytes);
        {
            const uint32_t kSkinOutputBytesCheck = kSkinOutputBytes;
            if (!cairns::SkinPoolFitsDevice(kSkinOutputBytesCheck,
                                            rhi_.device.caps)) {
                CAIRNS_PRINT_ERR(
                    "[FATAL] kSkinOutputBytes=%u exceeds device "
                    "max_storage_buffer_range=%u. The skin pool would "
                    "be bound past the addressable range and writes "
                    "past it would silently no-op (Adreno 730 floor "
                    "is 256 MB).\n",
                    kSkinOutputBytesCheck,
                    rhi_.device.caps.max_storage_buffer_range);
                std::abort();
            }
        }
        if (!rhi_.alloc.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: alloc.Init failed\n");
            return false;
        }
        if (!rhi_.resources.Init(rhi_.device, cpu_block_)) {
            CAIRNS_PRINT("GreaterInit: resources.Init failed\n");
            return false;
        }
        // Persistent skin output pool. Buffer is private-heap (kDefault);
        // RangePool measures slices in vec4 vertex units. Sized once at
        // init; exhaustion fails loudly (Alloc returns an invalid slice).
        // >kHeapBlockBytes (128 MB) drops into the dedicated-block path in
        // MemoryAllocator::AllocBuffer, so we land in our own VkDeviceMemory.
        {
            // kSkinOutputBytes from MemoryBudget (above). 128 MB mobile (Adreno
            // 730 maxStorageBufferRange floor) / 1 GB desktop.
            rhi::BufferDesc bd{};
            bd.byte_size = kSkinOutputBytes;
            bd.usage = rhi::kUsageStorage | rhi::kUsageVertex;
            bd.memory = rhi::Memory::kDefault;
            skinning_.output_pool_buffer = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
            if (skinning_.output_pool_buffer.IsNull()) {
                CAIRNS_PRINT("GreaterInit: skinning_.output_pool_buffer alloc failed\n");
                return false;
            }
            // Slices in vec4 units (16 B). Capacity = total / 16.
            static_assert(sizeof(glm::vec4) == cairns::kSkinVertexStride,
                          "skin vertex stride must equal sizeof(glm::vec4); "
                          "RangePool offsets are scaled by kSkinVertexStride "
                          "on bind.");
            skinning_.output_pool.Init(kSkinOutputBytes / 16u);
        }
        // Persistent palette out + world scratch for GPU palette eval.
        // 1024 actors * 256 mat4 = 16 MB each.
        {
            rhi::BufferDesc bd{};
            bd.usage = rhi::kUsageStorage;
            bd.memory = rhi::Memory::kDefault;
            bd.byte_size = kAnimActorsCap * kAnimMaxJoints * 64u;
            skinning_.palette_out_buf = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
            bd.byte_size = kAnimActorsCap * kAnimMaxNodes * 64u;
            skinning_.world_scratch_buf = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
            if (skinning_.palette_out_buf.IsNull() || skinning_.world_scratch_buf.IsNull()) {
                CAIRNS_PRINT("GreaterInit: anim_eval persistent buffers alloc failed\n");
                return false;
            }
        }
        // Sibling subsystems init before frames; Pipelines owns the
        // descriptor set layouts, so it must precede frames.Init.
        if (!rhi_.gpu_profiler.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: gpu_profiler.Init failed\n");
            return false;
        }
        rhi_.offscreen_targets.Init(rhi_.device);
        if (!rhi_.pipelines.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: pipelines.Init failed\n");
            return false;
        }
        if (!rhi_.frames.Init(rhi_.device, rhi_.pipelines)) {
            CAIRNS_PRINT("GreaterInit: frames.Init failed\n");
            return false;
        }
        // dyn_globals_ + dyn_drawtmp_: per-FIF descriptor sets for unlit's
        // set 0 + set 2, backed by the kDynamic master.
        // Must run AFTER frames.Init (needs descriptor_pool_) and BEFORE
        // initRenderPipeline (the unlit PSO references their layout).
        {
            cairns::rhi::DynamicBinding gb{};
            gb.slot = 0;
            gb.kind = cairns::rhi::BufferKind::kUniform;
            gb.max_range = sizeof(cairns::rhi::RenderPassGlobals);
            gb.stages = static_cast<cairns::rhi::ShaderStage>(
                cairns::rhi::kStageVertex | cairns::rhi::kStageFragment);
            cairns::rhi::DynamicBuffersDesc gd{};
            gd.debug_name = "dyn_globals";
            gd.bindings =
                std::span<const cairns::rhi::DynamicBinding>(&gb, 1);
            dyn_globals_ = rhi_.resources.CreateDynamicBuffers(rhi_.alloc,
                                                                 rhi_.frames, gd);
            cairns::rhi::DynamicBinding db{};
            db.slot = 0;
            db.kind = cairns::rhi::BufferKind::kUniform;
            db.max_range = sizeof(cairns::rhi::DrawTmp);
            db.stages = static_cast<cairns::rhi::ShaderStage>(
                cairns::rhi::kStageVertex | cairns::rhi::kStageFragment);
            cairns::rhi::DynamicBuffersDesc dd{};
            dd.debug_name = "dyn_drawtmp";
            dd.bindings =
                std::span<const cairns::rhi::DynamicBinding>(&db, 1);
            dyn_drawtmp_ = rhi_.resources.CreateDynamicBuffers(rhi_.alloc,
                                                                 rhi_.frames, dd);
            // Post-effect params: same shape (dyn UBO over the kDynamic
            // master, VERTEX|FRAGMENT so the set stays layout-compatible
            // with globals_set_layout_ on vk).
            cairns::rhi::DynamicBinding pb{};
            pb.slot = 0;
            pb.kind = cairns::rhi::BufferKind::kUniform;
            pb.max_range = 64;
            pb.stages = static_cast<cairns::rhi::ShaderStage>(
                cairns::rhi::kStageVertex | cairns::rhi::kStageFragment);
            cairns::rhi::DynamicBuffersDesc pd{};
            pd.debug_name = "dyn_postfx";
            pd.bindings =
                std::span<const cairns::rhi::DynamicBinding>(&pb, 1);
            dyn_postfx_ = rhi_.resources.CreateDynamicBuffers(rhi_.alloc,
                                                               rhi_.frames, pd);
            if (dyn_globals_.IsNull() || dyn_drawtmp_.IsNull() ||
                dyn_postfx_.IsNull()) {
                CAIRNS_PRINT("GreaterInit: DynamicBuffers create failed\n");
                return false;
            }
        }
        if (!cfg.surfaceless) {
            if ( !initSwapChain(cfg)) {
                CAIRNS_PRINT("GreaterInit: initSwapChain failed\n");
                return false;
            }
        }
        { // init debug assets
            // prefab_store_.glb_paths is appended by LoadPrefabBatch via
            // AppendGlbPaths (the manifest line). GreaterInit just
            // builds a local list of paths from CAIRNS_GLB overrides
            // (if any) and hands them to LoadPrefabBatch; the
            // [PICK] log's name-by-filename works for both boot-
            // override prefabs AND runtime cairns.prefab.load.
            std::vector<std::filesystem::path> glb_paths;
            if (!engine_cfg_.glb_overrides.empty()) {
                for (const std::string& tok : engine_cfg_.glb_overrides) {
                    if (tok.empty()) {
                        continue;
                    }
                    std::filesystem::path p(tok);
                    if (p.is_absolute()) {
                        glb_paths.push_back(p);
                    } else {
                        std::filesystem::path resolved;
                        if (!cairns::GetStaticResourceFilepath(tok, resolved)) {
                            return false;
                        }
                        glb_paths.push_back(resolved);
                    }
                }
            }
            // NO IMPLICIT BOOT LOAD: default (empty glb_overrides) leaves
            // the prefab pool empty; the agent calls cairns.prefab.load
            // when it needs a Prefab. Boot is sub-second.
            if (!glb_paths.empty()) {
                LoadPrefabBatch(std::span<const std::filesystem::path>(
                    glb_paths.data(), glb_paths.size()));
            }

            {
                uint64_t total_skin_verts = 0;
                uint32_t skin_mesh_count = 0;
                uint32_t max_vert_per_mesh = 0;
                prefab_store_.meshes.ForEachLive(
                    [&](cairns::Mesh::Hot& mhot, cairns::Mesh::Cold&) {
                        if (mhot.attr_skinned_alias.IsNull() ||
                            ResolvedSharedSkin(mhot).IsNull() ||
                            mhot.vert_count == 0) {
                            return;
                        }
                        total_skin_verts += mhot.vert_count;
                        ++skin_mesh_count;
                        if (mhot.vert_count > max_vert_per_mesh) {
                            max_vert_per_mesh = mhot.vert_count;
                        }
                    });
                uint32_t max_joints = 0;
                uint32_t max_nodes = 0;
                uint32_t scene_count = 0;
                prefab_store_.prefabs.ForEachLive(
                    [&](cairns::Prefab::Hot&, cairns::Prefab::Cold& c) {
                        ++scene_count;
                        if (c.nodes.size() > max_nodes) {
                            max_nodes = static_cast<uint32_t>(c.nodes.size());
                        }
                        for (const cairns::Skin& s : c.skins) {
                            if (s.jointNodes.size() > max_joints) {
                                max_joints = static_cast<uint32_t>(
                                    s.jointNodes.size());
                            }
                        }
                    });
                CAIRNS_PRINT(
                    "[WORKLOAD] scenes=%u skinned_meshes=%u total_skin_verts=%llu "
                    "max_vert_per_mesh=%u max_joints=%u max_nodes=%u\n",
                    scene_count, skin_mesh_count,
                    static_cast<unsigned long long>(total_skin_verts),
                    max_vert_per_mesh, max_joints, max_nodes);
            }

            // CleanupTmps + per-mesh CPU clear happen inside
            // LoadPrefabBatch so every batch gets the same hygiene.
        }

        // EnTT scene-layer path. Acquire scene_mgr_.active + scene_mgr_.secondary
        // ALWAYS (regardless of prefab count): InstantiatePrefab looks up
        // scene_mgr_.pool.GetCold(scene_mgr_.active) and bails to entity:0
        // if it's null. scene_mgr_.pool is Reserved(cpu_block_, kMaxScenes)
        // in initResourceManagers -- block-backed, Cold* stable (no realloc
        // up to the cap), hashable.
        scene_mgr_.active = scene_mgr_.pool.Acquire();
        scene_mgr_.primary = scene_mgr_.active;  // index-0; UseScene may move active_
        if (cairns::Scene::Hot* wh = scene_mgr_.pool.GetHot(scene_mgr_.active)) {
            if (cairns::Scene::Cold* wc =
                    scene_mgr_.pool.GetCold(scene_mgr_.active)) {
                *wc = cairns::Scene::Cold{};
                // Re-seat the entt registry onto cpu_block_ (the default
                // Cold{} gives it the null-arena malloc fallback).
                wc->registry = cairns::Scene::Registry(
                    cairns::ChunkStdAllocator<entt::entity>(cpu_block_));
                wh->proxy_slot = 0;
                wh->dirty = true;
            }
        }
        // InitInitialViewport() acquired vp0 BEFORE scene_mgr_.active existed, so
        // its scene handle is stale-null. Bind it now that scene_mgr_.active is
        // real -- the per-viewport draw fan-out extracts each viewport's
        // bound scene, so a stale bind renders nothing.
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            if (cairns::Viewport::Hot* vh =
                    viewport_mgr_.pool.GetHot(viewport_mgr_.ids[v])) {
                vh->scene = scene_mgr_.active;
            }
        }
        scene_mgr_.proxies.resize(1);

        // Multi-scene coexistence: the secondary slot is acquired but
        // left empty.
        scene_mgr_.secondary = scene_mgr_.pool.Acquire();
        if (cairns::Scene::Hot* wh2 = scene_mgr_.pool.GetHot(scene_mgr_.secondary)) {
            if (cairns::Scene::Cold* wc2 =
                    scene_mgr_.pool.GetCold(scene_mgr_.secondary)) {
                *wc2 = cairns::Scene::Cold{};
                wc2->registry = cairns::Scene::Registry(
                    cairns::ChunkStdAllocator<entt::entity>(cpu_block_));
                wh2->proxy_slot = 1;
                wh2->dirty = true;
            }
        }
        scene_mgr_.proxies.resize(2);
        // Surfaceless mode: allocate the offscreen present_.final_target and CONTINUE
        // through normal init. The engine -- not the RHI -- is the one that
        // decides which texture the swap pass writes into each frame: in
        // surfaceless mode it builds a SwapResolveTarget pointing at
        // present_.final_target; in windowed mode it pulls one out of present_.swapchain.
        // SwapChain and Frames have no notion of "headless" mode.
        if (cfg.surfaceless) {
            present_.final_target_w = cfg.width;
            present_.final_target_h = cfg.height;
            rhi::TextureDesc td{};
            td.debug_name = "final_target";
            td.dimensions = {static_cast<int32_t>(cfg.width),
                             static_cast<int32_t>(cfg.height), 1};
            td.format = rhi::Format::kBgra8Unorm;
            td.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled |
                       rhi::kTexUsageTransferSrc | rhi::kTexUsageTransferDst;
            td.memory = rhi::Memory::kDefault;
            present_.final_target = rhi_.resources.CreateTexture(rhi_.alloc, td);
            if (present_.final_target.IsNull()) {
                CAIRNS_PRINT("GreaterInit: present_.final_target create failed\n");
                return false;
            }
        }
        if ( !initRenderPipeline() ) {
            CAIRNS_PRINT("GreaterInit: initRenderPipeline failed\n");
            return false;
        }
        const uint32_t init_w = cfg.surfaceless ? cfg.width : present_.swapchain.Width();
        const uint32_t init_h = cfg.surfaceless ? cfg.height : present_.swapchain.Height();
        if ( !rhi_.frames.InitTargets(rhi_.resources, rhi_.alloc, init_w, init_h) ) {
            CAIRNS_PRINT("GreaterInit: frames.InitTargets failed\n");
            return false;
        }
        initSkinKernel();  // best-effort; missing shader doesn't fail GreaterInit.
        initAnimEvalKernel();  // best-effort; failure -> GPU palette eval off.
        uploadAnimTablesGpu();  // flattens + uploads all scene tables.
        // Skins were Acquired BEFORE this call, so their cached
        // gpu_prefab_header_idx (UINT32_MAX) is stale. Backfill from each
        // skin's scene now that the headers exist.
        skinning_.skins.ForEachLive(
            [&](cairns::SkinnedAttachment::Hot& h,
                cairns::SkinnedAttachment::Cold& c) {
                cairns::Prefab::Hot* sht = prefab_store_.prefabs.GetHot(c.scene);
                if (sht) {
                    h.gpu_prefab_header_idx = sht->gpu_prefab_header_idx;
                }
            });
        // textureHandles never change after scene load; build the
        // engine-side prefab_store_.resident_textures once here. Per-frame
        // draw() just points the packet span at this vector.
        prefab_store_.resident_textures.clear();
        for (cairns::PrefabId sid : prefab_store_.prefab_ids) {
            cairns::Prefab::Cold* scold = prefab_store_.prefabs.GetCold(sid);
            if (!scold) {
                continue;
            }
            for (rhi::Handle<rhi::Texture> th : scold->textureHandles) {
                prefab_store_.resident_textures.push_back(th);
            }
        }
        // Skin Group B + anim_eval route through DynamicBuffers, created
        // post-scene-load so the backing handles exist. Descriptor sets
        // allocated here are layout-compatible with the pipeline's set 0
        // layout (built earlier from frames.plat.skin_group_b_layout_ /
        // anim_eval_layout_) because the per-binding (type, count, stage)
        // tuple matches exactly. Binding 1 (palettes) is backed by the
        // persistent skinning_.palette_out_buf (anim_eval writes it);
        // the per-batch dynamic offset still selects the bucket's palette
        // window. When skinning_.eval_tables_uploaded is false, no backing
        // => kDynamic master fallback.
        if (!skinning_.skin_kernel.IsNull() && !skinning_.output_pool_buffer.IsNull()) {
            cairns::rhi::DynamicBinding gb[4]{};
            for (uint32_t i = 0; i < 4; ++i) {
                gb[i].stages = cairns::rhi::kStageCompute;
            }
            gb[0].slot = 0;
            gb[0].kind = cairns::rhi::BufferKind::kUniform;
            gb[0].max_range = 64u;
            gb[0].has_dynamic_offset = true;
            gb[1].slot = 1;
            gb[1].kind = cairns::rhi::BufferKind::kStorage;
            gb[1].max_range = 1u << 20;
            gb[1].has_dynamic_offset = true;
            if (skinning_.eval_tables_uploaded) {
                gb[1].backing = skinning_.palette_out_buf;
            }
            gb[2].slot = 2;
            gb[2].kind = cairns::rhi::BufferKind::kStorage;
            gb[2].max_range = 16384u;
            gb[2].has_dynamic_offset = true;
            gb[3].slot = 3;
            gb[3].kind = cairns::rhi::BufferKind::kStorage;
            gb[3].max_range = 0;  // VK_WHOLE_SIZE
            gb[3].has_dynamic_offset = false;
            gb[3].backing = skinning_.output_pool_buffer;
            cairns::rhi::DynamicBuffersDesc gd{};
            gd.debug_name = "dyn_skin_group_b";
            gd.bindings =
                std::span<const cairns::rhi::DynamicBinding>(gb, 4);
            skinning_.dyn_skin_group_b =
                rhi_.resources.CreateDynamicBuffers(rhi_.alloc, rhi_.frames, gd);
            if (skinning_.dyn_skin_group_b.IsNull()) {
                CAIRNS_PRINT("GreaterInit: dyn_skin_group_b create failed\n");
                return false;
            }
        }
        // recreateAnimDynBindings() is the ONE creation path for
        // skinning_.dyn_anim_eval: here, after the first runtime load
        // (skinning_.eval_tables_uploaded flips false->true), and after any
        // anim buffer is destroyed+recreated on growth. At boot this call
        // is a no-op (eval_tables_uploaded is false); uploadAnimTablesGpu
        // calls the helper once buffers exist.
        if (!recreateAnimDynBindings()) {
            return false;
        }
        // Globals + drawtmp DYNAMIC UBO descriptors point at the master
        // kDynamic buffer with sizeof(struct) range; per-pass bind supplies
        // the offset. Written ONCE here, never per frame.
        rhi_.frames.WriteUnlitDescriptors(rhi_.resources, rhi_.alloc);
        if ( !initParticles() ) {
            CAIRNS_PRINT("GreaterInit: initParticles failed\n");
            return false;
        }
        // A background render worker on native; inline (record at Submit on
        // the calling thread) when the platform runs single-threaded (the
        // browser -- same signal as the build pool: 0 worker threads =>
        // fully inline).
        const bool background_render = cairns::platform::WorkerThreadCount() > 0;
        render_thread_ = std::make_unique<cairns::RenderThread>(
            [this](cairns::FramePacket& pkt) { this->RecordFrame(pkt); },
            background_render);
        return true;
    }

}  // namespace cairns

namespace cairns {

bool Engine::initResourceManagers() {
        using namespace cairns;
        using namespace cairns::rhi;
        // Reserve the engine's order-stable parallel lists to the residency
        // target up front, so a batch load doesn't walk a vector-doubling
        // chain.
        constexpr size_t kPrefabResidencyCap = 600;
        // POD loose vectors re-seat + reserve onto cpu_block_.
        ReseatOnBlock(prefab_store_.prefab_ids, kPrefabResidencyCap);
        ReseatOnBlock(prefab_store_.per_prefab_asset, kPrefabResidencyCap);
        ReseatOnBlock(material_dedup_, 8192);
        ReseatOnBlock(effect_textures_, 32);
        ReseatOnBlock(prefab_store_.resident_textures, kPrefabResidencyCap * 4);
        prefab_store_.glb_paths.reserve(kPrefabResidencyCap);  // path strings stay heap (not interned)
        prefab_store_.per_batch_shared_skin.reserve(64);

        // Reserve the persistent ResourceManager pools onto cpu_block_
        // (size==capacity -> span-hashable; counted in the budget). Caps are
        // upper bounds for the 600-GLB residency ceiling; exceeding one aborts
        // in Acquire (chunk-backed, never grows). InitInitialViewport runs
        // AFTER this so viewport_mgr_.pool is chunk-backed too.
        prefab_store_.prefabs.Reserve(cpu_block_, static_cast<uint16_t>(kPrefabResidencyCap));
        prefab_store_.meshes.Reserve(cpu_block_, 8192);
        prefab_store_.materials.Reserve(cpu_block_, 8192);
        skinning_.skins.Reserve(cpu_block_, 4096);
        scene_mgr_.pool.Reserve(cpu_block_, static_cast<uint16_t>(kMaxScenes));
        viewport_mgr_.pool.Reserve(cpu_block_, 16);
        scene_mgr_.assets.Pool().Reserve(cpu_block_, 1024);
        return true;
    }

}  // namespace cairns

namespace cairns {

void Engine::InitInitialViewport() {
        if (viewport_mgr_.active_count > 0 && !viewport_mgr_.ids[0].IsNull()) {
            return;
        }
        cairns::ViewportId id = viewport_mgr_.pool.Acquire();
        if (auto* h = viewport_mgr_.pool.GetHot(id)) {
            *h = cairns::Viewport::Hot{};
            h->layout_rect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
            h->scene = scene_mgr_.active;
        }
        if (auto* c = viewport_mgr_.pool.GetCold(id)) {
            *c = cairns::Viewport::Cold{};
        }
        viewport_mgr_.ids[0] = id;
        viewport_mgr_.active_count = 1;
        viewport_mgr_.active = id;
        viewport_mgr_.active_index = 0;
        const uint32_t name = viewport_mgr_.next_name++;
        viewport_mgr_.names[viewport_mgr_.names_count++] = ViewportName{name, id};
    }

}  // namespace cairns

namespace cairns {

bool Engine::initCpuAllocators() {
        // Reserve the one CPU block up front (fail-loud, no silent malloc
        // past budget); sub-regions are carved from it below.
        const cairns::MemoryBudget mb = cairns::MemoryBudget::Default();
        cpu_block_.InitReserved(mb.cpu_persistent_bytes);
        for (PerSlot& s : slots_) {
            // Carve the per-frame slab from the block (kRegionFrame) so the
            // arena's [0,Used) lands in the hashable block. 16 MB >
            // chunk_bytes -> the block routes it to its own malloc; the
            // arena data is hashed directly regardless.
            void* slab = cpu_block_.Allocate(
                static_cast<uint32_t>(kArenaBytesPerSlot), 16, kRegionFrame);
            // Zero the slab: alignment padding is hashed with [0,Used), so
            // it must be deterministic, not fresh-malloc garbage (oversize
            // block allocs aren't 0xCC-prefilled like the chunk pool).
            std::memset(slab, 0, kArenaBytesPerSlot);
            s.arena.Init(slab, kArenaBytesPerSlot);
        }
        // Carve the persistent prefab interning arena (names + nested
        // load tables). Fixed 96 MB (the measured 100-GLB footprint is ~58 MB);
        // oversize => its own malloc, so it does NOT eat the 256 MB mobile chunk
        // reservation. Overflow fails loud via AllocSliceOrDie. Fail loud here
        // too if the malloc itself failed (Android OOM) rather than writing
        // through a null base. Zero-filled for deterministic padding.
        void* prefab_slab = cpu_block_.Allocate(
            static_cast<uint32_t>(kPrefabArenaBytes), 16, kRegionPersistent);
        if (prefab_slab == nullptr) {
            CAIRNS_PRINT_ERR("[PREFAB-ARENA] FATAL: could not reserve %zu MiB\n",
                             kPrefabArenaBytes / (1024 * 1024));
            std::abort();
        }
        std::memset(prefab_slab, 0, kPrefabArenaBytes);
        prefab_arena_.Init(prefab_slab, kPrefabArenaBytes);
        return true;
    }

}  // namespace cairns

namespace cairns {

bool Engine::initSwapChain(const rhi::InitConfig& cfg) {
        if ( !rhi_.device.InitSwapChain(present_.swapchain, cfg)) {
            return false;
        }

        return true;
    }

}  // namespace cairns

namespace cairns {

cairns::SkinId Engine::TryCreateSkinForScene(cairns::PrefabId scene_id,
                                          float time_offset) {
        // Loud reason for every Null return so we don't silently
        // drop heroes to bind pose. Names the scene so the user can map
        // back to a GLB filename via prefab_store_.prefab_ids[scene_idx].
        auto fail = [&](const char* why) -> cairns::SkinId {
            CAIRNS_PRINT_ERR(
                "[SKIN-FAIL] scene_id=(idx=%u,gen=%u) reason=%s\n",
                static_cast<unsigned>(scene_id.index),
                static_cast<unsigned>(scene_id.generation), why);
            return cairns::SkinId::Null;
        };
        cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(scene_id);
        cairns::Prefab::Cold* scold = prefab_store_.prefabs.GetCold(scene_id);
        if (!shot || !scold) {
            return fail("scene handle dead");
        }
        if (scold->skins.empty()) {
            return fail("scold.skins empty");
        }
        if (scold->clips.empty()) {
            return fail("scold.clips empty");
        }
        const int clip_idx =
            cairns::SelectWalkingClip(scold->clips, prefab_arena_);
        if (clip_idx < 0) {
            return fail("SelectWalkingClip returned -1");
        }
        cairns::Handle<cairns::Mesh> skinned_mesh;
        uint32_t vert_count = 0;
        for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
            cairns::Mesh::Hot* mhot = prefab_store_.meshes.GetHot(mid);
            if (mhot && !mhot->attr_skinned_alias.IsNull() &&
                mhot->vert_count > 0) {
                skinned_mesh = mid;
                vert_count = mhot->vert_count;
                break;
            }
        }
        if (skinned_mesh.IsNull() || vert_count == 0) {
            return fail("no mesh with attr_skinned_alias + vert_count");
        }
        cairns::PoolSlice slice = skinning_.output_pool.Alloc(vert_count);
        if (!slice.IsValid()) {
            // Graceful, not fatal -- over-cap actors render in bind pose
            // (the caller skips SkinRef on a Null skin) instead of aborting the
            // whole app. 256 MB = Adreno maxStorageBufferRange floor.
            return fail("skinning output pool exhausted (256 MB cap) -- "
                        "actor falls back to bind pose");
        }
        cairns::SkinId sid = skinning_.skins.Acquire();
        cairns::Prefab::Hot* scene_hot = prefab_store_.prefabs.GetHot(scene_id);
        // Build the per-actor pos_stream alias of
        // skinning_.output_pool_buffer, pre-offset to slice.offset * 16 B.
        // Skinned draws point Draw::vertex_buffers[0] at this handle; no
        // side-channel byte offset on Draw.
        rhi::Handle<rhi::Buffer> pos_stream_h =
            rhi::Handle<rhi::Buffer>::Null;
        if (!skinning_.output_pool_buffer.IsNull()) {
            // CRITICAL: snapshot pool fields BEFORE the next Acquire.
            // ResourceManager::Acquire does hot_.emplace_back() which may
            // reallocate the underlying std::vector -- any Hot* fetched
            // earlier becomes dangling. The skinned-rendering "exploded
            // triangles" regression was exactly this UB read.
            uint16_t pool_heap_idx = 0;
            uint32_t pool_off = 0;
            {
                rhi::Buffer::Hot* pool_hot =
                    rhi_.resources.GetHot(skinning_.output_pool_buffer);
                if (!pool_hot) {
                    return cairns::SkinId::Null;
                }
                pool_heap_idx = pool_hot->heap_buffer_index;
                pool_off = pool_hot->offset_in_heap;
            }
            pos_stream_h = rhi_.resources.buffers.Acquire();
            rhi::Buffer::Hot* alias_hot =
                rhi_.resources.buffers.GetHot(pos_stream_h);
            alias_hot->heap_buffer_index = pool_heap_idx;
            alias_hot->offset_in_heap =
                pool_off +
                slice.offset * static_cast<uint32_t>(sizeof(glm::vec4));
        }
        if (auto* h = skinning_.skins.GetHot(sid)) {
            *h = cairns::SkinnedAttachment::Hot{};
            h->slice_offset = slice.offset;
            h->joint_count =
                static_cast<uint32_t>(scold->skins[0].jointNodes.size());
            h->time_offset = time_offset;
            h->time_scale = 1.0f;
            h->mesh = skinned_mesh;
            // Cached at create so the frame path skips a double-resolve.
            h->gpu_prefab_header_idx =
                scene_hot ? scene_hot->gpu_prefab_header_idx : UINT32_MAX;
            h->gpu_clip_duration =
                (scold->gpu_clip_duration > 0.0f) ? scold->gpu_clip_duration
                                                   : 1.0f;
            h->pos_stream = pos_stream_h;
        }
        if (auto* c = skinning_.skins.GetCold(sid)) {
            *c = cairns::SkinnedAttachment::Cold{};
            c->scene = scene_id;
            c->skin_index = 0;
            c->clip_index = clip_idx;
            c->slice = slice;  // full slice lives Cold; only Free reads it
        }
        return sid;
    }

}  // namespace cairns

namespace cairns {

void Engine::ApplyPendingResize() {
        const uint32_t cur_w = present_.final_target.IsNull() ? present_.swapchain.Width()
                                                       : present_.final_target_w;
        const uint32_t cur_h = present_.final_target.IsNull() ? present_.swapchain.Height()
                                                       : present_.final_target_h;
        const bool dims_drifted = (cur_w != present_.last_seen_swap_w) ||
                                   (cur_h != present_.last_seen_swap_h);
        if (!present_.resize_pending && !dims_drifted) {
            return;
        }
        if (render_thread_) {
            render_thread_->Drain();
        }
        rhi_.device.WaitIdle();
        rhi_.offscreen_targets.FlushFramebuffers();
        if (!present_.final_target.IsNull() &&
            (present_.resize_pending_w != present_.final_target_w ||
             present_.resize_pending_h != present_.final_target_h) &&
            present_.resize_pending_w != 0 && present_.resize_pending_h != 0) {
            ResizeFinalTarget(present_.resize_pending_w, present_.resize_pending_h);
        }
        present_.last_seen_swap_w = present_.final_target.IsNull() ? present_.swapchain.Width()
                                                    : present_.final_target_w;
        present_.last_seen_swap_h = present_.final_target.IsNull() ? present_.swapchain.Height()
                                                    : present_.final_target_h;
        present_.resize_pending = false;
    }

}  // namespace cairns

namespace cairns {

bool Engine::ResizeFinalTarget(uint32_t w, uint32_t h) {
        if (present_.final_target.IsNull()) {
            return false;
        }
        rhi_.resources.Destroy(rhi_.alloc, present_.final_target);
        present_.final_target = rhi::Handle<rhi::Texture>::Null;
        present_.final_target_w = w;
        present_.final_target_h = h;
        rhi::TextureDesc td{};
        td.debug_name = "final_target";
        td.dimensions = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
        td.format = rhi::Format::kBgra8Unorm;
        td.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled |
                   rhi::kTexUsageTransferSrc | rhi::kTexUsageTransferDst;
        td.memory = rhi::Memory::kDefault;
        present_.final_target = rhi_.resources.CreateTexture(rhi_.alloc, td);
        return !present_.final_target.IsNull();
    }

}  // namespace cairns

namespace cairns {

void Engine::EnsureHighlightsTex() {
        if (!picking_.highlights_tex.IsNull() &&
            picking_.highlights_tex_rev == picking_.highlights_rev) {
            return;
        }
        if (!picking_.highlights_tex.IsNull()) {
            rhi_.resources.Destroy(rhi_.alloc, picking_.highlights_tex);
            picking_.highlights_tex = rhi::Handle<rhi::Texture>::Null;
        }
        std::array<uint32_t, kMaxHighlights + 1> pack{};
        const uint32_t n =
            static_cast<uint32_t>(std::min<size_t>(picking_.highlights.size(),
                                                   kMaxHighlights));
        pack[0] = n;
        for (uint32_t i = 0; i < n; ++i) {
            pack[i + 1] = picking_.highlights[i].id;
        }
        rhi::TextureDesc td{};
        td.debug_name = "highlights_tex";
        td.dimensions = {static_cast<int32_t>(kMaxHighlights + 1), 1, 1};
        td.format = rhi::Format::kR32Uint;
        td.usage = rhi::kTexUsageSampled | rhi::kTexUsageTransferDst;
        td.memory = rhi::Memory::kDefault;
        td.initial_data = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(pack.data()),
            sizeof(uint32_t) * pack.size());
        picking_.highlights_tex = rhi_.resources.CreateTexture(rhi_.alloc, td);
        picking_.highlights_tex_rev = picking_.highlights_rev;
    }

}  // namespace cairns

namespace cairns {

void Engine::EnsureIdTargets(uint32_t w, uint32_t h) {
        if (w == id_target_w_ && h == id_target_h_ &&
            !id_target_[0].IsNull()) {
            return;
        }
        for (int v = 0; v < kNumViewports; ++v) {
            if (!id_target_[v].IsNull()) {
                rhi_.resources.Destroy(rhi_.alloc, id_target_[v]);
                id_target_[v] = rhi::Handle<rhi::Texture>::Null;
            }
        }
        id_target_w_ = w;
        id_target_h_ = h;
        for (int v = 0; v < kNumViewports; ++v) {
            rhi::TextureDesc td{};
            td.debug_name = "id_target";
            td.dimensions = {static_cast<int32_t>(w),
                             static_cast<int32_t>(h), 1};
            td.format = rhi::Format::kR32Uint;
            td.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled |
                       rhi::kTexUsageTransferSrc;
            td.memory = rhi::Memory::kDefault;
            id_target_[v] = rhi_.resources.CreateTexture(rhi_.alloc, td);
        }
    }

}  // namespace cairns

namespace cairns {

uint32_t Engine::ResolvePickRaycast(int vp, uint32_t px, uint32_t py,
                                const glm::mat4& inv_view_proj) {
        // Pick the CLICKED viewport's bound scene, not the globally-active
        // one -- clicking node J must resolve against node J's document.
        // Falls back to active for an out-of-range vp.
        cairns::SceneId pick_scene = scene_mgr_.active;
        if (vp >= 0 && vp < viewport_mgr_.active_count) {
            if (auto* vh = viewport_mgr_.pool.GetHot(viewport_mgr_.ids[vp])) {
                pick_scene = vh->scene;
            }
        }
        cairns::Scene::Cold* wc = scene_mgr_.pool.GetCold(pick_scene);
        if (!wc) { return 0u; }
        const float fw = static_cast<float>(FrameWidth());
        const float fh = static_cast<float>(FrameHeight());
        if (fw <= 0.0f || fh <= 0.0f) { return 0u; }
        const float ndc_x = (static_cast<float>(px) / fw) * 2.0f - 1.0f;
        const float ndc_y = 1.0f - (static_cast<float>(py) / fh) * 2.0f;
        // WebGPU/Metal clip space is z in [0,1]: near plane z=0, far z=1.
        const glm::vec4 nc = inv_view_proj * glm::vec4(ndc_x, ndc_y, 0.0f, 1.0f);
        const glm::vec4 fc = inv_view_proj * glm::vec4(ndc_x, ndc_y, 1.0f, 1.0f);
        const glm::vec3 ro = glm::vec3(nc) / nc.w;
        const glm::vec3 rf = glm::vec3(fc) / fc.w;
        const glm::vec3 rd = glm::normalize(rf - ro);
        const glm::vec3 inv_d = 1.0f / rd;  // 0-component -> inf, slab test handles it
        float best_t = 1.0e30f;
        uint32_t best_id1 = 0u;
        auto pick_view = wc->registry.view<const cairns::WorldTransform,
                                           const cairns::AssetRef>();
        for (entt::entity e : pick_view) {
            const cairns::AssetRef& ref = pick_view.get<const cairns::AssetRef>(e);
            cairns::Asset::Cold* ac = scene_mgr_.assets.Pool().GetCold(ref.asset);
            if (!ac) { continue; }
            cairns::Prefab::Hot* sh = prefab_store_.prefabs.GetHot(ac->cpu_graph);
            if (!sh || sh->meshes.empty()) { continue; }
            // Union EVERY mesh's bind AABB -- a first-mesh-only box leaves
            // the rest of a multi-mesh prefab unclickable. Local-space
            // union; transformed below.
            glm::vec3 lmin(1.0e30f);
            glm::vec3 lmax(-1.0e30f);
            for (cairns::Handle<cairns::Mesh> mid : sh->meshes) {
                cairns::Mesh::Hot* mh = prefab_store_.meshes.GetHot(mid);
                if (!mh || mh->bind_aabb_min.x > mh->bind_aabb_max.x) {
                    continue;
                }
                lmin = glm::min(lmin, mh->bind_aabb_min);
                lmax = glm::max(lmax, mh->bind_aabb_max);
            }
            if (lmin.x > lmax.x) { continue; }  // no mesh had a valid AABB
            const glm::mat4& world =
                pick_view.get<const cairns::WorldTransform>(e).world;
            glm::vec3 wmin(1.0e30f);
            glm::vec3 wmax(-1.0e30f);
            for (int i = 0; i < 8; ++i) {
                const glm::vec3 corner(
                    (i & 1) ? lmax.x : lmin.x,
                    (i & 2) ? lmax.y : lmin.y,
                    (i & 4) ? lmax.z : lmin.z);
                const glm::vec4 wc4 = world * glm::vec4(corner, 1.0f);
                const glm::vec3 wcv = glm::vec3(wc4) / wc4.w;
                wmin = glm::min(wmin, wcv);
                wmax = glm::max(wmax, wcv);
            }
            const glm::vec3 t0 = (wmin - ro) * inv_d;
            const glm::vec3 t1 = (wmax - ro) * inv_d;
            const glm::vec3 tmn = glm::min(t0, t1);
            const glm::vec3 tmx = glm::max(t0, t1);
            const float tnear = glm::max(glm::max(tmn.x, tmn.y), tmn.z);
            const float tfar = glm::min(glm::min(tmx.x, tmx.y), tmx.z);
            if (tnear <= tfar && tfar >= 0.0f) {
                const float t = tnear >= 0.0f ? tnear : tfar;
                if (t < best_t) {
                    best_t = t;
                    best_id1 = entt::to_integral(e) + 1u;
                }
            }
        }
        return best_id1;
    }

}  // namespace cairns

namespace cairns {

cairns::ViewportId Engine::ResolveViewportName(uint32_t counter) const {
        int lo = 0;
        int hi = static_cast<int>(viewport_mgr_.names_count);
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            const uint32_t k = viewport_mgr_.names[mid].counter;
            if (k == counter) {
                return viewport_mgr_.names[mid].id;
            }
            if (k < counter) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return cairns::ViewportId::Null;
    }

}  // namespace cairns

namespace cairns {

bool Engine::CloseViewport() {
        if (viewport_mgr_.active_count <= 1) {
            return false;
        }
        const int last_idx = viewport_mgr_.active_count - 1;
        cairns::ViewportId id = viewport_mgr_.ids[last_idx];
        viewport_mgr_.pool.GetHot(id)->layout_rect = glm::vec4(0.0f);
        viewport_mgr_.ids[last_idx] = cairns::ViewportId::Null;
        --viewport_mgr_.active_count;
        // Drop the name table entry for this id. The counter itself
        // remains burned (never reused) per the locked sub-decision.
        for (uint8_t i = 0; i < viewport_mgr_.names_count; ++i) {
            if (viewport_mgr_.names[i].id.index == id.index &&
                viewport_mgr_.names[i].id.generation == id.generation) {
                for (uint8_t j = i; j + 1 < viewport_mgr_.names_count; ++j) {
                    viewport_mgr_.names[j] = viewport_mgr_.names[j + 1];
                }
                --viewport_mgr_.names_count;
                break;
            }
        }
        viewport_mgr_.pool.Release(id);
        const float w = 1.0f / static_cast<float>(viewport_mgr_.active_count);
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            viewport_mgr_.pool.GetHot(viewport_mgr_.ids[v])->layout_rect =
                glm::vec4(w * static_cast<float>(v), 0.0f, w, 1.0f);
        }
        return true;
    }

}  // namespace cairns

namespace cairns {

uint32_t Engine::OpenViewport() {
        if (viewport_mgr_.active_count >= kNumViewports) {
            return UINT32_MAX;
        }
        cairns::ViewportId id = viewport_mgr_.pool.Acquire();
        // Reused-slot trap: re-init both halves so a previously-released
        // slot doesn't carry over.
        if (auto* h = viewport_mgr_.pool.GetHot(id)) {
            *h = cairns::Viewport::Hot{};
            h->scene = scene_mgr_.active;
        }
        if (auto* c = viewport_mgr_.pool.GetCold(id)) {
            *c = cairns::Viewport::Cold{};
        }
        const int idx = viewport_mgr_.active_count++;
        viewport_mgr_.ids[idx] = id;
        // Default rect: uniform tile across the swap pane until the agent
        // calls setLayout. Tiles add up to the full pane.
        const float w = 1.0f / static_cast<float>(viewport_mgr_.active_count);
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            viewport_mgr_.pool.GetHot(viewport_mgr_.ids[v])->layout_rect =
                glm::vec4(w * static_cast<float>(v), 0.0f, w, 1.0f);
        }
        const uint32_t name = viewport_mgr_.next_name++;
        // Append-sorted: viewport_mgr_.next_name is monotonic so the new
        // counter is always the largest seen.
        viewport_mgr_.names[viewport_mgr_.names_count++] = ViewportName{name, id};
        return name;
    }

}  // namespace cairns

namespace cairns {

bool Engine::SpawnFitted(const std::vector<std::string>& glbs, uint32_t instances,
                     bool animated) {
        if (glbs.empty() || instances == 0) {
            return true;
        }
        std::vector<uint32_t> pidx;
        pidx.reserve(glbs.size());
        for (const std::string& name : glbs) {
            const uint32_t idx =
                cairns::headless::RuntimeLoadGlbPath(this, name);
            if (idx == UINT32_MAX) {
                return false;
            }
            pidx.push_back(idx);
        }
        std::vector<float> extents(instances);
        for (uint32_t i = 0; i < instances; ++i) {
            extents[i] = PrefabExtentMax(pidx[i % pidx.size()]);
        }
        const std::vector<glm::mat4> worlds =
            FitGridToViewport(instances, extents);
        for (uint32_t i = 0; i < instances; ++i) {
            const uint32_t scene_idx = pidx[i % pidx.size()];
            const glm::vec3 center = PrefabAabbCenter(scene_idx);
            const glm::mat4 world =
                worlds[i] * glm::translate(glm::mat4(1.0f), -center);
            const uint32_t out = animated
                ? InstantiatePrefab(scene_idx, world, /*time_phase=*/0.0f)
                : InstantiatePrefabNoSkin(scene_idx, world);
            if (out == UINT32_MAX) {
                return false;
            }
        }
        return true;
    }

    // Generate a primitive mesh on the CPU and push it through the SAME static-
    // mesh path as a GLB (one mesh, one identity node, one 1x1-color material) --
    // so a primitive is a normal vbo mesh, not a special-case draw. Returns the
    // new prefab's idx in {first_prefab_idx, count}.
    Engine::LoadPrefabBatchResult Engine::LoadProceduralPrefab(
                cairns::PrimitiveKind kind, const glm::vec4& color) {
        LoadPrefabBatchResult r{};
        r.first_prefab_idx =
            static_cast<uint32_t>(prefab_store_.prefab_ids.size());
        const cairns::PrimitiveMesh pm = cairns::MakePrimitive(kind);
        if (pm.positions.empty() || pm.indices.empty()) {
            return r;
        }
        rhi_.device.WaitIdle();

        cairns::PrefabId sid = prefab_store_.prefabs.Acquire();
        cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(sid);
        cairns::Prefab::Cold* scold = prefab_store_.prefabs.GetCold(sid);
        if (!shot || !scold) {
            prefab_store_.prefabs.Release(sid);
            return r;
        }
        // Re-seat the block-backed prefab vectors (mirrors LoadPrefabFromGltf).
        auto reseat = [this](auto& v) {
            using V = std::decay_t<decltype(v)>;
            v = V(typename V::allocator_type(cpu_block_));
        };
        reseat(shot->meshes);
        reseat(shot->rootNodes);
        reseat(shot->materials);
        reseat(scold->nodes);
        reseat(scold->bind_pose);
        reseat(scold->textureHandles);
        reseat(scold->samplerHandles);
        reseat(scold->gpu_parent);
        reseat(scold->gpu_topo);
        reseat(scold->gpu_bind_pose);
        reseat(scold->gpu_channels);
        reseat(scold->gpu_samplers);
        reseat(scold->gpu_times);
        reseat(scold->gpu_values);
        reseat(scold->gpu_joint_nodes);
        reseat(scold->gpu_inverse_binds);
        reseat(scold->skins);
        reseat(scold->clips);
        reseat(scold->loaded_samplers);
        reseat(scold->loaded_textures);
        reseat(scold->materialToTextureIndex);
        reseat(scold->materialToSamplerIndex);

        // Mesh: pack generator output into the pos (vec4) + attr streams.
        cairns::Handle<cairns::Mesh> mid = prefab_store_.meshes.Acquire();
        cairns::Mesh::Hot* mhot = prefab_store_.meshes.GetHot(mid);
        cairns::Mesh::Cold* mcold = prefab_store_.meshes.GetCold(mid);
        const uint32_t vcount = static_cast<uint32_t>(pm.positions.size());
        glm::vec3 aabb_min(std::numeric_limits<float>::max());
        glm::vec3 aabb_max(std::numeric_limits<float>::lowest());
        mcold->cpuPositions.reserve(vcount);
        mcold->cpuAttrs.reserve(vcount);
        for (uint32_t i = 0; i < vcount; ++i) {
            const glm::vec3& p = pm.positions[i];
            mcold->cpuPositions.emplace_back(p, 1.0f);
            cairns::VertexAttribute a{};
            a.normal = glm::vec4(pm.normals[i], 0.0f);
            a.uv = pm.uvs[i];
            mcold->cpuAttrs.push_back(a);
            aabb_min = glm::min(aabb_min, p);
            aabb_max = glm::max(aabb_max, p);
        }
        mcold->cpuIndices = pm.indices;
        mhot->vert_count = vcount;
        mhot->bind_aabb_min = aabb_min;
        mhot->bind_aabb_max = aabb_max;
        cairns::Primitive prim{};
        prim.firstIndex = 0;
        prim.indexCount = static_cast<uint32_t>(pm.indices.size());
        prim.vertexOffset = 0;
        prim.materialIndex = 0;
        mhot->primitives.push_back(prim);
        shot->meshes.push_back(mid);

        // One identity root node -> mesh 0, no skin (static draw path).
        cairns::Node node{};
        node.localTransform = glm::mat4(1.0f);
        node.globalTransform = glm::mat4(1.0f);
        node.meshIndex = 0;
        node.skinIndex = -1;
        scold->nodes.push_back(node);
        shot->rootNodes.push_back(0);

        // 1x1 solid-color texture + sampler + material (unlit samples it).
        auto to_u8 = [](float c) {
            return static_cast<uint8_t>(glm::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        const uint8_t px[4] = {to_u8(color.r), to_u8(color.g), to_u8(color.b),
                               to_u8(color.a)};
        rhi::TextureDesc td{};
        td.dimensions = {1, 1, 1};
        td.format = rhi::Format::kRgba8Unorm;
        td.mip_levels = 1;
        td.array_layers = 1;
        td.usage = rhi::kTexUsageSampled | rhi::kTexUsageTransferDst;
        td.memory = rhi::Memory::kDefault;
        td.initial_data = std::span<const uint8_t>(px, 4);
        rhi::Handle<rhi::Texture> tex =
            rhi_.resources.CreateTexture(rhi_.alloc, td);
        rhi::SamplerDesc smp{};
        smp.min_filter = rhi::Filter::kNearest;
        smp.mag_filter = rhi::Filter::kNearest;
        rhi::Handle<rhi::Sampler> samp = rhi_.resources.CreateSampler(smp);
        scold->textureHandles.push_back(tex);
        scold->samplerHandles.push_back(samp);
        cairns::Handle<cairns::Material> matid =
            prefab_store_.materials.Acquire();
        cairns::Material::Cold* macold = prefab_store_.materials.GetCold(matid);
        macold->color = tex;
        macold->sampler = samp;
        shot->materials.push_back(matid);

        prefab_store_.prefab_ids.push_back(sid);
        r.count = 1;

        // ── upload + manifest, same order as LoadPrefabBatch ──
        rhi::Handle<rhi::Buffer> batch_shared_skin =
            rhi::Handle<rhi::Buffer>::Null;
        std::span<const cairns::PrefabId> new_span(
            prefab_store_.prefab_ids.data() + r.first_prefab_idx, r.count);
        if (!cairns::rhi::LoadPrefabsGpu(new_span, prefab_store_.prefabs,
                                          prefab_store_.meshes, rhi_.resources,
                                          rhi_.alloc, &batch_shared_skin)) {
            return r;
        }
        uint32_t batch_mesh_count = 0;
        StampBatchSkinAndMeshIds(new_span, batch_shared_skin, batch_mesh_count);
        BuildGroupABindGroups(new_span, batch_shared_skin);
        cairns::ValidationReport vreport{};
        ValidateAndCleanupTmps(new_span, r.first_prefab_idx, vreport);
        BuildMaterialSet2();
        BuildResidentTextures(new_span);
        StampPerPrefabAsset(new_span);
        std::array<std::filesystem::path, 1> synth{
            std::filesystem::path("primitive")};
        AppendGlbPaths(new_span,
                       std::span<const std::filesystem::path>(synth));
        return r;
    }

    // Spawn one primitive fit to the active viewport (like SpawnFitted).
    bool Engine::SpawnPrimitive(cairns::PrimitiveKind kind,
                                const glm::vec4& color) {
        const LoadPrefabBatchResult r = LoadProceduralPrefab(kind, color);
        if (r.count == 0) {
            return false;
        }
        const uint32_t scene_idx = r.first_prefab_idx;
        std::vector<float> extents{PrefabExtentMax(scene_idx)};
        const std::vector<glm::mat4> worlds = FitGridToViewport(1, extents);
        const glm::vec3 center = PrefabAabbCenter(scene_idx);
        const glm::mat4 world =
            worlds[0] * glm::translate(glm::mat4(1.0f), -center);
        return InstantiatePrefabNoSkin(scene_idx, world) != UINT32_MAX;
    }

    // Spawn one of each kind in a fitted grid (the "test all primitives"
    // scenario). Reuses FitGridToViewport exactly like SpawnFitted. Each
    // anims[i] (if present) is attached as a per-entity TransformAnim with the
    // fitted pose captured as its rest base -- so the grid animates with a mix
    // of transforms, not a global spin.
    bool Engine::SpawnPrimitivesGrid(
                const std::vector<cairns::PrimitiveKind>& kinds,
                const std::vector<glm::vec4>& colors,
                const std::vector<cairns::TransformAnim>& anims) {
        if (kinds.empty()) {
            return true;
        }
        std::vector<uint32_t> pidx;
        pidx.reserve(kinds.size());
        for (size_t i = 0; i < kinds.size(); ++i) {
            const glm::vec4 c =
                (i < colors.size()) ? colors[i] : glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            const LoadPrefabBatchResult r = LoadProceduralPrefab(kinds[i], c);
            if (r.count == 0) {
                return false;
            }
            pidx.push_back(r.first_prefab_idx);
        }
        std::vector<float> extents(pidx.size());
        for (size_t i = 0; i < pidx.size(); ++i) {
            extents[i] = PrefabExtentMax(pidx[i]);
        }
        const std::vector<glm::mat4> worlds =
            FitGridToViewport(static_cast<uint32_t>(pidx.size()), extents);
        for (size_t i = 0; i < pidx.size(); ++i) {
            const glm::vec3 center = PrefabAabbCenter(pidx[i]);
            const glm::mat4 world =
                worlds[i] * glm::translate(glm::mat4(1.0f), -center);
            const uint32_t out = InstantiatePrefabNoSkin(pidx[i], world);
            if (out == UINT32_MAX) {
                return false;
            }
            if (i >= anims.size()) {
                continue;
            }
            cairns::Scene::Cold* wc = scene_mgr_.pool.GetCold(scene_mgr_.active);
            if (!wc) {
                continue;
            }
            const entt::entity e = static_cast<entt::entity>(out);
            if (!wc->registry.all_of<cairns::Transform>(e)) {
                continue;
            }
            const cairns::Transform& tr = wc->registry.get<cairns::Transform>(e);
            cairns::TransformAnim a = anims[i];
            a.base_t = tr.t;
            a.base_r = tr.r;
            a.base_s = tr.s;
            wc->registry.emplace<cairns::TransformAnim>(e, a);
        }
        return true;
    }

}  // namespace cairns

namespace cairns {

void Engine::ApplyFlyMovement(const glm::vec3& move_input) {
        if (viewport_mgr_.cam_pose_override) {
            return;
        }
        cairns::FlyController& fc =
            viewport_mgr_.pool.GetCold(viewport_mgr_.active)->fly;
        const float cy = std::cos(fc.yaw);
        const float sy = std::sin(fc.yaw);
        const float cp = std::cos(fc.pitch);
        const float sp = std::sin(fc.pitch);
        const glm::vec3 forward(-cp * sy, sp, -cp * cy);
        // right = normalize(cross(forward, world_up)). Closed-form with
        // world_up=(0,1,0): right = (cy, 0, -sy) (independent of pitch).
        // At yaw=0,pitch=0 this is (1,0,0): +X is screen-right while looking
        // down -Z. Backend-agnostic -- vk's negative-height viewport flips
        // only Y, not X, so metal and vk see the same horizontal motion.
        const glm::vec3 right(cy, 0.0f, -sy);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        fc.position += right * move_input.x + up * move_input.y +
                        forward * move_input.z;
    }

}  // namespace cairns

namespace cairns {

uint32_t Engine::UnloadAllPrefabs() {
        const uint32_t n = static_cast<uint32_t>(prefab_store_.prefab_ids.size());
        if (n == 0) {
            return 0;
        }
        // Quiesce the render thread + GPU before releasing pool slots and
        // freeing resources. Otherwise an in-flight frame records draws against
        // the meshes we Release here -- the render thread crashes in
        // drawIndexedPrimitives on a freed index buffer (cairns_serve exit 139
        // on the load+reload+unload path). Same guard RuntimeLoadBatch uses
        // before mutating the pools.
        if (render_thread_) {
            render_thread_->Drain();
        }
        rhi_.device.WaitIdle();
        // Free the per-actor skin slices + skin pool. TryCreateSkinForScene
        // Allocs an output_pool slice per actor and never Frees it, and the
        // skins ResourceManager grows one entry per actor -- without this
        // BOTH leak across scenario switches until the pool overflows.
        // Safe here: render thread drained + WaitIdle above, and the caller
        // clears the scene first so no live SkinRef remains (a stale one
        // resolves to a null skin -> bind pose).
        skinning_.skins.Clear();
        skinning_.output_pool.Reset();
        for (cairns::PrefabId pid : prefab_store_.prefab_ids) {
            cairns::Prefab::Hot* phot = prefab_store_.prefabs.GetHot(pid);
            cairns::Prefab::Cold* pcold = prefab_store_.prefabs.GetCold(pid);
            if (pcold) {
                for (rhi::Handle<rhi::Texture> th : pcold->textureHandles) {
                    rhi_.resources.DeferFree(rhi_.alloc, th);
                }
                for (rhi::Handle<rhi::Sampler> sh : pcold->samplerHandles) {
                    rhi_.resources.DeferFree(sh);
                }
            }
            if (phot) {
                for (cairns::Handle<cairns::Mesh> mh : phot->meshes) {
                    cairns::Mesh::Hot* mhot = prefab_store_.meshes.GetHot(mh);
                    if (mhot) {
                        rhi_.resources.DeferFree(rhi_.alloc, mhot->posHandle);
                        rhi_.resources.DeferFree(rhi_.alloc, mhot->attrHandle);
                        rhi_.resources.DeferFree(rhi_.alloc, mhot->indexHandle);
                        if (!mhot->skin_group_a.IsNull()) {
                            rhi_.resources.DeferFree(mhot->skin_group_a);
                        }
                    }
                    prefab_store_.meshes.Release(mh);
                }
                for (cairns::Handle<cairns::Material> matid : phot->materials) {
                    cairns::Material::Hot* mathot = prefab_store_.materials.GetHot(matid);
                    if (mathot == nullptr) {
                        continue;  // deduped repeat already released this pass
                    }
                    if (!mathot->set2.IsNull()) {
                        rhi_.resources.DeferFree(mathot->set2);
                    }
                    prefab_store_.materials.Release(matid);
                }
            }
            prefab_store_.prefabs.Release(pid);
        }
        for (rhi::Handle<rhi::Buffer> sb : prefab_store_.per_batch_shared_skin) {
            if (!sb.IsNull()) {
                rhi_.resources.DeferFree(rhi_.alloc, sb);
            }
        }
        prefab_store_.per_batch_shared_skin.clear();
        prefab_store_.prefab_ids.clear();
        prefab_store_.per_prefab_asset.clear();
        prefab_store_.glb_paths.clear();
        prefab_store_.resident_textures.clear();
        // Every per-material set-2 descriptor set is dead (all materials
        // released above). Bulk-free them from the fixed vk material pool --
        // Destroy(BindGroup) only recycles the handle slot; without this the
        // pool leaks a set per material across scenario reloads until
        // allocation fails -> null set2 -> white actors (vk-only).
        rhi_.resources.ResetMaterialBindGroups();
        // Anim: reset cursors so the next upload starts fresh against
        // unallocated capacity. The 4x growth pad still holds so the
        // first post-Unload load triggers FULL once, then DELTA after.
        skinning_.uploaded_prefab_count = 0;
        skinning_.cur = {};
        skinning_.eval_tables_uploaded = false;
        skinning_.dyn_dirty = true;
        prefab_arena_.Reset();  // monotonic; else repeated loads overflow it
        return n;
    }

}  // namespace cairns

namespace cairns {

bool Engine::ReloadPipelineByName(const std::string& name) {
        const std::string shader_dir = cairns::GetBasePathSafe();
        if (name == "anim_eval") {
            rhi::ComputePipelineDesc desc{};
            desc.logical_shader = "anim_eval";
            desc.shader_dir = shader_dir.c_str();
            desc.debug_name = "anim_eval";
            desc.layout = rhi::ComputePipelineLayout::kAnimEval;
            rhi::Handle<rhi::Kernel> next =
                rhi_.pipelines.CreateComputePipeline(rhi_.resources,
                                                       rhi_.frames, desc);
            if (next.IsNull()) {
                CAIRNS_PRINT_ERR("[R2] anim_eval reload failed -- keeping "
                                 "last-good\n");
                return false;
            }
            if (!skinning_.eval_kernel.IsNull()) {
                rhi_.resources.DeferFree(skinning_.eval_kernel);
            }
            skinning_.eval_kernel = next;
            return true;
        }
        if (name == "skin") {
            rhi::ComputePipelineDesc desc{};
            desc.logical_shader = "skin";
            desc.shader_dir = shader_dir.c_str();
            desc.debug_name = "skin_compute";
            desc.layout = rhi::ComputePipelineLayout::kSkin;
            rhi::Handle<rhi::Kernel> next =
                rhi_.pipelines.CreateComputePipeline(rhi_.resources,
                                                       rhi_.frames, desc);
            if (next.IsNull()) {
                CAIRNS_PRINT_ERR("[R2] skin reload failed -- keeping "
                                 "last-good\n");
                return false;
            }
            if (!skinning_.skin_kernel.IsNull()) {
                rhi_.resources.DeferFree(skinning_.skin_kernel);
            }
            skinning_.skin_kernel = next;
            return true;
        }
        if (name == "particle") {
            rhi::ComputePipelineDesc desc{};
            desc.logical_shader = "particle";
            desc.shader_dir = shader_dir.c_str();
            desc.debug_name = "particle_compute";
            desc.layout = rhi::ComputePipelineLayout::kParticle;
            desc.dyn_set_0 = dyn_particle_parity_[0];
            rhi::Handle<rhi::Kernel> next =
                rhi_.pipelines.CreateComputePipeline(rhi_.resources,
                                                       rhi_.frames, desc);
            if (next.IsNull()) {
                CAIRNS_PRINT_ERR("[R2] particle reload failed -- keeping "
                                 "last-good\n");
                return false;
            }
            if (!particles_.kernel.IsNull()) {
                rhi_.resources.DeferFree(particles_.kernel);
            }
            particles_.kernel = next;
            return true;
        }
        return false;
    }

}  // namespace cairns

namespace cairns {

bool Engine::ReloadPrefab(uint32_t idx, const std::filesystem::path& path) {
        if (idx >= prefab_store_.prefab_ids.size()) {
            return false;
        }
        // Instrumentation: pairs with the Instruments signpost so the time
        // profiler bands reload work distinctly from steady-state frames.
        CAIRNS_PRINT_ERR("[RELOAD] begin idx=%u path=%s\n", idx,
                          path.filename().c_str());
        CAIRNS_SIGNPOST_INTERVAL_SCOPED("reload_prefab",
                                         path.filename().c_str());
#if CAIRNS_ALLOC_TRACE
        const cairns::alloc_count::Snapshot alloc_reload_begin =
            cairns::alloc_count::Now();
#endif
        cairns::PrefabId oldId = prefab_store_.prefab_ids[idx];
        // Snapshot the old prefab's owned resources BEFORE LoadPrefabBatch
        // -- it may grow the prefab_store_.prefabs/prefab_store_.meshes/prefab_store_.materials pools' backing
        // vectors and invalidate held Hot/Cold pointers.
        std::vector<cairns::Handle<cairns::Mesh>> old_meshes;
        std::vector<cairns::Handle<cairns::Material>> old_materials;
        std::vector<rhi::Handle<rhi::Texture>> old_textures;
        std::vector<rhi::Handle<rhi::Sampler>> old_samplers;
        {
            cairns::Prefab::Hot* oldH = prefab_store_.prefabs.GetHot(oldId);
            cairns::Prefab::Cold* oldC = prefab_store_.prefabs.GetCold(oldId);
            if (!oldH || !oldC) {
                return false;
            }
            // Pool vectors are block-backed (ChunkStdAllocator); copy
            // element-wise into the std-allocator snapshot locals.
            old_meshes.assign(oldH->meshes.begin(), oldH->meshes.end());
            old_materials.assign(oldH->materials.begin(), oldH->materials.end());
            old_textures.assign(oldC->textureHandles.begin(), oldC->textureHandles.end());
            old_samplers.assign(oldC->samplerHandles.begin(), oldC->samplerHandles.end());
        }
        std::array<std::filesystem::path, 1> single_path{path};
        LoadPrefabBatchResult r = RuntimeLoadBatch(single_path);
        if (r.count != 1) {
            return false;
        }
        cairns::PrefabId newId = prefab_store_.prefab_ids.back();
        // Re-fetch after the load: the pool's backing vector may have
        // grown, invalidating any pointer obtained pre-RuntimeLoadBatch.
        cairns::Prefab::Hot* oldH = prefab_store_.prefabs.GetHot(oldId);
        cairns::Prefab::Cold* oldC = prefab_store_.prefabs.GetCold(oldId);
        cairns::Prefab::Hot* newH = prefab_store_.prefabs.GetHot(newId);
        cairns::Prefab::Cold* newC = prefab_store_.prefabs.GetCold(newId);
        if (!oldH || !oldC || !newH || !newC) {
            return false;
        }
        *oldH = std::move(*newH);
        *oldC = std::move(*newC);
        prefab_store_.prefabs.Release(newId);
        prefab_store_.prefab_ids.pop_back();
        prefab_store_.per_prefab_asset.pop_back();
        prefab_store_.glb_paths.pop_back();
        prefab_store_.glb_paths[idx] = path;
        // DeferFree the snapshotted old resources: retire_frame =
        // current_frame_index + kFIF; drain happens at frame >=
        // retire_frame's start, after the fence proves the
        // last-referencing frame is GPU-done.
        for (rhi::Handle<rhi::Texture> th : old_textures) {
            rhi_.resources.DeferFree(rhi_.alloc, th);
        }
        for (rhi::Handle<rhi::Sampler> sh : old_samplers) {
            rhi_.resources.DeferFree(sh);
        }
        for (cairns::Handle<cairns::Mesh> mh : old_meshes) {
            cairns::Mesh::Hot* mhot = prefab_store_.meshes.GetHot(mh);
            if (mhot) {
                rhi_.resources.DeferFree(rhi_.alloc, mhot->posHandle);
                rhi_.resources.DeferFree(rhi_.alloc, mhot->attrHandle);
                rhi_.resources.DeferFree(rhi_.alloc, mhot->indexHandle);
                if (!mhot->skin_group_a.IsNull()) {
                    rhi_.resources.DeferFree(mhot->skin_group_a);
                }
            }
            prefab_store_.meshes.Release(mh);
        }
        for (cairns::Handle<cairns::Material> matid : old_materials) {
            cairns::Material::Hot* mathot = prefab_store_.materials.GetHot(matid);
            if (mathot == nullptr) {
                continue;  // deduped repeat already released this pass
            }
            if (!mathot->set2.IsNull()) {
                rhi_.resources.DeferFree(mathot->set2);
            }
            prefab_store_.materials.Release(matid);
        }
        // prefab_store_.resident_textures was populated by AppendGlbPaths' sibling
        // BuildResidentTextures during boot/load. The old prefab's
        // texture handles are now stale (Release happens at the deferred
        // drain kFIF frames later); per-frame draw uses prefab_store_.resident_textures
        // verbatim, so we must rebuild it from the current set of live
        // prefab textureHandles before the next render reads it.
        prefab_store_.resident_textures.clear();
        for (cairns::PrefabId pid : prefab_store_.prefab_ids) {
            cairns::Prefab::Cold* pc = prefab_store_.prefabs.GetCold(pid);
            if (!pc) {
                continue;
            }
            for (rhi::Handle<rhi::Texture> th : pc->textureHandles) {
                prefab_store_.resident_textures.push_back(th);
            }
        }
        CAIRNS_PRINT_ERR("[RELOAD] end idx=%u path=%s ok\n", idx,
                          path.filename().c_str());
#if CAIRNS_ALLOC_TRACE
        cairns::alloc_count::PrintDelta("[RELOAD]", alloc_reload_begin);
#endif
        return true;
    }

}  // namespace cairns

namespace cairns {

bool Engine::SetEntityTransform(uint32_t entity_int, const glm::mat4& world) {
        cairns::Scene::Cold* wc = scene_mgr_.pool.GetCold(scene_mgr_.active);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e) || !reg.all_of<cairns::Transform>(e)) {
            return false;
        }
        // Author TRS, not WorldTransform: extract t+s from the
        // translate*scale world; rotation stays as authored (set via the
        // entity ops). PropagateTransforms recomposes WorldTransform.
        cairns::Transform& tr = reg.get<cairns::Transform>(e);
        tr.t = glm::vec3(world[3]);
        tr.s = glm::vec3(world[0][0], world[1][1], world[2][2]);
        if (!reg.all_of<cairns::DirtyTransform>(e)) {
            reg.emplace<cairns::DirtyTransform>(e);
        }
        if (auto* wh = scene_mgr_.pool.GetHot(scene_mgr_.active)) {
            wh->dirty = true;
        }
        return true;
    }

}  // namespace cairns

namespace cairns {

glm::vec3 Engine::PrefabAabbCenter(uint32_t scene_idx) {
        if (scene_idx >= prefab_store_.prefab_ids.size()) {
            return glm::vec3(0.0f);
        }
        cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(prefab_store_.prefab_ids[scene_idx]);
        if (!shot) {
            return glm::vec3(0.0f);
        }
        for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
            cairns::Mesh::Hot* mh = prefab_store_.meshes.GetHot(mid);
            if (!mh) {
                continue;
            }
            if (mh->bind_aabb_min.x > mh->bind_aabb_max.x) {
                continue;
            }
            return (mh->bind_aabb_min + mh->bind_aabb_max) * 0.5f;
        }
        return glm::vec3(0.0f);
    }

}  // namespace cairns

namespace cairns {

float Engine::PrefabExtentMax(uint32_t scene_idx) {
        if (scene_idx >= prefab_store_.prefab_ids.size()) {
            return 0.0f;
        }
        cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(prefab_store_.prefab_ids[scene_idx]);
        if (!shot) {
            return 0.0f;
        }
        for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
            cairns::Mesh::Hot* mh = prefab_store_.meshes.GetHot(mid);
            if (!mh) {
                continue;
            }
            if (mh->bind_aabb_min.x > mh->bind_aabb_max.x) {
                continue;
            }
            const glm::vec3 ext = mh->bind_aabb_max - mh->bind_aabb_min;
            return std::max(ext.x, std::max(ext.y, ext.z));
        }
        return 0.0f;
    }

}  // namespace cairns

namespace cairns {

uint32_t Engine::CountAppendOnlyMismatches(
            std::span<const PrefabHandleSnapshot> prior) {
        uint32_t mismatches = 0;
        for (const PrefabHandleSnapshot& p : prior) {
            if (p.prefab_idx >= prefab_store_.prefab_ids.size()) {
                ++mismatches; continue;
            }
            cairns::Prefab::Hot* shot =
                prefab_store_.prefabs.GetHot(prefab_store_.prefab_ids[p.prefab_idx]);
            if (!shot || p.mesh_idx >= shot->meshes.size()) {
                ++mismatches; continue;
            }
            cairns::Mesh::Hot* mhot =
                prefab_store_.meshes.GetHot(shot->meshes[p.mesh_idx]);
            if (!mhot) {
                ++mismatches; continue;
            }
            if (mhot->posHandle.index != p.pos_idx ||
                mhot->posHandle.generation != p.pos_gen ||
                mhot->attrHandle.index != p.attr_idx ||
                mhot->attrHandle.generation != p.attr_gen ||
                mhot->indexHandle.index != p.idx_idx ||
                mhot->indexHandle.generation != p.idx_gen ||
                mhot->batch_id != p.batch_id ||
                mhot->global_base_vertex != p.global_base_vertex) {
                ++mismatches;
            }
        }
        return mismatches;
    }

}  // namespace cairns

namespace cairns {

std::vector<glm::mat4> Engine::FitGridToViewport(
            uint32_t n_total,
            std::span<const float> per_actor_extents) {
        std::vector<glm::mat4> out;
        if (n_total == 0) {
            return out;
        }
        out.reserve(n_total);
        const uint32_t cols = static_cast<uint32_t>(
            std::max(1.0f, std::ceil(std::sqrt(
                              static_cast<float>(n_total)))));
        const uint32_t rows = (n_total + cols - 1u) / cols;
        // Camera: y-axis FOV 90deg (the engine default). Aspect = active
        // viewport target dims; depth Z = -4 world units.
        const float fov_y = static_cast<float>(M_PI) * 0.5f;
        float aspect = 16.0f / 9.0f;
        if (present_.final_target_h > 0) {
            aspect = static_cast<float>(present_.final_target_w) /
                     static_cast<float>(present_.final_target_h);
        }
        const float depth = 4.0f;
        // kFitMargin shrinks the GRID extent so the outermost characters
        // get margin between their bind-pose AABB edge and the viewport
        // edge. (Animated poses extend beyond bind extent; at large N the
        // pre-margin grid spanned the full viewport and characters at the
        // edges clipped.) cell_size's 0.85 scales the CHARACTER within
        // its cell, independent of this.
        const float kFitMargin = 0.85f;
        const float visible_h = 2.0f * std::tan(fov_y * 0.5f) * depth * kFitMargin;
        const float visible_w = visible_h * aspect;
        const float cell_w = visible_w / static_cast<float>(cols);
        const float cell_h = visible_h / static_cast<float>(rows);
        const float cell_size = std::min(cell_w, cell_h) * 0.85f;
        const float start_x = -cell_w * (static_cast<float>(cols - 1u) * 0.5f);
        const float start_y = -cell_h * (static_cast<float>(rows - 1u) * 0.5f);
        for (uint32_t i = 0; i < n_total; ++i) {
            const uint32_t row = i / cols;
            const uint32_t col = i % cols;
            const float x = start_x + static_cast<float>(col) * cell_w;
            const float y = start_y + static_cast<float>(row) * cell_h;
            // Per-actor scale: cell_size / extent so each model fills the
            // same on-screen cell regardless of its raw GLB size. Extent
            // 0 (unknown / missing AABB) falls back to a small fixed scale.
            const float extent = (i < per_actor_extents.size() &&
                                   per_actor_extents[i] > 0.0f)
                                     ? per_actor_extents[i] : 100.0f;
            const float scale = cell_size / extent;
            glm::mat4 m(1.0f);
            m = glm::translate(m, glm::vec3(x, y, -depth));
            m = glm::scale(m, glm::vec3(scale));
            out.push_back(m);
        }
        return out;
    }

}  // namespace cairns

namespace cairns {

void Engine::StampPerPrefabAsset(std::span<const cairns::PrefabId> new_span) {
        prefab_store_.per_prefab_asset.reserve(prefab_store_.prefab_ids.size());
        for (cairns::PrefabId sid : new_span) {
            cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(sid);
            if (!shot || shot->meshes.empty()) {
                prefab_store_.per_prefab_asset.push_back(cairns::AssetId{});
                continue;
            }
            const cairns::Mesh::Hot* m0 =
                prefab_store_.meshes.GetHot(shot->meshes[0]);
            if (!m0) {
                prefab_store_.per_prefab_asset.push_back(cairns::AssetId{});
                continue;
            }
            const uint32_t prefab_idx =
                static_cast<uint32_t>(prefab_store_.per_prefab_asset.size());
            prefab_store_.per_prefab_asset.push_back(scene_mgr_.assets.RegisterExistingScene(
                prefab_idx, sid,
                m0->posHandle, m0->attrHandle, m0->indexHandle));
        }
    }

}  // namespace cairns

namespace cairns {

void Engine::ValidateAndCleanupTmps(
            std::span<const cairns::PrefabId> new_span,
            uint32_t first_prefab_idx,
            cairns::ValidationReport& vreport) {
        for (uint32_t pi = 0;
             pi < static_cast<uint32_t>(new_span.size()); ++pi) {
            const uint32_t prefab_idx = first_prefab_idx + pi;
            cairns::Prefab::Hot* shot =
                prefab_store_.prefabs.GetHot(prefab_store_.prefab_ids[prefab_idx]);
            if (!shot) {
                continue;
            }
            for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
                if (cairns::Mesh::Cold* mc = prefab_store_.meshes.GetCold(mid)) {
                    ValidateMeshWeights(*mc, vreport, prefab_idx);
                }
            }
        }
        for (cairns::PrefabId sid : new_span) {
            if (cairns::Prefab::Cold* sc = prefab_store_.prefabs.GetCold(sid)) {
                sc->CleanupTmps();
            }
        }
        for (cairns::PrefabId sid : new_span) {
            cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(sid);
            if (!shot) {
                continue;
            }
            for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
                if (cairns::Mesh::Cold* mc = prefab_store_.meshes.GetCold(mid)) {
                    mc->cpuPositions.clear();
                    mc->cpuAttrs.clear();
                    mc->cpuIndices.clear();
                }
            }
        }
    }

}  // namespace cairns

namespace cairns {

void Engine::BuildGroupABindGroups(
            std::span<const cairns::PrefabId> new_span,
            rhi::Handle<rhi::Buffer> batch_shared_skin) {
        for (cairns::PrefabId sid : new_span) {
            cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(sid);
            if (!shot) {
                continue;
            }
            for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
                cairns::Mesh::Hot* mhot = prefab_store_.meshes.GetHot(mid);
                if (!mhot || mhot->attr_skinned_alias.IsNull() ||
                    batch_shared_skin.IsNull() || mhot->vert_count == 0) {
                    continue;
                }
                cairns::rhi::BufferBinding bb[2]{};
                bb[0].slot = 0;
                bb[0].buffer = mhot->posHandle;
                bb[0].offset = mhot->global_base_vertex *
                    static_cast<uint32_t>(sizeof(glm::vec4));
                bb[0].range = mhot->vert_count *
                    static_cast<uint32_t>(sizeof(glm::vec4));
                bb[0].kind = cairns::rhi::BufferKind::kStorage;
                bb[1].slot = 1;
                bb[1].buffer = batch_shared_skin;
                bb[1].offset = mhot->skin_attr_base_vertex *
                    static_cast<uint32_t>(sizeof(cairns::PackedSkinVertex));
                bb[1].range = mhot->vert_count *
                    static_cast<uint32_t>(sizeof(cairns::PackedSkinVertex));
                bb[1].kind = cairns::rhi::BufferKind::kStorage;
                cairns::rhi::BindGroupDesc bgd{};
                bgd.debug_name = "skin_group_a";
                bgd.buffers = std::span<const cairns::rhi::BufferBinding>(
                    bb, 2);
                mhot->skin_group_a = rhi_.resources.CreateSkinGroupA(
                    rhi_.alloc, rhi_.frames, rhi_.pipelines, bgd);
            }
        }
    }

}  // namespace cairns

namespace cairns {

void Engine::StampBatchSkinAndMeshIds(
            std::span<const cairns::PrefabId> new_span,
            rhi::Handle<rhi::Buffer> batch_shared_skin,
            uint32_t& batch_mesh_count_out) {
        const uint16_t batch_id =
            static_cast<uint16_t>(prefab_store_.per_batch_shared_skin.size());
        prefab_store_.per_batch_shared_skin.push_back(batch_shared_skin);
        for (cairns::PrefabId sid : new_span) {
            cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(sid);
            if (!shot) {
                continue;
            }
            for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
                if (cairns::Mesh::Hot* mhot = prefab_store_.meshes.GetHot(mid)) {
                    mhot->batch_id = batch_id;
                    ++batch_mesh_count_out;
                }
            }
        }
    }

}  // namespace cairns

namespace cairns {

uint32_t Engine::CheckPrefabStateInvariants(
            std::vector<std::string>* out_msgs) {
        uint32_t v = 0;
        auto fail = [&](const char* what) {
            ++v;
            if (out_msgs) {
                out_msgs->emplace_back(what);
            }
        };
        const size_t n_prefabs = prefab_store_.prefab_ids.size();
        // (1) StampBatchSkinAndMeshIds:
        //     prefab_store_.per_batch_shared_skin has at least one entry whenever any
        //     prefab is resident; every Mesh::Hot::batch_id indexes it.
        if (n_prefabs > 0 && prefab_store_.per_batch_shared_skin.empty()) {
            fail("prefab_store_.per_batch_shared_skin empty but prefabs are resident");
        }
        const size_t n_batches = prefab_store_.per_batch_shared_skin.size();
        prefab_store_.meshes.ForEachLive(
            [&](cairns::Mesh::Hot& mh, cairns::Mesh::Cold&) {
                if (mh.batch_id >= n_batches) {
                    fail("Mesh::Hot::batch_id >= prefab_store_.per_batch_shared_skin.size()");
                }
            });
        // (2) BuildGroupABindGroups -- vk only; on metal skin_group_a is
        //     Null by design. Skip; not a portable invariant.
        // (3) ValidateAndCleanupTmps:
        //     every live Mesh::Cold has empty cpuPositions/cpuAttrs/
        //     cpuIndices after the batch finished.
        prefab_store_.meshes.ForEachLive(
            [&](cairns::Mesh::Hot&, cairns::Mesh::Cold& mc) {
                if (!mc.cpuPositions.empty() || !mc.cpuAttrs.empty() ||
                    !mc.cpuIndices.empty()) {
                    fail("Mesh::Cold cpu temporaries not cleared");
                }
            });
        // (4) BuildMaterialSet2:
        //     every live Material::Hot has non-null set2.
        prefab_store_.materials.ForEachLive(
            [&](cairns::Material::Hot& mat, cairns::Material::Cold&) {
                if (mat.set2.IsNull()) {
                    fail("Material::Hot::set2 is Null");
                }
            });
        // (5) BuildResidentTextures:
        //     prefab_store_.resident_textures.size() == sum of every live prefab's
        //     Cold.textureHandles.size().
        size_t sum_tex = 0;
        prefab_store_.prefabs.ForEachLive(
            [&](cairns::Prefab::Hot&, cairns::Prefab::Cold& pc) {
                sum_tex += pc.textureHandles.size();
            });
        if (prefab_store_.resident_textures.size() != sum_tex) {
            fail("prefab_store_.resident_textures.size() != sum_of_prefab_textureHandles");
        }
        // (6) StampPerPrefabAsset:
        //     prefab_store_.per_prefab_asset.size() == prefab_store_.prefab_ids.size().
        if (prefab_store_.per_prefab_asset.size() != n_prefabs) {
            fail("prefab_store_.per_prefab_asset.size() != prefab_store_.prefab_ids.size()");
        }
        // (7) AppendGlbPaths:
        //     prefab_store_.glb_paths.size() == prefab_store_.prefab_ids.size(). [PICK] log
        //     resolves prefab_idx -> filename via this.
        if (prefab_store_.glb_paths.size() != n_prefabs) {
            fail("prefab_store_.glb_paths.size() != prefab_store_.prefab_ids.size()");
        }
        // (8) AcquireSceneCells:
        //     scene_mgr_.active valid (entt registry exists for instantiate).
        if (scene_mgr_.active.IsNull()) {
            fail("scene_mgr_.active is Null (no entt container)");
        }
        return v;
    }

}  // namespace cairns

namespace cairns {

uint32_t Engine::InstantiatePrefabImpl(uint32_t scene_idx, const glm::mat4& world,
                                    float time_phase, bool attach_skin) {
        if (scene_idx >= prefab_store_.prefab_ids.size() ||
            scene_idx >= prefab_store_.per_prefab_asset.size()) {
            return UINT32_MAX;
        }
        cairns::Scene::Cold* wc = scene_mgr_.pool.GetCold(scene_mgr_.active);
        if (!wc) {
            return UINT32_MAX;
        }
        auto& reg = wc->registry;
        const entt::entity e = reg.create();
        // Author TRS, not a baked WorldTransform.
        // PropagateTransforms composes WorldTransform each frame (identity
        // root; the scene root is applied in Extract). Every current caller
        // passes translate(t)*scale(s), so extract t (col 3) + s (diagonal)
        // EXACTLY -- ComposeTRS reproduces `world` bit-for-bit
        // (STATEHASH-neutral). Rotation authoring arrives with the entity ops.
        cairns::Transform tr;
        tr.t = glm::vec3(world[3]);
        tr.s = glm::vec3(world[0][0], world[1][1], world[2][2]);
        reg.emplace<cairns::Transform>(e, tr);
        reg.emplace<cairns::DirtyTransform>(e);
        cairns::AssetRef ar;
        ar.asset = prefab_store_.per_prefab_asset[scene_idx];
        reg.emplace<cairns::AssetRef>(e, ar);
        cairns::Renderable rdr;
        rdr.layer_mask = 0xFFFFFFFFu;
        rdr.flags = cairns::kProxyVisible;
        reg.emplace<cairns::Renderable>(e, rdr);
        if (attach_skin) {
            cairns::SkinId sid =
                TryCreateSkinForScene(prefab_store_.prefab_ids[scene_idx], time_phase);
            if (!sid.IsNull()) {
                reg.emplace<cairns::SkinRef>(e, cairns::SkinRef{sid});
            }
        }
        // Mark world dirty so the proxy extract picks up the new entity.
        if (auto* wh = scene_mgr_.pool.GetHot(scene_mgr_.active)) {
            wh->dirty = true;
        }
        return static_cast<uint32_t>(entt::to_integral(e));
    }

}  // namespace cairns

namespace cairns {

std::vector<Engine::PrefabHandleSnapshot> Engine::SnapshotPrefabHandles() {
        std::vector<PrefabHandleSnapshot> out;
        for (uint32_t pi = 0;
             pi < static_cast<uint32_t>(prefab_store_.prefab_ids.size()); ++pi) {
            cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(prefab_store_.prefab_ids[pi]);
            if (!shot) {
                continue;
            }
            for (uint32_t mi = 0;
                 mi < static_cast<uint32_t>(shot->meshes.size()); ++mi) {
                cairns::Mesh::Hot* mhot = prefab_store_.meshes.GetHot(shot->meshes[mi]);
                if (!mhot) {
                    continue;
                }
                PrefabHandleSnapshot s{};
                s.prefab_idx = pi;
                s.mesh_idx = mi;
                s.pos_idx  = mhot->posHandle.index;
                s.pos_gen  = mhot->posHandle.generation;
                s.attr_idx = mhot->attrHandle.index;
                s.attr_gen = mhot->attrHandle.generation;
                s.idx_idx  = mhot->indexHandle.index;
                s.idx_gen  = mhot->indexHandle.generation;
                s.batch_id = mhot->batch_id;
                s.global_base_vertex = mhot->global_base_vertex;
                out.push_back(s);
            }
        }
        return out;
    }

}  // namespace cairns

namespace cairns {

Engine::LoadPrefabBatchResult Engine::RuntimeLoadBatch(
            std::span<const std::filesystem::path> glbs) {
        rhi_.device.WaitIdle();
        LoadPrefabBatchResult r = LoadPrefabBatch(glbs);
        if (r.count > 0) {
            // anim tables are a flat per-prefab GPU array; re-flatten +
            // re-upload picks up the new prefabs. Cost scales with total
            // prefabs (not batch); cheap relative to parse.
            uploadAnimTablesGpu();
            // Backfill any SkinId records that pre-dated the new headers.
            skinning_.skins.ForEachLive(
                [&](cairns::SkinnedAttachment::Hot& h,
                    cairns::SkinnedAttachment::Cold& c) {
                    if (cairns::Prefab::Hot* sht = prefab_store_.prefabs.GetHot(c.scene)) {
                        h.gpu_prefab_header_idx = sht->gpu_prefab_header_idx;
                    }
                });
        }
        return r;
    }

}  // namespace cairns

namespace cairns {

Engine::LoadPrefabBatchResult Engine::LoadPrefabBatch(
            std::span<const std::filesystem::path> glbs) {
        // Bracket every load with a printf + an Instruments signpost so
        // the time profiler distinguishes load work from steady-state
        // frames.
        const char* first_path =
            glbs.empty() ? "<empty>" : glbs.front().filename().c_str();
        CAIRNS_PRINT_ERR("[LOAD] begin batch n=%zu first=%s\n",
                          glbs.size(), first_path);
        CAIRNS_SIGNPOST_INTERVAL_SCOPED("load_prefab_batch", first_path);
#if CAIRNS_ALLOC_TRACE
        const cairns::alloc_count::Snapshot alloc_load_begin =
            cairns::alloc_count::Now();
#endif

        LoadPrefabBatchResult r{};
        r.first_prefab_idx = static_cast<uint32_t>(prefab_store_.prefab_ids.size());

        // Per-stage timing. steady_clock so the trace numbers are
        // wall-clock; the byte-gate doesn't reference them.
        using Clock = std::chrono::steady_clock;
        const auto t_total = Clock::now();
        cairns::LoadTrace trace{};

        // ── parse + validate + prepare resources per glb (no GPU upload yet) ──
        const auto t_parse = Clock::now();
        cairns::ValidationReport vreport{};
        for (const std::filesystem::path& p : glbs) {
            cairns::PrefabId sid = prefab_store_.prefabs.Acquire();
            cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(sid);
            cairns::Prefab::Cold* scold = prefab_store_.prefabs.GetCold(sid);
            if (!shot || !scold ||
                !cairns::LoadPrefabFromGltf(p, *shot, *scold, prefab_store_.meshes, cpu_block_,
                                            prefab_arena_)) {
                CAIRNS_PRINT_ERR("[LoadPrefabBatch] parse failed: %s\n",
                                  p.string().c_str());
                prefab_store_.prefabs.Release(sid);
                continue;
            }
            // Validate against engine caps before upload.
            const uint32_t prefab_idx_for_log =
                static_cast<uint32_t>(prefab_store_.prefab_ids.size());
            if (!ValidatePrefab(*scold, vreport, prefab_idx_for_log)) {
                CAIRNS_PRINT_ERR(
                    "[LoadPrefabBatch] validation failed for %s -- "
                    "skipping prefab.\n", p.string().c_str());
                prefab_store_.prefabs.Release(sid);
                continue;
            }
            cairns::PreparePrefabResources(
                *shot, *scold, rhi_.resources, rhi_.alloc,
                prefab_store_.materials, material_dedup_,
                /*dedup_scope=*/(static_cast<uint64_t>(sid.index) << 16) |
                    sid.generation);
            prefab_store_.prefab_ids.push_back(sid);
            ++r.count;
        }
        trace.Add("parse_gltf",
                   std::chrono::duration<double, std::milli>(
                       Clock::now() - t_parse).count(),
                   0, r.count);
        if (r.count == 0) {
            trace.total_ms = std::chrono::duration<double, std::milli>(
                Clock::now() - t_total).count();
            prefab_store_.last_load_trace = trace;
            return r;
        }

        // ── upload the new batch's prefabs to NEW kDefault buffers ──
        const auto t_upload = Clock::now();
        rhi::Handle<rhi::Buffer> batch_shared_skin =
            rhi::Handle<rhi::Buffer>::Null;
        std::span<const cairns::PrefabId> new_span(
            prefab_store_.prefab_ids.data() + r.first_prefab_idx, r.count);
        if (!cairns::rhi::LoadPrefabsGpu(new_span, prefab_store_.prefabs, prefab_store_.meshes,
                                          rhi_.resources, rhi_.alloc,
                                          &batch_shared_skin)) {
            CAIRNS_PRINT_ERR(
                "[LoadPrefabBatch] LoadPrefabsGpu failed for %u prefabs\n",
                r.count);
            trace.total_ms = std::chrono::duration<double, std::milli>(
                Clock::now() - t_total).count();
            prefab_store_.last_load_trace = trace;
            return r;
        }
        trace.Add("upload_kdefault",
                   std::chrono::duration<double, std::milli>(
                       Clock::now() - t_upload).count(),
                   0, r.count);

        // ══════════════════════════════════════════════════════════════
        // THE MANIFEST. The runtime/post-upload state-agreement
        // transformation, written as an explicit ordered list of named
        // one-liners. Adding engine state that depends on prefabs =
        // add a line here AND its matching invariant in
        // CheckPrefabStateInvariants. There is no other site.
        // A forgotten member is a visible hole in this list, not a
        // silent fallback discovered overnight.
        // ══════════════════════════════════════════════════════════════
        uint32_t batch_mesh_count = 0;
        StampBatchSkinAndMeshIds(new_span, batch_shared_skin,
                                  batch_mesh_count);

        const auto t_group_a = std::chrono::steady_clock::now();
        BuildGroupABindGroups(new_span, batch_shared_skin);
        trace.Add("skin_group_a",
                   std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t_group_a).count(),
                   0, r.count);

        const auto t_cleanup = std::chrono::steady_clock::now();
        ValidateAndCleanupTmps(new_span, r.first_prefab_idx, vreport);
        trace.Add("cleanup_tmps",
                   std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t_cleanup).count());

        BuildMaterialSet2();           // span-independent (idempotent)
        BuildResidentTextures(new_span);
        StampPerPrefabAsset(new_span);
        AppendGlbPaths(new_span, glbs);  // [PICK] log
        // Anim tables: RuntimeLoadBatch re-runs uploadAnimTablesGpu
        // (delta upload) after this returns.

        // ── finalize trace + bump counters ──
        trace.total_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - t_total).count();
        trace.prefabs_added = r.count;
        trace.meshes_added = batch_mesh_count;
        prefab_store_.last_load_trace = trace;
        prefab_store_.last_validation_report = vreport;
        ++prefab_store_.loader_counters.batches_loaded;
        prefab_store_.loader_counters.prefabs_resident += r.count;
        prefab_store_.loader_counters.meshes_resident += batch_mesh_count;
        prefab_store_.loader_counters.last_batch_ms = trace.total_ms;
        if (trace.total_ms > prefab_store_.loader_counters.peak_batch_ms) {
            prefab_store_.loader_counters.peak_batch_ms = trace.total_ms;
        }
        // End-of-batch marker: pairs with [LOAD] begin so log scanning
        // can compute per-batch wall time without hunting for the
        // LoadTrace summary.
        CAIRNS_PRINT_ERR("[LOAD] end batch ms=%.3f count=%u\n",
                          trace.total_ms, r.count);
        // Arena/block high-water -- visible in logcat so the mobile 256 MB
        // budget headroom is observable. prefab_arena (names/children/skin/clip
        // slices) and the cpu_block_ in-class total (pools + prefab tables +
        // entt; mesh cpu* are malloc, not here).
        CAIRNS_PRINT_ERR("[PREFAB-ARENA] used=%zu KiB / %zu KiB cap\n",
                          prefab_arena_.Used() / 1024,
                          prefab_arena_.Capacity() / 1024);
        CAIRNS_PRINT_ERR("[CPU-BLOCK] in_use=%llu KiB / %llu KiB budget\n",
                          (unsigned long long)(cpu_block_.BytesInUse() / 1024),
                          (unsigned long long)(
                              cairns::MemoryBudget::Default().cpu_persistent_bytes
                              / 1024));
#if CAIRNS_ALLOC_TRACE
        cairns::alloc_count::PrintDelta("[LOAD]", alloc_load_begin);
#endif
        return r;
    }

}  // namespace cairns

namespace cairns {

bool Engine::ValidatePrefab(const cairns::Prefab::Cold& cold,
                                cairns::ValidationReport& report,
                                uint32_t prefab_idx) {
        const uint8_t pre_errors = report.issue_count;
        const uint32_t node_count =
            static_cast<uint32_t>(cold.nodes.size());
        if (node_count > kAnimMaxNodes) {
            report.Add(cairns::ValidationSeverity::kError,
                        "node_count > kAnimMaxNodes (256)", prefab_idx);
        }
        uint32_t max_joints = 0;
        for (const cairns::Skin& s : cold.skins) {
            const uint32_t jc = static_cast<uint32_t>(s.jointNodes.size());
            if (jc > max_joints) {
                max_joints = jc;
            }
        }
        if (max_joints > kAnimMaxJoints) {
            report.Add(cairns::ValidationSeverity::kError,
                        "max_joints > kAnimMaxJoints (256)", prefab_idx);
        }
        // Per-mesh weight-sum check requires Mesh::Cold (pre-CleanupTmps)
        // -- runs in LoadPrefabBatch via ValidateMeshWeights below since
        // mesh data is owned by the engine pool, not by Prefab::Cold.
        return report.issue_count == pre_errors;
    }

}  // namespace cairns

namespace cairns {

std::vector<std::filesystem::path> Engine::ResolveDebugGlbPaths(
            uint32_t cursor, uint32_t count) {
        std::vector<std::filesystem::path> out;
        out.reserve(count);
        const uint32_t start =
            cairns::kDebugGlbsToParseStart + cursor;
        const uint32_t end_excl = std::min<uint32_t>(
            start + count,
            cairns::kDebugGlbsToParseStart + cairns::kDebugGlbsToParse);
        for (uint32_t i = start; i < end_excl; ++i) {
            std::filesystem::path p;
            if (!cairns::GetStaticResourceFilepath(cairns::kDebugGlbs[i], p)) {
                // kDebugGlbs is a compile-time roster -- every entry MUST be
                // bundled. A soft skip here silently shrinks the batch (the
                // "99" bug: a missing asset masquerading as a smaller load).
                // Fail loud, naming the gap, so it can never become a count.
                CAIRNS_PRINT_ERR(
                    "[LOAD] FATAL: roster asset unresolved: %s (idx %u) -- "
                    "asset not bundled for this platform; refusing to short "
                    "the batch\n",
                    cairns::kDebugGlbs[i], i);
                std::abort();
            }
            out.push_back(p);
        }
        return out;
    }

}  // namespace cairns

namespace cairns {

bool Engine::ValidateMeshWeights(const cairns::Mesh::Cold& mc,
                              cairns::ValidationReport& report,
                              uint32_t prefab_idx) {
        const uint8_t pre_errors = report.issue_count;
        uint32_t bad_vertices = 0;
        for (const cairns::SkinVertex& sv : mc.cpuSkinAttrs) {
            const float sum = sv.weights.x + sv.weights.y +
                              sv.weights.z + sv.weights.w;
            if (sum < 0.999f || sum > 1.001f) {
                ++bad_vertices;
            }
        }
        if (bad_vertices > 0) {
            // One warning per mesh (not per vertex) to bound issues[].
            report.Add(cairns::ValidationSeverity::kWarning,
                        "weight_sum != 1 on at least one skinned vertex",
                        prefab_idx);
        }
        return report.issue_count == pre_errors;
    }

}  // namespace cairns
