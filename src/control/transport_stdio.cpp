#include "control/transport_stdio.hpp"

#include <iostream>
#include <string>

#include "control/command_registry.hpp"
#include "util/json.hpp"

namespace cairns::control {

void StdioTransport::Run(CommandRegistry& registry, std::istream& in,
                         std::ostream& out, bool& quit_flag) {
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
        const json resp = registry.Dispatch(req);
        out << resp.dump() << "\n";
        for (const json& ev : registry.DrainEvents()) {
            out << ev.dump() << "\n";
        }
        out.flush();
        if (quit_flag) {
            return;
        }
    }
}

}  // namespace cairns::control
