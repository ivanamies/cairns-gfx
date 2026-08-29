#include "control/handlers/render_ops.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

#include "control/command_registry.hpp"
#include "engine_headless.hpp"
#include "util/json.hpp"

namespace cairns::control {

void RegisterRenderOps(CommandRegistry& registry, cairns::Engine* engine) {
    registry.Register(
        "render.frame",
        /*schema=*/json::object(),
        /*doc=*/"Render one frame to final_target_ (clear-only in P1C; scene "
                "render once P2 wires it).",
        [engine](const json&) -> json {
            if (!engine) {
                throw std::runtime_error("engine not initialized");
            }
            if (!cairns::headless::RenderFrame(engine)) {
                throw std::runtime_error("RenderHeadlessFrame failed");
            }
            return json::object();
        });

    registry.Register(
        "io.dumpTexture",
        /*schema=*/json::object(),
        /*doc=*/"Read back the named target to PNG. target='final' for now; "
                "viewport:N / shadow:N / depth:N to follow.",
        [engine](const json& args) -> json {
            if (!engine) {
                throw std::runtime_error("engine not initialized");
            }
            const std::string target = args.value("target", std::string{"final"});
            const std::string path = args.value("path", std::string{});
            if (path.empty()) {
                throw std::runtime_error("missing path arg");
            }
            if (target != "final") {
                throw std::runtime_error("only target=final supported in P1C");
            }
            if (!cairns::headless::DumpFinalTarget(
                    engine, std::filesystem::path(path))) {
                throw std::runtime_error("DumpFinalTarget failed");
            }
            return {{"path", path}};
        });
}

}  // namespace cairns::control
