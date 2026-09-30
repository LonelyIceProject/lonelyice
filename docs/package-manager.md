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

`depends` values, and `install <id>@<range>`, are ranges. They mean what they mean in npm's semver. The launcher's
resolver and the server's plugin loader use the same code (`Acore::VersionRange` in the core, which
`PluginMgr::Satisfies` calls):

- A range is a list of comparators separated by spaces and/or commas, and a version must satisfy every one of them:
  `>=1.0.0 <2.0.0`, `>=1.0.0,<2.0.0` and `>=1.0.0, <2.0.0` are the same range. An empty range, `*` or `x` matches
  everything.
- A comparator is an optional operator (`=`, `<`, `<=`, `>`, `>=`, `~`, `^`; `~>` is read as `~`) and a version. An
  operator followed by a space takes the next word as its version (`>= 1.2.0`).
- The version has one to three numeric parts (`1`, `1.2`, `1.2.3`), may start with `v`, and may end in `+build`
  metadata, which is ignored. `x`, `X` or `*` stands for a part and every part after it (`1.x`, `1.2.*`).
- The version being checked is read as before: the leading digits of its first three parts, missing parts 0
  ([Version comparison](#version-comparison)).

| Comparator | Matches |
|---|---|
| `*`, `x` | any version |
| `1.2.3`, `=1.2.3` | exactly 1.2.3 |
| `1.2`, `1.2.x`, `=1.2` | `>=1.2.0 <1.3.0` |
| `1`, `1.x` | `>=1.0.0 <2.0.0` |
| `>=1.2.0`, `>=1.2` | 1.2.0 and newer |
| `>1.2.0` | newer than 1.2.0 |
| `>1.2` | `>=1.3.0` (newer than every 1.2.x) |
| `<2.0.0`, `<2` | older than 2.0.0 |
| `<=1.2.0` | 1.2.0 and older |
| `<=1.2` | `<1.3.0` (every 1.2.x included) |
| `~1.2.3` | `>=1.2.3 <1.3.0` |
| `~1.2` | `>=1.2.0 <1.3.0` |
| `~1` | `>=1.0.0 <2.0.0` |
| `^1.2.3` | `>=1.2.3 <2.0.0` |
| `^1.2` | `>=1.2.0 <2.0.0` |
| `^0.2.3` | `>=0.2.3 <0.3.0` |
| `^0.0.3` | `>=0.0.3 <0.0.4` |
| `^0.2`, `^0.0` | `>=0.2.0 <0.3.0`, `>=0.0.0 <0.1.0` |
| `^1`, `^0` | `>=1.0.0 <2.0.0`, `>=0.0.0 <1.0.0` |
| `>*`, `<*` | nothing |

Anything else is an error, never a partial reading: alternatives (`||`), hyphen ranges (`1.0.0 - 2.0.0`),
pre-release versions (`1.2.3-beta`), other operators (`==`, `=>`, `!=`), a fourth part, a number after a wildcard
(`1.x.3`). The package manager then stops with `the version range "<range>" for <id> (<who>) cannot be read at
"<part>": …`: for a request before resolving; for a `depends` of a catalog entry or an installed plugin when
resolving fails on it (such an entry can never be chosen, so an older one that fits is taken instead); on `enable`;
and on `pack`. The server skips a plugin with such a range in `depends`
(`cannot read the version range …`).

## Resolving an install or an update

The resolver takes requests (id → range), an update flag, the installed plugins and the catalog entries. The
installed plugins are the folders in `plugins/` and `plugins/.disabled/` whose `plugin.json` has an `id`.

| Command | Requests | Update flag |
|---|---|---|
| `--pkg install <id>[@<range>]...` | each id with its range, default `*` | off |
| `--pkg update <id>...` | each id with `*` | on |
| `--pkg update`, **Update all** | every *enabled* installed plugin that has a newer version in any catalog, with `*` | on |
| **Install** on a catalog row | the id with `*` | off |
| **Update** on an installed row | the id with `*` | on |

The resolver searches for a set of choices, one per id it has to decide, that satisfies every range in play. It
works depth first and takes a choice back when it leads to a dead end (backtracking), so a version that turns out
not to fit is replaced together with the ranges it brought:

1. **Ranges in play** always come from the current choices only:
   - each requested id's range, recorded as "requested";
   - the `depends` of every *enabled* installed plugin that stays: not touched by the request, or kept in its
     installed version;
   - the `depends` of every chosen catalog package. An installed plugin that is replaced brings its new version's
     ranges, no longer the old ones.
2. **Ids to decide** are the requested ids and, repeatedly, the dependencies of what has been decided. They are
   decided in id order.
3. **Candidates** for an id, tried in this order, each only if it satisfies every range in play for that id:
   - without the update flag for that id: the installed version (kept), then the catalog versions from the newest
     down. A catalog version can be older than the installed one (`install x@1.0.0` downgrades, the step shows
     `from -> to`);
   - with the update flag (`update`, **Update**): the catalog versions newer than the installed one from the
     newest, then the installed version, then older catalog versions.
   Catalog entries with the installed version are the installed copy and are not offered again. When versions are
   equal, the entry read first comes first.
4. **Disabled plugins never satisfy anything.** The server does not load them. If an id to decide is a disabled
   plugin (requested or needed), that branch fails with `<id> is disabled: enable it first (needed by: …)`; run
   `enable` first (which checks the plugin's own dependencies, see below).
5. **Conflicts** are checked on a complete set of choices, in both directions, among what will be enabled
   afterwards (the enabled installed plugins and the chosen packages):
   - a chosen package that lists an enabled plugin or another chosen package in its `conflicts`:
     `<id> conflicts with <other>`;
   - an enabled installed plugin that stays and lists a chosen package in its `conflicts` (read from its
     `plugin.json`): `<id> cannot be installed: the installed plugin <other> conflicts with it`.
   A conflict is a dead end like any other, so another version is tried.
6. **Failure.** When no set of choices works, the error of the deepest dead end is shown, usually
   `no suitable version of <id>: needs <range> (<who>), …` or one of the above. The search stops after 20,000 steps
   with `the dependencies could not be resolved: too many version combinations`.
7. **Order.** Chosen catalog packages become steps with dependencies first. A plugin kept in its installed version
   gets no step. With no steps, the result is "nothing to install" (`--pkg`) or "all installed" (launcher).

Only ids in play are decided: an installed plugin that nothing in the request touches stays as it is, even if its
own dependencies are already broken. `update` without ids requests only enabled plugins that have a strictly newer
version (see [Version comparison](#version-comparison)); a rebuilt package with the same version is never offered.

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

An install runs in two phases. Nothing installed changes until every package of the plan has been downloaded,
checked and unpacked.

Before anything, the install is refused while a server runs on the plugins folder (see
[Enable, disable, remove](#enable-disable-remove)). `plugins/.staging` is emptied. Then, for each step in order,
dependencies first, the package manager:

1. Downloads `url` (http(s) or local, as for catalogs).
2. Checks `size` if the entry has a non-zero one: the download must have exactly that many bytes.
3. Checks `sha256` if the entry has one: the SHA-256 of the download in hex must equal it; upper and lower case are
   the same.
4. Unpacks the zip into `plugins/.staging/<id>`.
   - The zip must have `plugin.json` at its root, or in one top folder of any name. With a top folder, entries
     outside it are ignored.
   - An entry with an absolute path, a `:` or a `..` part stops the install with
     `invalid path in the package: <name>`.
   - A zip without `plugin.json` stops with `the package has no plugin.json`.
5. Checks that the unpacked `plugin.json` has the entry's `id` and `version`. Otherwise the install stops with
   `<id>: the package's plugin.json does not match the index`.

A failed check stops the install with `<id>: the package is damaged (size or sha256 mismatch)`. If the entry has
neither field, nothing is checked. Any failure in this phase deletes `plugins/.staging` and leaves the plugins as
they were.

## Installing

When every package is staged, the package manager replaces the folders, step by step:

1. Everything that is in the way is moved (renamed) into `plugins/.backup/<time>/`: the installed copy of the
   plugin, enabled or disabled, and whatever else is at `plugins/<id>`. If that fails:
   `<id>: could not move the old version aside, is the server running? (…)`.
2. The staged folder is moved to `plugins/<id>`. If that fails: `<id>: could not install (…)`.

Every move is recorded. When one fails, all moves made so far, of this step and the earlier ones, are undone in
reverse order: the new folders go back to `.staging`, the old ones back where they were, and the error ends with
"Nothing was changed: the previous versions are back in place." If a move cannot be undone, the error lists the
folders to move by hand (`<from> > <to>`), and `plugins/.backup/<time>` is kept. After the last step, the
backups and `plugins/.staging` are deleted and the installed and updated plugins are listed. Moves are renames
inside the plugins folder, so they do not copy anything; on Windows a folder with a loaded library cannot be
renamed, which makes a running server fail step 1 before anything changed.

The new version is always enabled (`plugins/<id>`). Files outside the plugin folder are never touched: the plugin's
`configs/modules/<name>.conf` stays through updates and removal.

## Enable, disable, remove

| Action | Effect | Refused when |
|---|---|---|
| `disable <id>` | Moves `plugins/<folder>` to `plugins/.disabled/<folder>`. The server does not look into `.disabled`. | An enabled plugin needs it, directly or through others. |
| `enable <id>` | Moves it back. | One of its dependencies is not installed, is disabled or is out of range; a range of its `depends` cannot be read; it conflicts with an enabled plugin, or an enabled plugin conflicts with it. |
| `remove <id>` | Deletes the plugin folder, enabled or disabled. | An enabled plugin needs it, directly or through others. |

A refusal names the dependents: `<id> is required by: …` (`--pkg`) or `<name> is needed by: ….` (launcher). A
refused `enable` names everything that is missing:
`<id> cannot be enabled, it needs: <dep> <range> (not installed), <dep> (disabled), <dep> <range> (installed: <version>)`,
or `<id> conflicts with <other>`. Enable the dependencies first, dependencies of dependencies before them.

**While the server runs** nothing changes. The server process (`LonelyIce --server`, also `--apply` and
`--deploy`) holds `plugins/.cache/server.lock` open and locked for as long as it runs; the operating system drops the
lock when the process ends, however it ends. While the lock is held:

- `--pkg install`, `update`, `remove`, `enable`, `disable` and `apply` stop at once with
  `the server is running on this plugins folder (<folder>): stop it first`;
- the package manager's install, remove, enable and disable refuse with the same message, so the launcher does too;
- the Plugins page refuses every change while its own server runs, before that
  ("Stop the server: plugins change while it is off.").

`list`, `available` and `pack` work while the server runs.

Removing or disabling a plugin does not undo its SQL updates: its tables and rows stay in the databases. Only its
patch recipes are undone on the next start (see below).

## On the next server start

The server (`LonelyIce --server`, started by the launcher) runs these steps in order:

1. **Load plugins.** The server reads `plugins/`, checks dependencies, conflicts and ABI, and loads the libraries
   ([Plugin API](/docs/plugin-api#loading)).
2. **Open the databases.**
3. **Plugin SQL.** The server hashes the core database connection strings (`LoginDatabaseInfo`,
   `CharacterDatabaseInfo`, `WorldDatabaseInfo`) and, for every `.sql` file in the `databases` folders (`auth`,
   `characters`, `world`) of every loaded plugin, the plugin id, the plugin version, the database, the file's
   relative path and the SHA-256 of its contents. It compares the hash with `plugins/.cache/sql.stamp`.
   - When they differ and `Updates.EnableDatabases` is off (the normal case after the wizard), the core's updater
     runs over the loaded plugins' folders only. When it is on, the updater has already applied them.
   - The new stamp is saved afterwards.
   - If the updater fails, the server does not start (`@@LI state failed database`).
   - An edited SQL file changes the stamp, so the updater runs; what it does with a file already recorded in the
     database's `updates` table under another hash follows the core's rules (`Updates.Redundancy`).
   - Databases a plugin owns (an object in `databases`, such as playerbots' own database) are not covered: the
     plugin opens and updates them itself on every start, with its own settings.
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

An install is all or nothing (see [Installing](#installing)).

| Failure | State afterwards |
|---|---|
| A catalog cannot be read | That catalog is skipped. |
| Resolution error (bad range, no suitable version, disabled dependency, conflict) | Nothing changed. |
| The server runs on the plugins folder | Nothing changed. |
| Download, size or sha256 check, unpacking or manifest check of any step | Nothing changed; `plugins/.staging` is deleted. |
| Moving an old folder aside or the staged folder in | Everything is moved back; nothing changed. |
| Moving back fails as well | The error lists the folders to move by hand; the old ones are in `plugins/.backup/<time>`. |
| The process dies while folders are moved | Some plugins may be new, the others old; the old folders of replaced ones stay in `plugins/.backup/<time>`. Install again. |
| Remove, enable or disable fails | The folder stays where it was, or partly deleted for a remove. |

After you fix the cause, run the same install or update again. On the Plugins page a failure sets the status to
failed and adds the error to the events. `--pkg` prints `error: <message>` and exits with code 1.
