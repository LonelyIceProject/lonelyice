#ifndef LONELYICE_PLUGINS_H
#define LONELYICE_PLUGINS_H

#include <filesystem>
#include <string>
#include <vector>

namespace LonelyIce
{
    // What the launcher reads from a plugin's plugin.json (the server loads the plugins itself).
    struct PluginManifest
    {
        std::string id, version, name;              // name in the launcher's language (ru, else en)
        std::filesystem::path dir;
        std::filesystem::path configDist;           // empty: no config
        std::filesystem::path settings;             // settings.json, empty when the plugin has none
        std::vector<std::filesystem::path> addons;  // client addon folders
    };

    std::vector<PluginManifest> ReadPlugins(std::filesystem::path const& pluginsDir);

    // configs/modules/<name>.conf for a plugin's .conf.dist
    std::string ConfigFileName(PluginManifest const& plugin);
}

#endif
