#ifndef LONELYICE_STORAGEPLAN_H
#define LONELYICE_STORAGEPLAN_H

#include "LauncherSettings.h"
#include "Plugins.h"
#include "StorageCheck.h"
#include <filesystem>

namespace LonelyIce
{
    // Where the server keeps its data: the databases' location and the game data cache (LauncherSettings).
    struct StorageChoice
    {
        std::string location = "local";
        bool cache = false;
        RemoteDatabase remote;

        bool Remote() const { return location != "local"; }
        bool operator==(StorageChoice const&) const = default;
        // The same storage for the server (a connection typed for another location does not count).
        bool Same(StorageChoice const& o) const { return location == o.location && cache == o.cache && (!Remote() || remote == o.remote); }
    };

    // A plugin's storage with the folder it is installed in.
    struct StorageProviderInfo
    {
        StorageProvider provider;
        std::filesystem::path dir;
    };

    // Install steps that make a storage hold what the server needs.
    struct StoragePlan
    {
        bool db = false;        // create the databases (or bring existing ones up to date)
        bool newDatabases = false;  // db creates them: the player's account goes in
        bool unpack = false;    // fill the game data cache
        bool pack = false;      // drop a cache that is no longer wanted

        bool Empty() const { return !db && !unpack && !pack; }
    };

    // Maps and cameras extracted into dataDir (not terrain tiles built from the client, which carry a stamp).
    bool HasUnpackedFiles(std::filesystem::path const& dataDir);
    // sqlChanged: setup/sql.pak differs from the one the databases were last deployed from, so existing databases
    // are brought up to date too.
    StoragePlan PlanStorage(StorageState const& state, bool cache, std::filesystem::path const& dataDir, bool sqlChanged);

}

#endif
