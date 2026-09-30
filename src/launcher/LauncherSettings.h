#ifndef LONELYICE_LAUNCHERSETTINGS_H
#define LONELYICE_LAUNCHERSETTINGS_H

#include <cstdint>
#include <filesystem>
#include <string>

namespace LonelyIce
{
    // A database server holding the server's databases as <prefix>auth, <prefix>characters, <prefix>world, ...
    struct RemoteDatabase
    {
        std::string host = "127.0.0.1", port, user = "acore", password, prefix = "acore_";

        bool operator==(RemoteDatabase const&) const = default;
    };

    // lonelyice.ini next to the exe (UTF-8).
    struct LauncherSettings
    {
        std::filesystem::path file;

        std::filesystem::path clientPath;
        std::filesystem::path serverConfig;
        std::filesystem::path dataRoot;  // server folder made by the wizard (configs, db, data, logs, backups); empty = next to the exe
        std::string locale;              // client language to start, empty = keep Config.wtf
        std::string runner;              // Linux, macOS: the program that runs Wow.exe (wine); unused on Windows
        bool writeRealmlist = true;      // fix realmlist of the start locale before launching
        bool clearWdb = true;
        bool autoStart = false;
        bool stopWithGame = false;
        bool trayOnClose = true;
        std::string language;            // launcher language code, empty = English
        int uiScale = 0;                 // percent on top of the system display scale, 0 = largest that fits the screen
        std::string backupSchedule = "daily"; // off, daily (at backupTime), or hours between backups: 1, 3, 6, 12
        std::string backupTime = "04:00";
        int backupDays = 14;             // one backup per day for that many days, then one per week
        int backupBudgetMb = 2048;       // disk space the backups may take, 0 = no limit
        std::string lastBackupDay;       // YYYY-MM-DD of the last scheduled daily backup
        int64_t lastBackupAt = 0;        // time of the last scheduled backup (hourly schedules)
        std::string sqlStamp;            // setup/sql.pak the databases were last deployed from
        // Where the databases are: "local" (SQLite files in <dataRoot>/db) or the id of a plugin's storage (a database
        // server, see StorageProvider), reached with remote.
        std::string location = "local";
        // The client's game data unpacked (the DBC files into the world database, maps and cameras into data/):
        // faster, takes space. Off, the server reads it from the client's archives; always on with a remote location.
        bool dataCache = false;
        RemoteDatabase remote;
        std::string realmName;           // last known, for the settings tab while the server is down
        std::string pendingRealmName;    // applied when the server comes up
        std::string packageIndex = DefaultPackageIndex;   // plugin package indexes, ";"-separated URLs, files or folders
        std::string packageIndexOff;     // indexes kept in the list but not read, same format
        std::string packageLocale;       // catalog filter: only packages with texts in this language, empty: all

        static constexpr char const* DefaultPackageIndex = "https://lonelyice.com/packages/index.json";
        static constexpr char const* DefaultRunner = "wine";

        // The client's game data stays in its archives (local location without the cache).
        bool ReadsClient() const { return location == "local" && !dataCache; }

        void Load();
        void Save() const;
    };
}

#endif
