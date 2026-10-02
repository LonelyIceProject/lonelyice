# Command line

`LonelyIce.exe` (`LonelyIce` on Linux and macOS) is one program with several modes. Without a mode argument it
opens the launcher window; the launcher runs the other modes as child processes of the same executable (the
server, the extractors, the database steps of the wizard), and they can be run by hand the same way. The mode is
the first of `--tui` (`-nw`, `--nw`), `--headless`, `--server`, `--tool`, `--pkg`, `--backup` or `--pack` found anywhere on the command line. Builds without the graphical launcher open the TUI by default when it is enabled. On Windows the
executable is a GUI program: the console modes attach to the console they were started from (`--server` reads
commands from it as well). Messages of `--pkg` and `--backup` are in the launcher's language ([Environment variables](/docs/environment),
`LONELYICE_LANG`).

## Modes

| Mode | Started by | Purpose |
|---|---|---|
| (none) | the player | The launcher window. |
| `--tui`, `-nw`, `--nw` | the administrator over SSH or a local terminal | Plugin installation, configuration and storage preparation. |
| `--headless` | the administrator, external process tools | Run the configured server in the foreground without stdin commands. |
| `--server` | the launcher, the wizard, `--pkg apply` | World and auth server in one process, and the one-shot database steps. |
| `--tool` | the wizard | Client data extractors: maps, terrain tiles, vmaps, mmaps. |
| `--pkg` | the player, scripts | Plugin package manager. |
| `--backup` | the player, scripts | Backups of the databases: make, list, restore, export. |
| `--pack` | the build (`lonelyice_release`) | Packs the core's SQL and default configs into `setup/`. |

## Launcher

```
LonelyIce [--tray]
```

| Option | Meaning |
|---|---|
| `--tray` | Start with the window hidden in the notification area. Only when `launcher.trayOnClose: true` ([server.yaml](/docs/configuration)); otherwise the window opens as usual. |

Exit code 0; 1 when the window or its OpenGL context cannot be created or the interface cannot be loaded (a
message box says which).

## Server

For terminal configuration and a separate server process using saved launcher settings, see
[Terminal setup and headless servers](/docs/terminal). Both `--tui` and `--headless` accept `--settings <server.yaml>`.
The TUI does not start or stop servers and does not install operating-system services.

`--server` is the low-level core entry point: it reads `.conf` files and environment variables directly and
does not load or regenerate the YAML profile. Use `--headless --settings <profile>` for a configured deployment.

```
LonelyIce --server [-c <worldserver.conf>] [--deploy | --apply | --dbc fill|drop | --storage-check]
                   [--config-policy <policy>] [--no-console]
```

Without a one-shot option the process runs auth and world until it is stopped. It writes the log and
`@@LI` control lines to stdout and reads console commands from stdin, one per line; the protocol is described in
[How the launcher runs the server](/docs/launcher-architecture). Closing stdin (end of input) saves and stops the
server. The databases, the plugins folder and the game data come from the config and the environment
([Environment variables](/docs/environment)).

| Option | Meaning |
|---|---|
| `-c <file>`, `--config <file>` | The worldserver.conf to use. Default: `configs/worldserver.conf` in the working directory on Windows, `<CONF_DIR>/worldserver.conf` of the core build elsewhere. Relative paths in the config (`DataDir`, `sqlite:` files) are resolved against the working directory. |
| `--deploy` | Open (and, as the config's `Updates.*` options allow, create and update) the databases, apply the plugins' patches, create the player's account (`LONELYICE_ACCOUNT`, when set and missing) and set the realm name (`LONELYICE_REALMNAME`, when set), then exit. The wizard's "Databases" step. Reports `@@LI deploy ok`, `@@LI deploy failed account` or `@@LI deploy failed patches`; a database failure ends it before with `@@LI state failed database`. |
| `--apply` | Open the databases, apply the SQL of plugins changed since the last start and the plugins' patches (DBC rows, recipe SQL, client archive when `LONELYICE_CLIENT` is set), then exit. `--pkg apply` runs this. |
| `--dbc fill` | Unpack the client's DBC files (`LONELYICE_CLIENT`, `LONELYICE_LOCALE`, else the client's first locale) into `dbc_*` tables of the world database, then exit ([Server game data](/docs/server-data)). Any value other than `drop` fills. |
| `--dbc drop` | Drop the `dbc_*` tables from the world database, then exit. |
| `--storage-check` | Load the plugins (so their database backends are registered), try to open the auth, characters and world databases the config (or the environment) names and report what it found, then exit. Needs no server folder. |
| `--config-policy <policy>` | The core's config severity policy (`--config-policy=<policy>` also works), see the core's `doc/ConfigPolicy.md`. |
| `--no-console` | Disable stdin commands and shutdown on EOF. Use SIGINT or SIGTERM for graceful shutdown. |

When several one-shot options are given, the first applicable in this order wins: `--storage-check`, `--dbc`,
`--apply`, `--deploy`. The one-shot modes report their result as a control line (`@@LI deploy ok`,
`@@LI apply failed`, `@@LI dbc ok`, `@@LI check done`, ...).

