# Performance log

Numbers keyed by git commit. Workload, build mode, and platform notes inline.
Newest first.

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
