# How the launcher runs the server

For contributors: how the launcher window, the server and the wizard's steps work together. They are one
executable ([Command line](/docs/cli)) but never share a process: the launcher starts the server and every heavy
step as a child process of itself, talks to the server over its stdin and stdout, and passes settings through the
environment ([Environment variables](/docs/environment)). A server crash or a failed extractor therefore never
takes the window down, and the launcher can always report what happened.

## Processes

| Process | Command line | Working directory | Started from |
|---|---|---|---|
| Launcher | `LonelyIce [--tray]` | any | the player |
| Server | `LonelyIce --server -c <config>` | the server folder | `ServerProcess` (Start button, Play, auto start, restart) |
| Database steps | `LonelyIce --server --deploy -c <config>`, `--server --dbc fill\|drop -c <config>` | the server folder | `Installer` (wizard) |
| Extractors | `LonelyIce --tool maps\|tiles\|vmaps\|assemble\|mmaps ...` | the server folder or its `data/` | `Installer` (wizard) |
| Storage check | `LonelyIce --server --storage-check -c <temp>/LonelyIce/storage-check.conf` | the temporary folder | `StorageCheck` (wizard, Settings → Storage) |
| Game | `Wow.exe` (Linux, macOS: `[client] runner` with `Wow.exe`) | the game folder | Play |

All children except the game get their stdout and stderr through one pipe and their stdin through another
(`Platform::Child`); on Windows they have no console window. The wizard's steps and the storage check run below
normal priority and have stdin closed at once, so a tool waiting for a key reads end of input instead of hanging.
The storage check is killed after 40 seconds. The game is started without pipes (on Linux and macOS in its own
session). Catalog downloads, plugin installs, backups and the client's addon sync run on threads of the launcher,
not in child processes.

The launcher's loop waits for window events (0.5 s while the window is visible, 1 s while hidden); reader threads
wake it when output arrives. Each tick takes the new log lines, the server state, the account list and the
results of background work.

## Settings reaching the server

The server reads `worldserver.conf` and `configs/modules/*.conf` itself; the launcher adds, per start:

- `AC_PLUGINS_DIR`: the plugins folder next to the executable;
- `LONELYICE_CLIENT` and `LONELYICE_LOCALE` when a client was found (the server builds the plugins' client
  patches into it while it starts);
- `LONELYICE_DATA=client` when the game data is read from the client, else `AC_DBC_FROM_DATABASE=1`;
- with a plugin's database server, the four `AC_*_DATABASE_INFO` connection strings and the storage's `config`
  values;
- when the config is not in `<server folder>/configs`, the module configs next to it as `AC_*` values.

The Settings page writes the server's values straight into `worldserver.conf` and the module and plugin configs,
keeping comments and the file's own style. What a change needs is part of each field: `now`, `reload` (the
launcher sends `reload config` to a running server) or `restart` (the launcher restarts it). The realm name lives
in the auth database: it is kept as `[server] pendingRealmName` and sent with `@@realmname` once the server is
ready.

## Server output (stdout)

Everything the server prints is the log, except lines starting with `@@LI `: control lines, which the launcher
consumes and never shows. Log lines are converted from the console code page on Windows and kept (the last 5000
waiting, the last 600 shown).

