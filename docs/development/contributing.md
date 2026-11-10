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
| `cmake` | project settings, pinned dependencies, file embedding, packages (CPack) |
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

The version lives in the `VERSION` file only (read by CMake, shown by `zalie --version`). Between two releases it
is the next version with a `-dev` suffix (`0.2.0-dev` after `0.1.0`), and its section of
[the changelog](../CHANGELOG.md) is `## [0.2.0] - Unreleased`. Update the changelog in the same commit as the change.

The [Packages workflow](https://github.com/sjanel/zalie/actions/workflows/package.yml) (`.github/workflows/package.yml`)
builds the Release packages of the three platforms (see [Download](../getting-started/install.md#download)) on each
push to `main`, on the pull requests that change the packaging, and on demand. Each package is installed or run on
the runner before its upload: the `.deb` on fresh Ubuntu 24.04 and Debian 13 containers, the Windows executable with
only the system directories in its `PATH`, the macOS bundle checked for leftover references to Homebrew.

- **Development builds**: on a push to `main` of a `-dev` version, the packages replace those of the
  [`dev` pre-release](https://github.com/sjanel/zalie/releases/tag/dev), deleted and created again on the new commit
  (with its tag). Only the latest development build is kept.
- **Releases**: to release version `X.Y.Z`,
    1. drop the suffix (`X.Y.Z` in `VERSION`) and replace `Unreleased` by the date in the changelog;
    2. tag that commit `vX.Y.Z` and push the tag (`git tag vX.Y.Z && git push origin vX.Y.Z`); the workflow fails if
       the tag does not match `VERSION`;
    3. the workflow attaches the packages and their SHA-256 to a **draft** release, with the section of the changelog
       as notes: review it on the [Releases](https://github.com/sjanel/zalie/releases) page, then publish it (a
       release created from the GitHub interface instead gets the packages attached once the workflow ends);
    4. start the next version: `X.Y+1.0-dev` in `VERSION`, and a new `## [X.Y+1.0] - Unreleased` section in the
       changelog.

The `dev` tag moves at each push to `main`: a `git fetch --tags` (or a pull from an editor that fetches tags) of a
clone that has the previous one refuses to update it (*would clobber existing tag*). `git fetch --tags --force`
updates it.
