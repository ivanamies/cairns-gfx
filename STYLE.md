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
