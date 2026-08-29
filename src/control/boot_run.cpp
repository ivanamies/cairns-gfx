#include "control/boot_run.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#include <SDL3/SDL.h>

#include "control/command_registry.hpp"
#include "util/json.hpp"
#include "util/log.hpp"
#include "util/misc.hpp"

namespace cairns::control {

void RunBootScript(CommandRegistry& registry) {
    std::filesystem::path path;
    if (!cairns::GetStaticResourceFilepath("run.js", path)) {
        CAIRNS_PRINT_ERR(
            "[run.js] MANDATORY asset not bundled (resolve failed). Fix "
            "the build's asset pipeline.\n");
        std::abort();
    }
    SDL_IOStream* io = SDL_IOFromFile(path.string().c_str(), "rb");
    if (!io) {
        CAIRNS_PRINT_ERR("[run.js] failed to open %s\n",
                          path.string().c_str());
        std::abort();
    }
    const Sint64 size = SDL_GetIOSize(io);
    std::string code;
    if (size > 0) {
        code.resize(static_cast<size_t>(size));
        const size_t n =
            SDL_ReadIO(io, code.data(), static_cast<size_t>(size));
        code.resize(n);
    }
    SDL_CloseIO(io);
    if (code.empty()) {
        CAIRNS_PRINT_ERR("[run.js] %s is empty.\n", path.string().c_str());
        std::abort();
    }
    CAIRNS_PRINT("[run.js] dispatching %zu bytes from %s\n",
                  code.size(), path.string().c_str());

    json req;
    req["op"] = "cairns.script.eval";
    req["args"] = json::object();
    req["args"]["code"] = code;
    const json resp = registry.Dispatch(req);
    if (resp.contains("error")) {
        CAIRNS_PRINT_ERR("[run.js] eval failed: %s\n", resp.dump().c_str());
        std::abort();
    }
    CAIRNS_PRINT("[run.js] ok\n");
}

}  // namespace cairns::control
