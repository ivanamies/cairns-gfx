# Mistakes

A running record, from the user, of things I broke or never finished. Read
before touching adjacent code so they don't get re-introduced.

- **Didn't propagate allocators.** Randomly over-allocated `MTL::Heap`s and
  `MTL::Buffer`s by copying tutorial-style "block per (memory type, slot)"
  shapes instead of finishing the migration to the cairns allocator
  architecture. Result: one heap per slot per memory type when the design
  calls for **exactly one heap + one master buffer for the whole bump path**.
- **Deleted `Handle<BindGroup>` and `Handle<DynamicOffsets>` to "save time."**
  The hot path looked like just an integer at the moment the RHI was stood up,
  so I dropped the typed handles. That's wrong — the typed handles are the
  contract; the hot-path integer shape is a representation detail.
- **Added an `std::counting_semaphore` for no good reason.** The project
  explicitly restricts threading primitives to `std::thread`, `std::mutex`,
  `std::condition_variable`. See `feedback-threading-primitives.md`. I keep
  reaching for newer primitives by reflex; stop.
- **Mangled `Draw` struct packing by adding random padding bytes** for
  alignment/comment reasons that didn't survive review. Don't add or move
  fields without an actual reason.
- **Broke bind-group split-by-frequency** by collapsing into a single giant
  descriptor set (tutorial-style again). Set-0 / set-1 / set-2 / set-3 by
  update frequency is the design (RenderPassGlobals / Material /
  ShaderSpecific / DynamicOffsets); merging defeats the entire reason
  bind groups are split.
- **Keep talking about base-instance and push-constants.** Base-instance is
  proven not to work in our pipeline. Push constants are emulated with UBOs.
  Stop suggesting either; both are explicitly deferred levers.
  (`feedback-no-instancing-pushconstants.md` covers this too.)
- **Don't continue the "handle-pilled" pattern.** During the allocator work
  the rule is **`Handle<T>` first** — bind/use handles end-to-end, resolve to
  pointer/index only as a local at the lowest level. I keep introducing raw
  pointers / indices in places where a typed handle was the established shape.
