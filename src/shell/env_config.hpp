// src/shell/env_config.hpp
//
// Read CAIRNS_* env vars at the shell boundary (sdl-min / cairns_serve) and
// lower them into an EngineConfig. The engine itself never reads
// std::getenv -- shells are the one place env meets policy.

#pragma once

#include "engine.hpp"  // cairns::EngineConfig

namespace cairns::shell {

EngineConfig LoadEngineConfigFromEnv();

// True iff CAIRNS_AGENT_STDIN is set (used by main.cpp to enable the live
// agent transport). Kept separate from EngineConfig because the agent
// transport is shell-side -- the engine doesn't know about it.
bool AgentStdinEnabledFromEnv();

}  // namespace cairns::shell
