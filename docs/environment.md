# Environment variables

LonelyIce passes most of what its child processes need through the environment: the launcher starts the server,
the wizard's steps and the storage check with variables of its own (`LONELYICE_*`) and with overrides of the
server's config (`AC_*`). A child gets the launcher's own environment with these added; a variable the launcher
sets replaces one of the same name (on Windows names ignore case). This page lists every variable the launcher,
the server process and the tools read, who sets it and why. None of them is needed to play; the ones marked as
developer variables are for working on LonelyIce.

## LonelyIce variables

| Variable | Read by | Set by | Meaning |
|---|---|---|---|
| `LONELYICE_LANG` | every mode | the launcher, for itself and so for its children | Language of messages and the interface: `en`, `de`, `es`, `fr`, `ru`; unknown: English. Takes precedence over `[launcher] language` ([lonelyice.ini](/docs/ini)). Every process sets it to the language it picked. |
| `LONELYICE_CLIENT` | `--server` | the launcher (server start, when a client was found), the wizard (`--deploy`, `--dbc`), `--pkg apply --client` | The game folder. The server builds the plugins' client patch archives into it while it starts; with `LONELYICE_DATA=client` it reads the game data from it; `--dbc fill` unpacks from it. |
| `LONELYICE_LOCALE` | `--server` | the launcher (server start), the wizard (`--dbc`) | The client locale the game data is read in (`enUS`, `ruRU`, ...): `[client] locale`, else `SET locale` of the client's `Config.wtf`. Empty: the client's first locale. |
| `LONELYICE_DATA` | `--server` | the launcher, when `[server] location = local` and `dataCache = 0` | `client`: DBC tables, terrain and cameras come from the client's archives, nothing unpacked ([Server game data](/docs/server-data)). The server then sets `AC_DBC_FROM_DATABASE=1` for itself. |
| `LONELYICE_ACCOUNT` | `--server --deploy` | the wizard, when an account was entered (required for new databases, optional for existing ones) | `<login>\t<password>\t<gm level>` (tab-separated). Creates the account if it does not exist (login and password upper-cased, as the client sends them) with that GM level on all realms (0: none). An account that exists keeps its password and rights. |
| `LONELYICE_REALMNAME` | `--server --deploy` | the wizard, for new databases and when the realm name was changed; not for a database update | Realm name written to `realmlist` for the config's `RealmID`. Not set: the realm keeps its name. |
| `LONELYICE_REALM_ADDRESS` | `--server` | nobody (the player, for a realm reached from outside the local network) | With `BindIP` not on a loopback address: the address written to `realmlist.address` instead of this computer's LAN address. |
| `LONELYICE_ASSETS` | every mode that reads embedded files | developer | A folder laid out like `src/assets` (`ui/`, `lang/`, `fonts/`, `icons/`). A file found there is used instead of the embedded one, so the interface markup, styles and texts can be edited without rebuilding; the launcher reads them when it (re)loads the page, e.g. on a language switch. The mmaps generator's settings are looked up as `tools/mmaps-config.yaml` there as well. |
| `LONELYICE_UI_SCRIPT` | the launcher | developer | Path of a script file. While the file exists, the launcher runs its lines on its next tick and deletes it: `show` (show the window), `tab <id>` (a tab of the main page), `tray <id>` (a tray menu item), `close` (as the window's close button), `click <element id>`, `type <element id> <text>` (sets the value of a form field). Used to drive the interface from tests. |

## Server config overrides (AC_*)

The core reads every config option from an environment variable as well, and the variable wins over the config
files: `AC_` followed by the option name in upper case, with `.`, `-` and spaces turned into `_` and a `_` between a
lower-case letter and a following capital and between letters and digits.

| Option | Variable |
|---|---|
| `PluginsDir` | `AC_PLUGINS_DIR` |
| `DBC.FromDatabase` | `AC_DBC_FROM_DATABASE` |
| `Updates.EnableDatabases` | `AC_UPDATES_ENABLE_DATABASES` |
| `LoginDatabaseInfo` | `AC_LOGIN_DATABASE_INFO` |
| `Rate.XP.Kill` | `AC_RATE_XP_KILL` |

The launcher sets these:

| Variable | When | Value and purpose |
|---|---|---|
| `AC_PLUGINS_DIR` | every server child (start, `--deploy`, `--dbc`, `--storage-check`); `--pkg apply` | `plugins` next to the executable (`--pkg apply`: its `--plugins` folder): the server loads the plugins the launcher manages, whatever the config says. |
| `AC_DBC_FROM_DATABASE` | server start with the data cache on | `1`: the core loads its DBC stores from the `dbc_*` tables. |
| `AC_UPDATES_ENABLE_DATABASES` | `--deploy` (install, storage switch, and the database update before a start when `setup/sql.pak` changed) | `7`: the updater creates and updates auth, characters and world. Normal starts use the config's value, `0` in the config the wizard writes. |
| `AC_PLAYERBOTS_UPDATES_ENABLE_DATABASES` | `--deploy` | `1`: the same for the playerbots database. |
| `AC_LOGIN_DATABASE_INFO`, `AC_CHARACTER_DATABASE_INFO`, `AC_WORLD_DATABASE_INFO`, `AC_PLAYERBOTS_DATABASE_INFO` | a plugin's storage (`[server] location` not `local`): server start, the wizard's steps, the storage check | `<id>:<host>;<port>;<user>;<password>;<prefix><name>` with `<name>` `auth`, `characters`, `world`, `playerbots` ([lonelyice.ini](/docs/ini), `[remote]`). |
| the same four | the storage check of the built-in files | `sqlite:<server folder>/db/<name>.sqlite`; playerbots with `;attach=characters=<server folder>/db/characters.sqlite`. |
| `AC_<KEY>` of the storage's `config` | with a plugin's storage | The values the plugin's manifest lists under `storage.config` ([Plugin format](/docs/plugin-format)), `{bin}` replaced by the plugin's `server/<platform>` folder and `{exe}` by `.exe` on Windows (empty elsewhere). |
| `AC_DISABLE_INTERACTIVE` | the wizard's steps and the storage check; the server sets it for itself in every mode | `1`: the core's updater never asks on stdin whether to create a missing database; it creates it. |

The core also reads `AC_CONFIG_POLICY`, the severity policy for config problems (see the core's
`doc/ConfigPolicy.md`); LonelyIce does not set it. An `AC_*` variable set in the environment the launcher was
started from reaches the server as well, unless the launcher sets the same name.

## System variables

| Variable | Read by | Meaning |
|---|---|---|
| `HOME` | Linux, macOS | The per-user server folder offered by the wizard (`~/Library/Application Support/LonelyIce` on macOS, `~/.local/share/LonelyIce` on Linux), the default Wine prefix searched for the client (`~/.wine/drive_c`), the desktop entry (`~/Desktop`), and a `DataDir` starting with `~` in the server's config. |
| `XDG_DATA_HOME` | Linux | When set, replaces `~/.local/share` for `applications/lonelyice.desktop`, and for the per-user server folder when it is an absolute path. |

## Build variables

`LONELYICE_CORE_DIR`, `LONELYICE_PLUGIN_DIRS`, `LONELYICE_DEPS` and `LONELYICE_RELEASE_DIR` are CMake cache
variables of the build, not environment variables ([Building LonelyIce](/docs/building)).
