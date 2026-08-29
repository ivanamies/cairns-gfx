# studio surface notes — divergence ledger + refusals + cairns wins

**Internal doc.** Cites Unity by name because internal clarity > marketing
discretion. The user-facing surface (`studio.*` registry namespace,
`studio.js` adapter classes) is deliberately neutral — see `~/dev/plans/
2026-06-05_gfx_unity-shaped-op-surface.md` for the rationale ("clone the
shape for corpus density; be discreet about it").

This doc has three jobs, in priority order:

1. **The refuse list** — Unity ops we explicitly DO NOT ship under any name.
2. **The cairns wins** — architectural decisions kept un-renamed because we
   do them better than Unity (no customers yet ⇒ no compat tax).
3. **The divergence ledger** — Unity ops we DO ship under the same name but
   with different semantics, each with the LOUD form that makes the
   divergence visible (rename / runtime throw / await), never doc-only.

If you're trying to add a new `studio.*` op, read all three sections
first. The default for a new Unity op is "refuse" or "reserve" (namespace
+ `engine_status:unimplemented`), not "clone."

---

## 1. Refuse list (canon)

These Unity APIs do not exist under any name in `studio.*`. If a Unity
script reaches for them, it gets `unknown_op`. If a contributor proposes
adding one, this doc is the reason to push back.

| Unity API | Reason |
|---|---|
| `MonoBehaviour` lifecycle: `Awake` / `Start` / `OnEnable` / `Update` / `FixedUpdate` / `LateUpdate` / `OnDisable` / `OnDestroy` / `OnGUI` | Cairns win #7: engine exposes primitives, scripts compose policy. Per-frame work subscribes to `cairns.events.frame` (when wired); there is deliberately no `Update()` to override. |
| `SendMessage` / `BroadcastMessage` | String-based dynamic dispatch. Replace with explicit `cairns.dispatch` (already typed via JSON Schema). |
| Tag strings + `GameObject.Find("name")` | Stringly-typed entity lookup. Use `EntityRef` / handle directly; the registry is the lookup surface, not a global name pool. |
| `AssetDatabase` (entire namespace) | Editor-only ceremony coupled to Unity's serialization model. Not a runtime API. |
| `SerializedProperty` + Editor extensions | Same. Editor authoring is a separate product surface (not Day-1). |
| Coroutines: `IEnumerator` + `yield return new WaitForSeconds(...)` | Wall-clock coupled; conflicts with cairns win #1 (deterministic clock). Replace with deterministic-clock-aware scheduled tasks. |
| Prefab system (`PrefabUtility`, prefab variants, nested prefabs) | Too coupled to Unity's serialization model. "Scenes as data + EnTT spawning" is the cairns shape. |
| `InvokeRepeating` | Same issue as coroutines. |
| `PlayerPrefs` | Separate config-storage surface; expose via `cairns.config.*` if needed. |
| `OnGUI` / Unity IMGUI | Half-deprecated in Unity itself. Cairns has Dear ImGui through `cairns.imgui.*`; runtime UI is a separate planned arc. |
| Tag/Layer string-based filtering | Replace with explicit bitmasks or named-set membership. |
| Synchronous `Instantiate(prefab)` | Refinement 1 (the loud-divergence invariant): the sync name reserves Unity-sync semantics, which we don't honor. Only `InstantiateAsync` ships. Caller writes `await`; today the function returns directly so `await` resolves immediately, but the syntactic divergence is locked in. |
| Setters on Unity read-only properties (e.g. anything that would force surprising mutation semantics) | Forces the divergence into a differently-named cairns op. See `Time.deltaTime` row in the divergence ledger. |

---

## 2. Cairns wins (un-renamed, deliberately different)

These are decisions cairns makes that Unity makes worse. They keep
`cairns.*`-prefixed names; the studio surface does NOT paper over them.
When in doubt about hiding a cairns mechanism behind a Unity name: don't.

1. **Deterministic clock as a first-class mode.** `FrameClock` interface,
   `FixedClock` implementation, `CAIRNS_DUMP` ⇒ byte-identical PNG output
   across runs. Unity's `Time.fixedDeltaTime` is a hint; there is no
   project-level "produce the same pixels twice" mode. Surface as
   `cairns.time.set(dt)` / `cairns.time.fixedClock(true|false)`. The
   studio `Time.deltaTime` getter still works; setting time is a
   different name.
2. **Multi-world / EntityRef.** Explicit `WorldId` + `EntityRef`,
   `World::Hot`/`World::Cold` split. Unity has one global scene tree
   (`SceneManager`-as-hack). Studio `GameObject.Find` scopes to the active
   world; `cairns.world.*` is the explicit multi-world API.
3. **RHI abstraction with byte-identical metal+vk goldens.** Dev-time
   invariant: every commit byte-gates `sdl-min` on both backends. Unity's
   SRP doesn't promise pixel parity. Internal only — not surfaced.
4. **Bump-allocator + memory archetypes.** `Arena` / `ChunkAllocator` /
   `RangePool` / `ResourceManager<T>` are named in the engine; per-frame
   uploads go through a known bump ring. Unity hides this entirely
   behind GC. Expose introspection as `cairns.mem.*`; don't bury under
   Unity-style `Resources.totalAllocatedMemory`.
5. **`Handle<T>` + ResourceManager.** Typed generational handles;
   `handle.IsNull()` reflects destruction. Unity's `UnityEngine.Object` is
   a managed reference with overridden `==null`. Studio `GameObject ==
   null` returns false today (handle-staleness detection deferred); when
   it lands, it lowers to a generation check on the underlying handle.
6. **Headless-first / NDJSON control plane.** `cairns_serve` is a real
   binary; the registry + `script.eval` + agent stdin transport are the
   primary surface. Unity's `-batchmode` is an afterthought. Cairns wins
   here by design — the studio surface works identically in both shells.
7. **Engine exposes primitives, scripts compose policy.** The refuse list
   above is the practical expression of this win. Studio scripts call
   `cairns.dispatch` directly or use the studio wrapper classes; there
   is no MonoBehaviour to inherit from. `Cairns.onFrame(cb)` is the
   reserved channel for per-frame work (lands with the
   `event`/`subscribe` channel from the headless plan).
8. **EnTT-shaped events (when wired).** If we ever add per-entity hooks,
   they follow the EnTT signal/sink shape (`world.on<T>().connect`), not
   MonoBehaviour. Reserved namespace; no shape committed yet.

---

## 3. Divergence ledger (loud form, not doc-only)

Per Refinement 1 of the design: a divergence may live in the name or in a
runtime failure — **never only in the docs.** The ledger here is the
*explanation*; the loud form is the *enforcement*.

| Unity op | Studio shipping | Loud form | Reason |
|---|---|---|---|
| `Instantiate(prefab)` | `GameObject.InstantiateAsync(asset_id, world_id?)` | **Rename.** Sync `Instantiate` refused. Caller writes `await`; today the function returns directly so `await` resolves immediately, but the syntax pins the eventual asynchrony of spawn+upload. | Win #5 (Handle generations + future async upload completion). |
| `Camera.main` | `Camera.main` (single-instance-strict) | **Throw.** Returns the unique Camera component; throws if zero or >1 instead of Unity's silent return-first. | Eliminates the "I have multiple cameras and silently get the wrong one" footgun. |
| `Time.deltaTime` setter | getter only; setting is `cairns.time.set(t)` | **Rename.** Studio `Time.deltaTime` getter returns `pkt.dt`; no setter exists on `Time`. Setting clock state is a different name. | Win #1 (deterministic clock). |
| `Random.Range` | reads active-world RNG | **Scope-explicit.** Studio `Random.Range` always reads the active world's RNG; cross-world is `cairns.world.rng(world_id, ...)`. | Win #2 (multi-world); silent cross-world reads would be a Heisenbug. |
| `Application.Quit()` | `cairns.app.quit` | **None.** Semantics match. No divergence, no action. | — |
| `Cairns.onFrame(cb)` | reserved (not Unity-named) | **Throw.** Stub that throws "not yet wired" until the `event`/`subscribe` channel lands. | Surface reservation; prevents the eventual API from getting back-named to a refused MonoBehaviour-style `Update()`. |

### QuickJS async limitation (engineering note)

`InstantiateAsync` is deliberately a normal function that returns the
`GameObject` directly, NOT an `async` function and NOT one that returns
`Promise.resolve(...)`. Two reasons:

1. `await` on a non-Promise resolves to the value immediately, so call
   sites use `await GameObject.InstantiateAsync(...)` exactly as they
   would in Unity-shape JavaScript. The syntactic divergence (the `await`
   keyword sitting visible in the source) stays.
2. QuickJS's promise machinery leaves shutdown state that trips a
   refcount assert at `JS_FreeRuntime` time when scripts go through
   `cairns.dispatch` (reproduced 2026-06-05, root-caused to the global
   object ref leak in `JsDispatch` — fixed — and the leftover Promise
   tracking; further fixes deferred). Returning the value directly
   avoids the machinery entirely.

When the engine grows real async upload completion (asset upload + spawn
visible-next-frame), this becomes a real Promise return without changing
call sites. The `await` was the contract.

---

## Versioning policy

**Pin Unity 2022.3 LTS** as the reference for cloned names + signatures.
**Never auto-follow.** The cloned core (scene graph, Transform,
Component, Camera, Material, math) is essentially version-invariant — it
has been stable for >15 years — so the pin barely matters for the ~120
core ops; it mainly fixes the reserved-tail vocabulary. Document any
deliberate drift here in the ledger.

(Confirm the exact current LTS string at pin time. The choice of *which*
recent LTS is low-stakes precisely because the cloned surface is the
stable part.)

---

## Two-namespace rule

- `cairns.*` is the source-of-truth registry surface for cairns features.
  Every studio class lowers to one or more `cairns.*` ops via
  `cairns.dispatch`.
- `studio.*` (when ops live here directly) is reserved for adapter ops
  that have no clean cairns underlying op yet. **Default to lowering
  through `cairns.*`**; ship a top-level `studio.*` op only if the
  adapter logic exceeds ~5 lines of JS-side composition.
- `tools.*` is the meta layer (`tools.list`, `tools.search`). No ops
  migrate here; no studio names sit here.

If you're tempted to put real engine logic in `studio.*`, that's a smell:
the logic belongs in a `cairns.*` op.
