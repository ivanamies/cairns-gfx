# Performance log

Numbers keyed by git commit. Workload, build mode, and platform notes inline.
Newest first.

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

### Samsung S22 Vulkan Release — not captured this round
Android device not attached during this measurement. Re-run from
`scripts/regenerate_perf.sh android` (or push + on-device run) when the
device is back online.

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
