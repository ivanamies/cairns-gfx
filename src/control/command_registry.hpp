// control/command_registry.hpp
//
// NDJSON command registry + transport-agnostic dispatch.
//
// One CommandRegistry; three frontends (decision #1 in the plan):
//   - stdio NDJSON (cairns_serve / cairns_app agent transport)
//   - ImGui buttons (P5: every editor action goes through Dispatch)
//   - QuickJS (P5: every registered op bound as a JS function)
//
// Each Register() carries (schema, doc, handler). The schema rides registration
// so `tools.list` can emit a JSON-Schema manifest that an external agent or
// MCP server consumes -- decision #4 (self-describing surface).

#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "util/json.hpp"

namespace cairns::control {

struct Command {
    json schema;
    std::string doc;
    std::function<json(const json&)> fn;
};

class CommandRegistry {
public:
    static CommandRegistry& Instance();

    void Register(std::string name, json schema, std::string doc,
                  std::function<json(const json&)> fn);

    // Returns a fully-formed response JSON ({"id":..., "ok":true/false, ...}).
    // Never throws; handler exceptions are caught and converted to error
    // responses so a misbehaving op cannot kill the dispatch loop.
    json Dispatch(const json& request);

    // Emits the {name -> {schema, doc}} manifest for `tools.list`.
    json ToolsList() const;

    // For tests / reset between sessions. Not threadsafe.
    void Clear();

private:
    CommandRegistry() = default;
    std::unordered_map<std::string, Command> commands_;
};

}  // namespace cairns::control
