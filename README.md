# cairns-gfx

A Metal + Vulkan rendering engine, brought up in a strict canonical order
(below), with handle-pilled / array-pilled data design. Single repo, two
backends, byte-gated per-commit. SDL3 + ImGui + EnTT + Taskflow + custom
RHI.

---

## Product spec / north star

> User is sick of everyone else's proprietary and/or copy-left (bad)
> GUI-driven human-first slow and frail editor engines not immanentizing
> the glorious eschaton of tool-calling VLM recursive self-improvement.
> **Editor-engine must be fast, must be extensible, must be HEADLESS,
> must be AI-first.**

Concretely: the engine binary stays C++ + an embedded QuickJS host (no
libpython, no GIL, no `pip` deps). Every operation is reachable both
interactively (SDL3 + ImGui) and headlessly (newline-delimited JSON over
stdin/stdout). Inference (SDXL, ControlNets, detectors, lifters) lives
in a separate ComfyUI server — local or remote — that the engine talks
to over HTTP. Python is a first-class **caller** via a thin pip client
(`pip install cairns_editor_client`) that subprocesses the editor and
pipes JSON. The engine doesn't link libpython; users mix our editor
with Blender's `bpy`, HuggingFace notebooks, and their own scripts in
whatever language they already speak.

Full headless surface design at
`~/dev/plans/gfx_headless-editor-requirements.md`.

### The canonical demo (verification scenario, ratcheted)

There is **one** verification scenario; every new system extends it.
Don't fork demos per system — each new feature must demo against the
same scene at the same scale, so a single eyeball + a single perf
readout walks the entire engine surface in one pass.

**Baseline (resizing + cameras):**
- Two viewports, side-by-side.
- **Viewport 0 (left):** 100 GLBs × 32 slices = 3200 entities at small
  hero scale.
- **Viewport 1 (right):** 100 GLBs × 1 = 100 entities at normal hero
  scale.
- Window resizes cleanly — drag the corner, both viewports follow at
  half-width each, no leaks, no validation noise.
- Click viewport 0 → focused. Fly around with WASD or vim hjkl + RMB
  mouse-look. Q/E for vertical. Shift for speed.
- Click viewport 1 → focus switches. Fly around that one.
- Take a screenshot.
- Read perf from the engine's stderr log: per-pass GPU times
  (`particle_sim`, `forward_vp0`, `forward_vp1`, `swap`) + CPU times
  (`frame`, `build_draws`, `record`) + total FPS.

**+ shadows** (after the lighting & shadows plan lands):
- Same demo as baseline. **And now add shadows.** Per-light shadow map
  generation passes show up in the log. Verify shadows render
  correctly on both viewports' content. Verify the lighting eval cost
  per pixel doesn't blow the per-platform budget (M2 Max metal,
  MoltenVK, iPhone, S22).

**+ animation** (after the skinning plan lands):
- Same demo. **And now add animation.** Heroes in both viewports are
  skinned + posed (one mocap clip applied via the bone-name retarget
  primitive). `skinning_compute` pass shows up. Verify skin output is
  visually correct (no broken winding, no UV tears) AND that the
  per-pass timer for skinning fits the budget. S22 forward time stays
  in the documented band (see PERFORMANCE.md S22 thermal note).

**+ mesh LOD** (after the LOD plan lands):
- Same demo. **And now check mesh LOD un-effed the triangle
  bottleneck on phones.** The geometry-bound forward pass diagnosed
  at `6386768` (tiny-quad commit) was specifically per-triangle
  vertex/binner work on Adreno. LOD swap-out at distance must drop
  per-frame triangle count by ≥4× at the documented camera poses,
  and S22 forward GPU time must drop proportionally — not flat. This
  is the ratchet that proves LOD actually solved the problem, not
  just shipped a system.

### How I (Coding Claude) actually run the demo

I can't click or WASD-fly interactively. I drive the demo headlessly
via the determinism harness:
1. `editor --serve` (long-running mode).
2. JSON commands to spawn two worlds, populate them (3200 small + 100
   normal-scale), open two viewports.
3. `viewport.setCamera(0, pose_a)`, `render.viewport(0)`,
   `io.dumpTexture(...)` — "click VP0 + fly" reduced to a scripted pose.
4. Repeat with `viewport.setCamera(1, pose_b)` — "click VP1 + fly."
5. Screenshot = `io.dumpTexture` of the swap chain's final output.
6. Perf readout = parse stderr `[Timer]` lines (`per-pass` + `frame` +
   `build_draws` + `record`).
7. For each ratchet step (+ shadows, + animation, + LOD), the same
   script gets one extra setup command at the top and the same
   verification pass at the bottom. The eyeball stays on the same
   scene; the numbers ratchet.

**Honest delegation**: I verify byte-identity, determinism, and perf
readouts via the harness. You verify "did clicking + flying actually
feel right + does it look like the heroes you wanted" by running the
interactive build. The two checks are complementary, not competing.

---

## Requirements: headless iteration + live interaction

Two standing requirements, every platform/backend (memory: `general-requirements`):

**R1 — Headless, fast, no human.** Boot surfaceless, drive entirely over the op
surface (NDJSON / `cairns.script.eval`), capture a PNG or image-hash, iterate.
No window, no clicks. How Coding Claude iterates and how the goldens gate.

**R2 — Windowed live interaction.** A user opens the windowed (or browser) app,
**clicks/picks an entity**, says "move it left", and Claude **evals a script
against the live app** to move it. Needs three things wired per platform: a drive
transport into the live process, `cairns.selection.get` to read the pick, and a
transform op to move it. (`cairns.scene.setTransform {entity,x,y,z,scale}` exists
but is *absolute*; a relative `cairns.scene.translate` / a `getTransform` is the
one small op still to add so "left" composes from the current pose.)

