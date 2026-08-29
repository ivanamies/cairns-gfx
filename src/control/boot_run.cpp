#include "control/boot_run.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#include "control/command_registry.hpp"
#include "platform/platform.hpp"
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
    std::string code;
    if (!cairns::platform::ReadAsset(path, code)) {
        CAIRNS_PRINT_ERR("[run.js] failed to open %s\n",
                          path.string().c_str());
        std::abort();
    }
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
