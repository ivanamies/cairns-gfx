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
std::atomic<uint64_t> g_scene_counter{0};
std::atomic<uint64_t> g_asset_counter{0};
std::atomic<uint64_t> g_entity_counter{0};

}  // namespace

void RegisterSceneOps(CommandRegistry& registry, cairns::Engine& engine) {
    // ──────────────────────────────────────────────────────────────────
    // #225 R3: Unity-shaped op surface. cairns.scene.* = Scene (the
    // container, Unity sense); cairns.prefab.* = Prefab (the loaded GLB,
    // Unity Instantiate target). Old cairns.world.* + cairns.asset.load
    // names register as deprecated aliases (one release; delete after).
    // ──────────────────────────────────────────────────────────────────

    registry.Register(
        "cairns.scene.create",
        json::object(),
        "Allocate a new Scene (Unity Scene == cairns container of "
        "GameObjects). Returns {scene: synthetic id}. Stub until the "
        "scene-per-viewport extract path lands.",
        [](const json&) -> json {
            return {{"scene", g_scene_counter.fetch_add(1)}};
        });

    registry.Register(
        "cairns.scene.clear",
        json::object(),
        "Nuke every entity in active_scene_. Returns {cleared:N}. "
        "Leaks any held skin_output_pool_ slices + alias buffer handles "
        "(no skin Release path yet); fine for occasional debug-session "
        "resets, do not loop.",
        [&engine](const json&) -> json {
            const uint32_t n =
                cairns::headless::ClearActiveScene(&engine);
            return {{"cleared", n}};
        });

    registry.Register(
        "cairns.scene.instantiate",
        json::object(),
        "Instantiate a Prefab into the active Scene at a transform. "
        "Args: {prefab (uint), x/y/z (float, world position), scale "
        "(float), time_phase (float, anim time offset)}. Legacy alias "
        "key 'scene_idx' is also accepted. "
        "Returns: {entity, prefab}.",
        [&engine](const json& args) -> json {
            uint32_t prefab = args.value("prefab", uint32_t{0});
            if (args.contains("scene_idx")) {
                prefab = args.value("scene_idx", uint32_t{0});
            }
            const float x = args.value("x", 0.0f);
            const float y = args.value("y", 0.0f);
            const float z = args.value("z", -3.0f);
            const float scale = args.value("scale", 0.005f);
            const float time_phase = args.value("time_phase", 0.0f);
            const uint32_t eid = cairns::headless::InstantiatePrefab(
                &engine, prefab, x, y, z, scale, time_phase);
            return {{"entity", eid}, {"prefab", prefab}};
        });

    registry.Register(
        "cairns.scene.instantiateGrid",
        json::object(),
        "Instantiate N Prefabs in a grid. Stub today; engine doesn't "
        "yet honor placement. Args: {count}. Returns: {first, last, "
        "count}.",
        [](const json& args) -> json {
            const uint64_t n = args.value("count", uint64_t{0});
            const uint64_t start = g_entity_counter.fetch_add(n);
            return {{"first", start},
                    {"last", start + (n > 0 ? n - 1 : 0)},
                    {"count", n}};
        });

    registry.Register(
        "cairns.scene.listEntities",
        json::object(),
        "List every entity in active_scene_. Returns {entities:[uint32 "
        "ids], count:N}.",
        [&engine](const json&) -> json {
            std::vector<uint32_t> ents =
                cairns::headless::ListActiveSceneEntities(&engine);
            json arr = json::array();
            for (uint32_t e : ents) {
                arr.push_back(e);
            }
            return {{"entities", std::move(arr)},
                    {"count", static_cast<uint32_t>(ents.size())}};
        });

    registry.Register(
        "cairns.scene.setTransform",
        json::object(),
        "Overwrite an entity's WorldTransform. Pairs with listEntities "
        "for no-flash relayout. Args: {entity, x, y, z, scale}. "
        "Returns: {ok, entity}.",
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

    registry.Register(
        "cairns.prefab.load",
        json::object(),
        "Load a GLB/texture/audio file as a Prefab. Today: stub returns "
        "a synthetic asset id (no GPU upload); real load lands with the "
        "AssetRegistry hookup. Args: {path}.",
        [](const json& args) -> json {
            const std::string path = args.value("path", std::string{});
            return {{"prefab", g_asset_counter.fetch_add(1)},
                    {"path", path}};
        });

    registry.Register(
        "cairns.prefab.count",
        json::object(),
        "Number of pre-loaded Prefabs (GLBs). Instantiate args clamp to "
        "[0, count).",
        [&engine](const json&) -> json {
            return {{"count", cairns::headless::NumPrefabs(&engine)}};
        });

    registry.Register(
        "cairns.prefab.dims",
        json::object(),
        "Bind-pose extent (max axis component, world units) of the "
        "Prefab's first skinned mesh. Args: {prefab} (legacy alias: "
        "{scene_idx}). Returns: {extent_max, prefab}.",
        [&engine](const json& args) -> json {
            uint32_t prefab = args.value("prefab", uint32_t{0});
            if (args.contains("scene_idx")) {
                prefab = args.value("scene_idx", uint32_t{0});
            }
            return {{"extent_max",
                     cairns::headless::PrefabExtentMax(&engine, prefab)},
                    {"prefab", prefab}};
        });

    // ── #224 L3: the instrument (`cairns.loader.*`) ──
    registry.Register(
        "cairns.loader.trace",
        json::object(),
        "Last LoadPrefabBatch trace: per-stage timings + total + counts. "
        "Returns: {total_ms, prefabs_added, meshes_added, ms:{stage:ms,…}, "
        "stages:[{name, ms, bytes, count}]}.",
        [&engine](const json&) -> json {
            const cairns::LoadTrace t =
                cairns::headless::LastLoadTrace(&engine);
            json stages = json::array();
            json ms = json::object();
            for (uint8_t i = 0; i < t.stage_count; ++i) {
                stages.push_back({{"name", t.stages[i].name},
                                  {"ms",   t.stages[i].ms},
                                  {"bytes",t.stages[i].bytes},
                                  {"count",t.stages[i].count}});
                ms[t.stages[i].name] = t.stages[i].ms;
            }
            return {{"total_ms",            t.total_ms},
                    {"bytes_uploaded",      t.bytes_uploaded},
                    {"prefabs_added",       t.prefabs_added},
                    {"meshes_added",        t.meshes_added},
                    {"actors_instantiated", t.actors_instantiated},
                    {"stages",              std::move(stages)},
                    {"ms",                  std::move(ms)}};
        });

    registry.Register(
        "cairns.loader.counters",
        json::object(),
        "Live LoaderCounters: residency + batch stats. Returns: "
        "{prefabs_resident, meshes_resident, textures_resident, "
        "actors_live, bytes_resident, bytes_uploaded_total, "
        "batches_loaded, last_batch_ms, peak_batch_ms}.",
        [&engine](const json&) -> json {
            const cairns::LoaderCounters c =
                cairns::headless::Counters(&engine);
            return {{"bytes_resident",       c.bytes_resident},
                    {"prefabs_resident",     c.prefabs_resident},
                    {"meshes_resident",      c.meshes_resident},
                    {"textures_resident",    c.textures_resident},
                    {"actors_live",          c.actors_live},
                    {"bytes_uploaded_total", c.bytes_uploaded_total},
                    {"batches_loaded",       c.batches_loaded},
                    {"last_batch_ms",        c.last_batch_ms},
                    {"peak_batch_ms",        c.peak_batch_ms}};
        });

    // ── viewport ops ──
    registry.Register(
        "cairns.viewport.open",
        json::object(),
        "Open a new viewport on the swap pane (#194). Caps at the "
        "engine's kNumViewports. Returns {viewport: 'vpN'} on success "
        "(engine-assigned monotonic name; never reused) or "
        "{error:'capped'} when full.",
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
        json::object(),
        "Close the highest-index viewport. Returns {ok:false} if only "
        "one viewport is live (cannot drop below 1).",
        [engine = &engine](const json&) -> json {
            const bool ok = cairns::headless::CloseViewport(engine);
            return {{"ok", ok},
                    {"active_count", cairns::headless::ActiveViewportCount(engine)}};
        });

    registry.Register(
        "cairns.viewport.setLayout",
        json::object(),
        "Set a viewport's NDC tile on the swap pane. args = "
        "{viewport: 'vpN', x, y, w, h} in [0..1].",
        [engine = &engine](const json& args) -> json {
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

    registry.Register(
        "cairns.viewport.setCamera",
        json::object(),
        "Set a viewport's camera pose. Records intent today; actual "
        "camera plumbing lands with the resizing-and-cameras plan.",
        [](const json& args) -> json {
            return {{"viewport", args.value("viewport", uint64_t{0})}};
        });

    registry.Register(
        "cairns.viewport.setScene",
        json::object(),
        "Bind a viewport to a Scene (the composition surface: multiple "
        "viewports each bound to a different Scene composite into one "
        "image). Stub today; engine bind plumbing lands with #226.",
        [](const json& args) -> json {
            return {{"viewport", args.value("viewport", uint64_t{0})},
                    {"scene", args.value("scene", uint64_t{0})}};
        });

    registry.Register(
        "cairns.window.resize",
        json::object(),
        "Reallocate final_target_ at the new dimensions. Real work in "
        "headless mode; future windowed-mode wiring routes through "
        "Engine::requestResizeFrameBuffer.",
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

    // ──────────────────────────────────────────────────────────────────
    // Deprecated aliases (one release, then delete).
    //
    // Per #225 R3: legacy cairns.world.* (a misnomer -- the container is
    // Unity's Scene) + cairns.asset.load (Prefab is the right noun for a
    // loaded GLB) + cairns.viewport.setWorld + the studio.* drops still
    // dispatch to the new ops above. Existing JS/NDJSON callers keep
    // working until they migrate.
    // ──────────────────────────────────────────────────────────────────
    registry.RegisterAlias("cairns.world.create",        "cairns.scene.create");
    registry.RegisterAlias("cairns.world.clear",         "cairns.scene.clear");
    registry.RegisterAlias("cairns.world.instantiate",   "cairns.scene.instantiate");
    registry.RegisterAlias("cairns.world.instantiateGrid","cairns.scene.instantiateGrid");
    registry.RegisterAlias("cairns.world.spawnHero",     "cairns.scene.instantiate");
    registry.RegisterAlias("cairns.world.listEntities",  "cairns.scene.listEntities");
    registry.RegisterAlias("cairns.world.setTransform",  "cairns.scene.setTransform");
    registry.RegisterAlias("cairns.world.numScenes",     "cairns.prefab.count");
    registry.RegisterAlias("cairns.world.sceneDims",     "cairns.prefab.dims");
    registry.RegisterAlias("cairns.asset.load",          "cairns.prefab.load");
    registry.RegisterAlias("cairns.viewport.setWorld",   "cairns.viewport.setScene");

    // Pre-#225 top-level names (also deprecated).
    registry.RegisterAlias("world.create",         "cairns.scene.create");
    registry.RegisterAlias("world.clear",          "cairns.scene.clear");
    registry.RegisterAlias("world.instantiate",    "cairns.scene.instantiate");
    registry.RegisterAlias("world.instantiateGrid","cairns.scene.instantiateGrid");
    registry.RegisterAlias("asset.load",           "cairns.prefab.load");
    registry.RegisterAlias("viewport.open",        "cairns.viewport.open");
    registry.RegisterAlias("viewport.close",       "cairns.viewport.close");
    registry.RegisterAlias("viewport.setLayout",   "cairns.viewport.setLayout");
    registry.RegisterAlias("viewport.setCamera",   "cairns.viewport.setCamera");
    registry.RegisterAlias("viewport.setWorld",    "cairns.viewport.setScene");
    registry.RegisterAlias("window.resize",        "cairns.window.resize");
}

}  // namespace cairns::control
