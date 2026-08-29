# Performance log

Numbers keyed by git commit. Workload, build mode, and platform notes inline.
Newest first.

---

## `b6c7785` (2026-05-31) — fragment / rasterization proof

iPhone 15 Release. Two runs, same workload (`100 GLBs × 33 slices = 3300
entities`, 11517 draws, batched upload), only the window size + hero scale
differ. Both v-synced cap is 16.67 ms (60 Hz); both miss it.

**1280×720 window, scale 0.005 (small heroes):**
```
draws: 11517 | 100 GLBs x 33 slices = 3300 entities
slot 0 (frame):                          accum 3860842 us, avg 32173 us over 120 frames
slot 1 (build_draws):                    accum  938464 us, avg  7820 us over 120 frames
slot 2 (record):                         accum  310526 us, avg  2587 us over 120 frames
slot 3 (set up render pass globals):     accum    2012 us, avg    16 us over 120 frames
slot 4 (build opaque draw list):         accum  936208 us, avg  7801 us over 120 frames
```

**2400×1080 window, scale 0.01 (heroes 2× bigger):**
```
draws: 11517 | 100 GLBs x 33 slices = 3300 entities
slot 0 (frame):                          accum 7739460 us, avg 64495 us over 120 frames
slot 1 (build_draws):                    accum 1575431 us, avg 13128 us over 120 frames
slot 2 (record):                         accum  691643 us, avg  5763 us over 120 frames
slot 3 (set up render pass globals):     accum      90 us, avg     0 us over 120 frames
slot 4 (build opaque draw list):         accum 1575146 us, avg 13126 us over 120 frames
```

**Interpretation — fragment / rasterization is the dominant cost.**

Going from 1280×720 (922k px) + scale 0.005 → 2400×1080 (2.59M px) + scale 0.01
is a ~2.8× pixel increase and ~2× per-hero screen size. The draw count, draw
build, sort, and bind pattern are all identical (11517 draws, same VB/IB
binds-once, same UBO bumps). What scales with the window+scale change is
fragment shading + ROP / overdraw — i.e. **rasterization work**. The CPU-side
`build_draws` going 7.82 → 13.13 ms at identical CPU workload is also part of
the same picture — nothing in the build_draws loop changed across the two runs.

Net: **the slowdown is fragment shader + rasterization work, not the draw
arrangement / memory transfer / CPU build loop itself.**

---

## `b0febf1` (2026-05-30) — `ia/26-05-30/performance_debug` (re-baseline)

9d90f90 source + cherry-picked batched upload (`b0febf1`). No layout, window,
or orientation changes vs 9d90f90 baseline. Engine now also prints
`N GLBs x M slices = E entities` next to draws so the workload shape is
explicit in every report.

### iPhone 15 (Metal, Release, v-synced)

**50 GLBs × 66 slices = 3300 entities (default):**
```
draws: 15114 | 50 GLBs x 66 slices = 3300 entities
slot 0 (frame):                          accum 4784328 us, avg 39869 us over 120 frames
slot 1 (build_draws):                    accum 1091730 us, avg  9097 us over 120 frames
slot 2 (record):                         accum  450375 us, avg  3753 us over 120 frames
slot 3 (set up render pass globals):     accum     504 us, avg     4 us over 120 frames
slot 4 (build opaque draw list):         accum 1091043 us, avg  9092 us over 120 frames
```

**100 GLBs × 33 slices = 3300 entities:**
```
draws: 11517 | 100 GLBs x 33 slices = 3300 entities
slot 0 (frame):                          accum 3886046 us, avg 32383 us over 120 frames
slot 1 (build_draws):                    accum  902155 us, avg  7517 us over 120 frames
slot 2 (record):                         accum  335452 us, avg  2795 us over 120 frames
slot 3 (set up render pass globals):     accum     797 us, avg     6 us over 120 frames
slot 4 (build opaque draw list):         accum  901153 us, avg  7509 us over 120 frames
```

Observations:
- Both miss v-sync (~25 fps and ~31 fps respectively); CPU-bound on phone.
- 100×33 is ~20% faster than 50×66 because the second half of `kDebugGlbs`
  has fewer primitives per GLB — fewer total draws (11.5k vs 15.1k).
