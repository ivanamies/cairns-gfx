# PERFORMANCE_THROWAWAY.md

**Throwaway** working log for the #229 allocation-less rework. Separate from the
curated `PERFORMANCE.md` (do not edit that one). Delete this when #229 lands.

One section per milestone (M0..M7). After each, regenerate the receipts and
append. A milestone that regresses fps/cpu-time or fails to drop its targeted
allocation tag to ~0 is not done.

## Workload

- **Perf:** the boot scene is already **500 actors = 100 GLBs x 5 slices**
  (`assets/run.js`). Surfaceless `cairns_serve`, render 120 frames, read the
  `[Timer]` slots (= the imgui HUD numbers).
- **Alloc:** load 3 GLBs + instantiate + render + **reload 1** + clear +
  unloadAll, on a `-DCAIRNS_GFX_ALLOC_TRACE=ON` build. The engine prints
  `[ALLOC-RECEIPT]` deltas at the `[LOAD]`/`[RELOAD]` fences.

## Regenerate

```
# perf (real timing -- non-trace build)
scripts/_receipts.sh build/spec-mac-metal/Release/cairns_serve perf

# alloc receipts (trace build)
cmake -S . -B build/metal-alloctrace -G Ninja -DCAIRNS_GFX_BACKEND=metal \
  -DCAIRNS_GFX_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release -DCAIRNS_GFX_ALLOC_TRACE=ON
cmake --build build/metal-alloctrace --target cairns_serve -j
scripts/_receipts.sh build/metal-alloctrace/Release/cairns_serve alloc
```

**Known caveat:** `cairns_serve` exits 139 (SIGSEGV) at teardown on a heavy
scene. PRE-EXISTING and unrelated to #229 — the non-trace binary crashes
identically on the same workload, and it happens *after* all work + receipts
flush. Tracked separately; not a regression.

---

## M0 — baseline (commit `21c893a` + receipts infra)

Backend: macOS metal, surfaceless, Release. Captured 2026-06-19.

### Perf (500 actors, 120 frames)

| slot | avg | notes |
|---|---|---|
| frame | **6666 us** (~150 fps) | total |
| skinning_compute | **3921 us** | dominant — 500 skinned actors |
| forward_vp0 | 1276 us | |
| build_draws | 579 us | |
| record | 577 us | |
| swap | 163 us | |
| skin_eval | 35 us | |
| particle_sim | 65 us (over 46 frames) | |
| present_wait | 0 us | surfaceless |

### Alloc (the disaster, measured)

| phase | allocs | frees | bytes (churn) | ms |
|---|---|---|---|---|
| boot load (100 GLBs) | **5,400,016** | 406,260 | 12.25 GB | 3990 |
| load aatrox.glb (1) | 65,632 | 991 | 137 MB | 37 |
| load ahri.glb (1) | 80,389 | 10,461 | 155 MB | 50 |
| load aatrox_blood_moon.glb (1) | 68,016 | 1,044 | 156 MB | 48 |
| **reload aatrox.glb (1)** | **65,687** | **65,652** | 137 MB | 50 |

**Reads:** ~65–80k allocs per single-GLB load; reload of ONE prefab frees+allocs
~65k each (the "where there is one there are many" storm). Boot = 5.4M allocs /
12.25 GB churn. These are the M1/M3/M5 kill targets. `[ALLOC-RECEIPT]` is the
always-on total; `print_allocator` (already deployed on e.g.
`per_batch_shared_skin_`) is the per-tag attribution to confirm each kill.

_No-regression gate for later milestones:_ frame ≤ 6666 us, skinning_compute
≤ 3921 us, correctness suites green (spec 105/105, scenarios 96/97 [G6 #9b
pre-existing], stress 8/8, jsmoke 4/4).

---

## M1 — AnimationSampler flat read (commit pending)

Parse now stores glTF accessor handles only; the selected walk clip's keyframes
are read straight into the flat `gpu_times/gpu_values` at flatten time. The
per-sampler `vector<float> times` / `vector<vec4> values` heap pairs (one per
sampler, all clips) are gone, and unused clips cost zero keyframe reads. Dead
`SampleSampler`/`SampleClip` deleted. **GPU anim data byte-identical.**

