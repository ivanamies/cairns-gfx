#include "control/agent_stdin_drain.hpp"

#include <cstdlib>
#include <iostream>
#include <utility>

#include "control/command_registry.hpp"
#include "util/json.hpp"

namespace cairns::control {

AgentStdinDrain::~AgentStdinDrain() { Stop(); }

void AgentStdinDrain::Start() {
    if (!std::getenv("CAIRNS_AGENT_STDIN")) {
        return;
    }
    enabled_ = true;
    reader_ = std::thread([this] { ReaderLoop(); });
}

void AgentStdinDrain::Stop() {
    if (!enabled_) {
        return;
    }
    quit_.store(true);
    // std::getline blocks on stdin; the typical exit path is the user
    // (or the controlling agent) closing stdin -> getline returns false ->
    // loop exits. Without that we'd need a portable interruptible read.
    // For now, detach if the reader is still blocked at shutdown so the
    // app can exit cleanly. Acceptable because the reader thread holds no
    // resources that outlive process exit.
    if (reader_.joinable()) {
        reader_.detach();
    }
    enabled_ = false;
}

void AgentStdinDrain::ReaderLoop() {
    std::string line;
    while (!quit_.load() && std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::lock_guard<std::mutex> lk(queue_m_);
        queue_.push_back(std::move(line));
        line.clear();
    }
}

void AgentStdinDrain::Drain(CommandRegistry& registry, std::ostream& out) {
    if (!enabled_) {
        return;
    }
    std::deque<std::string> batch;
    {
        std::lock_guard<std::mutex> lk(queue_m_);
        batch.swap(queue_);
    }
    for (const std::string& line : batch) {
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
        const json resp = registry.Dispatch(req);
        out << resp.dump() << "\n";
        out.flush();
    }
}

}  // namespace cairns::control
