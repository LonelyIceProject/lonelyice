# ALE, AIO and Mythic Plus

Three independent upstream projects are available as LonelyIce packages. Their port repositories contain
pinned source revisions, build configuration and patches; upstream source is fetched during compilation.
The first port releases are experimental and need in-game verification.

| Package | Purpose | Dependencies |
|---|---|---|
| `mod-ale` | Lua 5.2 server scripting | LonelyIce core ABI `lonelyice-ac-2` |
| `aio` | Server Lua and the AIO client addon | `mod-ale` |
| `mod-mythic-plus` | Dungeon difficulty, scaling, rewards and advancement | LonelyIce core ABI `lonelyice-ac-2`; client patch |

ALE and Mythic Plus provide Windows x64 and Linux x64 libraries. AIO contains Lua and client addon files.

## Installing ALE and AIO

Install **AIO addon framework** on the Plugins page or in the terminal wizard. Its ALE dependency is installed
automatically. For a command-line installation:

```sh
LonelyIce --pkg install aio
LonelyIce --pkg apply --client /path/to/WoW
```

Restart the server after installation. AIO's `AIO_Client` addon must be installed on every participating
WoW 3.3.5a client. On a headless server, copy `plugins/aio/client/addons/AIO_Client` to the player's
`Interface/AddOns/AIO_Client` directory. Restart the game or reload its UI.

AIO is a framework for custom Lua interfaces. Installing it alone does not add a gameplay interface.
Consumer packages can depend on `aio`, place Lua files under `lua/`, and load the framework with
`local AIO = require("AIO")`. Handlers must validate client-supplied arguments.

ALE loads Lua from enabled, successfully loaded packages and from `ALE.ScriptPath` (default `lua_scripts`).
Configure ALE on the plugin's settings screen; values are saved in the active YAML profile. Extensions load
first; ordinary scripts follow ALE's filename ordering. Use `require` for Lua module dependencies.
Auto reload watches the local scripts directory only; restart after updating packages.

Lua 5.2.4 is statically included, so no separate Lua runtime is needed. ALE differs from original Eluna:
check existing scripts individually. Playerbots-specific Lua bindings are excluded from this engine package.
Lua runs with the server's process privileges, so install scripts from trusted sources.

## Installing Mythic Plus

Back up the world and characters databases before installing on an existing realm. The package adds custom
items, vendors and progression tables and alters base stats and rewards. Its first release is experimental;
combat balance, dungeon completion, rewards and client interactions still need in-game testing.

Install **Mythic Plus** from the catalog or run:

```sh
LonelyIce --pkg install mod-mythic-plus
LonelyIce --pkg apply --client /path/to/WoW
```

Configure the plugin through GUI or TUI and restart the server. Apply its client patch to every participating
client. For a headless realm, distribute the generated client patch separately. Named advancement spell IDs
are allocated through LonelyIce's DBC recipes. The SQL is adapted to LonelyIce's current schema and database
dialect layer, including the default SQLite backend.

Create a group. Before entering a dungeon, its leader can use `.mp set mythic`, `.mp set legendary`, or
`.mp set ascendant`. `.mp set normal` and `.mp set heroic` clear the custom group difficulty. `.mp status`
shows settings. Difficulty changes are rejected inside dungeons. Death allowance `0` means unlimited;
disabled dungeon IDs and enabled difficulties are comma-separated lists in the settings.

Mythic Plus does not depend on ALE or AIO. Its upstream `MPUi` interface is absent from the pinned upstream
repository and is not bundled. Installing AIO does not replace that interface. SQL intended for a separate
NPCBots wander-node module is omitted.

## Sources and validation

- [ALE port and build instructions](https://github.com/LonelyIceProject/mod-ale-plugin), based on
  [AzerothCore ALE](https://github.com/azerothcore/mod-ale).
- [AIO package and build instructions](https://github.com/LonelyIceProject/aio-plugin), based on
  [Rochet2 AIO](https://github.com/Rochet2/AIO).
- [Mythic Plus port and build instructions](https://github.com/LonelyIceProject/mod-mythic-plus-plugin), based on
  [araxiaonline Mythic Plus](https://github.com/araxiaonline/mod-mythic-plus).

Windows and Linux compilation and package checks were performed. Mythic Plus SQL was applied to an isolated
current SQLite schema; the adapter has regression tests. AIO was exercised with a mocked Lua host for startup,
event registration, message serialization and protocol separation. These checks do not verify gameplay or
communication between a real WoW client and ALE.
