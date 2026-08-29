// control/command_registry.hpp
//
// NDJSON command registry + transport-agnostic dispatch.
//
// One CommandRegistry; three frontends:
//   - stdio NDJSON (cairns_serve / cairns_app agent transport)
//   - ImGui buttons (every editor action goes through Dispatch)
//   - QuickJS (every op reachable via cairns.dispatch)
//
// Each Register() carries (schema, doc, handler). The schema rides registration
// so `tools.list` can emit a JSON-Schema manifest that an external agent or
// MCP server consumes (self-describing surface).

#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "util/json.hpp"

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

// (name, op_id) lookup table. MUST stay sorted -- Dispatch binary-searches
// it to resolve name -> op_id (no hash, no per-request string copy).
struct CommandIndex {
    std::string name;
    uint32_t op_id = 0;
    bool operator<(const CommandIndex& o) const { return name < o.name; }
};

class CommandRegistry {
public:
    // Reserve to the ~60-op surface (+headroom) so Register doesn't
    // doubling-grow the flat command vectors during boot registration.
    CommandRegistry() {
        commands_.reserve(128);
        sorted_names_.reserve(128);
    }
    // Non-copyable/movable: holds a mutex and hands out `this` to alias
    // forwarders. Constructed + passed by reference (no process singleton).
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

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
    // unconditional broadcasts -- no subscribe semantics (per-topic filter,
    // subscription ids) exist yet; clients can already drop events by
    // topic. Thread-safe so a render-thread handler can publish too.
    void PublishEvent(std::string&& topic, json&& data);
    std::vector<json> DrainEvents();

private:
    // Flat array indexed by OpId + sorted name lookup table. Dispatch:
    // binary search sorted_names_ to resolve name -> op_id, then
    // commands_[op_id] is the handler. RegisterAlias adds another
    // sorted_names_ entry pointing at the canonical op_id. Register
    // inserts in order so the table stays sorted.
    std::vector<Command> commands_;
    std::vector<CommandIndex> sorted_names_;
    std::mutex events_m_;
    std::vector<json> events_;
};

}  // namespace cairns::control
