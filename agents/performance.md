# Hot-path details

Details behind the performance rules of `AGENTS.md`.

## Buffers

**Prefer `RawChars` / `RawChars32` over `std::string`** for internal buffers: smaller `sizeof`, simpler, no SSO /
null-terminator baggage. `RawChars32` (16 bytes) for model fields, `RawChars` (64-bit size) for bodies
(`zalie/raw-chars.hpp`).

### Single-allocation append idiom

`AppendAll(out, parts...)` (`zalie/raw-chars.hpp`) is the one way to append several pieces: it sums their maximum
sizes, checks the capacity once (exponential growth), then writes them with raw pointers. Prefer it to a sequence of
`append` calls or to `std::format`.

```cpp
AppendAll(out, "Imported '", name, "': ", nbRequests, ' ', Escaped(value, kJsonEscapes), '\n');
```

- Parts: string views (and what converts to them), string literals (their size is a constant), a `char`, an integer
  (written in decimal by aeronet's `WriteInt`), and any `CustomPart`: a type with `maxSize()` and
  `char* writeTo(char*)`, which writes at most `maxSize()` bytes and returns the end of what it wrote. The parts must
  not point into `out`.
- Custom parts of `zalie/encoding.hpp`: `Escaped(text, table)` with the compile-time `EscapeTable`s (JSON, C++, shell,
  PowerShell, desktop entry), `Base64`. Also there: `AppendUtf8`, `ReadHex`, `ControlEscape`, `AppendJsonUnescaped`.
  Write a new escaping as a table and an `Escaped` part, not as another switch.
- `Concat({...})` builds a new buffer in one allocation; `AssignSafe` assigns a value that may point into the buffer.
- Lower level, when `AppendAll` does not fit (a loop of unknown length): `ensureAvailableCapacity(n)` reserves `n`
  *extra* bytes, `ensureAvailableCapacityExponential(n)` doubles the capacity (use it in loops), then
  `aeronet::Append(view, ptr)` / `aeronet::Copy` (inlined small-copy dispatch, faster than `memcpy` for short
  fragments) and `setEnd(ptr)`. `unchecked_append` copies once the capacity is ensured.
- `RawChars(std::size_t)` / `RawChars32(std::size_t)` only allocate memory, they don't fill it unlike `std::string`.
  They do not set the size, only the capacity: use `setSize()` after filling.
- Character classes and comparisons: aeronet's `isspace / isdigit / tolower / CaseInsensitiveEqual / is_tchar`
  directly; `zalie/string-utils.hpp` only adds `Trim`, `StartsWithIgnoreCase`, `FindIgnoreCase`, `ContainsIgnoreCase`,
  `AssignLower`. URL encoding: `zalie/url.hpp` on aeronet's `URLEncode` / `url::DecodeInPlace`.

## Containers

The amc containers have 32-bit sizes: index them with their own size type (`std::uint32_t`) or cast a `std::size_t`
index. Clang reports the narrowing with `-Wshorten-64-to-32`, MSVC too.

## `std::optional` members

**Avoid `std::optional<T>` for member fields when the type already has a natural "empty" / "invalid" state**: an empty
string or `string_view`, an empty container, a `0` / sentinel value, a null pointer, or an object whose own `valid()` /
`empty()` reports absence. `std::optional<T>` adds `alignof(T)` bytes of discriminant + padding per field, grows
`sizeof`, adds a branch on every access, and slows compiles. Same idea for object construction and "may-fail" outcomes:
a constructor that leaves the object `empty()` on bad input (no `static T::parse(...)` factory), or a factory that
returns an `invalid()` object, beats wrapping in `std::optional` / throwing. Reserve `std::optional` for return values
where absence is genuinely indistinguishable from any valid value.
