# Building LonelyIce

LonelyIce builds on Windows, Linux and macOS from the same sources. Documentation is written in English only.

## Windows

Visual Studio 2022, CMake 3.22 or newer, Git, Boost 1.87 and OpenSSL 3.

```
git clone --recursive https://github.com/LonelyIceProject/lonelyice
cd lonelyice
tools\build-ui-deps.ps1
cmake -S . -B build -DBOOST_ROOT=<boost> -DOPENSSL_ROOT_DIR=<openssl>
cmake --build build --config RelWithDebInfo --target lonelyice_release
```

`build/lonelyice-release` then holds the application, `plugins/` and `setup/`.

## Linux and macOS

The Linux x64 terminal release, including the core and bundled plugins, has been built and checked on
Ubuntu 24.04. Linux desktop and macOS builds have not been verified.

The game client stays the Windows `Wow.exe` and runs through Wine: `client.runner` in `local.yaml` names the
program that starts it (default `wine`; arguments separated by spaces, e.g. `wine64` or a Proton/CrossOver wrapper).
The server, the launcher and the tools run natively. A headless server does not need Wine.

### Linux

A compiler with C++20 (GCC 11+ or Clang 14+), CMake 3.22 or newer, Git, Boost, OpenSSL 3 and the development
packages SDL3 needs for X11, Wayland and OpenGL. On Debian / Ubuntu:

```
sudo apt install build-essential cmake git pkg-config \
    libboost-all-dev libssl-dev zlib1g-dev libbz2-dev libreadline-dev libncurses-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev \
    libdrm-dev libgbm-dev libdbus-1-dev libudev-dev libasound2-dev libpulse-dev
```

Wine (`sudo apt install wine`) runs the game client.

### macOS

Xcode command line tools (`xcode-select --install`) and [Homebrew](https://brew.sh):

```
brew install cmake git boost openssl@3
```

`tools/build-ui-deps.sh` finds Homebrew's OpenSSL by itself; pass `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`
to CMake as below. The game client needs a Wine build for macOS (for example from Homebrew or CrossOver).

### Build

```
git clone --recursive https://github.com/LonelyIceProject/lonelyice
cd lonelyice
tools/build-ui-deps.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel --target lonelyice_release
```

On macOS add `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"` to the configure line. `tools/build-ui-deps.sh` uses
all CPUs; `JOBS=4 tools/build-ui-deps.sh` uses fewer. As on Windows, `build/lonelyice-release` then holds the
application (`LonelyIce`), `plugins/` and `setup/`.

## Terminal and headless builds

FTXUI supplies the terminal interface on Windows, Linux and macOS. CMake uses an installed FTXUI 7 package
when available; otherwise it fetches the pinned v7.0.3 source commit and builds it statically. For an offline
build, supply `-DFETCHCONTENT_SOURCE_DIR_FTXUI=/path/to/ftxui` with that checkout, or install its CMake package.

On Linux, a server/terminal build needs the core's compiler, Boost, OpenSSL and database/archive development
dependencies, but no X11, Wayland, OpenGL, SDL3, FreeType, RmlUi or Wine. On Debian / Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build git pkg-config \
    libboost-all-dev libssl-dev zlib1g-dev libbz2-dev libreadline-dev libncurses-dev
```

Bundled plugins can need additional dependencies: the MySQL storage plugin needs the database client development
package, and bot tactics needs LuaJIT and sol2 headers. Follow each plugin's README and set
`TACTICS_DEPS_DIR` to its prepared dependencies directory when needed. Then build:

```sh
tools/build-ui-deps.sh --headless
cmake -S . -B build-headless -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLONELYICE_GUI=OFF -DLONELYICE_TUI=ON
cmake --build build-headless --parallel --target lonelyice_release
```

On Windows, the dependency-script equivalent is `tools/build-ui-deps.ps1 -Headless` and the CMake options are
the same. `LONELYICE_TUI=OFF` omits FTXUI; `LONELYICE_GUI=OFF -DLONELYICE_TUI=OFF` builds only the command-line
and server roles. Both options default to ON. See [Terminal setup and headless servers](/docs/terminal).

## Notes

- Windows release packaging scans executable and plugin DLL imports recursively and includes third-party
  dependencies and the MSVC redistributable runtime. Windows system DLLs are supplied by the OS. Missing
  dependencies fail packaging; use `LONELYICE_RUNTIME_DIRS` for additional DLL search directories.
- Auxiliary launcher libraries are linked statically; the core's bundled zlib is static on Windows. MSBuild
  vcpkg auto-linking is disabled so globally installed import libraries cannot override explicit CMake choices.
  Core libraries, plugins and the OpenSSL/MySQL runtimes remain DLLs and are included in the release.
- The server core is the `external/azerothcore` submodule; `LONELYICE_CORE_DIR` points elsewhere.
- Plugins in `plugins/` (submodules with a `plugin.json`) are built and shipped with the release;
  `LONELYICE_PLUGIN_DIRS` replaces that list. Some plugins have their own requirements, see their READMEs.
- Interface text lives in `src/assets/lang/<code>/*.lang` (`key = value`, `{0}` for arguments) and is embedded in
  the executable; code uses `Tr("key", ...)`, the markup `@{key}`. English is the fallback for missing keys. With
  `LONELYICE_ASSETS=<src/assets>` the launcher reads the markup and these files from disk.
- `tools\build-ui-deps.ps1` (Windows) and `tools/build-ui-deps.sh` (Linux, macOS) fetch and build the libraries
  LonelyIce adds to the core into `deps/` once: SDL3, FreeType and RmlUi for the interface, StormLib for client
  archives, miniz for plugin packages and libcurl for downloads (TLS through Schannel on Windows, OpenSSL elsewhere).
