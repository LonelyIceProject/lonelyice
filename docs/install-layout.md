# Install layout

A LonelyIce release is one folder: the program, the core libraries, the shipped plugins and the setup packs. The
first-run wizard then creates a server folder with the configs, the databases and the game data, and changes a few
files in the game client. This page lists what is where, what may be deleted and what to back up. The settings
file is described in [lonelyice.ini](/docs/ini), the game data in [Server game data](/docs/server-data), a plugin
folder in [Plugin format](/docs/plugin-format).

## Release folder

What the `lonelyice_release` build target writes to `build/lonelyice-release` ([Building LonelyIce](/docs/building)):

| Path | Contents |
|---|---|
| `LonelyIce.exe` | Launcher, server, extractors and package manager in one program (`LonelyIce` on Linux and macOS). |
| `common.dll`, `shared.dll`, `database.dll`, `game.dll` | The core's shared libraries, which the program and the plugins link against (`libcommon.so` ... on Linux, `.dylib` on macOS). |
| `libcrypto-3-x64.dll`, `libssl-3-x64.dll`, `legacy.dll` | OpenSSL, Windows only (elsewhere the system's). |
| `worldserver.exe`, `authserver.exe` | The fork's plain servers, when the build has them: for running the same plugins as a dedicated server without LonelyIce. The launcher does not use them. |
| `plugins/<id>/` | The plugins shipped with the release, one folder each. |
| `setup/sql.pak` | The core's SQL (base, updates, custom, the static modules' SQL): what the wizard deploys the databases from. |
| `setup/configs.pak` | `worldserver.conf.dist` and the static modules' `.conf.dist` files. |

## Program folder at run time

The launcher adds to the program folder:

| Path | Contents |
|---|---|
| `lonelyice.ini` | The launcher's settings. |
| `plugins/<id>/` | Enabled plugins; the server loads this folder (`AC_PLUGINS_DIR`). |
| `plugins/.disabled/<id>/` | Disabled plugins; the server does not look there. |
| `plugins/.cache/icons/` | Catalog icons, `<id>-<version>.png`. |
| `plugins/.cache/sql.stamp` | Hash of the contents of the loaded plugins' SQL files (for `auth`, `characters`, `world`) and the core database connections at the last server start; a different hash makes the next start apply the plugins' SQL. |
| `plugins/.cache/server.lock` | Held open (locked) by the server process while it runs; `--pkg` refuses to change plugins while it is locked. The file itself stays. |
| `plugins/.staging/` | Packages being unpacked during an install; removed when it finishes. |
| `plugins/.backup/<time>/` | The old plugin folders while an install replaces them; removed when it finishes or is rolled back. Left behind only if the process died in the middle: the old folders are in it. |

With the server folder "Next to LonelyIce.exe", its contents below are in the program folder too.

## Server folder

Chosen in the wizard's "Location" step and kept as `[server] root`:

| Choice | Folder |
|---|---|
| Inside the game folder | `<game folder>/LonelyIce` |
| Next to LonelyIce.exe | the program folder |
| In your user data folder (Linux, macOS) | `~/.local/share/LonelyIce` (`$XDG_DATA_HOME/LonelyIce`), `~/Library/Application Support/LonelyIce` on macOS |
| Another folder | any writable folder |

The server runs with this folder as its working directory, and the config the wizard writes uses paths relative to
it.

