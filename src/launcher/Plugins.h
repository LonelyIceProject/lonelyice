#ifndef LONELYICE_PLUGINS_H
#define LONELYICE_PLUGINS_H

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace LonelyIce
{
    // One field of a plugin's settings.json (docs/plugin-format.md, 6).
    struct PluginSetting
    {
        std::string key, type;                  // type: bool, int, float, string, choice
        std::string apply = "restart";          // now, reload, restart
        std::string label, hint;
        std::vector<std::pair<std::string, std::string>> options;   // value, label
        std::optional<double> min, max;
        std::optional<std::string> def;         // "default"; else the value in the .conf.dist
    };

    // What the launcher reads from a plugin's plugin.json (the server loads the plugins itself).
    struct PluginManifest
    {
        std::string id, version, name;              // name in the launcher's language (ru, else en)
        std::string description;
        std::vector<std::pair<std::string, std::string>> depends;  // id, version range
        std::filesystem::path dir;
        std::filesystem::path configDist;           // empty: no config
        std::filesystem::path settings;             // settings.json, empty when the plugin has none
        std::vector<std::filesystem::path> addons;  // client addon folders
        std::filesystem::path patches;              // patch recipes (DBC rows, named ids, SQL), empty: none
        bool serverLibrary = false;                 // has server code (must be loaded to count as enabled)
        std::vector<std::string> provides;          // capabilities, e.g. "database:mysql" (a database backend)
    };

    // A plugin's settings group; empty fields when it has none or the file is broken (error set).
    struct PluginSettings
    {
        std::string group, hint;
        std::vector<PluginSetting> fields;
        std::string error;
    };

    std::vector<PluginManifest> ReadPlugins(std::filesystem::path const& pluginsDir);
    // Load order: dependencies first, by id otherwise (missing dependencies are ignored here).
    std::vector<PluginManifest const*> OrderByDependencies(std::vector<PluginManifest> const& plugins);
    PluginSettings ReadPluginSettings(PluginManifest const& plugin);

    // configs/modules/<name>.conf for a plugin's .conf.dist
    std::string ConfigFileName(PluginManifest const& plugin);
}

#endif
