#ifndef LONELYICE_LAUNCHERRUNTIME_H
#define LONELYICE_LAUNCHERRUNTIME_H

#include "LauncherSettings.h"
#include "Platform.h"
#include "StoragePlan.h"

namespace LonelyIce
{
    struct LaunchContext
    {
        std::filesystem::path exe, exeDir, root, config, plugins;
        Platform::Env env;
    };

    // Paths in local.yaml are resolved against the server profile directory.
    void ResolveSettingsPaths(LauncherSettings& settings);
    LaunchContext CreateLaunchContext(LauncherSettings const& settings);
    // Enabled plugins with a matching ABI/platform and an installed server library.
    std::vector<StorageProviderInfo> EnabledStorageProviders(std::filesystem::path const& pluginsDir);
    // Only these private core adapter files use CONF; server.yaml/local.yaml remain the source of truth.
    bool GenerateRuntimeConfig(LauncherSettings const& settings, std::string& error);
    bool MigrateServerConfig(LauncherSettings& settings, std::filesystem::path const& config, std::string& error);
    // validate=false is for setup and inspection: no client requirement and no generated files are rewritten.
    bool PrepareLaunch(LauncherSettings& settings, LaunchContext& context, std::string& error, bool validate = true);
}

int HeadlessMain(int argc, char** argv);

#endif
