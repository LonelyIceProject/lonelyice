# Backups

The launcher backs up the SQLite databases of the built-in storage (`[server] location = local`): `auth`,
`characters`, `world` and, with the playerbots plugin, `playerbots`. A backup keeps only what changed since the
previous one, so they can run every hour without filling the disk. They run from Maintenance → Backups, on a
schedule ([Settings](/docs/ini), `[backup]`) and from the command line ([Command line](/docs/cli), `--backup`).
With a plugin's database server there are no launcher backups.

## What changed

A backup finds the changes in steps, each cheaper than the next:

1. **Files.** The size and modification time of each database file and of its write-ahead log (`-wal`) are
   compared with those saved at the previous backup (`store/state`). A database whose files are unchanged is not
   read: the backup reuses its pages from the previous backup. Once a day every database is read anyway.
2. **Pages.** The other databases are read page by page (4 KiB pages as SQLite stores them); each page's hash is
   compared with the page at the same place in the previous backup. Only pages the store does not hold yet are
   written. When no database has a changed page, no backup is made (the schedule stays quiet; the button says so).
3. **Tables.** For a changed database the b-trees are walked from their root pages (`sqlite_schema`), so every
   changed page is attributed to its table (an index's pages to its table). The backup list shows them:
   `characters (character_inventory, item_instance, +2) · playerbots`.

So `world` costs space once (about 160 MB compressed), and later only when something in it changes: a GM command
(`.npc add`, `.gobject add`, `.tele add`), a plugin's SQL, a database update.

## Consistency while the server runs

A backup never stops or blocks the server. For each database it opens a read transaction, which pins a snapshot,
and checkpoints the write-ahead log. SQLite never copies pages newer than an open reader's snapshot into the
database file, so once the log is checkpointed up to the snapshot, the file holds exactly the snapshot until the
transaction ends, and the pages are read from the file. When a server keeps writing so that no checkpoint reaches
the snapshot within about two seconds, SQLite's backup interface copies the database into `store/tmp/` and that
copy is read instead. Databases are backed up one after the other; each is consistent in itself.

## The store

```
backups/store/
  packs/00000001.pack          pages, each compressed on its own (deflate), a table of their hashes at the end
  snapshots/<id>.snap          one backup: per database its file, page size and pages as runs of pack entries
  state                        file sizes and times at the newest backup
```

`<id>` is the local time of the backup, `YYYY-MM-DD_HHMMSS`. A pack holds the new pages of one backup; its table
(hash, offset, sizes of every entry) comes last, so a pack is complete once renamed from `.pack.tmp`. Pages are
compared by a 128-bit hash (MurmurHash3) and checked against it again when read back. A snapshot file is text:

```
lonelyice-snapshot 1
time 1790791000
reason manual
added 61440
db characters 4096 2649 14 characters.sqlite
table 9 characters
table 5 mail
run 1 0 1200
run 7 0 14
run 1 1214 1435
end
```

`db` gives the name, page size, page count, changed pages and file name; `run <pack> <first entry> <count>`
lists the pages in order. `reason` is `manual`, `scheduled` or `restore` (taken right before a restore).
The first backup stores every page; each later one mostly refers to the packs before it.

## Which backups stay

After each backup:

- the newest backup always stays, and every backup of the last 24 hours;
- for `[backup] days` days (default 14), the newest backup of each day;
- older, the newest backup of each week (weeks from Monday);
- when the store is larger than `[backup] budget` MB (default 2048; 0: no limit), the oldest backups go until the
  pages still referred to fit. When the newest backup alone is larger, only it stays and the launcher says so.

Pages nobody refers to any more are removed: a pack without such pages is deleted, a pack mostly unused, or many
small packs, are rewritten into one new pack and the snapshots are pointed at it. Every step leaves the store
usable when interrupted; leftovers (`*.tmp`, `store/tmp/`) are removed by the next backup.

Folders from earlier versions (`backups/<YYYY-MM-DD_HHMMSS>/` with full copies) are listed but never deleted by
the launcher.

## Restore and export

**Restore** (Maintenance → Backups → Restore, confirmed with a second click; `--backup restore <id>`) needs the
server stopped: it is refused while any process has a database open. It first backs up the current state
(`reason restore`, shown as "Before a restore"), then writes each database that differs from the backup as
`<file>.restore`, lets SQLite check it (`quick_check`), moves the current file aside, removes its `-wal` and
`-shm` and puts the restored file in place. Databases already equal to the backup are left alone.

**Export** writes the database files of a backup into `backups/export/<id>/` (and opens the folder), for copying
elsewhere or for opening a backup with any SQLite tool. Exports are not part of the store; delete them when done.
