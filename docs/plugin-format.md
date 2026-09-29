# LonelyIce plugin format

Status: draft 1. Applies to the LonelyIce fork of AzerothCore (`LonelyIceProject/azerothcore-wotlk`). The server
side (manifest core fields, library entry points, loading) is described in the fork's `doc/Plugins.md`; this
document adds what LonelyIce and its package manager use on top: launcher settings, client files, packages.

A plugin adds server code, database content, configuration, launcher settings and client files to a
server without rebuilding the core. Plugins are what the launcher's future package manager installs,
updates and removes, together with their dependencies.

The plugin loader is part of the core (`game` library), not of the launcher. A plain `worldserver.exe`
of the fork, run as a dedicated server without LonelyIce, loads the same plugins from the same folder:
it resolves dependencies, loads the DLLs, reads their configs and applies their SQL through its own
database updater. The launcher only reads the manifests to show settings, place client files and manage
packages; everything a plugin needs at run time works without it.

## 1. Package layout

A plugin is a folder (installed) or a `.zip` of that folder (distributed). The folder name is the plugin id.

```
lonelyice.tactics/
  plugin.json                  manifest, required
  server/                      server code (optional: data-only and client-only plugins have none),
    windows-x64/                 one folder per platform the package was built for; a package can carry
      lonelyice_tactics.dll      any number of them
    linux-x64/
      liblonelyice_tactics.so
    macos-arm64/
      liblonelyice_tactics.dylib
  sql/
    world/ characters/ auth/   update files, AzerothCore updater layout (see 5)
    <database id>/             base + updates of a database the plugin owns
  conf/
    mod_lonelyice_tactics.conf.dist
  lua/ ...                     any runtime files the plugin reads, found via its own folder
  client/
    addons/BotTactics/...      copied into Interface\AddOns
    patches/...                recipes for client patches (see 7)
  icon.png                     64x64, shown by the launcher
  LICENSE
  README.md
```

The core finds plugins in `PluginsDir` (worldserver.conf, default `plugins` next to the config's working
directory). Each subfolder with a `plugin.json` is a plugin.

## 2. Manifest (`plugin.json`)

```json
{
  "format": 1,
  "id": "lonelyice.tactics",
  "version": "1.3.0",
  "name": { "en": "Bot tactics", "ru": "Тактики ботов" },
  "description": { "en": "Gambit-style orders for playerbots", "ru": "Приказы для ботов в стиле гамбитов" },
  "authors": [ "LonelyIceProject" ],
  "license": "GPL-2.0-or-later",
  "homepage": "https://github.com/LonelyIceProject/mod-lonelyice-tactics",

  "core": { "abi": "lonelyice-ac-1" },
  "platforms": [ "windows-x64", "linux-x64" ],
  "depends": { "playerbots": ">=1.0.0" },
  "conflicts": [],

  "server": {
    "library": "lonelyice_tactics"
  },
  "databases": {
    "world": "sql/world",
    "characters": "sql/characters"
  },
  "config": "conf/mod_lonelyice_tactics.conf.dist",
  "settings": "settings",
  "client": {
    "addons": [ "client/addons/BotTactics" ]
  }
}
```

| Field | Meaning |
|---|---|
| `format` | Manifest format version. The loader refuses formats it does not know. |
| `id` | Unique, lowercase, `[a-z0-9.-]`. Reverse-domain style for third parties (`author.feature`). |
| `version` | Semantic version of the plugin. |
| `name`, `description` | Localized strings: an object keyed by locale (`en` required) or a plain string. |
| `core.abi` | Binary interface the library was built against (see 4). Required when `server` is present. |
| `platforms` | Platforms the package has server builds for (`windows-x64`, `linux-x64`, `linux-arm64`, `macos-x64`, `macos-arm64`), one subfolder of `server/` each. Omitted: the plugin has no server library and runs everywhere. |
| `depends` | Plugin id → version range (`>=1.2.0`, `^1.2`, `1.2.x`, `*`). Loaded before this plugin. |
| `conflicts` | Plugin ids that must not be installed together with this one. |
| `server.library` | Library base name. The loader looks in `server/<platform>/` of the running system and adds the platform's form: `name.dll` on Windows, `libname.so` on Linux, `libname.dylib` on macOS. A plugin without a build for the running platform is skipped with a message. |
| `databases` | Update folders per core database (`auth`, `characters`, `world`) and databases the plugin owns (see 5). |
| `config` | The plugin's `.conf.dist`. Its settings are read through the normal config manager. |
| `settings` | Launcher settings schema, inline array or the name of a file `settings.json` (see 6). |
| `client` | Client files (see 7). |

