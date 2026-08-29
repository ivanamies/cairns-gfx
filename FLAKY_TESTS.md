# FLAKY_TESTS.md

Running record of non-deterministic / flaky golden tests, per platform, so we
can deal with them deliberately later. A "flake" here = a test whose
pass/fail (or image hash) changes run-to-run with no code change, OR a baked
ref that no longer matches a correct render. Updated 2026-06-19 during #229.

The deterministic correctness gate for #229 is **macOS metal isolated runs**.

---

## Cross-platform (all backends)

- **`[Studio] autoload FAILED: SyntaxError: redeclaration of 'Vector3'`** —
  printed (stderr) on essentially every `SetupJs`/scenario. The JS context's
  global `Vector3` (from the studio autoload / `assets/init.js`) is re-declared
  because globals accumulate across SCENARIOs instead of starting clean. Not a
  test failure by itself, but it is the **root cause of the #9b shared-state
  flakes** below (JS + ImGui static state bleeds between SCENARIOs in the
  single-process Catch2 binary). Fixing the per-scenario JS/ImGui reset most
  likely kills the #9b flakes too. Tracked in modularization-notes #9b.

- **`[scenarios]` full-suite shared-state flake (#9b)** — running the WHOLE
  `[scenarios]` set in one process flakes the ImGui- and particle-touching
  cases because ImGui has process-global static state and the JS context bleeds
  (above). README guidance: run `[particles]` and `[imgui]` (and `[subject]`)
  **isolated** for stable results. Affects metal + vk; presumed iOS/Android too.

---

## macOS metal (`macos-metal`) — the trusted gate

- **G6 `imgui.overlay`** (`[scenarios]` / `[imgui]`): currently FAILS even
  deterministically in full-suite (observed hash `d9b77b1a…` consistent across
  runs) and solo (the ref was baked in full-suite context — #9b says there is
  no single bake context that passes both solo and full-suite). Pre-existing,
  predates #229. This is the 1 failing assertion in the otherwise-green metal
  `[scenarios]` 96/97.
- **G1 `particles.state`** (`[particles]`): stable when run isolated; flakes in
  full-suite (#9b). Per-platform ref (Release FP differs from Debug).
- Everything else (subjects, stress, hot_reload, nested, two-scene, frustum) is
  **deterministic** on metal isolated runs.

## macOS vk / MoltenVK (`macos-vk`)

- **Intermittent subject render flake (~1 in 7 runs)** — `[subject]` (and
  therefore `[scenarios]`) occasionally produces a different image hash for ONE
  of the 6 subjects, **even when `[subject]` is run isolated**. Low-frequency,
  could not pin which subject (rare). Re-baking CANNOT fix this — the renderer
  itself is non-deterministic on vk. Suspected race / uninitialized GPU memory
  in the MoltenVK path (NOT the #229 CPU load change: metal is fully stable, the
  flake is intermittent, and it pre-dates #229). **Needs root-cause** (run under
  a GPU validation layer / zero-init suspect buffers). Refs were re-baked
  2026-06-19 so vk passes the majority of runs, but this flake remains.
- **G1 `particles.state`: SKIPs** on vk (not a flake — `ReadParticleBuffer` /
  `Resources::ReadBackBuffer` is a vk stub returning false). Gap, not flake.
- G6 imgui + full-suite #9b: same as metal.

## iOS sim metal (`ios-sim-metal`) / iOS metal (`ios-metal`)

- Refs last baked at commit `a9a97b1` (phase E), **before the JS conversion
  (`4f87f47`)**. Like vk was, these are very likely **stale** (test labels /
  scene composition changed) and will fail until re-baked on a booted simulator.
  Not re-verified this session. Treat as "unknown / probably stale," not
  confirmed-flaky.

## Android vk — device (`android-vk`) + AVD (`android-vk-emu`)

- Same as iOS: refs baked at `a9a97b1` pre-JS-conversion; **likely stale**, not
  re-verified. S22 device must be physically connected to bake `android-vk`;
  otherwise only the SwiftShader AVD (`android-vk-emu`) bakes. If vk's
  intermittent non-determinism is a MoltenVK-specific issue it may not appear on
  Adreno; if it's a generic vk zero-init bug it will. Unknown until tested.

---

## Related non-test known issue (not a flake)

- **`cairns_serve` exit 139 (SIGSEGV)** — two distinct PRE-EXISTING causes:
  - **unloadAll path: FIXED** (2026-06-19). `UnloadAllPrefabs` released pool
    slots + freed resources with no render-thread/GPU quiesce, so an in-flight
    frame drew freed meshes. Added `render_thread_->Drain()` +
    `device.WaitIdle()` guard; `load+instantiate+render+clear+unloadAll` exits 0.
  - **reload path: still open, DEFERRED to after #229** (TODO.md "Deferred").
    Hot-reloading a skinned prefab mid-render crashes ~kFIF frames later. Metal
    validation: `setBuffer ... offset(71202720) must be 0` — a skinning/anim
    compute kernel binds a stale ~71 MB offset. Confirmed identical at safepoint
    `21c893a`, NOT a #229 regression. Repro: `wC3.ndjson` under
    `MTL_DEBUG_LAYER=1`.

---

## TODO when we deal with these

1. **#9b root cause**: give each SCENARIO a clean JS context + reset ImGui
   static state (or destroy/recreate ImGui context per scenario without
   invalidating engine-held handles). Kills the cross-scenario bleed, the
   `Vector3` redeclaration spam, and the metal G6 + G1 full-suite flakes.
2. **vk intermittent subject flake**: run `[subject]` in a loop under a Vulkan
   validation layer; zero-init suspect GPU buffers; check for a missing barrier
   / fence in the MoltenVK submit path. Pin which subject first.
3. **Re-bake iOS + Android** once the JS-conversion drift is confirmed (same
   procedure as the 2026-06-19 vk re-bake), from a visually-verified state.