| Platform | R1 — headless drive | R2 — windowed click → eval → move | Capture |
|---|---|---|---|
| **macOS Metal** | ✅ surfaceless `serve`/`sdl-min` + NDJSON on stdin (`CAIRNS_AGENT_STDIN=1`); `cairns_golden_tests` | ✅ `sdl-min` window; user picks (mouse → `selection_`); I pipe NDJSON → `cairns.selection.get` + `cairns.scene.setTransform` | `cairns.io.dumpTexture` → PNG (`target:final`/`window`) |
| **macOS Vulkan** | ✅ identical (one binary per backend) | ✅ identical | identical |
| **iOS (sim/device, Metal)** | ✅ goldens on the simulator (xcodebuild) | ⚠ transport gap — no on-device stdin; needs a socket/USB NDJSON bridge | in-test PNG dump |
| **Android (Vulkan)** | ✅ goldens via `adb` on AVD/device | ⚠ transport gap — `adb forward` socket NDJSON not yet wired | PNG pulled via `adb` |
| **WebGPU native (macOS)** | ✅ surfaceless wgpu-native + NDJSON (`CAIRNS_GFX_BACKEND=webgpu`); golden gate renders triangle + die/two-die/viking, matches macos-metal | 🟡 `SDL_WINDOW_METAL` + metal-layer surface (sdl3webgpu glue) + present-by-copy; `dev_drive.sh start wgpu`. Builds + boots clean (surface/device/pipelines OK); on-screen render unconfirmed in headless CI — verify interactively | offscreen readback → PNG |
| **Web / Chrome (WASM)** | ✅ the SAME `src/main.cpp` SDL shell; boots + renders in headed AND headless Chrome (WebGPU via emdawnwebgpu); `[Timer]`/`[STEADY]` over the CDP console | ✅ headed Chrome: CDP `Input.dispatchMouseEvent` → SDL event → ImGui picker → scenario; `window.cairns.dispatch` bridge for sync ops (picker click needs headed) | CDP `Page.captureScreenshot` → PNG |

✅ wired · ⚠ partial (transport gap) · 🔭 planned (see `~/dev/plans/gfx_webgpu-wgpu-native-standup.md`).

---

## Architecture (game + render thread)

Pipeline depth 2: the main (game) thread builds frame N+1 while the render
thread does `Frames::Begin` / `Execute` / `End` for frame N. Steady-state
wall-clock collapses to `max(game_build, render_cycle) ≈ vsync_interval` so
we land at 60 fps so long as the GPU itself fits in vsync.

```
 main (game) thread                          render thread
 ────────────────────                        ──────────────────
 loop:                                       loop:
   SDL_PollEvents                              pkt = queue.Pop()    // blocks if empty
   ImGui NewFrame + UI + Render                Frames::Begin(res, alloc)
   FrameClock::Tick → sim step                   ↳ semaphore_wait + AcquireNextImage
   Extract → proxies (FrameArena)              Build per-draw UBOs from pkt
   BuildMeshOpaqueDraws → drawList             (rhi_.alloc.BumpAllocate ×N)
   BuildSkinFrame → palettes + batches         Build + Bake RenderGraph
   FramePacket pkt{...}                        graph_.Execute(fc, swapchain_)
   queue.Push(pkt)                             Frames::EndSubmit
   queue.WaitIfFull(kFramesInFlight)           if pkt.dump_frame: do dump + signal
```

**Ownership rules:**
- Game thread: SDL, ImGui, sim, Extract, BuildMeshOpaqueDraws,
  BuildSkinFrame. No GPU bump-alloc, no `Frames::*`, no render-graph
  build (graph build + Bake + Execute live on the render thread inside
  `RecordFrame`; see Deferred refactors below).
- Render thread: per-draw UBO bumps (one allocator on one thread),
  render-graph build + Bake + Execute, `Frames::Begin/EndSubmit`. No sim,
  no ImGui, no resource `Acquire`/`Release`.
- Shared: `Resources::GetHot()` is read-only and safe.
  `Resources::Acquire` / `Release` are game-thread-only (load-time today;
  runtime spawns deferred).

**Hand-off:**
- Forward SPSC queue (game → render) of depth `kFramesInFlight = 2` carries
  `FramePacket*` allocated from the per-frame `FrameArena` slot.
- Parity-return SPSC queue (render → game) carries the post-pass particle
  parity for the next sim step.

**Determinism:** under `CAIRNS_DUMP`, the dump frame collapses to a lock-step
handshake so `scripts/verify_metal.sh` stays byte-identical to the existing
golden.

### Deferred refactors

Lives in `TODO.md` ("Deferred refactors" section). Render-graph
build-on-game-thread is the headliner; see there.

---

## Render graph: where we strayed from Granite

`src/render/render_graph` is a partial copy of Themaister's Granite render graph
(`renderer/render_graph.{cpp,hpp}`). Granite's headline feature is *automatic,
complete* barrier/semaphore generation — and as of `309fb53` the
synchronization model is fully ported for correctness (invalidate/flush +
WAR + buffers, all backends off the same graph-computed barriers; the
historical cross-frame `final_target_` WAW flake is dead). **Rule: copy
Granite, do not re-invent.** Point-by-point:

**The core stray — barriers belonged in the graph with persistent per-resource
state. PORTED (texture model `08fccb5`; WAR + buffers `3d9cd68`..`309fb53`).**

- **Granite computes barriers in `bake()`; now we do too.** `render_graph::
  Execute` computes per-pass `invalidate` (before) + `flush` (after) barriers
  from each resource's persistent `PipelineEvent` (`Texture::Cold.sync`) and
  hands them to the backend via `BeginRenderPass(invalidate)` /
  `EndRenderPass(flush)`. Was: nothing (vk ad-hoc `transition()`; metal ad-hoc
  `compute_fence_`).
- **`PipelineEvent` is now persistent across frames** (`Texture::Cold` /
  `Buffer::Cold`), so it survives the per-frame graph rebuild and the cross-frame
  `final_target_` WAW is tracked. We collapse Granite's `invalidated_in_stage[64]`
  to a coarse `invalidated_access`/`_stages` pair (gap 1 below).
- **RAW + WAW + layout + WAR are all modeled.** Inputs → invalidate (RAW),
  outputs → flush (WAW + layout), and Granite's "fake flush, access=0" for
  read-only resources is ported: reads join `src_stages`, a later writer emits
  an execution-only barrier and resets the accumulation
  (`AccessResource`, `rhi/barrier.hpp` — pure, spec-driven).
