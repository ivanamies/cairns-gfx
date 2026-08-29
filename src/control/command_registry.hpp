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
    // When non-empty, this command is a deprecated alias. Dispatch annotates
    // the result with _deprecated_alias_for; ToolsList surfaces the field so
    // an agent reading the manifest sees the alias relationship.
    std::string aliased_for;
};

class CommandRegistry {
public:
    static CommandRegistry& Instance();

    void Register(std::string name, json schema, std::string doc,
                  std::function<json(const json&)> fn);

    // Register `alias` as a deprecated forwarder to `canonical`. Dispatching
    // the alias runs the canonical handler and decorates the response with
    // `_deprecated_alias_for: canonical`. ToolsList shows the alias with
    // deprecated:true + aliased_for:canonical. One-release alias policy --
    // legacy top-level op names redirect to their cairns.* canonical homes.
    void RegisterAlias(std::string alias, std::string canonical);

    // Returns a fully-formed response JSON ({"id":..., "ok":true/false, ...}).
    // Never throws; handler exceptions are caught and converted to error
    // responses so a misbehaving op cannot kill the dispatch loop.
    json Dispatch(const json& request);

    // Emits the {name -> {schema, doc, deprecated?, aliased_for?}} manifest
    // for `tools.list`. Sorted alphabetically by name for stable diffs.
    json ToolsList() const;

    // Substring search over name + doc. Sorted: exact-prefix > name-substr >
    // doc-substr. Used by `tools.search` to keep the 500-op manifest cost off
    // the agent's context window.
    json ToolsSearch(const std::string& query,
                     const std::string& namespace_prefix,
                     size_t limit) const;

    // For tests / reset between sessions. Not threadsafe.
    void Clear();

private:
    CommandRegistry() = default;
    std::unordered_map<std::string, Command> commands_;
};

}  // namespace cairns::control
