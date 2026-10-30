# Contributing

Bug reports, ideas and pull requests are welcome on [GitHub](https://github.com/sjanel/zalie). Before a pull
request: build with warnings as errors, run the tests, format the code.

```bash
cmake --preset debug -D ZALIE_WARNINGS_AS_ERRORS=ON
cmake --build --preset debug
ctest --preset debug
clang-format -i $(git ls-files '*.hpp' '*.cpp')
```

## Repository layout

| Path | Content |
| ---- | ------- |
| `src/core` | `zalie_core`: model, resolution, snippets, importers, persistence, executor. No GUI |
| `src/ui` | `zalie_ui`: the interface (Dear ImGui), no platform backend |
| `src/app` | `zalie`: `main()`, GLFW window, OpenGL renderer, command line |
| `tests` | unit, integration and headless UI tests, import fixtures in `tests/data` |
| `assets` | logo and sprite sheet (generated, see [Artwork](artwork.md)) |
| `cmake` | project settings, pinned dependencies, file embedding |
| `docs`, `mkdocs.yml` | this documentation |
| `tools` | asset generator, screenshot script |
| `packaging` | Linux desktop entry |

See [Architecture](architecture.md) for how the pieces fit together.

## Code conventions

Zalie follows the conventions of [aeronet](https://github.com/sjanel/aeronet):

- C++23, formatted by `clang-format` (Google style, 120 columns, `.clang-format`), checked by `clang-tidy`
  (`.clang-tidy`).
- `PascalCase` for types and free functions, `camelCase` for methods and variables, `_member` for private data
  members, `p` prefix for raw pointers (`pWindow`), `k` prefix for constants (`kRunFps`).
- Files in `kebab-case`, `.hpp` / `.cpp`, tests in `<name>_test.cpp`.
- **Bytes and strings**: `aeronet::RawChars` / `RawChars32` instead of `std::string` (trivially relocatable, no SSO,
  no null terminator); `std::string_view` for non-owning access; `StringPool` (`ObjectArrayPool<char>`) for many
  immutable strings sharing one lifetime.
- **Containers**: `amc::vector`, `amc::SmallVector`, `amc::FlatSet`. A new aggregate stored in them declares
  `using trivially_relocatable = AllTriviallyRelocatable<...>` with the types of its members.
- No work proportional to the size of a body or a collection in a frame: precompute when the data changes (revision
  counters), and draw through `ImGuiListClipper`.
- No GL or platform calls in `src/ui` (textures go through Dear ImGui's texture protocol), no GUI dependency in
  `src/core`.
- Plain hyphens in text and comments, no typographic dashes.

## Tests

Every change of behavior comes with a test, see [Testing](testing.md). UI changes can (and should) be tested
headless in `tests/ui`.

## Dependencies

Dependencies are pinned to an archive and its SHA-256 in `cmake/Dependencies.cmake`. To bump one, change both:

```bash
curl -sL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz | sha256sum
```

Keep the build self-contained: a new dependency must be fetchable with `FetchContent`, buildable statically on the
three platforms, and justified by what it saves.

## Documentation

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements-docs.txt
mkdocs serve            # http://127.0.0.1:8000, live reload
mkdocs build --strict   # what the CI runs
```

Screenshots are regenerated with `tools/take-screenshots.sh` (see [Testing](testing.md#screenshots)).

## Releases

The version lives in the `VERSION` file only (read by CMake, shown by `zalie --version`). Update
[the changelog](../CHANGELOG.md) in the same commit.
