## cairns-gfx: canonical implementation order

The order each subsystem MUST be brought up in, regardless of branch / fork /
rebase / cherry-pick churn. Each later layer depends on the earlier ones being
solid; do not skip ahead.

1. **RHI** — device, swapchain, command recorder, frames, single forward pass.
2. **Allocators** — A: bump arena (CPU). B: chunk allocator. C: range pool / OffsetAllocator. D: ResourceManager<T> + Hot/Cold. GPU side: single heap + single master buffer per memory type, bump ring for upload/dynamic. Batched upload (10 GLBs / batch) baked into LoadScenesGpu from day one.
3. **Profiling** — Timer slots (frame, build_draws, record, etc), printf report every 120 frames, cpu-ms history ring. GPU completion-handler timer slot on Metal.
4. **ImGui** — vendor imgui core + impl_sdl3, RHI-routed renderer (own pipeline + per-frame bump upload of vtx/idx + per-cmd scissor + font atlas as CreateTexture). FPS text + cpu-ms PlotLines overlay. Drawn at the end of the existing single forward pass — no separate composite pass required.
5. **Threading** — game / render thread split with `kFramesInFlight=2`. SPSC handoff via std::mutex + std::condition_variable ONLY (no semaphores, latches, barriers, shared_mutex, atomic wait/notify). Per-slot RenderGraph + draw-list containers so producer and consumer never share mutable state.
6. **RenderGraph / RenderProxies** — multi-pass graph (compute / depth-prepass / forward / composite / blur / depthviz / parallel branches), Extract from SceneWorld → RenderProxyArrays, draw build off proxies. Transient resource aliasing via OffsetAllocator over baked lifetimes.
7. **Scene layer** — SceneEntity / SceneWorld as ResourceManager<Hot/Cold>, packed live_entities for dense iteration. Light/Camera/Skin proxies on the same Hot/Cold pattern.
8. **Animation** — skinning. Persistent skin output pool (RangePool over a GPU buffer), per-instance joint palette via FrameArena, compute-pass deformation, skin_output bound as a vertex buffer to the existing mesh pipeline. (Tier 1 load-time joint inv-binds in BumpArena; Tier 2 persistent skin output in RangePool; Tier 3 per-frame palettes in FrameArena.)
9. **Physics** — broadphase + narrowphase + solver. CPU first; parallelism via the threading carve from step 5. Hot loops on flat arrays-of-structs-of-arrays in BumpArena.
10. **Scripting** — embedded VM for gameplay code. Calls into the systems above via thin C wrappers; no direct RHI/scene mutation from script threads.

Out of order = fragile. New subsystem? Find where it slots in vs this list, and only land it after everything below it (lower number) is healthy.

---

## SDL3 App From Source Minimal Example
This is a minimal example for building and using SDL3, SDL_Mixer, SDL_Image, and SDL_ttf_ from source 
using C++ and CMake. It also demonstrates setting up things like macOS/iOS
bundles.
See [src/main.cpp](src/main.cpp) for the code. 

### Building And Running
Are you a complete beginner? If so, read [this](https://github.com/Ravbug/sdl3-sample/wiki/Setting-up-your-computer)!
Otherwise, install CMake and your favorite compiler, and follow the commands below:
```sh
# You need to clone with submodules, otherwise SDL will not download.
git clone https://github.com/Ravbug/sdl3-sample --depth=1 --recurse-submodules
cd sdl3-sample
cmake -S . -B build
```
You can also use an init script inside [`config/`](config/). Then open the IDE project inside `build/` 
(If you had CMake generate one) and run!

### cairns-gfx: selecting the GPU backend (macOS)
Generate the Xcode project for a specific backend with `-DCAIRNS_GFX_BACKEND=metal|vulkan`:
```sh
cmake -B build/metal -G Xcode -DCAIRNS_GFX_BACKEND=metal    # Metal
cmake -B build/vk    -G Xcode -DCAIRNS_GFX_BACKEND=vulkan   # Vulkan (MoltenVK; run via ./run_vk.sh)
```

### Architecture (game + render thread)

Pipeline depth 2: the main (game) thread builds frame N+1 while the render
thread does Frames::Begin / Execute / End for frame N. Steady-state wall-clock
collapses to `max(game_build, render_cycle) ≈ vsync_interval` so we land at
60 fps so long as the GPU itself fits in vsync.

```
 main (game) thread                          render thread
 ────────────────────                        ──────────────────
 loop:                                       loop:
   SDL_PollEvents                              pkt = queue.Pop()    // blocks if empty
   ImGui NewFrame + UI + Render                Frames::Begin(res, alloc)
   FrameClock::Tick → sim step                   ↳ semaphore_wait + AdvanceFrame
   Extract → proxies (FrameArena)              Build per-draw UBOs from pkt
   BuildMeshOpaqueDraws → drawList             (rhi_.alloc.BumpAllocate ×N)
   Build RenderGraph (AddPass + Bake)          graph_.Execute(fc, swapchain_)
   FramePacket pkt{...}                          ↳ AcquireSwapchain inside composite
   queue.Push(pkt)                             Frames::End → submit + present
   queue.WaitIfFull(kFramesInFlight)           if pkt.dump_frame: do dump + signal
```

Ownership rules:
- Game thread: SDL, ImGui, sim, Extract, BuildMeshOpaqueDraws, render-graph
  build. No GPU bump-alloc, no `Frames::*`.
- Render thread: per-draw UBO bumps (one allocator on one thread), graph Execute,
  Frames::Begin/End. No sim, no ImGui, no resource Acquire/Release.
- Shared: `Resources::GetHot()` is read-only and safe. `Resources::Acquire` /
  `Release` are game-thread-only (load-time today; runtime spawns deferred).

Hand-off:
- Forward SPSC queue (game → render) of depth `kFramesInFlight = 2` carries
  `FramePacket*` allocated from the per-frame `FrameArena` slot.
- Parity-return SPSC queue (render → game) carries the post-pass particle
  parity for the next sim step.

Determinism: under `CAIRNS_DUMP`, the dump frame collapses to a lock-step
handshake so `scripts/verify_metal.sh` stays byte-identical to the existing
golden.

## Supported Platforms
I have tested the following:
| Platform | Architecture | Generator |
| --- | --- | --- |
| macOS | x86_64, arm64 | Xcode |
| iOS | x86_64, arm64 | Xcode |
| tvOS | x86_64, arm64 | Xcode |
| visionOS* | arm64 | Xcode |
| Windows | x86_64, arm64 | Visual Studio |
| Linux | x86_64, arm64 | Ninja, Make |
| Web* | wasm | Ninja, Make |
| Android* | x86, x64, arm, arm64 | Ninja via Android Studio |

*See further instructions in [`config/`](config/)

Note: UWP support was [removed from SDL3](https://github.com/libsdl-org/SDL/pull/10731) during its development. For historical reasons, you can get a working UWP sample via this commit: [df270da](https://github.com/Ravbug/sdl3-sample/tree/df270daa8d6d48426e128e50c73357dfdf89afbf)

## Updating SDL
Just update the submodule:
```sh
cd SDL
git pull
cd ..

cd SDL_ttf
git pull
```
You don't need to use a submodule, you can also copy the source in directly. This
repository uses a submodule to keep its size to a minimum.

## Reporting issues
Is something not working? Create an Issue or send a Pull Request on this repository!
