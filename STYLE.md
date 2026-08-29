# cairns-gfx style

Project conventions. Edit when a new rule is established. Don't write a
"mistakes" log -- the rule lives here, applied going forward.

## Namespaces

All cairns namespaces are lower case. `cairns`, `cairns::rhi`,
`cairns::control`, `cairns::headless`, `cairns::scene`. Third-party
namespaces (e.g. `MTL`, `NS`, `CA`, `VK_*`, `ImGui`) keep their upstream
casing.

## Naming

- **No ordinals in names.** Never `verbTwo`, `OpenSecondViewport`,
  `secondary_scene_`, `SetupTwoSceneViewports`, `secondaryNoun`, `ThirdFoo`. A
  name is ALWAYS the singular `verb`/`verbOne` or the general,
  count/index-parameterized `verbN`. Adding an ordinal-named specialization
  requires EXPLICIT permission. "Get it done" / "let's ship" is never a reason —
  it always *costs* ship time, every single time. The composition belongs in JS
  + a general function, not in bespoke C++.

## Banned constructs

- **Default arguments.** Every parameter is explicit at every call site.
  Add an overload or pass the value; no `T x = default` in a signature.
- **`std::shared_ptr`.** Ownership is single and explicit: `std::unique_ptr`,
  or a handle/index into a pool. Shared ownership hides lifetime.
- **`std::map` / `std::unordered_map`.** Use a flat sorted `std::vector` +
  `std::lower_bound`, or an array indexed by id. ALWAYS ask before adding any
  hash map (see MISTAKES.md).
- **Global mutable state — no globals, no singletons, no thread-locals.** No
  file-scope mutable `static`, no function-local `static`, no `static X&
  Instance()`, no `thread_local`. Construct the object and pass it explicitly
  (by reference); per-instance state lives as a member. Global state bleeds
  across instances and blocks per-context construction (a fresh
  registry/engine per test SCENARIO). ONLY exceptions: state the language/ABI
  forces global (the replaceable global `operator new`/`delete`), or a
  3rd-party dependency forces it.

## We are Aaltonen-pilled and Acton-pilled

Two patron saints, two rules:

- **Aaltonen (handle-pilled).** Sebastian Aaltonen's resource-management
  designs: typed `Handle<T>` end-to-end, OffsetAllocator over fixed buffers,
  one giant heap + one master buffer per memory type, bump rings for
  per-frame UBOs. Indirection is by integer index into dense pools, not by
  pointer. The handle IS the contract — don't drop it to "save an int" or
  collapse it to a raw pointer at the last second.

- **Acton (array-pilled).** Mike Acton's DOD: flat
  arrays-of-structs-of-arrays, hot loops iterate contiguous memory, no
  hidden allocations, no virtual dispatch on the hot path, no hash maps
  where a flat array indexed by id fits. The data is the program.

When in doubt: handle-then-index, array-then-element.

## No pimpl in this repo

When a header needs different members on Metal vs Vulkan, use the
SwapChain-style `#if CAIRNS_METAL ... #elif CAIRNS_VULKAN ...` access
contract directly in the header. No opaque `void* impl_`, no separate
`_noop.cpp` TU, no virtual call, no extra heap allocation. The ugliness
is the point — it's the same ugliness that lets the hot path be one
struct field deref instead of a vtable + indirect call.

> "I'm ugly and I'm proud." — SpongeBob

## Anti-singleton (scene layer)

- **No "the world."** No global/static/singleton world, registry, camera,
  or selection.
- **No implicit "current scene."** Every public op takes an explicit
  `WorldId` or `EntityRef`.
- **Never pass a bare `entt::entity` across a boundary.** The cross-boundary
  reference is `EntityRef = { WorldId, entt::entity }`. A bare `entt::entity`
  lives only inside a scope that has already resolved its world.
  Code-review rule: a bare `entt::entity` in a signature, member, or
  container is a defect.
- **`active_world` is UI focus ONLY** — never read by core logic.