## 3. Server library

The library links against the core shared libraries of the same ABI (`common`, `shared`, `database`,
`game`) and against the libraries of the plugins it depends on. It exports two C functions; the SDK header
generates them, using `__declspec(dllexport)` on MSVC and default visibility on GCC and Clang:

```cpp
#include "PluginApi.h"

void AddTacticsScripts();   // the usual Add*Scripts(): creates ScriptObjects

AC_PLUGIN(AddTacticsScripts)
```

expands to

```cpp
extern "C" char const* AcorePlugin_Abi()        { return AC_PLUGIN_ABI; }
extern "C" char const* AcorePlugin_Platform()   { return AC_PLUGIN_PLATFORM; }
extern "C" void        AcorePlugin_AddScripts() { AddTacticsScripts(); }
```

Load order at server start (worldserver and the LonelyIce server process run the same code):

1. Read all manifests, drop the ones with an unknown `format`, check `conflicts`, resolve `depends` into a
   load order (topological, by id for ties). A plugin with a missing or out-of-range dependency is skipped
   with an error, and so is everything that depends on it.
2. Load each library (Windows: `LoadLibraryEx` with the plugin folder added to the DLL search path;
   Linux and macOS: `dlopen` with `RTLD_NOW | RTLD_GLOBAL`, so a plugin's symbols are visible to the plugins
   that depend on it), compare `AcorePlugin_Abi()` and `AcorePlugin_Platform()` with the core's; a
   mismatch skips the plugin.
3. Register the plugin's config (`<config dir>/modules/<conf name>`, falling back to the `.dist` in the
   plugin folder), its SQL folders (5) and its id in the enabled-modules list.
4. After the static modules' scripts, call each plugin's scripts function in load order.

Plugins are loaded once per process; there is no hot reload.

## 4. ABI

C++ plugins share classes, allocators and the C++ runtime with the core, so a library only works with the
core build it was compiled against. Compatibility is the pair of

* `AC_PLUGIN_ABI`: a string set when the core is configured (`-DAC_PLUGIN_ABI=lonelyice-ac-1` for LonelyIce releases), fixed per fork release, for example `lonelyice-ac-1`, bumped whenever a
  header change can break binaries;
* `AC_PLUGIN_PLATFORM`: operating system, architecture and C++ runtime family, for example `windows-x64`
  (MSVC 14.x, dynamic release CRT), `linux-x64` (GCC/Clang with libstdc++), `macos-arm64` (Apple Clang, libc++).

A package can carry builds for several platforms of one ABI. The package repository keeps one package
per plugin version and ABI. Everything in one process (core and plugins) is built with the same toolchain
family for that platform.

## 5. Databases

Plugins do not know which database engine the core runs on (SQLite, MySQL, later PostgreSQL):

* SQL files are written once, in the AzerothCore SQL dialect the updater has always read. The core's dialect
  layer translates each statement for the active backend when it applies the file. Backend-specific files
  and `overrides/` folders are not allowed in plugins.
* Plugin code reads and writes data only through the core's database interfaces: `DatabaseWorkerPool`
  (`WorldDatabase`, `CharacterDatabase`, `LoginDatabase`), prepared statements, query results, transactions,
  and `ModuleDatabasePool` for databases the plugin owns. It never includes or calls a driver.

Plugin SQL follows the AzerothCore updater: files are named `YYYY_MM_DD_NN_description.sql`, applied once
and tracked in the database's `updates` table by name and hash. `databases.world` etc. add the folder to
the core database's update list (state `MODULE`).

