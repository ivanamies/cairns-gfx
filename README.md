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
`~/dev/plans/2026-06-05_gfx_headless-editor-requirements.md`.

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

## Canonical implementation order

The order each subsystem MUST be brought up in, regardless of branch / fork /
rebase / cherry-pick churn. Each later layer depends on the earlier ones being
solid; do not skip ahead.

1. **RHI** — device, swapchain, command recorder, frames, single forward pass.
2. **Allocators** — A: bump arena (CPU). B: chunk allocator. C: range pool /
   OffsetAllocator. D: `ResourceManager<T>` + Hot/Cold. GPU side: single heap
   + single master buffer per memory type, bump ring for upload/dynamic.
   Batched upload (10 GLBs / batch) baked into `LoadScenesGpu` from day one.
3. **Profiling** — Timer slots (`frame`, `build_draws`, `record`, etc.),
   `PrintReport` every 120 frames, cpu-ms history ring. GPU completion-handler
   timer slot on Metal.
4. **ImGui** — vendor imgui core + impl_sdl3, RHI-routed renderer (own pipeline
   + per-frame bump upload of vtx/idx + per-cmd scissor + font atlas as
   `CreateTexture`). FPS text + cpu-ms PlotLines overlay. Drawn at the end of
   the swap pass — no separate composite pass required.
5. **Threading** — game / render thread split with `kFramesInFlight = 2`.
   SPSC handoff via `std::mutex` + `std::condition_variable` ONLY (no
   semaphores, latches, barriers, `shared_mutex`, atomic wait/notify).
   Per-slot draw-list + arena containers so producer and consumer never
   share mutable state. A single shared `RenderGraph` instance is reused
   each frame via `BindSlotArena(slot, ...)` — built + baked + executed on
   the render thread inside `RecordFrame`. See "Deferred refactors" below.
6. **RenderGraph / RenderProxies** — multi-pass graph (compute / forward /
   composite / depth-PIP / ui), Extract from `entt::registry` →
   `RenderProxyArrays`, draw build off proxies. Transient resource aliasing
   via `OffsetAllocator` over baked lifetimes.
7. **Scene layer** — `Handle<World>` pool, EnTT components
   (`WorldTransform` / `AssetRef` / `Renderable` / `Skin` / `Parent` /
   `DirtyTransform` / `Name`), `AssetRegistry` for dedup + suballoc + refcount,
   per-view `Camera` + persistent imported target.
8. **Animation** — skinning. Persistent skin output pool (`RangePool` over a
   GPU buffer), per-instance joint palette via `FrameArena`, compute-pass
   deformation, `skin_output` bound as a vertex buffer to the existing mesh
   pipeline. (Tier 1 load-time joint inverse-binds in `BumpArena`; Tier 2
   persistent skin output in `RangePool`; Tier 3 per-frame palettes in
   `FrameArena`.)
9. **Physics** — broadphase + narrowphase + solver. CPU first; parallelism
   via the threading carve from step 5. Hot loops on flat
   arrays-of-structs-of-arrays in `BumpArena`.
10. **Scripting** — embedded VM for gameplay code. Calls into the systems
    above via thin C wrappers; no direct RHI/scene mutation from script
    threads.

Out of order = fragile. New subsystem? Find where it slots in vs this list,
and only land it after everything below it (lower number) is healthy.

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
complete* barrier/semaphore generation. Our copy stopped at scheduling and never
ported the synchronization model, so the backends do ad-hoc barriers with gaps —
the `three_champ_static` golden flake (FLAKY_TESTS #2) is a cross-frame
`final_target_` write-after-write that nothing barriers. **Rule: copy Granite,
do not re-invent.** Point-by-point:

**The core stray — barriers live in the wrong place, with no persistent
per-resource state:**

- **Granite computes barriers in `bake()`; we compute none.** Granite emits, per
  physical pass, `invalidate` (before) + `flush` (after) `Barrier` lists. We
  emit nothing: vk does ad-hoc `transition()` on a layout change only; metal a
  `compute_fence_` for compute→graphics only. No graph-level barrier pass exists.
- **Granite keeps `PipelineEvent` per physical resource, PERSISTENT across
  frames; we keep almost nothing.** Granite's `physical_events[]` holds
  `{layout, to_flush_access, invalidated_in_stage[64], src_stages, wait
  semaphores}` and survives `bake()` rebuilds, so a resource's first use in
  frame N+1 invalidates against frame N's flush. We rebuild per frame and
  persist only vk's `vk_layout` (no flush-access, no invalidate tracking) →
  cross-frame WAW on `final_target_` is invisible. **This is the flake.**
- **Invalidate/flush models RAW + WAW + WAR; we model (at most) layout-change
  RAW.** Granite: inputs → invalidate (RAW vs `to_flush_access`); outputs →
  flush (record writes); read-only resources get a "fake flush, access=0" to
  catch WAR. vk's `transition()` skips same-layout transitions, so WAW (two
  writes, same layout — exactly `final_target_` frame-to-frame) and WAR are
  never barriered.
