#include "LauncherRuntime.h"
#include "ConfFile.h"
#include "ConfigEnv.h"
#include "GameClient.h"
#include "ProfileConfig.h"
#include "ProfilePlugins.h"
#include "Pak.h"
#include "Lang.h"
#include "PackageManager.h"
#include "Plugins.h"
#include "ServerLock.h"
#include <algorithm>
#include <fstream>
#include <atomic>
#include <map>
#include <thread>
#include <fkYAML/node.hpp>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace fs = std::filesystem;
using namespace LonelyIce;

std::vector<StorageProviderInfo> LonelyIce::EnabledStorageProviders(fs::path const& pluginsDir)
{
    std::vector<StorageProviderInfo> providers;
    for (PluginManifest const& plugin : ReadPlugins(pluginsDir))
    {
        if (!plugin.storage || !plugin.serverLibrary)
            continue;
        try
        {
            std::ifstream input(plugin.dir / "plugin.json", std::ios::binary);
            fkyaml::node manifest = fkyaml::node::deserialize(input);
            if (!manifest.contains("core") || !manifest["core"].is_mapping()
                || !manifest["core"].contains("abi") || !manifest["core"]["abi"].is_string()
                || manifest["core"]["abi"].get_value<std::string>() != Packages::Manager::CoreAbi())
                continue;
            if (manifest.contains("platforms"))
            {
                bool compatible = false;
                if (manifest["platforms"].is_sequence())
                    for (auto const& platform : manifest["platforms"].as_seq())
                        if (platform.is_string() && platform.get_value<std::string>() == Packages::Manager::Platform())
                            compatible = true;
                if (!compatible)
                    continue;
            }
            std::string const library = manifest["server"]["library"].get_value<std::string>();
#ifdef _WIN32
            std::string const filename = library + ".dll";
#elif defined(__APPLE__)
            std::string const filename = "lib" + library + ".dylib";
#else
            std::string const filename = "lib" + library + ".so";
#endif
            std::error_code ec;
            fs::path const binary = plugin.dir / "server" / Packages::Manager::Platform()
                / Platform::Utf8ToPath(filename);
            if (fs::is_regular_file(binary, ec))
                providers.push_back({ *plugin.storage, plugin.dir });
        }
        catch (std::exception const&)
        {
            // A damaged manifest cannot supply a database backend.
        }
    }
    return providers;
}

void LonelyIce::ResolveSettingsPaths(LauncherSettings& settings)
{
    if (settings.file.empty())
        settings.file = Platform::ExePath().parent_path() / "server.yaml";
    settings.file = fs::absolute(settings.file).lexically_normal();
    auto resolve = [&](fs::path& path)
    {
        if (!path.empty())
            path = (path.is_absolute() ? path : settings.file.parent_path() / path).lexically_normal();
    };
    resolve(settings.clientPath);
    resolve(settings.serverConfig);
    resolve(settings.dataRoot);
}

LaunchContext LonelyIce::CreateLaunchContext(LauncherSettings const& settings)
{
    LauncherSettings resolved = settings;
    ResolveSettingsPaths(resolved);
    LaunchContext context;
    context.exe = Platform::ExePath();
    context.exeDir = context.exe.parent_path();
    context.root = resolved.dataRoot.empty() ? context.exeDir : resolved.dataRoot;
    context.config = context.root / ".runtime" / "configs" / "worldserver.conf";
    context.plugins = context.exeDir / "plugins";
    return context;
}

namespace
{
    void SetYamlValues(ConfFile& config, std::optional<fkyaml::node> const& values, std::string const& group)
    {
        if (!values)
            return;
        if (!values->is_mapping())
            throw std::runtime_error("Expected a YAML settings mapping at " + group);
        for (auto const& [key, value] : values->as_map())
        {
            if (!key.is_string())
                throw std::runtime_error("Expected string setting names at " + group);
            std::string const name = key.get_value<std::string>();
            std::string const text = ProfileConfig::Scalar(value);
            if (name.empty() || name.find_first_of("\r\n=\0", 0, 4) != std::string::npos
                || text.find_first_of("\r\n\0", 0, 3) != std::string::npos)
                throw std::runtime_error("A core setting cannot contain line breaks or NUL characters: " + group);
            config.Set(name, text, value.is_string());
        }
    }

    void AppendYamlEnvironment(Platform::Env& env, std::optional<fkyaml::node> const& values)
    {
        if (!values)
            return;
        if (!values->is_mapping())
            throw std::runtime_error("Expected a YAML settings mapping.");
        for (auto const& [key, value] : values->as_map())
        {
            if (!key.is_string())
                throw std::runtime_error("Expected a string YAML setting name.");
            env.emplace_back(EnvName(key.get_value<std::string>()), ProfileConfig::Scalar(value));
        }
    }

    // Only directories freshly created by this generation are removed; reject symlink adapters.
    void RemoveGenerated(fs::path const& path, fs::path const& runtime)
    {
        if (path.empty() || path.parent_path() != runtime)
            return;
        std::error_code ignored;
        if (fs::is_symlink(path, ignored) || ignored)
            return;
        fs::remove_all(path, ignored);
    }
}

