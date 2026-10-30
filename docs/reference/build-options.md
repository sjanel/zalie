# Build options

| Option | Default | Description |
| ------ | ------- | ----------- |
| `ZALIE_BUILD_APP` | `ON` | The desktop application (Dear ImGui, GLFW, OpenGL 3). `OFF` builds and tests the core library only (no GUI dependency) |
| `ZALIE_BUILD_TESTS` | `ON` when top-level | Unit, integration and headless UI tests. Also builds the aeronet server (the integration tests use a local server) |
| `ZALIE_ENABLE_TLS` | `ON` | https, through OpenSSL (must be installed) |
| `ZALIE_ENABLE_HTTP2` | `ON` | HTTP/2 (ALPN `h2`, and `h2c` prior knowledge) |
| `ZALIE_ENABLE_COMPRESSION` | `ON` | gzip / deflate (zlib-ng), zstd and brotli decompression of the responses |
| `ZALIE_GLFW_WAYLAND` | `OFF` | Native Wayland backend of GLFW on Linux (X11 is always built, and works under XWayland) |
| `ZALIE_ENABLE_WARNINGS` | `ON` when top-level | Strict warnings (`-Wall -Wextra -Wpedantic -Wshadow...`, `/W4`) |
| `ZALIE_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` / `/WX` (CI) |
| `ZALIE_ENABLE_ASAN` | `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `ZALIE_ENABLE_LTO` | `ON` | Link-time optimization of Release builds, for Zalie and its dependencies (except zlib-ng, which opts out to keep its runtime CPU dispatch safe). Skipped when the toolchain does not support it, or when the C and C++ compilers differ (GCC + Clang on macOS) |

The executable is written to `<build>/bin` (`<build>/bin/<Config>` with Visual Studio and Xcode), unless
`CMAKE_RUNTIME_OUTPUT_DIRECTORY` is set. The configure step prints the outcome, e.g.
`[zalie] version 0.1.0 - ... lto: on`.

## Dependencies

All fetched with `FetchContent`, pinned to an archive and its SHA-256 (`cmake/Dependencies.cmake`):

| Dependency | Version | Used for |
| ---------- | ------- | -------- |
| [aeronet](https://github.com/sjanel/aeronet) | `main` (pinned commit) | HTTP/1.1 + HTTP/2 client, `RawChars`, `ObjectArrayPool`; its server in the tests |
| [amc](https://github.com/AmadeusITGroup/amc) | through aeronet | `amc::vector`, `SmallVector`, `FlatSet` for trivially relocatable types |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.93.0 | user interface |
| [GLFW](https://github.com/glfw/glfw) | 3.5.1 | window, input, clipboard, drag and drop |
| [glaze](https://github.com/stephenberry/glaze) | 9.0.0 | JSON reflection of the persisted types, Postman import |
| [stb](https://github.com/nothings/stb) | pinned commit | `stb_image` (embedded images, image responses), `stb_image_write` (screenshots) |
| [GoogleTest](https://github.com/google/googletest) | 1.18.0 | tests |

Any of them can be redirected to a local checkout, for instance to develop Zalie and aeronet side by side:

```bash
cmake -S . -B build -D FETCHCONTENT_SOURCE_DIR_AERONET=$HOME/workspaces/aeronet
```

The aeronet archive can also be changed with `-D ZALIE_AERONET_URL=... -D ZALIE_AERONET_SHA256=...`.
