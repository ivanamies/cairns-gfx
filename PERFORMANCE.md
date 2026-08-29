# Performance log

Numbers keyed by git commit. Workload, build mode, and platform notes inline.
Newest first.

---

## `b69f582` (2026-07-05) — perf smoke, 4-platform pass

Scenario-driven (desktop via `scripts/dev_drive.sh` eval of the scenario script;
Android + web via the picker), 120-frame timer windows at steady state. Actor
counts differ by platform budget: desktop 300, phone 100, web 50 (web MEMFS caps
the champion preload at 50; the S22 tile / 256 MB skin-pool budget at 100/300).

### macOS, M2 Max, Release, 2560×1440 — perf_smoke_300 (300 actors)

| Pass               | metal   | vk      |
|--------------------|---------|---------|
| `frame`            | 1.65 ms | 1.99 ms |
| `build_draws`      | 1.47 ms | 1.77 ms |
| `record`           | 1.35 ms | 0.51 ms |
| `forward_vp0`      | 1.06 ms | 1.49 ms |
| `skinning_compute` | 3.54 ms | 3.28 ms |
| `swap`             | 0.13 ms | 0.09 ms |
| `skin_eval`        | 0.04 ms | 0.04 ms |
| `present_wait`     | 0.00 ms | 0.04 ms |
| `acquire_wait`     | —       | 0.04 ms |
| `fence_wait`       | —       | 0.01 ms |

### Android S22 (Adreno 730), Release, 2115×1008 — perf_smoke_100 (100 actors)

| Pass               | avg     |
|--------------------|---------|
| `frame`            | 9.32 ms |
| `build_draws`      | 3.17 ms |
| `record`           | 3.91 ms |
| `forward_vp0`      | 3.68 ms |
| `skinning_compute` | 9.06 ms |
| `swap`             | 0.45 ms |
| `skin_eval`        | 0.19 ms |
| `present_wait`     | 5.26 ms |
| `acquire_wait`     | 2.40 ms |
| `fence_wait`       | 9.37 ms |

### WebGPU, headed Chrome (ANGLE/Metal), 1280×720 — perf_smoke_50 (50 actors)

⚠️ **GPU timings are broken on wgpu.** `gpu_frame` reads 0.00 and the GPU-pass
slots (`forward_vp0`, `skinning_compute`) are not recorded — only the CPU-side
slots below are valid. Browser-variable (~±40% window-to-window; values are
representative). Capture method: README.md "Driving the WASM build in a HEADED
browser."

| Pass               | avg      |
|--------------------|----------|
| `frame`            | ~2.0 ms  |
| `build_draws`      | ~0.8 ms  |
| `record`           | ~1.0 ms  |
| `skin_eval`        | 0.02 ms  |
| `present_wait`     | 0.00 ms  |
| `forward_vp0`      | — (GPU timing broken) |
| `skinning_compute` | — (GPU timing broken) |

---

## `6674340` (2026-06-22) — perf smoke, 300 actors / 100 distinct GLBs

**Forced down from 500 → 300 actors by WebGPU.** The anim_eval SSBO pack (12 → 6
buffers, to fit WebGPU's 8/10 storage-buffers-per-stage floor) and the
WebGPU-portable **256 MB skin-pool cap** are the cause: the whole skin-output
pool is bound as ONE storage buffer, and the max storage-buffer-binding range is
128 MB on mobile (Adreno-730 / S22, measured on-device) / 256 MB on desktop +
webgpu. 400 actors' skinned output is ~285 MB (measured headless via cairns_serve
— it overflows 256 MB and aborts); 300 fits (~214 MB). So the desktop benchmark
drops 500 → 300 as the price of WebGPU support.

Loaded via the new `cairns.scenario.load("00_perf_smoke_300")`. M2 Max, sdl-min
windowed, ~40 s capture / 120-frame window, vsync (present_pacing ~13.3 ms).

| Pass               | metal Release | vk Release |
|--------------------|---------------|------------|
| `frame`            |  0.58 ms      |  1.26 ms   |
| `build_draws`      |  0.46 ms      |  1.06 ms   |
| `record`           |  0.45 ms      |  0.33 ms   |
| `skinning_compute` |  3.56 ms      |  3.21 ms   |
| `particle_sim`     |  0.10 ms      |  0.00 ms   |
| `forward_vp0`      |  1.11 ms      |  1.46 ms   |
| `swap`             |  0.12 ms      |  0.09 ms   |
| `skin_eval`        |  0.02 ms      |  0.03 ms   |
| `present_wait`     |  0.00 ms      |  0.03 ms   |
| `acquire_wait`     | —             |  0.03 ms   |
| `fence_wait`       | —             |  0.01 ms   |

`skin_eval` (the packed anim_eval palette build) is ~0.02–0.03 ms — the 12 → 6
SSBO pack added no measurable per-frame cost vs the pre-pack 0.03 ms baseline.
The cost of WebGPU portability is the actor-count drop (500 → 300), not frame time.

---

## `73ce3c4` (2026-06-17) — run.js boot, 500 actors / 100 distinct GLBs

### macOS, M2 Max, vk Release, 2560×1440, 30s capture, 120-frame window

| Pass               | avg     |
|--------------------|---------|
| `frame`            | 20.66 ms |
| `build_draws`      |  0.59 ms |
| `record`           |  0.19 ms |
| `skinning_compute` |  5.00 ms |
| `particle_sim`     |  0.01 ms |
| `forward_vp0`      |  1.99 ms |
| `swap`             |  0.13 ms |
| `skin_eval`        |  0.03 ms |
| `present_wait`     |  0.01 ms |
| `acquire_wait`     |  0.01 ms |
| `fence_wait`       |  0.00 ms |

### Samsung S22 vk Release — BROKEN

---

## `c78ed77` (2026-06-15) — revert S.1 LDS palette; 500 actors / 100 distinct GLBs

### macOS, M2 Max, 2560×1440, vsync, CAIRNS_AGENT_STDIN + spawnTotal(500)

| Pass               | metal Release | vk Release |
|--------------------|---------------|------------|
| `frame`            | 20.84 ms      | 20.81 ms   |
| `build_draws`      |  0.54 ms      |  0.59 ms   |
| `record`           |  0.47 ms      |  0.21 ms   |
| `skinning_compute` |  5.68 ms      |  5.04 ms   |
| `particle_sim`     |  0.10 ms      |  0.01 ms   |
| `forward_vp0`      |  1.78 ms      |  1.74 ms   |
| `swap`             |  0.26 ms      |  0.14 ms   |
| `present_pacing`   | 12.54 ms      | —          |
| `skin_eval`        |  0.03 ms      |  0.03 ms   |
| `present_wait`     |  0.00 ms      |  0.01 ms   |
| `acquire_wait`     | —             |  0.01 ms   |
| `fence_wait`       | —             |  0.00 ms   |

Note on `frame` = 20.84 ms: `CGDisplayCopyDisplayMode(CGMainDisplayID()).refreshRate` returned **47.95 Hz** at the time of this capture. 1/47.95 Hz = 20.86 ms. The MBP built-in ProMotion display had stepped to its 48 Hz tier. After forcing the display to 60 Hz via System Settings → Displays → Refresh Rate, the panel reports 60.0 Hz. The 20.84 ms was the panel, not the engine.

---

## bisect (2026-06-15) — skinning_compute regression 2026-06-09 → 2026-06-11

Bisect run today on `bisect/skinning-perf` to chase the bad animation GPU numbers.
500 actors / 100 distinct GLBs, metal Release, M2 Max, 2560×1440, vsync (frame locked ~20.84 ms).
Bisect threshold: skinning_compute < 8 ms = good, ≥ 8 ms = bad.

