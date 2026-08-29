// control/agent_stdin_drain.hpp
//
// Live-app agent transport (decision #6 in the headless-editor plan): a
// reader thread on stdin pushes NDJSON command lines into a queue; the main
// thread drains the queue each frame and dispatches each line through the
// shared CommandRegistry. Responses go to stdout.
//
// Gated by CAIRNS_AGENT_STDIN=1 -- without the env var, cairns_app's stdin
// stays untouched (the normal interactive run doesn't get its stdin captured
// and stdout doesn't get prefixed with JSON responses).
//
// The reader uses std::thread + std::mutex (per the threading-primitives
// memory: no semaphores / latches / barriers / shared_mutex / atomic wait).

#pragma once

#include <atomic>
#include <deque>
#include <iosfwd>
#include <mutex>
#include <string>
#include <thread>

namespace cairns::control {

class CommandRegistry;

class AgentStdinDrain {
public:
    AgentStdinDrain() = default;
    ~AgentStdinDrain();

    AgentStdinDrain(const AgentStdinDrain&) = delete;
    AgentStdinDrain& operator=(const AgentStdinDrain&) = delete;

    // No-op if CAIRNS_AGENT_STDIN env var is unset; in that case Enabled()
    // returns false and Drain() is a fast no-op.
    void Start();
    void Stop();

    bool Enabled() const { return enabled_; }

    // Pulls all queued lines + dispatches each. Responses go to `out`.
    // Call once per frame from the main thread. Bounded: drains everything
    // available at the call moment, never blocks for more.
    void Drain(CommandRegistry& registry, std::ostream& out);

private:
    void ReaderLoop();

    bool enabled_ = false;
    std::atomic<bool> quit_{false};
    std::thread reader_;
    std::mutex queue_m_;
    std::deque<std::string> queue_;
};

}  // namespace cairns::control
