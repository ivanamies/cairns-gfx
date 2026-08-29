# studio surface notes — clone, new noun, refuse

**Internal doc.** Cites Unity by name because internal clarity > marketing
discretion.

**The naming rule (#225):** every symbol on the studio surface sorts into
exactly one of three bins. There is no "divergence" axis — and the
**Clone** and **New noun** columns are the entire decision procedure.

1. **Clone** — Unity has the concept → cairns uses Unity's exact name and
   shape, no asterisk. Transfer of Unity muscle memory is free.
2. **New noun** — Unity has no concept of it → cairns picks a clean new
   name *because there is no Unity word to be faithful to*.
3. **Refuse** — Unity has it, but it's a wart we don't clone. (Section
   below; canon list.)

If you're adding a new studio symbol, decide which bin first. If the
answer is "I want it to be Unity-but-with-an-asterisk," you're in the
wrong frame: split into Clone (the part Unity has) and New noun (the
part Unity doesn't), or move it to Refuse.

---

## 1. Clone — Unity name = cairns name, no notes

| Unity 2022.3 | cairns | lowers to |
|---|---|---|
| `Scene` (container of GameObjects) | `Scene` | `cairns.scene.*` |
| `Prefab` | `Prefab` | `cairns.prefab.*` |
| `GameObject` | `GameObject` | wraps `cairns.scene.instantiate` result |
| `Transform` | `Transform` | wraps `WorldTransform` component |
| `Component` | `Component` | wraps entt components |
| `Material` | `Material` | wraps `cairns::Material` |
| `Mesh` | `Mesh` | wraps `cairns::Mesh` |
| `Camera` | `Camera` | `cairns.viewport.*` for the binding |
| `Vector3` / `Vector2` / `Quaternion` | same | math, pure JS |
| `Mathf.{PI,Deg2Rad,Lerp,Clamp,…}` | same | math, pure JS |
| `Application.Quit` | same | `cairns.app.quit` |
| `Time.deltaTime` (getter) | same | `pkt.dt` from the FrameClock |
| `Random.Range` | same | active-scene RNG (`cairns.rng.*`) |
| `Resources.Load`-shape sync loader | `Prefabs.load(source, count, cursor)` | `cairns.prefab.loadBatch` |

That's the whole clone column. Anything Unity-named that isn't here
either lives under Refuse or doesn't have an analogue yet — open an
issue, don't invent a private name.

---

## 2. New noun — cairns concept Unity lacks

Unity has multi-scene editing (since 5.3) and additive runtime scenes,
so cairns holding several `Scene`s live is plain Unity. **It is not a
new noun.** Plural-active scenes are clone behavior used fully.

There is exactly ONE new noun on the surface — the multi-scene
compositor — and it lives under `Editor.*`:

| cairns | what | why no Unity name |
|---|---|---|
| `Editor.scenes` | the array of all live scenes (no implicit active scene) | Unity's `SceneManager` encodes single-active. cairns wants plural-active; using `SceneManager` would inherit the wrong shape. |
| `Editor.newScene()` | create a fresh `Scene` | NOT `SceneManager.LoadScene` — same reason. |
| `Editor.show(scene, viewport)` | bind a scene to a viewport pane | Unity's per-camera scene targeting is editor-internal, not a runtime API. |
| `Editor.thumbnails(scene, opts)` (#226 CAP-2 stub) | virtualized render-to-texture pool of generated previews | No Unity analogue. Throws `"not wired yet (#226 CAP-2)"` until #226. |
| `Editor.compose(scene, viewport, opts)` (#226 CAP-3 stub) | a viewport whose pixels are a shader function of OTHER viewports' rendered targets | **This is the core differentiator.** Unity additive scenes share one camera stack and render into one image; they are never independent render sources a third pass samples. Throws until #226 lands the pass. |
| `Scene.addCamera(...)` (#226 CAP-1 stub) | cameras as entities IN a scene (multi-camera per viewport with split) | Unity's camera stack is per-viewport-display, not per-scene-entity. Different shape. Throws until #226. |

The R6 not-yet-wired stubs follow the `Cairns.onFrame` precedent: the
names are reserved, the Unity prior is held off (because there isn't
one), and nothing silently half-works.

---

## 3. Refuse list (canon)

Unity APIs cairns does NOT ship under any name. If a Unity script
reaches for them, it gets `unknown_op`.

| Unity API | Reason refused |
|---|---|
| `MonoBehaviour` lifecycle: `Awake` / `Start` / `OnEnable` / `Update` / `FixedUpdate` / `LateUpdate` / `OnDisable` / `OnDestroy` / `OnGUI` | Engine exposes primitives, scripts compose policy. Per-frame work subscribes to `cairns.events.frame` (when wired); there is no `Update()` to override. |
| `SendMessage` / `BroadcastMessage` | String-based dynamic dispatch. Use explicit `cairns.dispatch` (typed via JSON Schema). |
| Tag strings + `GameObject.Find("name")` | Stringly-typed entity lookup. Use `EntityRef` / handle directly. |
| `AssetDatabase` (entire namespace) | Editor-only ceremony coupled to Unity's serialization model. Not a runtime API. |
| `SerializedProperty` + Editor extensions | Same. Editor authoring is a separate product surface. |
| Coroutines: `IEnumerator` + `yield return new WaitForSeconds(...)` | Wall-clock coupled; conflicts with the deterministic clock. |
| `PrefabUtility`, prefab variants, nested prefabs | Coupled to Unity's serialization model. Plain `Prefab` + `Scene.instantiate` is the cairns shape. |
| `InvokeRepeating` | Same as coroutines. |
| `PlayerPrefs` | Separate config-storage surface; expose via `cairns.config.*` if needed. |
| `OnGUI` / Unity IMGUI | Cairns has Dear ImGui through `cairns.imgui.*`. |
| Tag/Layer string-based filtering | Replace with explicit bitmasks or named-set membership. |
| **Global `Instantiate(prefab)` (silent active-scene target)** | The multi-scene wart: in multi-scene code it bites the moment you have >1 scene open, which is the cairns common case. `Scene.instantiate(prefab, pos)` carries the explicit scene target. Even Unity's own multi-scene docs route through `SceneManager.MoveGameObjectToScene` to undo the global — refusing the global is *more* faithful to Unity's own multi-scene guidance. |
| `SceneManager.LoadScene(name, Single | Additive)` | Encodes single-active-scene. Use `Editor.newScene()` + plural-active `Editor.scenes`. |
| `Camera.main`'s silent return-first-or-tagged | Unity returns the first tagged main camera silently. cairns `Camera.main` throws on 0 or >1 (loud). |
| `Time.deltaTime` setter | Unity allows mutating wall-clock state via Time; cairns puts that on `cairns.time.set(t)`. The getter is a clone; the setter is refused on Time itself. |

---

## QuickJS async limitation (engineering note)

`GameObject.InstantiateAsync` is deliberately a normal function that
returns the `GameObject` directly, NOT an `async` function and NOT one
that returns `Promise.resolve(...)`. Two reasons:

1. `await` on a non-Promise resolves to the value immediately, so call
   sites use `await GameObject.InstantiateAsync(...)` exactly as they
   would in Unity-shape JS. The `await` keyword sits visible in the
   source — when real async upload completion lands, the body becomes
   a true Promise return without changing call sites.
2. QuickJS's promise machinery leaves shutdown state that trips a
   refcount assert at `JS_FreeRuntime` time when scripts go through
   `cairns.dispatch` (reproduced at `757f552`; the global-object-ref leak
   in `JsDispatch` is fixed, further fixes deferred). Returning the
   value directly avoids the machinery entirely.

In #225 R4 the global `InstantiateAsync` is a deprecated alias (one
release). New code calls `scene.instantiate(prefab, pos)` — also sync,
also returns directly, same QuickJS reasoning.

---

## Versioning policy

**Pin Unity 2022.3 LTS** as the reference for cloned names + signatures.
**Never auto-follow.** The cloned core (scene graph, Transform,
Component, Camera, Material, math) is essentially version-invariant — it
has been stable for >15 years — so the pin barely matters for the ~120
core ops; it mainly fixes the reserved-tail vocabulary. Document any
deliberate drift here.

---

## Two-namespace rule

- `cairns.*` is the source-of-truth registry surface for cairns
  features. Every studio class lowers to one or more `cairns.*` ops via
  `cairns.dispatch`.
- `studio.*` (when ops live here directly) is reserved for adapter ops
  that have no clean cairns underlying op yet. **Default to lowering
  through `cairns.*`**; ship a top-level `studio.*` op only if the
  adapter logic exceeds ~5 lines of JS-side composition.
- `tools.*` is the meta layer (`tools.list`, `tools.search`). No ops
  migrate here; no studio names sit here.

If you're tempted to put real engine logic in `studio.*`, that's a
smell: the logic belongs in a `cairns.*` op.
