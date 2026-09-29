# Building LonelyIce

Windows, Visual Studio 2022, CMake 3.22 or newer, Git, Boost 1.87 and OpenSSL 3.

```
git clone --recursive https://github.com/LonelyIceProject/lonelyice
cd lonelyice
tools\build-ui-deps.ps1
cmake -S . -B build -DBOOST_ROOT=<boost> -DOPENSSL_ROOT_DIR=<openssl>
cmake --build build --config RelWithDebInfo --target lonelyice_release
```

`build/lonelyice-release` then holds the application, `plugins/` and `setup/`.

- The server core is the `external/azerothcore` submodule; `LONELYICE_CORE_DIR` points elsewhere.
- Plugins in `plugins/` (submodules with a `plugin.json`) are built and shipped with the release;
  `LONELYICE_PLUGIN_DIRS` replaces that list. Some plugins have their own requirements, see their READMEs.
- Interface text lives in `src/assets/lang/<code>/*.lang` (`key = value`, `{0}` for arguments) and is embedded in
  the exe; code uses `Tr("key", ...)`, the markup `@{key}`. English is the fallback for missing keys. With
  `LONELYICE_ASSETS=<src/assets>` the launcher reads the markup and these files from disk.
- `tools\build-ui-deps.ps1` fetches and builds the libraries LonelyIce adds to the core into `deps/` once: SDL3, FreeType
  and RmlUi for the interface, StormLib for client archives, miniz for plugin packages and libcurl for downloads.
