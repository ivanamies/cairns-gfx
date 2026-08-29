#include "control/handlers/lifecycle_ops.hpp"

#include "control/command_registry.hpp"
#include "util/json.hpp"

namespace cairns::control {

namespace {

constexpr const char* kEngineVersion = "0.1.0";
constexpr const char* kProtocolVersion = "1";

}  // namespace

void RegisterLifecycleOps(CommandRegistry& registry, bool* quit_flag) {
    registry.Register(
        "app.ping",
        /*schema=*/json::object(),
        /*doc=*/"Liveness probe. Returns {pong: true}.",
        [](const json&) -> json { return {{"pong", true}}; });

    registry.Register(
        "app.version",
        /*schema=*/json::object(),
        /*doc=*/"Returns engine and protocol versions.",
        [](const json&) -> json {
            return {{"engine", kEngineVersion},
                    {"protocol", kProtocolVersion}};
        });

    registry.Register(
        "app.quit",
        /*schema=*/json::object(),
        /*doc=*/"Signals the transport loop to exit cleanly after the next "
                "response is flushed.",
        [quit_flag](const json&) -> json {
            if (quit_flag) {
                *quit_flag = true;
            }
            return {{"quitting", true}};
        });

    registry.Register(
        "tools.list",
        /*schema=*/json::object(),
        /*doc=*/"Returns the manifest of registered ops as JSON-Schema.",
        [&registry](const json&) -> json {
            return {{"tools", registry.ToolsList()}};
        });
}

}  // namespace cairns::control
