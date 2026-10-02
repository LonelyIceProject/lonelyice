#include "LauncherSettings.h"
#include "ProfileConfig.h"
#include "Platform.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace LonelyIce;

namespace
{
    int ReadInt(ProfileConfig const& profile, char const* section, char const* key, int fallback)
    {
        int64_t value = profile.Integer({ section, key }, fallback);
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
            throw std::runtime_error(std::string("YAML integer is outside the supported range at ")
                + section + '/' + key);
        return int(value);
    }
}

bool LauncherSettings::Load(std::string* error)
{
    ProfileConfig profile;
    std::filesystem::path const path = file.empty() ? Platform::ExePath().parent_path() / "server.yaml" : file;
    std::string message;
    if (!profile.Load(path, message))
    {
        loadError = message;
        if (error)
            *error = message;
        return false;
    }
    try
    {
        if (profile.Integer({ "format" }, 1) != 1)
            throw std::runtime_error("Unsupported YAML profile format. Use format: 1.");
        LauncherSettings loaded;
        loaded.file = profile.ProfilePath();
        loaded.clientPath = Platform::Utf8ToPath(profile.String({ "client", "path" }));
        loaded.locale = profile.String({ "client", "locale" });
        loaded.runner = profile.String({ "client", "runner" }, DefaultRunner);
        loaded.writeRealmlist = profile.Boolean({ "client", "writeRealmlist" }, true);
        loaded.clearWdb = profile.Boolean({ "client", "clearWdb" }, true);
        loaded.dataRoot = Platform::Utf8ToPath(profile.String({ "server", "root" }));
        loaded.autoStart = profile.Boolean({ "launcher", "autoStart" });
        loaded.stopWithGame = profile.Boolean({ "launcher", "stopWithGame" });
        loaded.trayOnClose = profile.Boolean({ "launcher", "trayOnClose" }, true);
        loaded.language = profile.String({ "launcher", "language" });
        loaded.uiScale = ReadInt(profile, "launcher", "uiScale", 0);
        loaded.uiScale = loaded.uiScale ? std::clamp(loaded.uiScale, 50, 300) : 0;
        loaded.backupTime = profile.String({ "backup", "time" }, "04:00");
        loaded.backupSchedule = profile.String({ "backup", "schedule" }, "daily");
        loaded.backupDays = std::max(1, ReadInt(profile, "backup", "days", 14));
        loaded.backupBudgetMb = std::max(0, ReadInt(profile, "backup", "budget", 2048));
        loaded.lastBackupDay = profile.String({ "backup", "lastDay" });
        loaded.lastBackupAt = profile.Integer({ "backup", "lastAt" });
        loaded.realmName = profile.String({ "server", "realmName" }, "LonelyIce");
        loaded.sqlStamp = profile.String({ "runtime", "sqlStamp" });
        loaded.configVersion = ReadInt(profile, "runtime", "configVersion", 0);
        loaded.location = profile.String({ "server", "location" }, "local");
        loaded.dataCache = profile.Boolean({ "server", "dataCache" });
        if (loaded.location.empty())
            throw std::runtime_error("server/location must name local or a database storage provider.");
        if (loaded.location != "local")
            loaded.dataCache = true;
        loaded.remote.host = profile.String({ "remote", "host" }, loaded.remote.host);
        if (auto port = profile.Get({ "remote", "port" }))
        {
            if (!port->is_string() && !port->is_integer())
                throw std::runtime_error("Expected a string or integer at remote/port.");
            loaded.remote.port = ProfileConfig::Scalar(*port);
        }
        loaded.remote.user = profile.String({ "remote", "user" }, loaded.remote.user);
        loaded.remote.password = profile.String({ "remote", "password" });
        loaded.remote.prefix = profile.String({ "remote", "prefix" }, loaded.remote.prefix);
        loaded.pendingRealmName = profile.String({ "runtime", "pendingRealmName" });
        loaded.packageIndex = profile.String({ "packages", "index" }, DefaultPackageIndex);
        loaded.packageIndexOff = profile.String({ "packages", "disabled" });
        loaded.packageLocale = profile.String({ "packages", "locale" });
        *this = std::move(loaded);
        if (error)
            error->clear();
        return true;
    }
    catch (std::exception const& exception)
    {
        loadError = "Invalid configuration in " + Platform::PathToUtf8(profile.ProfilePath())
            + " or local.yaml: " + exception.what();
        if (error)
            *error = loadError;
        return false;
    }
}

void LauncherSettings::ApplyToProfile(ProfileConfig& profile) const
{
    using Layer = ProfileConfig::Layer;
    profile.Set({ "format" }, int64_t(1));
    auto portable = [&](char const* section, char const* key, fkyaml::node value)
    {
        profile.SetEffective({ section, key }, std::move(value));
    };
    auto local = [&](char const* section, char const* key, fkyaml::node value)
    {
        profile.Set({ section, key }, std::move(value), Layer::Local);
        profile.Remove({ section, key }, Layer::Profile);
    };
    local("client", "path", Platform::PathToUtf8(clientPath));
    local("client", "runner", runner);
    local("server", "root", Platform::PathToUtf8(dataRoot));
    local("runtime", "sqlStamp", sqlStamp);
    local("runtime", "configVersion", configVersion);
    local("runtime", "pendingRealmName", pendingRealmName);
    local("backup", "lastDay", lastBackupDay);
    local("backup", "lastAt", lastBackupAt);
    local("remote", "host", remote.host);
    local("remote", "port", remote.port);
    local("remote", "user", remote.user);
    local("remote", "password", remote.password);
    local("remote", "prefix", remote.prefix);
    portable("client", "locale", locale);
    portable("client", "writeRealmlist", writeRealmlist);
    portable("client", "clearWdb", clearWdb);
    portable("server", "realmName", realmName);
    portable("server", "location", location);
    portable("server", "dataCache", dataCache);
    portable("launcher", "autoStart", autoStart);
    portable("launcher", "stopWithGame", stopWithGame);
    portable("launcher", "trayOnClose", trayOnClose);
    portable("launcher", "language", language);
    portable("launcher", "uiScale", uiScale);
    portable("backup", "schedule", backupSchedule);
    portable("backup", "time", backupTime);
    portable("backup", "days", backupDays);
    portable("backup", "budget", backupBudgetMb);
    portable("packages", "index", packageIndex);
    portable("packages", "disabled", packageIndexOff);
    portable("packages", "locale", packageLocale);
}

bool LauncherSettings::Save(std::string* error) const
{
    ProfileConfig profile;
    std::string message;
    std::filesystem::path const path = file.empty() ? Platform::ExePath().parent_path() / "server.yaml" : file;
    if (profile.Load(path, message))
    {
        try
        {
            ApplyToProfile(profile);
            if (profile.Save(message))
            {
                if (error)
                    error->clear();
                return true;
            }
        }
        catch (std::exception const& exception)
        {
            message = exception.what();
        }
    }
    if (error)
        *error = message;
    return false;
}