- **Both backends now execute the SAME graph-computed barriers** (no mismatch —
  see the no-metal-vulkan-mismatch standard): metal via a per-resource
  `MTLFence` (`sync_fence_` on `TextureColdPlat`, replacing the ad-hoc
  `gfx_fence_`); vk via one `vkCmdPipelineBarrier` (`apply_invalidate_barriers`,
  replacing the ad-hoc `transition()`, now deleted). The graph carries the
  abstract `BarrierLayout/Access/Stage`; each backend translates the leaf.
- **Metal untracked-heap caveat — a Metal semantics gap, NOT a Granite gap.**
  Granite is Vulkan: its images are tracked, so a barrier both ORDERS and
  COHERES. Metal placement-heap resources are `HazardTrackingModeUntracked`, so
  an `MTLFence` orders but does NOT cohere — the host read of `final_target_`
  saw stale memory even after a correct WAW fence. Fix: `final_target_` (and any
  host-read target) is allocated as a DEDICATED TRACKED `MTLTexture` (outside the
  untracked placement heap) so Metal auto-coheres it. Granite never needs this;
  it's a consequence of our untracked-by-default heaps.
- **Buffers ride the same invalidate/flush** (no layouts). Passes declare
  `ReadBuffer`/`WriteBuffer`; compute passes get the barrier walk via
  `BeginComputePass`/`EndComputePass`. Leaves: vk = one global
  `VkMemoryBarrier` per boundary; metal = per-buffer `MTLFence`
  (`BufferColdPlat`, stash/drain around dispatch encoders); webgpu = implicit.
  The anim_eval → skin → forward vertex-fetch chain runs on these graph
  barriers alone — the ad-hoc `compute_fence_` is deleted.

**Granite features we lack entirely (rough value order):**

1. **Per-stage invalidation tracking** (`invalidated_in_stage[64]`) — avoids
   redundant barriers + correct stage scoping. We'd brute-force ALL_COMMANDS.
2. **Split barriers via `VkEvent`** (`pipeline_barrier_src_stages`,
   `wait_events`) to overlap work across a barrier.
3. **Pass reordering to maximize overlap** (bake step: minimize the
   latest-pass-we-must-wait-for). We do topo order only.
4. **Render-pass merging into Vulkan subpasses** (adjacent graphics passes,
   shared attachments, `BY_REGION_BIT`, `VkSubpassDependency`). Every pass is its
   own render pass / encoder — no tiler bandwidth win.
5. **Transient aliasing with aliasing barriers** (`alias_transfer`: copy barrier
   state, force layout `UNDEFINED` at the alias boundary). We do basic transient
   assignment (`AcquireTransientTexFlat`) but emit no aliasing barrier.
6. **Async compute / multi-queue semaphores** (`used_queues`,
   `wait_{graphics,compute}_semaphore`, `CONCURRENT` ownership). Single-queue.
7. **History / feedback resources** (`history_inputs`,
   `physical_history_events` — read the previous frame's version). Absent.

**Correctness parity: done** — persistent `PipelineEvent`, graph-computed
invalidate/flush on textures AND buffers, WAR fake-flush, all backends off the
same barriers, `final_target_` dedicated-tracked for Metal coherence.
**Remaining = perf items only** (per-stage scoping, split-barrier events, pass
reorder, subpass merge, transient aliasing barriers [NPR plan M0c], async
compute, history resources). Plan:
`~/dev/plans/gfx_granite-sync-port.md`.

---

## Frame pacing: where we differ from Unity

Unity's frame pacing (and its Android Frame Pacing / Swappy library) targets a
*stable presentation cadence* — it picks a swap interval, schedules present
times against the display's refresh, and is free to drive the swapchain from a
dedicated present/render thread on most platforms.

We're **macOS/Metal-first**, and the platform constraint dominates: the
`CAMetalLayer` drawable + window live on the **main thread**, so acquire/present
must ultimately route there. So instead of Unity's "present from wherever, pace
to refresh," we do **main-thread gymnastics**: the game thread builds the frame
packet and hands it to the render thread (`RenderThread::Submit`), the render
thread records, and the present is handed *back* to the main thread via the
`present_queue_` + `Frames::Present` (a no-op body on Metal — the actual
`presentDrawable` is enqueued on the command buffer, which Apple allows
off-thread, but the drawable acquire + window ownership keep us main-thread
bound). The result is correct but it is *plumbing to satisfy the main-thread
present*, not a frame-pacing controller: we do not yet target a stable cadence or
pace against the display link the way Unity/Swappy do. A real pacing controller
(VSync-locked target frame time, present-time scheduling) is future work.

---

## Memory management

Source of truth for **which struct/system gets which allocator**. Distilled
from `~/dev/plans/ALLOCATOR_HANDOFF.md`. When in doubt, match an existing row.

### Four allocator archetypes (one per lifetime shape)

| # | Allocator | Header | Owns | Currency | Use when |
|---|---|---|---|---|---|
| **A** | `cairns::BumpArena` / `FrameArena` | `src/util/cpu_arena.hpp` | a slab (not memory) | `uint32` offset | per-frame transient; reset every frame |
| **B** | `cairns::ChunkAllocator` | `src/util/chunk_allocator.hpp` | malloc-backed 4 MB chunks (size-class free lists) | `void*` (behind a handle/index elsewhere) | irregular size + irregular lifetime; the root CPU memory source |
| **C** | `cairns::RangePool` / `TypedPool<T>` | `src/util/cpu_pool.hpp` | offsets only (no memory) | offset (`PoolSlice`) | persistent, individual free, **variable-size ranges in one buffer** (typically a GPU buffer) |
| **D** | `cairns::ResourceManager<T>` | `src/core/handle.hpp` (templated; usable from anywhere) | dense Hot/Cold SoA arrays | `Handle<T>` (generational) | persistent objects with individual create/destroy and **stable identity** |

**Layering** (read it back as a mental model):
- **B owns memory.**
- **A's slab is borrowed from B** (the engine's `FrameArena` slab is
  `hot_arena_.Allocate(N)`).
