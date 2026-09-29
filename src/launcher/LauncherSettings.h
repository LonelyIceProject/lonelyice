#ifndef LONELYICE_LAUNCHERSETTINGS_H
#define LONELYICE_LAUNCHERSETTINGS_H

#include <filesystem>
#include <string>

namespace LonelyIce
{
    // lonelyice.ini next to the exe.
    struct LauncherSettings
    {
        std::filesystem::path file;

        std::wstring clientPath;
        std::wstring serverConfig;
        std::wstring dataRoot;           // server folder made by the wizard (configs, db, data, logs, backups); empty = next to the exe
        std::string locale;              // client language to start, empty = keep Config.wtf
        bool writeRealmlist = true;      // fix realmlist of the start locale before launching
        bool clearWdb = true;
        bool autoStart = false;
        bool stopWithGame = false;
        bool trayOnClose = true;
        std::string language;            // launcher language code, empty = English
        int uiScale = 0;                // percent on top of the Windows display scale, 0 = largest that fits the screen
        std::string backupTime = "04:00"; // empty = no scheduled backups
        int backupKeep = 7;
        std::string lastBackupDay;       // YYYY-MM-DD of the last scheduled backup
        std::string sqlStamp;            // setup\sql.pak the databases were last deployed from
        std::string realmName;           // last known, for the settings tab while the server is down
        std::string pendingRealmName;    // applied when the server comes up
        std::string packageIndex = DefaultPackageIndex;   // plugin package indexes, ";"-separated URLs, files or folders
        std::string packageIndexOff;     // indexes kept in the list but not read, same format

        static constexpr char const* DefaultPackageIndex = "https://raw.githubusercontent.com/LonelyIceProject/packages/main/index.json";

        void Load();
        void Save() const;
    };
}

#endif
