#ifndef LONELYICE_CONFIGENV_H
#define LONELYICE_CONFIGENV_H

#include "Platform.h"
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace LonelyIce
{
    using EnvList = Platform::Env;     // UTF-8 name, value

    // The server reads module configs only from <working dir>\configs\modules. When the chosen
    // worldserver.conf lives elsewhere (e.g. configs-sqlite), its modules\*.conf are passed as AC_* overrides.
    // "Updates.EnableDatabases" -> "AC_UPDATES_ENABLE_DATABASES", as the server's config loader expects.
    std::string EnvName(std::string const& key);

    EnvList ModuleConfigOverrides(std::filesystem::path const& configFile, std::filesystem::path const& workDir);

    struct MySqlServer
    {
        std::string host, port, user, password, prefix;
    };

    // Every database of the server (auth, characters, world, playerbots) on a MySQL server as <prefix><name>,
    // as *DatabaseInfo overrides; mysqlProgram is the mysql client the core applies sql files with.
    EnvList MySqlDatabaseOverrides(MySqlServer const& server, std::filesystem::path const& mysqlProgram);
}

#endif
