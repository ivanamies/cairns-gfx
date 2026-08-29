// control/handlers/perf_ops.hpp
//
// perf.last + the determinism stubs (rng.seed, time.set). Reads the
// static Timer accumulator state defined in util/timer.hpp so it returns
// the same numbers as the "[Timer]" stderr lines (frozen contract).

#pragma once

namespace cairns::control {

class CommandRegistry;

void RegisterPerfOps(CommandRegistry& registry);

}  // namespace cairns::control
