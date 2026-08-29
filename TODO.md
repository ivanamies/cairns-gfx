# cairns-gfx TODO

Single source of truth for outstanding work. README.md should NOT carry
todos — anything aspirational, deferred, or planned belongs here.
Historical and architectural prose stays in README; future work lives
here.

---

## #picking-accel — CPU ray-cast pick is a linear scan (2026-06-22)

Picking was moved from the GPU id-buffer readback to a CPU ray-cast
(`Engine::ResolvePickRaycast`) so it is SYNCHRONOUS + identical on metal/vulkan/
webgpu -- the browser cannot read the GPU id buffer back synchronously, which was
the only API asymmetry. The GPU id buffer stays only for the outline edge-detect
(GPU-side, already symmetric).

Current shortcuts, fine at hundreds of actors, fix before the 3300-GLB rung:
- **O(entities) linear scan** every pick. Add a BVH or uniform grid so it's
  sub-linear. (Deliberately NOT done yet.)
- **First-mesh bind-pose AABB only.** Union all of a prefab's meshes, and use the
  live animated AABB for skinned actors (the bind pose can be loose/tight vs the
  current pose). Today an animated actor's pick box is the bind-pose box.
- Ray-AABB only (no triangle test) -- a click just inside the box but off the
  silhouette still selects. Acceptable for now.

---

## #resize-surface-bugs — window/surface resize is broken across backends (2026-06-22)

One bug class, three surfaces. The engine renders correctly (golden gate is green
on metal/vk/webgpu; headless WebGPU captures the die/viking perfectly and stably)
— the breakage is all in **swapchain/surface ↔ window resize handling**, and it's
flaky, so investigate with a real repro before "fixing."

- **Browser (cairns_web) — canvas/surface size desync → black after frame 1.**
  REPRODUCIBLE: `web_main.cpp` configures the WebGPU surface + the engine's
  offscreen `final_target_` at a fixed **1280×720**, and `Frame()` copies
  final_target_ → the surface each frame. In a window whose canvas backing store
  isn't exactly 1280×720 (any non-1280×720 window, or dpr≠1), the canvas resizes
  and desyncs from the fixed surface → the copy mismatches → canvas goes black
  after the first good frame. Confirmed: `--window-size=1320,840` (canvas backing
  lands at exactly 1280×720) renders + STAYS; a 2400-wide window goes black.
  FIX: in `Frame()`, poll `emscripten_get_canvas_element_size` (or watch the
  canvas) and on change reconfigure the surface AND resize the engine's
  final_target_/depth to match — i.e. a real resize path, not a fixed size.
  Stopgap: pin the canvas backing to 1280×720 in `shell.html` and letterbox via CSS.
- **Browser — intermittent "2nd+ scenario load renders black."** User-observed
  (first `scn()` click renders, subsequent clicks black). NOT reproducible via CDP
  `Runtime.evaluate(scn(...))` (3 sequential loads all rendered, no console errors),
  so it's timing/rAF- or input-path-dependent, likely entangled with the resize
  desync above or a `scene.clear`+`prefab.unloadAll`→recreate race. Needs a
  deterministic repro (record the exact click cadence) before fixing.
- **Native — random resize bugs on Vulkan + macOS-SDL/metal.** User-reported:
  resizing the sdl-min window intermittently corrupts/blacks the render. Same class
  — the swapchain recreate on `SDL_EVENT_WINDOW_RESIZED` is racy. The three surfaces
  should share ONE resize path: on resize, recreate the swapchain + resize every
  offscreen graph target (final_target_, depth, id, color_off, …) atomically before
  the next frame records. Today each backend handles it ad hoc.

---

## #webgpu-browser-strictness — animated champions in Chrome (2026-06-22)

**RESOLVED 2026-06-23 — the WebGPU/Chrome web app reached metal/vulkan SDL
parity.** Same imgui scenario picker + HUD; all scenarios (triangle, dice,
viking, static + ANIMATED champions, 20-champ + depth strip); CPU ray-cast
pick + selection highlight; click-to-pick. The Dawn-strictness rejections below
are fixed (anim_eval barrier, depthviz sampler), and the bundle was replaced
with lazy GLB fetch-on-demand so the tab no longer OOMs. Remaining browser-only
follow-ups (not parity blockers): the depthviz non-filtering note, async
JS-dispatch under ASYNCIFY, the resize bug class.

