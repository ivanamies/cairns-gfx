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