bool LonelyIce::GenerateRuntimeConfig(LauncherSettings const& settings, std::string& error)
{
    error.clear();
    LaunchContext context = CreateLaunchContext(settings);
    ProfileConfig profile;
    if (!profile.Load(settings.file.empty() ? context.exeDir / "server.yaml" : settings.file, error))
        return false;
    Packages::ServerLock lock(context.plugins);
    if (!lock.Held())
    {
        error = Tr("runtime.migrate_locked");
        return false;
    }
    fs::path runtime, staging, backup;
    try
    {
        fs::create_directories(context.root / ".runtime");
        runtime = fs::canonical(context.root / ".runtime");
        static std::atomic<uint64_t> serial = 0;
        std::string const suffix = std::to_string(Platform::TickMs()) + "-" + std::to_string(++serial);
        staging = runtime / ("configs.staging-" + suffix);
        backup = runtime / ("configs.backup-" + suffix);
        if (!fs::create_directory(staging))
            throw std::runtime_error("Cannot create a unique generated configuration directory.");
        bool extracted = true;
        bool const read = Pak::Read(context.exeDir / "setup" / "configs.pak",
            [&](std::string const& entry, std::string const& content)
            {
                fs::path relative = Platform::Utf8ToPath(entry).lexically_normal();
                std::string generated = Platform::PathToUtf8(relative);
                if (generated.ends_with(".dist"))
                    generated.resize(generated.size() - 5);
                relative = Platform::Utf8ToPath(generated);
                if (relative.empty() || relative.is_absolute() || relative.has_root_name())
                    extracted = false;
                for (auto const& part : relative)
                    if (part == "..")
                        extracted = false;
                if (!extracted)
                    return false;
                fs::path const destination = staging / relative;
                fs::create_directories(destination.parent_path());
                std::ofstream output(destination, std::ios::binary | std::ios::trunc);
                output.write(content.data(), std::streamsize(content.size()));
                output.close();
                extracted = bool(output);
                return extracted;
            }, error);
        if (!read || !extracted)
        {
            if (error.empty())
                error = Tr("runtime.config_write", Platform::PathToUtf8(context.config));
            RemoveGenerated(staging, runtime);
            return false;
        }
        ConfFile world;
        if (!world.Load(staging / "worldserver.conf"))
            throw std::runtime_error("The release configuration package has no worldserver.conf.");
        world.Set("EnablePlayerSettings", "1");
        world.Set("DataDir", "data", true);
        world.Set("LogsDir", "logs", true);
        world.Set("SourceDirectory", "sql", true);
        world.Set("Updates.EnableDatabases", "0");
        world.Set("BindIP", "127.0.0.1", true);
        unsigned const cores = std::max(1u, std::thread::hardware_concurrency());
        world.Set("MapUpdate.Threads", std::to_string(std::clamp(int(cores) - 4, 1, 8)));
        world.Set("LoginDatabaseInfo", "sqlite:db/auth.sqlite", true);
        world.Set("CharacterDatabaseInfo", "sqlite:db/characters.sqlite", true);
        world.Set("WorldDatabaseInfo", "sqlite:db/world.sqlite", true);
        if (auto dataDir = profile.Get({ "server", "settings", "DataDir" }))
        {
            fs::path data = Platform::Utf8ToPath(ProfileConfig::Scalar(*dataDir));
            data = (data.is_absolute() ? data : context.root / data).lexically_normal();
            if (data != (context.root / "data").lexically_normal())
                throw std::runtime_error(Tr("runtime.data_dir_managed"));
        }
        SetYamlValues(world, profile.Get({ "server", "settings" }), "server/settings");
        world.Set("DataDir", "data", true);
        if (!world.Save())
            throw std::runtime_error("Cannot save generated server configuration.");
        fs::create_directories(staging / "modules");
        for (PluginManifest const& plugin : ReadPlugins(context.plugins))
        {
            if (plugin.configDist.empty())
                continue;
            std::string const name = ConfigFileName(plugin);
            if (name.empty() || fs::path(name).filename() != fs::path(name))
                throw std::runtime_error("A plugin has an invalid configuration filename.");
            fs::path const destination = staging / "modules" / Platform::Utf8ToPath(name);
            fs::copy_file(plugin.configDist, destination, fs::copy_options::overwrite_existing);
            ConfFile module;
            if (!module.Load(destination))
                throw std::runtime_error("Cannot read a plugin's default configuration.");
            if (name == "playerbots.conf")
            {
                module.Set("PlayerbotsDatabaseInfo",
                    "sqlite:db/playerbots.sqlite;attach=characters=db/characters.sqlite", true);
                module.Set("Playerbots.Updates.EnableDatabases", "0");
                module.Set("AiPlayerbot.MinRandomBots", "100");
                module.Set("AiPlayerbot.MaxRandomBots", "100");
                for (char const* key : { "AiPlayerbot.CombatStrategies", "AiPlayerbot.NonCombatStrategies",
                    "AiPlayerbot.RandomBotCombatStrategies", "AiPlayerbot.RandomBotNonCombatStrategies" })
                    module.Set(key, "+tactics", true);
            }
            SetYamlValues(module, profile.Get({ "plugins", plugin.id, "settings" }),
                "plugins/" + plugin.id + "/settings");
            if (!module.Save())
                throw std::runtime_error("Cannot save generated plugin configuration.");
        }
        fs::path const active = runtime / "configs";
        if (fs::is_symlink(active))
            throw std::runtime_error("The private generated configs directory must not be a symbolic link.");
        bool const previous = fs::exists(active);
        if (previous)
            fs::rename(active, backup);
        try
        {
            fs::rename(staging, active);
        }
        catch (...)
        {
            if (previous)
                fs::rename(backup, active);
            throw;
        }
        RemoveGenerated(backup, runtime);
        return true;
    }
    catch (std::exception const& exception)
    {
        RemoveGenerated(staging, runtime);
        // Filesystem exception text can contain paths, but never generated configuration values or credentials.
        error = Tr("runtime.generate_failed", exception.what());
        return false;
    }
}

