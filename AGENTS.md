# Zalie - agent guidelines

Zalie is a light, local-only HTTP client (Bruno / Postman like): C++23, CMake, the
[aeronet](https://github.com/sjanel/aeronet) HTTP client, Dear ImGui + GLFW + OpenGL 3. Linux (primary), macOS,
Windows. Goals: instant startup, smooth with any response size, no CPU when idle, small self-contained binary.

Before a task of one of these kinds, read its file:

| Task | Read |
| --- | --- |
| Hot-path code: buffers, string building, struct layout, `std::optional` members | `agents/performance.md` |
| Interface code or its tests (`src/ui`, `tests/ui`) | `agents/ui.md` |
| A field of the saved model, anything read or written as JSON (glaze) | `agents/persistence.md` |
| A build outside the default tree: another compiler, sanitizers, Release | `agents/builds.md` |
| Docs of a change: MkDocs site, build options, screenshots, changelog | `agents/docs-and-changelog.md` |

## Layout

- `src/core` (`zalie_core`): model, variables, request resolution, snippets, importers (Bruno, Postman, curl),
  persistence (glaze, private), executor (worker threads + aeronet `HttpClient`), load runner. **No GUI dependency.**
- `src/ui` (`zalie_ui`): the whole interface. **No platform / GL calls**: textures go through Dear ImGui's texture
  protocol, the host feeds events. Tested headless with `imgui_impl_null`.
- `src/app` (`zalie`): `main()`, GLFW window, OpenGL 3 backend, event loop, command line, offscreen screenshots. The
  executable is built in `<build>/bin/zalie`; Release builds use LTO (`ZALIE_ENABLE_LTO`).
- `tests/core`, `tests/ui`, `tests/data`; `docs/` (MkDocs Material); `tools/` (asset generator, screenshots).
- `resources/` holds private photos: never commit, copy or publish them (it is git-ignored).

## The prime directive: performance

For any change ask, in order: *does this allocate? does this copy? does this grow a struct? does this run per frame?*

- **No unnecessary copies, no reallocations, minimize memory.** Prefer zero/single-allocation data paths (append idiom
  in `agents/performance.md`).
- **Order struct/class fields to minimize padding** (largest alignment first, group by size).
- **Prefer `std::string_view` over `const std::string&`** wherever ownership isn't taken.
- Strings and bytes: `aeronet::RawChars` / `RawChars32`, not `std::string` (trivially relocatable, no SSO, **no null
  terminator**: never pass `.data()` to an API expecting a C string, glaze reads use `.null_terminated = false`).
- Many immutable strings with one lifetime: `StringPool` / `StringList` (`aeronet::ObjectArrayPool<char>`), the struct
  holds `std::string_view`s into it.
- Containers: `amc::vector`, `amc::SmallVector`, `amc::FlatSet`. Aggregates stored in them declare
  `using trivially_relocatable = AllTriviallyRelocatable<...>` (`zalie/raw-chars.hpp`).
- **No `std::optional<T>` member when the type has a natural "empty" / "invalid" state** (empty string or container,
  sentinel, null pointer, `empty()` / `valid()` object).
- Prefer C style function pointers over `std::function`. One line functions not requiring additional includes can be
  defined in the header.
- Replace unreachable code and useless defensive checks with `assert`.
- Nothing proportional to a body or collection size per frame: precompute on change (revision counters), draw with
  `ImGuiListClipper`. Heavy work (JSON pretty-printing, line indexing) runs on the worker thread.
- The loop must stay event driven: `App::animating()` is true only while something moves.

## Conventions

- Comments, code and docs: plain hyphens only, no typographic dashes.
- **PascalCase**: types, type aliases, *static* methods, *free* functions. **camelCase**: variables, *instance* methods.
  Private members `_size`, raw pointers `pData`, constants `kMaxSize`. Names of at least 2 chars, except `i` / `j` / `k`
  loop indices and `x` / `y` screen coordinates. A small named struct over `std::pair` / `std::tuple`.
- Files `kebab-case.hpp/.cpp`, tests `<name>_test.cpp`.
- Modern C++23, RAII (Rule of 0 or 5), ranges where they read well, `.contains()` over `find() != npos`, explicit types
  over `auto` (except long nested types, iterators).
- In a class/struct: **methods first, all fields last.** Short doc comment on public functions **in the header only**.
- Format with `clang-format` (`.clang-format`: Google, 120 columns).
- clang-tidy (`.clang-tidy`) and include-what-you-use: fix the warnings the IDE shows, don't run clang-tidy locally.
  Known ignore: chrono suffix warnings.
- Factor duplicates into small helpers; test utilities go next to the tests (`tests/ui/ui-test-fixture.hpp`).
- Errors: report them with context (`std::expected<T, RawChars32>` up to the interface), never ignore the return value
  of a function that can fail. Only the basic exception guarantee is required.

## Build and test

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

- The default tree is `build/` (or `build/debug` with presets). CMake + Ninja, **never pass `-j`** to the build.
  **Always rebuild the targets before running them.**
- **Disk writes wear the SSD**: while iterating, build only what you run (`ninja -C build/debug zalie_<name>_test`);
  full build and full `ctest` **once**, when the change is finished. Don't rebuild when no source changed, don't
  reconfigure a tree without a reason.
- **Never build in a new tree on disk (`build-release/`, the `asan` preset...) unless the user asks in the current
  conversation** (CI runs AddressSanitizer). Suspect a memory bug: propose a sanitizer run. Throwaway builds go to RAM:
  `agents/builds.md`.
- Tests: `ctest --preset debug`, which sets a timeout: tests can freeze. Every behavior change gets a test; UI changes
  are testable headless (`agents/ui.md`). Cover edge cases *and* large / chunked data paths.

## Definition of done (every fix/feature)

1. **Tests**: the change is covered (target 100 % for new code), the whole suite passes.
2. **No warning**: the build is warning-free (`-D ZALIE_WARNINGS_AS_ERRORS=ON`) and `clang-format --dry-run` reports
   nothing.
3. **Docs**: the MkDocs page of the feature; `docs/ROADMAP.md` when relevant. The rest of the site and
   `mkdocs build --strict`: `agents/docs-and-changelog.md`.
4. **`docs/CHANGELOG.md`**: a short entry for user visible changes (`agents/docs-and-changelog.md`). The version lives
   in `VERSION` only (`X.Y.Z-dev` between releases, see `docs/development/contributing.md`).

## Git

Never commit or push unless explicitly asked. Use git for read-only operations by default; branch before committing if
on `main`.
