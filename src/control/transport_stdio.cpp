#include "control/transport_stdio.hpp"

#include <chrono>
#include <iostream>
#include <string>

#include "control/command_registry.hpp"
#include "util/json.hpp"

namespace cairns::control {

namespace {

int64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void StdioTransport::Run(CommandRegistry& registry, std::istream& in,
                         std::ostream& out, bool& quit_flag,
                         std::atomic<int64_t>* heartbeat_ns) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        json req;
        try {
            req = json::parse(line);
        } catch (const std::exception& e) {
            json resp;
            resp["ok"] = false;
            resp["error"] = {{"code", "bad_json"}, {"message", e.what()}};
            out << resp.dump() << "\n";
            out.flush();
            continue;
        }
        if (heartbeat_ns) {
            heartbeat_ns->store(SteadyNs(), std::memory_order_release);
        }
        const json resp = registry.Dispatch(req);
        out << resp.dump() << "\n";
        for (const json& ev : registry.DrainEvents()) {
            out << ev.dump() << "\n";
        }
        out.flush();
        if (heartbeat_ns) {
            heartbeat_ns->store(SteadyNs(), std::memory_order_release);
        }
        if (quit_flag) {
            return;
        }
    }
}

}  // namespace cairns::control
