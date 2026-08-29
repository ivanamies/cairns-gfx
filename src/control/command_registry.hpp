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
#include <mutex>
#include <string>
#include <vector>

#include "util/alloc_tags.hpp"
#include "util/json.hpp"
#include "util/print_allocator.hpp"

namespace cairns::control {

struct Command {
    std::string name;  // owned canonical name
    json schema;
    std::string doc;
    std::function<json(const json&)> fn;
    // When non-empty, this command is a deprecated alias. Dispatch annotates
    // the result with _deprecated_alias_for; ToolsList surfaces the field so
    // an agent reading the manifest sees the alias relationship.
    std::string aliased_for;
};

// #215 (name, op_id) sorted lookup table. Binary-searched at Dispatch
// time -- one std::string construction per request (from the json op
// field) and one comparison cascade, no hash.
struct CommandIndex {
    std::string name;
    uint32_t op_id = 0;
    bool operator<(const CommandIndex& o) const { return name < o.name; }
};

class CommandRegistry {
public:
    static CommandRegistry& Instance();

    void Register(std::string&& name, json&& schema, std::string&& doc,
                  std::function<json(const json&)>&& fn);

    // Register `alias` as a deprecated forwarder to `canonical`. Dispatching
    // the alias runs the canonical handler and decorates the response with
    // `_deprecated_alias_for: canonical`. ToolsList shows the alias with
    // deprecated:true + aliased_for:canonical. One-release alias policy --
    // legacy top-level op names redirect to their cairns.* canonical homes.
    void RegisterAlias(std::string&& alias, std::string&& canonical);

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

    // Event channel: any handler may publish a structured event. The
    // transport drains the queue after each dispatch and writes one NDJSON
    // line per event ({"event": <topic>, "data": {...}}). Today events are
    // unconditional broadcasts -- subscribe semantics (per-topic filter,
    // subscription ids) are a follow-up; clients can already drop frames by
    // topic. Thread-safe so a render-thread handler can publish too.
    void PublishEvent(std::string&& topic, json&& data);
    std::vector<json> DrainEvents();

private:
    CommandRegistry() = default;
    // #215 flat array indexed by OpId + sorted name lookup table. Replaces
    // std::unordered_map<std::string, Command>. Dispatch: binary search
    // the sorted_names_ vector to resolve name -> op_id, then commands_
    // [op_id] is the handler. RegisterAlias adds another entry in
    // sorted_names_ pointing at the canonical op_id. Names live on
    // Command::name so sorted_names_ stores std::string copies (small
    // hashable map -> sorted vector trade); reserve once and re-sort
    // after each Register.
    std::vector<Command,
                cairns::print_allocator<Command,
                                        cairns::tags::RegistryCommands>>
        commands_;
    std::vector<CommandIndex,
                cairns::print_allocator<CommandIndex,
                                        cairns::tags::RegistrySortedNames>>
        sorted_names_;
    std::mutex events_m_;
    std::vector<json,
                cairns::print_allocator<json,
                                        cairns::tags::RegistryEvents>>
        events_;
};

}  // namespace cairns::control
