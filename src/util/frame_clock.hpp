// util/frame_clock.hpp
//
// Deterministic two-clock architecture (Fiedler fixed-timestep).
//
//   WallClock   -- live mode (production). SDL_GetTicks-based seconds delta.
//   FixedClock  -- golden mode (CAIRNS_DUMP). Returns kFixedDt every tick.
//
// The game thread feeds clock dt into a Fiedler accumulator. sim_frame_,
// sim_angle_deg_, and the particle compute step count are advanced by the
// accumulator in fixed kFixedDt increments; render_angle_deg_ interpolates
// with alpha. Same sim code path in golden capture and production -- only
// the clock source differs.
//
// Which sim/render state uses which clock:
//
//   FixedClock (deterministic)        | WallClock (real time)
//   --------------------------------- | ---------------------------------
//   scene rotation angle              | main-loop accumulator dt source
//   particle compute dt (UBO)         | render interpolation alpha source
//   particle spawn / RNG (seed 42)    | cpu-ms / FPS HUD + frame-time graph
//   future physics                    | ImGui overlay anims; vsync; logging
//
// Invariant: nothing WallClock produces is written into persistent sim state
// -- the accumulator only emits a per-frame step count + alpha in [0, 1),
// never stored across frames. No code outside this header should call
// SDL_GetTicks() for sim/render state.

#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

namespace cairns {

inline constexpr double   kFixedDt          = 1.0 / 60.0;
inline constexpr uint32_t kMaxStepsPerFrame = 5;
inline constexpr double   kMaxFrameDt       = 0.250;
inline constexpr float    kRotDegPerSec     = 22.5f;
inline constexpr uint64_t kGoldenDumpFrame  = 60;

class FrameClock {
public:
    virtual ~FrameClock() = default;
    // Seconds elapsed since the previous Tick(). First Tick returns 0.0 so
    // the first frame doesn't see startup jitter feeding the accumulator.
    virtual double Tick() = 0;
};

class WallClock final : public FrameClock {
public:
    double Tick() override {
        const uint64_t now = SDL_GetTicks();
        if (last_ms_ == 0) {
            last_ms_ = now;
            return 0.0;
        }
        const double dt = (now - last_ms_) / 1000.0;
        last_ms_ = now;
        return dt;
    }

private:
    uint64_t last_ms_ = 0;
};

class FixedClock final : public FrameClock {
public:
    explicit FixedClock(double dt = kFixedDt) : dt_(dt) {}
    double Tick() override { return dt_; }

private:
    double dt_;
};

}  // namespace cairns