- **Buffers go through the same invalidate/flush in Granite; ours don't.** We
  track `buf_reads`/`buf_writers` for *scheduling* but emit no buffer barriers.

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

**Parity plan (in order):** port `PipelineEvent` (persistent `physical_events[]`)
+ the `Barrier`/invalidate/flush model into `bake()`; have both backends
*execute* the computed barriers (vk: `vkCmdPipelineBarrier`; metal: the same
edges as `MTLFence`/`memoryBarrier`) instead of ad-hoc ones. That kills the flake
and makes the backends behave identically. Then layer the perf items (per-stage
scoping, events, reorder, subpass merge, async compute). See TODO "Render-graph
gaps vs Granite".

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

## Style / conventions

A growing set of small conventions that aren't worth a Mistake but are worth
not re-litigating in PR review.

### We are Aaltonen-pilled and Acton-pilled

Two patron saints, two rules:

- **Aaltonen (handle-pilled).** Sebastian Aaltonen's resource-management
  designs: typed `Handle<T>` end-to-end, OffsetAllocator over fixed buffers,
  one giant heap + one master buffer per memory type, bump rings for
  per-frame UBOs. Indirection is by integer index into dense pools, not by
  pointer. The handle IS the contract — don't drop it to "save an int" or
  collapse it to a raw pointer at the last second.

- **Acton (array-pilled).** Mike Acton's DOD: flat
  arrays-of-structs-of-arrays, hot loops iterate contiguous memory, no
  hidden allocations, no virtual dispatch on the hot path, no hash maps
  where a flat array indexed by id fits. The data is the program.

When in doubt: handle-then-index, array-then-element.

### No pimpl in this repo

When a header needs different members on Metal vs Vulkan, use the
SwapChain-style `#if CAIRNS_METAL ... #elif CAIRNS_VULKAN ...` access
contract directly in the header. No opaque `void* impl_`, no separate
`_noop.cpp` TU, no virtual call, no extra heap allocation. The ugliness
is the point — it's the same ugliness that lets the hot path be one
struct field deref instead of a vtable + indirect call.

> "I'm ugly and I'm proud." — SpongeBob

### Anti-singleton (scene layer)

- **No "the world."** No global/static/singleton world, registry, camera,
  or selection.
- **No implicit "current scene."** Every public op takes an explicit
  `WorldId` or `EntityRef`.
- **Never pass a bare `entt::entity` across a boundary.** The cross-boundary
  reference is `EntityRef = { WorldId, entt::entity }`. A bare `entt::entity`
  lives only inside a scope that has already resolved its world.
  Code-review rule: a bare `entt::entity` in a signature, member, or
  container is a defect.
- **`active_world` is UI focus ONLY** — never read by core logic.

---

## Mistakes

A running record, from the user, of things I broke or never finished. Read
before touching adjacent code so they don't get re-introduced.

- **Didn't propagate allocators.** Randomly over-allocated `MTL::Heap`s and
  `MTL::Buffer`s by copying tutorial-style "block per (memory type, slot)"
  shapes instead of finishing the migration to the cairns allocator
  architecture. Result: one heap per slot per memory type when the design
  calls for **exactly one heap + one master buffer for the whole bump path**.
- **Deleted `Handle<BindGroup>` and `Handle<DynamicOffsets>` to "save time."**
  The hot path looked like just an integer at the moment the RHI was stood
  up, so I dropped the typed handles. That's wrong — the typed handles are
  the contract; the hot-path integer shape is a representation detail.
