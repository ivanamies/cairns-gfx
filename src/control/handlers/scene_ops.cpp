#include "control/handlers/scene_ops.hpp"

#include <atomic>
#include <cstdio>
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
                "engine-side EnTT registry pre-allocates kMaxScenes slots "
                "but doesn't yet track these ids -- stub until P2 lands.",
        [](const json&) -> json {
            return {{"world", g_world_counter.fetch_add(1)}};
        });

    registry.Register(
        "cairns.world.clear",
        /*schema=*/json::object(),
        /*doc=*/"#269: nuke every entity in active_scene_. Returns "
                "{cleared:N}. Leaks any held skin_output_pool_ slices + "
                "alias buffer handles (no skin Release path yet); fine "
                "for occasional debug-session resets, do not loop.",
        [&engine](const json&) -> json {
            const uint32_t n =
                cairns::headless::ClearActiveWorld(&engine);
            return {{"cleared", n}};
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
        /*doc=*/"Open a new viewport on the swap pane (#194). Caps at the "
                "engine's kNumViewports. Returns {viewport: 'vpN'} on "
                "success (engine-assigned monotonic name; never reused) "
                "or {error:'capped'} when full. Default layout tiles "
                "uniformly across the swap pane; override via "
                "cairns.viewport.setLayout.",
        [engine = &engine](const json& args) -> json {
            const int counter = cairns::headless::OpenViewport(engine);
            const uint32_t w = args.value("w", cairns::headless::GetFinalTargetWidth(engine));
            const uint32_t h = args.value("h", cairns::headless::GetFinalTargetHeight(engine));
            if (counter < 0) {
                return {{"error", "capped"},
                        {"viewport", nullptr},
                        {"w", w},
                        {"h", h}};
            }
            char name[16];
            std::snprintf(name, sizeof(name), "vp%d", counter);
            return {{"viewport", std::string(name)},
                    {"w", w},
                    {"h", h},
                    {"active_count", cairns::headless::ActiveViewportCount(engine)}};
        });

    registry.Register(
        "cairns.viewport.close",
        /*schema=*/json::object(),
        /*doc=*/"Close the highest-index viewport. Returns {ok:false} if "
                "only one viewport is live (cannot drop below 1).",
        [engine = &engine](const json&) -> json {
            const bool ok = cairns::headless::CloseViewport(engine);
            return {{"ok", ok},
                    {"active_count", cairns::headless::ActiveViewportCount(engine)}};
        });

    registry.Register(
        "cairns.viewport.setLayout",
        /*schema=*/json::object(),
        /*doc=*/"Set a viewport's NDC tile on the swap pane. args = "
                "{viewport: 'vpN', x, y, w, h} in [0..1]. The viewport "
                "field is the engine-assigned monotonic name returned "
                "from cairns.viewport.open. Default for a freshly-opened "
                "viewport is a uniform horizontal tile.",
        [engine = &engine](const json& args) -> json {
            // viewport name: prefer string "vpN"; fall back to integer
            // for back-compat with older clients.
            uint32_t counter = 0;
            bool parsed = false;
            const auto& v = args["viewport"];
            if (v.is_string()) {
                const std::string& s = v.get_ref<const std::string&>();
                if (s.size() > 2 && s[0] == 'v' && s[1] == 'p') {
                    counter = static_cast<uint32_t>(
                        std::strtoul(s.c_str() + 2, nullptr, 10));
                    parsed = true;
                }
            } else if (v.is_number_integer()) {
                counter = static_cast<uint32_t>(v.get<int64_t>());
                parsed = true;
            }
            const float x = static_cast<float>(args.value("x", 0.0));
            const float y = static_cast<float>(args.value("y", 0.0));
            const float w = static_cast<float>(args.value("w", 1.0));
            const float h = static_cast<float>(args.value("h", 1.0));
            const bool ok = parsed &&
                cairns::headless::SetViewportLayoutByName(engine, counter,
                                                           x, y, w, h);
            char name[16];
            std::snprintf(name, sizeof(name), "vp%u", counter);
            return {{"ok", ok}, {"viewport", std::string(name)},
                    {"x", x}, {"y", y}, {"w", w}, {"h", h}};
        });
    registry.RegisterAlias("viewport.close", "cairns.viewport.close");
    registry.RegisterAlias("viewport.setLayout", "cairns.viewport.setLayout");

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

    // #269: real spawn op. Inserts one hero entity in active_scene_
    // from a pre-loaded scene (scene_idx in [0, NumPrefabs())). The
    // auto-spawn grid in Engine::GreaterInit retires once this path
    // replaces the env-driven entity count.
    registry.Register(
        "cairns.world.spawnHero",
        json::object(),
        "Spawn one hero entity in active world from a pre-loaded scene. "
        "Args: scene_idx (uint), x/y/z (float, world position), "
        "scale (float), time_phase (float, anim time offset). "
        "Returns: entity (entt id) or 0 on bad scene_idx.",
        [&engine](const json& args) -> json {
            const uint32_t scene_idx = args.value("scene_idx", uint32_t{0});
            const float x = args.value("x", 0.0f);
            const float y = args.value("y", 0.0f);
            const float z = args.value("z", -3.0f);
            const float scale = args.value("scale", 0.005f);
            const float time_phase = args.value("time_phase", 0.0f);
            const uint32_t eid = cairns::headless::InstantiatePrefab(
                &engine, scene_idx, x, y, z, scale, time_phase);
            return {{"entity", eid}, {"scene_idx", scene_idx}};
        });

    registry.Register(
        "cairns.world.numScenes",
        json::object(),
        "Number of pre-loaded scenes (GLBs). Spawn args clamp to [0, N).",
        [&engine](const json&) -> json {
            return {{"count", cairns::headless::NumPrefabs(&engine)}};
        });

    // #269: scene-extent query. Returned in WORLD units (the scale a
    // InstantiatePrefab arg of 1.0 produces). Callers divide a target on-screen
    // cell size by this to get per-actor scale, normalizing the visual
    // size of a heterogeneous GLB set.
    registry.Register(
        "cairns.world.sceneDims",
        json::object(),
        "Bind-pose extent (max axis component, world units) of the "
        "scene's first skinned mesh. Args: {scene_idx}. Returns: "
        "{extent_max} or {extent_max:0} when unavailable.",
        [&engine](const json& args) -> json {
            const uint32_t scene_idx = args.value("scene_idx", uint32_t{0});
            return {{"extent_max",
                     cairns::headless::PrefabExtentMax(&engine,
                                                           scene_idx)},
                    {"scene_idx", scene_idx}};
        });

    // #269: enumerate active_scene_ entities. Used by no-flash relayout
    // -- caller queries the list, reposts setTransform for each, then
    // appends N-existing via spawnHero.
    registry.Register(
        "cairns.world.listEntities",
        json::object(),
        "List every entity in active_scene_. Returns "
        "{entities:[uint32 ids], count:N}.",
        [&engine](const json&) -> json {
            std::vector<uint32_t> ents =
                cairns::headless::ListActiveWorldEntities(&engine);
            json arr = json::array();
            for (uint32_t e : ents) {
                arr.push_back(e);
            }
            return {{"entities", std::move(arr)},
                    {"count", static_cast<uint32_t>(ents.size())}};
        });

    // #269: mutate one entity's WorldTransform. Pairs with listEntities
    // for the no-flash relayout. translation + uniform scale only --
    // matches the spawnHero argument shape.
    registry.Register(
        "cairns.world.setTransform",
        json::object(),
        "Overwrite an entity's WorldTransform. Args: "
        "{entity, x, y, z, scale}. Returns: {ok}.",
        [&engine](const json& args) -> json {
            const uint32_t e = args.value("entity", uint32_t{0});
            const float x = args.value("x", 0.0f);
            const float y = args.value("y", 0.0f);
            const float z = args.value("z", -3.0f);
            const float scale = args.value("scale", 1.0f);
            return {{"ok", cairns::headless::SetEntityTransform(
                              &engine, e, x, y, z, scale)},
                    {"entity", e}};
        });
}

}  // namespace cairns::control
