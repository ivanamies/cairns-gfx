#pragma once

// Two-clock determinism. The sim consumes only a fixed dt + a sim-frame counter;
// wall time lives in a separate scheduling layer. The sim code path is identical
// in production and golden capture -- only the clock source differs -- so a
// "verification mode" can never drift from the real mode.
//
// WHICH PRODUCT USES WHICH CLOCK:
//
//   FixedClock (kFixedDt, deterministic)        WallClock (real time)
//   --------------------------------------      ------------------------------------
//   * scene rotation angle (sim_angle_deg_)     * main-loop accumulator (production)
//   * particle compute dt (push/UBO)            * render interpolation alpha source
//   * particle spawn / RNG (seed 42)            * cpu-ms / FPS HUD + frame-time graph
//   * any future physics (rigidbody/fluid)      * ImGui overlay animations
//   * golden-capture accumulator (CAIRNS_DUMP)  * vsync / present timing
//                                               * logging timestamps
//                                               * audio playback (real-time)
//
// Invariant: nothing WallClock produces may be written into persistent sim state.
// The accumulator only emits a per-frame step count + an alpha in [0,1) for render
// interpolation; neither is stored across frames.

#include <cstdint>

#include "util/timer.hpp"  // cairns::timestamp_ns (static inline)

namespace cairns {

class FrameClock {
public:
    virtual ~FrameClock() = default;
    // Wall-clock-equivalent delta since the last call, in seconds.
    virtual float Tick() = 0;
};

// Production: real elapsed seconds between frames.
class WallClock : public FrameClock {
public:
    float Tick() override {
        const uint64_t now = cairns::timestamp_ns();
        if (last_ns_ == 0) {
            last_ns_ = now;
            return 0.0f;
        }
        const float dt = static_cast<float>(now - last_ns_) / 1.0e9f;
        last_ns_ = now;
        return dt;
    }

private:
    uint64_t last_ns_ = 0;
};

// Golden capture: a constant dt -> one sim step per render frame, byte-reproducible.
class FixedClock : public FrameClock {
public:
    explicit FixedClock(float dt) : dt_(dt) {}
    float Tick() override { return dt_; }

private:
    float dt_;
};

}  // namespace cairns