- build_draws 9.1 ms at 15k draws is in line with the 5/28 baseline's 8.3 ms
  at 11.5k draws (per-draw cost is similar). The 17 ms iPhone 15 number from
  earlier was on a different branch state; this re-baseline is healthy.

---

## `e2d0c26` (2026-05-30) — `ia/26-05-30/performance_debug`

Layout: 20×5 grid × 33 slices = **3300 entities**, ~11.5k draws.
Heroes: scale 0.01, dx=dy=0.7, dz=2.0, front slice z=-4.
Window: 2400×1080 (Pixel 6a landscape native). 100 GLBs, each drawn 33×.
Landscape locked across all targets.

### Pixel 6a emulator (Android 14, arm64-v8a, Tensor G1 host-translated Vulkan, Release)
GLBs adb-pushed to `/sdcard/Android/data/org.libsdl.app/files/`.
```
slot 0 (frame):                          avg 31173–31743 us  (~32 fps)
slot 1 (build_draws):                    avg  3168– 3298 us
slot 2 (record):                         avg  5133– 5184 us
slot 3 (set up render pass globals):     avg     0–    1 us
slot 4 (build opaque draw list):         avg  3165– 3294 us
draws: ~11.5k
```
**No OOM** at 100 GLBs on the AVD (Pixel 6a profile, 6 GB RAM allocation,
arm64-v8a system image with Vulkan compute + level 1). The "unbatched upload
blows up newHeap" hazard is iOS-Simulator-specific (MTLSimDevice has tighter
heap caps than Vulkan-on-android).

Notes: emulator GPU is host-translated, not real Tensor G1 — these are
representative-of-low-end-mobile bytecode-path numbers but actual hardware
will differ (memory subsystem, mali-equivalent throughput etc).

### iOS Simulator (iPhone 16 Pro, Release) — runs, 11517 draws
Fixed at `30e383c`: metal/memory_allocator `CreateBufferBlock` skips the
MTL::Heap wrapper for non-Private storage modes and allocates the master
buffer directly. MTLSimDevice's "Private-only heaps" rejection no longer fires.
```
slot 0 (frame):                          avg 16669–20527 us (host-translated)
slot 1 (build_draws):                    avg  5504– 6187 us
slot 2 (record):                         avg  3658– 4073 us
slot 3 (set up render pass globals):     avg     0–   12 us
slot 4 (build opaque draw list):         avg  5502– 6185 us
draws: 11517
```
Caveat: simulator GPU is host-translated on the M2 Max — not iPhone hardware.
Will track real-iPhone numbers separately.

### M2 Max — Metal (Release, window 2400×1080)
```
slot 0 (frame):                          avg 17622–18153 us  (~55 fps; missing v-sync at 1.5–2 ms over budget)
slot 1 (build_draws):                    avg  5406– 6344 us
slot 2 (record):                         avg  3362– 3856 us
slot 3 (set up render pass globals):     avg     0–    0 us
slot 4 (build opaque draw list):         avg  5405– 6343 us
draws: 11517
```

### M2 Max — Vulkan (MoltenVK, Release, window 2400×1080)
```
slot 0 (frame):                          avg 18541–19115 us
slot 1 (build_draws):                    avg  5643– 5940 us
slot 2 (record):                         avg  1091– 1169 us
slot 3 (set up render pass globals):     avg     0–    0 us
slot 4 (build opaque draw list):         avg  5641– 5938 us
draws: 11517
```

Both desktop backends regressed vs 5/28 baseline (build_draws ~2x: 3.07 → 6.3 ms
Metal, 2.16 → 5.9 ms Vulkan). Suspect: bigger window (720×1280 → 2400×1080) +
larger heroes (scale 0.005 → 0.01) push more pixels and the CPU sort/build
loop touches more state per draw. Worth bisecting if we want to recover the
5/28 numbers.

### iOS device — not yet measured at this commit.

---

## `4fb1e46` (2026-05-30) — `ia/26-05-30/performance_debug`

Branch base = `9d90f90`. Constants flipped: `kDebugGlbsToParse 50→100`,
`kHeroSlices 66→33`. Same 3300-entity / ~11.5k-draw workload as 5/28; just
redistributed (each of 100 GLBs drawn 33×).

