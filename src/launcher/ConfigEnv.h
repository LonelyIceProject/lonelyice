#ifndef LONELYICE_CONFIGENV_H
#define LONELYICE_CONFIGENV_H

#include "LauncherSettings.h"
#include "Platform.h"
#include "Plugins.h"
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace LonelyIce
{
    using EnvList = Platform::Env;     // UTF-8 name, value

    // "Updates.EnableDatabases" -> "AC_UPDATES_ENABLE_DATABASES", as the server's config loader expects.
    std::string EnvName(std::string const& key);

    // Every database of the server (auth, characters, world, playerbots) as <prefix><name> on the provider's database
    // server, as *DatabaseInfo overrides, plus the config values the provider needs; binDir: its server/<platform> folder.
    EnvList RemoteDatabaseOverrides(StorageProvider const& provider, RemoteDatabase const& db, std::filesystem::path const& binDir);
    // The same for the SQLite files in <root>/db, with absolute paths.
    EnvList LocalDatabaseOverrides(std::filesystem::path const& root);
}

#endif
