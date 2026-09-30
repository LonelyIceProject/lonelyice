# LonelyIce plugin format

Status: format 1. Applies to the LonelyIce fork of AzerothCore (`LonelyIceProject/azerothcore-wotlk`). The server
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
  data/
    sql/world/ characters/ ... update files, AzerothCore updater layout (see 5)
    patches.json               DBC rows with named ids, their SQL, client files (see 7)
  conf/
    mod_lonelyice_tactics.conf.dist
  lua/ ...                     any runtime files the plugin reads, found via its own folder
  client/
    addons/BotTactics/...      copied into Interface\AddOns
  settings.json                launcher settings (see 6)
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
  "locales": [ "en", "ru" ],

  "core": { "abi": "lonelyice-ac-2" },
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
  "settings": "settings.json",
  "patches": "data/patches.json",
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
| `locales` | Languages the plugin's texts are translated into (see below); `["*"]` for a plugin without texts. |
| `core.abi` | Binary interface the library was built against (see 4). Required when `server` is present. |
| `platforms` | Platforms the package has server builds for (`windows-x64`, `linux-x64`, `linux-arm64`, `macos-x64`, `macos-arm64`), one subfolder of `server/` each. Omitted: the plugin has no server library and runs everywhere. |
| `depends` | Plugin id → version range, as in npm's semver: comparators separated by spaces and/or commas, all of which must hold (`>=1.2.0`, `^1.2`, `~1.2.3`, `1.2.x`, `>=1.0.0 <2.0.0`, `>=1.0.0,<2.0.0`, `*`; [Package manager](/docs/package-manager#version-ranges)). Loaded before this plugin. A range that cannot be read is an error, never read in part. |
| `conflicts` | Plugin ids that must not be enabled together with this one. Checked in both directions: installing, enabling or loading either of the two is refused while the other is enabled. |
| `server.library` | Library base name. The loader looks in `server/<platform>/` of the running system and adds the platform's form: `name.dll` on Windows, `libname.so` on Linux, `libname.dylib` on macOS. A plugin without a build for the running platform is skipped with a message. |
| `server.apps` | Programs that load the plugin: `worldserver`, `authserver`, `dbimport` (default `["worldserver"]`). The LonelyIce server process runs world and auth and loads plugins made for either. A database backend lists all three. |
| `databases` | Update folders per core database (`auth`, `characters`, `world`) and databases the plugin owns (see 5). |
| `config` | The plugin's `.conf.dist`. Its settings are read through the normal config manager. |
| `settings` | Launcher settings: the name of a file (`settings.json`, also found without this field) or the schema inline (see 6). |
| `patches` | Patch recipe file: DBC rows with named ids, SQL, client files (see 7). |
| `client` | `addons`: client addon folders (see 7). |
| `provides` | Capabilities the plugin gives, e.g. `database:mysql` for a library that registers the MySQL database backend (`RegisterBackendDriver`). Informational; the launcher offers a storage through `storage`. |
| `storage` | A place for the server's databases that the plugin adds (a database server, see below). |
| `source` | For a plugin built from a module's own repository (see 10): `repo` (git URL) and `commit` of the module. |

### `locales`