### Correctness
spec 105/105, **stress 8/8** (100 animated champions byte-identical), scenarios
96/97 (only G6 imgui #9b, pre-existing), animated subject hashes unchanged.

### Perf (no regression)
frame avg **6670 us** (run 2; baseline 6666), skinning_compute 3928 us
(baseline 3921). Run-to-run noise ±5% — M1 touches only the load path, GPU
passes read byte-identical buffers.

### Alloc (the kill)

| phase | M0 baseline | M1 | reduction |
|---|---|---|---|
| **reload aatrox.glb** | 65,687 a / **65,652 f** | 2,025 a / **1,990 f** | **97%** |
| load aatrox.glb | 65,632 a | 1,970 a | 97% |
| load ahri.glb | 80,389 a | 12,009 a | 85% |
| load aatrox_blood_moon.glb | 68,016 a | 2,158 a | 97% |
| boot load (100 GLBs) | 5,400,016 a | **500,872 a** | **91%** |

The reload allocation storm is gone (65.7k frees → 2.0k). Residual ~2k/load is
the remaining load-time vectors (mesh cpu*, the flat `gpu_*` push_back growth,
node/clip/channel vectors) — M3 (pre-size from accessor metadata) + M5 targets.
ahri's larger residual = more keyframes in its walk clip.

**vk goldens are STALE (pre-existing, NOT an M1 regression).** vk
`[stress]`/`[scenarios]` fail on STATIC subjects too (one die, viking room) —
impossible for M1 (anim-only) to cause, and the identical M1 code renders them
byte-identical on metal. The vk refs were never re-baked after the JS conversion
(`4f87f47`, metal-only). **Metal is the trusted correctness gate** for #229; vk
is compile-checked + the no-behavior-change argument. vk re-bake is a separate
follow-up. (vk re-baked 2026-06-19; see FLAKY_TESTS.md for the residual vk flake.)

---

## M2 — render-thread allocation-free (commit pending)

A 26-agent adversarial audit of the per-frame render path (find → verify)
confirmed `RenderGraph::Bake` scratch is already per-slot-arena-backed, and
surfaced **4 confirmed per-frame heap allocations**:

1. **Render-graph pass closures** (`render_graph.cpp:198`, all modes incl.
   headless): `SetupFn`/`ExecuteFn` were `std::function`; `Reset()` frees them
   every frame and `AddPass` re-allocates — `forward_vp0` + `swap` always run,
   so 4-6 mallocs/frame for the over-SBO closures (≥24 B on libc++).
2-4. **ImGui snapshot** (`engine.hpp:3343-3363`, WINDOWED only): `IM_NEW
   ImDrawData` + `CloneOutput` + `CmdLists.push_back` each frame via ImGui's
   global malloc allocator. Not on the headless gate; deferred (M2-followup —
   persistent per-slot snapshot or `ImGui::SetAllocatorFunctions`).

### Fix (item 1)
New `cairns::InplaceFunction<Sig, 128>` (`src/util/inplace_function.hpp`): a
fixed inline-buffer callable, drop-in for `std::function`, **never heap-
allocates** (ctor `static_assert`s the closure fits; largest measured ~80 B).
`SetupFn`/`ExecuteFn` now use it. No globals (the dispatch vtable is a
per-callable `constexpr`).

### Correctness
metal byte-identical: spec 105/105, stress 8/8, scenarios 96/97 (G6 #9b),
jsmoke 4/4.

### Receipt (steady allocs / 60 frames, settled window)
| | allocs | frees |
|---|---|---|
| M0/M1 baseline | 3174 | 3178 |
| **M2** | **2814** | **2814** |

~360/60 = ~6 closure mallocs/frame eliminated; render thread now allocation-
free. The residual ~47/frame (balanced) is the per-command NDJSON `json::parse`
(one `render.frame` command each) — **M6's target, not the render path**.

### Perf (no regression)
frame avg 6797 us (baseline 6666, within ±5% noise; GPU-dominated by
skinning_compute 3995 us).

---

## M3 — load-time pre-sizing (commit pending)

Pre-pass the glTF primitives for total vert/index counts → `reserve` the mesh
`cpu*` arrays once (was a ~14-realloc doubling chain per mesh in
`LoadMeshFromGltf`). Same for the `scene_gpu` per-batch `pos/attr/idx/skin`
arrays. Byte-identical: spec 105/105, stress 8/8, subject 6/6.

### Receipt (per-GLB load allocs)
| | M1 | M3 |
|---|---|---|
| aatrox.glb | 1970 | 1921 |
| ahri.glb | 12009 | 11957 |
| aatrox_blood_moon.glb | 2158 | 2107 |
| boot (100 GLBs) | 500,872 | 497,257 |

**Modest (~50/GLB)** — the doubling chains ARE gone, but meshes/GLB is small so
the absolute win is small. The bulk per-GLB residual (~1900) is fastgltf-internal
allocation + many small load vectors (nodes/clips/channels/textures); those are
**M0b's arena target** (carve from one reservation → bump, not malloc). M3's
pre-sizing makes those arena carves correctly-sized.

---

## M4 — reserve-to-cap at init (commit pending)

Reserve the NON-canon growing containers up front so boot doesn't doubling-grow
them: engine parallel lists (`prefab_ids_`/`glb_paths_`/`per_prefab_asset_`/
`resident_textures_`/`per_batch_shared_skin_`) in `initResourceManagers`,
`CommandRegistry` `commands_`/`sorted_names_` in its ctor, and `Resources`
`deferred_` ring in `Resources::Init` (metal + vk). Byte-identical (spec 105/105,
stress 8/8, jsmoke 4/4). Marginal alloc win (tens of allocs — these grow to
hundreds, ~log2 reallocs each); mostly tidiness. **The big pool target
(`ResourceManager` hot/cold) is Aaltonen-canon with no `Reserve()` — NOT touched
(needs permission to add one).** Caps will centralize into M0b's `MemoryBudget`.

---

## M0b — reservation FOUNDATION (commit pending)

The user's "allocate 1 GB, chop it up, only ever use that X GB." Two pieces:
- **`MemoryBudget`** (`src/util/memory_budget.hpp`): the single source of every
  domain reservation size, per-platform (CPU-persistent **1 GB desktop / 256 MB
  Android**, GPU resident, staging ring, JS 256 MB, NDJSON 4 MB). M5 reads
  `gpu_*`; the CPU-arena activation reads `cpu_persistent_bytes`.
- **`ChunkAllocator` fixed-reservation mode** (`InitReserved`): HARD-CAPS the
  chunk pool at `ceil(budget/block)` — fails loud (null → `ChunkStdAllocator`
  aborts) instead of mallocing past budget. Chunks stay 4 MB each (chunked
  backing, no single giant OS alloc → Android-safe), grow lazily to the cap.

Spec: `[memory]` 3 cases / 1013 assertions; full spec **108/108** (was 105).

**Dormant by design (no receipt yet).** `ChunkAllocator` has ZERO users today —
activating a 1 GB reservation now would just waste RAM (Android-OOM risk) since
nothing carves from it. The **payoff** is the follow-on migration: route the hot
load vectors (`Mesh::Cold`/`Prefab::Cold` cpu+gpu, anim spans) onto a
`ChunkStdAllocator` over this reservation (per-prefab arena, freed wholesale at
unload) — that's where the boot ~497k load allocs collapse. The mechanism +
sizing are now in place and tested; activation is the next structural pass.

---

## M5 — GPU reservation sizing via MemoryBudget (commit pending)

`MemoryBudget` is now the **live** single source for the GPU reservation: the
engine's persistent skin-output pool (the biggest up-front GPU reservation — 1 GB
desktop / 128 MB Android, already reserved once at `GreaterInit`) and its
device-cap fail-loud check (`SkinPoolFitsDevice`, the Adreno-730 lesson) both
read `MemoryBudget::Default().gpu_skin_pool_bytes` instead of scattered
`#if`-platform `constexpr`s. Same values, one place to tune. Added
`gpu_skin_pool_bytes`. Byte-identical: spec 108/108, stress 8/8, subject 6/6;
metal + vk compile.

**Deferred (structural follow-on):** pre-reserving the `MemoryAllocator` mesh/
texture blocks up front to `gpu_resident_bytes` (the headline "~5 GB reserved")
is a real metal+vulkan change with the same waste/Android-OOM concern unless
paired with streaming residency. Not rushed at the tail of this session (GPU
memory management = exactly the "small disaster" risk). The skin pool — the
dominant fixed GPU reservation — is already up-front and now budget-sourced.

---

## M7 — QuickJS per-context heap (commit pending)

QuickJS now allocates from a **fixed `ChunkAllocator` reservation** (256 MB,
`MemoryBudget::js_heap_bytes`) instead of libc malloc: `JS_NewRuntime2` with
custom `JSMallocFunctions` whose `opaque` is the per-runtime `ChunkAllocator`;
added `ChunkAllocator::Reallocate` + static `UsableSize` for the JS realloc /
usable-size hooks; `JS_SetMemoryLimit` as a graceful pre-cap. A size-class
free-list is the right tool for QuickJS's millions of tiny allocs (NOT the
offset allocator — that's the GPU range domain). **This also ACTIVATES M0b's
`ChunkAllocator` — it's no longer dormant; QuickJS is a real user.**

Bounds the JS heap by construction (only ever 256 MB, fail-loud past it) and
recycles cells within the reservation across `script.reload` fresh-context
swaps (no OS churn). Verified: jsmoke 4/4, all JS-driven `[scenarios]` pass
(1 fail = G6 #9b flake), stress 8/8, spec 108/108, **serve boot smoke clean**
(run.js's 100-GLB JS boot runs end-to-end on the new heap), metal+vk compile.

_Receipt note:_ not visible in `[ALLOC-RECEIPT]` — that counts C++ `operator
new`; QuickJS is C `malloc`. The win is the **bounded** heap, not a delta in the
C++ counter. Per-context wholesale-region reclaim (vs. free-list recycle) is a
later refinement.

---

## M6 — NDJSON dispatch copy-elimination (commit pending)

`CommandRegistry::Dispatch` no longer copies the op name into a `std::string`
(now a `string_view` into the request + a heterogeneous `FindOp` comparator) nor
copies the `args` subtree (passed by const-ref straight from the request, no-args
binds one empty object via pointer — no ternary copy-to-common-type). Byte-
identical: spec 108/108, jsmoke 4/4, scenarios green (1 = G6 #9b).

### Receipt (steady allocs / 60 frames)
| | allocs |
|---|---|
| M2 | 2814 |
| **M6** | **2688** |

**Modest (~2/command).** Honest scope: true "zero-alloc NDJSON" additionally
needs (a) a SAX tokenizer to skip the request-tree `json::parse` (~3-5/command)
+ a per-command scratch arena, and (b) a handler-signature migration off
`std::function<json(const json&)>` — large, deferred. AND the dominant ~45/frame
steady residual is **not** NDJSON: it's the surfaceless per-frame render/extract
path (a separate concern; the real windowed app renders via `draw()`, never
parsing NDJSON per frame). M6 took the safe, contained copy-elimination.

---

# Compaction plan (2026-06-21) — "all CPU state in one block + determinism hash"

**Branch** `ia/26-06/gfx/compact-cpu-state-one-block`. Phases P0..P7 of
`~/dev/plans/2026-06-21_gfx_compact-cpu-state-one-block.md`.

**THERMAL CAVEAT (read this):** this is an M-series laptop under *sustained*
load (continuous builds + golden runs keep the GPU hot). Absolute frame/GPU
times swing **2.5–3×** with thermal state — the cold first-boot baseline (frame
2242µs) is the coolest reading; warm steady-state is ~5000–7000µs. So **absolute
numbers across phases are NOT comparable** unless measured back-to-back on the
same thermal state. The durable signals are: (a) the per-pass *breakdown* shape,
(b) `cpu_block_` live bytes per region, (c) golden green. Behavior-neutral phases
(P0/P0b/P0c) have perf parity *by construction*.

Harness: `scripts/_receipts.sh build/spec-mac-metal/Release/cairns_serve perf`
(500 actors, 120-frame `[Timer]`).

## P0 — wire `cpu_block_` ChunkAllocator (no behavior change)

`Engine::cpu_block_` + `InitReserved(MemoryBudget::cpu_persistent_bytes)` (1 GB,
fail-loud) + 4 region tags. Nothing carved from it yet. **Golden: metal 97/97;
vk 44/45** (the 1 = known intermittent vk subject flake). Perf parity by
construction.

| reading | frame | build_draws | record | skinning_compute | forward_vp0(GPU) | swap(GPU) |
|---|---|---|---|---|---|---|
| cold baseline (pre-P0) | 2242 | 105 | 177 | 1385 | 310 | 53 |
| P0 (warm/throttled) | 6231 | 468 | 505 | 3735 | 1237 | 28 |

The P0 row is the same code path under thermal load, not a regression — see
caveat. `cpu_block_` live bytes: 0 (nothing carved yet).

## P0b — purge `<iostream>` (kill the static-init alloc)

8 TUs dropped `#include <iostream>` (the only header that triggers
`std::ios_base::Init` → cout/cin/cerr + locale alloc *before main*). `std::cerr`
logs → `std::fprintf(stderr,…)`; the NDJSON stdin/stdout transport
(`StdioTransport`, `AgentStdinDrain`) converted from `std::istream`/`std::getline`
to C-stdio `FILE*` (new `control/stdio_lines.hpp` ReadLine/WriteLine). `src/` now
has **0** `#include <iostream>`. **Golden: metal three_champ-flake-only (run 2
fully clean); serve C-stdio transport verified responding `{"ok":true}`.**
Behavior-neutral. frame ~5144µs (warm, = P0 thermal band — not a regression).

| reading | frame | build_draws | skinning_compute | forward_vp0(GPU) |
|---|---|---|---|---|
| P0b (warm) | 5144 | 536 | 2911 | 964 |

## P0c — process-globals → Engine-owned

Ripped the 3 process-global `std::atomic<uint64_t>` ID counters: `g_asset_counter`
+ `g_entity_counter` were **unused** (deleted); `g_scene_counter` →
`Engine::NextSceneId()` (per-instance, via `headless::NextSceneId`). Kills the
two-Engine ID-sharing that would false-diverge the run-to-run hash. `Math.random`/
`Date` aren't used by any script → hygiene is a no-op. **JsState static DEFERRED:**
coupled to the `CommandRegistry::Instance()` singleton (a separate excision); the
JS path resets its context per registration (`ResetJsContext` → pristine globals,
stateless-between-dispatches *by design*) and the QuickJS heap lives outside
`cpu_block_` / the hash, so determinism isn't blocked by it. **Golden green ×2;
serve `scene.create` → per-Engine 0,1.** Behavior-neutral. frame ~6623µs (warm).

## P1 — frame region from the block + kill vestigial vectors

`PerSlot::arena` slab now carved from `cpu_block_` (`kRegionFrame`, 16 MB ×
`kFramesInFlight=2` = **32 MB live in the block**) instead of a per-slot
`std::vector<uint8_t> arena_storage` (deleted). The arena's `[0,Used)` (drawList,
sorted, matrices, proxies) is now in the hashable block. Deleted the **six
vestigial `ProxyArray<T>` std::vector slots** (lines/points/skins/lights/cameras/
layers) + the `ProxyArray<T>` wrapper from `RenderProxyArrays` — never produced,
escaped the hash. **Golden: metal 10/10 ×2; vk subject 1-flake; serve boot 60
frames OK.** (16 MB > 4 MB chunk → block routes the slab to its own malloc;
oversize-into-reservation is a follow-on. frame ~6729µs warm.)

| reading | frame | build_draws | skinning_compute | forward_vp0(GPU) | cpu_block live |
|---|---|---|---|---|---|
| P1 (warm) | 6729 | 676 | 3937 | 1285 | ~32 MB (frame slabs) |

## P2 — ResourceManager pools → cpu_block_

`ChunkStdAllocator` gained a **null-arena default ctor (malloc fallback)** +
`propagate_on_container_move_assignment=true` — so `ResourceManager` retyped to
block-backed vectors is **behavior-identical** until Reserved (all pools,
including the RHI ones, keep malloc). `ResourceManager::Reserve(block,cap)` fixes
capacity (Acquire never grows → `Cold*` stable + span-hashable, no garbage tail);
Acquire fail-loud at cap (debug). **Reserved prefabs(600)/meshes(8192)/materials
(8192)/skins(4096)/scenes(8) onto the block** (replaced the scenes Acquire/Release
pre-grow loop). `viewports_` stays malloc (acquired in InitInitialViewport, before
the block exists in GreaterInit). **Validation: builds green; golden 10/10 ×2;
serve boot loaded the full 500-actor / 100-prefab / 500-mesh / 1700-prim workload
from the block, no abort/OOM.** frame ~7433µs (warm — thermal creep, not a
regression; pools-in-block is alloc-source-only).

| reading | frame | build_draws | skinning_compute | forward_vp0(GPU) | pools |
|---|---|---|---|---|---|
| P2 (warm) | 7433 | 618 | 4490 | 1461 | 5/6 in block (viewports malloc) |

## P7 (MVP) — per-frame SIM determinism hash → **the three_champ verdict**

`util/fnv1a.hpp` (streaming FNV-1a 64). In golden mode, before `Submit`, hash the
**frame arena `[0,Used)`** (drawList/sorted/matrices/entity_ids/proxies — all POD,
now in the block) + the sim drivers (`render_angle_deg_`/`accumulator_`/
`sim_frame_`) and print `[STATEHASH] frame=N sim=<hex>`. Also restored the
**frame-slab zero-fill** (dropped in P1 — without it the arena's alignment padding
was fresh-malloc garbage that diverged run-to-run).

**RESULT — three_champ static, 16 runs:** the SIM-hash *sequence* is **byte-
identical every run (1 distinct digest)**. → The per-frame CPU sim input is
**provably deterministic**; when three_champ flakes it is **NOT** the CPU sim
varying — it is render-encode / GPU-execution side. This resolves the key unknown
from the original flake hunt (which had only black-box pixel sampling).

Side note: the pixel flake (≈20% earlier this session) did **not** reproduce in 12
back-to-back runs here — either thermal/timing-sensitive (machine now hot) or the
restored slab zero-fill masked an uninitialized-read. Worth a cold-machine re-check
+ the RENDER-side (kDynamic) hash to nail it.

---

## P3 (partial) — Mesh::Cold cpu* temporaries → block (2026-06-21)

`Mesh::Cold::{cpuPositions,cpuAttrs,cpuIndices,cpuSkinAttrs}` retyped to
`std::vector<T, ChunkStdAllocator<T>>`, re-seated onto `cpu_block_` in
`LoadMeshFromGltf` (block threaded through `LoadPrefabFromGltf`). Behavior-neutral
— only the allocator type changes; every `reserve`/`push_back`/`clear`/`empty`
site untouched (POCMA move-assign adopts the block, replacing the `.clear()` head).
Mesh load-temporaries now fall under the 1 GB cap + are hash-reachable. `ArenaSlice<T>`
DS landed in `cpu_arena.hpp` (pure-POD `{offset,count}`) — staged for the later
raw-span pass; loader took the behavior-neutral `ChunkStdAllocator` route.

**Perf — metal, 500 actors (100 GLB × 5), 120 frames (warm, thermally-dominated):**

| slot | avg µs |
|---|---|
| frame | 5246 |
| build_draws | 718 |
| record | 536 |
| skinning_compute | 2854 |
| particle_sim | 51 |
| forward_vp0 | 952 |
| swap | 21 |
| skin_eval | 33 |

In-band with prior phases (frame 5–7 ms warm, this throttling laptop). No alloc
regression; 500-actor boot loads clean from the block, no exhaustion.

---

## STATUS @ autonomous run (2026-06-21, cont.)

**Committed, green, tested:** P0 · P0b · P0c · P1 · P2 · P4 (loose POD engine
vectors → block) · P6 (entt → `ChunkStdAllocator`) · P7-MVP (SIM determinism hash
→ verdict above) · **P3-partial** (Mesh::Cold cpu* → block, above). Golden 10/10
metal every phase; VK green except the **pre-existing** imgui-overlay font-atlas
flake (verified: fails without P3 too); 500-actor serve boot loads from the block.

**Remaining:**
- **P3 (rest)** Prefab::Cold/Hot tables + Node/Skin/Clip nested vectors + string
  interning → block (behavior-neutral `ChunkStdAllocator` route, as Mesh::Cold).
- **P5** fastgltf temporary PMR (after P3 destinations are block-backed).
- **P7 finish:** RENDER (kDynamic) hash, first-divergence dump, `tests/test_state_hash.cpp`,
  the counting-`new` Q2 verifier. Plus the `JsState`/`CommandRegistry` de-singleton
  (P0c deferred) before the run-to-run *test* (the run-to-run *hash* already verified).
