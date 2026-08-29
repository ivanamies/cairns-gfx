// engine/particle_system.hpp
//
// Particle subsystem state, split out of the Engine god class (C2 S3). Groups
// the compute kernel + render shaders + the double-buffered SSBOs + the
// cross-thread parity handshake (mutex/cv/counters) into one cohesive unit --
// the render thread writes parity_out under parity_m so the game thread chains
// frame N+1's parity_in from frame N's parity_out. Engine owns one of these and
// its init/draw systems operate on it; keeping the lock WITH the data it guards
// is the point (they must never drift apart).

#pragma once

#include <condition_variable>
#include <mutex>

#include "rhi/resource_manager.hpp"

namespace cairns {

struct ParticleSystem {
    static constexpr uint32_t kParticleCount = 512;

    rhi::Handle<rhi::Kernel> kernel;
    rhi::Handle<rhi::Shader> render_shader;
    rhi::Handle<rhi::Shader> render_offscreen;
    // #222 Phase A.1 fix: id-less variant for the no-id forward pass.
    rhi::Handle<rhi::Shader> render_offscreen_noid;
    rhi::Handle<rhi::Buffer> ssbo[2];

    // Render thread writes back parity_out under parity_m so the game thread can
    // chain frame N+1's parity_in from frame N's parity_out without sharing a
    // mutable counter. In steady state Acquire(slot N+1) already happens-after
    // Release of slot N, so the cv wait is a no-op.
    std::mutex parity_m;
    std::condition_variable parity_cv;
    uint32_t latest_parity_out = 0;
    uint64_t latest_parity_frame = 0;

    // Particle RNG seed; takes effect on the next initParticles.
    uint32_t random_seed = 42;
    // #229 C3: the sim/draw gate moved to a per-scene ParticleEmitterComponent
    // (Engine::AnyBoundSceneHasEmitter). No global enable flag here.
};

}  // namespace cairns
