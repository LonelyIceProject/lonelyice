#include "LauncherSettings.h"
#include "IniFile.h"
#include "Platform.h"
#include <algorithm>
#include <cstdlib>

namespace
{
    bool ReadBool(LonelyIce::IniFile const& ini, char const* section, char const* key, bool def)
    {
        return ini.Get(section, key, def ? "1" : "0") == "1";
    }

    int ReadInt(LonelyIce::IniFile const& ini, char const* section, char const* key, int def)
    {
        return std::atoi(ini.Get(section, key, std::to_string(def)).c_str());
    }

    std::filesystem::path ReadPath(LonelyIce::IniFile const& ini, char const* section, char const* key)
    {
        return LonelyIce::Platform::Utf8ToPath(ini.Get(section, key, ""));
    }
}

void LonelyIce::LauncherSettings::Load()
{
    IniFile ini;
    ini.Load(file);
    clientPath = ReadPath(ini, "client", "path");
    locale = ini.Get("client", "locale", "");
    runner = ini.Get("client", "runner", DefaultRunner);
    writeRealmlist = ReadBool(ini, "client", "writeRealmlist", true);
    clearWdb = ReadBool(ini, "client", "clearWdb", true);
    serverConfig = ReadPath(ini, "server", "config");
    dataRoot = ReadPath(ini, "server", "root");
    autoStart = ReadBool(ini, "launcher", "autoStart", false);
    stopWithGame = ReadBool(ini, "launcher", "stopWithGame", false);
    trayOnClose = ReadBool(ini, "launcher", "trayOnClose", true);
    language = ini.Get("launcher", "language", "");
    uiScale = ReadInt(ini, "launcher", "uiScale", 0);
    uiScale = uiScale ? std::clamp(uiScale, 50, 300) : 0;
    backupTime = ini.Get("backup", "time", "04:00");
    backupKeep = std::max(1, ReadInt(ini, "backup", "keep", 7));
    lastBackupDay = ini.Get("backup", "lastDay", "");
    realmName = ini.Get("server", "realmName", "");
    sqlStamp = ini.Get("server", "sqlStamp", "");
    pendingRealmName = ini.Get("server", "pendingRealmName", "");
    packageIndex = ini.Get("packages", "index", DefaultPackageIndex);
    packageIndexOff = ini.Get("packages", "disabled", "");
}

void LonelyIce::LauncherSettings::Save() const
{
    IniFile ini;
    ini.Load(file);     // keeps what this version does not know
    auto flag = [](bool b) { return std::string(b ? "1" : "0"); };
    ini.Set("client", "path", Platform::PathToUtf8(clientPath));
    ini.Set("client", "locale", locale);
    ini.Set("client", "runner", runner);
    ini.Set("client", "writeRealmlist", flag(writeRealmlist));
    ini.Set("client", "clearWdb", flag(clearWdb));
    ini.Set("server", "config", Platform::PathToUtf8(serverConfig));
    ini.Set("server", "root", Platform::PathToUtf8(dataRoot));
    ini.Set("server", "realmName", realmName);
    ini.Set("server", "sqlStamp", sqlStamp);
    ini.Set("server", "pendingRealmName", pendingRealmName);
    ini.Set("launcher", "autoStart", flag(autoStart));
    ini.Set("launcher", "stopWithGame", flag(stopWithGame));
    ini.Set("launcher", "trayOnClose", flag(trayOnClose));
    ini.Set("launcher", "language", language);
    ini.Set("launcher", "uiScale", std::to_string(uiScale));
    ini.Set("backup", "time", backupTime);
    ini.Set("backup", "keep", std::to_string(backupKeep));
    ini.Set("backup", "lastDay", lastBackupDay);
    ini.Set("packages", "index", packageIndex);
    ini.Set("packages", "disabled", packageIndexOff);
    ini.Save(file);
}
