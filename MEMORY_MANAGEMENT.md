# cairns-gfx memory management

Source of truth for **which struct/system gets which allocator**. Distilled from
`~/dev/plans/ALLOCATOR_HANDOFF.md`. When in doubt, match an existing row.

## Four allocator archetypes (one per lifetime shape)

| # | Allocator | Header | Owns | Currency | Use when |
|---|---|---|---|---|---|
| **A** | `cairns::BumpArena` / `FrameArena` | `src/util/cpu_arena.hpp` | a slab (not memory) | `uint32` offset | per-frame transient; reset every frame |
| **B** | `cairns::ChunkAllocator` | `src/util/chunk_allocator.hpp` | malloc-backed 4 MB chunks (size-class free lists) | `void*` (behind a handle/index elsewhere) | irregular size + irregular lifetime; the root CPU memory source |
| **C** | `cairns::RangePool` / `TypedPool<T>` | `src/util/cpu_pool.hpp` | offsets only (no memory) | offset (`PoolSlice`) | persistent, individual free, **variable-size ranges in one buffer** (typically a GPU buffer) |
| **D** | `cairns::ResourceManager<T>` | `src/rhi/resource_manager.hpp` (templated; usable from anywhere) | dense Hot/Cold SoA arrays | `Handle<T>` (generational) | persistent objects with individual create/destroy and **stable identity** |

**Layering** (read it back as a mental model):
- **B owns memory.**
- **A's slab is borrowed from B** (the engine's `FrameArena` slab is `hot_arena_.Allocate(N)`).
- **Load-time tier = a `BumpArena` whose slab is one B chunk**; freed wholesale at asset unload
  by returning the chunk to B.
- **C owns only offsets** — the backing buffer is whatever the caller passes (usually a GPU
  buffer created via the untouched GPU allocator).
- **D owns its own dense Hot/Cold arrays** (no B underneath — `ResourceManager<T>` allocates its
  own backing `std::vector`s).

GPU memory (`src/rhi/{vulkan,metal}/memory_allocator.*`, `src/rhi/allocator.*`) is a separate
concern and not touched by the CPU side. Palettes etc. still upload through
`rhi::Allocator::BumpAllocate(..., Memory::kDynamic)`.

## What goes where (data → allocator)

### A — per-frame transient (`FrameArena`; `BumpStdAllocator<T>` for STL containers)
Rebuilt every frame; valid only until the next `BeginFrame(frame_)` on this slot.
- `RenderPassGlobals`, `MaterialGpu`, `DrawTmp` (currently bumped into `Memory::kDynamic` — same
  lifetime, GPU side; CPU staging that touches it = A)
- Draw list + sorted draw keys (when rebuilt every frame)
- Render proxies (extract output)
- Joint palettes (CPU side) → uploaded into `kDynamic`
- `InstanceMeta` / `SkinMeshBatch` (per-frame skinning arrays)
- ImGui vertex/index/cmd buffers — immediate-mode, discarded at frame end
- Color-correction per-frame params → `kDynamic`
- `drawParallel`'s `e_all` (the "all entities" subset list — already wired)
- Physics solver scratch (contacts, islands, broadphase pairs) ⚠ data-dependent size + MT →
  needs growable + per-thread variant; that extension lives in `cpu_arena.hpp` when it lands

### B — general chunk allocator (`ChunkAllocator`; `Allocator<T>` STL adapter)
The root CPU memory source. Anything irregular in size or lifetime that isn't transient.
- The engine's `hot_arena_` (this IS a `ChunkAllocator` via the `Arena` typedef)
- `Scene::meshes` / `Scene::nodes` (long-lived once loaded)
- The engine's `scenes_`, `drawList_`, `drawListSorted_`, `root_nodes_stack_cache_` —
  `std::vector<T, cairns::Allocator<T>>` already routes here
- Animation clips (keyframe tracks) — load-time; per-asset `BumpArena` over a B chunk, returned
  wholesale on unload