- **Load-time tier = a `BumpArena` whose slab is one B chunk**; freed
  wholesale at asset unload by returning the chunk to B.
- **C owns only offsets** — the backing buffer is whatever the caller passes
  (usually a GPU buffer created via the untouched GPU allocator).
- **D owns its own dense Hot/Cold arrays** (no B underneath —
  `ResourceManager<T>` allocates its own backing `std::vector`s).

GPU memory (`src/rhi/{vulkan,metal}/memory_allocator.*`,
`src/rhi/allocator.*`) is a separate concern and not touched by the CPU side.
Palettes etc. still upload through
`rhi::Allocator::BumpAllocate(..., Memory::kDynamic)`.

### What goes where (data → allocator)

#### A — per-frame transient (`FrameArena`; `BumpStdAllocator<T>` for STL containers)
Rebuilt every frame; valid only until the next `BeginFrame(frame_)` on this
slot.
- `RenderPassGlobals`, `MaterialGpu`, `DrawTmp` (currently bumped into
  `Memory::kDynamic` — same lifetime, GPU side; CPU staging that touches it
  = A)
- Draw list + sorted draw keys (when rebuilt every frame)
- Render proxies (extract output)
- Joint palettes (CPU side) → uploaded into `kDynamic`
- `InstanceMeta` / `SkinMeshBatch` (per-frame skinning arrays)
- ImGui vertex/index/cmd buffers — immediate-mode, discarded at frame end
- Color-correction per-frame params → `kDynamic`
- Physics solver scratch (contacts, islands, broadphase pairs) ⚠ data-
  dependent size + MT → needs growable + per-thread variant; that extension
  lives in `cpu_arena.hpp` when it lands

#### B — general chunk allocator (`ChunkAllocator`; `Allocator<T>` STL adapter)
The root CPU memory source. Anything irregular in size or lifetime that
isn't transient.
- The engine's `hot_arena_` (this IS a `ChunkAllocator` via the `Arena`
  typedef)
- `Scene::meshes` / `Scene::nodes` (long-lived once loaded)
- The engine's `scenes_`, `drawList_`, `drawListSorted_`,
  `root_nodes_stack_cache_` — `std::vector<T, cairns::Allocator<T>>` already
  routes here
- Animation clips (keyframe tracks) — load-time; per-asset `BumpArena` over
  a B chunk, returned wholesale on unload
- Skins (joint-index sets) — same
- Inverse-bind matrices — same
- Packed skin SSBO CPU staging — `Mark`/`Rewind` inside a load-time bump
  arena
- Scene mesh CPU temporaries (positions/attrs/indices) — dropped after
  `LoadScenesGpu`
- Scene hierarchy edge lists (`Node::children`)
- Strings (object names, file paths)
- Editor UI retained state, undo/redo buffers

#### C — range / offset pool (`RangePool`; `TypedPool<T>` for CPU-backed)
Persistent, individual create/destroy, variable-size ranges in ONE backing
buffer; the unit is an offset (`PoolSlice`).
- `skin_output_pool_` — the canonical user (one persistent GPU buffer;
  per-entity skinned vertex ranges spawn/despawn individually;
  `s.offset * stride` is the bind offset)
- Future single-buffer sub-allocations of the same shape

#### D — resource manager (`ResourceManager<T>`; `Handle<T>` generational)
Persistent objects with individual create/destroy and **stable identity**
(so stale handles are detected, not silently aliased).

Currently wired:
- All GPU resources: `Buffer`, `Texture`, `Sampler`, `BindGroup`,
  `DynamicBuffers`, `Shader`, `Kernel` (`src/rhi/resources.hpp`).
- `World` (Hot/Cold; `World::Cold` holds a `std::unique_ptr<entt::registry>`
  for pointer stability across pool growth).
- `Asset` (Hot/Cold; `Hot` = shared GPU buffer handles; `Cold` =
  `const Scene*` + suballoc slices + ref count). Owned by `AssetRegistry`,
  the one class wrapper that earns its name (dedup + load + suballoc +
  cascading lifetime).
- Inside each `World`, an `entt::registry` whose `entt::entity` is itself a
  generational handle — components attached per entity. Cross-boundary
  references travel as `EntityRef = { WorldId, entt::entity }`.

**Deferred until skinned content lands**:
- `SkinnedAttachment` Hot/Cold (the per-skin generational descriptor that
  pairs with `RangePool` for the GPU output range). See `P8` skin audit in
  `src/scene/components.hpp` for the design.

**Deferred until an editor or runtime spawn lands** (these are load-once +
index-stable today, so D's generational handles add cost without buying
anything testable):
- `Node` (`Scene::nodes` is `std::vector<Node>`). It's a TREE, not a flat
  pool — `Node::children` carries node indices, `Extract` walks
  `scene.nodes[node_idx]` deeply. Converting to `ResourceManager<Node>`
  means children become `Handle<Node>` (or live in a side array) and the
  tree walk becomes handle-resolution per step. Mechanical refactor, but
  no spawn/despawn exercises it today.
- `LoadedMaterial` (engine `materials_` is `std::vector<LoadedMaterial>`,
  indexed by `MatId`, which is packed into `DrawKey` + parallels
  `material_bind_groups_`). `MatId` becoming `Handle<LoadedMaterial>`
  ripples through the draw key encoding and every prim's `material_id`.
  Same call: land it the day an editor lets materials be
  created/edited/deleted at runtime.

Future (when they exist): physics bodies / colliders.

The Hot/Cold split per type: per-draw / per-frame fields in **Hot**
(transform, mesh handle, flags); names, editor metadata, undo refs in
**Cold**. The hot path walks the **packed live list** of indices (updated
on `Acquire`/`Release`), not the sparse Hot array.

### House rules

**One-line standard:** *store handles/indices/offsets; resolve to a pointer
only as a local; the only allocator that traffics in escaping pointers is
B, and only behind a handle/index owned elsewhere.*

