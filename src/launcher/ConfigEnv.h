#ifndef LONELYICE_CONFIGENV_H
#define LONELYICE_CONFIGENV_H

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace LonelyIce
{
    using EnvList = std::vector<std::pair<std::wstring, std::wstring>>;

    // The server reads module configs only from <working dir>\configs\modules. When the chosen
    // worldserver.conf lives elsewhere (e.g. configs-sqlite), its modules\*.conf are passed as AC_* overrides.
    EnvList ModuleConfigOverrides(std::filesystem::path const& configFile, std::filesystem::path const& workDir);
}

#endif