bool LonelyIce::MigrateServerConfig(LauncherSettings& settings, fs::path const&, std::string& error)
{
    // YAML has no legacy migration. Rebuild the private adapter from the current profile.
    return GenerateRuntimeConfig(settings, error);
}

bool LonelyIce::PrepareLaunch(LauncherSettings& settings, LaunchContext& context, std::string& error, bool validate)
{
    error.clear();
    try
    {
        ResolveSettingsPaths(settings);
        context = CreateLaunchContext(settings);
        ProfileConfig profile;
        if (!profile.Load(settings.file, error))
            return false;
        if (validate)
        {
            Packages::Manager packages(context.plugins);
            if (!ValidateInstalledProfilePlugins(profile, packages, error))
            {
                error += "\n" + Tr("runtime.plugins_sync_required", Platform::PathToUtf8(settings.file));
                return false;
            }
            if (!GenerateRuntimeConfig(settings, error))
                return false;
        }
        ConfFile config;
        bool const hasConfig = config.Load(context.config);
        fs::path client = GameClient::Detect(settings.clientPath, context.exeDir);
        bool const validClient = !client.empty() && GameClient::IsClientDir(client);
        if (validate && settings.ReadsClient() && !validClient)
        {
            error = Tr("runtime.client_required");
            return false;
        }
        context.env.clear();
        // The core's CONF reader strips quotes and whitespace. Environment overrides preserve YAML scalars exactly.
        AppendYamlEnvironment(context.env, profile.Get({ "server", "settings" }));
        for (PluginManifest const& plugin : ReadPlugins(context.plugins))
            AppendYamlEnvironment(context.env, profile.Get({ "plugins", plugin.id, "settings" }));
        Platform::Env managed = {
            { "AC_PLUGINS_DIR", Platform::PathToUtf8(context.plugins) },
            { EnvName("DataDir"), "data" },
            { "LONELYICE_CLIENT", validClient ? Platform::PathToUtf8(client) : "" },
            { "LONELYICE_LOCALE", !settings.locale.empty() ? settings.locale
                : validClient ? GameClient::ReadConfigLocale(client) : "" },
            { "LONELYICE_DATA", settings.ReadsClient() ? "client" : "" },
            { EnvName("DBC.FromDatabase"), settings.ReadsClient() ? "0" : "1" },
            { "AC_DISABLE_INTERACTIVE", "1" },
            { "LONELYICE_PENDING_REALM_NAME", settings.realmName },
            { "LONELYICE_SETTINGS_FILE", Platform::PathToUtf8(settings.file) }
        };
        context.env.insert(context.env.end(), managed.begin(), managed.end());
        if (settings.location != "local")
        {
            bool found = false;
            for (StorageProviderInfo const& plugin : EnabledStorageProviders(context.plugins))
                if (plugin.provider.id == settings.location)
                {
                    EnvList remote = RemoteDatabaseOverrides(plugin.provider, settings.remote,
                        plugin.dir / "server" / Packages::Manager::Platform());
                    context.env.insert(context.env.end(), remote.begin(), remote.end());
                    found = true;
                    break;
                }
            if (validate && !found)
            {
                error = Tr("runtime.storage_unavailable", settings.location);
                return false;
            }
        }
        else if (hasConfig)
        {
            ConfFile bots;
            bots.Load(context.config.parent_path() / "modules" / "playerbots.conf");
            for (char const* key : { "LoginDatabaseInfo", "CharacterDatabaseInfo", "WorldDatabaseInfo",
                "PlayerbotsDatabaseInfo" })
            {
                std::optional<std::string> value = config.Get(key);
                if (std::string_view(key) == "PlayerbotsDatabaseInfo")
                    if (std::optional<std::string> module = bots.Get(key))
                        value = module;
                if (value)
                    context.env.emplace_back(EnvName(key), *value);
            }
        }
        return true;
    }
    catch (std::exception const& exception)
    {
        error = exception.what();
        return false;
    }
}
