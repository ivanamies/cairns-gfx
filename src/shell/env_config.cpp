// src/shell/env_config.cpp

#include "shell/env_config.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace cairns::shell {

EngineConfig LoadEngineConfigFromEnv() {
    EngineConfig cfg;

    if (const char* p = std::getenv("CAIRNS_DUMP")) {
        cfg.dump_path = p;
    }

    if (const char* p = std::getenv("CAIRNS_CAM_POSE")) {
        EngineConfig::CamPose pose;
        int n = std::sscanf(p, "%f,%f,%f,%f,%f", &pose.x, &pose.y, &pose.z,
                            &pose.yaw, &pose.pitch);
        if (n == 5) {
            cfg.cam_pose = pose;
        }
    }

    if (const char* p = std::getenv("CAIRNS_GLB")) {
        std::string spec(p);
        size_t start = 0;
        while (start <= spec.size()) {
            size_t comma = spec.find(',', start);
            std::string tok = spec.substr(
                start, comma == std::string::npos ? std::string::npos
                                                  : comma - start);
            if (!tok.empty()) {
                cfg.glb_overrides.push_back(std::move(tok));
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
    }

    // CLI shells default particles ON (the windowed/serve app expects
    // them); tests use the particles_enabled=false default and enable
    // per-scenario. CAIRNS_NO_PARTICLES overrides for headless debugging.
    cfg.particles_enabled = (std::getenv("CAIRNS_NO_PARTICLES") == nullptr);

    cfg.anim_vert_report = (std::getenv("CAIRNS_ANIM_VERT_REPORT") != nullptr);

    return cfg;
}

bool AgentStdinEnabledFromEnv() {
    return std::getenv("CAIRNS_AGENT_STDIN") != nullptr;
}

}  // namespace cairns::shell
