# Install

Download a package of the [latest release](https://github.com/sjanel/zalie/releases), or build Zalie from source.

## Download

| Platform | Package | Runs on |
| -------- | ------- | ------- |
| Debian / Ubuntu, x86_64 | `zalie_<version>_amd64.deb` | Ubuntu 24.04+, Debian 13+ |
| Windows, x64 | `zalie-<version>-windows-x64.zip` | Windows 10+ |
| macOS, Apple Silicon | `zalie-<version>-macos-arm64.dmg` | macOS 26+ |

Each package comes with its SHA-256 (`.sha256`). To try what is coming next, the
[`dev` pre-release](https://github.com/sjanel/zalie/releases/tag/dev) holds the packages of the latest commit of
`main`, versioned `X.Y.Z-dev` (the next release) and replaced at each change. Installed over a development build, the
`.deb` of the release `X.Y.Z` is an upgrade.

- **Debian / Ubuntu**: `sudo apt install ./zalie_0.1.0_amd64.deb` installs Zalie with its dependencies (OpenSSL 3, the
  X11 libraries), its desktop entry and its icons; `sudo apt remove zalie` removes it.
- **Windows**: unzip anywhere and run `zalie.exe`: the OpenSSL and Visual C++ runtime DLLs are next to it. The
  executable is not signed: on the first launch, SmartScreen asks to confirm (*More info*, then *Run anyway*).
- **macOS**: open the disk image and drag Zalie to *Applications*. The application is not notarized by Apple: on the
  first launch macOS refuses to open it; allow it in *System Settings > Privacy & Security* (*Open Anyway*), or
  remove the quarantine flag: `xattr -dr com.apple.quarantine /Applications/zalie.app`.

## Build from source

Zalie is built with CMake. Every dependency is fetched and pinned automatically (aeronet, Dear ImGui, GLFW, glaze,
amc, stb, GoogleTest), except OpenSSL (https support) and the windowing headers of your system.

### Requirements

| | Linux | macOS | Windows |
| - | ----- | ----- | ------- |
| Compiler | GCC 13+ or Clang 22+ | Homebrew GCC 15 for C++ (as aeronet), Apple Clang for C / Objective-C (GLFW) | Visual Studio 2022 (MSVC, latest update) |
| CMake | 3.28+ | 3.28+ | 3.28+ |
| OpenSSL | `libssl-dev` | `brew install openssl` | pre-installed on GitHub runners, or [Win64 OpenSSL](https://slproweb.com/products/Win32OpenSSL.html) |
| Window system | X11 headers (see below) | built in | built in |
| OpenGL | 3.0 | 3.2 core | 3.0 |

On Debian / Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build git libssl-dev \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev
# Optional, for the native Wayland backend (-D ZALIE_GLFW_WAYLAND=ON):
sudo apt install libwayland-dev wayland-protocols libxkbcommon-dev
```

On Fedora: `sudo dnf install gcc-c++ cmake ninja-build openssl-devel libXrandr-devel libXinerama-devel
libXcursor-devel libXi-devel libXext-devel`.

!!! note "No OpenGL headers needed"
    Zalie loads OpenGL at run time through the small loader bundled with Dear ImGui, and GLFW loads the X11
    extension libraries at run time too: only their *headers* are needed to build. The executable itself only links
    the C++ runtime and OpenSSL.

### Build

```bash
git clone https://github.com/sjanel/zalie.git
cd zalie
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Release
cmake --build build
./build/bin/zalie
```

The executable is written to `bin/` in the build directory. Release builds use link-time optimization (LTO) when the
toolchain supports it (`-D ZALIE_ENABLE_LTO=OFF` to disable it): smaller and faster, at the cost of a longer link.

With the presets of `CMakePresets.json` (`debug`, `release`, `asan`), each in `build/<preset>`:

```bash
cmake --preset release && cmake --build --preset release
./build/release/bin/zalie
```

On Windows, from a *Developer PowerShell for VS 2022*:

```powershell
cmake -S . -B build -A x64 -D OPENSSL_ROOT_DIR="C:/Program Files/OpenSSL-Win64"
cmake --build build --config Release
.\build\bin\Release\zalie.exe
```

On macOS the build produces an application bundle (`open build/bin/zalie.app`). Configure it like the CI does (give
Apple Clang with its full path: CMake rejects a bare `clang` for Objective-C):

```bash
brew install gcc@15 ninja openssl
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Release -D CMAKE_CXX_COMPILER=g++-15 \
  -D CMAKE_C_COMPILER="$(xcrun --find clang)" -D CMAKE_OBJC_COMPILER="$(xcrun --find clang)"
```

### Install

```bash
sudo cmake --install build                # /usr/local/bin/zalie + desktop entry and icons on Linux
cmake --install build --prefix ~/.local   # for your user only, no root
```

On Linux, the install also adds the desktop entry (`share/applications/zalie.desktop`) and the icons
(`share/icons/hicolor`). On Windows, `zalie.exe` goes to the top of the prefix, next to the OpenSSL and Visual C++
runtime DLLs it loads, so that the directory can be copied to another machine.

The packages of the [download table](#download) are made by `cpack` from a Release build, in `<build>/packages`:

```bash
cpack --config build/CPackConfig.cmake               # Linux: the .deb (needs dpkg-shlibdeps, from dpkg-dev)
cpack --config build/CPackConfig.cmake -C Release    # Windows: the zip
```

The macOS disk image is made by the CI only (`.github/workflows/package.yml`), which also copies the Homebrew
libraries (GCC runtime, OpenSSL) into the application bundle.

### Run the tests

```bash
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure --timeout 60
```

See [Testing](../development/testing.md) for the organization of the tests, and [Build options](../reference/build-options.md)
for the CMake options.

## Icon in the task bar (Linux)

Desktops like GNOME do not use the icon of a window: they look for the desktop entry matching it
(`StartupWMClass=Zalie`), and show a generic icon without one. So that the dog shows up even when Zalie runs straight
from its build directory, Zalie writes its own entry at launch when **no** `zalie.desktop` exists yet:
`~/.local/share/applications/zalie.desktop` (it starts the executable that wrote it) and its icon in
`~/.local/share/icons/hicolor/256x256/apps`. It prints the path when it does.

- An entry installed by `cmake --install` or a package always wins: Zalie never touches it, and removes its own one
  if both exist.
- Two builds launched alternately (debug, release) do not fight: the entry keeps the first executable while it
  exists.
- `zalie --no-desktop-entry` disables it; delete the two files to remove them.

The desktop may take a second to notice a new entry: if the gear icon stays the first time, restart Zalie.

## Where are my data?

Zalie keeps its settings, history and collections in one directory, that you can change with `--home` or the
`ZALIE_HOME` environment variable:

| Platform | Default directory |
| -------- | ----------------- |
| Linux | `$XDG_CONFIG_HOME/zalie`, else `~/.config/zalie` |
| macOS | `~/Library/Application Support/Zalie` |
| Windows | `%APPDATA%\Zalie` |

See [Files and formats](../reference/file-format.md).
