# FLAKY_TESTS.md

Running record of non-deterministic / flaky golden tests, per platform, so we
can deal with them deliberately later. A "flake" here = a test whose
pass/fail (or image hash) changes run-to-run with no code change, OR a baked
ref that no longer matches a correct render. Updated 2026-06-20: imgui.ini
nondeterminism fixed; metal three_champ_static GPU flake found + characterized.

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

- **`[scenarios]` full-suite shared-state flake (#9b)** — PARTIALLY RESOLVED.
  The imgui run-to-run nondeterminism (`imgui.ini`) and the JS `Vector3` bleed
  are fixed (`io.IniFilename = nullptr` + `ResetJsContext`). The dominant
  remaining full-suite flake is NOT shared CPU state — it is the GPU
  subject-render flake (three_champ_static on metal, subjects on vk; see
  per-platform sections + TODO #2), which proves out as bistable and GPU-side.
  README guidance still holds: run `[subject]` isolated for a stable gate until
  TODO #2 lands.

---

## macOS metal (`macos-metal`) — the trusted gate

- **G6 `imgui.overlay`** (`[scenarios]` / `[imgui]`): the run-to-run
  nondeterminism was `imgui.ini` — nothing set `io.IniFilename`, so the HUD
  window's `SetNextWindowPos(..., FirstUseEver)` deferred to whatever position a
  prior run had saved on disk. FIXED 2026-06-19: `EnsureImguiContextImpl`
  (tests/test_seams.cpp) sets `io.IniFilename = nullptr`; solo G6 is now
  run-to-run deterministic (`a9e932d1…`) and no stray `imgui.ini` is written.
  REMAINING (fragility, not a flake): the committed ref `114e543591…` was baked
  in full-suite order and only matches in full-suite — solo renders a different
  (clean) hash because the *shared* imgui context's font atlas is built by the
  FIRST scenario's engine, not G6's. Robust fix = fresh imgui context per
  SCENARIO (`ResetImguiContextImpl`, verified it no longer aborts), then re-bake
  to `a9e932d1…`. NOT shipped: per-scenario recreate re-uploads the atlas every
  scenario, and that GPU-allocation churn rides the same path as the
  three_champ flake below — deferred behind it (do TODO #2 first, then this is
  free).
- **three_champ_static.f09 render flake (~1/6 in full `[scenarios]`)** —
  CORRECTION: metal is NOT fully deterministic. The three-static-champions
  subject occasionally renders a different image (observed `5ab2c425…` vs ref
  `cfee9552…`) in full-suite; STABLE when `[subject]` is run isolated (0/15).
  Proven properties: **bistable** (always that one wrong hash, never random
  garbage), **GPU-side** (unchanged under `MallocPreScribble=1` → not CPU
  uninitialized memory), and amplified by cross-scenario GPU-allocation churn.
  Same class as the macOS-vk subject flake. REJECTED hypothesis: adding a
  render-graph `skinning_compute`→`forward` read-dependency barrier on the skin
  output pool did NOT fix it (still ~4/30) → not the skin-pool vertex-fetch
  ordering. Root cause still open — see TODO #2.
- **G1 `particles.state`** (`[particles]`): stable isolated; passed full-suite
  consistently after the `.ini` fix + the earlier `ResetJsContext` work.
  Per-platform ref (Release FP differs from Debug).
- Everything else (one_die, two_die, viking_room, stress, hot_reload, nested,
  two-scene, frustum) is **deterministic** on metal.

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

1. **#9b imgui robustness** (mostly done): JS context reset (`ResetJsContext`)
   + `io.IniFilename = nullptr` have landed. Remaining: flip
   `EnsureImguiContext` to per-scenario `ResetImguiContextImpl` and re-bake G6
   to `a9e932d1…` so it passes solo AND full-suite. Blocked on TODO #2 — the
   recreate's font-atlas re-upload amplifies the GPU flake.
2. **GPU subject-render flake** (metal three_champ_static + vk subject, same
   root): bistable, GPU-side (CPU uninit ruled out via `MallocPreScribble`,
   skin-pool barrier ruled out). Next: capture both renders under Metal GPU
   frame-capture / shader validation and diff. Prime suspect: a per-frame
   buffer written-then-read with no graph-tracked dependency (the particle
   `sim_out` is `WriteBuffer`-only with no graph reader — exact same pattern as
   the skin pool). Pin the resource, then add the dependency or zero-init it.
   One fix should cover both backends.
3. **Re-bake iOS + Android** once the JS-conversion drift is confirmed (same
   procedure as the 2026-06-19 vk re-bake), from a visually-verified state.
