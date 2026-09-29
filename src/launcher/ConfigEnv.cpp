#include "ConfigEnv.h"
#include <cctype>
#include <fstream>

namespace fs = std::filesystem;

// Same conversion as IniKeyToEnvVarKey in src/common/Configuration/Config.cpp
std::string LonelyIce::EnvName(std::string const& key)
{
    std::string r = "AC_";
    for (std::size_t i = 0; i < key.size(); ++i)
    {
        char c = key[i];
        if (c == ' ' || c == '.' || c == '-')
        {
            r += '_';
            continue;
        }
        char up = char(std::toupper(static_cast<unsigned char>(c)));
        if (i + 1 < key.size())
        {
            char n = key[i + 1];
            bool cNum = std::isdigit(static_cast<unsigned char>(c)) != 0;
            bool nNum = std::isdigit(static_cast<unsigned char>(n)) != 0;
            if ((!std::isupper(static_cast<unsigned char>(c)) && std::isupper(static_cast<unsigned char>(n))) || cNum != nNum)
            {
                r += up;
                r += '_';
                continue;
            }
        }
        r += up;
    }
    return r;
}

namespace
{
    std::string Trim(std::string s)
    {
        s.erase(0, s.find_first_not_of(" \t\r"));
        s.erase(s.find_last_not_of(" \t\r") + 1);
        return s;
    }
}

LonelyIce::EnvList LonelyIce::ModuleConfigOverrides(fs::path const& configFile, fs::path const& workDir)
{
    EnvList env;
    std::error_code ec;
    fs::path configDir = configFile.parent_path();
    if (fs::equivalent(configDir, workDir / "configs", ec))
        return env;

    for (fs::directory_iterator it(configDir / "modules", ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->path().extension() != ".conf")
            continue;

        std::ifstream in(it->path());
        std::string line;
        while (std::getline(in, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == '[')
                continue;
            std::size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = Trim(line.substr(0, eq));
            std::string value = Trim(line.substr(eq + 1));
            std::string clean;
            for (char c : value)
                if (c != '"')
                    clean += c;

            env.emplace_back(EnvName(key), clean);
        }
    }
    return env;
}

namespace
{
    constexpr std::pair<char const*, char const*> Databases[] = { { "LoginDatabaseInfo", "auth" }, { "CharacterDatabaseInfo", "characters" },
        { "WorldDatabaseInfo", "world" }, { "PlayerbotsDatabaseInfo", "playerbots" } };

    void Replace(std::string& s, std::string const& from, std::string const& to)
    {
        for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
            s.replace(p, from.size(), to);
    }
}

LonelyIce::EnvList LonelyIce::RemoteDatabaseOverrides(StorageProvider const& provider, RemoteDatabase const& db, fs::path const& binDir)
{
    EnvList env;
    std::string const port = db.port.empty() ? provider.port : db.port;
    // "<scheme>:host;port;user;password;database", the core's connection string
    for (auto [key, name] : Databases)
        env.emplace_back(EnvName(key), provider.id + ":" + db.host + ";" + port + ";" + db.user + ";" + db.password + ";" + db.prefix + name);
    for (auto const& [key, value] : provider.config)
    {
        std::string v = value;
        Replace(v, "{bin}", Platform::PathToUtf8(binDir));
#ifdef _WIN32
        Replace(v, "{exe}", ".exe");
#else
        Replace(v, "{exe}", "");
#endif
        env.emplace_back(EnvName(key), v);
    }
    return env;
}

LonelyIce::EnvList LonelyIce::LocalDatabaseOverrides(fs::path const& root)
{
    EnvList env;
    auto file = [&](char const* name) { return Platform::PathToUtf8(root / "db" / (std::string(name) + ".sqlite")); };
    for (auto [key, name] : Databases)
        env.emplace_back(EnvName(key), "sqlite:" + file(name) + (std::string_view(name) == "playerbots" ? ";attach=characters=" + file("characters") : ""));
    return env;
}