| Exit code | Meaning |
|---|---|
| 0 | Stopped normally, or the one-shot step succeeded. `--storage-check` exits with 0 once the check has run; its result is in the control lines. |
| 1 | Failed: config, client data, database, realm, auth or network (reported before as `@@LI state failed <reason>`), a one-shot step failed, or the core stopped with its error code. |
| 2 | The core's restart code (e.g. the `server restart` console command). The GUI launcher restarts its owned server; a standalone or headless process exits with this code. |

```
cd C:\Games\LonelyIce
LonelyIce.exe --server -c .runtime\configs\worldserver.conf
LonelyIce.exe --server --dbc fill -c .runtime\configs\worldserver.conf
```

The working directory matters: the default config written by the wizard uses paths relative to the server folder
(`sqlite:db/world.sqlite`, `DataDir = data`), so run it from there.

## Tools

```
LonelyIce --tool maps     <client dir> <data dir> [<mask>]
LonelyIce --tool tiles    <client dir> <data dir> <locale>
LonelyIce --tool vmaps    <client dir> <data dir>
LonelyIce --tool assemble <data dir>
LonelyIce --tool mmaps    <data dir> <threads>
```

| Tool | What it does |
|---|---|
| `maps` | The core's map extractor: writes `<data>/maps`, `<data>/dbc` and `<data>/Cameras`. `<mask>` selects what, as a sum of 1 (maps), 2 (DBC files), 4 (cameras); default 7 (all). The wizard runs it with 5 and puts the DBC files into the database instead (`--server --dbc fill`). Both paths must be shorter than 120 characters. |
| `tiles` | Builds every terrain tile of the client into `<data>/maps` the way the server builds them on demand when it reads the client ([Server game data](/docs/server-data)); needed before `mmaps` in that mode. `<locale>` is a client locale such as `enUS`. Progress: `@@LI tiles <done> <total>`. |
| `vmaps` | The core's vmap extractor: deletes `<data>/Buildings`, then extracts the raw models into it (it runs inside `<data>`). |
| `assemble` | Turns `<data>/Buildings` into `<data>/vmaps` and deletes `Buildings`. |
| `mmaps` | The core's mmaps generator with its embedded settings: writes `<data>/mmaps`. Needs `<data>/maps` (not empty) and `<data>/vmaps`. `<threads>` is at least 1. A temporary `<data>/mmaps-config.yaml` is written and removed. |

Output goes to stdout. A failure is reported as `@@LI fail <message>`, and every tool ends with
`@@LI exit <code>`.

| Exit code | Meaning |
|---|---|
| 0 | Done. |
| 1 | Unknown tool or wrong argument count, `tiles` or `assemble` failed, or the extractor's own failure code. |
| 2 | `maps`: a path is too long; `vmaps`: `<data>` cannot be entered; `mmaps`: maps or vmaps missing, or the settings could not be written. |

