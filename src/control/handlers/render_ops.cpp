#include "control/handlers/render_ops.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

#include "control/command_registry.hpp"
#include "engine_headless.hpp"
#include "util/json.hpp"

namespace cairns::control {

void RegisterRenderOps(CommandRegistry& registry, cairns::Engine& engine) {
    registry.Register(
        "cairns.render.frame",
        /*schema=*/json::object(),
        /*doc=*/"Render one headless frame to final_target_ (fixed clock).",
        [engine = &engine](const json&) -> json {
            if (!cairns::headless::RenderFrame(engine)) {
                throw std::runtime_error("RenderHeadlessFrame failed");
            }
            return json::object();
        });

    registry.Register(
        "cairns.io.dumpTexture",
        /*schema=*/json::object(),
        /*doc=*/"Read back the named target to PNG. target='final' "
                "(headless final_target_) or 'window' (queued swapchain dump).",
        [engine = &engine](const json& args) -> json {
            const std::string target = args.value("target", std::string{"final"});
            const std::string path = args.value("path", std::string{});
            if (path.empty()) {
                throw std::runtime_error("missing path arg");
            }
            if (target == "final") {
                if (!cairns::headless::DumpFinalTarget(
                        engine, std::filesystem::path(path))) {
                    throw std::runtime_error("DumpFinalTarget failed");
                }
                return {{"path", path}, {"target", target}};
            }
            if (target == "window") {
                // Queue the windowed swapchain dump for the next frame.
                // The actual readback happens in Frames::End after the
                // engine renders. Only meaningful in cairns_app (sdl-min);
                // in cairns_serve there's no swapchain so it'd be a no-op.
                if (!cairns::headless::RequestWindowDump(
                        engine, std::filesystem::path(path))) {
                    throw std::runtime_error("RequestWindowDump failed");
                }
                return {{"path", path},
                        {"target", target},
                        {"note", "queued for next frame end"}};
            }
            throw std::runtime_error("unsupported target -- use final or window");
        });

    registry.RegisterAlias("render.frame", "cairns.render.frame");
    registry.RegisterAlias("io.dumpTexture", "cairns.io.dumpTexture");
}

}  // namespace cairns::control