- **Added an `std::counting_semaphore` for no good reason.** The project
  explicitly restricts threading primitives to `std::thread`, `std::mutex`,
  `std::condition_variable`. See `feedback-threading-primitives.md`. I keep
  reaching for newer primitives by reflex; stop.
- **Mangled `Draw` struct packing by adding random padding bytes** for
  alignment/comment reasons that didn't survive review. Don't add or move
  fields without an actual reason.
- **Broke bind-group split-by-frequency** by collapsing into a single giant
  descriptor set (tutorial-style again). Set-0 / set-1 / set-2 / set-3 by
  update frequency is the design (RenderPassGlobals / Material /
  ShaderSpecific / DynamicOffsets); merging defeats the entire reason
  bind groups are split.
- **Keep talking about base-instance and push-constants.** Base-instance is
  proven not to work in our pipeline. Push constants are emulated with UBOs.
  Stop suggesting either; both are explicitly deferred levers.
  (`feedback-no-instancing-pushconstants.md` covers this too.)
- **Don't continue the "handle-pilled" pattern.** During the allocator work
  the rule is **`Handle<T>` first** — bind/use handles end-to-end, resolve
  to pointer/index only as a local at the lowest level. I keep introducing
  raw pointers / indices in places where a typed handle was the established
  shape.
- **Added an `std::unordered_map` to `AssetRegistry::by_key_` without
  asking.** Violated `feedback-ask-before-hashmap`. Replaced with sorted
  `std::vector<KeyEntry>` + `std::lower_bound` binary search. Lesson:
  ALWAYS ask before introducing a hash map. Even when "the keyspace is
  small" — that's exactly when the flat-array win is biggest.

## Bisecting a vk regression on a moving op-surface (the L9-era hell)

The vk wedge bisect (9737baa..ia/dev) **cost a session** because the NDJSON
op surface mutates inside the bisect range. Every time you jump commits
the spawn/instantiate plumbing changes shape — `prefab.loadBatch` doesn't
exist before `26a1af7`, `scene.instantiateGrid` is a stub at `f8af786`,
the engine's auto-spawn from `CAIRNS_N`/`CAIRNS_GLB` is killed at
`bd617d3`. Repeat **before** you start the Android re-bisect.

### The five op-surface eras (newest first)

