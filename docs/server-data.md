# Server game data

The server needs data from the game client: the DBC tables (spells, maps, areas, ...), terrain tiles
(`maps/*.map`), cinematic cameras, and optionally collision (`vmaps`) and paths (`mmaps`). LonelyIce can take it
in two ways; `[server] storage` in `lonelyice.ini` says which, and Maintenance → Server data switches it.

## `client`: read from the client (default)

Nothing is unpacked. `LONELYICE_DATA=client` makes the server (`src/storage/ClientData`):

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

## `unpacked`

Everything is unpacked once and the server no longer needs the client: maps and cameras into `<DataDir>`
(`LonelyIce --tool maps <client> <data> 5`), the DBC files into real `dbc_*` tables of the world database
(`LonelyIce --server --dbc fill`, through the core's database interfaces). The server runs with
`DBC.FromDatabase = 1`. Going back to `client` drops those tables and files (`--server --dbc drop`).

A real `dbc_*` table takes precedence over the virtual one of the same name.

## Table layout

`dbc_<file name in lower case>`: one column per character of the core's format string for the file
(`DBCfmt.h`), named `f<position>`, except the index column (`n`, or `d`) which is `ID`; a file without an index
has `ID` on its first column. There is no primary key: an index may repeat (the core keeps the last row).
