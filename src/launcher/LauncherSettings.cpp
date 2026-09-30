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
    if (std::optional<std::string> old = ini.Get("server", "storage"))
    {
        // storage = client | unpacked | mysql, with [mysql] for the last (the first storage settings)
        location = *old == "mysql" ? "mysql" : "local";
        dataCache = *old != "client";
        remote = { ini.Get("mysql", "host", remote.host), ini.Get("mysql", "port", ""), ini.Get("mysql", "user", remote.user),
            ini.Get("mysql", "password", ""), ini.Get("mysql", "prefix", remote.prefix) };
    }
    else
    {
        location = ini.Get("server", "location", "local");
        dataCache = ReadBool(ini, "server", "dataCache", false);
        remote = { ini.Get("remote", "host", remote.host), ini.Get("remote", "port", ""), ini.Get("remote", "user", remote.user),
            ini.Get("remote", "password", ""), ini.Get("remote", "prefix", remote.prefix) };
    }
    if (location.empty())
        location = "local";
    if (location != "local")
        dataCache = true;
    pendingRealmName = ini.Get("server", "pendingRealmName", "");
    packageIndex = ini.Get("packages", "index", DefaultPackageIndex);
    packageIndexOff = ini.Get("packages", "disabled", "");
    packageLocale = ini.Get("packages", "locale", "");
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
    ini.Remove("server", "storage");
    ini.RemoveSection("mysql");
    ini.Set("server", "location", location);
    ini.Set("server", "dataCache", flag(dataCache));
    ini.Set("remote", "host", remote.host);
    ini.Set("remote", "port", remote.port);
    ini.Set("remote", "user", remote.user);
    ini.Set("remote", "password", remote.password);
    ini.Set("remote", "prefix", remote.prefix);
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
    ini.Set("packages", "locale", packageLocale);
    ini.Save(file);
}
