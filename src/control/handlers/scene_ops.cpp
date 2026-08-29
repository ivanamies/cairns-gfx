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
        "cairns.pipeline.reload",
        json::object({{"name", "<anim_eval|skin|particle>"}}),
        "#228 R2: hot-reload a compute kernel by logical name. "
        "KEEP-LAST-GOOD: on failure the existing pipeline stays bound "
        "and rendering continues. Returns {ok, name}.",
        [&engine](const json& args) -> json {
            std::string name = args.value("name", std::string());
            const bool ok =
                cairns::headless::ReloadPipelineByName(&engine, name);
            return {{"ok", ok}, {"name", name}};
        });

    registry.Register(
        "cairns.prefab.reload",
        json::object({{"path", "<glb path>"}}),
        "#228 R1: reload a Prefab in place behind its stable PrefabId. "
        "Re-parses the GLB at args.path, swaps the pool slot's contents, "
        "and DeferFrees the previous GPU resources through the F1 (v2) "
        "per-resource retire-frame ring. Entities holding AssetRef "
        "remain valid and render the new mesh next frame. Returns "
        "{ok, path}.",
        [&engine](const json& args) -> json {
            std::string path = args.value("path", std::string());
            const bool ok =
                cairns::headless::ReloadPrefabByPath(&engine, path);
            return {{"ok", ok}, {"path", path}};
        });

    registry.Register(
        "cairns.prefab.unloadAll",
        json::object(),
        "#228 F2: drop every resident prefab. DeferFrees each prefab's "
        "textures/samplers/meshes/materials through the F1 ring (released "
        "kFIF frames later, no GPU drain). ClearActiveScene runs first "
        "so post-Unload the active scene is empty. Returns {unloaded:N}. "
        "Engine is fully ready for fresh loads immediately after.",
        [&engine](const json&) -> json {
            const uint32_t n =
                cairns::headless::UnloadAllPrefabs(&engine);
            return {{"unloaded", n}};
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
        "#224 L5: instantiate `prefab_count` resident prefabs starting "
        "at `first_prefab_idx` into the active scene; slide all existing "
        "actors to the new fitted grid (no flash). Args: "
        "{first_prefab_idx, prefab_count}. Returns: {entities:[ids], count}.",
        [&engine](const json& args) -> json {
            const uint32_t first =
                args.value("first_prefab_idx", uint32_t{0});
            const uint32_t cnt =
                args.value("prefab_count", uint32_t{0});
            std::vector<uint32_t> ents =
                cairns::headless::InstantiateGridFitted(&engine, first, cnt);
            json arr = json::array();
            for (uint32_t e : ents) {
                arr.push_back(e);
            }
            return {{"entities", std::move(arr)},
                    {"count",    static_cast<uint32_t>(ents.size())}};
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
        "#224 L9: load ONE GLB at `path` as a Prefab. Path is absolute "
        "or a short name resolved via the engine's static resource "
        "lookup. Returns: {prefab, path, ok}. The JS catalog script "
        "loops + calls this per file -- the engine does NOT take a "
        "list, by design (the loop belongs in the script).",
        [&engine](const json& args) -> json {
            const std::string path = args.value("path", std::string{});
            const uint32_t prefab_idx =
                cairns::headless::RuntimeLoadGlbPath(&engine, path);
            const bool ok = prefab_idx != UINT32_MAX;
            return {{"prefab", ok ? prefab_idx : 0u},
                    {"path",   path},
                    {"ok",     ok}};
        });

    registry.Register(
        "cairns.prefab.loadBatch",
        json::object(),
        "#224 L5: parse + upload `count` GLBs from the kDebugGlbs "
        "window starting at `cursor`. Runs Device::WaitIdle first; "
        "re-uploads anim tables after. Args: {cursor, count, source}. "
        "Returns: {first_prefab_idx, count, prefabs:[{id,name,extent}]}.",
        [&engine](const json& args) -> json {
            const uint32_t cursor = args.value("cursor", uint32_t{0});
            const uint32_t count  = args.value("count",  uint32_t{0});
            cairns::headless::LoadBatchExport r =
                cairns::headless::RuntimeLoadGlbs(&engine, cursor, count);
            json prefabs = json::array();
            for (uint32_t i = 0; i < r.count; ++i) {
                const uint32_t idx = r.first_prefab_idx + i;
                prefabs.push_back({
                    {"id",      idx},
                    {"name",    std::string("prefab") + std::to_string(idx)},
                    {"extent",  cairns::headless::PrefabExtentMax(
                                    &engine, idx)}});
            }
            return {{"first_prefab_idx", r.first_prefab_idx},
                    {"count",            r.count},
                    {"prefabs",          std::move(prefabs)}};
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

    // ── #224 L8: editor-chrome (selection outline) toggle ──
    registry.Register(
        "cairns.editor.chrome",
        json::object(),
        "Toggle editor chrome (the selection outline pass). "
        "Args: {on:bool}. When off, the selection STATE is preserved "
        "(highlights_ + revision counter) but the outline is NOT drawn. "
        "Stylized highlight (materials-era; rim/ink/toon) is in-canvas "
        "art and unaffected by this flag. Default: on.",
        [&engine](const json& args) -> json {
            const bool on = args.value("on", true);
            cairns::headless::SetEditorChromeEnabled(&engine, on);
            return {{"on", cairns::headless::EditorChromeEnabled(&engine)}};
        });

    // ── #224 L6: APPEND-only debug pair ──
    registry.Register(
        "cairns.debug.snapshotPrefabHandles",
        json::object(),
        "Snapshot every live Mesh::Hot's handles + batch_id. Stashed "
        "on Engine; cairns.debug.assertAppendOnly compares against it. "
        "Returns: {snapshot_size}.",
        [&engine](const json&) -> json {
            const uint32_t n =
                cairns::headless::DebugSnapshotPrefabHandles(&engine);
            return {{"snapshot_size", n}};
        });
    registry.Register(
        "cairns.debug.checkInvariants",
        json::object(),
        "#228 H2: walk the LoadPrefabBatch manifest's contract. Returns "
        "{violations:N, messages:[…]}. 0 == every manifest line agrees "
        "with its invariant (per_prefab_asset_.size() == prefab_ids_.size(), "
        "every live Material has set2, resident_textures_ == sum of all "
        "prefab textureHandles, etc.). Adding engine state without its "
        "matching invariant fails this check the first frame after a load.",
        [&engine](const json&) -> json {
            cairns::headless::InvariantsExport e =
                cairns::headless::DebugCheckInvariants(&engine);
            json arr = json::array();
            for (const std::string& m : e.messages) {
                arr.push_back(m);
            }
            return {{"violations", e.violations},
                    {"messages",   std::move(arr)}};
        });

    registry.Register(
        "cairns.debug.assertAppendOnly",
        json::object(),
        "Compare current Mesh::Hot handles against the prior snapshot. "
        "0 mismatches == APPEND-only contract held. "
        "Returns: {mismatches}.",
        [&engine](const json&) -> json {
            const uint32_t m =
                cairns::headless::DebugAssertAppendOnly(&engine);
            return {{"mismatches", m}};
        });
    registry.Register(
        "cairns.debug.loadTwice",
        json::object(),
        "#224 L7: load the same {cursor,count} GLB window twice, fit "
        "transforms each time, count mismatches (modulo trace timing). "
        "0 mismatches == deterministic. Returns: {mismatches, cursor, count}.",
        [&engine](const json& args) -> json {
            const uint32_t cursor = args.value("cursor", uint32_t{0});
            const uint32_t count  = args.value("count",  uint32_t{0});
            const uint32_t m = cairns::headless::DebugDeterminismCheck(
                &engine, cursor, count);
            return {{"mismatches", m},
                    {"cursor",     cursor},
                    {"count",      count}};
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
        "cairns.prefab.validate",
        json::object(),
        "#224 L2: return the validation report from the LAST "
        "LoadPrefabBatch call (issues + ok flag). No args. Returns: "
        "{ok, issue_count, issues:[{sev, what, prefab_idx}]}.",
        [&engine](const json&) -> json {
            const cairns::ValidationReport rep =
                cairns::headless::LastValidationReport(&engine);
            json arr = json::array();
            for (uint8_t i = 0; i < rep.issue_count; ++i) {
                arr.push_back({
                    {"sev",
                     rep.issues[i].sev == cairns::ValidationSeverity::kError
                         ? "error" : "warning"},
                    {"what", rep.issues[i].what},
                    {"prefab_idx", rep.issues[i].prefab_idx}});
            }
            return {{"ok", rep.ok},
                    {"issue_count", static_cast<uint32_t>(rep.issue_count)},
                    {"issues", std::move(arr)}};
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
        "Set viewport `viewport` (index) camera pose {x,y,z,yaw,pitch}.",
        [engine = &engine](const json& args) -> json {
            const int vp = static_cast<int>(args.value("viewport", 0));
            const float x = args.value("x", 0.0f);
            const float y = args.value("y", 0.0f);
            const float z = args.value("z", 0.0f);
            const float yaw = args.value("yaw", 0.0f);
            const float pitch = args.value("pitch", 0.0f);
            return {{"ok", cairns::headless::SetViewportCamera(engine, vp, x, y,
                                                              z, yaw, pitch)}};
        });

    registry.Register(
        "cairns.viewport.setScene",
        json::object(),
        "Bind viewport `viewport` (index) to scene `scene` (0 primary, "
        "1 secondary). Each viewport renders only its bound scene (#195).",
        [engine = &engine](const json& args) -> json {
            const int vp = static_cast<int>(args.value("viewport", 0));
            const uint32_t scene = args.value("scene", 0u);
            return {{"ok", cairns::headless::SetViewportScene(engine, vp,
                                                             scene)}};
        });

    registry.Register(
        "cairns.viewport.particles",
        json::object(),
        "Toggle per-viewport particle rendering: {viewport (index), on}.",
        [engine = &engine](const json& args) -> json {
            const int vp = static_cast<int>(args.value("viewport", 0));
            const bool on = args.value("on", true);
            return {{"ok",
                     cairns::headless::SetViewportParticles(engine, vp, on)}};
        });

    registry.Register(
        "cairns.scene.use",
        json::object(),
        "Retarget the active scene for subsequent spawns: {index} "
        "(0 primary, 1 secondary).",
        [engine = &engine](const json& args) -> json {
            cairns::headless::UseScene(engine, args.value("index", 0u));
            return json::object();
        });

    registry.Register(
        "cairns.scene.spawnFitted",
        json::object(),
        "Load + fit-to-frame + center `instances` actors cycling over `glbs`, "
        "spawned into the active scene. {glbs:[name...], instances, animated}.",
        [engine = &engine](const json& args) -> json {
            std::vector<std::string> glbs;
            if (args.contains("glbs") && args["glbs"].is_array()) {
                for (const auto& g : args["glbs"]) {
                    glbs.push_back(g.get<std::string>());
                }
            }
            const uint32_t instances =
                args.value("instances", static_cast<uint32_t>(glbs.size()));
            const bool animated = args.value("animated", false);
            if (!cairns::headless::SpawnFitted(engine, glbs, instances,
                                               animated)) {
                throw std::runtime_error("SpawnFitted failed");
            }
            return {{"instances", instances}};
        });

    registry.Register(
        "cairns.render.advanceFrames",
        json::object(),
        "Render `count` headless frames forward (fixed clock).",
        [engine = &engine](const json& args) -> json {
            const uint32_t count = args.value("count", 1u);
            if (!cairns::headless::AdvanceFrames(engine, count)) {
                throw std::runtime_error("AdvanceFrames failed");
            }
            return {{"count", count}};
        });

    registry.Register(
        "cairns.particles.enable",
        json::object(),
        "Enable/disable the particle compute+draw globally: {on}.",
        [engine = &engine](const json& args) -> json {
            cairns::headless::EnableParticles(engine, args.value("on", true));
            return json::object();
        });

    registry.Register(
        "cairns.render.tinyTriangle",
        json::object(),
        "Draw a single red NDC triangle (no scene): pipeline + clear + one "
        "draw. {on}.",
        [engine = &engine](const json& args) -> json {
            cairns::headless::SetTinyTriangle(engine, args.value("on", true));
            return json::object();
        });

    registry.Register(
        "cairns.render.nestedGraph",
        json::object(),
        "Compose color + resolved-depth + extra-camera passes: {on}. Open and "
        "aim the extra-camera viewports separately (viewport.open/setCamera).",
        [engine = &engine](const json& args) -> json {
            cairns::headless::SetNestedGraphMode(engine, args.value("on", true));
            return json::object();
        });

    registry.Register(
        "cairns.imgui.golden",
        json::object(),
        "Draw the imgui overlay in golden/headless mode: {on}. Pair with "
        "cairns.hud.set for a byte-stable overlay.",
        [engine = &engine](const json& args) -> json {
            cairns::headless::SetImguiInGolden(engine, args.value("on", true));
            return json::object();
        });

    registry.Register(
        "cairns.hud.set",
        json::object(),
        "Inject fixed HUD numbers so the overlay is byte-stable: {cpu_ms, fps}.",
        [engine = &engine](const json& args) -> json {
            cairns::headless::SetInjectedHud(engine,
                                             args.value("cpu_ms", 16.6f),
                                             args.value("fps", 60.0f));
            return json::object();
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
