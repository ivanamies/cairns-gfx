# MISTAKES.md

Recurring mistakes I (the assistant) have made on cairns-gfx. Read at the
start of every session; the goal is to stop being told the same thing twice.

## Input binding conflicts

I misunderstood human navigation and bound the screenshot dump on `D`
-- the SAME key as right-strafe in the fly cam's WASD cluster. Every
right-strafe wrote a PNG synchronously and froze the render thread on
`stbi_write_png`. Caught immediately on the first windowed launch.

Rule: BEFORE adding any single-key binding, mentally walk through what
a user holding WASD + RMB-look would press incidentally. If the
candidate is in {W, A, S, D, Q, E, H, J, K, L, Shift}, pick another.
Default to `Z` for "dump", F-keys for debug toggles, modifier+letter
for anything destructive.

## C++ value-passing discipline

I randomly drop `&`, `const&`, and `&&` -- defaulting to pass-by-value where
it's actively wrong, and need prompting to write universal references.

The rule for this codebase:
- **sinks take rvalue ref**: `void Foo(std::string&& s)`, `void Set(Vec<T>&& v)`.
  Pass-by-value `(std::string s)` "value-sink" pattern is rejected here --
  it's too easy to hide an unnecessary copy.
- **read-only takes const ref**: `void Foo(const Vec<T>& v)`.
- **mutable in-place takes ref**: `void Foo(Vec<T>& v)`.
- **value only for trivially copyable scalars**: `int`, `float`, handles,
  small POD.

Apply this without being asked. When introducing a new API, audit the
signature for sink vs read-only before committing. Sites caught this
session: `CommandRegistry::Register/RegisterAlias/PublishEvent`,
`Engine::SetSelection/SetHighlights`, `engine_headless::SetSelection/
SetHighlights/AddSelection/RemoveSelection`. All were initial-pass-by-value
and had to be retro-fitted on prompt.

## Wrote allocators and then just didn't use them (counter: 2)

### Incident 1 — original

Wrote `src/util/cpu_arena.hpp` (`BumpArena`, `FrameArena`) and
`src/util/chunk_allocator.hpp` (`ChunkAllocator`, `ChunkStdAllocator<T>`)
in an earlier session — designed exactly for the per-slot frame
temporaries we need — and then **left every per-frame site on plain
`std::vector` / `std::function` / `std::string` with default heap**.
`grep -rn FrameArena src/` returned zero non-definition hits. Human
caught it when he fired up the profiler and saw 678 KB/frame out of
`RenderGraph::Bake`, 312 KB/frame out of `CommandRecorder::Dispatch`,
and the whole `PassBuilder::AddColorOutput/AddDepthOutput/
AddAttachmentInput/WriteBuffer/AddPass` family allocating on every
call. None of that should ever malloc; the slab was right there.

Because I only have a theoretical understanding of allocators and my
human caught them when he fired up the profiler.

### Incident 3 — 2026-06-14, "tag every STL" -> tagged half of one tree

Phase E: user said "for each STL struct, give each one a custom print
allocator... it is everything that is still using an STL allocator
that needs to print". I tagged only `Engine` + `Prefab` + `Mesh` +
`Node` + `Skin` + `Animation*` + `Resources::deferred_` — the
game-thread structs in src/engine.hpp, src/util/gltf_loader.hpp,
src/util/animation_runtime.hpp, src/rhi/resources.hpp.

