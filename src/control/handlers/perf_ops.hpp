// control/handlers/perf_ops.hpp
//
// perf.last + the determinism stubs (rng.seed, time.set). Reads the
// static Timer accumulator state defined in util/timer.hpp so it returns
// the same numbers as the "[Timer]" stderr lines (frozen contract).

#pragma once

namespace cairns { class Engine; }

namespace cairns::control {

class CommandRegistry;

// engine may be null (cairns_serve before engine_ok / cairns_app's
// pre-engine ops). rng.seed is a no-op-but-recorded when engine is null.
void RegisterPerfOps(CommandRegistry& registry, cairns::Engine& engine);

}  // namespace cairns::control
