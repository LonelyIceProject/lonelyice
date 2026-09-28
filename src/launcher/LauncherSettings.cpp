#include "LauncherSettings.h"
#include "TextUtil.h"
#include <algorithm>
#include <Windows.h>

namespace
{
    std::wstring ReadW(std::filesystem::path const& file, wchar_t const* section, wchar_t const* key, wchar_t const* def)
    {
        wchar_t buf[2048];
        GetPrivateProfileStringW(section, key, def, buf, 2048, file.c_str());
        return buf;
    }

    std::string Read(std::filesystem::path const& file, wchar_t const* section, wchar_t const* key, std::string const& def)
    {
        return LonelyIce::WideToUtf8(ReadW(file, section, key, LonelyIce::Utf8ToWide(def).c_str()));
    }

    bool ReadBool(std::filesystem::path const& file, wchar_t const* section, wchar_t const* key, bool def)
    {
        return ReadW(file, section, key, def ? L"1" : L"0") == L"1";
    }

    void Write(std::filesystem::path const& file, wchar_t const* section, wchar_t const* key, std::wstring const& value)
    {
        WritePrivateProfileStringW(section, key, value.c_str(), file.c_str());
    }

    void Write(std::filesystem::path const& file, wchar_t const* section, wchar_t const* key, std::string const& value)
    {
        Write(file, section, key, LonelyIce::Utf8ToWide(value));
    }

    void Write(std::filesystem::path const& file, wchar_t const* section, wchar_t const* key, bool value)
    {
        Write(file, section, key, std::wstring(value ? L"1" : L"0"));
    }
}

void LonelyIce::LauncherSettings::Load()
{
    clientPath = ReadW(file, L"client", L"path", L"");
    locale = Read(file, L"client", L"locale", "");
    writeRealmlist = ReadBool(file, L"client", L"writeRealmlist", true);
    clearWdb = ReadBool(file, L"client", L"clearWdb", true);
    serverConfig = ReadW(file, L"server", L"config", L"");
    autoStart = ReadBool(file, L"launcher", L"autoStart", false);
    stopWithGame = ReadBool(file, L"launcher", L"stopWithGame", false);
    trayOnClose = ReadBool(file, L"launcher", L"trayOnClose", true);
    uiScale = _wtoi(ReadW(file, L"launcher", L"uiScale", L"0").c_str());
    uiScale = uiScale ? std::clamp(uiScale, 50, 300) : 0;
    backupTime = Read(file, L"backup", L"time", "04:00");
    backupKeep = std::max(1, _wtoi(ReadW(file, L"backup", L"keep", L"7").c_str()));
    lastBackupDay = Read(file, L"backup", L"lastDay", "");
    realmName = Read(file, L"server", L"realmName", "");
    pendingRealmName = Read(file, L"server", L"pendingRealmName", "");
}

void LonelyIce::LauncherSettings::Save() const
{
    Write(file, L"client", L"path", clientPath);
    Write(file, L"client", L"locale", locale);
    Write(file, L"client", L"writeRealmlist", writeRealmlist);
    Write(file, L"client", L"clearWdb", clearWdb);
    Write(file, L"server", L"config", serverConfig);
    Write(file, L"server", L"realmName", realmName);
    Write(file, L"server", L"pendingRealmName", pendingRealmName);
    Write(file, L"launcher", L"autoStart", autoStart);
    Write(file, L"launcher", L"stopWithGame", stopWithGame);
    Write(file, L"launcher", L"trayOnClose", trayOnClose);
    Write(file, L"launcher", L"uiScale", std::to_wstring(uiScale));
    Write(file, L"backup", L"time", backupTime);
    Write(file, L"backup", L"keep", std::to_wstring(backupKeep));
    Write(file, L"backup", L"lastDay", lastBackupDay);
}
