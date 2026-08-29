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
