# Server game data

The server needs data from the game client: the DBC tables (spells, maps, areas, ...), terrain tiles
(`maps/*.map`), cinematic cameras, and optionally collision (`vmaps`) and paths (`mmaps`). Where it keeps its
databases and how it gets the game data are two settings in `server.yaml` (`server.location`, `dataCache`),
chosen in the wizard's "Data" step and in Settings → Storage.

## Location

- `local` (default): the databases are SQLite files in `<server folder>/db`.
- the `id` of a plugin's storage (`docs/plugin-format.md`, `storage`), e.g. `mysql` with mod-lonelyice-mysql: the
  databases are on a database server, reached with `remote` host, port, user, password and prefix. The launcher
  passes `<id>:host;port;user;password;<prefix><name>` for every `*DatabaseInfo` (auth, characters, world,
  playerbots) and the plugin's config values as environment overrides. The launcher's backups work on the built-in
  files only and are off there.

## Disk cache

Off (the default, only with `local`), nothing is unpacked. `LONELYICE_DATA=client` makes the server
(`src/storage/ClientData`):

- register an SQLite extension with one read-only virtual table per DBC file the core loads (`dbc_spell`,
  `dbc_map`, ...), served straight from the client's MPQ archives (`DbcTables`). The core runs with
  `DBC.FromDatabase = 1` and loads its DBC stores from these tables; the `*_dbc` override tables apply on top as
  always;
- serve the core's data files (`DataFiles`) itself: a terrain tile is built from the client's ADT file the first
  time its grid loads (the map extractor's conversion, a few ms) and kept in `<DataDir>/maps` with a stamp of
  the client archives, so later loads read it like an extracted file; cameras come from the archives.

The locale read is `LONELYICE_LOCALE` (the launch language, else `SET locale` in the client's `Config.wtf`).
When mmaps are generated, all tiles are built first (`LonelyIce --tool tiles`), since the generator reads them
all.

On, everything is unpacked once and the server no longer needs the client: maps and cameras into `<DataDir>`
(`LonelyIce --tool maps <client> <data> 5`), the DBC files into real `dbc_*` tables of the world database
(`LonelyIce --server --dbc fill`, through the core's database interfaces). The server runs with
`DBC.FromDatabase = 1`. Turning it off drops those tables and files (`--server --dbc drop`). A database server has
no virtual tables, so the cache is always on there.

A real `dbc_*` table takes precedence over the virtual one of the same name.

## Checking and switching

`LonelyIce --server --storage-check -c <any config>` opens the databases the environment names, with the plugins'
backends loaded, and reports `@@LI check db <auth|characters|world> ok|missing|empty|error <message>`,
`@@LI check dbc <unpacked> <total>` and `@@LI check done`. It needs no server folder: the launcher runs it with a
config of its own whenever a storage is picked (after the connection fields stop changing) and again on Apply.
When the chosen storage lacks something (databases, the cache, or holds a cache no longer wanted), the wizard
shows what it will do (and asks for the player's account when the databases are new) and does it; otherwise the
setting changes at once.

## Table layout

`dbc_<file name in lower case>`: one column per character of the core's format string for the file
(`DBCfmt.h`), named `f<position>`, except the index column (`n`, or `d`) which is `ID`; a file without an index
has `ID` on its first column. There is no primary key: an index may repeat (the core keeps the last row).