**The reference gradient (best → worst):**
`Handle<T>` (generational, stale-detecting) → index (into a known array) →
offset (into a known buffer/slab) → raw pointer.

**Anti-patterns to reject in review:**
- Any struct that **stores** a `Node*`, `Entity*`, or `Clip*` obtained from
  B. Storing it is the use-after-free a handle system exists to prevent;
  resolving it transiently inside one function is fine.
- Re-introducing a `void*→metadata` side table on top of B. The 16 B header
  before each user pointer makes `Free` O(1) without one.
- Over-pilling the hot loop: resolving a generational handle per vertex /
  joint / proxy is self-sabotage. Handles live at boundaries and storage;
  the inner loop walks dense arrays directly (Hot/Cold SoA).

### File inventory

| File | Role |
|---|---|
| `src/util/chunk_allocator.hpp` | B — `ChunkAllocator` + `ChunkStdAllocator<T>` |
| `src/util/cpu_arena.hpp` | A — `BumpArena`, `FrameArena`, `BumpStdAllocator<T>` |
| `src/util/cpu_pool.hpp` | C — `PoolSlice`, `RangePool`, `TypedPool<T>` |
| `src/util/offset_allocator.hpp` | Sebastian Aaltonen's offset suballocator (C wraps it) |
| `src/util/std_allocator.hpp` | aliases (`Arena = ChunkAllocator`, `Allocator<T> = ChunkStdAllocator<T>`) for backward-compat call sites |
| `src/core/handle.hpp` | D — generic `Handle<T>` + `ResourceManager<T>` |
| `src/rhi/resource_manager.hpp` | GPU resource types (`Buffer`, `Texture`, ...) + `using` aliases re-exporting D into `cairns::rhi::` |
| `src/rhi/{vulkan,metal}/memory_allocator.*` | GPU memory — **DO NOT TOUCH from CPU code** |

---

## Testing

Everything builds and runs in **Release** by default (the root CMakeLists
defaults `CMAKE_BUILD_TYPE=Release`; the spec target keeps asserts on via its
own `-UNDEBUG`, so optimization + asserts coexist). Two tiers:

- **Tier S — spec tests.** Pure CPU, no engine, no GPU. Build first, run in
  milliseconds, identical on every platform via `ctest -L spec`. ~105
  SCENARIOs covering the data-oriented core (allocators, frustum math,
  particle determinism, draw-key bit layout, render-graph scheduling,
  multi-scene per-viewport draw fan-out, scene ECS PODs, RHI descriptor PODs,
  threading SPSC invariants).
- **Tier G — golden / divergence tests.** Two categories, both per-platform
  image refs + a few per-platform buffer/state refs:
  - **`[scenarios]`** (`test_golden_scenarios.cpp`) — correctness. The rendered
    subjects (triangle, one/two die, viking_room, three static/animated
    champions) as flat `[subject]` SCENARIOs, plus the targeted subsystem
    scenarios (particles state-hash, hot reload, two scenes/two heroes,
    nested graph, frustum cull, imgui stability).
  - **`[stress]`** (`test_golden_stress.cpp`) — scale (100 distinct animated
    champions). Heavy; kept out of `[scenarios]` so the fast pass stays fast.
  Image refs live under `tests/refs/` as `{name}.f09.{platform}.imghash`
  (frame 9) and `.f55.imghash` (frame 55).

Plan and notes:
- `dev/plans/gfx_test-tech-tree-redo-phase-a-to-g.md` — the plan
- `dev/plans/gfx_modularization-notes.md` — generalization
  opportunities accumulated while writing the suite

### Configure (once per platform / build flavor)

```sh
# macOS Metal
cmake -S . -B build/spec-mac-metal -DCAIRNS_GFX_BUILD_TESTS=ON \
  -DCAIRNS_GFX_BUILD_GOLDEN_TESTS=ON -DCAIRNS_GFX_BACKEND=metal -G Ninja

# macOS Vulkan (MoltenVK)
cmake -S . -B build/spec-mac-vk -DCAIRNS_GFX_BUILD_TESTS=ON \
  -DCAIRNS_GFX_BUILD_GOLDEN_TESTS=ON -DCAIRNS_GFX_BACKEND=vulkan -G Ninja

# iOS simulator
cmake -S . -B build/spec-ios-sim -DCAIRNS_GFX_BUILD_TESTS=ON \
  -DCAIRNS_GFX_BUILD_GOLDEN_TESTS=ON -DCAIRNS_GFX_BACKEND=metal \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -G Ninja

# Android NDK (S22 or Pixel 6a AVD)
cmake -S . -B build/spec-android-vk -DCAIRNS_GFX_BUILD_TESTS=ON \
  -DCAIRNS_GFX_BUILD_GOLDEN_TESTS=ON -DCAIRNS_GFX_BACKEND=vulkan \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 \
  -DMOBILE_ASSETS_DIR=/tmp/cairns-spec-android-assets -G Ninja
```

### Tier S — spec suite

```sh
# macOS metal (~102 SCENARIOs, ~1s)
cmake --build build/spec-mac-metal --target cairns_spec_tests -j
ctest --test-dir build/spec-mac-metal -L spec --output-on-failure

# macOS vk
cmake --build build/spec-mac-vk --target cairns_spec_tests -j
ctest --test-dir build/spec-mac-vk -L spec --output-on-failure

# iOS sim (boot iPhone 16 / iOS 18.3 first)
xcrun simctl boot AFF28AB7-49F7-421A-B589-7D318757C566
cmake --build build/spec-ios-sim --target cairns_spec_tests -j
xcrun simctl spawn booted \
  ./build/spec-ios-sim/cairns_spec_tests.app/cairns_spec_tests --reporter compact

# Android NDK (S22: R5CTA35BGLL; AVD: emulator-5554)
cmake --build build/spec-android-vk --target cairns_spec_tests -j
adb -s R5CTA35BGLL push build/spec-android-vk/cairns_spec_tests /data/local/tmp/
adb -s R5CTA35BGLL shell chmod +x /data/local/tmp/cairns_spec_tests
adb -s R5CTA35BGLL shell /data/local/tmp/cairns_spec_tests --reporter compact
```

