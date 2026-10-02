# Terminal setup and headless servers

`LonelyIce --tui` (also `-nw` or `--nw`) opens a keyboard-driven setup and configuration interface.
It uses the same plugin manager, configuration model, storage checks and installer as the graphical launcher.
It does not start, stop or supervise the game server, and does not install operating-system services.

```sh
./LonelyIce --tui
./LonelyIce -nw --settings /srv/lonelyice/server.yaml
```

An interactive terminal is required. Over SSH, request a terminal when running a command directly:

```sh
ssh -t wow-host '/opt/lonelyice/LonelyIce -nw --settings /srv/lonelyice/server.yaml'
```

The overview shows the selected paths and storage. Plugins lists installed packages and can load the catalog
on request. Settings includes the core's settings and groups supplied by installed plugins. Preparation handles
first-run setup and storage changes. Use Tab/Shift-Tab to move between controls, arrows to choose entries, Space
to toggle a checkbox and Enter to activate a button. The interface's footer describes additional shortcuts.

## Installation wizard

On an unprepared server the TUI opens **Installation wizard** automatically. You can also open it from
Overview or Preparation. It guides you through paths, storage, components and an optional account, then checks
the storage and displays a preparation plan. Confirm the plan to install; generated-file replacement requires
an additional confirmation. Progress and recent output are displayed while preparation runs. Esc cancels;
failed or cancelled preparation requires another storage check before retrying.

Keep the complete release together, including `setup/` and `plugins/`. Supply a complete local WoW 3.3.5a client
folder as a game-data source even on a headless host. Server-only preparation is selected by default. Vmaps and
mmaps are optional choices. The wizard saves the YAML profile and local overrides after successful preparation
and leaves server startup to `--headless`.

Install or enable optional plugins in Plugins before choosing their provider in the wizard. Provider connection
fields follow the enabled storage plugins; other dynamic plugin settings remain available in Settings. Returning
to configuration preserves the wizard's draft until you save, prepare or discard it. Before starting a LAN server,
configure its network access in Settings as described below.

See [YAML configuration](/docs/configuration) for profile sharing, local overrides and package synchronization.

## Plugins and their settings

Loading a catalog and downloading packages run in the background while the interface shows progress.
Review the dependency plan before installing. Packages are filtered by platform and core ABI, just as in `--pkg`.
An installed plugin's settings are read from its `settings.json`: booleans, integers, decimal numbers, strings
and choices use their labels, defaults, hints and limits. There is no built-in list of plugin-specific fields.

Saving configuration does not restart a running server. Saved changes take effect on its next start. Stop an externally running server before changing
plugin packages, preparing game data or checking/changing storage.

## Storage plugins

A compatible installed storage plugin adds its provider to the storage choices. Installing the plugin leaves
the current storage selected. Choose the new provider, enter the host, port, user, password and database prefix,
then check the connection and review the preparation plan. Changing a connection field invalidates its old check.
The selected storage is saved only after preparation succeeds.

A MySQL plugin connects to an existing MySQL server; it does not install MySQL in the operating system. A
Windows-only package cannot be installed on a Linux server. Use a compatible Linux build from the release or catalog, or build and install it separately with the matching core ABI.

Switching storage selects another set of databases; it does not migrate characters. The original databases
remain available when their storage is selected again. Remote storage requires unpacked game tables. Preparing
them requires a local path to the user's WoW 3.3.5a client. Server-only preparation does not alter the game
client's realmlist, addons or shortcuts.

## Running the configured server

After saving the configuration and closing the TUI, start the server separately:

For LAN clients, open **Settings → Network and resources** and turn off **This computer only**.
The YAML equivalent is `server.settings.BindIP: 0.0.0.0`; this may also be a machine-local override.
The realm then advertises this host's LAN address. Clients use `set realmlist <host-address>` (include the
login port if it differs from 3724). The storage choice `local` refers to SQLite databases, independently
of which network interfaces accept game connections.

```sh
./LonelyIce --headless --settings /srv/lonelyice/server.yaml
```

`--headless` reads launcher settings, resolves paths relative to the settings file, prepares the selected
storage and game-data environment, and runs auth and world in the current process. It uses no graphics and
does not read commands from stdin. End-of-input therefore does not stop it. SIGINT or SIGTERM requests a graceful
shutdown. The exit code is the server's exit code, including 2 for a requested restart; there is no automatic
restart loop in this mode.

For an optional background process on a POSIX host, use the host's normal process tools:

```sh
nohup /opt/lonelyice/LonelyIce --headless --settings /srv/lonelyice/server.yaml \
    </dev/null >>/srv/lonelyice/server-console.log 2>&1 &
server_pid=$!
printf 'Server PID: %s\n' "$server_pid"
# Later, with the PID of this server process:
kill -TERM "$server_pid"
```

This does not arrange startup after reboot or restart after a crash. Automatic backups scheduled by the
graphical launcher also require that launcher; headless deployments can invoke `--backup` from their existing
scheduler. The TUI itself owns no game-server process, so closing it has no effect on a separately running server.

`--server` remains the lower-level mode for callers that supply configuration and environment themselves.
It retains its stdin command protocol and shutdown on EOF. `--server --no-console` disables only that input
reader; `--headless` additionally supplies the saved launcher's environment and working directory.

## Building without desktop libraries

See [Building](/docs/building). `LONELYICE_GUI=OFF` removes the SDL3/RmlUi launcher and its graphical assets.
`LONELYICE_TUI=ON` includes the terminal interface; it is enabled by default. With GUI disabled, starting
LonelyIce without a mode opens the TUI. Both interfaces may be disabled for a CLI/server-only executable.
