// tests/test_particle_emitter.cpp
//
// SPEC: cairns::ParticleRng / SeedParticles (src/render/particle_emitter.hpp)
// TAGS: [spec][particles][determinism]   implementation-order: particles (5)
// BUILDING BLOCK OF: Tier G scenario 1 (particles), scenario 3 (right viewport).
//
// CONTRACT: a cross-platform particle divergence test only works if the emitter
// RNG is byte-identical on every platform. std::rand is non-portable -> banned.
// std::uniform_real_distribution is non-portable across libc++/libstdc++ ->
// banned. Production uses the std::mt19937 ENGINE (standard-mandated sequence)
// and maps to [0,1) by hand.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "render/particle_emitter.hpp"

using cairns::EmitterParams;
using cairns::Particle;
using cairns::ParticleRng;
using cairns::SeedParticles;

SCENARIO("the engine sequence is the standard-mandated std::mt19937(42)",
         "[spec][particles][determinism][regression]") {
    // std::mt19937 is portable by the standard; these are its first outputs for
    // seed 42 on every conforming implementation.
    ParticleRng rng(42);
    REQUIRE(rng.NextU32() == 0x5FE1DC66u);
    REQUIRE(rng.NextU32() == 0xCBEA3DB3u);
    REQUIRE(rng.NextU32() == 0xF362035Cu);
    REQUIRE(rng.NextU32() == 0x2EF5950Eu);
}

SCENARIO("same seed yields identical particles; different seed differs",
         "[spec][particles][determinism]") {
    EmitterParams e{};
    e.seed = 42;
    std::vector<Particle> a;
    std::vector<Particle> b;
    std::vector<Particle> c;
    SeedParticles(1000, e, a);
    SeedParticles(1000, e, b);
    e.seed = 7;
    SeedParticles(1000, e, c);

    REQUIRE(a.size() == 1000);
    bool ab_identical = true;
    bool ac_identical = true;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].pos != b[i].pos || a[i].vel != b[i].vel) {
            ab_identical = false;
        }
        if (a[i].pos != c[i].pos) {
            ac_identical = false;
        }
    }
    REQUIRE(ab_identical);
    REQUIRE_FALSE(ac_identical);
}

SCENARIO("unit draws stay in the half-open zero-to-one range",
         "[spec][particles][determinism]") {
    ParticleRng rng(123);
    for (int i = 0; i < 100000; ++i) {
        const float u = rng.NextUnit();
        REQUIRE(u >= 0.0f);
        REQUIRE(u < 1.0f);
    }
}

SCENARIO("particles seed onto the emitter disk", "[spec][particles]") {
    EmitterParams e{};
    e.disk_radius = 2.0f;
    e.seed = 99;
    std::vector<Particle> p;
    SeedParticles(4096, e, p);
    for (const auto& q : p) {
        const float r2 = q.pos.x * q.pos.x + q.pos.y * q.pos.y;
        REQUIRE(r2 <= e.disk_radius * e.disk_radius + 1e-4f);
    }
}
