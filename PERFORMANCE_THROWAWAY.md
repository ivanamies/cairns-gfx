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
follow-up.
