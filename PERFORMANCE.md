# Performance log

Numbers keyed by git commit. Workload, build mode, and platform notes inline.
Newest first.

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

### iOS Simulator (iPhone 16 Pro, Release) — still crashes
Same `MTLSimDevice::newHeapWithDescriptor:` SIGABRT but **the actual error is
not OOM** — it's:
```
(Metal) -[MTLSimDevice newHeapWithDescriptor:], line 1199:
   error 'MTLStorageModePrivate is required for heaps'
```
The iOS Simulator's Metal stub refuses to create any MTL::Heap that isn't
`StorageModePrivate`. Our upload bump ring (and readback ring) use
`StorageModeShared` because the CPU needs to map them — heaps are right out
on the simulator. Real iPhone is unaffected (full Metal supports Shared heaps).

This is unrelated to batched-upload work — that fix landed at `e67a2bb` (CPU
peak now bounded to 1 batch × 10 GLBs instead of all-GLBs-at-once) and is
verified to render clean on macOS Metal Release. iOS-Sim will need a separate
"shared-mode bump buffer not heap" carve-out before it can run, since the
simulator can't host the same allocator shape as real hardware.

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