| Line | Meaning |
|---|---|
| `@@LI state starting` | First line of every server process. |
| `@@LI state loading` | Databases open, auth listening, the world is loading. |
| `@@LI state ready port=<port> realm=<name>` | The world accepts players. |
| `@@LI state stopping` | The world loop ended; saving and shutting down. |
| `@@LI state failed <reason>` | Start failed: `config`, `client`, `database`, `realm`, `auth` or `network`. The process exits with 1. |
| `@@LI stat players=<n> chars=<n> uptime=<s> diff=<ms>` | Every 2 seconds while running: sessions with a client (real players), characters in the world (bots included), uptime in seconds, average world update time. |
| `@@LI acc <id>\t<name>\t<gm level>\t<characters>\t<last login>\t<bot>` | One account, answer to `@@accounts`; `<bot>` is 1 for `RNDBOT*` accounts. |
| `@@LI char <name>\t<account>\t<level>\t<online>` | One character of a non-bot account, answer to `@@accounts`. |
| `@@LI accend` | End of the account list. |
| `@@LI done ok`, `@@LI done fail` | A console command finished (its output comes before, as log lines). |
| `@@LI realmname ok` | Answer to `@@realmname`. |
| `@@LI deploy ok`, `@@LI deploy failed account` | Result of `--deploy`. |
| `@@LI apply ok`, `@@LI apply failed` | Result of `--apply`. |
| `@@LI dbc <done> <total> <table>`, `@@LI dbc ok`, `@@LI dbc failed` | Progress and result of `--dbc`. |
| `@@LI check db <auth\|characters\|world> ok\|missing\|empty\|error <message>` | `--storage-check`: a database opens and holds the server's tables, does not exist, exists without them, or cannot be reached. A server that does not answer is not asked for the next database. |
| `@@LI check dbc <present> <total>` | `--storage-check`: unpacked DBC tables in the world database (only when it opened). |
| `@@LI check done` | `--storage-check` finished. |

The extractors use the same prefix: `@@LI fail <message>`, `@@LI tiles <done> <total>` and a final
`@@LI exit <code>`; the wizard reads their plain output for progress as well.

## Server input (stdin)

One line per command, UTF-8.

| Line | Effect |
|---|---|
| `@@quit` | Stop: save everyone and exit with 0. |
| `@@accounts` | Report accounts and characters (`acc`, `char`, `accend`). |
| `@@realmname <name>` | Rename the realm in the auth database (`realmname ok`). |
| any other line | A console command, as typed in a server console (e.g. `account create <login> <password>`, `reload config`); its output is printed, then `done ok` or `done fail`. |
| end of input | As `@@quit`: the launcher is gone, so the server saves and stops. |

## States, stop and restart

`ServerProcess` follows the `state` lines: Stopped, Starting, Loading, Ready, Stopping, Failed.

- Start: the launcher checks that setup is complete (else it opens the wizard), that the config exists, that a
  client is there when the data is read from it and that a storage plugin is installed when one is chosen, then
  starts the child.