The languages a player can use the plugin in: what it shows to players (chat and gossip texts, its rows in the
database's `*_locale` tables, localized strings of its patch recipes, its client addons, its launcher settings) is
translated into each language listed. A language whose translation covers only a few texts is left out. The
catalogs and the launcher filter packages by it.

| Code | Language | Game locales |
|---|---|---|
| `en` | English | enUS, enGB |
| `de` | German | deDE |
| `es` | Spanish | esES, esMX |
| `fr` | French | frFR |
| `ko` | Korean | koKR |
| `ru` | Russian | ruRU |
| `zh-CN` | Chinese (Simplified) | zhCN |
| `zh-TW` | Chinese (Traditional) | zhTW |

These are the languages of the game's locales, so the codes are the keys of localized strings too (`"name": { "en":
…, "ru": … }`). `["*"]` marks a plugin without texts of its own (a database backend, a rule change): it fits every
language. Without `locales` the languages are unknown, and a filter by language leaves the plugin out. Other codes
are refused by `--pkg pack` and by the catalog site.

### `storage`

A plugin whose library registers a database backend can offer it as a storage. The wizard and Settings → Storage
then list it next to the built-in files (SQLite), with fields for the server, port, user, password and database
prefix:

```json
"storage": {
  "id": "mysql",
  "name": { "en": "MySQL server", "ru": "Сервер MySQL" },
  "port": 3306
}
```

| Field | Meaning |
|---|---|
| `id` | The backend's connection string scheme. Every database is passed as `<id>:host;port;user;password;<prefix><name>` (`auth`, `characters`, `world`, `playerbots`). |
| `name` | Localized name of the storage. |
| `port` | Default port. |
| `config` | Optional config values the server needs with this storage, passed as `AC_*` overrides. `{bin}` is the plugin's `server/<platform>` folder, `{exe}` is `.exe` on Windows and empty elsewhere. |

The launcher checks a storage by running the core against it (`LonelyIce --server --storage-check`), so the
backend has to be registered when the library loads (`AC_PLUGIN_ON_LOAD`, see 3).

## 3. Server library

The library links against the core shared libraries of the same ABI (`common`, `shared`, `database`,
`game`; only up to `shared` for a plugin that also loads in authserver and dbimport) and against the libraries
of the plugins it depends on. It exports C functions; the SDK header generates them, using
`__declspec(dllexport)` on MSVC and default visibility on GCC and Clang:

```cpp
#include "PluginApi.h"

void AddTacticsScripts();   // the usual Add*Scripts(): creates ScriptObjects

