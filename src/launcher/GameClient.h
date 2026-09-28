#ifndef LONELYICE_GAMECLIENT_H
#define LONELYICE_GAMECLIENT_H

#include <filesystem>
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
        // saved path → exe folder → its parent → two levels below the parent → Blizzard registry key.
        std::filesystem::path Detect(std::filesystem::path const& saved, std::filesystem::path const& exeDir);
        ClientInfo Inspect(std::filesystem::path const& dir);
        // Writes "set realmlist <host>" for the given locales (all when empty); the first overwrite keeps a realmlist.wtf.bak.
        bool WriteRealmlist(ClientInfo const& info, std::string const& host, std::vector<std::string> const& locales, std::string& error);
        std::string ReadConfigLocale(std::filesystem::path const& dir);   // SET locale from WTF\Config.wtf
        bool SetConfigLocale(std::filesystem::path const& dir, std::string const& locale);
        void ClearWdb(std::filesystem::path const& dir);
        // On success *process receives the game process handle (caller closes it).
        bool Launch(std::filesystem::path const& dir, std::string& error, void** process);
        bool IsRunning(std::filesystem::path const& dir);
    }
}

#endif