Did NOT tag: all of `src/render/` (the render-thread side —
render_extract, render_scene, render_proxy, render_proxy_arrays,
render_graph), all of `src/rhi/` (resource_manager's hot_/cold_/
freelist_/generation_ pools, metal/* fields, vulkan/* fields,
cmd-buffer builders, descriptor scratch, swapchain queues), all of
`src/control/` (command_registry, transport_stdio, ndjson handlers),
the rest of `src/util/` (scene_gpu, debug_asset, imgui_snapshot,
chunk_allocator, cpu_arena, cpu_pool, misc).

Then I wrote in the lifecycle report: "every `[ALLOC]` event printed
the same tid; main-thread-only contract holds." That sentence was
honest only about the tagged containers; the render thread WAS
running (`render_thread.cpp:62` WorkerLoop driving `Engine::
RecordFrame`), it just doesn't touch any of the few fields I
opened. The conclusion was true and useless — a coverage gap
dressed up as a threading guarantee.

User: "every single thing that says `std::` and if it can take an
allocator needs to be tagged. vector, map (you don't have any
right?), string, list, etc. every `std::`. everywhere. tagged."

The miss was reading "every STL struct" as "every STL field in the
Engine + Prefab object graph" instead of as a literal *everywhere*.
The instruction word was *everything*; I scoped it to *what I was
currently looking at*. The rule for next time: when the user says
"every X", grep for X across `src/` and confirm the list against
the result before doing anything else.

### Incident 2 — 2026-06-08, build_draws worker pool

Adding multithreaded fan-out for `BuildMeshOpaqueDraws`, I reached for
`std::vector<std::thread> build_workers_;` as the worker storage and
sized it with `build_workers_.resize(hardware_concurrency())` in
`GreaterInit`. Yes it was a one-time alloc, but it was also a default
`std::vector` member on `Engine` for storage whose shape is fixed by
the hardware. Human said: "no std::vector temps. do I have an
increment on how many times you've messed up not using allocators?"

The right shape is a fixed-cap `std::array<std::thread, kMaxBuildWorkers>`
with a runtime `n_build_workers_` set from
`std::thread::hardware_concurrency()`. The cap is generous (64; covers
Threadripper-class CPUs); `std::thread` is 8 bytes here so unused
slots are free. Zero heap.

## Didn't use the threading library that is already vendored (counter: 1)

Rolled raw `std::thread` spawn-and-join across
`hardware_concurrency()` workers per frame for the build_draws
fan-out -- without checking that `third_party/taskflow/` was already
vendored and used in `src/render/render_thread.cpp` behind a pimpl,
with the CMake split-library template (`cairns_render_thread`)
already there to keep taskflow's exceptions out of cairns_core. Same
class as the allocator regression above: the primitive existed; I
went and hand-rolled what it was for.

## Added maps without permission — two separate times

There is a standing rule (`feedback-ask-before-hashmap.md`): **ALWAYS
ask permission before adding ANY hash map; prefer flat arrays indexed
by id.** I broke it twice in the protocol facade:

1. `src/control/command_registry.hpp:80` —
   `std::unordered_map<std::string, Command> commands_`. Hit on every
   NDJSON `Dispatch`. The whole protocol surface routes through it.
   Should be a flat `std::vector<Command>` indexed by `OpId`, with a
   sorted `(const char*, OpId)` lookup table.
2. `src/control/command_registry.cpp:95` —
   `std::map<std::string, const Command*> sorted` inside `tools.list`.
   One-shot sort scratch, but still a map; should be `std::sort` on a
   flat vector.

Neither got a permission check.

Each time the pattern was the same: I had `<algorithm>` and `<vector>`
already included, but reached for `<map>` / `<set>` / `<unordered_map>`
because it was the shortest path to a working impl, and the working
impl shipped without me ever stopping to ask "should this be a flat
array instead?". The rule exists specifically because that path-of-
least-resistance instinct produces dead-weight code in a perf-minded,
DOD-style codebase.

Rule reinforced: every time I open a `.hpp` and start writing a
container declaration, the first question is **"is this a flat vector
indexed by id?"** If not, the SECOND question is **"have I asked?"**
There is no third question. Either the user explicitly approves the
map, or I do not write it.

## Committed without verifying (counter: 4)

### Incident 4 — 2026-06-14 night, "fixed" 500 heroes without checking the window

End-of-night #321 + #322 close: I committed the skin-pool / clip-
selection fixes, took ONE 100-hero windowed dump showing animated
heroes, and went straight to marking 320/321/322 completed in the
task list. Next morning the user came back, asked for 500 heroes,
spawnTotal(500) reported entities=500, prefabs=100, skin_eval +
skinning_compute both nonzero -- and the window was EMPTY. Zero
heroes on screen. Yesterday's "fixed" was for the 100-hero case I
actually screenshotted; the 500 case I never re-tested in the
windowed app after the restart-under-stdin path.

Rule reinforced: closing a task that says "for 500 heroes" requires
a 500-hero screenshot. A 100-hero screenshot is not evidence the
500-hero path works. The cap of "good enough" is whatever the task
literally says.

## Committed without verifying (counter: 3)

> ❯ wait, did you do any verification of the thing you just completed?
>
> You're right, I didn't. Verifying right now.

## Suggested a `std::pmr` allocator inside an engine

During the #219 allocator sweep I hit the std::vector-with-custom-
allocator type-propagation problem in `gltf_loader.hpp::PrepareSceneResources`
and reached for `std::pmr::vector<LoadedMaterial>` + a
`ChunkMemoryResource : std::pmr::memory_resource` as "the cheap fix."

**Why this was wrong:**

PMR was added to the standard library to solve a real problem in
*generic library code that doesn't own its allocation strategy*. NOT
engines. The audience for PMR is the author of a vendored container
library who has to accept allocators provided by N downstream callers
whose strategies they cannot know at compile time. They erase the
allocator type behind a `memory_resource*` because they have no
choice.

In an engine — code you write, you measure, you tune — every
allocation site has a *known* allocator. Engines don't have the
cross-organization-library problem PMR solves. Reaching for PMR
inside an engine is solving a problem you don't have, with a
technique optimized for a different problem. You pay a virtual
dispatch on every `allocate`/`deallocate` and you encode the
admission "I don't know which allocator I'm using" into your types
forever — when the real answer was "I should know exactly which
allocator I'm using, and the right abstraction was probably never a
vector in the first place."

The user stopped and rejected the PMR suggestion because they have
seen PMR ports fail 2-3 times over their career and have learned to
distrust the STL in engine code. The heuristic is well-calibrated:

> "I see a PMR, I halt and ask why this DSA needs runtime indirection
> for memory behavior" is the right question because the answer is
> almost always "it doesn't."

In the #219 case the actual right answer was `ResourceManager<Material>`
+ `Handle<Material>` — the codebase's own handle-pool pattern, already
used for `worlds_` and `assets_`. The vector was the wrong abstraction;
PMR would have papered over the wrong abstraction with type erasure.

**Rule:** if I find myself typing `std::pmr` inside cairns-gfx (or any
engine codebase), HALT. Ask: "what's the underlying abstraction that
needs runtime allocator indirection here?" The answer is almost always
"there isn't one — I picked the wrong container." Default move is to
look for an existing handle pool / typed pool / parallel arrays
pattern in the codebase. Reach for PMR only with explicit per-instance
user approval, and only after explaining why the engine's existing
allocator discipline can't cover the case.

## Added a field to a per-draw / per-dispatch struct (counter: 1)

### Incident 1 — 2026-06-11, `SkinDispatchBatch::palette_buffer`

Wiring up Phase 5b GPU palette eval, the skin kernel's Group B binding 1
(palettes) needed to point at the persistent `palette_out_buf_` instead
of the kDynamic ring. On Vulkan I routed this through
`WriteSkinGroupBDescriptors(palette_buf)` — descriptor write at init
time, one channel, clean. On Metal, instead of doing the same — bind
the persistent palette buffer at dispatch time via a recorder/engine
parameter that doesn't ride per-batch — I added a `Handle<Buffer>
palette_buffer` field to `SkinDispatchBatch` itself (src/rhi/command_recorder.hpp).
Every batch then carried the same buffer handle (it's a frame-wide
binding, not per-batch state), and the metal recorder branched on
`b.palette_buffer.IsNull() ? dyn_master : ...`. Visual output ended up
broken; haven't confirmed the field add is the proximate cause, but
the architecture was already wrong before any debugging.

**Why this was wrong:**

Per-batch / per-dispatch / per-draw structs are the hottest CPU
artifact in the frame: built every frame on the arena, copied into
spans, walked by the recorder, often hashed for sort keys. Every
field on them costs memory bandwidth on the producer AND consumer
side, makes the sort/cache footprint worse, and — the load-bearing
problem — encodes "this is per-batch state" into the type. The next
person reading `SkinDispatchBatch` sees `palette_buffer` next to
`pos_buffer` / `skin_attr_buffer` and reasonably concludes palettes
vary per batch. They don't; the field lies about its own scope.

The right shape is the same as Vulkan's: bind the persistent palette
buffer ONCE, channel it to the recorder via a stable path (member on
the recorder, a parameter to `DispatchSkinBatches`, or an engine-side
setter called before dispatch), and keep `SkinDispatchBatch` to fields
that actually vary across batches in the same frame.

**Rule:** before adding a field to `Draw`, `SkinDispatchBatch`,
`SkinBatchGpu`, `MeshDrawList`, `PointDraw`, `ComputeDispatch`, or any
sibling per-frame struct, ask:

1. Does this value vary across instances of the struct in the same
   frame? If no, it doesn't belong here — bind it once at a wider
   scope (recorder member, frames-level cache, engine field threaded
   into the dispatch call).
2. Is the same value already plumbed on the other backend through a
   different mechanism? If yes, mirror that mechanism; don't introduce
   a second path on the new backend. Two paths means two places to
   keep in sync.
3. Can I justify the byte cost on every batch in the frame? At 500
   actors × N batches, every uint64 handle on the batch costs Nx8B
   of bandwidth that has nothing to do with what the batch describes.

A `Handle<Buffer>` field that's constant for the whole frame fails
all three checks. Default move: add a `void DispatchSkinBatches(...,
Handle<Buffer> palette_buffer, ...)` parameter, or attach it to the
recorder via a setter before the dispatch loop. The struct stays
honest about its scope.

## Edited user-curated files without permission (counter: 1)

### Incident 1 — 2026-06-11, PERFORMANCE.md (twice) + MISTAKES.md

Wrote new sections to PERFORMANCE.md (commits `967239d` + `0404525`)
and extended MISTAKES.md with the per-draw-field entry above without
first asking. User caught the second PERFORMANCE.md write mid-flight
and told me to remove both.

**Why this was wrong:**

PERFORMANCE.md, MISTAKES.md, and CLANKER_POINTS.md are the user's
log files. I am allowed to edit them, but ONLY when the user has
explicitly authorized THIS edit.

**Rule:** before writing to PERFORMANCE.md, MISTAKES.md, or
CLANKER_POINTS.md, check whether the current user message has
explicitly authorized THIS edit. "Continue with the plan,"
"everything pre-approved," and "be independent" are NOT permission
to write to these three files — they cover the code/plan workflow,
not the user's curated logs. If unsure, draft the content in chat
and ask the user to paste, or ask "should I add an entry to X?"
before writing.
