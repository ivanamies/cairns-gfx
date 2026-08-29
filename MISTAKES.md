# MISTAKES.md

Recurring mistakes I (the assistant) have made on cairns-gfx. Read at the
start of every session; the goal is to stop being told the same thing twice.

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