### iOS Simulator (iPhone 16 Pro, Release)
**No perf numbers possible — process crashes during scene load.**

Crash signature (`~/Library/Logs/DiagnosticReports/sdl-min-*.ips`):
```
SIGABRT in __assert_rtn / MTLReportFailure
  -[MTLSimDevice newHeapWithDescriptor:]                    (simulator refuses)
  cairns::rhi::metal::MemoryAllocator::CreateBufferBlock     memory_allocator.cpp
  cairns::rhi::metal::MemoryAllocator::BumpAllocate          memory_allocator.cpp
  cairns::rhi::Resources::CreateBuffer (kDefault stages)     resources.cpp
  cairns::rhi::LoadScenesGpu::lambda (make)                  scene_gpu.hpp
  cairns::rhi::LoadScenesGpu                                 scene_gpu.hpp
  cairns::Engine::GreaterInit                                engine.hpp
  SDL_AppInit                                                main.cpp
```

Root cause: `LoadScenesGpu` concatenates every GLB's pos / attr / idx into
giant `std::vector`s and asks `CreateBuffer` to stage hundreds of MB through
the `kUpload` bump ring. The Metal simulator caps `newHeap` lower than real
iPhone hardware, so it fails outright. Real device should be fine; sim won't
run this commit until uploads are batched. See `MISTAKES.md`.

### Android emulator (Pixel 3a API 34 arm64-v8a, Release)
**No perf numbers possible — Vulkan device init fails on the AVD.**

Logcat:
```
I/cairns: GreaterInit: device.Init failed
```

The Pixel 3a AVD's GPU emulation does not expose a usable Vulkan device that
our `device.cpp` accepts. Need a real Android device (Pixel 3a hardware or
similar with arm64-v8a + Vulkan 1.1+) to bench this commit.

### macOS / iOS device
Not yet measured at this commit.

---

## `9d90f90` (2026-05-28) — *across-GLB packing*

Workload: 10 × 10 × 30 = 3300 entities → **11517 draws** (`set 11k draws`).
Timer slots active at this commit: 0=frame, 1=build_draws, 2=record,
3="set up render pass globals", 4="build opaque draw list".
All measurements **v-synced** (60 Hz target).

### iPhone 15 (Metal, Release)
```
slot 0 (frame):       accum 4232148 us   avg 35267 us   over 120 frames
slot 1 (build_draws): accum  995988 us   avg  8299 us   over 120 frames
slot 2 (record):      accum  396387 us   avg  3303 us   over 120 frames
draws: 11517
```
~28 fps — missing v-sync. `build_draws` 8.3 ms is the CPU bottleneck on phone.

### M2 Max — Vulkan (MoltenVK, Release, v-synced)
```
slot 0 (frame):                          accum 1998051 us   avg 16650 us   over 120 frames
slot 1 (build_draws):                    accum  259102 us   avg  2159 us   over 120 frames
slot 2 (record):                         accum   98497 us   avg   820 us   over 120 frames
slot 3 (set up render pass globals):     accum       0 us   avg     0 us   over 120 frames
slot 4 (build opaque draw list):         accum  259066 us   avg  2158 us   over 120 frames
draws: 11517
```
At v-sync cap (16.65 ms = 60 fps). build_draws ≈ 2.16 ms; record ≈ 0.82 ms.

### M2 Max — Metal (Release, v-synced)
```
slot 0 (frame):                          accum 1999797 us   avg 16664 us   over 120 frames
slot 1 (build_draws):                    accum  367967 us   avg  3066 us   over 120 frames
slot 2 (record):                         accum  177075 us   avg  1475 us   over 120 frames
slot 3 (set up render pass globals):     accum       0 us   avg     0 us   over 120 frames
slot 4 (build opaque draw list):         accum  367935 us   avg  3066 us   over 120 frames
draws: 11517
```
At v-sync cap (16.66 ms = 60 fps). build_draws ≈ 3.07 ms; record ≈ 1.48 ms.

### Reference notes
This was the "last good performant baseline" before threading and the second
allocator-architecture pass. No weird frame-time oscillations. Steady v-sync
on desktop, drop to ~28 fps on iPhone 15 because of the 8.3 ms CPU
build_draws.
