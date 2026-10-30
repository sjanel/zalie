# Zalie - agent guidelines

Zalie is a light, local-only HTTP client (Bruno / Postman like): C++23, CMake, the
[aeronet](https://github.com/sjanel/aeronet) HTTP client, Dear ImGui + GLFW + OpenGL 3. Linux (primary), macOS,
Windows. Goals: instant startup, smooth with any response size, no CPU when idle, small self-contained binary.

## Layout

- `src/core` (`zalie_core`): model, variables, request resolution, snippets, importers (Bruno, Postman, curl),
  persistence (glaze, private), executor (worker threads + aeronet `HttpClient`). **No GUI dependency.**
- `src/ui` (`zalie_ui`): the whole interface. **No platform / GL calls**: textures go through Dear ImGui's texture
  protocol, the host feeds events. Tested headless with `imgui_impl_null`.
- `src/app` (`zalie`): `main()`, GLFW window, OpenGL 3 backend, event loop, command line, offscreen screenshots. The
  executable is built in `<build>/bin/zalie`; Release builds use LTO (`ZALIE_ENABLE_LTO`).
- `tests/core`, `tests/ui`, `tests/data`; `docs/` (MkDocs Material); `tools/` (asset generator, screenshots).
- `resources/` holds private photos: never commit, copy or publish them (it is git-ignored).

## Performance rules (same spirit as aeronet)

- Strings and bytes: `aeronet::RawChars` / `RawChars32`, not `std::string` (trivially relocatable, no SSO, **no null
  terminator**: never pass `.data()` to an API expecting a C string, glaze reads use `.null_terminated = false`).
- Many immutable strings with one lifetime: `StringPool` / `StringList` (`aeronet::ObjectArrayPool<char>`), the struct
  holds `std::string_view`s into it.
- Containers: `amc::vector`, `amc::SmallVector`, `amc::FlatSet`. Aggregates stored in them declare
  `using trivially_relocatable = AllTriviallyRelocatable<...>` (`zalie/raw-chars.hpp`).
- Nothing proportional to a body or collection size per frame: precompute on change (revision counters), draw with
  `ImGuiListClipper`. Heavy work (JSON pretty-printing, line indexing) runs on the worker thread.
- The loop must stay event driven: `App::animating()` is true only while something moves.

## Conventions

- `PascalCase` types and free / static functions, `camelCase` methods and variables, `_member` private fields,
  `p` prefix for raw pointers, `k` prefix for constants. Files `kebab-case.hpp/.cpp`, tests `<name>_test.cpp`.
- Methods first, fields last. Short doc comments in headers only. Prefer explicit types over `auto`.
- Format with `clang-format` (`.clang-format`: Google, 120 columns), respect `.clang-tidy`.
- Plain hyphens in code, comments and docs: no typographic dashes.
- Dear ImGui is 1.93: `GetContentRegionMax` is gone, `PathStroke` / `AddRect` take flags last, some helpers are
  internal (`imgui_internal.h`). Shortcut routes need one frame to register (UI tests render `frame(2)` first).

## Build and test

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

- Always rebuild the targets before running them. Every behavior change gets a test; UI changes are testable headless
  (`tests/ui/ui-test-fixture.hpp`).
- Before finishing: the build is warning-free (`-D ZALIE_WARNINGS_AS_ERRORS=ON`), the whole suite passes, and
  `clang-format --dry-run` reports nothing.

### Disk writes (SSD wear)

The default tree is `build/` (or `build/debug` with presets). Throwaway trees (another compiler, sanitizers, Release
checks) go to a RAM disk that allows executables, `$XDG_RUNTIME_DIR` (`/run/user/<uid>`; `/dev/shm` is `noexec` on
the development machine), reusing the fetched sources with `-D FETCHCONTENT_SOURCE_DIR_<NAME>=$PWD/build/_deps/<name>-src`
and `-D CMAKE_CXX_FLAGS=-g0` when debug info is not needed. Delete them when done.

## Docs

- `mkdocs build --strict` must pass (CI). Screenshots come from `tools/take-screenshots.sh` (demo mode of the app).
- Keep `docs/CHANGELOG.md` updated with user visible changes; the version lives in `VERSION` only.