### Tier G — golden tests (desktop)

The cairns_golden_tests binary runs from the build dir; assets are staged
alongside.

```sh
# macOS metal
cmake --build build/spec-mac-metal --target cairns_golden_tests -j
./build/spec-mac-metal/cairns_golden_tests --reporter compact

# Filtered subsets (binary lands under Release/ in a Release build dir):
./build/spec-mac-metal/Release/cairns_golden_tests "[scenarios]"  # correctness
./build/spec-mac-metal/Release/cairns_golden_tests "[stress]"     # 100 champs
./build/spec-mac-metal/Release/cairns_golden_tests "[subject]"    # rendered subjects only
./build/spec-mac-metal/Release/cairns_golden_tests "[particles]"  # particle state-hash alone

# Same shape for vk:
./build/spec-mac-vk/Release/cairns_golden_tests --reporter compact
```

### Tier G — iOS simulator

Assets bundle into `.app/Resources/` via the CMake glob.

```sh
cmake --build build/spec-ios-sim --target cairns_golden_tests -j
xcrun simctl spawn booted \
  ./build/spec-ios-sim/cairns_golden_tests.app/cairns_golden_tests \
  "[subject]" --reporter compact
```

### Tier G — Android (S22 device or Pixel AVD)

NDK raw binary; assets pushed via adb; path resolution via env vars
(`CAIRNS_BASE_PATH`, `CAIRNS_PLATFORM_KEY`). `CAIRNS_PLATFORM_KEY`
distinguishes the Adreno device (`android-vk`) from the SwiftShader
emulator (`android-vk-emu`) so refs don't collide.

```sh
DEV=emulator-5554   # or R5CTA35BGLL for S22
PLATFORM_KEY=android-vk-emu   # use android-vk on S22

# Stage binary + libs + assets (once per device reset)
adb -s $DEV shell mkdir -p /data/local/tmp/cairns_test/refs
adb -s $DEV push build/spec-android-vk/cairns_golden_tests /data/local/tmp/cairns_test/
for so in build/spec-android-vk/*.so; do
  adb -s $DEV push "$so" /data/local/tmp/cairns_test/
done
adb -s $DEV push /tmp/cairns-spec-android-assets/. /data/local/tmp/cairns_test/

# Run
adb -s $DEV shell "cd /data/local/tmp/cairns_test && \
  CAIRNS_PLATFORM_KEY=$PLATFORM_KEY \
  CAIRNS_BASE_PATH=/data/local/tmp/cairns_test \
  CAIRNS_TEST_REFS_DIR=/data/local/tmp/cairns_test/refs \
  LD_LIBRARY_PATH=. ./cairns_golden_tests '[subject]' --reporter compact"

# Pull baked refs back into the repo
adb -s $DEV pull /data/local/tmp/cairns_test/refs/. tests/refs/
```

### Eyeball / headless PNG inspection

```sh
# Dump rendered PNGs alongside the hash check.
CAIRNS_DUMP_PNGS=/tmp/cairns-pngs \
  ./build/spec-mac-vk/cairns_golden_tests "[subject]"

# A tiny stb-image-based pixel sampler lives at /tmp/png_inspect.c
# (rebuild with: clang -std=c11 /tmp/png_inspect.c -o /tmp/png_inspect)
# It prints non-clear-pixel counts and a bounding box so you can verify
# headlessly that "viking_room actually renders viking_room."
```

### Re-baking refs

Refs auto-bake on first run when missing. To force a re-bake:

```sh
# Delete one to force-bake on next run (it will trivially pass once).
rm tests/refs/<name>.<platform>.imghash

# Or force-bake everything:
CAIRNS_GFX_BAKE_REFS=1 ./build/.../cairns_golden_tests
```

### Known caveats

