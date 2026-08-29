# Style

A growing set of small conventions that aren't worth a Mistake but are worth
not re-litigating in PR review.

## We are Aaltonen-pilled and Acton-pilled

Two patron saints, two rules:

- **Aaltonen (handle-pilled).** Sebastian Aaltonen's resource-management
  designs: typed `Handle<T>` end-to-end, OffsetAllocator over fixed buffers,
  one giant heap + one master buffer per memory type, bump rings for
  per-frame UBOs. Indirection is by integer index into dense pools, not by
  pointer. The handle IS the contract -- don't drop it to "save an int" or
  collapse it to a raw pointer at the last second.

- **Acton (array-pilled).** Mike Acton's DOD: flat arrays-of-structs-of-arrays,
  hot loops iterate contiguous memory, no hidden allocations, no virtual
  dispatch on the hot path, no hash maps where a flat array indexed by id
  fits. The data is the program.

When in doubt: handle-then-index, array-then-element.

## No pimpl in this repo

When a header needs different members on Metal vs Vulkan, use the
SwapChain-style `#if CAIRNS_METAL ... #elif CAIRNS_VULKAN ...` access
contract directly in the header. No opaque `void* impl_`, no separate
`_noop.cpp` TU, no virtual call, no extra heap allocation. The ugliness
is the point -- it's the same ugliness that lets the hot path be one
struct field deref instead of a vtable + indirect call.

> "I'm ugly and I'm proud." -- SpongeBob