| Path | Contents |
|---|---|
| `configs/worldserver.conf` | The server's config. Created from `worldserver.conf.dist` once, with LonelyIce's values: SQLite databases in `db/`, `DataDir = data`, `LogsDir = logs`, `SourceDirectory = sql`, `Updates.EnableDatabases = 0`, `BindIP = 127.0.0.1`, `MapUpdate.Threads` from the CPU count, the rates chosen in the wizard. The wizard does not overwrite an existing one; Settings edits it in place. |
| `configs/worldserver.conf.dist` | Rewritten from `setup/configs.pak` on every "Databases" step. |
| `configs/modules/<name>.conf` | Configs of the static modules and of the plugins (copied from the plugin's `.conf.dist` at install, or when Settings first saves a value of a plugin installed later). On Windows the server reads them from `configs/modules` of its working directory, i.e. here; on Linux and macOS the core looks in `<CONF_DIR>/modules` of its build instead. |
| `configs/modules/<name>.conf.dist` | The static modules' defaults from `setup/configs.pak`. |
| `db/auth.sqlite`, `db/characters.sqlite`, `db/world.sqlite` | The databases with the built-in storage (`[server] location = local`). |
| `db/playerbots.sqlite` | The playerbots plugin's database (it attaches `characters.sqlite`). |
| `data/maps/` | Terrain: extracted by the map extractor (data cache on), or tiles built from the client as grids load, with `stamp.txt` naming the client archives they came from (data cache off). |
| `data/Cameras/` | Cinematic cameras, data cache on only. |
| `data/vmaps/` | Collision from the vmaps step. |
| `data/mmaps/` | Navigation meshes from the mmaps step (optional). |
| `logs/` | The server's log files (`LogsDir`). |
| `backups/<YYYY-MM-DD_HHMMSS>/` | The launcher's backups: `auth.sqlite`, `characters.sqlite`, `playerbots.sqlite` (not `world.sqlite`), copied with SQLite's backup interface, so a running server is fine. |
| `sql/` | The SQL from `setup/sql.pak`, only while the "Databases" step runs; removed afterwards. |
| `data/Buildings/`, `data/mmaps-config.yaml` | Temporary files of the vmaps and mmaps steps. |

With a plugin's database server (`[server] location` not `local`) there is no `db/` in use and no launcher backups.

## Game client

LonelyIce changes these files in the game folder:

| Path | When |
|---|---|
| `Data/<locale>/realmlist.wtf` | Set to `127.0.0.1` by the wizard and before each game start (`[client] writeRealmlist`). The first overwrite keeps `realmlist.wtf.bak`. |
| `WTF/Config.wtf` | `SET locale` before each start (`[client] locale`), `SET realmList` when present, `SET accountName` by the wizard. |
| `Cache/WDB/` | Deleted before each start (`[client] clearWdb`) and by the wizard. |
| `Data/<locale>/patch-<locale>-4.MPQ` | The plugins' client patch, built by the server at start when plugins have patches, removed when none has. An archive of that name LonelyIce did not write is kept as `patch-<locale>-4.MPQ.bak`. |
| `Interface/AddOns/<addon>/`, `Interface/AddOns/lonelyice-addons.txt` | The plugins' addons, copied before each start, and the list of addons LonelyIce placed (addons of removed plugins are deleted). |

The storage check writes `LonelyIce/storage-check.conf` in the system's temporary folder.

## What can be deleted

| Path | Effect |
|---|---|
| `logs/*` | Nothing but the logs. |
| old folders in `backups/` | Nothing but those backups. |
| `sql/`, `data/Buildings/`, `data/mmaps-config.yaml`, `plugins/.staging/` | Leftovers of an interrupted step; the next run of that step starts over. |
| `data/maps/` with the data cache off | The tiles are built again as grids load. |
| `data/mmaps/` | Optional in the wizard; the Maintenance page then reports it missing. |
| `plugins/.cache/icons/` | Downloaded again with the next catalog load. |
| `plugins/.cache/sql.stamp` | The next start runs the updater over the plugins' SQL folders; files already applied are skipped. |
| `worldserver.exe`, `authserver.exe` | Only needed for a dedicated server without LonelyIce. |
| `Data/<locale>/patch-<locale>-4.MPQ` | Built again at the next server start. |

Keep `setup/`: the wizard's "Databases" step needs `sql.pak` and `configs.pak`, and the launcher compares
`sql.pak` with `[server] sqlStamp` to notice database updates. Removing `data/maps/` with the data cache on, or
`db/world.sqlite`, makes the launcher open the wizard.

## What to back up

- `db/`, all four files. The launcher's own backups leave out `world.sqlite`; the world database also holds the
  plugins' named ids (`plugin_ids`) and patch stamps (`plugin_patches`) and anything changed in the world by hand.
  With the server running, use the launcher's backup (Maintenance → Backups) for the other three; copy
  `world.sqlite` with the server stopped.
- `configs/`: the server's and the plugins' settings.
- `lonelyice.ini`: the launcher's settings, including the server folder and the storage.
- `plugins/` if exact plugin versions matter; otherwise they can be installed again from the catalogs.

With a database server, back the databases up with that server's tools.