- `[scenarios]` full-suite run flakes the particles and imgui image hashes
  intermittently (shared ImGui static state across SCENARIOs; tracked in
  modularization-notes #9b). Run `[particles]` and `[imgui]` separately
  for stable results.
- S22 device must be physically connected for the `android-vk` refs to
  be writable; otherwise only AVD (`android-vk-emu`) bakes.
- iOS sim runs require a booted simulator (`xcrun simctl boot <UDID>`).
- Desktop goldens run from the build dir (assets staged alongside the
  binary); `cd build/<dir>` first if running with a relative path.
- Frustum-cull counter scenarios SKIP until a production cull stage lands
  (engine does not call `cairns::AabbOutsideFrustum`); tracked in
  modularization-notes #10b.
- Particle-buffer / resolved-depth / skin-buffer SECTIONs SKIP until
  `Resources::ReadBackBuffer` lands on both backends (open task).

## WebGPU backend — build & run

The third RHI backend: gfx-rs **wgpu-native** C bindings headless/native,
emdawnwebgpu in the browser. At parity with metal/vk on the golden subjects;
stand-up history in `~/dev/plans/gfx_webgpu-wgpu-native-standup.md`.

### Build + run

```sh
# wgpu-native artifacts are gitignored; fetch once after a fresh checkout:
third_party/wgpu-native/fetch.sh   # macOS-arm64 prebuilt, pinned v29.0.0.0

# configure + build the headless host (Ninja single-config):
cmake -S . -B build/spec-mac-webgpu -DCAIRNS_GFX_BACKEND=webgpu -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build/spec-mac-webgpu --target cairns_serve

# render the procedural triangle + dump final_target_ to a PNG:
cd build/spec-mac-webgpu/Release
( echo '{"op":"cairns.primitive.create","args":{"type":"triangle"}}'
  echo '{"op":"cairns.render.frame"}'
  echo '{"op":"cairns.io.dumpTexture","args":{"target":"final","path":"out.png"}}'
  echo '{"op":"cairns.app.quit"}'; sleep 4 ) | timeout 30 ./cairns_serve
# -> out.png (1280x720): a centered red triangle on the clear color.

# Forward goldens (die / two-die / viking). Build the golden test, then bake
# or compare the macos-webgpu refs. CAIRNS_DUMP_PNGS=<dir> also writes per-frame
# PNGs (eyeball them); they should visually match the macos-metal renders.
cmake -S . -B build/spec-mac-webgpu -DCAIRNS_GFX_BACKEND=webgpu \
  -DCMAKE_BUILD_TYPE=Release -G Ninja -DCAIRNS_GFX_BUILD_TESTS=ON \
  -DCAIRNS_GFX_BUILD_GOLDEN_TESTS=ON
cmake --build build/spec-mac-webgpu --target cairns_golden_tests
cd build/spec-mac-webgpu/Release
./cairns_golden_tests "Scenario: subject: one die (single static textured mesh)"
# Catch2 ANDs multiple name args -- run scenarios one at a time, not in one call.
# CAIRNS_GFX_BAKE_REFS=1 overwrites; a missing macos-webgpu ref auto-bakes once.
```

### Driving the WASM build in a browser (perf / interactive)

The web build runs the **same `src/main.cpp` SDL shell as native** (SDL canvas
window + `ImGui_ImplSDL3` + SDL main callbacks; WebGPU via emdawnwebgpu). It
boots and renders in **both headed and headless** Chrome — WebGPU works headless
(the HUD + picker + scene all draw). Prefer **headed** for *interactive* work:
CDP-injected mouse events only reach the emscripten canvas in a headed window, so
**loading a scenario via a picker click needs headed** (a pre-existing
Chrome/emscripten input quirk, unchanged by the SDL migration). Headless still
boots, renders, and streams `[Timer]`/`[STEADY]` over the CDP console.

Serve the build, launch a **dedicated, throwaway-profile** Chrome, drive over CDP:

```sh
# 1. Serve (single-threaded ASYNCIFY build -- no COOP/COEP headers needed):
( cd build/web-webgpu/Release && python3 -m http.server 8771 )

# 2. Chrome + remote debugging. --user-data-dir MUST be a throwaway (see the
#    crash warning below); NEVER the user's default profile. Add --headless=new
#    for an unattended render (boots + draws; picker clicks need headed).
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --remote-debugging-port=9333 --enable-unsafe-webgpu --use-angle=metal \
  --user-data-dir=/tmp/cairns-chrome-profile \
  http://localhost:8771/cairns_web.html
```

**Select a scenario by CLICKING its picker button** (headed) via CDP
`Input.dispatchMouseEvent` (`mouseMoved` → `mousePressed` → `mouseReleased`,
`button:"left"`). The click flows DOM → SDL's emscripten video driver → SDL event
→ `ImGui_ImplSDL3_ProcessEvent` → the picker → the launcher — the SAME path as
native. Read perf/logs by draining `Runtime.consoleAPICalled`.
`scripts/web_capture.mjs` is the CDP scaffold (add the Input click; sequence:
navigate → boot-wait → Input click → long wait for load+steady → drain console).

**What does NOT work (and why):**

- ❌ **`window.cairns.dispatch("cairns.prefab.loadBatch", …)` directly — CRASHES
  THE TAB.** `loadBatch` is ASYNCIFY (it lazy-fetches GLBs and awaits), but the
  `cairns_dispatch` ccall is synchronous, so emscripten hits
  `Aborted(Assertion failed: The call to cairns_dispatch is running
  asynchronously…)`. That `abort()` tears down the WASM instance. The **picker
  click is the only async-safe way in** — it dispatches from the main loop.
- ❌ **Synthetic JS DOM events** (`canvas.dispatchEvent(new MouseEvent(...))`) are
  **ignored**: emscripten/SDL only honor **trusted** events (`isTrusted`), so
  they never reach ImGuiIO. Use CDP `Input.dispatchMouseEvent` (OS-level, trusted).

> ⚠️ **CRASH WARNING — never automate the user's browser.** Always launch a
> throwaway `--user-data-dir` and your own Chrome instance. A crashing
> WASM/WebGPU tab can destabilize Chrome's shared GPU process and take down
> **other tabs — including unsaved documents — in the same session.** The
> `loadBatch`-abort above is exactly such a crash; it has killed a live browser
> window mid-session. Treat the browser as the user's live workspace, not a
> disposable target.

> **GPU timings are BROKEN on wgpu.** The HUD `gpu_frame` reads `0.00` and the
> GPU-pass timer slots (`forward_vp0`, `skinning_compute`) are not recorded on
> the WebGPU backend — only the CPU-side slots (`frame`, `build_draws`,
> `record`, `skin_eval`, `present_wait`) are valid. See PERFORMANCE.md.

> After adding a new `assets/*.wgsl`, RE-RUN cmake configure — `file(GLOB)`
> only re-globs at configure time, so a freshly added shader isn't copied next
> to the binary until you reconfigure (a "missing wgsl" stderr line is this).

### Smokes (de-risk, off by default; binaries land in the build-dir ROOT, not Release/)

```sh
cmake -S . -B build/spec-mac-metal -DCAIRNS_BUILD_WGPU_SMOKE=ON   # any backend dir
cmake --build build/spec-mac-metal --target wgpu_link_smoke
cmake --build build/spec-mac-metal --target wgpu_readback_smoke
build/spec-mac-metal/Release/wgpu_link_smoke       # prints the adapter (Apple M2 Max)
build/spec-mac-metal/Release/wgpu_readback_smoke   # pixel0 = 64 128 191 255 (readback proof)
```

### Verify metal/vk unaffected (the whole backend is `#if CAIRNS_WEBGPU`-guarded)

```sh
cmake --build build/spec-mac-metal --target cairns_golden_tests
(cd build/spec-mac-metal/Release && ./cairns_golden_tests "[scenarios]")  # 11 passed / 1 skipped
```

### Layout + invariants

- Backend code: `src/rhi/webgpu/*.cpp` + `*_plat.hpp`, every file `#if CAIRNS_WEBGPU`-guarded.
  The `file(GLOB_RECURSE)` picks them up; metal/vk compile them as empty TUs (stay green).
- Macro `CAIRNS_WEBGPU` (`util/define.hpp` from `gfx_config.hpp.in`); CMake
  `-DCAIRNS_GFX_BACKEND=webgpu`. `cairns_core` + `cairns_render_thread` link the
  `wgpu_native` INTERFACE target (static `.a` + macOS frameworks + include dir).
- Handles type-erased to `void*` (like vk): `api_view`=WGPUTextureView,
  `api_image`=WGPUTexture, `api_pso`=WGPURenderPipeline.
- Allocator (`memory_allocator.cpp`): per-resource individual `WGPUBuffer`s + a CPU-staging
  bump ring; uploads via `wgpuQueueWriteBuffer` (WebGPU has no persistent host mapping).
- Readback (`resources.cpp ReadBackTextureRgba`): `copyTextureToBuffer` (bytesPerRow
  256-aligned) → `wgpuBufferMapAsync` + `wgpuDevicePoll(true)` → unpad rows → BGRA→RGBA swizzle.

### Gotchas (cost real time — do not relearn)

- **WebGPU defaults `maxStorageBufferBindingSize` to 128 MB.** The 1 GB desktop skin pool
  exceeds it → `Device::Init` queries `wgpuAdapterGetLimits` + passes them as `requiredLimits`
  at device creation. Without this, GreaterInit FATALs on the skin-pool fit check.
- **A `Null` graphics-pipeline handle HANGS GreaterInit.** `CreateGraphicsPipeline` must
  `Acquire` a real handle (a null `api_pso` is fine for not-yet-implemented pipelines) — never
  return `Handle<Shader>::Null`.
- Bundled `run.js` runs at boot (`serve_main.cpp RunBootScript`); it loads GLBs (the
  `wgpuQueueWriteBuffer` traffic you see). `engine_ok` gates render/scene ops registration —
  if GreaterInit fails, `cairns.render.frame` is `unknown_op`.

## Known deferrals (acknowledged, not bugs)

Moved to `TODO.md`. README carries architecture, not work items.

---

## Editor extensions (imgui stack)

The editor is built on Dear ImGui (docking branch). Sub-systems are
extensions chosen per task:

| Editor task | Library | Status |
|---|---|---|
| Node graph (ComfyUI core) | **imgui-node-editor** (thedmd) | ✅ pick — keep it; `imnodes` is the lighter fallback |
| Open/save files | **ImGuiFileDialog** (aiekick) | ✅ pick — fine; `portable-file-dialogs` for native OS dialogs |
| Property / node-param panels | **roll your own over EnTT components** | ⚠️ replaces "imgui_inspect" (that one's Rust); no good C++ drop-in |
| Inspect images / latents / ControlNet maps | **imgui_tex_inspect** (andyborrell) and/or **ImmVision** (pthom) | ➕ the C++ lib named like "imgui_inspect" — add it |
| 3D gizmos: move/rotate/scale, place camera entities | **ImGuizmo** (CedricGuillemet) | ➕ not listed but needed — manipulate objects, pose placed cameras |
| Plots: loss curves, histograms, throughput | **ImPlot** (epezent) | ➕ for the data-generator metrics/QA |
| Docking / panel layout | **Dear ImGui docking branch** | ℹ️ core, not an extension — this is the screen-space/UI camera's layout |
| Prompt / code text editing | core `InputTextMultiline`; **ImGuiColorTextEdit** or **Zep** for syntax highlighting | core covers prompts; code editor only if there are code nodes |
| Loading / progress spinners | **imspinner** | optional polish |
| Markdown (help/docs panels) | **imgui_md** | optional polish |
| Toggle switches | **imgui_toggle** | optional polish |
| Toast notifications ("generation complete") | **ImGuiNotify** | optional polish |
| Knob / dial widgets | **imgui-knobs** | optional polish |

---

## Building

### Selecting the GPU backend (macOS)

Generate the Xcode project for a specific backend with
`-DCAIRNS_GFX_BACKEND=metal|vulkan`:

```sh
cmake -B build/metal -G Xcode -DCAIRNS_GFX_BACKEND=metal    # Metal
cmake -B build/vk    -G Xcode -DCAIRNS_GFX_BACKEND=vulkan   # Vulkan (MoltenVK; run via ./run_vk.sh)
```

### SDL3 sample boilerplate

Inherited from the upstream SDL3 sample. Build and use SDL3, SDL_Mixer,
SDL_Image, and SDL_ttf from source using C++ and CMake, with macOS/iOS
bundle setup baked in. See [src/main.cpp](src/main.cpp).

Are you a complete beginner? Read
[this](https://github.com/Ravbug/sdl3-sample/wiki/Setting-up-your-computer).
Otherwise:

```sh
# clone with submodules, otherwise SDL will not download.
git clone https://github.com/Ravbug/sdl3-sample --depth=1 --recurse-submodules
cd sdl3-sample
cmake -S . -B build
```

Init scripts inside [`config/`](config/) handle platform-specific generator
choice. Then open the IDE project inside `build/` and run.

### Supported platforms

| Platform | Architecture | Generator |
| --- | --- | --- |
| macOS | x86_64, arm64 | Xcode |
| iOS | x86_64, arm64 | Xcode |
| tvOS | x86_64, arm64 | Xcode |
| visionOS* | arm64 | Xcode |
| Windows | x86_64, arm64 | Visual Studio |
| Linux | x86_64, arm64 | Ninja, Make |
| Web* | wasm | Ninja, Make |
| Android* | x86, x64, arm, arm64 | Ninja via Android Studio |

*See further instructions in [`config/`](config/).

Note: UWP support was [removed from SDL3](https://github.com/libsdl-org/SDL/pull/10731)
during its development. For historical reasons, a working UWP sample exists
at [df270da](https://github.com/Ravbug/sdl3-sample/tree/df270daa8d6d48426e128e50c73357dfdf89afbf).

### Updating SDL

Just update the submodule:

```sh
cd third_party/SDL && git pull && cd -
cd third_party/SDL_ttf && git pull && cd -
```

You don't need to use a submodule, you can also copy the source in directly.
This repository uses a submodule to keep its size to a minimum.

### Reporting issues

Create an Issue or send a Pull Request on this repository.
