# MISTAKES.md

Recurring mistakes I (the assistant) have made on cairns-gfx. Read at the
start of every session; the goal is to stop being told the same thing twice.

## Input binding conflicts

I misunderstood human navigation and bound the screenshot dump on `D`
-- the SAME key as right-strafe in the fly cam's WASD cluster. Every
right-strafe wrote a PNG synchronously and froze the render thread on
`stbi_write_png`. Caught immediately on the first windowed launch.

Rule: BEFORE adding any single-key binding, mentally walk through what
a user holding WASD + RMB-look would press incidentally. If the
candidate is in {W, A, S, D, Q, E, H, J, K, L, Shift}, pick another.
Default to `Z` for "dump", F-keys for debug toggles, modifier+letter
for anything destructive.

## C++ value-passing discipline

I randomly drop `&`, `const&`, and `&&` -- defaulting to pass-by-value where
it's actively wrong, and need prompting to write universal references.

The rule for this codebase:
- **sinks take rvalue ref**: `void Foo(std::string&& s)`, `void Set(Vec<T>&& v)`.
  Pass-by-value `(std::string s)` "value-sink" pattern is rejected here --
  it's too easy to hide an unnecessary copy.
- **read-only takes const ref**: `void Foo(const Vec<T>& v)`.
- **mutable in-place takes ref**: `void Foo(Vec<T>& v)`.
- **value only for trivially copyable scalars**: `int`, `float`, handles,
  small POD.

Apply this without being asked. When introducing a new API, audit the
signature for sink vs read-only before committing. Sites caught this
session: `CommandRegistry::Register/RegisterAlias/PublishEvent`,
`Engine::SetSelection/SetHighlights`, `engine_headless::SetSelection/
SetHighlights/AddSelection/RemoveSelection`. All were initial-pass-by-value
and had to be retro-fitted on prompt.
