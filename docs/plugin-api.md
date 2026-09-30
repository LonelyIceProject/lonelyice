# Plugin API (C++)

The server side of a plugin: how a plugin library is built against the LonelyIce fork of AzerothCore, the entry
points it exports, the checks the loader makes and what it does with a plugin, in which order. The loader
(`PluginMgr`, `src/server/shared/Plugins` of the fork) is part of the core, so everything here applies to the
fork's plain worldserver, authserver and dbimport as much as to LonelyIce. The package side (manifest fields,
launcher settings, patches, client files, catalogs) is in [Plugin format](/docs/plugin-format); the fork's own
summary is its `doc/Plugins.md`.

## Core build

Plugin libraries need the core built as shared libraries, with a name for its binary interface:

```
cmake -DWITH_DYNAMIC_LINKING=ON -DAC_PLUGIN_ABI=<name> -DAC_PLUGIN_SOURCE_DIRS=<plugin dir>;<plugin dir> ...
```

| Variable | Meaning |
|---|---|
| `AC_PLUGIN_ABI` | Name of the binary interface, compiled into the `shared` library and into every plugin. Default `azerothcore-dev`; LonelyIce releases use `lonelyice-ac-2` (set by LonelyIce's top-level `CMakeLists.txt`). |
| `AC_PLUGIN_SOURCE_DIRS` | Plugin source folders built with the core, `;`-separated; each holds a `plugin.json` and a `CMakeLists.txt`. LonelyIce passes `LONELYICE_PLUGIN_DIRS` ([Building LonelyIce](/docs/building)). |
| `AC_PLUGINS_OUTPUT_DIR` | Where built plugins are laid out; default `plugins` next to the programs (`bin/<config>/plugins`). |

A core built without shared libraries takes the same plugin sources, built into the programs (see
[Built into the programs](#built-into-the-programs)).

## AddPlugin

A plugin's `CMakeLists.txt` calls

```cmake
AddPlugin(<target> SOURCES <files...> [CORE <library>] [LINK <libraries...>] [INCLUDES <dirs...>]
          [RUNTIME_FILES <files...>] [EXPORT_ALL])
```

| Argument | Meaning |
|---|---|
| `SOURCES` | The plugin's sources; `src/` of the plugin folder is on the include path. |
| `CORE` | The core library to build against: `game` (default), or `shared` for a plugin that also loads in authserver and dbimport and must not pull the game library in there. When the build has no such library, the plugin is skipped. |
| `LINK` | Further libraries, e.g. the libraries of plugins this one depends on, or a database client library. |
| `INCLUDES` | Public include folders, for plugins that link against this one. |
| `RUNTIME_FILES` | Files the library needs at run time (DLLs), copied next to it. |
| `EXPORT_ALL` | Export every symbol of the library (for plugins that link against it; needed on Windows, other platforms export by default). |

The library is named after `server.library` of the manifest and placed in
`<AC_PLUGINS_OUTPUT_DIR>/<id>/server/<platform>/`. `plugin.json`, the folders `data`, `sql`, `conf`, `lua`,
`client` and the files `settings.json`, `icon.png`, `LICENSE`, `README.md` are copied next to it on every build
(folders are replaced, so removed files disappear). Every plugin is compiled with `AC_PLUGIN_BUILD` defined, for
headers that choose between `dllexport` and `dllimport`.

## Entry points

A plugin library includes `PluginApi.h` and ends with exactly one entry macro:

```cpp
#include "PluginApi.h"

void AddTacticsScripts();       // creates the plugin's ScriptObjects, as a module's Add*Scripts() does

AC_PLUGIN(AddTacticsScripts)
```

| Macro | Entry points |
|---|---|
| `AC_PLUGIN(addScripts)` | Scripts, registered by worldserver after the static modules' scripts. |
| `AC_PLUGIN_ON_LOAD(onLoad)` | Code run right after the library is loaded, in every program that loads it, before the configs are read and the databases open. |
| `AC_PLUGIN_ENTRY(onLoad, addScripts)` | Both. Either may be `nullptr`. |

Both functions are plain `void()` functions (`PluginFunction`). In a shared-library build the macro defines
four C functions, exported with `__declspec(dllexport)` on Windows and default visibility elsewhere:

```cpp
extern "C" char const* AcorePlugin_Abi();        // AC_PLUGIN_ABI
extern "C" char const* AcorePlugin_Platform();   // AC_PLUGIN_PLATFORM
extern "C" void        AcorePlugin_OnLoad();     // calls onLoad, if any
extern "C" void        AcorePlugin_AddScripts(); // calls addScripts, if any
```

`AC_PLUGIN_PLATFORM` is fixed by the compiler's target:

| Value | Target |
|---|---|
| `windows-x64` | Windows, x86-64 |
| `linux-x64` | Linux, x86-64 |
| `linux-arm64` | Linux, AArch64 |
| `macos-x64` | macOS, x86-64 |
| `macos-arm64` | macOS, Apple silicon |
| `unknown` | anything else |

In authserver and dbimport only `onLoad` runs; they have no scripts. A database backend is a plugin whose
`onLoad` calls `RegisterBackendDriver` (`IDbConnectionBackend.h`) and whose manifest lists
`"apps": [ "worldserver", "authserver", "dbimport" ]`; see the fork's `doc/Plugins.md`.

## Loading

The programs call the loader after their main config and the log are loaded, before the module configs and the
databases:

| Program | Loads plugins with `server.apps` containing |
|---|---|
| worldserver | `worldserver` |
| authserver | `authserver` |
| dbimport | `dbimport` |
| `LonelyIce --server` | `worldserver` or `authserver` (both run in that process) |

The folder is the config option `PluginsDir` (default `plugins`, relative to the working directory); LonelyIce
sets it through `AC_PLUGINS_DIR` ([Environment variables](/docs/environment)). `PluginMgr::Load`:

1. Reads `plugin.json` of every subfolder, in name order (folders without one are ignored, so
   `plugins/.disabled` is never looked into). A plugin not made for this program (`server.apps`,
   default `["worldserver"]`) is left out without a message. Unknown `format`, a missing `id` or `version`, a
   manifest that cannot be parsed and an `id` already used by another folder are logged as errors and skipped.
2. Adds the plugins built into the program that have no folder (with a warning: their configs and SQL are not
   used).
3. Drops, until nothing changes, plugins whose `depends` are missing or out of range and plugins whose
   `conflicts` name a plugin still in the set.
4. Orders the rest: dependencies first, ties by id.
5. For each plugin in that order: skip it if a dependency failed to load; otherwise open its library (below).
   A plugin that fails is logged (`Plugin <id> <version> not loaded: <reason>`) and counts as failed for the
   plugins after it.
6. For each loaded plugin: register its config (`config` of the manifest) with the config manager, add its
   `databases` folders for `auth`, `characters` and `world` to the updater (state `MODULE`), log
   `Plugin <id> <version> (<name>)`, then call its `onLoad`.

Opening a library:

- a plugin built into the program uses its registered entry points, no library and no ABI check;
- a plugin without `server.library` (data or client only) has nothing to open and loads;
- `core.abi` of the manifest must equal the core's `AC_PLUGIN_ABI`;
- a core without shared libraries refuses plugin libraries;
- the file `server/<platform>/<library>` must exist (`name.dll`, `libname.so`, `libname.dylib`);
- Windows: `LoadLibraryEx` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`, so the
  DLLs next to the plugin library are found; Linux and macOS: `dlopen` with `RTLD_NOW | RTLD_GLOBAL`, so a
  plugin's symbols are visible to the plugins loaded after it;
- `AcorePlugin_Abi` and `AcorePlugin_Platform` must be exported, with `AcorePlugin_OnLoad` or
  `AcorePlugin_AddScripts`, and must return the core's ABI and platform.

Then the program loads the module configs: for each plugin with a `config`, first the `.conf.dist` from the
plugin folder, then, if it exists, `modules/<name>.conf`, whose values override it key by key. That `modules`
folder is the one beside the main config file the program loaded (`-c`; LonelyIce passes its absolute path), on
every system; only when there is no such folder does the core use `modules` of its default config directory
(`configs/` of the working directory on Windows, the build's `CONF_DIR` elsewhere,
`ConfigMgr::GetModulesConfigPath()`). The static modules' configs are looked up in the same folder. `reload config`
reads all of them again. Options in the `.conf` that the
`.dist` does not declare are reported as unknown. worldserver and `LonelyIce --server` register the plugins'
scripts in the modules script loader, after the static modules (`sPluginMgr->AddScripts()`), and add every
loaded plugin's id to the enabled-modules list. Libraries stay loaded until the process exits; there is no
unloading or reloading.

| Message | Cause |
|---|---|
| `unsupported manifest format <n>` | `format` is not 1. |
| `manifest needs id and version` | `id` or `version` missing. |
| `bad plugin.json: ...` | The manifest cannot be parsed. |
| `id <id> is already used by <folder>` | Two folders with the same `id`. |
| `skipped: needs <id> <range>[, installed <version>]` | A dependency is missing or out of range. |
| `skipped: conflicts with <id>` | A plugin from `conflicts` is installed. |
| `a dependency failed to load` | A plugin this one depends on was not loaded. |
| `built for core <abi>, this server is <abi>` | `core.abi` of the manifest differs. |
| `this server is built without shared libraries ...` | A library plugin on a static core. |
| `no build for <platform> (<path>)` | The package has no library for this platform. |
| `LoadLibrary failed, error <n>`, `dlopen failed: ...` | The system could not load the library (often a missing dependency). |
| `not a plugin library (missing exports)` | The entry points are missing. |
| `library built for <abi> <platform>, this server is ...` | The library's own ABI or platform differs. |

## PluginMgr

`PluginMgr.h` (library `shared`), reached as `sPluginMgr`:

| Member | Meaning |
|---|---|
| `void Load(std::filesystem::path const& dir, std::vector<std::string> const& apps = { "worldserver" })` | Reads, orders and loads the plugins of `dir` made for one of `apps`, as above. Called once by each program. |
| `void AddScripts()` | Calls every loaded plugin's scripts entry point, in load order. Called from the modules script loader. |
| `std::vector<PluginInfo> const& GetPlugins() const` | All plugins in load order, including those that failed (`loaded == false`, `error` set). |
| `PluginInfo const* Find(std::string const& id) const` | The plugin with that id, or `nullptr`. |
| `bool IsLoaded(std::string const& id) const` | Whether that plugin is loaded. |
| `static bool Satisfies(std::string const& version, std::string const& range)` | Version range check, as used for `depends`. |

`PluginInfo`:

| Field | Meaning |
|---|---|
| `id`, `version`, `name` | From the manifest (`name`: the string, or its `en` text). |
| `dir` | The plugin's folder. A plugin finds its own runtime files there: `sPluginMgr->Find("<id>")->dir / "lua"`. |
| `library` | Path of the library for this platform; empty without server code. |
| `abi` | `core.abi` of the manifest. |
| `apps` | `server.apps`. |
| `configFile`, `configDist` | `<name>.conf` looked up in the modules config folder (beside the main config), and the `.conf.dist` in the plugin folder, loaded first. |
| `databases` | Core database (`auth`, `characters`, `world`) and update folder. Plugin-owned databases (objects in the manifest) are not read. |
| `depends`, `conflicts` | From the manifest. |
| `loaded`, `error` | Result of loading. |
| `handle`, `onLoad`, `addScripts` | The library handle and entry points. |

Ranges for `Satisfies` (and `depends`) mean what they mean in npm's semver: comparators separated by spaces and/or
commas, all of which must hold (`>=1.0.0 <2.0.0` or `>=1.0.0,<2.0.0`). `^1.2.3` is `>=1.2.3 <2.0.0`, `^0.2.3` is
`>=0.2.3 <0.3.0`, `^0.0.3` is `>=0.0.3 <0.0.4`; `~1.2.3` and `~1.2` stay below 1.3.0, `~1` below 2.0.0; a partial
version covers what it leaves out (`1.2`, `1.2.x`: `>=1.2.0 <1.3.0`; `<=1.2`: `<1.3.0`; `>1.2`: `>=1.3.0`); `*`
matches anything. The table is in [Package manager](/docs/package-manager#version-ranges). A range that cannot be
read (`||`, hyphen ranges, pre-release versions, unknown operators) never matches; the core's
`Acore::VersionRange::IsValid(range, &badTerm)` (`VersionRange.h`) tells whether a range can be read and where it
cannot, and the loader skips a plugin whose `depends` has such a range.

## Plugin code

- Configuration: `sConfigMgr->GetOption<T>("Key", default)`, as in a module; the options come from the plugin's
  `.conf.dist` and `.conf`, and `AC_<KEY>` environment variables override them.
- Databases: only through the core's interfaces: `WorldDatabase`, `CharacterDatabase`, `LoginDatabase`, prepared
  statements, transactions, and `ModuleDatabasePool` for a database the plugin owns. The loader registers update
  folders for `auth`, `characters` and `world` only; a plugin-owned database (an object in `databases`) is
  opened, created and updated by the plugin itself (playerbots: `ModuleDBUpdater` in its `DatabaseScript`). SQL
  files follow [Plugin format](/docs/plugin-format), section 5.
- Other plugins: declare them in `depends` (they load first) and link against their libraries with `LINK`; check
  an optional one with `sPluginMgr->IsLoaded("<id>")`.
- Named ids given out for the plugin's patches are read from the world table `plugin_ids`
  ([Plugin format](/docs/plugin-format), section 7).

## Built into the programs

With a core built without shared libraries, `AddPlugin` builds the plugin as an object library linked into each
program of `server.apps` (worldserver when missing), copies its `RUNTIME_FILES` next to the programs and still
lays out the plugin folder, without a library. It defines `AC_PLUGIN_STATIC` and `AC_PLUGIN_ID` (the manifest's
id), and the entry macro then registers the entry points under that id before `main` runs, instead of exporting
them:

```cpp
void RegisterStaticPlugin(char const* id, PluginFunction onLoad, PluginFunction addScripts);
```

(through a `StaticPluginRegistrar` object). The loader uses these entry points for the plugin folder of the same
id, so configs, SQL, dependencies and load order work as for a library; a built-in plugin whose folder is missing
is loaded anyway, without its configs and SQL.
