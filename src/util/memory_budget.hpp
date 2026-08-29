// util/memory_budget.hpp
//
// The SINGLE source of truth for every memory-domain reservation size.
// Sized per-platform from the 500-GLB / 500-actor target; reserved once at
// Engine::GreaterInit. Subsystems READ these numbers instead of each
// re-deriving a `constexpr` cap.
//
// Desktop gets the headline 1 GB CPU arena; mobile is floored down because
// Adreno devices OOM easily and the persistent CPU arena is system RAM
// separate from the GPU heaps. Per-platform sizing lives HERE so there is
// exactly one place to tune.
#pragma once

#include <cstdint>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace cairns {

struct MemoryBudget {
    // CPU-persistent arena (Prefab/Mesh Cold, anim flat spans, names);
    // carved by ChunkAllocator.
    uint64_t cpu_persistent_bytes = 0;
    // CPU per-frame slab, PER SLOT (render graph / draw scratch). Borrowed
    // from the persistent region; reset each frame.
    uint64_t cpu_frame_slab_bytes = 0;
    // GPU resident pool (mesh/idx/attr, textures, anim tables, skin output).
    // Device-cap checked.
    uint64_t gpu_resident_bytes = 0;
    // The persistent skin-output pool: a single fixed GPU buffer reserved once
    // at GreaterInit (already up-front today). Bound to a compute kernel, so it
    // must fit the device's max_storage_buffer_range (Adreno 730 floor 128 MB).
    uint64_t gpu_skin_pool_bytes = 0;
    // GPU host-visible staging ring, PER SLOT (uploads).
    uint64_t gpu_staging_ring_bytes = 0;
    // QuickJS heap reservation.
    uint64_t js_heap_bytes = 0;
    // NDJSON per-command scratch.
    uint64_t ndjson_scratch_bytes = 0;

    // Per-platform defaults. Desktop = headline sizes; mobile = floored to fit
    // Adreno/Apple-mobile budgets (the persistent CPU arena streams residency
    // rather than holding all 500 GLBs at once).
    static MemoryBudget Default() {
        MemoryBudget b;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        // The CPU arena is NOT bound by the SSBO limit (that floor applies
        // to gpu_skin_pool). The S22 has 8 GB; 512 MB lets the 100-GLB load
        // (incl. the transient mesh cpu* peak) fit in-block with headroom.
        b.cpu_persistent_bytes = 512ull * 1024 * 1024;   // 512 MB
        b.gpu_resident_bytes = 128ull * 1024 * 1024;
        b.gpu_skin_pool_bytes = 128ull * 1024 * 1024;    // Adreno 730 floor (SSBO)
#else
        b.cpu_persistent_bytes = 1024ull * 1024 * 1024;  // 1 GB
        b.gpu_resident_bytes = 1024ull * 1024 * 1024;    // 1 GB
        // 256 MB on desktop + webgpu (mobile is 128 MB above). The whole-pool
        // storage bind must fit max_storage_buffer_range: 128 MB is the mobile
        // floor (Adreno-730 / S22), but desktop runs 500 actors (5x mobile) whose
        // skinned output overflows 128 MB, so desktop + webgpu get 256 MB (their
        // ranges are GBs). SkinPoolFitsDevice enforces the per-platform bind.
        b.gpu_skin_pool_bytes = 256ull * 1024 * 1024;    // 256 MB (desktop + webgpu)
#endif
        b.cpu_frame_slab_bytes = 16ull * 1024 * 1024;    // 16 MB / slot
        b.gpu_staging_ring_bytes = 64ull * 1024 * 1024;  // 64 MB / slot
        b.js_heap_bytes = 256ull * 1024 * 1024;          // 256 MB
        b.ndjson_scratch_bytes = 4ull * 1024 * 1024;     // 4 MB
        return b;
    }
};

}  // namespace cairns
