// control/transport_stdio.hpp
//
// Synchronous line-oriented NDJSON transport: read a line from stdin, parse
// JSON, Dispatch, write the response line to stdout, repeat until EOF.
//
// stdout is protocol-only -- anything that writes a non-JSON line corrupts
// the stream (see plan risk #1). All logging routes to stderr by P0c.

#pragma once

#include <iosfwd>

namespace cairns::control {

class CommandRegistry;

class StdioTransport {
public:
    // Synchronous Run loop. Returns when stdin reaches EOF or quit_flag flips.
    // The handler for `app.quit` should flip a quit flag the caller passes in.
    static void Run(CommandRegistry& registry, std::istream& in,
                    std::ostream& out, bool& quit_flag);
};

}  // namespace cairns::control
