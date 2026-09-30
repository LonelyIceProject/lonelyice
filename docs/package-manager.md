# How the package manager works

The package manager is part of `LonelyIce.exe`. The launcher's Plugins page and `LonelyIce --pkg`
([Command line](/docs/cli#package-manager)) run the same code. It reads catalogs, resolves versions and
dependencies, and installs, updates, enables, disables and removes plugin folders. It only changes files in the
plugins folder. The databases and the game client catch up on the next server start (or `--pkg apply`). The
package and index formats are in [Plugin format](/docs/plugin-format#8-packages).

## Catalogs

A catalog is one `index.json`. The catalogs in use are `[packages] index` in `lonelyice.ini` ([lonelyice.ini](/docs/ini)),
and `[packages] disabled` holds the ones kept but not read. Both lists are separated by `;`, and spaces and tabs
around each entry are trimmed. The default is `https://lonelyice.com/packages/index.json`. An old
`https://raw.githubusercontent.com/LonelyIceProject/packages/main/index.json` entry is replaced with it when the
settings are read. `--pkg --index <catalogs>` replaces the list for one command.

| Location | Read as |
|---|---|
| `http://…`, `https://…` | Downloaded with libcurl. |
| `file://…` | A local file (`file:///C:/dir/index.json` on Windows). |
| A path to a file | That file. |
| A path to a folder | `<folder>/index.json`. |

Downloads accept only `http` and `https`, including after redirects, and follow up to 10 redirects. Certificate
checks are on. The connect timeout is 15 seconds, and a transfer is aborted when less than 1 byte per second
arrives for 60 seconds. An HTTP status of 400 or higher is an error.

When you add a catalog on the Plugins page, the launcher refuses:

- a location containing `;`;
- a local path that does not exist;
- a folder without `index.json`;
- a location already in the list.

**Reading a catalog.** The document must have a `packages` array. Without one the catalog fails with
"not a package index". `name` (localized) is shown in the catalog list. `format` is not checked. An entry is kept
only when all of these hold:

1. `id`, `version` and `url` are present.
2. If the entry has `platforms`: its `core` equals this build's ABI and its `platforms` include this system (see
   [Platforms and core ABI](#platforms-and-core-abi)). An entry without `platforms` is kept whatever its `core` is.

`url`, `icon` and `page` are resolved against the index location. Values that start with `http://`, `https://` or
`file://`, and absolute paths, are used as they are. Anything else is appended to the index location, cut after its
last `/` or `\`.

**Several catalogs.** Catalogs are read in list order. The entries of all readable catalogs go into one list, in
catalog order and then entry order, with nothing merged or removed. The same id can therefore appear several times,
from one catalog or several:

- The Plugins page shows the newest version of each id. When versions are equal, the entry read first wins. With
  more than one catalog, each row names its catalog.
- The resolver takes the newest version that fits. When versions are equal, the entry read first wins, so list
  order only breaks ties. A later catalog with a higher version still wins. You cannot tie a package to a catalog.
- Dependencies are resolved from all catalogs together.

**Failures.** A catalog that cannot be read is skipped. `--pkg` prints `skipped index <location>: <error>`, and the
Plugins page marks it in the catalog list and the events. Loading as a whole fails only when every catalog fails
(`error: package index: …`). An empty list loads nothing and is not an error.

**When catalogs are read.** The launcher reads them when it starts or when the Plugins page first opens, on
**Check for updates**, and after the catalog list changes. `--pkg available`, `install` and `update` read them on
every run. `list`, `remove`, `enable`, `disable`, `apply` and `pack` do not read them.

## Version comparison

The launcher (`Manager::CompareVersions`) compares versions like this:

1. It splits the version at `.` and reads each part's leading digits as a number. A part without leading digits
   counts as 0.
2. It compares the first three parts numerically. Missing parts count as 0, and parts after the third are ignored.

| Comparison | Result |
|---|---|
| `1.10.0` vs `1.9.0` | `1.10.0` is newer |
| `1.2` vs `1.2.0` | equal |
| `1.2.3-beta` vs `1.2.3` | equal (`3-beta` reads as 3) |
| `1.2.3.4` vs `1.2.3` | equal |
| `v1.2.0` vs `0.2.0` | equal (`v1` reads as 0) |

The launcher accepts any version string. The LonelyIce catalog accepts only `x.y.z` with digits
([What the catalog checks](/guides/package-review)).

## Version ranges

`depends` values, and `install <id>@<range>`, are ranges. The launcher's resolver and the server's plugin loader use
the same function (`PluginMgr::Satisfies` in the core):

- A range is split at whitespace into terms, and a version must satisfy every term. An empty range or `*` matches
  everything.
- Each term is an optional operator followed by a version. The operator is the leading run of the characters
  `<`, `>`, `=`, `^`, `~`.
- In the term's version, a part that is `x`, `*` or empty is a wildcard, and missing parts are wildcards too.
  Comparisons with `<`, `>`, `^` and `~` treat a wildcard as 0.

| Term | Matches |
|---|---|
| `*` | any version |
| `1.2.3` (or `=1.2.3`) | exactly 1.2.3 |
| `1.2`, `1.2.x`, `1.2.*` | 1.2.anything |
| `1`, `1.x` | 1.anything |
| `>=1.2.0`, `>=1.2` | 1.2.0 and newer |
| `>1.2.0`, `>1.2` | newer than 1.2.0 (1.2.1 matches) |
| `<2.0.0`, `<2` | older than 2.0.0 |
| `<=1.2.0`, `<=1.2` | 1.2.0 and older (1.2.1 does not match) |
| `^1.2.3` | at least 1.2.3, major 1 |
| `^1.2` | at least 1.2.0, major 1 |
| `^0.3.1` | at least 0.3.1, major 0 and minor 3 |
| `^0.0.3` | at least 0.0.3, major 0 and minor 0 (0.0.9 matches) |
| `~1.2.3` | at least 1.2.3, major 1 and minor 2 |
| `~1.2` | 1.2.anything |
| `~1` | 1.0.anything (not 1.x) |
| `>=1.2.0 <2.0.0` | both terms: 1.2.0 up to, not including, 2.0.0 |

Not supported:

| Syntax | What happens |
|---|---|
| `||` | Read as a term that never matches. |
| Hyphen ranges (`1.0.0 - 2.0.0`) | The `-` is read as a term that never matches. |
| Commas (`>=1.0.0,<2.0.0`) | Read as a single term. Everything after the comma is lost, so this means `>=1.0.0`. |
| Operators other than the six above (`=`, `==`, `=>`, `~>`) | Treated like no operator: exact or wildcard match. |

Separate terms with spaces.

## Resolving an install or an update

The resolver takes requests (id → range), an update flag, the installed plugins and the catalog entries. The
installed plugins are the folders in `plugins/` and `plugins/.disabled/` whose `plugin.json` has an `id`.

| Command | Requests | Update flag |
|---|---|---|
| `--pkg install <id>[@<range>]...` | each id with its range, default `*` | off |
| `--pkg update <id>...` | each id with `*` | on |
| `--pkg update`, **Update all** | every installed plugin that has a newer version in any catalog, with `*` | on |
| **Install** on a catalog row | the id with `*` | off |
| **Update** on an installed row | the id with `*` | on |

1. **Constraints.** Each requested id gets its range, recorded as "requested". Each *enabled* installed plugin that
   is not requested adds its own `depends` ranges, so an install or update never leaves an enabled plugin out of
   range.
2. **Choice.** The resolver repeats over the wanted ids until nothing changes, for at most 32 rounds:
   - An installed version is kept when it satisfies every range collected for its id, unless the id is requested
     with the update flag on.
   - Otherwise the resolver takes the newest catalog version that satisfies every collected range. That can be
     older than the installed one (`install x@1.0.0` downgrades), in which case the step shows `from -> to`. The
     chosen version's `depends` are added as ranges, and those ids become wanted.
   - When no version fits, resolving stops with `no suitable version of <id>: needs <range> (<who>), …`.
3. **Conflicts.** For each chosen package, every id in its `conflicts` is checked against the installed plugins
   (enabled or disabled) and the other chosen packages. A match stops with `<id> conflicts with <other>`. Only the
   chosen package's own list is checked, because the launcher does not read `conflicts` of installed plugins. The
   server checks both sides when it starts ([Plugin API](/docs/plugin-api#loading)).
4. **Order.** Chosen packages become steps with dependencies first. A package already installed in the chosen
   version gets no step. With no steps, the result is "nothing to install" (`--pkg`) or "all installed" (launcher).

The resolver has these limits:

- **Disabled plugins count as installed.** A disabled plugin that fits a range satisfies the resolver, but the
  server does not load disabled plugins, so a plugin that needs it is skipped at start. `install` of a disabled
  plugin that fits does nothing. Use `enable` instead.
- **No backtracking.** If the resolver picks a version and later replaces it, the ranges that version added still
  apply for the rest of the run.
- **Update detection.** `update` without ids requests only plugins that have a strictly newer version (see
  [Version comparison](#version-comparison)). A rebuilt package with the same version is never offered.

## Platforms and core ABI

| Value | Where it comes from |
|---|---|
| This build's ABI | `AC_PLUGIN_ABI` of the core LonelyIce was built with; `lonelyice-ac-2` for current releases ([Plugin API](/docs/plugin-api#core-build)). |
| This build's platform | `AC_PLUGIN_PLATFORM`: `windows-x64`, `linux-x64`, `linux-arm64`, `macos-x64`, `macos-arm64` ([Plugin API](/docs/plugin-api#entry-points)). |

Entries that fail the catalog filter ([Catalogs](#catalogs)) are neither shown nor chosen. A dependency that exists
only for another ABI or platform ends in "no suitable version". The package manager does not check installed
plugins against the ABI. The server does that when it loads them: it compares `core.abi` with its own ABI, and the
library's exported ABI and platform with its own. A plugin that fails is not loaded, and neither are the plugins that
depend on it ([Plugin API](/docs/plugin-api#loading)).

## Languages

Index entries carry the `locales` of the manifest ([Plugin format](/docs/plugin-format#locales)). A package matches a
language when its `locales` list that code or `*`.

| Where | Filter |
|---|---|
| Plugins page, **Catalog** view | **Language** (saved as `[packages] locale`). Empty: all packages. |
| `--pkg available --locale <code>` | Only matching packages. |

A package without `locales` matches no language, so a language filter hides it. The filter does not apply to the
**Installed** and **Updates** views or to dependency resolution. Names and descriptions from the index are shown in
the launcher's language, then `en`, then any language.

## Download and verification

The steps run in order, dependencies first. For each step, the package manager:

1. Downloads `url` (http(s) or local, as for catalogs).
2. Checks `size` if the entry has a non-zero one: the download must have exactly that many bytes.
3. Checks `sha256` if the entry has one: the SHA-256 of the download, in lowercase hex, must equal it. An uppercase
   value never matches.

A failed check stops the install with `<id>: the package is damaged (size or sha256 mismatch)`. If the entry has
neither field, nothing is checked.

## Unpacking and installing

For each downloaded step:

1. `plugins/.staging/<id>` is emptied, and the zip is unpacked into it.
   - The zip must have `plugin.json` at its root, or in one top folder of any name. With a top folder, entries
     outside it are ignored.
   - An entry with an absolute path, a `:` or a `..` part stops the install with
     `invalid path in the package: <name>`.
   - A zip without `plugin.json` stops with `the package has no plugin.json`.
2. The unpacked `plugin.json` must have the entry's `id` and `version`. Otherwise the install stops with
   `<id>: the package's plugin.json does not match the index`.
3. The installed copy of the plugin, enabled or disabled, is deleted. If that fails:
   `<id>: could not remove the old version, is the server running? (…)`.
4. The staged folder is moved to `plugins/<id>`. An update of a disabled plugin is therefore enabled.

After the last step, `plugins/.staging` is deleted. Files outside the plugin folder are never touched: the plugin's
`configs/modules/<name>.conf` stays through updates and removal.

## Enable, disable, remove

| Action | Effect | Refused when |
|---|---|---|
| `disable <id>` | Moves `plugins/<folder>` to `plugins/.disabled/<folder>`. The server does not look into `.disabled`. | An enabled plugin needs it, directly or through others. |
| `enable <id>` | Moves it back. | Never. The plugin's own dependencies are not checked. |
| `remove <id>` | Deletes the plugin folder, enabled or disabled. | An enabled plugin needs it, directly or through others. |

A refusal names the dependents: `<id> is required by: …` (`--pkg`) or `<name> is needed by: ….` (launcher).

The Plugins page refuses every change while the server runs ("Stop the server: plugins change while it is off.").
`--pkg` does not check whether the server runs. On Windows, a loaded plugin library is locked, and the change then
fails with "…, is the server running?".

Removing or disabling a plugin does not undo its SQL updates: its tables and rows stay in the databases. Only its
patch recipes are undone on the next start (see below).

## On the next server start

The server (`LonelyIce --server`, started by the launcher) runs these steps in order:

1. **Load plugins.** The server reads `plugins/`, checks dependencies, conflicts and ABI, and loads the libraries
   ([Plugin API](/docs/plugin-api#loading)).
2. **Open the databases.**
3. **Plugin SQL.** The server hashes the database connection strings and, for every `.sql` file in the `databases`
   folders of every loaded plugin, the plugin id, the plugin version, the database, the file's relative path and
   its size. It compares the hash with `plugins/.cache/sql.stamp`.
   - When they differ and `Updates.EnableDatabases` is off (the normal case after the wizard), the core's updater
     runs over the loaded plugins' folders only. When it is on, the updater has already applied them.
   - The new stamp is saved afterwards.
   - If the updater fails, the server does not start (`@@LI state failed database`).
   - The stamp does not cover file contents. An SQL file edited without a new plugin version or a new size is not
     applied again.
4. **Patch recipes** ([Plugin format](/docs/plugin-format), section 7).
   - Each plugin's stamp in the world table `plugin_patches` covers the recipe text, the plugin version and the
     installer version. A plugin whose stamp changed is uninstalled and installed again with the same named ids.
   - Plugins no longer in `plugins/` (removed or disabled) have their `uninstall` SQL run and their named ids
     released.
   - A plugin with a server library that did not load is left out.
   - A failure is logged as `Plugin patches failed: …`, and the server still starts. `--apply` and `--deploy` report
     it as failed.
5. **Client archive.** When the launcher knows the game folder, it passes it to the server (`LONELYICE_CLIENT`).
   For every client locale, the server rebuilds `Data/<locale>/patch-<locale>-4.MPQ` when its stamp changed. The
   stamp covers the recipes, the plugin versions, the patch files, the named ids and the sizes of the source
   archives. The archive is removed when no recipe remains.

Client addons (`client.addons`) are not handled at server start. The launcher copies them into `Interface/AddOns`
when it starts the game, and removes the addons of plugins that are gone.

`--pkg apply -c <worldserver.conf> [--client <game folder>]` runs steps 1 to 5 without starting the world
([Command line](/docs/cli#package-manager)).

## Icon cache

After the catalogs load, the launcher downloads the `icon` of every catalog entry to
`plugins/.cache/icons/<id>-<version>.png`. `--pkg` does not download icons.

| Rule | Effect |
|---|---|
| A file with that name exists | It is not downloaded again. |
| The download fails or is not a PNG | Nothing is saved. There is no error. |
| Old icons | Never deleted or refreshed. A new icon needs a new version. |

Installed plugins show `icon.png` from their own folder.

## Failures and recovery

An install has no transaction. Each step is complete on its own, and earlier steps are not rolled back.

| Failure | State afterwards |
|---|---|
| A catalog cannot be read | That catalog is skipped. |
| Resolution error (no suitable version, conflict) | Nothing changed. |
| Download, size or sha256 check, unpacking or manifest check of a step | The steps before it are installed. This plugin's installed copy is unchanged. `plugins/.staging/<id>` may be left behind and is emptied on the next install. |
| Deleting the old copy fails | The old copy may be partly deleted. Stop the server and install again. |
| Moving the staged folder fails | The old copy is already gone. Install again. |
| Remove, enable or disable fails | The folder stays where it was, or partly deleted for a remove. |

After you fix the cause, run the same install or update again. Steps that were already done have no step the second
time, because the installed version already matches. On the Plugins page a failure sets the status to failed and
adds the error to the events. `--pkg` prints `error: <message>` and exits with code 1.
