#include "control/handlers/perf_ops.hpp"

#include <array>

#include "control/command_registry.hpp"
#include "engine_headless.hpp"
#include "util/json.hpp"
#include "util/timer.hpp"

namespace cairns::control {

void RegisterPerfOps(CommandRegistry& registry, cairns::Engine& engine) {
    registry.Register(
        "cairns.perf.last",
        /*schema=*/json::object(),
        /*doc=*/"Returns the static Timer accumulator state -- per-slot "
                "{name, accum_us, avg_us, frames}. Same numbers as the "
                "'[Timer]' stderr lines; either channel is valid.",
        [](const json&) -> json {
            json slots = json::array();
            for (uint32_t i = 0; i < cairns::Timer::kMaxSlots; ++i) {
                const uint64_t iters = cairns::Timer::accum_itrs_[i];
                if (iters == 0) {
                    continue;
                }
                const uint64_t accum = cairns::Timer::accum_times_[i];
                const char* name = cairns::Timer::slot_names_[i];
                slots.push_back({
                    {"slot", i},
                    {"name", name ? name : "?"},
                    {"accum_us", accum},
                    {"avg_us", accum / iters},
                    {"frames", iters},
                });
            }
            return {{"slots", slots}};
        });

    // rng.seed updates Engine::random_seed_, which initParticles reads via
    // std::srand. Init-time particles already ran, so the seed applies at
    // the NEXT particle (re-)init -- not retroactive on the current state.
    registry.Register(
        "cairns.rng.seed",
        /*schema=*/json::object(),
        /*doc=*/"Seed the engine RNG. Takes effect at next initParticles; "
                "today not yet retroactive on the live particle SSBO.",
        [engine = &engine](const json& args) -> json {
            const uint64_t n = args.value("n", uint64_t{42});
            const uint32_t n32 = static_cast<uint32_t>(n);
            cairns::headless::SetRandomSeed(engine, n32);
            return {{"seed", n32}, {"engine_bound", true}};
        });

    // time.set is a stub: FixedClock advancement isn't directly settable
    // (render.frame({dt}) is the planned shape). The op records intent only.
    registry.Register(
        "cairns.time.set",
        /*schema=*/json::object(),
        /*doc=*/"Set the deterministic sim time (stub today; "
                "render.frame({dt}) is the planned shape).",
        [](const json& args) -> json {
            const double t = args.value("t", 0.0);
            return {{"t", t}, {"note", "stub; render.frame({dt}) planned"}};
        });

    // Deterministic sim clock (Unity Time.{time,deltaTime,frameCount}).
    // Fixed timestep; read-only.
    registry.Register(
        "cairns.time.get",
        /*schema=*/json::object(),
        /*doc=*/"Deterministic sim clock: {time (s), dt (s), frame}. Fixed "
                "timestep -- time = frame * dt.",
        [&engine](const json&) -> json {
            double time = 0.0;
            double dt = 0.0;
            uint64_t frame = 0;
            cairns::headless::TimeNow(&engine, time, dt, frame);
            return {{"time", time}, {"dt", dt}, {"frame", frame}};
        });

    registry.RegisterAlias("perf.last", "cairns.perf.last");
    registry.RegisterAlias("rng.seed", "cairns.rng.seed");
    registry.RegisterAlias("time.set", "cairns.time.set");
}

}  // namespace cairns::control