| date          | commit       | description                                | skinning_compute    | forward_vp0 | record  | build_draws | verdict |
|---------------|--------------|--------------------------------------------|---------------------|-------------|---------|-------------|---------|
| 2026-06-09 23:56 | `c838f5f` | EOD 6/9 — P9 walking-clip + SkinRef attach | — (slot not present)| 1.72 ms     | 0.40 ms | 0.52 ms     | —       |
| 2026-06-10 23:56 | `302f58c` | EOD 6/10 — imgui flicker fix               | 5.18 ms             | 3.87 ms     | 0.60 ms | 0.64 ms     | good    |
| 2026-06-11 21:11 | `9737baa` | phase A.1 conditional id MRT               | 4.97 ms             | —           | —       | —           | good    |
| 2026-06-11 21:50 | `a82c326` | windowed crash fix (ImDrawData bypass)     | 4.86 ms             | —           | —       | —           | good    |
| 2026-06-11 21:57 | `2694662` | phase H.6 hoist resident_textures          | 4.84 ms             | —           | —       | —           | good    |
| 2026-06-11 22:03 | `a155ac9` | phase E.0 vk generic recorder loops        | 4.94 ms             | —           | —       | —           | good    |
| **2026-06-11 22:08** | **`b5495c4`** | **phase S.1 LDS palette in skin.comp** | **11.70 ms**    | —           | —       | —           | **first BAD** |
| 2026-06-11 23:26 | `9a94846` | EOD 6/11 — Phase S.2 pack skin attrs       | 11.09 ms            | 1.49 ms     | 0.48 ms | 0.53 ms     | bad     |
| 2026-06-15 21:23 | `577938e` | known-good rewind = `984dae1`              | 14.11 ms            | 1.51 ms     | 0.42 ms | 0.47 ms     | bad     |

First bad commit: `b5495c4`. `shared mat4 s_palette[256]` (16 KB threadgroup memory per workgroup); metal mirror stores palette as 4-rows-per-joint with rebuild-on-read.

### Samsung S22 vk Release (Adreno 730), 1280×720, CAIRNS_N=500 default workload

| commit | description | frame | skinning_compute | forward_vp0 | build_draws | record | present_wait | acquire_wait | swap | skin_eval |
|---|---|---|---|---|---|---|---|---|---|---|
| `9737baa` | phase A.1 conditional id MRT (good) | 42.04 ms | 30.46 ms | 11.13 ms | 12.77 ms | 12.76 ms | 28.07 ms | 25.42 ms | 0.44 ms | 0.22 ms |
| `b5495c4` | phase S.1 LDS palette in skin.comp (first BAD) | — | — | — | — | — | — | — | — | — |

`b5495c4` row not captured on S22: all animations are broken at this commit on Adreno (skin output unusable), so the perf number isn't a valid comparison.

---

## `577938e` (2026-06-15) — known-good baseline, 500 actors / 100 distinct GLBs

### macOS Metal Release — M2 Max, 2560×1440

| Pass               | avg     |
|--------------------|---------|
| CPU                | 21.39 ms (avg 20.85, peak 24.96) |
| FPS                | 47      |
| `gpu_frame`        | 20.28 ms |
| `frame`            | 20.84 ms |
| `build_draws`      |  0.47 ms |
| `record`           |  0.42 ms |
| `forward_vp0`      |  1.51 ms |
| `swap`             |  0.24 ms |
| `present_pacing`   |  4.35 ms |
| `skinning_compute` | 14.11 ms |
| `skin_eval`        |  0.03 ms |
| `present_wait`     |  0.00 ms |

---

## `<skinning P3>` (2026-06-09) — #221 Skinning Phase 3: ring growth + persistent skin output pool

Memory budget note (no perf rows yet -- skinned content not loaded yet):

- `kDynamic` per-slot ring grown 16 MB -> 32 MB on both backends
  (`src/rhi/vulkan/memory_allocator.cpp:73` + Metal mirror at
  `src/rhi/metal/memory_allocator.cpp:91`). Cost: +16 MB / slot *
  `kFramesInFlight` host-visible memory per backend. Drives palette /
  InstanceMeta / Params bumps in Phase 5 (palettes alone ~12 MB at the
  v7 3000-actor target).
- `kArenaBytesPerSlot` raised 4 MiB -> 16 MiB
  (`src/engine.hpp:kArenaBytesPerSlot`). Covers per-frame skin staging on
  the per-slot CPU arena; Phase 5 will print `BumpArena::HighWater()` and
  the constant may be revised from data in that commit.
