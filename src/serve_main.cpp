// cairns_serve -- headless NDJSON control plane host.
//
// P0a: bare echo loop. Reads lines from stdin, echoes the same JSON-ish line
// back to stdout. No Engine, no GPU, no SDL. Exists to prove the CMake split:
// cairns_core static lib + thin shells (cairns_app=sdl-min + cairns_serve).
//
// P0c will replace this with the CommandRegistry + Dispatch loop.

#include <iostream>
#include <string>

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::cout << "{\"ok\":true,\"echo\":\"" << line << "\"}\n";
        std::cout.flush();
    }
    return 0;
}