A plugin that owns a database declares it with a connection key and base folder:

```json
"databases": {
  "playerbots": { "config": "PlayerbotsDatabaseInfo", "base": "sql/playerbots/base", "updates": "sql/playerbots/updates" }
}
```

The server applies plugin SQL itself on start when `Updates.EnableDatabases` allows it, and creates a
plugin-owned database when `Updates.AutoSetup` is on, exactly as for core databases. LonelyIce runs the
same step during its install wizard so the first start is fast.

## 6. Launcher settings

`settings.json` (or the inline array) describes what the launcher shows in its Settings tab. Values live
in the plugin's config file; the launcher edits them in place, keeping comments.

```json
{
  "group": { "en": "Tactics", "ru": "Тактики" },
  "hint":  { "en": "Orders for your bots", "ru": "Приказы для ваших ботов" },
  "fields": [
    { "key": "Tactics.Enable", "type": "bool", "apply": "restart",
      "label": { "en": "Enabled", "ru": "Включено" } },
    { "key": "Tactics.MaxRules", "type": "int", "min": 1, "max": 64, "apply": "reload",
      "label": { "en": "Rules per bot", "ru": "Правил на бота" },
      "hint": { "en": "More rules cost more CPU", "ru": "Больше правил — больше нагрузка" } },
    { "key": "Tactics.Mode", "type": "choice", "apply": "now",
      "options": [ { "value": "0", "label": { "en": "Off", "ru": "Выкл" } }, { "value": "1", "label": "Party" } ] }
  ]
}
```

| `type` | Control |
|---|---|
| `bool` | checkbox, written in the style the file already uses (`1`/`0`, `true`/`false`) |
| `int`, `float` | number field with optional `min`, `max` |
| `string` | text field |
| `choice` | drop-down of `options` |

`apply`: `now` (read on every use), `reload` (`.reload config`), `restart` (server restart). The launcher
uses it to decide what to do after saving.

## 7. Client files

* `client.addons`: folders copied into `Interface\AddOns` of the game client, replaced on update and
  removed on uninstall.
* `client.patches`: declarative recipes, not binaries and not programs. Patches that change game data (DBC
  rows, interface files) are built on the player's machine from the player's own client by code built into
  the launcher and the command-line package manager, so a plugin never runs code on the client side:

  ```json
  "patches": [ {
    "name": "waystones",
    "priority": "L",
    "dbc": [ { "file": "DBFilesClient\\TaxiNodes.dbc", "rows": "client/dbc/TaxiNodes.csv", "key": "ID", "mode": "upsert" } ],
    "files": [ { "from": "client/files/Interface/Icons/waystone.blp", "to": "Interface\\Icons\\waystone.blp" } ]
  } ]
  ```

  The installer reads the listed DBC tables from the player's MPQs for every installed locale, applies the
  rows, adds the files, and writes `patch-<locale>-<priority>.MPQ` into `Data\<locale>`. Game data is never
  distributed in a package; files a plugin adds must be its own.

The same installer code creates and updates databases for the package manager, through the same core
dialect layer, so installing a plugin offline and starting the server give identical results.

## 8. Package repository (planned)

An index file lists packages; the launcher downloads it, resolves dependencies and installs zips.

```json
{
  "format": 1,
  "packages": [
    { "id": "lonelyice.tactics", "version": "1.3.0", "core": "lonelyice-ac-1",
      "platforms": [ "windows-x64", "linux-x64" ],
      "depends": { "playerbots": ">=1.0.0" },
      "url": "https://github.com/LonelyIceProject/mod-lonelyice-tactics/releases/download/v1.3.0/lonelyice.tactics-1.3.0-lonelyice-ac-1.zip",
      "sha256": "…", "size": 1234567 }
  ]
}
```

Client-only plugins (addons) have no `core` and no `platforms` and can be installed for any server. Paths
inside packages always use `/`; the launcher and the loader convert them for the host system.

## 9. Static builds

The same sources still build as classic AzerothCore modules: put the repository into `modules/` and build
with `MODULES=static`. `plugin.json` is then used only for the launcher settings and client files.