- Stop: `@@quit` on stdin; the state becomes Stopping at once.
- Restart: stop, and start again when the state reaches Stopped.
- Exit: exit code 0 or 2 (the core's restart code), or any code once the state is Stopping (after `@@quit` or the
  server's own `state stopping`), is Stopped; any other code is Failed with the reason from `state failed`, else
  `exit code <n>`. A process that reported `state failed` stays Failed. After code 2 (`server restart <seconds>`,
  e.g. the Commands tab's Restart card) the launcher starts the server again, as with its own Restart, unless it
  is quitting.
- Closing the window with `[launcher] trayOnClose = 1` only hides it. Quitting (tray menu, or closing with
  `trayOnClose = 0`) stops the server and waits for it; closing again while it saves kills it. When the desktop
  session ends the launcher sends `@@quit` and leaves; the server object then closes stdin and waits up to
  60 seconds before it kills the process.
- The launcher going away in any other way closes the pipe: the server reads end of input and saves.

Plugins are installed, removed, enabled and disabled only while the server is stopped; the databases and the
client follow on the next start (the plugins' SQL through `plugins/.cache/sql.stamp`, their patches while the
server starts, their addons when the game is started).

## Server start sequence

What `--server` does, in order; the one-shot options leave at the marked points.

1. `state starting`; sets `AC_DISABLE_INTERACTIVE=1` for itself; loads the config (`state failed config`) and the log.
2. Loads the plugins made for `worldserver` or `authserver` from `PluginsDir` (see [Plugin API](/docs/plugin-api)),
   then the module configs, and initializes the scripts.
3. `--storage-check` leaves here.
4. With `LONELYICE_DATA=client`, opens the client's data (`state failed client`).
5. Opens the databases, with the updater as the config allows (`state failed database`), then applies the SQL
   of plugins that changed since the last start.
6. `--dbc` leaves here.
7. Applies the plugins' patches: named ids, server DBC rows, recipe SQL, the client archives when
   `LONELYICE_CLIENT` is set. A failure is logged and the start goes on. `--apply` leaves here.
8. `--deploy` creates the account and sets the realm name, then leaves.
9. Opens the client's terrain archives (client data only), reads the realm (`state failed realm`), starts auth on
   `RealmServerPort` (`state failed auth`), `state loading`, loads the world, starts SOAP when enabled and the
   world network (`state failed network`), `state ready ...`, then runs the world loop.

## First run and the wizard

The launcher opens the wizard instead of starting the server while setup is incomplete: no server config, or
with the built-in storage no world database file, or with the data cache on an empty `data/maps`. The wizard's
pages are Client, Location, Data (storage and components), Realm (name, rates, bots, the player's account), Game
(client preparation), Install and Done. Install runs these steps on a worker thread (`Installer`), each chosen
one in this order:

| Step | What runs |
|---|---|
| Databases | Writes the configs from `setup/configs.pak` (new files only get LonelyIce's values), unpacks `setup/sql.pak` into `<server folder>/sql`, runs `--server --deploy` with `AC_UPDATES_ENABLE_DATABASES=7`, `LONELYICE_ACCOUNT`, `LONELYICE_REALMNAME` and `LONELYICE_CLIENT`, requires `@@LI deploy ok`, removes `sql/`. Progress comes from the updater's "Applying" lines. |
| Unpack game data | `--tool maps <client> <data> 5` (maps and cameras), then `--server --dbc fill`. |
| Remove the disk cache | `--server --dbc drop`, then deletes `data/dbc`, `data/Cameras`, `data/maps`. |
| vmaps | `--tool vmaps`, then `--tool assemble`. |
| mmaps | With the data read from the client, `--tool tiles` first; then `--tool mmaps <data> <threads>`. |
| Client preparation | realmlist, WDB cache, the plugins' addons, `accountName` in `Config.wtf`, a desktop shortcut. |

While `maps` and `vmaps` run, the client archives LonelyIce wrote are moved out of the client's sight so the
extractors see stock data. A failed step stops the install; Retry runs it again with the steps after it, skipping
those that finished (a storage switch runs all its steps again). On success
the launcher saves the server folder, client, storage and, after the Databases step, `[server] sqlStamp` and
`realmName` to `lonelyice.ini`.

Changing the storage in Settings → Storage runs a storage check against the new place; if nothing is missing the
setting changes at once, otherwise the wizard shows a Storage page with what it will do (new or updated
databases, unpacking or removing the cache; the account when the databases are new) and runs the same steps.

## Starting the game

Play starts the server first when it is not ready and continues when it is. Then, on a thread, it copies the
plugins' addons into the client; after that it fixes the realmlist (`[client] writeRealmlist`), writes the start
locale to `Config.wtf`, clears `Cache/WDB` (`[client] clearWdb`) and starts the game. When the game exits and
`[launcher] stopWithGame` is on, the server is stopped.

## Source map

| File | Role |
|---|---|
| `src/Main.cpp` | Picks the mode. |
| `src/launcher/Launcher.cpp` | The window, its pages and the tick loop. |
| `src/launcher/ServerProcess.cpp` | The server child: start, stop, the control lines. |
| `src/server/ServerMain.cpp` | `--server`: the control protocol and the one-shot modes. |
| `src/server/AuthService.cpp` | Auth inside the server process. |
| `src/launcher/Wizard.cpp`, `Installer.cpp` | The wizard's pages and its steps. |
| `src/launcher/StorageCheck.cpp`, `StorageForm.cpp` | The storage check and the storage form. |
| `src/launcher/ConfigEnv.cpp` | Config keys as `AC_*` variables, connection strings. |
| `src/launcher/SettingsModel.cpp`, `ConfFile.cpp` | The Settings page and in-place config editing. |
| `src/launcher/LauncherSettings.cpp` | `lonelyice.ini`. |
| `src/tools/ToolMain.cpp`, `PkgMain.cpp`, `PackMain.cpp` | `--tool`, `--pkg`, `--pack`. |
| `src/common/Platform.cpp` | Child processes, environment, consoles per operating system. |
