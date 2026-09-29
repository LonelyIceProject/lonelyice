#ifndef LONELYICE_GAMECLIENT_H
#define LONELYICE_GAMECLIENT_H

#include "Platform.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace LonelyIce
{
    struct ClientLocale
    {
        std::string name;       // ruRU, enGB, ...
        std::string realmlist;  // current value, empty if the file is missing
    };

    struct ClientInfo
    {
        std::filesystem::path dir;
        std::string version;    // "3.3.5.12340"
        std::vector<ClientLocale> locales;
        bool valid = false;
    };

    namespace GameClient
    {
        bool IsClientDir(std::filesystem::path const& dir);
        // A file or folder of the client by its Windows name, whatever its case on disk.
        std::filesystem::path Child(std::filesystem::path const& dir, std::string const& name);
        // saved path, exe folder, its parent, two levels below the parent, then the Blizzard registry key (Windows)
        // or the default Wine prefix.
        std::filesystem::path Detect(std::filesystem::path const& saved, std::filesystem::path const& exeDir);
        ClientInfo Inspect(std::filesystem::path const& dir);
        // Writes "set realmlist <host>" for the given locales (all when empty); the first overwrite keeps a realmlist.wtf.bak.
        bool WriteRealmlist(ClientInfo const& info, std::string const& host, std::vector<std::string> const& locales, std::string& error);
        std::string ReadConfigLocale(std::filesystem::path const& dir);   // SET locale from WTF\Config.wtf
        bool SetConfigLocale(std::filesystem::path const& dir, std::string const& locale);
        bool SetConfigValue(std::filesystem::path const& dir, std::string const& key, std::string const& value);  // SET key "value", added if missing
        void ClearWdb(std::filesystem::path const& dir);
        // Starts Wow.exe (through runner, e.g. wine, on Linux and macOS); process receives the started child.
        bool Launch(std::filesystem::path const& dir, std::string const& runner, std::string& error, std::unique_ptr<Platform::Child>& process);
        bool IsRunning(std::filesystem::path const& dir);
    }
}

#endif
