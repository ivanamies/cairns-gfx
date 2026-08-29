// engine/engine_config.hpp
//
// Engine startup options, split out of engine.hpp so the shell boundary
// (env_config) and other callers can name EngineConfig without pulling the
// whole 6000-line engine header.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cairns {

// Engine-level startup options. The shell (sdl-min / cairns_serve) lowers
// CAIRNS_* env knobs into this struct at startup so the engine never reads
// std::getenv directly. Empty / default-constructed values mean "use the
// engine's built-in default" so partial population is safe.
struct EngineConfig {
    // CAIRNS_DUMP: when non-empty, FixedClock + one-shot dump on
    // kGoldenDumpFrame to this path, then exit(0). Drives byte-gates.
    std::filesystem::path dump_path;

    // CAIRNS_CAM_POSE: pin every viewport's fly controller to this fixed
    // (pos, yaw_rad, pitch_rad). Disables live fly input.
    struct CamPose {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float yaw = 0.0f;
        float pitch = 0.0f;
    };
    std::optional<CamPose> cam_pose;

    // CAIRNS_GLB: comma-separated list of glb names / paths. Empty =>
    // engine default (kDebugGlbs window).
    std::vector<std::string> glb_overrides;

    // Run the engine with a FixedClock (deterministic frame dt) WITHOUT the
    // CLI dump_path side effect of "dump at kGoldenDumpFrame then exit(0)."
    // The golden test harness needs the determinism without the exit -- it
    // can't be killed mid-suite. dump_path != "" still implies fixed clock
    // (CLI behaviour preserved); this bool is the only way to ask for fixed
    // clock without the dump+exit path.
    bool use_fixed_clock = false;

    // A.2: gate particle_sim + particle_draw at the source. Default OFF for
    // every ladder rung + every scenario except G1 (and G3-right viewport).
    // Particle compute writes via std::rand-shaped paths consumed downstream;
    // with this off, L1 "pipeline + clear + one draw" is actually that.
    // Set via Engine::EnableParticles(bool) at runtime; CLI app and serve
    // shell default ON via main / serve_main lowering.
    bool particles_enabled = false;

    // CAIRNS_ANIM_VERT_REPORT: per-frame [ANIM-VERTS] receipt of the vertices
    // the skin kernel actually dispatches (post-cull, post-cap). Diagnostic for
    // the scale rungs + frustum cull; off by default.
    bool anim_vert_report = false;
};

}  // namespace cairns
