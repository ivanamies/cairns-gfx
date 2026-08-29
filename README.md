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
   Per-slot `RenderGraph` + draw-list containers so producer and consumer
   never share mutable state.
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
   FrameClock::Tick → sim step                   ↳ semaphore_wait + AdvanceFrame
   Extract → proxies (FrameArena)              Build per-draw UBOs from pkt
   BuildMeshOpaqueDraws → drawList             (rhi_.alloc.BumpAllocate ×N)
   Build RenderGraph (AddPass + Bake)          graph_.Execute(fc, swapchain_)
   FramePacket pkt{...}                          ↳ AcquireSwapchain inside composite
   queue.Push(pkt)                             Frames::End → submit + present
   queue.WaitIfFull(kFramesInFlight)           if pkt.dump_frame: do dump + signal
```

**Ownership rules:**
- Game thread: SDL, ImGui, sim, Extract, BuildMeshOpaqueDraws, render-graph
  build. No GPU bump-alloc, no `Frames::*`.
- Render thread: per-draw UBO bumps (one allocator on one thread), graph
  Execute, `Frames::Begin/End`. No sim, no ImGui, no resource
  `Acquire`/`Release`.
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

## Known deferrals (acknowledged, not bugs)

- **EnTT internals still on `std::allocator`.** `entt::registry`'s
  component pages go through `std::allocator<T>`, not `cairns::Allocator`.
  Routing through `cairns::basic_registry<entt::entity, cairns::Allocator<entt::entity>>`
  is mechanical surgery (every `view<T>` / `storage<T>` call site has to
  carry the allocator type), but right now `cairns::Arena::kEnabled = false`
  means the Arena just `malloc`s anyway — so the surgery would route
  through a wrapper that calls `malloc` instead of routing through
  `malloc` directly. Revisit when `kEnabled` flips to true and the
  ChunkAllocator path is live.
- **`World::Cold::registry` is held by value.** Dropping the prior
  `std::unique_ptr<entt::registry>` saves one heap alloc per world open
  (~8 across the program). Safe because `worlds_.Reserve(kMaxWorlds)`
  guarantees the cold_ vector never reallocates, so `World::Cold*` stays
  stable for the engine's lifetime.

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
