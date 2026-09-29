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

> **Not tested yet.** The Linux and macOS builds are written to work but have not been built or run so far. Reports
> and fixes are welcome.

The game client stays the Windows `Wow.exe` and runs through Wine: `[client] runner` in `lonelyice.ini` names the
program that starts it (default `wine`; arguments separated by spaces, e.g. `wine64` or a Proton/CrossOver wrapper).
The server, the launcher and the tools run natively.

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

## Notes

- The server core is the `external/azerothcore` submodule; `LONELYICE_CORE_DIR` points elsewhere.
- Plugins in `plugins/` (submodules with a `plugin.json`) are built and shipped with the release;
  `LONELYICE_PLUGIN_DIRS` replaces that list. Some plugins have their own requirements, see their READMEs.
- Interface text lives in `src/assets/lang/<code>/*.lang` (`key = value`, `{0}` for arguments) and is embedded in
  the executable; code uses `Tr("key", ...)`, the markup `@{key}`. English is the fallback for missing keys. With
  `LONELYICE_ASSETS=<src/assets>` the launcher reads the markup and these files from disk.
- `tools\build-ui-deps.ps1` (Windows) and `tools/build-ui-deps.sh` (Linux, macOS) fetch and build the libraries
  LonelyIce adds to the core into `deps/` once: SDL3, FreeType and RmlUi for the interface, StormLib for client
  archives, miniz for plugin packages and libcurl for downloads (TLS through Schannel on Windows, OpenSSL elsewhere).
