# Builds outside the default tree

## Disk writes are a hard budget (SSD wear)

Every byte a build writes wears out the developer's SSD, and a debug build of Zalie and its dependencies weighs about
500 MB. The default tree is `build/` (or `build/debug` with presets, the one `.clangd` reads): iterate there, on the
targets you run.

## Throwaway builds go to RAM, not to the SSD

Throwaway trees (another compiler, sanitizers, Release checks) go to a RAM disk that allows executables:
`$XDG_RUNTIME_DIR` (`/run/user/<uid>`, about 3 GB). `/dev/shm` is `noexec` on the development machine: binaries cannot
be run from there.

Reuse the sources already fetched, and drop the debug info when it is not needed:

```bash
deps=(); for d in build/debug/_deps/*-src; do n=$(basename "$d" -src); deps+=(-D "FETCHCONTENT_SOURCE_DIR_${n^^}=$PWD/$d"); done
cmake -S . -B "$XDG_RUNTIME_DIR/zalie-exp" -G Ninja -D CMAKE_BUILD_TYPE=Debug -D CMAKE_CXX_FLAGS=-g0 "${deps[@]}"   # + the options under test
ninja -C "$XDG_RUNTIME_DIR/zalie-exp" <target>
```

CMake warns that some of these variables are unused when an option turns the matching dependency off: harmless.

- `-D ZALIE_BUILD_APP=OFF` keeps the tree small when the interface is not involved.
- AddressSanitizer + UndefinedBehaviorSanitizer: `-D ZALIE_ENABLE_ASAN=ON`.
- ThreadSanitizer has no option: pass `-fsanitize=thread` in `CMAKE_CXX_FLAGS`. On the development machine its binaries
  need `setarch "$(uname -m)" -R <binary>` (high-entropy ASLR), and the gtest discovery step fails at link time for the
  same reason, but the binary is produced.
- tmpfs is RAM: `rm -rf` the directory as soon as you are done.

## Dependencies

`ZALIE_AERONET_URL` and `ZALIE_AERONET_SHA256` are cache variables: after a bump of the pin in
`cmake/Dependencies.cmake`, an existing tree keeps the old one unless reconfigured with
`-U ZALIE_AERONET_URL -U ZALIE_AERONET_SHA256`.

## Other platforms

macOS and Windows are only compiled by the CI (`.github/workflows`): watch it after touching platform-specific code.
The packages (`cmake/Packaging.cmake`, the install rules of `src/app/CMakeLists.txt`) are only made and checked by
`package.yml`; locally, test the `.deb` with `cpack --config <tree>/CPackConfig.cmake -B "$XDG_RUNTIME_DIR/zalie-pkg"`
(output and staging in RAM).
