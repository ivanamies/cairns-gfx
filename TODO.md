# cairns-gfx TODO

Single source of truth for outstanding work. README.md should NOT carry
todos — anything aspirational, deferred, or planned belongs here.
Historical and architectural prose stays in README; future work lives
here.

---

## Active

### #224 Loading system (P0) — COMPLETE (2026-06-13)
Plan: `/Users/ivanamies/dev/plans/2026-06-13_gfx_loading-system-224.md`.
Byte-gate green every commit; studio_js smoke 9/9 throughout.
- [x] **L0** ListActiveWorld / ClearActiveWorld → …ActiveScene rename
- [x] **L1** `Engine::LoadPrefabBatch(span<path>)` + per-batch shared skin
              (`per_batch_shared_skin_` list + `Mesh::Hot::batch_id`)
- [x] **L2** `Engine::ValidatePrefab` + `…MeshWeights` + `cairns.prefab.validate`
- [x] **L3** `LoadTrace` + `LoaderCounters` PODs +
              `cairns.loader.{trace,counters}` (boot batch = ~40s parse +
              1s upload — instrument quantifies the load slowness)
- [x] **L4** `Engine::FitGridToViewport(N, extents)` — pure-fn camera solve
- [x] **L5** `cairns.prefab.loadBatch` + real `cairns.scene.instantiateGrid`
              (the "+50 +50 +50 auto-fit no-flash" deliverable)
- [x] **L6** APPEND-only acceptance test
              (`cairns.debug.snapshotPrefabHandles` /`assertAppendOnly`)
- [x] **L7** `FitGridToViewport` determinism check (`cairns.debug.loadTwice`)
- [x] **L8** Editor chrome (selection outline) toggle (`cairns.editor.chrome`)

Follow-ups queued (above): two-sort screenshot strategy
(full editor vs per-viewport canvas) + chrome-leak byte-gate.

### #225 Unity-shaped rename pass — COMPLETE (2026-06-13)
Plan: `/Users/ivanamies/dev/plans/2026-06-13_gfx_unity-rename-225.md`.
Byte-gate green every commit; studio_js smoke harness covers the new
surface (9/9 on both backends).
- [x] **R-1** TODO.md extraction + WebGPU ladder
- [x] **R0** C++ `Scene` (GLB) → `Prefab` (~150 sites, 10 files)
- [x] **R1** C++ `World` → `Scene` (~100 sites, 12 files)
- [x] **R2** `LoadedMaterial`→`Material`, `SpawnHero`→`InstantiatePrefab`,
              `NumScenes`→`NumPrefabs`
- [x] **R3** NDJSON ops `cairns.world.*`→`cairns.scene.*` +
              `cairns.asset.load`→`cairns.prefab.load` +
              `cairns.viewport.setWorld`→`setScene`,
              all legacy names register as RegisterAlias (one release)
- [x] **R4** `studio_js.hpp` rewrite — `Prefab` / `Scene.instantiate` /
              `Prefabs.{load,loadOne,list}` / `Editor.{scenes,newScene,
              show,thumbnails,compose}`; `GameObject.scene` (new) +
              `.world` deprecated alias
- [x] **R5** `docs/studio_notes.md` rewritten: clone / new noun / refuse
              bins (no "divergence ledger"); `scripts/_studio_js_smoke.js`
              + `scripts/verify_studio_js.sh` (9 assertions)
- [x] **R6** `#226` stubs throw "not wired yet (#226 CAP-N)"; comment
              audit (multi-world → multi-scene, etc.)

Below the wall: **#226** (the three engine capabilities behind the
worked example) lives in its own plan — multi-camera per viewport,
virtualized RT pool for `Editor.thumbnails`, multi-target composition
pass for `Editor.compose`. Each is real rendering plumbing that moves
pixels, needs its own goldens, and must NOT share commits with the
rename.

