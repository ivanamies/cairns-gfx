// control/transport_stdio.hpp
//
// Synchronous line-oriented NDJSON transport: read a line from stdin, parse
// JSON, Dispatch, write the response line to stdout, repeat until EOF.
//
// stdout is protocol-only -- anything that writes a non-JSON line corrupts
// the stream (see plan risk #1). All logging routes to stderr.
//
// C-stdio (FILE*) rather than iostream: <iostream>'s static std::ios_base::Init
// allocates cout/cin/cerr + locale before main (#229 M0b kills that alloc).

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace cairns::control {

class CommandRegistry;

class StdioTransport {
public:
    // Synchronous Run loop. Returns when `in` reaches EOF or quit_flag flips.
    // The handler for `app.quit` should flip a quit flag the caller passes in.
    //
    // W1 watchdog hook: |heartbeat_ns| is updated to the current
    // steady_clock ns BEFORE each Dispatch and AFTER each response is
    // flushed. A separate watchdog thread monitors this counter and
    // calls std::abort() if it stops advancing. Pass nullptr to opt out.
    static void Run(CommandRegistry& registry, std::FILE* in, std::FILE* out,
                    bool& quit_flag,
                    std::atomic<int64_t>* heartbeat_ns = nullptr);
};

}  // namespace cairns::control
