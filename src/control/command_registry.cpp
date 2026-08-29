#include "control/command_registry.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <string_view>
#include <utility>

namespace cairns::control {

namespace {

// #215 binary-search the sorted lookup table for `name`. Returns op_id or
// UINT32_MAX if not found.
constexpr uint32_t kNoOp = 0xFFFFFFFFu;
// #229 M6: takes a string_view + a heterogeneous comparator so dispatch
// doesn't copy the op name into a std::string (nor build a scratch
// CommandIndex) per command.
uint32_t FindOp(const std::vector<CommandIndex>& sorted, std::string_view name) {
    auto it = std::lower_bound(
        sorted.begin(), sorted.end(), name,
        [](const CommandIndex& idx, std::string_view n) { return idx.name < n; });
    if (it == sorted.end() || it->name != name) {
        return kNoOp;
    }
    return it->op_id;
}

void InsertSorted(std::vector<CommandIndex>& sorted, std::string name,
                  uint32_t op_id) {
    CommandIndex idx{std::move(name), op_id};
    auto it = std::lower_bound(sorted.begin(), sorted.end(), idx);
    sorted.insert(it, std::move(idx));
}

}  // namespace

void CommandRegistry::Register(std::string&& name, json&& schema, std::string&& doc,
                               std::function<json(const json&)>&& fn) {
    const uint32_t op_id = static_cast<uint32_t>(commands_.size());
    Command cmd;
    cmd.name = name;
    cmd.schema = std::move(schema);
    cmd.doc = std::move(doc);
    cmd.fn = std::move(fn);
    commands_.push_back(std::move(cmd));
    InsertSorted(sorted_names_, std::move(name), op_id);
}

void CommandRegistry::RegisterAlias(std::string&& alias, std::string&& canonical) {
    // Register a new command for the alias that dispatches to the canonical
    // op via the registry singleton. Pre-#215 used a separate per-alias
    // Command storing aliased_for + a forwarding lambda; same shape here,
    // just landed in the flat commands_ vector.
    const uint32_t op_id = static_cast<uint32_t>(commands_.size());
    std::string canonical_copy = canonical;
    Command cmd;
    cmd.name = alias;
    cmd.schema = json::object();
    cmd.doc = "DEPRECATED alias for `" + canonical + "`.";
    cmd.aliased_for = canonical;
    cmd.fn = [this, canonical_copy](const json& args) -> json {
        json req;
        req["op"] = canonical_copy;
        req["args"] = args;
        const json resp = this->Dispatch(req);
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
    commands_.push_back(std::move(cmd));
    InsertSorted(sorted_names_, std::move(alias), op_id);
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
    // #229 M6: op as a view into the request (no per-command string copy).
    const std::string_view op = op_it->get_ref<const std::string&>();
    const uint32_t op_id = FindOp(sorted_names_, op);
    if (op_id == kNoOp || op_id >= commands_.size()) {
        resp["ok"] = false;
        resp["error"] = {{"code", "unknown_op"}, {"message", op}};
        return resp;
    }
    // #229 M6: pass args by const-ref straight from the request -- no subtree
    // copy. The no-args case materializes one empty object (null default costs
    // nothing); a pointer avoids the ternary's copy-to-common-type.
    const auto args_it = request.find("args");
    const json* args_ptr = nullptr;
    json empty_args;  // null -> no allocation unless the no-args branch runs
    if (args_it != request.end() && args_it->is_object()) {
        args_ptr = &*args_it;
    } else {
        empty_args = json::object();
        args_ptr = &empty_args;
    }
    try {
        json result = commands_[op_id].fn(*args_ptr);
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
    // sorted_names_ is already sorted alphabetically -- iterate it directly
    // for stable manifest output (was a std::map<string, const Command*>
    // scratch in the unordered_map era).
    json out = json::array();
    for (const CommandIndex& idx : sorted_names_) {
        const Command& cmd = commands_[idx.op_id];
        json entry = {{"name", cmd.name}, {"doc", cmd.doc}, {"schema", cmd.schema}};
        if (!cmd.aliased_for.empty()) {
            entry["deprecated"] = true;
            entry["aliased_for"] = cmd.aliased_for;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

namespace {

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
    struct Match {
        int bucket;
        const Command* cmd;
        bool operator<(const Match& o) const {
            if (bucket != o.bucket) return bucket < o.bucket;
            return cmd->name < o.cmd->name;
        }
    };
    std::vector<Match> hits;
    hits.reserve(commands_.size());
    for (const Command& cmd : commands_) {
        if (!namespace_prefix.empty() &&
            !starts_with_ci(cmd.name, namespace_prefix)) {
            continue;
        }
        int bucket = -1;
        if (!query.empty() && starts_with_ci(cmd.name, query)) {
            bucket = 0;
        } else if (contains_ci(cmd.name, query)) {
            bucket = 1;
        } else if (contains_ci(cmd.doc, query)) {
            bucket = 2;
        }
        if (bucket < 0) {
            continue;
        }
        hits.push_back(Match{bucket, &cmd});
    }
    std::sort(hits.begin(), hits.end());
    if (limit == 0) {
        limit = 20;
    }
    if (hits.size() > limit) {
        hits.resize(limit);
    }
    json out = json::array();
    for (const Match& m : hits) {
        json entry = {{"name", m.cmd->name},
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
    sorted_names_.clear();
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