| commit range                             | how to spawn N actors                                              | notes                                                                                                       |
|------------------------------------------|--------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------|
| `73ce3c4`..`HEAD` (bundled run.js)       | edit `assets/run.js` then rebuild — boot runs it                  | mandatory asset; `boot_run.cpp` aborts if missing; identical content on every platform                       |
| `26a1af7`..`73ce3c4~1`                   | `prefab.loadBatch(cursor,count)` + N× `scene.instantiateGrid`     | both ops real; `instantiate` returns `UINT32_MAX` on fail (post-`bc826b0`)                                  |
| `bc826b0`..`26a1af7~1`                   | same ops but textures render as white silhouettes                | bc826b0 fixes the L9 strands; 26a1af7 (L10b) builds material set2 inside LoadPrefabBatch                    |
| `bd617d3`..`bc826b0~1` (3 commits, **untestable**) | `prefab.loadBatch` works but `InstantiatePrefab` silently returns entity=0 | per_prefab_asset_ / active_scene_ / resident_textures_ stranded by L9; skip these commits in the bisect      |
| `e9ac791`..`bd617d3~1`                   | engine auto-loads 100 GLBs at boot → drive entity spawn via `scene.clear` + N× `scene.instantiate(prefab,x,y,z,scale,time_phase)` | `instantiateGrid` is a STUB (returns synthetic id only); `prefab.load*` are STUBS too                       |
| ..`e9ac791~1` (pre-#224 L4)              | engine auto-spawns from `CAIRNS_N` + `CAIRNS_GLB` env vars        | no NDJSON ops; `kHeroSlices=33` default-multiplier; **`9737baa` was last known-good S22 vk row** in PERFORMANCE.md |

### Driving each era

```jsonc
// era 5 (HEAD, run.js): edit assets/run.js, rebuild, launch.
// Same NDJSON-ish shape but via cairns.dispatch in JS:
cairns.dispatch("cairns.prefab.loadBatch", { cursor: 0, count: 100 });
for (let k = 0; k < 5; ++k) {
    cairns.dispatch("cairns.scene.instantiateGrid",
                    { first_prefab_idx: 0, prefab_count: 100 });
}
```

```ndjson
// era 4 (post-26a1af7): pipe to sdl-min via CAIRNS_AGENT_STDIN=1, or to
// cairns_serve via stdin.
{"op":"cairns.scene.clear"}
{"op":"cairns.prefab.loadBatch","args":{"cursor":0,"count":9}}
{"op":"cairns.scene.instantiateGrid","args":{"first_prefab_idx":0,"prefab_count":9}}
```

```ndjson
// era 2 (e9ac791..bd617d3~1): the engine auto-loaded 100 GLBs at boot. To
// spawn 9 explicitly:
{"op":"cairns.scene.clear"}
{"op":"cairns.scene.instantiate","args":{"prefab":0,"x":-1.6,"y":-1.6,"z":-4.0,"scale":0.005,"time_phase":0.0}}
{"op":"cairns.scene.instantiate","args":{"prefab":1,"x":0.0, "y":-1.6,"z":-4.0,"scale":0.005,"time_phase":0.137}}
// ... 7 more, prefab indices 2..8
```

```sh
# era 1 (..e9ac791): no NDJSON. Engine spawns from env vars at GreaterInit.
CAIRNS_N=500 ./sdl-min                                                     # default 100 GLBs, 500 actors
CAIRNS_N=9 CAIRNS_GLB="aatrox.glb,aatrox_blood_moon.glb,…" ./sdl-min        # 9 distinct GLBs, 9 actors
```

### Build + run loops

**Desktop vk Release** (always pass `-DCAIRNS_GFX_BACKEND=vulkan` — the
cmake default is metal and the silent-pick has burned hours):

```sh
cmake -S . -B build/vk -G Xcode -DCAIRNS_GFX_BACKEND=vulkan
cmake --build build/vk --target sdl-min --config Release -j8

CAIRNS_AGENT_STDIN=1 \
  ./build/vk/Release/sdl-min.app/Contents/MacOS/sdl-min \
  < driver.ndjson 2>tmp/stderr.log &

# wait, check pid, eyeball window
pgrep -af sdl-min
grep -E '\[Timer\] slot|draws |entities=' tmp/stderr.log | tail -20
```

For headless dump-and-compare runs (faster bisect iteration than the
windowed app): append `cairns.render.frame` ×4 + `cairns.io.dumpTexture`
+ `cairns.app.quit` to the driver and pipe into `cairns_serve` instead
of `sdl-min`.

**Android (S22) vk Release** — Android needs the windowed app
(`sdl-min` AAR is what gets built). No NDJSON transport, so eras 4/5 need
the NDJSON to come from `assets/run.js`; era 1 uses env-var auto-spawn
in `main.cpp`'s `__ANDROID__` block.

```sh
# Build + install. Gradle drives cmake; the cmake POST_BUILD copies
# assets/run.js into the APK assets dir. If you only changed run.js,
# force the copy:
cp assets/run.js third_party/SDL/android-project/app/src/main/assets/run.js

cd third_party/SDL/android-project
./gradlew :app:assembleRelease
adb -s <serial> install -r app/build/outputs/apk/release/app-release.apk

# Run + wait + capture.
adb -s <serial> logcat -c
adb -s <serial> shell am force-stop org.libsdl.app
adb -s <serial> shell am start -n org.libsdl.app/.SDLActivity

# Wait ~30s for steady state, then either eyeball the device or pull:
adb -s <serial> logcat -d | grep -E 'cairns|\[Timer\] slot|draws ' | tail -40

# Sanity-check process still alive (silent OOM-kill if not):
adb -s <serial> shell pidof org.libsdl.app
```

### Per-commit verification signal

The stderr / logcat line you grep for after each bisect step:

```
draws 1700 | 100 GLBs x 5 slices = 500 entities | resolution 1280 x 720
[Timer] slot 0 (frame): accum NNN us, avg NNN us over 120 frames
[Timer] slot 3 (skinning_compute): accum NNN us, avg NNN us over 120 frames
```

For the visual signal (wedge vs clean) on desktop you can `screencapture
-x tmp/shot.png`. On Android, eyeball the device or use `adb exec-out
screencap -p > tmp/shot.png`.

### Skipping the untestable strands

`bd617d3`..`bc826b0~1` is three commits where `cairns.scene.instantiate`
silently returns entity:0 because L9 stranded `per_prefab_asset_` /
`active_scene_` / `resident_textures_`. Don't try to verify them
individually; treat the whole block as "the bug is at one end of the
block, you can't tell which without surgery." Test `e9ac791` (good
side) and `bc826b0` (bad side) and conclude the regression is in one
of `{bd617d3, 64e1de0, 7cbac96, bc826b0}` — only `bd617d3` and
`bc826b0` have code changes (the other two are golden rebake + script
add), so the culprit is one of those two. The fix sat at the closer
match — once `bc826b0` made instantiate work, the wedge was already
present, so the introducing commit was `bd617d3` (kill the boot
bulk-load) and the eventual fix was to recreate the stranded
descriptor sets after the first runtime `loadBatch` (commit `a273119`
on `ia/dev` adds `recreateSkinGroupB`; H4b's `recreateAnimDynBindings`
was the other half).

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
    subjects formerly called "the ladder" (triangle, one/two die, viking_room,
    three static/animated champions) are now flat `[subject]` SCENARIOs, plus
    the targeted subsystem scenarios (particles state-hash, hot reload, two
    scenes/two heroes, nested graph, frustum cull, imgui stability).
  - **`[stress]`** (`test_golden_stress.cpp`) — scale (100 distinct animated
    champions). Heavy; kept out of `[scenarios]` so the fast pass stays fast.
  Image refs live under `tests/refs/` as `{name}.f09.{platform}.imghash`
  (frame 9) and `.f55.imghash` (frame 55).

