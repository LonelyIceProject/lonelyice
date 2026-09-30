# lonelyice.ini

`lonelyice.ini` next to the executable holds the launcher's own settings: where the game client and the server
folder are, where the databases live, how the game is started, backups and plugin catalogs. The server's settings
are not here; they are in the server folder's `configs/worldserver.conf` and `configs/modules/*.conf`
([Install layout](/docs/install-layout)). The launcher creates the file, reads it at start and writes it whenever
one of its settings changes and when it exits. Other modes read it too: every mode takes `[launcher] language`
for its messages, and `--pkg` takes `[packages] index` ([Command line](/docs/cli)).

## Format

```ini
[client]
path=C:\Games\WoW335
locale=ruRU

[server]
root=C:\Games\WoW335\LonelyIce
location=local
dataCache=0
```

- `[section]` and `key=value` lines; lines starting with `;` or `#` are comments. Section and key names ignore
  case. Values are taken as written (after trimming spaces); quotes are not removed.
- UTF-8 (a file in UTF-16 with a byte order mark is read as well). Paths are UTF-8.
- Flags are on only when the value is exactly `1`; anything else is off.
- Saving replaces the lines of the keys the launcher knows and keeps everything else (comments, unknown keys).
  Every known key is written, with its current value, on every save: edit the file while the launcher is closed,
  or a running launcher overwrites the change the next time it saves.

## [client]

| Key | Default | Meaning | Set by |
|---|---|---|---|
| `path` | empty | The WoW 3.3.5a folder (`Wow.exe` and `Data/common.MPQ`). Empty or not a client: the launcher looks next to the executable, in its parent folder, in the parent's subfolders (two levels), then where an installer put the game (Windows registry `InstallPath`; `~/.wine/drive_c/Program Files (x86)` and `Program Files` elsewhere), and writes the first client found here. | Overview → Browse; the wizard ("Client" step) on install |
| `locale` | empty | The client language the game starts in (`enUS`, `ruRU`, ...): written to `WTF/Config.wtf` (`SET locale`) before the game starts, and the locale the server reads its data in (`LONELYICE_LOCALE`). Empty: `Config.wtf` is left alone and its `SET locale` is used. | Overview → locale chips; Settings → Launcher → Client language |
| `runner` | `wine` | Linux and macOS: the program that runs `Wow.exe`, with arguments separated by spaces (e.g. `wine64`, a Proton or CrossOver wrapper). Unused on Windows. | file only |
| `writeRealmlist` | `1` | Before the game starts, set `realmlist.wtf` of the start locale (of every locale when none is known) to `127.0.0.1` when it points elsewhere (the first overwrite keeps `realmlist.wtf.bak`), and a `realmList` line in `Config.wtf` if there is one. | Settings → Launcher → Check realmlist before launch |
| `clearWdb` | `1` | Delete the client cache `Cache/WDB` before the game starts. | Settings → Launcher → Clear the client cache |

## [server]

| Key | Default | Meaning | Set by |
|---|---|---|---|
| `root` | empty | The server folder (configs, databases, game data, logs, backups). Empty: the executable's folder. | the wizard ("Location" step) |
| `config` | empty | The worldserver.conf to run. Empty: `<root>/configs/worldserver.conf`; without `root`, `configs-sqlite/worldserver.conf` or `configs/worldserver.conf` next to the executable, whichever exists. | file only; the wizard clears it on install |
| `location` | `local` | Where the databases are: `local` (SQLite files in `<root>/db`) or the `storage` id of an installed plugin (a database server, e.g. `mysql`), reached with `[remote]` ([Server game data](/docs/server-data)). | Settings → Storage; the wizard ("Data" step) |
| `dataCache` | `0` | `1`: the game data is unpacked (DBC files in the world database, maps and cameras in `DataDir`); `0`: the server reads it from the client. Always `1` when `location` is not `local`. | Settings → Storage; the wizard ("Data" step) |
| `realmName` | empty | The realm name last reported by the server (shown in Settings while the server is down). | the launcher: when the server is ready, and after installing the databases |
| `pendingRealmName` | empty | A new realm name, sent to the server (`@@realmname`) when it is next ready, then cleared. | Settings → Realm → Realm name |
| `sqlStamp` | empty | Size and time of the `setup/sql.pak` the databases were last deployed from. When the release's `sql.pak` differs, the launcher reports database updates and the wizard offers the "Databases" step again. | the launcher, after the wizard installed the databases |

