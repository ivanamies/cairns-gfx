#include "control/transport_stdio.hpp"

#include <chrono>
#include <cstdio>
#include <string>

#include "control/command_registry.hpp"
#include "control/stdio_lines.hpp"
#include "util/json.hpp"

namespace cairns::control {

namespace {

int64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void StdioTransport::Run(CommandRegistry& registry, std::FILE* in,
                         std::FILE* out, bool& quit_flag,
                         std::atomic<int64_t>* heartbeat_ns) {
    std::string line;
    while (ReadLine(in, line)) {
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
            WriteLine(out, resp.dump());
            std::fflush(out);
            continue;
        }
        if (heartbeat_ns) {
            heartbeat_ns->store(SteadyNs(), std::memory_order_release);
        }
        const json resp = registry.Dispatch(req);
        WriteLine(out, resp.dump());
        for (const json& ev : registry.DrainEvents()) {
            WriteLine(out, ev.dump());
        }
        std::fflush(out);
        if (heartbeat_ns) {
            heartbeat_ns->store(SteadyNs(), std::memory_order_release);
        }
        if (quit_flag) {
            return;
        }
    }
}

}  // namespace cairns::control
