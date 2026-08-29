// util/memory_budget.hpp
//
// #229 M0b: the SINGLE source of truth for every memory-domain reservation
// size (SRP). Sized per-platform from the 500-GLB / 500-actor target. Reserved
// once at Engine::GreaterInit; subsystems READ these numbers instead of each
// re-deriving a `constexpr` cap. M5 (GPU reservation) reads gpu_*; the CPU
// arena activation (migrating the load vectors onto ChunkAllocator's fixed
// reservation) reads cpu_persistent_bytes.
//
// Desktop gets the headline 1 GB CPU arena; Android is floored WAY down
// (256 MB) because Adreno devices OOM easily and the persistent CPU arena is
// system RAM separate from the GPU heaps. Per-platform sizing lives HERE so
// there is exactly one place to tune.
#pragma once

#include <cstdint>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace cairns {

struct MemoryBudget {
    // CPU-persistent arena (Prefab/Mesh Cold, anim flat spans, names). The
    // "1 GB arena" the user asked to start from -- carved by ChunkAllocator.
    uint64_t cpu_persistent_bytes = 0;
    // CPU per-frame slab, PER SLOT (render graph / draw scratch). Borrowed
    // from the persistent region; reset each frame. Matches today's BumpArena.
    uint64_t cpu_frame_slab_bytes = 0;
    // GPU resident pool (mesh/idx/attr, textures, anim tables, skin output).
    // The MemoryAllocator-block pre-reservation up to this cap is the M5
    // structural follow-on. Device-cap checked.
    uint64_t gpu_resident_bytes = 0;
    // The persistent skin-output pool: a single fixed GPU buffer reserved once
    // at GreaterInit (already up-front today). Bound to a compute kernel, so it
    // must fit the device's max_storage_buffer_range (Adreno 730 floor 128 MB).
    uint64_t gpu_skin_pool_bytes = 0;
    // GPU host-visible staging ring, PER SLOT (uploads).
    uint64_t gpu_staging_ring_bytes = 0;
    // QuickJS heap reservation (M7).
    uint64_t js_heap_bytes = 0;
    // NDJSON per-command scratch (M6).
    uint64_t ndjson_scratch_bytes = 0;

    // Per-platform defaults. Desktop = headline sizes; mobile = floored to fit
    // Adreno/Apple-mobile budgets (the persistent CPU arena streams residency
    // rather than holding all 500 GLBs at once).
    static MemoryBudget Default() {
        MemoryBudget b;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        b.cpu_persistent_bytes = 256ull * 1024 * 1024;   // 256 MB
        b.gpu_resident_bytes = 128ull * 1024 * 1024;
        b.gpu_skin_pool_bytes = 128ull * 1024 * 1024;    // Adreno 730 floor
#else
        b.cpu_persistent_bytes = 1024ull * 1024 * 1024;  // 1 GB
        b.gpu_resident_bytes = 1024ull * 1024 * 1024;    // 1 GB
        b.gpu_skin_pool_bytes = 1024ull * 1024 * 1024;   // 1 GB
#endif
        b.cpu_frame_slab_bytes = 16ull * 1024 * 1024;    // 16 MB / slot (existing)
        b.gpu_staging_ring_bytes = 64ull * 1024 * 1024;  // 64 MB / slot (existing)
        b.js_heap_bytes = 256ull * 1024 * 1024;          // 256 MB (M7)
        b.ndjson_scratch_bytes = 4ull * 1024 * 1024;     // 4 MB (M6)
        return b;
    }
};

}  // namespace cairns
