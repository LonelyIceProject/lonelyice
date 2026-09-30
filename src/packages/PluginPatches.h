#ifndef LONELYICE_PLUGINPATCHES_H
#define LONELYICE_PLUGINPATCHES_H

#include <filesystem>
#include <set>
#include <string>
#include <vector>

// Installs and removes the patches of plugins (docs/plugin-format.md, 7) on a server whose databases are open:
// gives out named ids (world table plugin_ids), writes the server's rows of patched DBC tables (spell_dbc, ...),
// runs the recipes' install SQL and keeps their uninstall SQL, and builds the client archives. Works through the
// core's database interfaces, so it does not depend on the database engine.
namespace LonelyIce::PluginPatches
{
    struct Options
    {
        std::filesystem::path pluginsDir;
        std::filesystem::path serverDbcDir;     // the server's extracted DBC files (DataDir/dbc)
        std::filesystem::path clientDir;        // empty: no client archives
        std::set<std::string> loaded;           // plugins with server code that loaded; others with server code wait
    };

    struct Result
    {
        bool ok = true;
        bool changed = false;
        std::vector<std::string> log;
        std::string error;
    };

    // Brings the databases (and the client) in line with the installed plugins: plugins that are gone are
    // uninstalled, new or changed recipes are (re)installed. A disabled plugin (pluginsDir/.disabled) keeps its
    // named ids for when it is enabled again; a removed one's ids are released. Call after the databases are
    // updated and before the world loads its DBC stores.
    Result Apply(Options const& options);
}

#endif
