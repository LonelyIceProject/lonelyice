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

    // A place for the server's databases that a plugin adds (plugin.json "storage", docs/plugin-format.md): a
    // database server reached with host, port, user and password, the databases named <prefix><auth|world|...>.
    struct StorageProvider
    {
        std::string id;                         // the core's connection string scheme: "<id>:host;port;user;password;database"
        std::string name;                       // in the launcher's language
        std::string port;                       // default port
        // config values the server needs with it ({bin}: the plugin's server/<platform> folder, {exe}: ".exe" on Windows)
        std::vector<std::pair<std::string, std::string>> config;
    };

    // What the launcher reads from a plugin's plugin.json (the server loads the plugins itself).
    struct PluginManifest
    {
        std::string id, version, name;              // name in the launcher's language (ru, else en)
        std::string description;
        std::string homepage;
        std::vector<std::string> locales;           // languages of its texts (PluginLocales), "*": has none; empty: not stated
        std::vector<std::pair<std::string, std::string>> depends;  // id, version range
        std::filesystem::path dir;
        std::filesystem::path configDist;           // empty: no config
        std::filesystem::path settings;             // settings.json, empty when the plugin has none
        std::vector<std::filesystem::path> addons;  // client addon folders
        std::filesystem::path patches;              // patch recipes (DBC rows, named ids, SQL), empty: none
        bool serverLibrary = false;                 // has server code (must be loaded to count as enabled)
        std::vector<std::string> provides;          // capabilities, e.g. "database:mysql" (a database backend)
        std::optional<StorageProvider> storage;     // a place for the databases, offered in the wizard and the settings
    };

    // A plugin's settings group; empty fields when it has none or the file is broken (error set).
    struct PluginSettings
    {
        std::string group, hint;
        std::vector<PluginSetting> fields;
        std::string error;
    };

    // Languages a plugin can state in "locales" (docs/plugin-format.md, 2): one per language of the game's locales.
    std::vector<std::string> const& PluginLocales();
    // Whether a plugin with these "locales" is usable in a language: it lists it or has no texts ("*").
    bool HasLocale(std::vector<std::string> const& locales, std::string const& locale);

    std::vector<PluginManifest> ReadPlugins(std::filesystem::path const& pluginsDir);
    // Load order: dependencies first, by id otherwise (missing dependencies are ignored here).
    std::vector<PluginManifest const*> OrderByDependencies(std::vector<PluginManifest> const& plugins);
    PluginSettings ReadPluginSettings(PluginManifest const& plugin);

    // configs/modules/<name>.conf for a plugin's .conf.dist
    std::string ConfigFileName(PluginManifest const& plugin);
}

#endif
