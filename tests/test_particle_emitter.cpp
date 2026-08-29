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
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
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

// Pin the NextUnit() FLOAT mapping formula, not just the underlying
// NextU32 sequence. Alternate mappings (e.g.
// (NextU32() & 0xFFFFFF) * (1/16777216), or
// (float)NextU32() / 4294967296.0f) all satisfy the [0,1) range test
// and the same-seed determinism test -- but produce different particle
// bytes than the spec-mandated `(NextU32() >> 8) * (1.0f/16777216.0f)`.
SCENARIO("NextUnit() emits the exact mt19937(42) mapping",
         "[spec][particles][determinism][regression]") {
    ParticleRng rng(42);
    REQUIRE(rng.NextUnit() == Catch::Approx(0.3745400906f).epsilon(1e-7));
    REQUIRE(rng.NextUnit() == Catch::Approx(0.7965429425f).epsilon(1e-7));
    REQUIRE(rng.NextUnit() == Catch::Approx(0.9507142901f).epsilon(1e-7));
    REQUIRE(rng.NextUnit() == Catch::Approx(0.1834347844f).epsilon(1e-7));
    // Bit-exact check on the first draw: pins the literal IEEE 754 bits,
    // catches any platform that drops to soft-float or fast-math reassoc.
    rng = ParticleRng(42);
    const float u0 = rng.NextUnit();
    uint32_t u0_bits = 0;
    std::memcpy(&u0_bits, &u0, sizeof(u0_bits));
    REQUIRE(u0_bits == 0x3EBFC3B8u);
}

// Pin that SeedParticles draws exactly 2 NextUnit() per
// particle (radius + theta). A wrong impl that drew 3 or used a different
// stride would produce different bytes despite matching `count`.
SCENARIO("SeedParticles consumes exactly two NextUnit per particle",
         "[spec][particles][determinism][regression]") {
    EmitterParams e{};
    e.seed = 42;
    e.disk_radius = 1.0f;
    std::vector<Particle> p;
    SeedParticles(2, e, p);
    REQUIRE(p.size() == 2);
    // The first particle should match what we get by hand using NextUnit
    // twice (radius, then theta).
    ParticleRng oracle(42);
    const float r0 = e.disk_radius * std::sqrt(oracle.NextUnit());
    const float theta0 = oracle.NextUnit() * 6.2831853071795864769f;
    const float x0 = r0 * std::cos(theta0);
    const float y0 = r0 * std::sin(theta0);
    REQUIRE(p[0].pos.x == Catch::Approx(x0).epsilon(1e-6));
    REQUIRE(p[0].pos.y == Catch::Approx(y0).epsilon(1e-6));
    // Second particle picks up the engine state at draw 3+4. If
    // SeedParticles drew a different count per particle (e.g. 3), this
    // would diverge.
    const float r1 = e.disk_radius * std::sqrt(oracle.NextUnit());
    const float theta1 = oracle.NextUnit() * 6.2831853071795864769f;
    const float x1 = r1 * std::cos(theta1);
    const float y1 = r1 * std::sin(theta1);
    REQUIRE(p[1].pos.x == Catch::Approx(x1).epsilon(1e-6));
    REQUIRE(p[1].pos.y == Catch::Approx(y1).epsilon(1e-6));
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
