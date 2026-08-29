// src/util/hud_stats.hpp
//
// PURE profiling math + an INJECTABLE HUD stat source. No GPU, no imgui, no
// hw_counter. This is the building block the imgui stability test rests on.
//
// WHY: the imgui HUD shows cpu-ms / FPS / a frame-time graph, all derived from
// Timer (which reads cntvct_el0 -- wall-clock, non-deterministic). You cannot
// golden-test an overlay whose numbers change every run. The fix is to make the
// HUD read from a HudStats value that is normally filled from Timer but can be
// OVERRIDDEN with fixed numbers in a test. Then "mock numbers to imgui, capture
// the screen, check stability" becomes a deterministic golden image.

#ifndef CAIRNS_UTIL_HUD_STATS_HPP
#define CAIRNS_UTIL_HUD_STATS_HPP

#include <array>
#include <cstdint>

namespace cairns {

// avg microseconds for a Timer slot (accum_times_[s] / accum_itrs_[s]).
constexpr uint64_t SlotAvgUs(uint64_t accum_us, uint64_t iters) {
    return iters ? accum_us / iters : 0;
}
constexpr float UsToMs(uint64_t us) { return static_cast<float>(us) / 1000.0f; }
constexpr float MsToFps(float ms) { return ms > 0.0f ? 1000.0f / ms : 0.0f; }

// The numbers the HUD draws. Normally filled from Timer once per frame; in a
// test, set directly so the overlay is byte-stable.
struct HudStats {
    static constexpr uint32_t kGraph = 64;
    float cpu_ms = 0.0f;
    float fps = 0.0f;
    std::array<float, kGraph> frame_ms{};  // ring of recent frame times
    uint32_t graph_head = 0;

    // Deterministic fixture: a HUD that always reads 16.6 ms / 60 fps and a flat
    // graph. The imgui golden test seeds this so the overlay never moves.
    static HudStats Mock() {
        HudStats s;
        s.cpu_ms = 16.6f;
        s.fps = 60.0f;
        s.frame_ms.fill(16.6f);
        s.graph_head = 0;
        return s;
    }
};

// Build HudStats from raw Timer accumulators for the frame + frame slot.
inline HudStats HudFromTimer(uint64_t frame_accum_us, uint64_t frame_iters,
                             const std::array<float, HudStats::kGraph>& graph,
                             uint32_t head) {
    HudStats s;
    const uint64_t avg_us = SlotAvgUs(frame_accum_us, frame_iters);
    s.cpu_ms = UsToMs(avg_us);
    s.fps = MsToFps(s.cpu_ms);
    s.frame_ms = graph;
    s.graph_head = head;
    return s;
}

}  // namespace cairns

#endif  // CAIRNS_UTIL_HUD_STATS_HPP