WebGPU reached full parity with metal in the **native/headless golden gate**
(16 passed / 1 skipped / 282 assertions, identical to metal). The browser
(Chrome/Dawn, the W6/W7 target) is STRICTER than native wgpu-native, so a few
things that pass the native gate are rejected in Chrome. STATIC champions render
fine in-browser (W7 move-a-champion works); ANIMATED (skinned) champions hit:

- ~~**anim_eval has 12 storage buffers > Chrome's 10**~~ — DONE (b727ddf, #231):
  packed 12 → 6 SSBOs (ae_i32 = parent/topo/joint_nodes/times-bits; ae_vec4 =
  bind_pose/values/inverse_binds; ae_word16 = channels/samplers; + headers +
  world_scratch + palette_out). Pure data-layout change, all 3 backends bit-identical
  (no re-bake). Plus a 10-SSBO / 256 MB boot floor in DeviceCaps that REFUSES devices
  below it (fc921fa) — so the count is no longer a silent-disable, and 6 is well under
  the floor.
- ~~**skin output pool 1 GiB whole-pool bind > 128 MiB floor**~~ — MITIGATED (fc921fa):
  desktop pool capped 1 GiB → 256 MB and the boot guard refuses devices with
  `maxStorageBufferRange` < 256 MB, so the whole-pool bind always fits. (A further
  refinement — binding only the per-batch output slice instead of `WGPU_WHOLE_SIZE` —
  would drop the requirement back toward the 128 MiB floor; not yet done.)
