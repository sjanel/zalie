# Testing

Zalie has 150 GoogleTest tests in three families, all discovered by `ctest` (one ctest entry per test):

| Family | Files | What they check |
| ------ | ----- | --------------- |
| Unit tests | `tests/core/*_test.cpp` | model, URL parsing, variables, request resolution, snippets, syntax highlighting, JSON formatting, JSONPath, text documents, string pools, persistence round trips, Bruno / Postman / curl import, the plans of performance runs (value generators, data files, parameter rows and previews) and their JSON reports |
| Integration tests | `tests/core/executor_test.cpp` | real HTTP exchanges against a local [aeronet](https://github.com/sjanel/aeronet) server: methods, headers, user agent, redirects, `HEAD`, big bodies and progress, body limit, timeout, cancellation (in flight and queued), connection errors, 40 concurrent requests on 4 workers, destruction with pending requests. `tests/core/load-runner_test.cpp`: performance runs (limits, warm-up, rate cap, statuses, errors, time series, varied values) |
| UI tests | `tests/ui/*_test.cpp` | the real interface, driven headless (see below): every section and response view, tabs and shortcuts, sending to a local server, cancelling with ++esc++, errors, pasting a curl command, JSONPath filter, response below / beside the request, imports, dialogs, filters, history, theme and HiDPI, large bodies, the Performance tab (parameter rows, plans compiled off the UI thread, runs against a local server, stop, confirmation, dropped data files, export, a dashboard of an hour), the chart widget (scales, reduction to one point per pixel, legend, hover, zoom) |

## Running

```bash
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure --timeout 60 -j 8
```

Useful variations:

```bash
ctest --test-dir build -R Executor                   # a subset, by regex on the test name
ctest --test-dir build --repeat until-fail:20 -j 8   # hunt flaky tests
./build/tests/zalie_app_test --gtest_filter='AppTest.Cancel*'
```

The tests do not need a network connection (servers listen on `127.0.0.1`, an ephemeral port), a display or a GPU.
They write only in temporary directories, removed at the end of each test.

## Headless UI tests

`zalie_ui` contains the whole interface but no platform or renderer backend: the tests run it with the *null*
backends of Dear ImGui (`imgui_impl_null.cpp`), so a frame is laid out, rendered into a draw list and its texture
requests acknowledged, exactly as in the application, without a window.

The fixture (`tests/ui/ui-test-fixture.hpp`) injects inputs between frames (keys, chords, typed characters, mouse
moves and clicks) and turns every Dear ImGui assertion into a test failure: a misuse of the ImGui API by the
UI code fails the build's tests instead of being silently ignored in Release.

Two details to know when writing a UI test:

- **Shortcuts need a warm-up frame**: Dear ImGui registers the routes of `Shortcut()` during a frame and resolves them
  at the next one. Render a couple of frames (`frame(2)`) before sending a chord.
- **Requests are asynchronous**: after sending, keep rendering frames until the tab is not pending any more
  (`waitResponse()`, bounded in time), as the event loop would.

## Integration tests

`executor_test.cpp` starts an `aeronet::SingleHttpServer` in the test process, with routes returning JSON, echoing
the request, redirecting, streaming slowly, or sending a large body. The executor under test uses the real aeronet
client and worker threads: what is checked is the full path from a `Request` of the model to an `Exchange` read by the
UI.

## Sanitizers and compilers

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

On Linux, the suite passes with GCC 13, Clang 22 and Clang 23, and with AddressSanitizer + UndefinedBehaviorSanitizer
(leak detection included). The CI (one workflow per platform in `.github/workflows`: `linux.yml`, `macos.yml`,
`windows.yml`) also builds and runs it on macOS and Windows, warnings as errors everywhere. Clang adds
`-Wshorten-64-to-32`, the narrowing MSVC reports: the amc containers have 32-bit sizes, index them with their own
size type (`std::uint32_t`) or cast a `std::size_t` index.

!!! tip "Spare your SSD"
    A debug build of Zalie and its dependencies weighs about 500 MB. For throwaway builds (another compiler,
    sanitizers), build in a RAM disk that allows executables (`$XDG_RUNTIME_DIR` usually does, `/dev/shm` is often
    mounted `noexec`) and reuse the already fetched sources:

    ```bash
    cmake -S . -B "$XDG_RUNTIME_DIR/zalie-gcc" -G Ninja -D CMAKE_CXX_COMPILER=g++-13 -D CMAKE_C_COMPILER=gcc-13 \
      -D FETCHCONTENT_SOURCE_DIR_AERONET=$PWD/build/_deps/aeronet-src \
      -D FETCHCONTENT_SOURCE_DIR_IMGUI=$PWD/build/_deps/imgui-src
    ```

## Coverage

The `coverage` job of `linux.yml` builds the tests with the source-based coverage of Clang 23
(`-fprofile-instr-generate -fcoverage-mapping`, and `NDEBUG`: the failure branch of an `assert` is not a gap), runs
them, merges their profiles with `llvm-profdata` and measures the lines and branches of `src/` with `llvm-cov`. The
total goes to the summary of the job, the details to [Codecov](https://codecov.io/gh/sjanel/zalie) (the badge of the
README) and to an HTML report, the `coverage-report` artifact of the run. `src/app` (window, OpenGL, `main`) is not
linked into the tests: it is not measured.

The same, locally (in a RAM disk, see above):

```bash
B=$XDG_RUNTIME_DIR/zalie-coverage
flags="-DNDEBUG -fprofile-instr-generate -fcoverage-mapping"
cmake -S . -B $B -G Ninja -D CMAKE_BUILD_TYPE=Debug -D CMAKE_C_COMPILER=clang-23 -D CMAKE_CXX_COMPILER=clang++-23 \
  -D CMAKE_C_FLAGS="$flags" -D CMAKE_CXX_FLAGS="$flags" -D CMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate
cmake --build $B && LLVM_PROFILE_FILE=$B/profiles/%p.profraw ctest --test-dir $B -j 8
llvm-profdata-23 merge -sparse $B/profiles/*.profraw -o $B/coverage.profdata
tests=($B/tests/zalie_*_test) && objects=("${tests[@]:1}")
llvm-cov-23 report --instr-profile=$B/coverage.profdata ${tests[0]} ${objects[@]/#/--object=} src
```

`llvm-cov-23 show --format=html --output-dir=<dir>` with the same arguments writes the annotated sources.

## Test data

`tests/data` holds the import fixtures: a Bruno collection (folders, environments, a deliberately broken `.bru` file)
and a Postman v2.1 export. Tests locate them through the `ZALIE_TEST_DATA_DIR` compile definition.

## Screenshots

The documentation screenshots are produced by the application itself, in its demo mode (hidden window, offscreen
framebuffer):

```bash
tools/take-screenshots.sh build/bin/zalie
```

This needs a display (X11, Wayland, or `xvfb-run`), unlike the tests.