Older versions wrote `[server] storage = client | unpacked | mysql` and a `[mysql]` section (`host`, `port`,
`user`, `password`, `prefix`). They are still read (`mysql` becomes `location = mysql`, `client` becomes
`dataCache = 0`, anything else `dataCache = 1`) and replaced by the keys above on the next save.

## [remote]

The connection of a plugin's database server, used when `[server] location` is not `local`. The launcher passes
every database as `<location>:<host>;<port>;<user>;<password>;<prefix><name>` for `auth`, `characters`, `world`
and `playerbots` ([Environment variables](/docs/environment)).

| Key | Default | Meaning | Set by |
|---|---|---|---|
| `host` | `127.0.0.1` | Server address. | Settings → Storage; the wizard ("Data" step) |
| `port` | empty | Port; empty: the storage's default `port` from the plugin's manifest. | same |
| `user` | `acore` | User name. | same |
| `password` | empty | Password, stored as plain text. | same |
| `prefix` | `acore_` | Database name prefix: `acore_auth`, `acore_characters`, `acore_world`, `acore_playerbots`. | same |

## [launcher]

| Key | Default | Meaning | Set by |
|---|---|---|---|
| `language` | empty | Interface language: `en`, `de`, `es`, `fr`, `ru`. Empty or unknown: English. The environment variable `LONELYICE_LANG` takes precedence. | the language switch in the window's header |
| `uiScale` | `0` | Interface size in percent, on top of the system's display scale; 50 to 300. `0`: the largest of 150, 125 and 100 % whose window fits the screen; the value chosen is written back. | Settings → Launcher → Interface scale (100 to 200 %); Ctrl + mouse wheel (steps of 25, 100 to 200) |
| `autoStart` | `0` | Start the server when the launcher starts (not while the wizard is needed). | Settings → Launcher → Start the server with the launcher |
| `stopWithGame` | `0` | Stop the server when the game the launcher started exits. | Settings → Launcher → Quitting the game stops the server |
| `trayOnClose` | `1` | Closing the window hides it in the notification area and the server keeps running; `0`: closing stops the server and quits. Also required for `--tray`. | Settings → Launcher → Closing the window minimizes to tray |

## [backup]

Scheduled backups copy the SQLite databases (auth, characters, playerbots) into `<root>/backups`
([Install layout](/docs/install-layout)). They run only while the launcher runs and only with
`[server] location = local`; the launcher looks every 30 seconds.

| Key | Default | Meaning | Set by |
|---|---|---|---|
| `time` | `04:00` | Daily time `HH:MM` (exactly five characters): the first check at or after it on a day starts that day's backup. Empty (or any other length): no scheduled backups. | Settings → Launcher → Backup, time |
| `keep` | `7` | How many backup folders to keep (at least 1); the oldest go after each backup, manual ones included. | Settings → Launcher → Backups to keep |
| `lastDay` | empty | `YYYY-MM-DD` of the last scheduled backup. | the launcher |

## [packages]

| Key | Default | Meaning | Set by |
|---|---|---|---|
| `index` | `https://lonelyice.com/packages/index.json` | Plugin catalogs in use, separated by `;`: http(s) URLs of an index, index files or folders holding `index.json` ([Plugin format](/docs/plugin-format), section 8). Empty: no catalogs. | Plugins → Package catalogs (add, switch on and off, remove, bring back the LonelyIce catalog) |
| `disabled` | empty | Catalogs kept in the list but not read, same format. | same |
| `locale` | empty | Catalog filter: only packages whose `locales` name this language (or `*`). Empty: all. | Plugins → Catalog → Language |