- ~~**anim_eval `workgroupBarrier()` is in non-uniform control flow**~~ — FIXED.
  Dawn rejected the `anim_eval` pipeline (`[Invalid ComputePipeline "anim_eval"]`)
  so animated champions never skinned in Chrome (wgpu-native + native goldens
  accepted it, hiding it). The stage-4 early `return` (on `sh.mesh_node`, read from
  storage so Dawn can't prove it uniform) put the barrier below it in non-uniform
  control flow. Fix (assets/anim_eval.wgsl): drop the early `return`, compute a
  `has_mesh` flag, guard the stage-4 WORK with it so every thread reaches the
  barrier. `sh` is uniform across the workgroup (shared `wid.x`) so output is
  identical -- the `three_champ_anim` webgpu golden stays 92/92 bit-identical.
- **depthviz samples a depth texture with a filtering sampler** — Dawn warns
  ("TextureSampleType::Depth used with a Filtering sampler"). Native is silent.
  Fix: bind a non-filtering sampler for the depthviz pass (point-sample is fine for
  the debug PIP). Only affects nested mode in-browser.
- ~~**WASM heap aborts at ~frame 450** (`Aborted(... corrupted its heap memory
  area (address zero)!)` via `checkStackCookie` in `runIter`)~~ — FIXED. ROOT CAUSE:
  `webgpu/resources.cpp Resources::AdvanceFrame` only did `++plat.frame_index_` and
  NEVER called `alloc.AdvanceFrame` (metal + vulkan both do) -- so the per-frame
  `kDynamic` bump-ring cursor was never reset, climbed ~38KB/frame, overflowed its
  slot, `BumpAllocate` returned nullptr, and (release: `assert` compiled out)
  `memcpy(nullptr+off)` scribbled address zero. Deterministic at ~frame 450
  (slot_size / 38KB). Hit EVERY long web session (idle, one_die, viking, champions
  -- it was never viking-specific); goldens never saw it because they stop at
  frame 55. Fix = mirror metal/vk: `plat.frame_index_++; a.AdvanceFrame(...)`.
  Localized via the FLAKE log's monotonically-climbing `goff[0]`; SAFE_HEAP was a
  dead end (false-positives "alignment fault" in emdawnwebgpu glue at init).
- ~~**scenario SWITCH renders blank** (first scenario draws; every switch after =
  draws 0)~~ — FIXED. General bug, ALL backends (reproduced on metal headless +
  web), == the old "renders first time, not subsequent" symptom. ROOT CAUSE:
  `AssetRegistry::RegisterExistingScene` (src/scene/asset_registry.hpp) dedups by
  `scene_idx` (the prefab index). `UnloadAllPrefabs` Releases the prefab pool +
  clears `per_prefab_asset_` but NEVER clears `assets_`, so after unload the pool
  restarts at index 0 and the new load's `RegisterExistingScene(scene_idx=0,...)`
  hits the STALE boot asset for key 0 and only bumped `ref_count` — leaving
  `cpu_graph` pointing at a Released (generation-bumped) PrefabId. `ExtractFromScene`
  then `GetHot(cpu_graph)==null` → `continue` skips every entity → 0 proxies → 0
  draws. Fix = on a dedup hit, rebind `cold->cpu_graph = scene_id` + refresh the Hot
  pos/attr/index handles to the freshly-loaded prefab (a genuine same-prefab dedup
  passes identical args → no-op). Goldens (single load, frame 55) never hit the
  collision; metal+webgpu goldens stay 92/92. Verified browser static↔anim↔die
  switches all draw. (Latent: `prefab.unloadAll` does NOT call ClearActiveScene
  despite its doc claiming so — entities persist; the picker works only because it
  dispatches `scene.clear` first. Worth aligning the doc/impl later.)

imgui parity in the browser is DONE (this commit): the surfaceless web app opts
into imgui via `Engine::SetImguiEnabled(true)` + the shared `ScenarioLauncher`
panel (`shell/scenario_launcher.hpp`, also used by native main.cpp); DOM mouse ->
ImGui IO in web_main.cpp. The HUD overlay + the scenario picker render in Chrome
and clicking a button loads that scenario -- identical UI to native metal/vk. The
old HTML button bar in shell.html was removed (it duplicated the imgui panel).

W7 deliverable (CDP-driven `scripts/web_move_champion.mjs`: list → select → move a
champion, before/after capture) is DONE with static champions.

---

## #229 scenario launcher + Unity-components refactor (2026-06-21)

Remaining work:

- **Features must be Components, not engine flags** (memory: features-are-
  components-not-engine-flags). Refactor these into entt Components the scene
  spawns + systems iterate; the engine booleans then vanish:
  - `EnableParticles(bool)` → a `ParticleEmitter` component.
  - `SetTinyTriangle(bool)` → a debug-draw component (or just a scene).
  - `SetNestedGraphMode(bool)` + `nested_graph_mode_` + the hardcoded bottom-right
    depth PIP → a `DepthView { rect, source_viewport }` component; the composite
    iterates DepthViews + draws each at its rect (no flag, no hardcoded corner).
- **De-dupe scenarios onto the shared scripts.** `test_golden_scenarios.cpp` still
  embeds inline JS duplicating `assets/scripts/*.js`. Rewire tests to eval the
  bundled scripts (`DriveScriptFile` seam started in `golden_js.hpp`) + bundle
  `scripts/` for the test binary.
- **Drop bundled demo cruft** — `assets/Inter-VariableFont.ttf`, `the_entertainer.ogg`,
  `gs_tiger.svg`, `mc_grass.jpeg` (SDL-template leftovers; confirm imgui doesn't
  load Inter). Remove the `add_resource` lines + the files.

---

## Render-graph gaps vs Themaister Granite (surfaced 2026-06-20)

Our `src/render/render_graph` is a partial copy of Granite's render graph,
whose headline feature is *automatic* barrier/semaphore insertion. The
`three_champ_static` golden flake (FLAKY_TESTS #2) exposed that the copy is
incomplete. Gaps, roughly highest-value first:

1. **Cross-frame / persistent-resource sync — OPEN (perf/correctness).**
   2026-06-21: the three_champ flake is PROBABLY FIXED — it went from ~30%
   reproduction to 0/200, cause unknown (likely a side effect of the #229
   arena/compaction work). Ruled out as a sync issue: debug-vk synchronization
   validation found zero hazards and the CPU sim+render hashes are byte-
   deterministic run-to-run. Still a real Granite gap:
   Granite tracks resources across frames + submissions and barriers persistent
   ones (history buffers, the backbuffer). Our graph rebuilds per-frame with no
   cross-frame dependency tracking, so the persistent `final_target_` (swap
   output) has no barrier between frame N's write and frame N+1's reuse. On
   Metal's untracked heaps that corrupts the host read-back ~1/8. Interim
   mitigation: a per-frame `final_target_` blit-read (forces untracked
   coherence). Proper fix: track imported/persistent resources' last-writer
   across frames and emit the barrier/fence.
2. **Metal graphics→graphics barriers — CLOSED 2026-06-20.** Granite barriers
   any write→read pass pair. vk did this via `transition()` layout barriers;
   Metal did NOT (untracked heaps, only `compute_fence_` for compute→graphics).
   Closed via `gfx_fence_` (EndRenderPass updates / BeginRenderPass waits) — the
   Metal mirror of vk's per-pass transition.
3. **Transient-resource aliasing barriers — UNVERIFIED, likely a gap.** Granite
   aliases transients in a pool AND barriers the aliasing (reused offset must
   sync against the prior resource's last use). Our offset allocator aliases by
   offset reuse; whether the graph emits the aliasing barrier on untracked Metal
   (`makeAliasable` + fence) is unverified.
4. **Fine-grained barrier stages — perf gap.** Granite computes precise src/dst
   stages + access masks per dependency. Ours is brute-force ALL_COMMANDS (vk)
   / whole-encoder fences (metal). Correct but over-syncs.
5. **Render-pass merging / vk subpasses — perf gap.** Granite merges compatible
   passes (tiled-GPU bandwidth). Ours runs each pass as its own render
   pass/command buffer.
6. **Async-compute / multi-queue — perf gap.** Granite schedules compute on a
   separate queue + cross-queue semaphores. Ours is single-queue.
7. **Timeline-semaphore cross-submission sync — tied to #1.** Granite uses
   timeline semaphores for queue/frame handoff; we lean on `WaitIdle` /
   render-thread `Drain` in golden/headless.

Also surfaced (not a graph gap): a **pre-existing MSAA sample-count mismatch**
(1-sample texture vs 4-sample pipeline) aborts the red-triangle subject under
`MTL_DEBUG_LAYER` — latent target/pipeline mismatch worth fixing.

---

## Active

### No global mutable state (no globals, no singletons, no thread-locals)
Delete every global, singleton (`static X& Instance()`), file-scope mutable
`static`, function-local `static`, and `thread_local` — construct the thing and
pass it explicitly; per-instance state lives as a member. ONLY exceptions: state
the language/ABI forces global (the replaceable global `operator new`/`delete`,
e.g. `src/util/alloc_count.cpp`'s counters) or a 3rd-party dependency forces it.
Known offenders:
- `CommandRegistry::Instance()` (src/control/command_registry.{hpp,cpp}) — the
  whole dispatch surface routes through one global. Blocks per-test registries
  (a fresh registry per SCENARIO bound to that SCENARIO's Engine). Make it
  constructible, pass `CommandRegistry&` everywhere (the `Register*Ops` already
  take it by ref). modularization-notes #12.
- `static JsState s` (src/control/handlers/script_ops.cpp:92) — one QuickJS
  runtime/context for the whole process, bound to whatever registry it first
  saw. Must become per-caller state owned alongside the registry it serves.
- `cairns::Timer` static accumulators (accum_times_/accum_itrs_/slot_names_) —
  shared across Engine instances; perf.last + the imgui overlay read them. A
  source of cross-Engine state bleed (modularization-notes #9b territory).
- Audit for more: grep `Instance()`, `static .*&`, file-scope `static` mutable,
  function-local `static`, `thread_local`.

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

### #268 Per-actor frustum cull (was S.3)
Bind-pose AABB capture at load is already on `Mesh::Hot`
(`bind_aabb_min/max`, used by [PICK]). Need: frustum test in
`BuildSkinFrame` vs live frusta, 1.5× extents pad. Skinning + anim_eval
cost becomes ∝ visible.
Test gate: the golden "actors outside the frustum are culled from the counters"
SKIPs at `tests/test_golden_scenarios.cpp:310` — `SKIP("LastFrameStats accessor
not wired yet")`. Wire a `LastFrameStats` accessor exposing the per-frame cull
counters so `REQUIRE(s.culled == kOutside)` can run; until then the cull test is
inert.

### #253 Phase 4 — half4 skin output (decide post P1/P3)

### Instanced draw
Draw N instances of one mesh in ONE call (instance count + per-instance data via
a dynamic-offset / storage buffer indexed by `instance_index`), not N separate
draws. WebGPU / WebGL / DX12 have no base-instance (Aaltonen slide 42), so index
per-instance data off `instance_index`. Consumers: two_die (die.glb ×2) + the
grid scenarios.

### Untextured champion in the nested golden (bad texture bind)
In the golden `nested graph: 20 GLBs resolved color + depth strip`
(`tests/test_golden_scenarios.cpp`, the `[scenarios]` nested case), one of the 20
GLBs renders as a flat WHITE untextured silhouette — row 2, col 3 of the 4×5 grid
(albedo/material not bound for that actor). Deterministic (not a flake). Fix the
texture/material bind, THEN re-bake `nested.color.*` — don't bake the broken
render as the golden. Identify the exact GLB (kDebugGlbs grid index) when fixing.

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
- Audit `blur` consumer — keep if used, drop if stale.

Cleanup should land before the WebGPU stand-up (rung 1+) so we're not
porting dead shaders to WGSL.

---

Last touched 2026-06-21. When adding a row: name the issue/branch in
the heading (e.g. `### #299 thing`) so `git log --grep` can find it.
