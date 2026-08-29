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
    cfg.tiny_quad = (std::getenv("CAIRNS_TINY_QUAD") != nullptr);

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

    if (const char* p = std::getenv("CAIRNS_N")) {
        cfg.entity_count = std::atoi(p);
    }
    if (const char* p = std::getenv("CAIRNS_SCALE")) {
        cfg.entity_scale = static_cast<float>(std::atof(p));
    }

    return cfg;
}

bool AgentStdinEnabledFromEnv() {
    return std::getenv("CAIRNS_AGENT_STDIN") != nullptr;
}

}  // namespace cairns::shell
