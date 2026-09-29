#ifndef LONELYICE_BACKUP_H
#define LONELYICE_BACKUP_H

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
        std::filesystem::path dir;
        uint64_t bytes = 0;
        std::string message;
    };

    // Consistent snapshot through the SQLite backup API; safe while the server is running.
    // Keeps the newest `keep` backup folders under root. The world database is skipped: it is rebuilt from SQL.
    BackupResult BackupDatabases(std::vector<DatabaseFile> const& dbs, std::filesystem::path const& root, int keep);
}

#endif
