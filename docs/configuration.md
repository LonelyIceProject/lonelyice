# YAML configuration

LonelyIce uses `server.yaml` beside the executable, or the profile selected with `--settings <path>`.
The sibling `local.yaml` overrides it recursively. Both files use the same structure. Lists and scalar values
are replaced; mappings are merged by key. No INI/CONF import or backward compatibility is provided.

## Portable server profile

```yaml
format: 1
server:
  realmName: LonelyIce
  location: local
  dataCache: true
  settings:
    Rate.XP.Kill: 3.0
    MaxPlayerLevel: 80
    BindIP: 0.0.0.0
    RealmServerPort: 3724
    WorldServerPort: 8085
plugins:
  playerbots:
    version: "1.0.4"
    enabled: true
    settings:
      AiPlayerbot.MinRandomBots: 100
      AiPlayerbot.MaxRandomBots: 100
packages:
  index: https://lonelyice.org/packages/index.json
  disabled: ""
  locale: ""
launcher:
  language: en
  autoStart: false
  stopWithGame: false
  trayOnClose: true
  uiScale: 0
client:
  locale: enUS
  writeRealmlist: true
  clearWdb: true
backup:
  schedule: daily
  time: "04:00"
  days: 14
  budget: 2048
```

Plugin IDs are the manifest's `id`, not necessarily the repository name. Versions are exact strings.
Only enabled plugins named by the applied profile run. Extra installed folders are retained but disabled,
with every change listed in the plan. A compatible package must exist for the destination platform and ABI.

`server.settings` contains the core's config keys. `plugins.<id>.settings` contains that plugin's config keys,
including keys not exposed by its settings schema. Dots in a config key are literal, not nested paths.
The data folder is managed as `<server.root>/data`; set local `server.root` to relocate it. A conflicting
`DataDir` override is rejected.

Values retain YAML types: booleans, integers, floats and strings. Quote version numbers, numeric strings,
paths containing special characters and clock times. Unknown fields survive interface saves.

## Machine-local overrides

```yaml
server:
  root: /srv/lonelyice
client:
  path: /srv/wow-client
  runner: wine
remote:
  host: 192.168.1.20
  port: 3306
  user: acore
  password: "replace-on-this-host"
  prefix: acore_
```

Relative paths are resolved against the profile directory. Paths, database credentials, deployment stamps
and backup timestamps are saved in `local.yaml`. Do not share that file. On POSIX, generated local files
are owner-readable/writable only. You can override any portable setting locally, for example
`server.settings.BindIP`; edits in the interfaces update its existing local override.

## Sharing and applying

Send `server.yaml`, then configure `local.yaml` on the other machine. Use the TUI's profile plan action,
or preview/apply from the command line:

```sh
./LonelyIce --pkg sync --settings /srv/lonelyice/server.yaml --dry-run
./LonelyIce --pkg sync --settings /srv/lonelyice/server.yaml
./LonelyIce -nw --settings /srv/lonelyice/server.yaml
./LonelyIce --headless --settings /srv/lonelyice/server.yaml
```

`--pkg snapshot --settings <profile>` explicitly records installed versions and enabled states into a
profile while preserving plugin settings. Ordinary package actions update their affected entries;
opening an interface never overwrites an imported desired list with the current installation.

The release ships a profile listing its bundled plugins. A different profile may require packages that
are not included in the release. A plugin connects to MySQL; it does not install a MySQL database server.

## Generated core configuration

Before setup and launch, LonelyIce derives `.runtime/configs/worldserver.conf` and
`.runtime/configs/modules/*.conf` in the server root from internal packaged defaults and the merged YAML.
Do not edit those generated files: they are replaced. GUI, TUI, installer, backups and headless launch share
the YAML model; `.conf.dist`, plugin manifests and settings schemas are package metadata, not user configs.
There is no systemd integration or server supervision in the TUI.
