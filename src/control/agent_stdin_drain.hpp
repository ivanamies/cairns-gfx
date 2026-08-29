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

#include "util/alloc_tags.hpp"
#include "util/print_allocator.hpp"

namespace cairns::control {

class CommandRegistry;

class AgentStdinDrain {
public:
    AgentStdinDrain() = default;
    ~AgentStdinDrain();

    AgentStdinDrain(const AgentStdinDrain&) = delete;
    AgentStdinDrain& operator=(const AgentStdinDrain&) = delete;

    // Shell-side caller decides whether to enable (sdl-min reads
    // CAIRNS_AGENT_STDIN; the agent transport is shell policy, not engine
    // policy). When |enabled| is false, Enabled() returns false and Drain()
    // is a fast no-op.
    void Start(bool enabled);
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
    using tagged_str = std::basic_string<
        char, std::char_traits<char>,
        cairns::print_allocator<char,
                                cairns::tags::StdinDrainQueueItem>>;
    std::deque<tagged_str,
               cairns::print_allocator<tagged_str,
                                       cairns::tags::StdinDrainQueue>>
        queue_;
};

}  // namespace cairns::control
