// util/json.hpp
//
// Thin alias over the vendored nlohmann/json single header. Inclusion is
// scoped to src/control/* and src/util/json.hpp consumers so the rest of the
// engine (RHI, render graph, scene) doesn't pay the compile-time cost of the
// heavy header. If we ever swap to simdjson or a smaller library this is the
// one place that changes.

#pragma once

// nlohmann/json uses exceptions internally; this single TU surface
// must NOT be compiled with -fno-exceptions. Localize to control/.
#include "json/json.hpp"

namespace cairns {

using json = nlohmann::json;

}  // namespace cairns
