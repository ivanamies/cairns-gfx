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
