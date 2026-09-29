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
}

#endif
