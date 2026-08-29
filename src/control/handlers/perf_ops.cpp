#include "control/handlers/perf_ops.hpp"

#include <array>

#include "control/command_registry.hpp"
#include "util/json.hpp"
#include "util/timer.hpp"

namespace cairns::control {

void RegisterPerfOps(CommandRegistry& registry) {
    registry.Register(
        "perf.last",
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

    // rng.seed + time.set are P4 stubs: the registry surface lights up so
    // an external caller / tools.list manifest exposes them, but the
    // engine-side wiring (seeded RNG in scene init, FixedClock advancement)
    // is a separate change tracked in the task list.
    registry.Register(
        "rng.seed",
        /*schema=*/json::object(),
        /*doc=*/"Seed the engine RNG (P4 stub; wiring pending).",
        [](const json& args) -> json {
            const uint64_t n = args.value("n", uint64_t{1});
            return {{"seed", n}, {"note", "stub; engine RNG wiring pending"}};
        });

    registry.Register(
        "time.set",
        /*schema=*/json::object(),
        /*doc=*/"Set the deterministic sim time (P4 stub; FixedClock "
                "advancement wiring pending).",
        [](const json& args) -> json {
            const double t = args.value("t", 0.0);
            return {{"t", t}, {"note", "stub; clock wiring pending"}};
        });
}

}  // namespace cairns::control