- Skins (joint-index sets) — same
- Inverse-bind matrices — same
- Packed skin SSBO CPU staging — `Mark`/`Rewind` inside a load-time bump arena
- Scene mesh CPU temporaries (positions/attrs/indices) — dropped after `LoadScenesGpu`
- Scene hierarchy edge lists (`Node::children`)
- Strings (object names, file paths)
- Editor UI retained state, undo/redo buffers

### C — range / offset pool (`RangePool`; `TypedPool<T>` for CPU-backed)
Persistent, individual create/destroy, variable-size ranges in ONE backing buffer; the unit is
an offset (`PoolSlice`).
- `skin_output_pool_` — the canonical user (one persistent GPU buffer; per-entity skinned vertex
  ranges spawn/despawn individually; `s.offset * stride` is the bind offset)
- Future single-buffer sub-allocations of the same shape

### D — resource manager (`ResourceManager<T>`; `Handle<T>` generational)
Persistent objects with individual create/destroy and **stable identity** (so stale handles
are detected, not silently aliased).
- All GPU resources (existing): `Buffer`, `Texture`, `Sampler`, `BindGroup`, `DynamicBuffers`,
  `Shader`, `Kernel` (`src/rhi/resources.hpp`)
- `SceneEntity` (replaces `SceneWorld::entities` raw `std::vector`)
- `LightProxy` (replaces `ProxyArray<LightProxy>` whose free list has **no generation** — a
  stale index silently aliases a reused light)
- `Node` (so reparenting / selection survive deletion)
- `LoadedMaterial` (so materials can be edited / deleted at runtime in an editor)
- Future: physics bodies / colliders

The Hot/Cold split per type: per-draw / per-frame fields in **Hot** (transform, mesh handle,
flags); names, editor metadata, undo refs in **Cold**. The hot path walks the **packed live
list** of indices (updated on `Acquire`/`Release`), not the sparse Hot array.

## House rules

**One-line standard:** *store handles/indices/offsets; resolve to a pointer only as a local; the
only allocator that traffics in escaping pointers is B, and only behind a handle/index owned
elsewhere.*

**The reference gradient (best → worst):**
`Handle<T>` (generational, stale-detecting) → index (into a known array) → offset (into a known
buffer/slab) → raw pointer.

**Anti-patterns to reject in review:**
- Any struct that **stores** a `Node*`, `Entity*`, or `Clip*` obtained from B. Storing it is the
  use-after-free a handle system exists to prevent; resolving it transiently inside one function
  is fine.
- Re-introducing a `void*→metadata` side table on top of B. The 16 B header before each user
  pointer makes `Free` O(1) without one.
- Over-pilling the hot loop: resolving a generational handle per vertex / joint / proxy is
  self-sabotage. Handles live at boundaries and storage; the inner loop walks dense arrays
  directly (Hot/Cold SoA).

## File inventory

| File | Role |
|---|---|
| `src/util/chunk_allocator.hpp` | B — `ChunkAllocator` + `ChunkStdAllocator<T>` |
| `src/util/cpu_arena.hpp` | A — `BumpArena`, `FrameArena`, `BumpStdAllocator<T>` |
| `src/util/cpu_pool.hpp` | C — `PoolSlice`, `RangePool`, `TypedPool<T>` |
| `src/util/offset_allocator.hpp` | Sebastian Aaltonen's offset suballocator (C wraps it) |
| `src/util/std_allocator.hpp` | aliases (`Arena = ChunkAllocator`, `Allocator<T> = ChunkStdAllocator<T>`) for backward-compat call sites |
| `src/rhi/resource_manager.hpp` | D — generic `Handle<T>` + `ResourceManager<T>` (GPU types instantiated alongside; CPU types live in `src/scene/`, `src/render/`) |
| `src/rhi/{vulkan,metal}/memory_allocator.*` | GPU memory — **DO NOT TOUCH from CPU code** |