Plan and notes:
- `dev/plans/2026-06-18_gfx_test-tech-tree-redo-phase-a-to-g.md` — the plan
- `dev/plans/2026-06-18_gfx_modularization-notes.md` — generalization
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
./build/spec-mac-metal/Release/cairns_golden_tests "[subject]"    # former ladder rungs only
./build/spec-mac-metal/Release/cairns_golden_tests "[particles]"  # G1 state-hash alone

# Same shape for vk:
./build/spec-mac-vk/Release/cairns_golden_tests --reporter compact
```

### Tier G — iOS simulator

Assets bundle into `.app/Resources/` via the CMake glob.

```sh
cmake --build build/spec-ios-sim --target cairns_golden_tests -j
xcrun simctl spawn booted \
  ./build/spec-ios-sim/cairns_golden_tests.app/cairns_golden_tests \
  "[ladder]" --reporter compact
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
  LD_LIBRARY_PATH=. ./cairns_golden_tests '[ladder]' --reporter compact"

# Pull baked refs back into the repo
adb -s $DEV pull /data/local/tmp/cairns_test/refs/. tests/refs/
```

### Eyeball / headless PNG inspection

```sh
# Dump rendered PNGs alongside the hash check.
CAIRNS_DUMP_PNGS=/tmp/cairns-pngs \
  ./build/spec-mac-vk/cairns_golden_tests "[ladder]"

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

- `[scenarios]` full-suite run flakes the G1 and G6 image hashes
  intermittently (shared ImGui static state across SCENARIOs; tracked in
  modularization-notes #9b). Run `[particles]` and `[imgui]` separately
  for stable results.
- S22 device must be physically connected for the `android-vk` refs to
  be writable; otherwise only AVD (`android-vk-emu`) bakes.
- iOS sim runs require a booted simulator (`xcrun simctl boot <UDID>`).
- Desktop `[ladder]` runs from build dir (assets staged alongside the
  binary); `cd build/<dir>` first if running with a relative path.
- G5 frustum-cull counters SKIP until a production cull stage lands
  (engine does not call `cairns::AabbOutsideFrustum`); tracked in
  modularization-notes #10b.
- G1 buffer / G4 resolved-depth / L6+L7 skin-buffer SECTIONs SKIP until
  `Resources::ReadBackBuffer` lands on both backends (open task).

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
