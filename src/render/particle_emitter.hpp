// src/render/particle_emitter.hpp
//
// DETERMINISTIC, PLATFORM-INDEPENDENT particle seeding, built on the STL.
//  - std::rand is not specified across implementations (Apple libc++ / NDK
//    libc++ / glibc all differ), so a cross-platform particle-buffer diff
//    would fail on the RNG, not the GPU.
//  - std::mt19937 IS portable -- the standard fully specifies its sequence.
//    std::uniform_real_distribution / std::uniform_int_distribution are NOT
//    (implementation-defined; libc++ and libstdc++ differ). So: the
//    std::mt19937 ENGINE, mapped to [0,1) BY HAND.

#ifndef CAIRNS_RENDER_PARTICLE_EMITTER_HPP
#define CAIRNS_RENDER_PARTICLE_EMITTER_HPP

#include <cstdint>
#include <random>
#include <vector>

#include <glm/glm.hpp>

namespace cairns {

// STL engine, hand-rolled uniform. NextUnit() returns [0,1) with 24 mantissa
// bits from the top of the 32-bit word -- identical bytes on every platform
// because std::mt19937's sequence is standard-mandated and the mapping is ours.
struct ParticleRng {
    std::mt19937 eng;
    explicit ParticleRng(uint32_t seed) : eng(seed) {}
    uint32_t NextU32() { return static_cast<uint32_t>(eng()); }
    float NextUnit() {
        return static_cast<float>(NextU32() >> 8) * (1.0f / 16777216.0f);
    }
};

struct Particle {
    glm::vec4 pos;   // xyz + pad
    glm::vec4 vel;   // xyz + life
};

struct EmitterParams {
    float disk_radius = 1.0f;   // VK-tutorial style: spawn on a disk
    float speed = 0.25f;
    uint32_t seed = 42u;        // matches the engine default
};

// Deterministic initial particle state. Same (count, params) -> same bytes,
// everywhere. This is what the divergence test hashes before any GPU step.
inline void SeedParticles(uint32_t count, const EmitterParams& e,
                          std::vector<Particle>& out) {
    ParticleRng rng(e.seed);
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const float r = e.disk_radius * glm::sqrt(rng.NextUnit());
        const float theta = rng.NextUnit() * 6.2831853071795864769f;
        const float x = r * glm::cos(theta);
        const float y = r * glm::sin(theta);
        out[i].pos = glm::vec4(x, y, 0.0f, 1.0f);
        out[i].vel = glm::vec4(x, y, 0.0f, 0.0f) *
                     (e.speed / (r > 0.0f ? r : 1.0f));
        out[i].vel.w = 1.0f;  // life
    }
}

}  // namespace cairns

#endif  // CAIRNS_RENDER_PARTICLE_EMITTER_HPP
