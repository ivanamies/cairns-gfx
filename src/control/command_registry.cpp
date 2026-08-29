#include "control/command_registry.hpp"

#include <exception>

namespace cairns::control {

CommandRegistry& CommandRegistry::Instance() {
    static CommandRegistry registry;
    return registry;
}

void CommandRegistry::Register(std::string name, json schema, std::string doc,
                               std::function<json(const json&)> fn) {
    commands_[std::move(name)] =
        Command{std::move(schema), std::move(doc), std::move(fn)};
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
    json out = json::array();
    for (const auto& [name, cmd] : commands_) {
        out.push_back({{"name", name},
                       {"doc", cmd.doc},
                       {"schema", cmd.schema}});
    }
    return out;
}

void CommandRegistry::Clear() { commands_.clear(); }

}  // namespace cairns::control