### Golden capture is CPU-side readback at a fixed frame — never a screenshot
Capture is `ReadFinalTargetRgba` / `DumpFinalTarget` off the offscreen target,
taken after a fixed frame interval (deterministic frame N) — NOT an OS
`screencapture` / window grab. WHY: a screenshot is non-deterministic
(compositor timing, DPI, window chrome, async present) and can't be
byte-gated; the fixed interval pins sim/particle state to the same frame every
run. (The 2026-06-19 G1 particle flake is the symptom of getting the *timing*
wrong even on the correct CPU-side path — see modularization-notes #9b.)

### Delete all singletons (unless dragged in by a 3rd-party dependency)
Every process singleton / Meyer's `static X& Instance()` / file-scope mutable
`static` must be deleted — construct the thing and pass it explicitly. The ONLY
allowed exception is a singleton forced on us by a 3rd-party dependency. Known
offenders:
- `CommandRegistry::Instance()` (src/control/command_registry.{hpp,cpp}) — the
  whole dispatch surface routes through one global. Blocks per-test registries
  (a fresh registry per SCENARIO bound to that SCENARIO's Engine). Make it
  constructible, pass `CommandRegistry&` everywhere (the `Register*Ops` already
  take it by ref). modularization-notes #12.
- `static JsState s` (src/control/handlers/script_ops.cpp:50) — one QuickJS
  runtime/context for the whole process, bound to whatever registry it first
  saw. Must become per-caller state owned alongside the registry it serves.
- `g_scene_counter` (src/control/handlers/scene_ops.cpp:21) — file-scope atomic;
  scene ids should come from the scenes_ pool, not a global counter.
- `cairns::Timer` static accumulators (accum_times_/accum_itrs_/slot_names_) —
  shared across Engine instances; perf.last + the imgui overlay read them. A
  source of cross-Engine state bleed (modularization-notes #9b territory).
- Audit for more: grep `Instance()`, `static .*&`, file-scope `static` mutable.

### Over-specialized test seams -> JS snippets / primitives
`Engine::SetupTwoSceneViewports(left, right, particles)`, `Engine::SpawnHeroFramed
(glb, animated)` (src/engine.hpp), and `test_seams::AdvanceToGoldenFrame` are
bespoke compositions baked in to drive ONE golden each. That is backwards for an
AI-first/headless engine: the engine should expose PRIMITIVE ops — load-glb,
fit+center, instantiate-into-scene, open-viewport, bind-viewport-scene, set-
viewport-particles, advance-N-frames — and a setup like "two scenes, a different
hero in each" should be a few lines of run.js / a studio_js snippet via
`cairns.dispatch`, not an engine method. `AdvanceToGoldenFrame` just hardcodes
`kGoldenDumpFrame+1` over the real primitive `AdvanceFrames(N)` (see
modularization-notes #11) — the test should pick N. Same root as
modularization-notes #9 (the open-viewport sequence repeats because the test
harness bypasses JS dispatch). Needs: a scene-targeted instantiate op (today
`InstantiatePrefab` hardcodes active_scene_, so the seam swaps active_scene_
around the spawn — a smell) + a `viewport.setScene` op. Delete the seams once
those primitives exist and the tests drive JS.

### Two-sort screenshot + test strategy (chrome on / chrome off)
The editor canvas has two pixel surfaces and the test matrix must
cover BOTH:
1. **Full editor screenshot** — what the user actually sees: scene +
   editor chrome (selection outlines, gizmos, dock UI, status overlays).
   Tests the editor experience end-to-end. Screenshot via the editor's
   final composite + chrome layer.
2. **Per-viewport canvas screenshot** — just the rendered scene content
   inside one viewport pane: no selection outlines, no editor cruft.
   Tests the in-canvas art (materials, stylized highlight) in isolation.
   This is what the remixer captures + scrolls + composites; it MUST be
   chrome-free or selection cruft leaks into final art.

Implementation: `cairns.io.dumpTexture {target:"window"}` is the full
editor; need a `target:"viewport:N"` mode that grabs JUST the viewport's
color target with `cairns.editor.chrome({on:false})` applied scope-locally
(so a chrome-on session can capture a chrome-off frame without flipping
global state). Both screenshot kinds need byte-gate golden coverage so
a chrome-leak regression on either surface fails the dev build.
Locks the §6 separation from #224.

### ~~#270 Fix broken animations on Samsung S22 (vk release)~~ — DONE (2026-06-19)
Driven by `scripts/dev_drive.sh` post-#269 spawn flow. Spawn one prefab
at a time until `[SKIN-FAIL]` fires; the log already names the failing
scene_id + reason. Needs in-app driver for on-device iteration (no NDJSON
socket on Android).

### #268 Per-actor frustum cull (was S.3)
Bind-pose AABB capture at load is already on `Mesh::Hot`
(`bind_aabb_min/max`, used by [PICK]). Need: frustum test in
`BuildSkinFrame` vs live frusta, 1.5× extents pad. Skinning + anim_eval
cost becomes ∝ visible.

### #253 Phase 4 — half4 skin output (decide post P1/P3)

---

## WebGPU stand-up ladder

Order is forced — each rung lights up new infrastructure. Don't skip.

1. **render one triangle** — RHI device/queue/surface bring-up, single
   forward pass, hardcoded vbo, hardcoded shader. proves WGSL pipeline
   + swap chain plumbing.
2. **render one die.glb** — GLB loader path (fastgltf still), vertex /
   index buffer upload via WebGPU queue, basic unlit shader sampling a
   texture. proves upload + sampling.
3. **render viking_room.glb** — non-trivial mesh + albedo texture +
   real camera. proves depth buffer + UV mapping + perspective xform.
4. **render three League of Legends GLBs** — multi-prefab path (the
   #225 `Prefab` pool), per-draw uniforms (`DynamicBuffers` over
   WebGPU's `hasDynamicOffset`). proves the per-draw bind story —
   THIS is the cliff where the macOS/vk architecture pays off (Phase
   D's WebGPU-shaped dynamic-offset path).
5. **render 3300 LoL GLBs** — proxy extract + sort + draw at scale.
   proves the draw-key sort, the bump-allocated dynamic ring, and
   that WebGPU's queue.submit cost doesn't melt at this draw count.
6. **animate 9 LoL GLBs** — compute kernel port (`skin.comp.glsl` +
   `anim_eval.comp.glsl` → WGSL). proves compute dispatch, SSBO
   bindings, the LDS palette path (WebGPU calls it workgroup memory).
   THIS is where Tint/`SPIRV-Cross` either Just Works or doesn't —
   high-risk rung.
7. **animate 500 LoL GLBs** — instanced compute batching at scale.
   proves the perf story carries through to WebGPU. should sit at
   ~30 FPS on a desktop browser if rungs 1–6 are clean. matches the
   product target ("30 FPS for 500 GLBs on a phone").

Reaching rung 7 means cairns is a WebGPU-shipped engine; reaching it
on a phone over WebGPU-on-mobile-Chrome is the actual win.

---

## Deferred refactors (from README — moved here)

### Move render-graph build to the game thread, per-slot
Today the graph is built + baked + executed on the render thread inside
`RecordFrame` (`src/engine.hpp`), with a single shared `RenderGraph`
instance reused per slot via `BindSlotArena(slot, ...)`. The threading
diagram in README describes the intended end state; getting there means
splitting graph build/bake (game thread, per-slot graphs) from Execute
(render thread). Not blocking any active perf push.

### EnTT internals still on `std::allocator`
`entt::registry`'s component pages go through `std::allocator<T>`, not
`cairns::Allocator`. Routing through
`cairns::basic_registry<entt::entity, cairns::Allocator<entt::entity>>`
is mechanical surgery (every `view<T>` / `storage<T>` call site has to
carry the allocator type), but right now `cairns::Arena::kEnabled = false`
means the Arena just `malloc`s anyway — so the surgery would route
through a wrapper that calls `malloc` instead of routing through
`malloc` directly. Revisit when `kEnabled` flips to true and the
ChunkAllocator path is live.

### `World::Cold::registry` is held by value (now `Scene::Cold` post-R1)
Dropping the prior `std::unique_ptr<entt::registry>` saves one heap
alloc per scene open (~8 across the program). Safe because
`scenes_.Reserve(kMaxScenes)` guarantees the cold_ vector never
reallocates, so `Scene::Cold*` stays stable for the engine's lifetime.

---

## Editor extensions to add (from README)

Per the Editor stack table — currently picked but not yet vendored / wired:
- **ImGuizmo** (CedricGuillemet) — 3D move/rotate/scale gizmos, camera placement
- **ImPlot** (epezent) — loss curves, histograms, throughput plots
- **imgui_tex_inspect** (andyborrell) and/or **ImmVision** (pthom) —
  inspect images / latents / ControlNet maps

Optional polish: `imspinner`, `imgui_md`, `imgui_toggle`, `ImGuiNotify`,
`imgui-knobs`.

---

## #226 capability plan (lives below the #225 wall)

Three engine capabilities behind the §4b worked example from the #225
plan. NEW rendering plumbing — must NOT share commits with the rename.
Will get its own plan + goldens.

- **CAP-1** multi-camera per viewport (`{cameras:[…], split:"h"/"v"}`)
- **CAP-2** virtualized render-to-texture pool (`Editor.thumbnails`)
- **CAP-3** multi-target composition pass — a viewport whose pixels
  *sample* N other viewports' rendered targets, not just tile them.
  THIS is the multi-scene compositor — the New Noun Unity lacks.

---

## Shader inventory cleanup (drive-by)

Source shader count is ~30 (21 GLSL stages + 12 Metal). Realistic target
~16-18 after:
- Consolidate `composite` / `composite_pip` / `composite3` into one
  pipeline with permutations (−2 files).
- Drop `depthviz` if its consumer is just debug (−1).
- Audit `blur` consumer — keep if used, drop if stale.

Cleanup should land before the WebGPU stand-up (rung 1+) so we're not
porting dead shaders to WGSL.

---

## Backlog (not committed to)

- **Aaltonen A.4 vertex packing** — 64 B → 16-20 B per vertex (RGBA8
  color + 2×16 octahedral normal + 2×16 octahedral tangent + half2 UV).
  3-4× stream-1 bandwidth cut. Golden re-bake required (quantization
  moves pixels). Decide alongside the lit pipeline work.
- **A.3 direct-to-swap fast path** — when `active_viewport_count_==1 &&
  !outline_on && !pip`, render forward directly into the swap render
  pass. MSAA on geometry = visual change; design memo before code.
- **R.2 BDA on Vulkan** — DESCRIPTOR-decongestion already achieved via
  Phase D's `DynamicBuffers` (WebGPU-shaped). BDA stays a possible vk
  fast path if measured win justifies. Adreno/Mali driver floor for
  `buffer_device_address` is the gate.
- **Delta-encoded draw stream** — Aaltonen end-state past ~10k draws.
  Per-draw change bitfield, only emit deltas. Reference only.

---

Last touched 2026-06-19. When adding a row: name the issue/branch in
the heading (e.g. `### #299 thing`) so `git log --grep` can find it.
