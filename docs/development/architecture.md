# Architecture and performance

Zalie is three layers, each one a CMake target:

| Target | Directory | Content | Dependencies |
| ------ | --------- | ------- | ------------ |
| `zalie_core` | `src/core` | model, variables, resolution, snippets, importers, persistence, executor | aeronet client, amc, glaze (private) |
| `zalie_ui` | `src/ui` | the whole interface, independent of the platform and the renderer | `zalie_core`, Dear ImGui (no backend) |
| `zalie` | `src/app` | GLFW window, OpenGL 3 renderer, event loop, command line | `zalie_ui`, GLFW, Dear ImGui backends |

The core has no GUI dependency and the UI no platform dependency: both are tested headless (see
[Testing](testing.md)).

## Life of a request

```text
UI thread                                   worker thread (x N)
---------                                   -------------------
Request (model, RawChars32 fields)
  -> ResolveRequest()                        
     variables, headers, auth   ----------> Executor queue
     ResolvedRequest (move-only)            aeronet::HttpClient (one per settings, per worker,
                                            keep-alive pool) + ResponseBodySink
                                              onHead: status, headers -> StringPool
                                              onBody: append to the body, reserved once
                                            ResponseBody::analyze(): kind, pretty JSON, line index
poll() each frame  <----------------------  completed Exchange (+ wake the event loop)
TextView draws the visible lines only
```

- **The UI thread never waits for the network.** Requests go to a pool of worker threads (4 by default). An aeronet
  `HttpClient` is not thread-safe: each worker owns its clients (one per distinct combination of settings), so
  keep-alive connections are reused between requests without any locking.
- **Progress is lock-free**: bytes received, announced size and cancellation are atomics shared between the worker
  and the UI. Cancelling stops the transfer at the next received bytes (aeronet has no cross-thread cancellation:
  a request still connecting ends with its timeout, its result is then discarded).
- **Heavy work happens on the worker**: JSON pretty-printing, content sniffing and line indexing are done once, when
  the response arrives, never in a frame.
- **Completion wakes the UI** through a C function pointer (`glfwPostEmptyEvent`), no polling.

## Memory and copies

- **`RawChars` instead of `std::string`** (aeronet): no SSO, no null terminator, trivially relocatable. Model fields are
  `RawChars32` (16 bytes instead of 32). Dear ImGui edits them in place through a resize callback: no intermediate
  `std::string`.
- **`amc` containers**: the model types declare themselves trivially relocatable (`AllTriviallyRelocatable<...>`),
  so `amc::vector` grows, inserts and erases them with `memcpy` / `realloc`. `amc::SmallVector` keeps small temporary
  lists (syntax tokens of a line, JSON nesting stack) inline, without allocation. `amc::FlatSet` for id sets.
- **`ObjectArrayPool<char>` for strings sharing one lifetime** (`StringPool`): the URL, headers and unresolved
  variables of a resolved request, the reason and headers of a response, warnings... are packed in one or two blocks
  (sized from an estimate) instead of one allocation per string; the structs hold `std::string_view`s into them, which
  stay valid when the struct is moved.
- **The response body is copied once**: from the receive buffer of the client into the body buffer, reserved from
  `Content-Length` (one allocation). The request body is handed to aeronet with `bodyStatic()`: not copied.
- **Snippets, resolved URL previews and JSON validation** are recomputed only when the request changes (a revision
  counter incremented by every edit), not every frame.

## Rendering

- **Virtualized views**: response bodies, snippets and the history use `ImGuiListClipper` over a `LineIndex` (offsets
  only, 8 bytes per line): only the visible lines are laid out, tokenized and drawn, straight from the body buffer. The
  states of multi-line syntax (XML comments, strings...) are precomputed per line, so any line can be drawn without
  the previous ones. Lines longer than 2 KB are split (on a UTF-8 boundary).
- **Event driven loop**: when nothing animates, the loop blocks in `glfwWaitEventsTimeout` (0% CPU); it renders
  continuously only while a request is in flight (the running dog) or a toast fades, at the vsync rate.
- **No GL in the UI code**: images (logo, sprites, image responses) go through Dear ImGui's texture protocol
  (`ImGuiBackendFlags_RendererHasTextures`), the renderer backend uploads them. The UI thus runs with any backend,
  including the null one of the tests.
- **Fonts and images are embedded** in the executable (generated sources, no `#embed` dependency): nothing to install
  next to it.

## Measured

On the development machine (Linux, Release build, clang 23):

| | |
| - | - |
| Executable | 5.7 MB (5.1 MB stripped); links only the C++ runtime and OpenSSL |
| Start to first frame | under 0.2 s |
| Memory | about 125 MB resident, of which about 75 MB of shared libraries of the OpenGL driver (Mesa: LLVM, gallium) and about 40 MB anonymous |
| A frame with a 300 000 lines JSON body | under 50 ms in a *debug* build (headless test `AppTest.LargeBodiesStayFast`) |

## What aeronet could expose to go further

The aeronet client does not expose (yet) per-phase timings (DNS, connect, TLS, first byte), the negotiated protocol
(h2 / http/1.1), the final URL after redirects, nor a cross-thread cancellation. Zalie measures the time to the head
in its body sink and the total time around the call; the [Roadmap](../ROADMAP.md) lists what it will show once
available.
