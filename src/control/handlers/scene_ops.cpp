#include "control/handlers/scene_ops.hpp"

#include <atomic>
#include <stdexcept>
#include <string>

#include "control/command_registry.hpp"
#include "engine_headless.hpp"
#include "util/json.hpp"

namespace cairns::control {

namespace {

// Synthetic id counters. These IDs are visible to the agent + script.eval
// flow today, but the rendering side doesn't yet honor them -- the canonical
// demo's scene composition lights up once the resizing-and-cameras plan
// lands and Engine grows the per-viewport extract / final_target_-routed
// swap pass. Until then the ops are surface-stable stubs.
std::atomic<uint64_t> g_world_counter{0};
std::atomic<uint64_t> g_asset_counter{0};
std::atomic<uint64_t> g_entity_counter{0};
std::atomic<uint64_t> g_viewport_counter{0};

}  // namespace

void RegisterSceneOps(CommandRegistry& registry, cairns::Engine& engine) {
    registry.Register(
        "cairns.world.create",
        /*schema=*/json::object(),
        /*doc=*/"Allocate a new world. Returns a synthetic world id; the "
                "engine-side EnTT registry pre-allocates kMaxWorlds slots "
                "but doesn't yet track these ids -- stub until P2 lands.",
        [](const json&) -> json {
            return {{"world", g_world_counter.fetch_add(1)}};
        });

    registry.Register(
        "cairns.world.clear",
        /*schema=*/json::object(),
        /*doc=*/"Reset a world's entity set. Stub.",
        [](const json& args) -> json {
            return {{"world", args.value("world", uint64_t{0})}};
        });

    registry.Register(
        "cairns.asset.load",
        /*schema=*/json::object(),
        /*doc=*/"Load a GLB / texture / audio file. Today: returns a "
                "synthetic asset id (no GPU upload); real load lands "
                "with the AssetRegistry hookup.",
        [](const json& args) -> json {
            const std::string path = args.value("path", std::string{});
            return {{"asset", g_asset_counter.fetch_add(1)},
                    {"path", path}};
        });

    registry.Register(
        "cairns.world.instantiate",
        /*schema=*/json::object(),
        /*doc=*/"Instantiate one asset into a world at a transform. Stub "
                "id; engine doesn't yet honor the entity placement.",
        [](const json& args) -> json {
            return {{"entity", g_entity_counter.fetch_add(1)},
                    {"world", args.value("world", uint64_t{0})},
                    {"asset", args.value("asset", uint64_t{0})}};
        });

    registry.Register(
        "cairns.world.instantiateGrid",
        /*schema=*/json::object(),
        /*doc=*/"Instantiate N copies of asset(s) into a world in a grid. "
                "Returns the id range; engine doesn't yet honor placement.",
        [](const json& args) -> json {
            const uint64_t n = args.value("count", uint64_t{0});
            const uint64_t start = g_entity_counter.fetch_add(n);
            return {{"first", start},
                    {"last", start + (n > 0 ? n - 1 : 0)},
                    {"count", n}};
        });

    registry.Register(
        "cairns.viewport.open",
        /*schema=*/json::object(),
        /*doc=*/"Open a viewport rendering a world. Today: returns id 0 + "
                "binds the existing final_target_; future viewports get "
                "their own offscreen targets when P2 wires per-viewport "
                "extract.",
        [engine = &engine](const json& args) -> json {
            const uint64_t vp = g_viewport_counter.fetch_add(1);
            const uint32_t w = args.value("w", cairns::headless::GetFinalTargetWidth(engine));
            const uint32_t h = args.value("h", cairns::headless::GetFinalTargetHeight(engine));
            return {{"viewport", vp}, {"w", w}, {"h", h}};
        });

    registry.Register(
        "cairns.viewport.setCamera",
        /*schema=*/json::object(),
        /*doc=*/"Set a viewport's camera pose. Records intent today; "
                "actual camera plumbing lands with the resizing-and-cameras "
                "plan.",
        [](const json& args) -> json {
            return {{"viewport", args.value("viewport", uint64_t{0})}};
        });

    registry.Register(
        "cairns.viewport.setWorld",
        /*schema=*/json::object(),
        /*doc=*/"Bind a viewport to a different world. Stub.",
        [](const json& args) -> json {
            return {{"viewport", args.value("viewport", uint64_t{0})},
                    {"world", args.value("world", uint64_t{0})}};
        });

    registry.Register(
        "cairns.window.resize",
        /*schema=*/json::object(),
        /*doc=*/"Reallocate final_target_ at the new dimensions. Real work "
                "in headless mode; future windowed-mode wiring routes "
                "through Engine::requestResizeFrameBuffer.",
        [engine = &engine](const json& args) -> json {
            const uint64_t w64 = args.value("w", uint64_t{1280});
            const uint64_t h64 = args.value("h", uint64_t{720});
            const uint32_t w = static_cast<uint32_t>(w64);
            const uint32_t h = static_cast<uint32_t>(h64);
            if (!cairns::headless::ResizeFinalTarget(engine, w, h)) {
                throw std::runtime_error("ResizeFinalTarget failed");
            }
            return {{"w", w}, {"h", h}};
        });

    // Legacy top-level names as deprecated aliases (one release).
    registry.RegisterAlias("world.create", "cairns.world.create");
    registry.RegisterAlias("world.clear", "cairns.world.clear");
    registry.RegisterAlias("asset.load", "cairns.asset.load");
    registry.RegisterAlias("world.instantiate", "cairns.world.instantiate");
    registry.RegisterAlias("world.instantiateGrid",
                           "cairns.world.instantiateGrid");
    registry.RegisterAlias("viewport.open", "cairns.viewport.open");
    registry.RegisterAlias("viewport.setCamera", "cairns.viewport.setCamera");
    registry.RegisterAlias("viewport.setWorld", "cairns.viewport.setWorld");
    registry.RegisterAlias("window.resize", "cairns.window.resize");
}

}  // namespace cairns::control