AC_PLUGIN(AddTacticsScripts)
```

expands to

```cpp
extern "C" char const* AcorePlugin_Abi()        { return AC_PLUGIN_ABI; }
extern "C" char const* AcorePlugin_Platform()   { return AC_PLUGIN_PLATFORM; }
extern "C" void        AcorePlugin_OnLoad()     { }
extern "C" void        AcorePlugin_AddScripts() { AddTacticsScripts(); }
```

`AC_PLUGIN_ON_LOAD(fn)` fills `AcorePlugin_OnLoad` instead, run right after the library loads in every
program that loads it (a database backend calls `RegisterBackendDriver` there), and
`AC_PLUGIN_ENTRY(onLoad, addScripts)` fills both. Built with the core configured without shared libraries,
the same sources are compiled into the programs of `server.apps` (the core's `doc/Plugins.md`).

Load order at server start (worldserver, authserver, dbimport and the LonelyIce server process run the same
code; each takes the plugins made for it, `server.apps`):

1. Read all manifests, drop the ones with an unknown `format`, check `conflicts`, resolve `depends` into a
   load order (topological, by id for ties). A plugin with a missing or out-of-range dependency is skipped
   with an error, and so is everything that depends on it.
2. Load each library (Windows: `LoadLibraryEx` with the plugin folder added to the DLL search path;
   Linux and macOS: `dlopen` with `RTLD_NOW | RTLD_GLOBAL`, so a plugin's symbols are visible to the plugins
   that depend on it), compare `AcorePlugin_Abi()` and `AcorePlugin_Platform()` with the core's; a
   mismatch skips the plugin.
3. Register the plugin's config (`<config dir>/modules/<conf name>`, falling back to the `.dist` in the
   plugin folder), its SQL folders (5) and its id in the enabled-modules list, then call its load function.
4. After the static modules' scripts, call each plugin's scripts function in load order.

Plugins are loaded once per process; there is no hot reload.

## 4. ABI

C++ plugins share classes, allocators and the C++ runtime with the core, so a library only works with the
core build it was compiled against. Compatibility is the pair of

* `AC_PLUGIN_ABI`: a string set when the core is configured (`-DAC_PLUGIN_ABI=lonelyice-ac-2` for LonelyIce releases), fixed per fork release, for example `lonelyice-ac-2`, bumped whenever a
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
same step during its install wizard so the first start is fast. Afterwards its updates stay off; when the SQL
files of the loaded plugins or the database connections differ from the last start (a hash kept in
`plugins/.cache/sql.stamp`), the server runs the updater over the plugins' folders only, so a plugin installed
or updated later gets its tables on the next start.

## 6. Launcher settings

`settings.json` describes what the launcher shows for the plugin: one group in its Settings tab, named `group`, next to the
core's groups. Values live in the plugin's config (`configs/modules/<name>.conf`, created from the plugin's `.dist`
when first saved); a key missing there shows the `.dist` value. The launcher edits values in place, keeping comments.
A plugin without `config` gets no group.

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
| `int`, `float` | number field with optional `min`, `max` (clamped on save) |
| `string` | text field |
| `choice` | drop-down of `options` |

`apply`: `now` (read on every use), `reload` (`.reload config`), `restart` (server restart, the default). The launcher
uses it to decide what to do after saving. `label`, `hint` and option labels are localized strings; `default` is
used when neither the config nor the `.dist` has the key.

## 7. Patches: DBC rows, named ids, client files

`patches` in the manifest names the plugin's recipe file (`data/patches.json` by convention). A recipe changes game
data declaratively; the plugin never ships game data and never runs code on the client:

```json
{
  "ids": {
    "translocation": { "table": "Spell.dbc", "copy": 44080 }
  },
  "patches": [ {
    "table": "Spell.dbc",
    "rows": [ {
      "id": "@translocation",
      "set": {
        "28": 6,
        "133": 3167,
        "136": { "en": "Translocation", "ru": "Транслокация" },
        "153": { "en": "" }
      }
    } ]
  } ],
  "install":   { "world": [ "INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES ({{id:translocation}}, 'spell_custom_translocation')" ] },
  "uninstall": { "world": [ "DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_custom_translocation'" ] },
  "files": [ { "from": "client/files/waystone.blp", "to": "Interface/Icons/waystone.blp" } ]
}
```

**Named ids.** A plugin does not pick ids for the rows it adds: two plugins could pick the same one. It declares a
name (`ids`) with its table and, optionally, the stock row the new row starts as a copy of. The installer gives the name
the next free id of the table (above the stock rows, the ids already given out and the fixed ids of all recipes) when
the plugin is installed or enabled, and takes it back when the plugin is removed or disabled. The ids live in the
world database, table `plugin_ids (plugin, name, dbc, id)`; while a plugin stays installed its ids never change.

* A row `"id": "@name"` is the plugin's named row (`"@other.plugin/name"` for another plugin's); it is added when
  missing (`"mode": "upsert"`). A row with a number changes a stock row (`"mode": "update"`, the default) or adds one
  (`"insert"`, `"upsert"`, `"copy": <id>`).
* `set` takes field numbers of the table (as in DBC editors). Values: an integer, a float (`1.5`), a string, a
  localized string (an object of locale → text; it fills the 16 locale slots, `ruRU` → `ru` → `en` fallback), or
  `{ "ref": "name" }` for another named id.
* `install` / `uninstall` are SQL statements per database (`world`, `characters`, `auth`) in the AzerothCore
  dialect, with `{{id:name}}` replaced by the ids. `uninstall` is stored when the plugin is installed and run when it
  is removed, even if its files are already gone.
* Plugin code reads its ids through the core's database interfaces, for example
  `SELECT id FROM plugin_ids WHERE plugin = 'lonelyice.waystones' AND name = 'translocation'` on `WorldDatabase`.

**Where the rows go.** The installer applies the recipes

* to the server: rows of tables the core reads from the world database as well (`Spell.dbc` → `spell_dbc`; each
  slot gets its own locale) are written there, so the server's extracted DBC files stay stock;
* to the client: for every locale installed in the client it takes the tables from the player's own stock archives,
  applies all recipes in dependency order, adds `files` and writes one archive, `Data/<locale>/patch-<locale>-4.MPQ`,
  marked as LonelyIce's (an archive of that name LonelyIce did not write is kept as `.bak`). Without recipes the
  archive is removed.

A stamp (recipe text, plugin version, installer version) is stored per plugin in `plugin_patches`; unchanged plugins
are skipped, changed ones are uninstalled and installed again with the same ids.

**When.** LonelyIce applies the patches every time its server starts, after the database updates and before the world
loads. For a plain worldserver the package manager does it: `LonelyIce.exe --pkg apply -c <worldserver.conf>
[--client <game folder>]`. Everything goes through the core's database layer, so it works on every backend the core
supports.

**Client addons.** `client.addons` folders are copied into `Interface/AddOns` before the game starts, replaced on
update and removed with the plugin (LonelyIce keeps its list in `Interface/AddOns/lonelyice-addons.txt`).

## 8. Packages

A package is a zip of the plugin folder (at the root of the zip or in one top folder). `LonelyIce.exe --pkg pack
<plugin folder> [<out dir>]` writes `<id>-<version>.zip` and prints its index entry.

An index (catalog) lists packages; the launcher's Plugins page and `--pkg` read it. Catalogs are set on the Plugins
page or in `lonelyice.ini`: `[packages] index` holds the catalogs in use and `disabled` the ones kept but not read,
both separated by `;`. A catalog is an http(s) URL of an index, a local index file or a folder holding `index.json`.
A catalog that cannot be read is skipped; the others still work. Package and icon URLs are relative to the index.

```json
{
  "format": 1,
  "name": { "en": "My catalog", "ru": "Мой каталог" },
  "packages": [
    { "id": "lonelyice.tactics", "version": "1.3.0", "name": { "en": "Bot tactics", "ru": "Тактики ботов" },
      "core": "lonelyice-ac-2", "platforms": [ "windows-x64", "linux-x64" ], "locales": [ "en", "ru" ],
      "depends": { "playerbots": ">=1.0.0" }, "conflicts": [],
      "icon": "lonelyice.tactics-1.3.0.png", "page": "https://lonelyice.example/packages/lonelyice.tactics",
      "url": "lonelyice.tactics-1.3.0.zip", "sha256": "…", "size": 1234567 }
  ]
}
```

The entry `--pkg pack` prints carries every field of the index format that the manifest knows: `id`, `version`,
`name`, `description`, `core` (the manifest's `core.abi`), `platforms`, `locales`, `depends`, `conflicts`, and `url`,
`sha256`, `size` of the zip. `--pkg pack` also copies the plugin's `icon.png` next to the zip and adds `icon` to the
entry; the launcher caches catalog icons in `plugins/.cache/icons`. Only `page` is left out, since only a catalog
knows it. It refuses unknown `locales` codes and `depends` ranges that cannot be read. `page` is the package's page on the
catalog's site, if it has one (resolved against the index like `url`); the launcher links it from the package (an
installed plugin that no catalog lists links its `homepage`). Its Plugins page filters the catalog by language
(`locales`), as does `--pkg available --locale <code>`.

Localized texts (manifest, settings, index) are objects of language code → text. The launcher shows its own
language (`en`, `de`, `es`, `fr`, `ru`), then `en`, then any.

Packages for another core ABI or without a build for this platform are not offered; client-only plugins (no
`core`, no `platforms`) work with any server. Installing resolves the dependency tree (newest versions that satisfy
every range, installed plugins kept when they fit, conflicts refused in both directions, a disabled plugin never
counting as present), downloads, checks size and sha256 of every package, and only then replaces the folders in
`plugins/<id>`, putting the old ones back on any failure. A disabled plugin is moved to `plugins/.disabled/<id>`,
where the core does not look. Removing or disabling a plugin that others need is refused, and so is enabling one
whose dependencies are not enabled. Changing plugins needs the server stopped (the package manager refuses while
it runs); the databases and the client follow on its next start.

```
LonelyIce.exe --pkg list | available | install <id>[@<range>]... | update [<id>...] | remove <id> |
                    enable <id> | disable <id> | apply -c <worldserver.conf> [--client <game folder>] |
                    pack <plugin folder> [<out dir>]