The wizard moves the client archives LonelyIce wrote (the plugins' client patch) out of the client's sight while
`maps` and `vmaps` run, so the extractors see stock data. Run by hand, the extractors read whatever is in the
client's `Data` folder.

```
LonelyIce.exe --tool maps "C:\Games\Client-3.3.5a" "C:\Games\LonelyIce\data" 5
LonelyIce.exe --tool mmaps "C:\Games\LonelyIce\data" 8
```

## YAML profiles

`--settings <server.yaml>` selects the portable profile; sibling `local.yaml` is loaded automatically.
GUI, TUI, headless, package and backup modes use this profile. To reproduce the plugin set, preview
`--pkg sync --settings <server.yaml> --dry-run`, then apply without `--dry-run`. `--pkg snapshot` explicitly
records installed versions and states. See [YAML configuration](/docs/configuration).

## Package manager

```
LonelyIce --pkg <command> [<arguments>] [--plugins <dir>] [--index <catalogs>]
```

| Command | Meaning |
|---|---|
| `list` | Installed plugins: id, version, name; disabled ones are marked. |
| `available [--locale <lang>]` | Packages of the catalogs for this core and platform; installed ones show the installed version. With `--locale`, only packages whose `locales` name that language (or `*`). |
| `install <id>[@<range>]...` | Installs the plugins with their dependencies, all or nothing. `<range>` as in `depends` (`>=1.2.0`, `^1.2`, `>=1.0.0,<2.0.0`, `1.2.x`, `*`; default `*`; quote it when it has spaces or `<`/`>`). Refused when a plugin needed is disabled. |
| `update [<id>...]` | Updates the given plugins, or every enabled plugin that has a newer compatible version. |
| `remove <id>` | Deletes the plugin folder. Refused while an enabled plugin needs it. |
| `enable <id>`, `disable <id>` | Moves the plugin between `plugins/<id>` and `plugins/.disabled/<id>`. Disabling is refused while an enabled plugin needs it; enabling is refused while one of the plugin's dependencies is missing, disabled or out of range, or a conflict with an enabled plugin exists. |
| `sync [--dry-run]` | Reconciles installed plugins with the YAML profile's exact versions and enabled states. Preview the plan with `--dry-run`; unlisted plugin folders are retained but disabled. |
| `snapshot` | Explicitly records installed versions and enabled states in the selected profile, preserving settings. |
| `apply [-c <worldserver.conf>] [--client <game folder>]` | Applies the plugins' SQL and patches to the databases (and the client archives with `--client`) now instead of on the next server start: runs `--server --apply` in this process with `AC_PLUGINS_DIR` set to the plugins folder. Relative paths of the config are resolved against the current directory. |
| `pack <plugin folder> [<out dir>]` | Writes `<out dir>/<id>-<version>.zip` (default: the current directory), copies the plugin's `icon.png` next to it and prints the package's entry for a catalog's `index.json` ([Plugin format](/docs/plugin-format), section 8); it does not write or change an `index.json`. Refused for unknown `locales` codes and for `depends` ranges that cannot be read. |

| Option | Default | Meaning |
|---|---|---|
| `--plugins <dir>` | `plugins` next to the executable | The plugins folder to work on. |
| `--index <catalogs>` | `packages.index` of `server.yaml` next to the executable | Catalogs, separated by `;`: http(s) URLs of an `index.json`, index files or folders holding `index.json`. |
| `--settings <profile>` | `server.yaml` beside the executable | YAML profile, merged with sibling `local.yaml`. |
| `--dry-run` | off | Preview `sync` without changing plugins or the profile. |
| `-c`, `--config <file>` | generated runtime config | Optional low-level config override for `apply`; without it, the launcher generates config and startup overrides from YAML. |
| `--client <dir>` | | The game folder for `apply`. |
| `--locale <lang>` | | Language filter for `available`. |

`list`, `remove`, `enable`, `disable`, `apply` and `pack` do not read the catalogs; the other commands do, and report a
catalog that cannot be read and go on with the others. `install`, `update`, `remove`, `enable`, `disable` and
`apply` refuse while a server runs on the plugins folder (it holds `plugins/.cache/server.lock` locked):
`error: the server is running on this plugins folder (<folder>): stop it first`. Changes to the databases and the
client follow on the next server start (or `apply`). Exit code 0 on success, 1 on an error (printed as
`error: <message>`) or without a command (usage).

```
LonelyIce.exe --pkg install lonelyice.tactics@^1.3
LonelyIce.exe --pkg --index "D:\catalog" available --locale ru
LonelyIce.exe --pkg pack plugins\lonelyice.tactics D:\catalog
cd C:\Games\LonelyIce
LonelyIce.exe --pkg apply --settings server.yaml --client C:\Games\Client-3.3.5a
```

## Backups

```
LonelyIce --backup [<command>] [-c <worldserver.conf>] [--root <server folder>]
```

The same backups as the launcher's, in `<server folder>/backups/store` ([Backups](/docs/backups)), with the
retention of the selected YAML profile (`backup.days`, `budget`).

| Command | Meaning |
|---|---|
| `create` (or none) | A backup now, while the server runs as well. Prints `Backup <id>: +<size>`, or `Nothing changed since backup <id>.` without making one. Then old backups are thinned out. |
| `list` | Backups, newest first: id, time, size added, what changed; then the size of the store. |
| `restore <id>` | Puts the backup's databases in place. Refused while any process has a database open (stop the server first); the current state is backed up first and its id printed. |
| `export <id>` | Writes the backup's database files into `<server folder>/backups/export/<id>/`. |

| Option | Default | Meaning |
|---|---|---|
| `-c`, `--config <file>` | as the launcher: generated `<server folder>/.runtime/configs/worldserver.conf` | The server config naming the databases (`*DatabaseInfo`, `modules/playerbots.conf`). |
| `--root <dir>` | the folder above the config's `configs` folder, else `server.root`, else the executable's folder | The server folder: relative database paths and `backups/`. |

Exit code 0 on success (also when nothing changed), 1 on an error (printed as `Error: <message>`) or an unknown
command (usage).

```
LonelyIce.exe --backup
LonelyIce.exe --backup list
LonelyIce.exe --backup restore 2026-09-30_040012
```

## Release packer

```
LonelyIce --pack <core source root> <out dir>
```

Run by the `lonelyice_release` build target with the core (`external/azerothcore`) and `<release>/setup`
([Building LonelyIce](/docs/building)). Writes

- `sql.pak`: every `.sql` file of `data/sql/base`, `data/sql/updates`, `data/sql/custom`, `data/sql/overrides`
  and `modules/*/data/sql`, except paths with a `create`, `old` or `archive` folder;
- `configs.pak`: `worldserver.conf.dist` and the `modules/*/conf/*.conf.dist` of the static modules (as
  `modules/<name>.conf.dist`).

The wizard unpacks them into the server folder ([Install layout](/docs/install-layout)). Exit code 0; 1 without
both arguments or when a file cannot be written.
