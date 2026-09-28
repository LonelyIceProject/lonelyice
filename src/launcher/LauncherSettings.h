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
        std::string locale;              // client language to start, empty = keep Config.wtf
        bool writeRealmlist = true;      // fix realmlist of the start locale before launching
        bool clearWdb = true;
        bool autoStart = false;
        bool stopWithGame = false;
        bool trayOnClose = true;
        int uiScale = 0;                 // percent on top of the Windows display scale, 0 = largest that fits the screen
        std::string backupTime = "04:00"; // empty = no scheduled backups
        int backupKeep = 7;
        std::string lastBackupDay;       // YYYY-MM-DD of the last scheduled backup
        std::string realmName;           // last known, for the settings tab while the server is down
        std::string pendingRealmName;    // applied when the server comes up

        void Load();
        void Save() const;
    };
}

#endif