options: --plugins <dir> (default: plugins next to the exe), --index <urls>, --locale <code> (available)
```
## 9. Static builds

The same sources still build as classic AzerothCore modules: put the repository into `modules/` and build
with `MODULES=static`. `plugin.json` is then used only for the launcher settings and client files.

## 10. Modules from their own repositories

An AzerothCore module that is not written as a plugin is packaged the way a distribution packages a program:
a small repository (`<module>-plugin`) holds only what turns the module's original sources into a plugin, and
the module itself is not forked.

```
mod-transmog-plugin/
  plugin.json          manifest; "source": { "repo": "https://github.com/azerothcore/mod-transmog.git", "commit": "<sha>" }
  CMakeLists.txt       FetchContent of source.repo at source.commit, git apply of patches/*.patch,
                       AddPlugin(<id> SOURCES <module>/src/* plugin/plugin.cpp), the module's conf/ and data/
                       laid out with the plugin
  plugin/plugin.cpp    AC_PLUGIN(Add<module>Scripts): the module's own script loader
  patches/*.patch      changes the module needs as a plugin, if any
```

`databases` and `config` in the manifest name the module's own files (`data/sql/db-world`,
`conf/transmog.conf.dist`). Patches stay small: paths a module hard-codes for `modules/<name>/`, calls into core
code the core does not export. A module that needs larger changes is forked instead, and `source.repo` points
at the fork.
