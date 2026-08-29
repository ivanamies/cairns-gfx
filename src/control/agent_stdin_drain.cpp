#include "control/agent_stdin_drain.hpp"

#include <cstdio>
#include <utility>

#include "control/command_registry.hpp"
#include "control/stdio_lines.hpp"
#include "util/json.hpp"

namespace cairns::control {

AgentStdinDrain::~AgentStdinDrain() { Stop(); }

void AgentStdinDrain::Start(bool enabled) {
    if (!enabled) {
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
    // ReadLine blocks on stdin; the normal exit path is the controlling
    // agent closing stdin -> ReadLine returns false -> loop exits. There is
    // no portable interruptible read, so if the reader is still blocked at
    // shutdown, detach -- it holds no resources that outlive process exit.
    if (reader_.joinable()) {
        reader_.detach();
    }
    enabled_ = false;
}

void AgentStdinDrain::ReaderLoop() {
    std::string line;
    while (!quit_.load() && ReadLine(stdin, line)) {
        if (line.empty()) {
            continue;
        }
        std::lock_guard<std::mutex> lk(queue_m_);
        queue_.push_back(std::move(line));
        line.clear();
    }
}

void AgentStdinDrain::Drain(CommandRegistry& registry, std::FILE* out) {
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
            WriteLine(out, resp.dump());
            std::fflush(out);
            continue;
        }
        const json resp = registry.Dispatch(req);
        WriteLine(out, resp.dump());
        std::fflush(out);
    }
}

}  // namespace cairns::control
