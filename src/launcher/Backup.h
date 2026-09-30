#ifndef LONELYICE_BACKUP_H
#define LONELYICE_BACKUP_H

#include "BackupStore.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace LonelyIce
{
    struct DatabaseFile
    {
        std::string name;               // auth, characters, world, playerbots
        std::filesystem::path path;     // empty when the database is not a local file (MySQL)
    };

    // Reads *DatabaseInfo from worldserver.conf and modules/playerbots.conf.
    std::vector<DatabaseFile> FindDatabases(std::filesystem::path const& worldConf, std::filesystem::path const& workDir);

    struct BackupResult
    {
        bool ok = false;
        bool created = false;               // false with ok: nothing changed since the newest backup
        std::string id;                     // the backup made, else the newest one
        uint64_t added = 0;                 // bytes it added to the store
        std::vector<std::string> changed;   // databases that changed
        std::filesystem::path dir;          // export: the folder written
        std::string message;
    };

    // A backup of the database files into <root>/store (BackupStore.h), safe while the server runs. What changed is
    // found in steps: a database whose files have not changed since the newest backup is not read (it is read anyway
    // once a day); of the others only pages the store does not have yet are stored, and when no page differs no
    // backup is made. Then the retention removes old backups.
    BackupResult BackupDatabases(std::vector<DatabaseFile> const& dbs, std::filesystem::path const& root,
        std::string const& reason, Backup::Retention const& retention);

    // Backups, newest first, without their page lists.
    std::vector<Backup::SnapshotFile> ListBackups(std::filesystem::path const& root);
    // What changed in a backup: "characters (mail, item_instance, +2) · playerbots".
    std::string DescribeBackup(Backup::SnapshotFile const& s);
    uint64_t BackupStoreBytes(std::filesystem::path const& root);

    // Puts the databases of a backup back in place of dbs. Refuses while a database is open (the server runs);
    // the current state is backed up first (reason "restore").
    BackupResult RestoreBackup(std::vector<DatabaseFile> const& dbs, std::filesystem::path const& root, std::string const& id);

    // Writes the database files of a backup into <root>/export/<id>.
    BackupResult ExportBackup(std::filesystem::path const& root, std::string const& id);
}

#endif
