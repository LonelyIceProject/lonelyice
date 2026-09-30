# The core fork

LonelyIce's server is a fork of AzerothCore: `LonelyIceProject/azerothcore-wotlk`, branch `main`. This page lists
what the fork changes compared with the code it is based on, where each change lives, and the rules for code and
SQL that has to run on it. Building it is covered in [Building the core](/guides/building-the-core), contributing in
[Contributing](/guides/contributing).

## What it is and why

The fork is based on [mod-playerbots/azerothcore-wotlk](https://github.com/mod-playerbots/azerothcore-wotlk),
branch `Playerbot`, which is AzerothCore with the core hooks mod-playerbots needs. The fork adds what LonelyIce
needs from a server core, written as general AzerothCore features:

- databases in SQLite files, so no database server has to be installed;
- server features as plugins, loaded at start without rebuilding the core;
- a build that another CMake project (the LonelyIce launcher) can include.

`authserver`, `worldserver` and `dbimport` build and run as usual. Nothing in the fork refers to LonelyIce; the
launcher only sets options the fork offers. The fork's README states the aim: changes that make sense for
AzerothCore itself are written to be proposed upstream. License: GPL-2.0-or-later, like AzerothCore.

## Branches and upstream

| Remote | Repository | Branch used |
|---|---|---|
| `origin` | `LonelyIceProject/azerothcore-wotlk` | `main`: the fork |
| `upstream` | `mod-playerbots/azerothcore-wotlk` | `Playerbot`: the base |

`main` is upstream's `Playerbot` with the fork's commits on top. The fork's GitHub repository also carries
upstream's `Playerbot` and `master` branches, unchanged. The LonelyIce launcher repository pins one commit of
`main` as its `external/azerothcore` submodule and moves that pointer when it needs a newer core (for example
together with an ABI change).

To see the fork's changes and what upstream has added since:

```sh
git remote add upstream https://github.com/mod-playerbots/azerothcore-wotlk.git
git fetch upstream
git log --oneline upstream/Playerbot..main    # the fork's commits
git log --oneline main..upstream/Playerbot    # upstream commits not in the fork yet
git diff $(git merge-base upstream/Playerbot main) main --stat
```

So far no upstream update has been taken into `main` since the fork was made; how that will be done (merge or
rebase) is not fixed yet.

The playerbots module has a fork of its own, `LonelyIceProject/mod-playerbots` (branch `main`, based on
upstream `master`). It keeps building as a classic module in `modules/mod-playerbots` and also builds as the plugin
`playerbots`; its README lists its changes.

## Change areas

| Area | Key places | Options and settings | More |
|---|---|---|---|
| Database layer, SQLite, backend registry | `src/server/database/Database`, `Database/Backends/SQLite`, `deps/sqlite` | `WITH_SQLITE`, `sqlite:` connection strings, `Database.SQLite.*` | [Storage](#storage-sqlite-and-the-backend-registry) |
| SQL translation | `src/server/database/Dialect`, `data/sql/overrides/sqlite`, `src/tools/sqlconv` | | [SQL rules](#sql-rules) |
| Plugin loader | `src/server/shared/Plugins`, `src/cmake/macros/AcorePlugin.cmake` | `AC_PLUGIN_ABI`, `AC_PLUGIN_SOURCE_DIRS`, `AC_PLUGINS_OUTPUT_DIR`, `PluginsDir` | [Plugin API](/docs/plugin-api) |
| Shared libraries on Windows | `src/cmake/macros/AcoreExport.cmake`, `AC_*_API` in headers | `WITH_DYNAMIC_LINKING` | [Builds](#shared-and-static-builds) |
| Subproject mode | top-level `CMakeLists.txt`, the `src/cmake` macros | `AC_SOURCE_DIR`, `AC_BINARY_DIR` | [Subproject](#subproject-mode) |
| Playerbots hooks | top-level `CMakeLists.txt` | `WITH_PLAYERBOTS_HOOKS` | [Hooks](#playerbots-hooks) |
| DBC data from the database | `src/server/game/DataStores/DBCStores.cpp` | `DBC.FromDatabase` | [Server game data](/docs/server-data) |
| Replaceable data file source | `src/server/game/DataStores/DataFileSource.h` | | [Game data](#game-data-sources) |
| Map extractor | `src/tools/map_extractor/System.cpp` | | [Game data](#game-data-sources) |

## Storage: SQLite and the backend registry

The core reaches every database through `IDbConnectionBackend` (`IDbConnectionBackend.h`), one object per physical
connection. `DatabaseConnection` (formerly `MySQLConnection`) sits above it, translates SQL through the dialect layer
and holds the prepared statements; query results are stored as `RowSet` cells, and `Field` reads them as before.

- **SQLite is built in** (`WITH_SQLITE`, default on; the amalgamation is vendored in `deps/sqlite`). A connection
  opens the file in WAL mode with foreign keys on; a missing file is reported as a missing database, and creating
  it (`Updates.AutoSetup`) creates its folder as well.
- **Other backends come from plugins.** The core has no MySQL code and no MySQL build dependency: `FindMySQL`,
  `deps/mysql`, `USE_MYSQL_SOURCES` and the `MySQLExecutable` option are gone, and the updater applies `.sql` files
  over the backend's own connection (`CreateScriptTarget`) instead of starting the `mysql` program. A plugin
  registers a driver with `RegisterBackendDriver(DatabaseBackend, DbBackendDriver)` in its `AC_PLUGIN_ON_LOAD`
  function. The MySQL driver lives in the `lonelyice.mysql` plugin (repository `mod-lonelyice-mysql`); see
  [Storage plugins](/guides/storage-plugins).
- **Backends are a fixed enum**: `MySQL`, `SQLite`, `PostgreSQL` (`DatabaseBackend.h`). A plugin can register or
  replace the driver of one of them, not add a new one. PostgreSQL has a connection string scheme but no driver and
  no translation yet.
- **SQLite extensions**: `AddSQLiteExtension(init)` (`Backends/SQLite/SQLiteExtensions.h`) runs `init` on every
  SQLite connection opened afterwards. LonelyIce uses it for its read-only `dbc_*` virtual tables
  ([Server game data](/docs/server-data)).
- **Compatibility**: `MySQLConnection.h` remains as aliases (`MySQLConnection = DatabaseConnection`,
  `MySQLConnectionInfo = DatabaseConnectionInfo`), so module code that includes it still compiles.

Connection strings (`LoginDatabaseInfo`, `WorldDatabaseInfo`, `CharacterDatabaseInfo`, and module databases such as
`PlayerbotsDatabaseInfo`), parsed in `DatabaseConnectionInfo.cpp`:

| Form | Backend |
|---|---|
| `sqlite:db/world.sqlite` | SQLite file; relative paths start at the server's working directory. `Updates.AutoSetup = 1` creates it. |
| `sqlite:db/playerbots.sqlite;attach=characters=db/characters.sqlite` | SQLite with another database attached under an alias; `;name=<name>` sets the logical name (default: the file stem). |
| `host;port_or_socket;user;password;database[;ssl]`, or with `mysql:` | MySQL; needs a plugin that registers the MySQL backend. |
| `pgsql:host;port;user;password;database[;ssl]` | PostgreSQL; no driver exists yet. |

The shipped `.conf.dist` files (worldserver, authserver, dbimport) default to SQLite files relative to the working
directory: `sqlite:db/auth.sqlite`, `sqlite:db/world.sqlite`, `sqlite:db/characters.sqlite`, created on the first
start with `Updates.AutoSetup = 1`; their comments show the MySQL form, which needs a backend plugin. authserver
and worldserver use the same auth file. mod-playerbots' `playerbots.conf.dist` defaults to
`sqlite:db/playerbots.sqlite;attach=characters=db/characters.sqlite` (the alias must be the characters database's
logical name, `characters` with the defaults).

New settings in `worldserver.conf` and `authserver.conf`:

| Setting | Default | Meaning |
|---|---|---|
| `Database.SQLite.BusyTimeoutMs` | `10000` | How long a connection waits for a lock held by another connection or process. |
| `Database.SQLite.Synchronous` | `NORMAL` | `PRAGMA synchronous` of the write-ahead log: `OFF`, `NORMAL`, `FULL`, `EXTRA`. |

SQLite databases always use one worker thread, and `Database.Reconnect.*` does not apply to them. Back up SQLite
files while the server is stopped (with their `-wal` and `-shm` files) or with `VACUUM INTO` while it runs.

`.server info` prints the database libraries in use (`Using database libraries: ...`) instead of the MySQL version.

## SQL rules

All SQL above the driver is written in AzerothCore's MySQL dialect: base files, update files, module and plugin SQL,
prepared statements and ad-hoc queries. The dialect layer (`src/server/database/Dialect`: lexer, statement
translator, schema model, DDL emitter, script runner) translates it for SQLite when it is prepared or applied; for
MySQL (and PostgreSQL) it passes through unchanged. Translation never changes the number or order of `?`
parameters. MySQL functions the core uses (`UNIX_TIMESTAMP`, `FROM_UNIXTIME`, `DATE_FORMAT`, `IF`, `FIND_IN_SET`,
`GREATEST`, `CONCAT_WS`, `SUBSTRING_INDEX`, ...) exist on SQLite as functions of the same name; the tested list is
`src/tools/sqlconv/tests/functions.txt`.

### SQL files

The updater applies `.sql` files statement by statement. It emulates some MySQL-only forms on SQLite (user variables
set with `SET @x = ...`, `CREATE TABLE ... LIKE`, `ALTER TABLE`, `DELETE t FROM t JOIN ...` on a table with a primary key,
`UPDATE t JOIN u ON ... SET t.x = ...`) and skips statements without meaning there (`LOCK TABLES`, `START TRANSACTION`, `COMMIT`,
`OPTIMIZE`, ...). These fail with `no translation, add an override file`:

| Not translated | Write instead |
|---|---|
| `DELIMITER`, `CALL`, `PREPARE`/`EXECUTE`, `CREATE VIEW`/`PROCEDURE`/`TRIGGER`, any `CREATE`/`DROP`/`ALTER` other than tables and indexes | Plain statements; logic in C++ |
| `USE`, `LOAD DATA`, `HANDLER` | Nothing: the updater picks the database from the folder |
| System variables (`@@...`), `INFORMATION_SCHEMA`, `:=` inside a statement | `CREATE TABLE IF NOT EXISTS`, `DROP TABLE IF EXISTS`; `SET @var = ...;` as its own statement |
| `UPDATE`/`DELETE ... LIMIT` | A `WHERE` on the key |
| `DELETE FROM a JOIN b ...`, `DELETE` from several tables, multi-table `DELETE`/`UPDATE` with `ORDER BY`/`LIMIT` | `DELETE FROM a WHERE key IN (SELECT ...)` |
| `UPDATE` with an outer join, `JOIN ... USING`, assigning a joined table's columns | `UPDATE a SET ... WHERE key IN (SELECT ...)` |
| `CREATE TABLE ... SELECT`, `CREATE TEMPORARY TABLE` | Create the table, then `INSERT ... SELECT` |
| Generated columns, functional index parts, `RENAME INDEX` | Plain columns and indexes |

For the core's own files and for modules in `modules/`, a file that cannot be translated gets a replacement of the
same name in `data/sql/overrides/<backend>/` (`sqlite`), in the source tree or in the module's folder; the updater
logs `Using override ... for ...`. Plugins do not use overrides: their SQL must translate as written
([Plugin format](/docs/plugin-format), section 5). The one exception is a plugin that updates a database of its own
through `ModuleDBUpdater` with its plugin folder as the source: playerbots does, and keeps
`data/sql/overrides/sqlite/2025_04_26_00.sql` for an upstream update that creates an index through
`INFORMATION_SCHEMA` and `PREPARE`. The auth, characters and world SQL of every plugin still has to translate as
written.

### Statements in code

The core's prepared statements were rewritten into forms that also suit PostgreSQL later. Follow the same forms in
new code and in plugins:

| Avoid | Use |
|---|---|
| `DELETE t FROM t JOIN u ...`, `UPDATE t, u SET ...` | `DELETE FROM t WHERE ... IN (SELECT ...)` or `NOT EXISTS (SELECT 1 ...)` |
| An integer column as a condition: `WHERE active` | `WHERE active <> 0` |
| String literals as aliases: `AS 'total'` | `AS total` |
| Non-aggregated columns missing from `GROUP BY` | List every selected non-aggregated column |

A core statement that the translator cannot handle gets a backend-specific text: the connection class implements
`DoPrepareStatementOverrides()` and calls `OverrideStatement(DatabaseBackend::SQLite, index, sql)`
(`CharacterDatabaseSQLite.cpp`, `LoginDatabaseSQLite.cpp`). Plugins cannot add statements to the core's pools; they
use ad-hoc queries or a `ModuleDatabasePool` of their own ([Shipping SQL](/guides/plugin-sql)).

Text comparison: columns with a case-insensitive MySQL collation become `COLLATE NOCASE` on SQLite, which folds
ASCII letters only; a `LIKE` on a binary operand becomes a case-sensitive `GLOB`.

### sqlconv

`sqlconv` (built with `TOOLS_BUILD=db-only` or `all`) checks SQL against the translator:

```sh
sqlconv translate update.sql                     # print the SQLite translation of a MySQL script
sqlconv load --db scratch.sqlite update.sql      # apply a script to an SQLite file
sqlconv lint --source <core source> [--modules <list>]   # build scratch databases from base, updates and modules
sqlconv lint --statements --source <core source>         # prepare every core and playerbots statement
sqlconv selftest                                 # the lexer, translator, DDL and ALTER checks
```

It also has the steps of a MySQL-to-SQLite data migration (`migrate`, `schema-diff`, `verify`); run `sqlconv help`
for the full list.

## Plugin loader and ABI

`PluginMgr` (`src/server/shared/Plugins`, library `shared`) loads plugin folders from `PluginsDir` at start, in
worldserver and, for plugins that list them in `server.apps`, in authserver and dbimport. It resolves dependencies
and conflicts, loads the libraries, registers their configs (`ConfigMgr::AddPluginConfig`) and SQL folders
(`UpdateFetcher::AddPluginDirectory`, state `MODULE`), runs `onLoad`, and worldserver registers their scripts after
the static modules' scripts and adds their ids to the enabled-modules list. `PluginApi.h` provides the entry
macros `AC_PLUGIN`, `AC_PLUGIN_ON_LOAD` and `AC_PLUGIN_ENTRY`; `AddPlugin()` (CMake) builds a plugin from a folder
with `plugin.json` and `CMakeLists.txt`.

`AC_PLUGIN_ABI` (default `azerothcore-dev`, LonelyIce releases `lonelyice-ac-2`) is compiled into `shared` and every
plugin; the loader refuses a library whose ABI or platform differs. The full reference is
[Plugin API](/docs/plugin-api); the manifest and packages are in [Plugin format](/docs/plugin-format).

## Shared and static builds

`-DWITH_DYNAMIC_LINKING=ON` (an AzerothCore option) sets `BUILD_SHARED_LIBS`. The fork makes this work on Windows:
`common`, `shared`, `database` and `game` are built as DLLs that export their API (`AcoreExportLibrary`: CMake's
generated export list, plus `AC_<NAME>_API` on data and static members that must be imported, for example in
`DBCStores.h`, `ObjectMgr.h`, `LootMgr.h`, `World.h`). Upstream built `game` as a static library in any case; in the
fork it follows `BUILD_SHARED_LIBS`. Collision's `WorldModelStore` is one instance across the libraries.

Plugin libraries need the shared build. Without it, `AddPlugin` builds the same plugin sources into the programs of
`server.apps` instead ([Built into the programs](/docs/plugin-api#built-into-the-programs)).

Targets outside the core that link against it receive what the core's headers depend on:

- C++20 (`cxx_std_20` on `acore-compile-option-interface`);
- the header-affecting definitions (`ACORE_API_USE_DYNAMIC_LINKING`, `NO_CORE_FUNCS`, `ACORE_DEBUG`,
  `WITHOUT_METRICS`, `ENABLE_VMAP_CHECKS`, ... through `AcoreAddDefinition` in `showoptions.cmake`);
- Boost's library folder, for MSVC auto-linking.

## Subproject mode

The build uses `AC_SOURCE_DIR` and `AC_BINARY_DIR` (the core's own folders) instead of `CMAKE_SOURCE_DIR` and
`CMAKE_BINARY_DIR`, so another project can include the core:

```cmake
add_subdirectory("${CORE_DIR}" azerothcore)
```

A parent project may set `CMAKE_RUNTIME_OUTPUT_DIRECTORY` first; the core keeps it (otherwise `<build>/bin`).
`revision.h` generation, config copying, modules, tools and tests all follow `AC_SOURCE_DIR`/`AC_BINARY_DIR`. The
LonelyIce launcher's top-level `CMakeLists.txt` is the working example.

## Playerbots hooks

The `Playerbot` branch compiles its hooks for mod-playerbots only when the module sits in `modules/` (the module
CMake defines `MOD_PLAYERBOTS`). `-DWITH_PLAYERBOTS_HOOKS=ON` defines `MOD_PLAYERBOTS` for the `database` library and
everything that links `game-interface`, so the hooks are compiled when playerbots is loaded as a plugin instead.
Default off; LonelyIce turns it on.

## Game data sources

- **DBC from the world database.** With `DBC.FromDatabase = 1` every DBC store reads its rows from a table
  `dbc_<file name in lower case>` of the world database instead of `<DataDir>/dbc`; the `*_dbc` override tables apply
  on top as before. `GetDBCFiles()` (`DBCStores.h`) lists every loaded file with its format and override table. The
  table layout is in [Server game data](/docs/server-data).
- **Data files through a replaceable source.** Terrain (`maps/*.map`) and cinematic cameras (`Cameras/*.m2`) are
  read through `DataFiles` (`DataFileSource.h`). The default reads `DataDir`; a module can call
  `DataFiles::SetSource()` before the world loads to serve them from elsewhere. LonelyIce builds tiles from the
  client's archives this way.
- **Map extractor.** `ConvertADT()` builds a `.map` image in memory, so the conversion can be reused without a file,
  and the liquid height map is reset for every tile (before, its last row and column kept the previous tile's
  values).

## Configuration and config policy

New options: `PluginsDir` (worldserver, authserver, dbimport; default `plugins`, relative to the working directory),
`DBC.FromDatabase`, `Database.SQLite.BusyTimeoutMs`, `Database.SQLite.Synchronous`. Removed: `MySQLExecutable`.
Every option can be overridden by an `AC_<KEY>` environment variable, as in AzerothCore
([Environment variables](/docs/environment)).

Module configs are read from the `modules` folder beside the main config file the program loaded (`-c`), on every
system; only when there is no such folder does the core fall back to `modules` in its default config directory
(`configs/` of the working directory on Windows, the build's `CONF_DIR` elsewhere; `ConfigMgr::GetModulesConfigPath()`).
Upstream always used the default directory, so a server started with `-c <dir>/worldserver.conf` ignored
`<dir>/modules`. A plugin's config is read from the `.conf.dist` in its folder first, then from `modules/<name>.conf`
in that folder, whose values win key by key. `reload config` reads them all again.

The configuration severity policy is AzerothCore's own (`doc/ConfigPolicy.md`, unchanged in the fork). It decides
how the loader reacts to config problems:

| Key | Applies to |
|---|---|
| `missing_file` | A missing or empty config file |
| `missing_option` | An option read in code but set in no file |
| `critical_option` | Required options (`RealmID`, `*DatabaseInfo`, ...) |
| `unknown_option` | Options in optional configs that nothing declares, e.g. keys in a plugin's `.conf` missing from its `.conf.dist` |
| `value_error` | Values that do not convert to the expected type |
| `default` | Every key not set explicitly |

Each key takes `skip`, `warn`, `error` or `fatal`. The policy comes from `--config-policy "<key>=<severity>,..."` on
the command line, else the `AC_CONFIG_POLICY` environment variable, else `AC_CONFIG_POLICY` in `conf/dist/config.sh`,
which also defines the presets `AC_CONFIG_POLICY_PRESET_DEFAULT`, `..._ZERO_CONF` and `..._STRICT`. Critical options
stay fatal by default. LonelyIce does not set a policy.

## Logging

Logging is AzerothCore's, unchanged (`doc/Logging.md`): `Logger.<name>=<level>,<appenders>` and
`Appender.<name>=<type>,<level>,<flags>[,...]` in the config. The fork's code logs to:

| Logger | What |
|---|---|
| `server.loading` | The plugin loader: `Plugin <id> <version> (<name>)` and why a plugin was not loaded |
| `sql.driver` | Connections, backends (`Database backend '<name>' registered`), SQLite connection options |
| `sql.updates` | The updater: applied files, overrides |
| `sql.sql` | Failed statements (errors), every statement with its time (debug) |

## Other fixes

| Commit subject | Effect |
|---|---|
| `database: unlock sync connection when callback throws` | A throwing callback on the synchronous connection no longer leaves it locked. |
| `dialect: bind empty blob as zero-length blob, not null` | Empty binary parameters stay empty on SQLite. |
| `fix(CMake): copy plugin files on every build` | A plugin's `plugin.json`, `data`, `sql`, `conf`, `lua` and `client` reach the output without relinking; removed files disappear. |
| `fix(Core/Config): load module configs beside the main config file` | Module and plugin configs come from `modules` beside the config given with `-c` ([Configuration](#configuration-and-config-policy)). |
| `fix(Core/Database): default to SQLite connection strings` | worldserver, authserver and dbimport default to `sqlite:db/*.sqlite`, in their `.conf.dist` and in code, so a plain build starts without a MySQL plugin. |
| `feat(Core/Plugins): read dependency version ranges like npm semver` | `VersionRange.{h,cpp}`: `depends` ranges as npm reads them; a range that cannot be read skips the plugin ([Plugin API](/docs/plugin-api#pluginmgr)). |

## The fork's own documents

| File | Content | On this site |
|---|---|---|
| `.github/README.md` | The fork's changes in short, above AzerothCore's README | This page |
| `doc/Plugins.md` | Plugins: folder layout, `plugin.json` core fields, entry macros, `AddPlugin`, load steps, databases | [Plugin API](/docs/plugin-api), [Plugin format](/docs/plugin-format) |
| `doc/ConfigPolicy.md` | Config severity policy (AzerothCore's) | [Configuration and config policy](#configuration-and-config-policy) |
| `doc/Logging.md` | Loggers and appenders (AzerothCore's) | [Logging](#logging) |
