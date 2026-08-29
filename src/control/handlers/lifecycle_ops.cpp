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
        /*doc=*/"Returns the manifest of registered ops as JSON-Schema. "
                "Sorted alphabetically by name for stable diffs. With ~500 "
                "ops live this is heavy -- prefer tools.search for "
                "discovery.",
        [&registry](const json&) -> json {
            return {{"tools", registry.ToolsList()}};
        });

    registry.Register(
        "tools.search",
        /*schema=*/json::object(),
        /*doc=*/"Substring search the registry. Args: "
                "{query: string, namespace_prefix?: string, limit?: int = "
                "20}. Sorted: exact-prefix-on-name > name-substring > "
                "doc-substring; alphabetical within bucket. Lets an agent "
                "discover the surface without loading the full manifest.",
        [&registry](const json& args) -> json {
            const std::string query =
                args.value("query", std::string{});
            const std::string ns_prefix =
                args.value("namespace_prefix", std::string{});
            const size_t limit =
                args.value("limit", static_cast<size_t>(20));
            return {{"tools",
                     registry.ToolsSearch(query, ns_prefix, limit)}};
        });
}

}  // namespace cairns::control