- `skin_output_pool_buffer_` 256 MB persistent storage buffer (dedicated
  block via `MemoryAllocator::AllocBuffer`'s >`kHeapBlockBytes` path),
  wrapped by `cairns::RangePool skin_output_pool_` in vec4 units.

---

## `88f7d70+` (2026-06-08) — #220 Steps 1+2+3 (handle-ify LoadedMaterial / Mesh / Scene) + render_graph PassRecord vectors -> std::array push_or_die + multithreaded build_draws experiment (WorkerPool, max-4 cap)

What changed since `dc9b669+`:
- `4e2189a` #220 Step 1: `LoadedMaterial -> ResourceManager<LoadedMaterial>`,
  `MatId = Handle<LoadedMaterial>`, set-2 bind group folded into
  `LoadedMaterial::Hot`. Parallel `material_bind_groups_` vector deleted.
- `94c12a5` #220 Step 2: `Mesh -> ResourceManager<Mesh>`, lifted out of
  `Scene::meshes` into engine-owned `meshes_`; `Scene::meshes` is now
  `std::vector<MeshId>`. CPU temporaries moved to `Mesh::Cold`.
- `18c922e` #220 Step 3: `Scene -> ResourceManager<Scene>`,
  `SceneId = Handle<Scene>`; `Asset::Cold::cpu_graph` flipped from
  `const Scene*` to `SceneId`. Engine carries a parallel
  `std::vector<SceneId> scene_ids_` for order-stable iteration.
- `5796e4d` `PassRecord::baked_color` `std::vector` ->
  `std::array<ColorAttachment, kMaxColorFormats=4>` + `uint8_t count`,
  overflow = fprintf+abort. -77 KB / -1849 grows seen in 3300-hero
  trace.
- `88f7d70` All five PassRecord `std::vector<uint16_t>` fields
  (`reads` / `writes` / `buf_reads` / `buf_writes` /
  `attachment_inputs`) plus `color_outputs` (already in `kMaxColorFormats`)
  and `baked_inputs` converted to fixed `std::array<,N>` + `uint8_t`
  count with push_or_die. Caps: reads=16, writes=8, buf_reads=4,
  buf_writes=4, attachment_inputs=16, baked_inputs=16 (sized for swap
  pass at `kNumViewports=8` headroom).
- Working tree (uncommitted): `BuildMeshOpaqueDraws` fan-out via a
  taskflow-backed `cairns::WorkerPool` pimpl
  (`src/util/worker_pool.{hpp,cpp}`, lives in `cairns_render_thread`
  static lib alongside `render_thread.cpp`). Pool sized to
  `min(hardware_concurrency(), 4)`; raised to that cap deliberately
  after a max-12 sweep showed the same regression. Result: net
  regression on all platforms; see below.

Workload: `CAIRNS_N=3300` (~100 GLBs × 33 slices), ~11220 draws. Release.
Steady-state medians (last 3 of 9–10 timer reports per backend per run).
macOS at 1280×720 (M2-class), S22 at 2115×1008. V-synced at 60 Hz on
macOS; thermally-pinned on S22 (~7–8 fps GPU-bound at this workload).

### macOS Metal Release — 1280×720

| Pass                  | ST baseline | std::thread (12) | taskflow pool (max 4) |
|-----------------------|-------------|------------------|------------------------|
| `frame` (CPU, vsync)  |  16.67 ms   |  16.66 ms        |  16.65 ms              |
| `build_draws` (CPU)   |   2.33 ms   |   2.40 ms (+3 %) |   3.06 ms (+31 %)      |
| `record` (CPU)        |   1.33 ms   |   1.28 ms        |   1.49 ms              |
| `particle_sim` (GPU)  |   0.010 ms  |   0.010 ms       |   0.010 ms             |
| `forward_vp0` (GPU)   |  8.6–9.2 ms |   8.1–9.0 ms     |   8.8–9.7 ms           |
| `swap` (GPU)          |   0.27 ms   |   0.24 ms        |   0.25 ms              |

vs `dc9b669+` ST: `build_draws` +0.23 (2.10 -> 2.33); `record` -0.04;
`forward_vp0` flat within run variance.

### macOS Vulkan / MoltenVK Release — 1280×720

| Pass                  | ST baseline | std::thread (12) | taskflow pool (max 4) |
|-----------------------|-------------|------------------|------------------------|
| `frame` (CPU, vsync)  |  16.65 ms   |  16.65 ms        |  16.62 ms              |
| `build_draws` (CPU)   |   2.20 ms   |   2.31 ms (+5 %) |   3.06 ms (+39 %)      |
| `record` (CPU)        |   0.56 ms   |   0.55 ms        |   0.64 ms              |
| `particle_sim` (GPU)  |   0.015 ms  |   0.016 ms       |   0.015 ms             |
| `forward_vp0` (GPU)   |  8.9–10.3 ms|   9.8–10.2 ms    |   8.4–10.2 ms          |
| `swap` (GPU)          |   0.04 ms   |   0.04 ms        |   0.04 ms              |

vs `dc9b669+` ST: `build_draws` +0.22 (1.98 -> 2.20).

### Android Vulkan Release — Samsung Galaxy S22 (Adreno 730), 2115×1008

Cold-start onwards. Workload here loads 98 of 100 GLBs (missing 2
from the apk assets; same 33-slice grid -> 11179 draws not 11220).
Reports captured per run before sweep killed.

| Pass                  | ST baseline (2 runs) | taskflow pool (max 4, 3 runs) |
|-----------------------|----------------------|-------------------------------|
| `frame` (CPU, wall)   | 117955 / 130952 us   | 141093 / 142284 / 144392 us   |
| `build_draws` (CPU)   |  8538 /  8052 us     |  8622 /  9092 /  8577 us      |
| `record` (CPU)        | 10504 / 10516 us     | 10861 / 11221 / 10971 us      |
| `particle_sim` (GPU)  |     0 /     0 us     |     0 /     0 /     0 us      |
| `forward_vp0` (GPU)   | 73881 / 87074 us     | 94797 / 94687 / 98774 us      |
| `swap` (GPU)          |   574 /   674 us     |   761 /   739 /   786 us      |

`build_draws` taskflow-max-4 vs ST: +0.4 ms average (8.30 -> 8.76).
`forward_vp0` GPU sustained at the high end (~95 ms) -- consistent
with the user-noted 85–120 ms thermal range on this device.

### Reading

Multithreaded `BuildMeshOpaqueDraws` at this workload is a net loss on
every platform tested, with both dispatch backends:
- raw `std::thread` fan-out (per-frame spawn): +3 % on metal, +5 % on
  vk macOS.
- taskflow `tf::Executor::silent_async` + mutex/cv join, max 4 workers:
  +31 % on metal, +39 % on vk macOS, +5 % on S22 vk.

Two effects compounding:
1. Per-chunk work is small (~290 µs on macOS, ~2 ms on S22 at hw=4)
   relative to the dispatch tax (per-task `std::function` construction
   inside `silent_async` + cv-join). At max-4 the regression matched
   max-12, ruling out E-core poisoning as the dominant cost on macOS.
2. `BuildMeshOpaqueDraws` is largely memory-bound (writing ~2.7 MB of
   `Draw` structs per frame); extra cores don't add DRAM bandwidth and
   shared-read calls (`materials_.GetHot`, `BufferBaseOffset`) bounce
   cache lines.

`WorkerPool` infrastructure is committed for later use (the larger
3-stage #221 pipeline will have units of work big enough to amortize
the dispatch cost). The fan-out at the build_draws site is left in
place but should be considered "experiment landed for record-keeping,
revisit with a coarser-grained workload or a tighter dispatch path
(e.g. `tf::Taskflow::for_each_index` with a single queue insertion)
before pulling".

Note on M-series scheduling: the cap-at-4 didn't recover the regression
because (per user) "M-series will still kick you to the bad cores" --
the OS does not honor a soft request to stay on P-cores when 4 workers
are unpinned. A real fix would need explicit QoS class hints
(`pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE)`) inside the
taskflow Executor's worker init, which is out of scope for this commit.

---

## `dc9b669+` (2026-06-06) — #201 / #202 / #204 / #205 landed + Frames::End -> EndSubmit/Present split (vkQueuePresentKHR hoisted to main)

What changed since `0b51ce3+`:
- #201 platform `#ifdef`s removed from `engine.hpp` + `main.cpp`. Engine-side
  surfaceless / dump / clear / resize paths moved into `rhi::Resources` +
  `rhi::Frames` + `rhi::Device`. SDL/RHI glue split into
  `src/shell/sdl_rhi_glue_{metal,vulkan}.cpp`.
- #202 `CAIRNS_*` env vars lowered into `EngineConfig` at the shell
  boundary (`src/shell/env_config.{hpp,cpp}`). Engine never reads
  `std::getenv`.
- #204 / #205 protocol facade `Engine*` -> `Engine&`,
  `bool* quit_flag` -> `bool&`. Dead null-check throws removed.
- `Frames::End` split into `EndSubmit` (render-thread-safe) +
  `Present` (main-thread-only). `vkQueuePresentKHR` on MoltenVK reaches
  into `CALayer` which is documented main-thread-only -- crashed under
  Instruments with `CA_ASSERT_MAIN_THREAD_TRANSACTIONS`. Per-slot
  `PresentPacket` (FrameContext + SwapResolveTarget + `present_ready`
  bool) on `PerSlot`; render thread stashes after EndSubmit, main
  thread waits + presents under `present_m_`. Metal `Present` is a
  no-op (presentDrawable via command buffer is already thread-safe),
  kept symmetric across backends so future ones (WebGPU on weird
  Android GL) don't re-learn it.

Workload: `100 GLBs × 33 slices = 3300 entities`, 11220 draws. Release.
Steady-state medians (last 3 of 4 timer reports per backend; first
report dropped as warm-up). 1280 × 720 logical (2560 × 1440 HiDPI on
M2 Max). V-synced at 60 Hz on macOS.

### macOS Metal Release — M2 Max, 1280×720

| Pass                  | avg      | vs `0b51ce3+` |
|-----------------------|----------|---------------|
| `frame` (CPU)         |  2.10 ms | -0.91 (build_draws + record split sharper) |
| `build_draws` (CPU)   |  2.10 ms | -0.25 |
| `record` (CPU)        |  1.34 ms | -0.15 |
| `particle_sim` (GPU)  |  0.010 ms| flat |
| `forward_vp0` (GPU)   |  9.13 ms | -1.51 |
| `swap` (GPU)          |  0.28 ms | -0.05 |
| GPU total             | ~9.42 ms | -1.56 |

v-sync cap (16.66 ms = 60 fps). The CPU `frame` slot is no longer the
sum-of-passes -- the present-on-main split moved the present wait off
the timed path, so `frame` now reads just the bracket around the slot
work plus the present wait. The GPU pass numbers (forward_vp0 + swap)
are the meaningful tracking targets.

### macOS Vulkan Release (MoltenVK) — M2 Max, 1280×720

| Pass                  | avg      | vs `0b51ce3+` |
|-----------------------|----------|---------------|
| `frame` (CPU)         |  1.97 ms | -0.97 |
| `build_draws` (CPU)   |  1.98 ms | -0.31 |
| `record` (CPU)        |  0.56 ms | -0.09 |
| `particle_sim` (GPU)  |  0.017 ms| +0.002 |
| `forward_vp0` (GPU)   | 10.57 ms | +0.49 (variance; range 10.2–11.1 within run) |
| `swap` (GPU)          |  0.045 ms| +0.005 |
| GPU total             |~10.63 ms | +0.49 |

v-sync cap. MoltenVK `record` is still ~2.4× cheaper than Metal record
(0.56 vs 1.34 ms) -- vk's wider command-buffer surface is being
exercised more aggressively. `forward_vp0` is consistently a hair
higher on vk than metal at this commit (~10.6 vs ~9.1).

### Android Vulkan Release — Samsung Galaxy S22 (SM-S901U, Adreno 730), 2115×1008

Cold-start single window (the user signalled "fine, record now" after
one report at ~5 s in, before thermal saturation). Workload here is
slightly lighter than macOS: 98 of 100 GLBs loaded (the apk's asset
copy missed 2; same 33-slice grid -> 3234 entities / 10890 draws).

| Pass                  | avg (cold start) | vs `0b51ce3+` thermal-saturated |
|-----------------------|------------------|---------------------------------|
| `frame` (CPU)         | 144.11 ms        | +136 (driving GPU @ 7 fps) |
| `build_draws` (CPU)   |   8.11 ms        | +0.15 |
| `record` (CPU)        |  11.01 ms        | +1.95 |
| `particle_sim` (GPU)  |   0     ms       | compute disabled on this run |
| `forward_vp0` (GPU)   |  99.42 ms        | -30.6 (cold start; previous figure was sustained-thermal ~130) |
| `swap` (GPU)          |   0.77 ms        | +0.04 |

Single-report sample only -- the thermal ramp to ~130 ms forward GPU
that the previous entry recorded isn't visible here because the run
was cut short. The cold-start 99 ms forward is consistent with the
previous note ("ramps from cold 97 ms to sustained ~130 ms").

---

## `0b51ce3+` (2026-06-06) — P0–P4 cameras+viewports+selection landed, rhi composition refactor, kNumViewports=1

P0–P4 of the Resizing & Cameras plan all landed (`#188`–`#192`), plus
the rhi composition-not-ifdef refactor across Pipelines/Device/Frames/
Resources/Allocator/CommandRecorder/SwapResolveTarget/InitConfig/
resource_manager. P2's two-viewport composite is now gated behind
`kNumViewports = 1` -- rendering the same world twice from two cameras
is wasteful for the default workload; multi-viewport returns when
multi-world content (#195) gives the second viewport something
different to render. The per-viewport plumbing (per-viewport globals,
per-viewport draw lists, split `forward_vp<i>` passes, half-width
composite) all stayed; only the count flipped from 2 to 1.

Workload: `100 GLBs × 33 slices = 3300 entities`, 11517 draws. Release.
Steady-state medians (last 3 of 9–10 timer reports, warmup window dropped).
1280×720 (2560×1440 HiDPI).

### macOS Metal Release — M2 Max, 1280×720
| Pass                  | avg      | vs `757f552` |
|-----------------------|----------|--------------|
| `frame` (CPU)         |  3.01 ms | +0.19 |
| `build_draws` (CPU)   |  2.35 ms | +0.14 |
| `record` (CPU)        |  1.49 ms | +0.04 |
| `particle_sim` (GPU)  |  0.011 ms | flat |
| `forward_vp0` (GPU)   | 10.64 ms | +1.54 (vs 9.10) |
| `swap` (GPU)          |  0.33 ms | -0.12 |
| GPU total             | ~10.98 ms | +1.43 |

### macOS Vulkan Release (MoltenVK) — M2 Max, 1280×720
| Pass                  | avg      | vs `757f552` |
|-----------------------|----------|--------------|
| `frame` (CPU)         |  2.94 ms | n/a (P2 forward split visible) |
| `build_draws` (CPU)   |  2.29 ms | n/a |
| `record` (CPU)        |  0.65 ms | -0.17 |
| `particle_sim` (GPU)  |  0.015 ms | flat |
| `forward_vp0` (GPU)   | 10.08 ms | +1.0 |
| `swap` (GPU)          |  0.04 ms | -0.06 |
| GPU total             | ~10.14 ms | +0.94 |

### Android Vulkan Release — Samsung Galaxy S22 (SM-S901U, Adreno), 2115×1008
| Pass                  | avg (thermal-saturated) |
|-----------------------|-------------------------|
| `frame` (CPU)         |   7.96 ms |
| `build_draws` (CPU)   |   6.97 ms |
| `record` (CPU)        |  10.83 ms |
| `particle_sim` (GPU)  |   0.000 ms |
| `forward_vp0` (GPU)   | ~130 ms (sustained) |
| `swap` (GPU)          |   ~1.0 ms |
| GPU total             | ~131 ms |

~7-9 fps. Native portrait-rotated landscape resolution is 2115×1008
≈ 2.13 M pixels, **2.3×** the macOS 1280×720 workload. GPU forward
sustained at ~130 ms after thermal saturation (cold-start runs come
in at ~95–105 ms; second 120-frame window 110–123 ms; settles to
~130 ms after a couple of minutes). GPU is ~10–13× the M2 Max at
~2.3× the pixels -- the Adreno + thermal envelope is the cap.

The earlier crash on this branch (`Engine::GreaterInit+1244` SIGSEGV
in strlen) was: I removed `ambessa.glb` + `ambessa_chosen_of_the_wolf.
glb` from the `kDebugGlbs` array, but the loop still indexed
`kDebugGlbs[3..103)` -- past the new array end. `std::string_view(nullptr)`
on the past-end garbage entry called `strlen(nullptr)`. Fix in this
commit: refilled the array to 103 entries with the two new
`anivia_papercraft` / `anivia_prehistoric` files from the staging
rename, and added a `static_assert` in `debug_asset.hpp` so the same
bug fails the build instead of the device. No defensive `std::min` --
hit the exact count or die at compile.

### iOS Release — iPhone 15
Not measured this commit. Needs device deploy; the iOS Xcode target
builds clean (`build/ios/`) but the run is a separate step.

### Reading the numbers
- macOS GPU forward got slightly heavier (+1 to +1.5 ms vs the
  `757f552` single-viewport baseline). Cause is structural: the
  per-viewport forward pass now writes to an offscreen at
  fb_w/kNumViewports × fb_h (with kNumViewports = 1 that's the full
  swap-target dimensions, so no shrinkage benefit). The composite is
  doing one fullscreen `DrawFullscreen` instead of going straight to
  the swapchain, which is the small but real cost.
- Vk `record` got CHEAPER (-0.17 ms) despite the indirection -- the
  vk offscreen framebuffer cache pays off on the second-and-later
  frames. The framebuffer cache is exactly what P3's
  `OffscreenTargetCache::FlushFramebuffers` invalidates on resize.
- Vk `swap` is ~8× smaller than Metal's (0.04 vs 0.33 ms). Same
  observation as before: metal's swap pass rebuilds its
  RenderPassDescriptor inline every frame. Low-hanging optimization.

---

## `757f552` (2026-06-06) — studio surface Day 1 (Unity-shaped scripting via `studio.js`)

Day 1 of the Unity-shaped op surface landed: `RegisterAlias` +
`Command.aliased_for` + `tools.search` on the registry; 18 ops migrated
to `cairns.*` with top-level deprecated aliases (one release); `studio.js`
autoloaded into the QuickJS context with `Vector3`/`Quaternion`/`Mathf` +
`GameObject`/`Component`/`Transform` wrappers + `Camera.main` strict-throw
+ `Cairns.onFrame` reserved stub; `docs/studio_notes.md` (internal
divergence ledger, says "Unity" out loud). Plus a `JsDispatch` refcount
leak fix (global+JSON refs were leaked per call → SIGABRT at
`JS_FreeRuntime` after enough `cairns.dispatch` calls).

None of these commits touch the windowed `sdl-min` `Engine::draw()` path.
Perf deltas vs `52d5d16` are pure thermal/system noise.

Workload: `100 GLBs × 33 slices = 3300 entities`, 11517 draws. Release.
Steady-state medians (warmup window dropped).

### macOS Metal Release — M2 Max, 1280×720 (2560×1440 HiDPI)
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  2.82 ms |
| `build_draws`   |  2.21 ms |
| `record`        |  1.45 ms |
| `particle_sim`  |  0.010 ms (GPU) |
| `forward`       |  9.10 ms (GPU) |
| `swap`          |  0.45 ms (GPU) |
| GPU total       | ~9.55 ms |

vs `52d5d16`: frame 2.87 → 2.82 (-0.05), build 2.23 → 2.21 (-0.02),
record 1.46 → 1.45 (-0.01), forward 9.27 → 9.10 (-0.17),
swap 0.46 → 0.45 (-0.01). Everything within thermal noise; net
−0.25 ms / frame combined, consistent with a slightly cooler run.

### macOS Vulkan (MoltenVK) Release — M2 Max, 1280×720 (2560×1440 HiDPI)
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  2.86 ms |
| `build_draws`   |  2.25 ms |
| `record`        |  0.63 ms |
| `particle_sim`  |  0.016 ms (GPU) |
| `forward`       |  9.08 ms (GPU) |
| `swap`          |  0.12 ms (GPU) |
| GPU total       | ~9.22 ms |

vs `52d5d16`: **the vk CPU regression is gone.** frame 4.42 → 2.86
(−1.56 ms), build 3.52 → 2.25 (−1.27 ms), record 1.00 → 0.63 (−0.37 ms).
The previous run's high CPU + high window-to-window variance was
thermal / system noise; this run lands back near the `c90a43b`
baseline (frame 3.20, build 2.55, record 0.72) — actually under it,
which is suspicious but reproducible across 4 windows here. GPU forward
9.38 → 9.08 (-0.30) within thermals; swap unchanged.

### Samsung S22 Vulkan Release — on-device, 2115×1008
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  8.61 ms |
| `build_draws`   |  7.53 ms |
| `record`        | 11.52 ms |
| `particle_sim`  |  n/a (Android Vulkan timestamps disabled, see f2625d1) |
| `forward`       | 90.24 ms (GPU) |
| `swap`          |  0.70 ms (GPU) |
| GPU total       | ~90.9 ms |

vs `52d5d16` S22: CPU side +0.83 ms (frame +0.83, build +0.74, record
+0.90); forward GPU 83.0 → 90.24 (+7.24 ms). Phone was charging on
USB-FAST again — Samsung thermal mgmt almost certainly throttling. The
prior entry's "re-run cold/non-charging" call still stands as the way
to disambiguate noise from a real regression. Three converged 120-frame
windows + warmup are consistent (forward 78 → 90 → 91 ms across windows,
the warmup was actually the coldest), so the throttled state is
reproducible.

### cairns_serve studio surface (not a perf gate, for record)
13-op NDJSON smoke test through `script.eval` + `cairns.dispatch`
roundtrip (Vector3 + Quaternion.Euler + GameObject.InstantiateAsync +
AddComponent + Camera.main loud-throw) runs in ~5 ms wall total. Studio
surface adds negligible per-call latency above the existing
`script.eval` path.

---

## `52d5d16` (2026-06-05) — headless editor mode P0–P5 + 0xCC heap garbage init

Full headless-editor-mode plan landed P0–P5: CMake split (`cairns_core` +
`sdl-min` + `cairns_serve`), RHI surface lift via `InitConfig`,
`CommandRegistry` + NDJSON stdio + `[Timer]` prefix on stderr, surfaceless
`Device::Init`, `Engine::GreaterInit({.surfaceless=true})` end-to-end,
`final_target_` + `render.frame` (clear-only) + `io.dumpTexture`,
`perf.last` + `rng.seed` engine-bound + stdin agent transport on
`cairns_app`, QuickJS vendored + `script.eval` + `cairns.dispatch`, P2
scene/viewport/window ops (stub-first surface; `window.resize` real),
windowed `io.dumpTexture target=window`. Plus defensive 0xCC garbage
init on every allocated GPU heap byte (Metal + Vulkan; `CAIRNS_HEAP_ZERO=1`
falls back to 0x00).

Workload: `100 GLBs × 33 slices = 3300 entities`, 11517 draws. Release.
Steady-state medians over multiple 120-frame windows (warmup window
dropped).

### macOS Metal Release — M2 Max, 1280×720 (2560×1440 HiDPI)
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  2.87 ms |
| `build_draws`   |  2.23 ms |
| `record`        |  1.46 ms |
| `particle_sim`  |  0.011 ms (GPU) |
| `forward`       |  9.27 ms (GPU, offscreen) |
| `swap`          |  0.46 ms (GPU, composite + PIP + ImGui) |
| GPU total       | ~9.74 ms |

vs `c90a43b`: `frame` +0.08 ms, `build_draws` +0.06 ms, `record` +0.03 ms
— all within thermal noise. `forward` 10.3 → 9.27 ms (~1.0 ms drop; this
is a real win, attributing to the 0xCC pre-fill on heap blocks
sometimes nudging the driver to commit pages eagerly — unproven, may be
noise). `swap` 0.31 → 0.46 ms (~0.15 ms regression; possibly the
init-time blit fillBuffer side effect noted in the V-METAL memory
file). Headless-editor-mode commits are all gated to `cfg.surfaceless`
in the windowed code path, so they shouldn't touch `sdl-min`'s steady
state. Net: GPU total -0.9 ms, CPU total +0.17 ms.

### macOS Vulkan (MoltenVK) Release — M2 Max, 1280×720 (2560×1440 HiDPI)
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  4.42 ms |
| `build_draws`   |  3.52 ms |
| `record`        |  1.00 ms |
| `particle_sim`  |  0.015 ms (GPU) |
| `forward`       |  9.38 ms (GPU) |
| `swap`          |  0.12 ms (GPU) |
| GPU total       | ~9.52 ms |

vs `c90a43b`: `frame` 3.20 → 4.42 ms (+1.22 ms); `build_draws` 2.55 →
3.52 ms (+0.97 ms); `record` 0.72 → 1.00 ms (+0.28 ms). **CPU-side
regression** on vk; `forward`/`swap` GPU within thermals (forward
9.07 → 9.38 ms, swap 0.036 → 0.12 ms). vk run had visibly higher
variance window-to-window (frame ranged 3.93–4.96 ms across 4 windows).
Possible explanations to chase: the 0xCC fill of the 88MB Vulkan bump
heap at allocation (one-time, shouldn't affect steady), the wider
include surface in cairns_core after the CMake split widening
compilation-unit boundaries that the linker handles differently, or
genuine thermal/system noise from the heavier headless-editor TUs.
Worth a focused investigation when the next round of vk work lands.

### Samsung S22 Vulkan Release — on-device, 2115×1008
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  7.78 ms |
| `build_draws`   |  6.79 ms |
| `record`        | 10.62 ms |
| `particle_sim`  |  n/a (Android Vulkan timestamps disabled, see f2625d1) |
| `forward`       | 83.0 ms (GPU) |
| `swap`          |  0.66 ms (GPU) |
| GPU total       | ~83.7 ms |

vs `c90a43b` S22: CPU side -0.6 ms across the three CPU timers (frame
8.10 → 7.78, build 7.12 → 6.79, record 10.72 → 10.62). **`forward`
regressed 56.5 → 83.0 ms (+47%) — significant**, and `swap` 0.43 → 0.66
(+0.23 ms). The `c90a43b` entry explicitly called itself the
"cold end of the thermally-noisy range" — this run is plausibly the
warm end, but two converged 120-frame windows (82 ms and 84 ms) and
a warmup window (96 ms) is a consistent profile not a one-off spike.
Phone was charging during the run (potential heat source); device
sat at 92% battery, USB-FAST charging — Samsung's thermal mgmt may
have throttled the GPU. **Re-run when next at the bench with cooler
device + non-charging state to confirm.** If the regression holds
cold, suspects to chase: anything that increased per-draw bandwidth
or shader instruction count between `c90a43b` and `52d5d16` — but
the only engine-side changes shipped are gated to `cfg.surfaceless`
so this is unexpected.

APK built from `third_party/SDL/android-project/` via
`./gradlew assembleRelease`. GLBs adb-pushed to
`/sdcard/Android/data/org.libsdl.app/files/` (100 files, ~150 MB).

### cairns_serve headless smoke (not a perf gate, for record)
NDJSON-driven `render.frame` (clear-only) + `io.dumpTexture("final")`
at 1280×720 produces a byte-identical 36970-byte PNG on metal and vk in
~25 ms wall round-trip (single command-buffer submit + waitUntilCompleted /
vkQueueWaitIdle + readback + stbi_write_png). Not directly comparable
to the windowed numbers — separate code path through
`Engine::RenderHeadlessFrame` / `DumpFinalTarget`, no render thread.

---

## `c90a43b` (2026-06-04) — EnTT scene layer landed (P0–P8 done; iOS Debug refreshed)

Full P0–P8 sequence of the EnTT scene-layer plan is in. Engine drives
the active world through `ResourceManager<World>` + `entt::registry`;
old `SceneEntity`/`SceneWorld` deleted; `PropagateTransforms` ready
(no-op pre-Transform authoring); a second world is open alongside the
active one (isolation gate green); skin wiring is deferred per audit.

Workload: `100 GLBs × 33 slices = 3300 entities`, 11517 draws. Release.
Steady-state medians over multiple 120-frame windows.

### macOS Metal Release — M2 Max, 1280×720
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  2.79 ms |
| `build_draws`   |  2.17 ms |
| `record`        |  1.43 ms |
| `particle_sim`  |  0.011 ms (GPU) |
| `forward`       | 10.3 ms (GPU, offscreen) |
| `swap`          |  0.31 ms (GPU, composite + PIP + ImGui) |
| GPU total       | ~10.6 ms |

`build_draws` dropped vs `042ebec` (2.84 → 2.17 ms) — EnTT view
iteration is leaner than the `std::vector<SceneEntity>` walk + manual
`scene_index` lookup that the old `Extract` did. `forward` /
`record` /` swap` all within prior thermals.

### macOS Vulkan (MoltenVK) Release — M2 Max, 1280×720
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  3.20 ms |
| `build_draws`   |  2.55 ms |
| `record`        |  0.72 ms |
| `particle_sim`  |  0.013 ms |
| `forward`       |  9.07 ms |
| `swap`          |  0.036 ms |
| GPU total       | ~9.12 ms |

Same MoltenVK-faster-than-native-Metal pattern from `042ebec` holds:
slimmer recorder path (`record` 0.72 vs 1.43 ms), tighter MSAA-resolve
on the swap pass.

### Samsung S22 Vulkan Release — on-device, 2115×1008
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  8.10 ms |
| `build_draws`   |  7.12 ms |
| `record`        | 10.72 ms |
| `particle_sim`  |  n/a (Android Vulkan timestamps disabled, see f2625d1) |
| `forward`       | 56.5 ms (GPU) |
| `swap`          |  0.43 ms (GPU) |
| GPU total       | ~57.0 ms |

S22 today settled at the "cold" end of the thermally-noisy range
documented at `042ebec` — `forward` 56.5 ms, well below the
steady-state ~120 ms observed in some warmer sessions. **Apply the
same thermal caveat**: a single reading on this device names a point
in the [~57, ~120] ms band, not a fixed steady-state. The pattern of
the EnTT path being within thermal noise of the post-render-graph
baseline holds.

### iOS Metal Release — iPhone 15, native res
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  7.12 ms (CPU, game thread post-Acquire) |
| `build_draws`   |  6.49 ms |
| `record`        |  4.53 ms |
| `gpu_frame`     | 34.87 ms (overall GPU; sum of PassTimer slots) |
| `swap`          |  0.93 ms (GPU) |
| `forward`       | ~33.94 ms (implicit = gpu_frame − swap; see note) |
| total           | 34.76 ms CPU / 29 FPS |

`forward` and `particle_sim` don't surface as their own overlay rows on
iOS: per `feedback-no-metal-timestamps`, the device lies about
`MTLCounterSampleBuffer` support and the codebase falls back to
`cmdbuf->GPUStartTime/EndTime` in the completion handler — which gives
accurate per-command-buffer timings but not per-pass within a single
cmd-buf. `gpu_frame` sums the populated `SlotForPass` slots, so it
captures the whole frame; `swap` is its own cmd-buf (composite + PIP +
ImGui), and the residual is `forward` + `particle_sim`. Particle sim
is sub-millisecond on every other platform so the implicit `forward`
≈ 33.94 ms is accurate to within noise.

iOS at this workload is GPU-bound (`gpu_frame` 34.87 vs CPU `frame`
7.12 ms): 29 FPS = ~34.5 ms/frame wall-clock, set by GPU not CPU.
That matches the iPhone 15 result documented at `9c8356e`
(prior-baseline ~32 ms forward at 1280×720 in earlier benches; this
run is at the iPhone's native screen res which is ~2.8× pixels).

### iOS Debug build env

`cmake -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCAIRNS_GFX_BACKEND=metal -B
build/ios` configures clean. `xcodebuild -sdk iphoneos -destination
'generic/platform=iOS' build CODE_SIGN_IDENTITY="" CODE_SIGNING_REQUIRED=NO`
links cleanly. Simulator build hits an architecture-define mismatch:
`define.hpp` keys `CAIRNS_APPLE` off `__APPLE__ && __aarch64__`, which
is false for x86_64-iphonesimulator → `CAIRNS_METAL=0` → `SwapChain`
has no body. Fix when next needed: drop the `__aarch64__` requirement
in `define.hpp`, or force `ARCHS=arm64` for simulator builds.

### Observations vs. `042ebec`
- `build_draws` is ~25% faster (M2 Max metal) on the EnTT path. The
  EnTT view's contiguous storage + tight component handling beats the
  legacy `std::vector<SceneEntity>::iterator` + `scenes_[scene_index]`
  indirection. Same effect on MoltenVK (smaller margin).
- `forward` / `swap` unchanged within thermals on all three platforms.
- Per-platform `gpu_frame` total (sum of `particle_sim` + `forward` +
  `swap`) is the same as `042ebec` within noise; the scene-layer
  rewrite is forward-time-neutral, as designed.

---

## `042ebec` (2026-06-04) — RecordFrame routed through render graph (forward → swap)

Frame is now graph-routed: `particle_sim` (kCompute) → `forward` (offscreen
single-sample color + depth) → `swap` (composite full-screen color +
bottom-right depth-silhouette PIP + ImGui, all in one MSAA swap encoder).
`forward` is no longer the swap pass — it writes to an offscreen color +
depth pair that `swap` samples. Adds one full-screen write + sample-back
vs the pre-graph single-pass shape.

Workload: `100 GLBs × 33 slices = 3300 entities`, 11517 draws. Release. All
three readouts are steady-state medians over many 120-frame windows
(post-thermal-warmup).

### macOS Metal Release — M2 Max, 1280×720
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  3.56 ms (game thread CPU post-Acquire) |
| `build_draws`   |  2.84 ms |
| `record`        |  1.73 ms (render thread CPU) |
| `particle_sim`  |  0.011 ms (GPU) |
| `forward`       | 10.5 ms (GPU, offscreen) |
| `swap`          |  0.32 ms (GPU, composite + PIP + ImGui) |
| GPU total       | ~10.85 ms |

### macOS Vulkan (MoltenVK) Release — M2 Max, 1280×720
| Pass            | avg     |
|-----------------|---------|
| `frame`         |  3.07 ms |
| `build_draws`   |  2.45 ms |
| `record`        |  0.67 ms |
| `particle_sim`  |  0.013 ms |
| `forward`       |  8.82 ms |
| `swap`          |  0.039 ms |
| GPU total       | ~8.87 ms |

MoltenVK is faster than native Metal here: slimmer recorder path
(`record` 0.67 vs 1.73 ms) and tighter MSAA-resolve-into-tile lowering
(`swap` 0.04 vs 0.32 ms). Forward edge (8.82 vs 10.5 ms) is roughly
within thermals.

### Samsung S22 Vulkan Release — on-device, 2115×1008

S22 forward is thermally noisy. Across multiple back-to-back launches
of the same APK, `forward` settled at one of two regimes:

- **Cold/transient**: ~60 ms forward, ~0.46 ms swap, total ~60.5 ms.
  Observed several times in the minutes after a fresh launch. Looked
  stable for 5–10 timer windows (~10 s each) before drifting.
- **Steady-state**: ~120 ms forward, ~0.98 ms swap, total ~121 ms.
  This is the rate the device holds once thermals settle, and matches
  the pre-graph `6386768` baseline within noise.

| Pass            | cold (~10 s window) | steady-state |
|-----------------|---------------------|--------------|
| `frame`         |  8.7 ms             |  9.0 ms      |
| `build_draws`   |  7.6 ms             |  7.7 ms      |
| `record`        | 11.5 ms             | 12.1 ms      |
| `particle_sim`  |  n/a (Android Vulkan timestamps disabled, see f2625d1) |  n/a   |
| `forward`       | 60 ms               | 121 ms       |
| `swap`          |  0.46 ms            |  0.98 ms     |
| GPU total       | ~60.5 ms            | ~122 ms      |

The original entry in this section reported only the cold reading.
Treat S22 forward as a range, not a point — quoting one number without
the warmup state attached is misleading on this device.

The graph routing itself is GPU-time-neutral on S22 within thermal
noise. An A/B that bumped `unlit_offscreen_` + `particle_render_offscreen_`
pipelines and the offscreen color/depth attachments back to MSAA-4
(same shape as the pre-graph swap renderpass) produced forward ≈ 62 ms
in the cold regime — same window as single-sample. MSAA is not the
load-bearing factor on Adreno here either.

### Observations
- New GPU rows (`particle_sim`, `forward`, `swap`) replace the
  pre-graph single `forward` pass that did everything. Splitting forward
  out makes the next levers (LOD for the binner term, alternate composite
  shapes) targetable independently.
- ImGui no longer has its own `ui` row — it's a tail draw inside `swap`
  for MSAA swap-renderpass reasons (see commit message for the
  storeOp=DONT_CARE / StoreActionMultisampleResolve constraint). The
  graph still expresses it as the read-from-`color_off`/`depth_off`
  consumer; the encoder boundary is the merge point.
- The `6386768` "geometry-bound" attribution should be read with a
  grain of salt — the tiny-quad test reduced both triangle count AND
  pixel coverage simultaneously, so it doesn't cleanly separate binner
  from per-pixel rasterizer work. Cost on this scene is likely a mix of
  both plus the ~30 ms submission floor; future levers (LOD, instancing
  if/when we allow it, deferred culling) attack the per-triangle term
  and should each be re-measured rather than assumed.

---

## `6386768` (2026-06-04) — tiny-quad diagnostic isolates geometry vs draw-submission

S22 Android Vulkan Release. CAIRNS_TINY_QUAD=1 pins every draw's
`triangle_count = 2`. Draw count + submission identical (11517 draws);
geometry throughput ~700× smaller.

| Pass                | Baseline (tiny=0) | Diagnostic (tiny=1) |
|---------------------|-------------------|---------------------|
| `forward` GPU       | ~124 ms           | **30-37 ms**        |
| `record` (CPU)      | ~10.6 ms          | ~16.6 ms            |
| `build_draws` (CPU) | ~6.7 ms           | ~11.7 ms            |

**Interpretation** (per planning-Claude's diagnostic schema):
- 20-30 ms → geometry-bound
- 60-80 → both
- 120+ → submission-bound

30-37 ms lands cleanly in the geometry-bound zone. Decomposition:
- ~94 ms attributable to vertex/binner throughput on the 16.5M-triangle scene
- ~30 ms irreducible draw-submission floor at 11517 draws

Adreno's per-draw submission cost is real (~2.6 µs/draw at the floor =
~30ms/11517) but **not** dominant. The bulk is the per-triangle binner
load Adreno pays on every triangle regardless of coverage — exactly what
the planning Claude predicted when they pointed out small-on-screen
doesn't help on a tiler.

**Banned-lever-free lever**: LOD. Attacks the per-triangle term directly,
doesn't touch draw count, stays clear of instancing/MDI. Floors at the
~30 ms submission term — below that needs the deferred levers.

Diagnostic toggle stays in tree: `CAIRNS_TINY_QUAD=1` (env-gated;
Android-side requires patching the runtime since std::getenv doesn't see
intent extras).

---

## `fce2ade` (2026-06-04) — game/render thread split landed; APK asset loading

Workload: `100 GLBs × 33 slices = 3300 entities`, 11517 draws. Release.

**Critical timer note**: `frame` (slot 0) is now the **game-thread CPU work
post-`Acquire`** -- not wall-clock between frames. Wall-clock per frame
(which includes Acquire backpressure waiting for the render thread) is
reflected in the ImGui overlay's `CPU` row. In steady state CPU ≈
render-thread frame time; `frame` shows how much CPU headroom is left.

**Metal macOS (M2 Max), 1280×720 windowed:**
```
draws 11517 | 100 GLBs x 33 slices = 3300 entities | resolution 1280 x 720
slot 0 (frame):        avg  2771 us over 120 frames   (game-thread CPU only)
slot 1 (build_draws):  avg  2190 us over 120 frames
slot 2 (record):       avg  1209 us over 118 frames   (render-thread encode)
slot 3 (particle_sim): avg    10 us over 118 frames
slot 4 (forward):      avg  9399 us over 117 frames
```
`frame` dropped 16.85 → 2.77 ms (~6× game-thread headroom) vs single-threaded.
`forward` GPU unchanged. Vsync caps wall-clock at 16.67 ms.

**iPhone 15 Pro Release (screenshot, native ~2556×1179 landscape):**
```
CPU 39.59 ms  |  25 FPS
avg 33.55 ms  |  peak 39.59 ms
gpu_frame   34.89 ms
frame        7.45 ms        (game-thread CPU only)
build_draws  6.71 ms
record       4.56 ms
```
Threading visible: `frame` 7.45 ms (game-thread CPU work) vs wall-clock
~40 ms (Acquire-paced behind render+GPU). gpu_frame unchanged from prior
single-threaded reading.

**Samsung S22 (SM-S901U, Adreno 730) Android Vulkan Release (screenshot, native 2115×1008 landscape):**
```
CPU 159.96 ms |  6 FPS
avg 154.14 ms |  peak 176.32 ms
gpu_frame    121.67 ms
frame          7.58 ms       (game-thread CPU only)
build_draws    6.66 ms
record        18.69 ms
```
gpu_frame 121.67 ms (vs ~134 ms pre-split — within noise; MSAA storeOp
change in `swap_chain.hpp` had no measurable effect, Adreno driver already
optimizes it). Forward pass remains the bottleneck. Per-draw cost ≈
122 ms / 11517 = 10.6 µs/draw on Adreno vs 35.5 / 11517 = 3.1 µs/draw on
Apple Silicon — ~3.4× driver-frontend gap. Not bandwidth (MSAA samples
are throwaway, fragment shader is trivial). Most likely candidate is
Adreno's per-draw binner/dyn-offset cost; banned-lever-free way to test
is mesh-primitive merging at scene load (collapses redundant draws within
a glTF), which we haven't tried yet.

---

## `c311cd7` (2026-06-04) — full readout + Android bisect

Workload: `100 GLBs × 33 slices = 3300 entities`, 11517 draws. Release.

**Metal macOS (M2 Max), 1280×720 windowed:**
```
draws 11517 | 100 GLBs x 33 slices = 3300 entities | resolution 1280 x 720
slot 0 (frame):        avg 16692 us over 120 frames
slot 1 (build_draws):  avg  2363 us over 120 frames
slot 2 (record):       avg  1148 us over 120 frames
slot 3 (particle_sim): avg    11 us over 120 frames
slot 4 (forward):      avg  9842 us over 119 frames
```
gpu_frame ≈ 9.85 ms. Frame vsync-capped at 16.69 ms = 60 FPS.

**Vulkan macOS (MoltenVK, M2 Max), 1280×720 windowed:**
```
draws 11517 | 100 GLBs x 33 slices = 3300 entities | resolution 1280 x 720
slot 0 (frame):        avg 16643 us over 120 frames
slot 1 (build_draws):  avg  2153 us over 120 frames
slot 2 (record):       avg   457 us over 120 frames
slot 3 (particle_sim): avg    16 us over 120 frames
slot 4 (forward):      avg 11033 us over 120 frames
```
gpu_frame ≈ 11.05 ms. Frame vsync-capped at 16.64 ms = 60 FPS. `record` is
4.4× lower than Metal — MoltenVK is doing more work on the GPU side
(forward 11.0 vs Metal 9.8) but less on the CPU encode side.

**iPhone 15 Pro Release (screenshot, native ~2556×1179 landscape):**
```
CPU 46.66 ms  |  21 FPS
avg 34.12 ms  |  peak 55.84 ms
gpu_frame     35.57 ms
frame         31.65 ms
build_draws    7.87 ms
record         3.06 ms
```
Thermally throttled into the low 20s after a few seconds; cold start hits
~28 FPS. Forward GPU dominates as on macOS but at 2.6× the pixel count.

**Samsung S22 (SM-S901U, Adreno 730) Android Vulkan Release (photo, native 2268×1080 landscape):**
```
CPU 165.96 ms |  6 FPS
avg 187.76 ms |  peak 267.61 ms
gpu_frame    133.85 ms
frame        188.83 ms
build_draws    5.97 ms
record         8.95 ms
```
~4× slower than iPhone 15 Pro on gpu_frame at similar pixel count.

### Android regression bisect (vs `f2625d1` baseline gpu_frame 128.78 ms → 140 ms)

User-driven bisect of the 9 commits in `f2625d1..c311cd7`. Two Vulkan-touching
candidates were prime suspects; both **exonerated**:

- `ab789d9` (vk bump: one VkDeviceMemory + HOST_COHERENT collapse) — **129 ms, not the culprit**.
- `73e1876` (swap_chain preTransform = IDENTITY, WSI rotates) — **127 ms, not the culprit**.

**Cause**: `2abd6af` raised the Android ImGui scale cap from 1.5× to 2.5×.
The overlay panel grows ~2.78× in pixel area (`ScaleAllSizes(2.5)`), which
costs ~10 ms in `forward` on Adreno's fragment pipeline. **Trade accepted,
not reverting** — readable overlay is worth the 10 ms.

---

## `f2625d1` (2026-06-03) — gpu_frame row, per-pass GPU timing landed

Workload: `100 GLBs × 33 slices = 3300 entities`, 11517 draws. Release.

**Metal macOS (M2 Max), 2556×1179:**
```
draws 11517 | 100 GLBs x 33 slices = 3300 entities | resolution 2556 x 1179
slot 0 (frame):                       accum 3759220 us, avg 31326 us over 120 frames
slot 1 (build_draws):                 accum  943232 us, avg  7860 us over 120 frames
slot 2 (record):                      accum  363615 us, avg  3030 us over 120 frames
slot 3 (set up render pass globals):  accum       3 us, avg     0 us over 120 frames
slot 4 (build opaque draw list):      accum  942902 us, avg  7857 us over 120 frames
slot 5 (particle_sim):                accum     285 us, avg     2 us over 120 frames
slot 6 (forward):                     accum 4265284 us, avg 35544 us over 120 frames
```

**iPhone 15 Pro (screenshot):**
```
CPU 25.65 ms | 39 FPS    avg 34.62 ms | peak 53.72 ms
gpu_frame              35.56 ms
frame                  30.81 ms
build_draws             8.03 ms
record                  3.06 ms
set up render pass globals  0 ms
build opaque draw list  8.03 ms
particle_sim            0.00 ms
forward                35.56 ms
```

**Samsung S22 (SM-S901U) Android Vulkan (screenshot):**
```
CPU 138.40 ms | 7 FPS    avg 35.53 ms | peak 457.9? ms
gpu_frame             128.78 ms
frame                 133.72 ms
build_draws             9.81 ms
record                 10.37 ms
set up render pass globals  0
build opaque draw list  9.57 ms
particle_sim            0.00 ms
forward               128.78 ms
```
~4× slower than iPhone 15 Pro on forward GPU (128.78 vs 35.56 ms).
Frame 133 ms = 7 FPS. Adreno 730 vs A17 Pro on this workload — fragment
throughput dominates as expected.

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

---

## 2026-06-21 — SDL windowed, post cpu_block_ compaction + string interning

Apple-silicon Mac, **SDL windowed** (real swapchain, 1280×720 window → 2560×1440
retina), **Release**, **500 actors (100 GLBs × 5)**, the run.js boot scene. Numbers
read live via `cairns.perf.last` after ~12 s of rendering. State = end of the #229
arena work (all CPU state in `cpu_block_`, prefab tables interned to `prefab_arena_`).
Windowed teardown now exits 0 (the 100-GLB oversize-Free use-after-free is fixed).

**Measured on commit `792998b`** (`perf: note skinning_compute 5.4ms is idle-clock`).

NOTE: the `frame` slot here is **CPU frame work only** (build+record+dispatch),
NOT the old vsync-inclusive frame time — present pacing is a separate slot now.
Thermally sensitive (±2–3× across runs on this throttling laptop); single snapshot.

### Metal (Release, windowed, 60 Hz vsync)
```
slot 0  frame              1430 us    (CPU frame work)
slot 1  build_draws        1231 us
slot 2  record              934 us
slot 3  skinning_compute   5444 us    (GPU compute -- 500 skinned actors, heaviest)
slot 4  particle_sim        109 us
slot 5  forward_vp0        1820 us    (GPU forward raster)
slot 6  swap                128 us
slot 7  present_pacing    10252 us    (vsync lock -- CPU idle here, fills to 16.6 ms)
slot 8  skin_eval            46 us
slot 9  present_wait          0 us
```
CPU frame work ~1.4 ms; vsync-locked at 60 fps (present_pacing absorbs the slack).
GPU skinning_compute (5.4 ms) is the dominant real cost.

### Vulkan / MoltenVK (Release, windowed)
```
slot 0  frame              1639 us    (CPU frame work)
slot 1  build_draws        1385 us
slot 2  record              374 us    (vk records far cheaper than metal's 934)
slot 3  skinning_compute   5404 us
slot 4  particle_sim          4 us
slot 5  forward_vp0        2088 us
slot 6  swap                 97 us
slot 8  skin_eval            51 us
slot 9  present_wait         31 us
slot 10 acquire_wait         19 us
slot 11 fence_wait            2 us
```
Different present model: MoltenVK swapchain (`present_wait`+`acquire_wait`+`fence_wait`
≈ 52 µs) vs Metal main-thread `present_pacing` (≈ 10 ms). CPU frame work ~1.6 ms.

### Takeaways
- **GPU skinning (`skinning_compute` ~5.4 ms)** is the single biggest cost on both
  backends — 500 skinned actors through the anim_eval/palette compute path. NOTE:
  5.4 ms is misleadingly high — the GPU isn't really engaged here (low clock /
  power-saving state). Profiled with the GPU forced to maximum, `skinning_compute`
  is **~2 ms**. Treat the 5.4 ms as an idle-clock reading, not the real ceiling.
- CPU frame work is cheap (~1.4–1.6 ms); `build_draws` dominates it (~1.2–1.4 ms).
- Metal `record` (0.93 ms) is ~2.5× vk `record` (0.37 ms).
- Present accounting differs by backend (Metal main-thread pacing vs MoltenVK
  swapchain) — compare CPU/GPU work slots across backends, not the present slots.

### Samsung Galaxy S22 (SM-S901U, Adreno 730) — Android Vulkan, Release

**100 actors** (100 GLBs × 1 — mobile renders 1/5th the desktop instances via the
platform-aware `cairns.instancePasses`; the Adreno tile budget can't take 500
skinned actors). Read from logcat `[Timer]` after settle. Same 100 GLBs loaded as
desktop. The GLB load took **9.1 s** on the phone.

**Measured on commit `4a5e39d`** (`mobile: cpu_persistent 256->512MB; cpu* stay
malloc`) — the native `.so` deployed to the device was built from that tree.
```
slot 0  frame              9108 us    (CPU frame -- but mostly GPU-fence-bound, below)
slot 1  build_draws        2992 us
slot 2  record             4208 us
slot 3  skinning_compute   8994 us    (GPU compute -- idle-clock; see note)
slot 4  particle_sim          6 us
slot 5  forward_vp0        3888 us    (GPU forward raster)
slot 6  swap                477 us
slot 8  skin_eval           222 us
slot 9  present_wait       5279 us
slot 10 acquire_wait       2264 us
slot 11 fence_wait         9155 us    (CPU stalls on the GPU fence -- GPU-bound)
```
`fence_wait` (9.2 ms) ≈ `frame` (9.1 ms): the CPU spends the frame waiting on the
GPU. GPU work (`skinning_compute` 9.0 ms + `forward_vp0` 3.9 ms) is the bottleneck,
and `skinning_compute` is again an **idle-clock** reading (Adreno power-saving) —
expect it well under this with the GPU pinned to max, same as the desktop ~5.4→2 ms
delta. `record` (4.2 ms) is notably heavier than desktop vk (0.37 ms).

**Memory (the point of the #229 arena work):** `[CPU-BLOCK] in_use = 173 MB /
512 MB budget` (33%), `[PREFAB-ARENA] 58 MB / 96 MB`. The mobile `cpu_persistent`
budget was raised 256 → 512 MB (256 MB was a conflation with the Adreno SSBO
`max_storage_buffer_range` floor — the CPU arena isn't bound by it). The transient
mesh cpu* (~600 MB across the batch) stay on malloc, not the block. Boots clean,
no OOM/abort.
