#include "control/command_registry.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <utility>

namespace cairns::control {

CommandRegistry& CommandRegistry::Instance() {
    static CommandRegistry registry;
    return registry;
}

void CommandRegistry::Register(std::string&& name, json&& schema, std::string&& doc,
                               std::function<json(const json&)>&& fn) {
    commands_[std::move(name)] =
        Command{std::move(schema), std::move(doc), std::move(fn), {}};
}

void CommandRegistry::RegisterAlias(std::string&& alias, std::string&& canonical) {
    auto canonical_copy = canonical;
    // Alias handler dispatches to the canonical op via the registry singleton.
    // Singleton lookup at call time (not capture) so a later registry rewire
    // is visible.
    Command cmd;
    cmd.schema = json::object();
    cmd.doc = "DEPRECATED alias for `" + canonical + "`.";
    cmd.aliased_for = canonical;
    cmd.fn = [canonical_copy](const json& args) -> json {
        json req;
        req["op"] = canonical_copy;
        req["args"] = args;
        const json resp = CommandRegistry::Instance().Dispatch(req);
        // Bubble error responses up as exceptions so the outer Dispatch
        // boundary fills them in identically to a native handler error.
        if (!resp.value("ok", false)) {
            std::string msg = "alias dispatch failed";
            if (resp.contains("error") && resp["error"].is_object() &&
                resp["error"].contains("message")) {
                msg = resp["error"]["message"].get<std::string>();
            }
            throw std::runtime_error(msg);
        }
        json result = resp.value("result", json::object());
        if (result.is_object()) {
            result["_deprecated_alias_for"] = canonical_copy;
        }
        return result;
    };
    commands_[std::move(alias)] = std::move(cmd);
}

json CommandRegistry::Dispatch(const json& request) {
    json resp;
    if (request.contains("id")) {
        resp["id"] = request["id"];
    }
    const auto op_it = request.find("op");
    if (op_it == request.end() || !op_it->is_string()) {
        resp["ok"] = false;
        resp["error"] = {{"code", "missing_op"},
                         {"message", "request must have a string 'op' field"}};
        return resp;
    }
    const std::string op = op_it->get<std::string>();
    const auto cmd_it = commands_.find(op);
    if (cmd_it == commands_.end()) {
        resp["ok"] = false;
        resp["error"] = {{"code", "unknown_op"}, {"message", op}};
        return resp;
    }
    json args = json::object();
    if (request.contains("args") && request["args"].is_object()) {
        args = request["args"];
    }
    try {
        json result = cmd_it->second.fn(args);
        resp["ok"] = true;
        resp["result"] = std::move(result);
    } catch (const std::exception& e) {
        resp["ok"] = false;
        resp["error"] = {{"code", "handler_error"}, {"message", e.what()}};
    } catch (...) {
        resp["ok"] = false;
        resp["error"] = {{"code", "handler_error"},
                         {"message", "unknown exception"}};
    }
    return resp;
}

json CommandRegistry::ToolsList() const {
    // Materialize into a sorted map first so the manifest order is stable
    // across builds (unordered_map iteration is implementation-defined).
    std::map<std::string, const Command*> sorted;
    for (const auto& [name, cmd] : commands_) {
        sorted.emplace(name, &cmd);
    }
    json out = json::array();
    for (const auto& [name, cmd] : sorted) {
        json entry = {{"name", name}, {"doc", cmd->doc}, {"schema", cmd->schema}};
        if (!cmd->aliased_for.empty()) {
            entry["deprecated"] = true;
            entry["aliased_for"] = cmd->aliased_for;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

namespace {

// Lower-case ASCII compare. Cheap; doesn't need to handle non-ASCII names.
bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    auto tolower = [](char c) -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j) {
            if (tolower(haystack[i + j]) != tolower(needle[j])) {
                break;
            }
        }
        if (j == needle.size()) {
            return true;
        }
    }
    return false;
}

bool starts_with_ci(const std::string& s, const std::string& prefix) {
    if (prefix.size() > s.size()) {
        return false;
    }
    auto tolower = [](char c) -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (tolower(s[i]) != tolower(prefix[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

json CommandRegistry::ToolsSearch(const std::string& query,
                                  const std::string& namespace_prefix,
                                  size_t limit) const {
    // Bucket matches by relevance:
    //   0 = exact-prefix-on-name (best)
    //   1 = name-substring
    //   2 = doc-substring
    // Within each bucket, sort alphabetically by name for stable output.
    struct Match {
        int bucket;
        std::string name;
        const Command* cmd;
        bool operator<(const Match& o) const {
            if (bucket != o.bucket) return bucket < o.bucket;
            return name < o.name;
        }
    };
    std::vector<Match> hits;
    for (const auto& [name, cmd] : commands_) {
        if (!namespace_prefix.empty() &&
            !starts_with_ci(name, namespace_prefix)) {
            continue;
        }
        int bucket = -1;
        if (!query.empty() && starts_with_ci(name, query)) {
            bucket = 0;
        } else if (contains_ci(name, query)) {
            bucket = 1;
        } else if (contains_ci(cmd.doc, query)) {
            bucket = 2;
        }
        if (bucket < 0) {
            continue;
        }
        hits.push_back(Match{bucket, name, &cmd});
    }
    std::sort(hits.begin(), hits.end());
    if (limit == 0) {
        limit = 20;
    }
    if (hits.size() > limit) {
        hits.resize(limit);
    }
    json out = json::array();
    for (const auto& m : hits) {
        json entry = {{"name", m.name},
                      {"doc", m.cmd->doc},
                      {"schema", m.cmd->schema}};
        if (!m.cmd->aliased_for.empty()) {
            entry["deprecated"] = true;
            entry["aliased_for"] = m.cmd->aliased_for;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

void CommandRegistry::Clear() {
    commands_.clear();
    {
        std::lock_guard<std::mutex> lk(events_m_);
        events_.clear();
    }
}

void CommandRegistry::PublishEvent(std::string&& topic, json&& data) {
    json env;
    env["event"] = std::move(topic);
    env["data"] = std::move(data);
    std::lock_guard<std::mutex> lk(events_m_);
    events_.push_back(std::move(env));
}

std::vector<json> CommandRegistry::DrainEvents() {
    std::vector<json> out;
    std::lock_guard<std::mutex> lk(events_m_);
    out.swap(events_);
    return out;
}

}  // namespace cairns::control
