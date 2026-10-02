#ifndef LONELYICE_PROFILEPLUGINS_H
#define LONELYICE_PROFILEPLUGINS_H

#include "PackageManager.h"
#include "ProfileConfig.h"
#include <functional>

namespace LonelyIce
{
    struct ProfilePluginChoice
    {
        std::string version;
        bool enabled = true;
    };

    struct ProfilePluginPlan
    {
        Packages::Plan install;
        // Unknown folders are retained; enabled plugins absent from the profile are explicitly disabled.
        std::vector<std::string> disable, enable, disableInstalled, retained;
        std::map<std::string, ProfilePluginChoice> desired;
        std::string installedStamp, profileStamp, error;
        bool Empty() const { return install.steps.empty() && disable.empty() && enable.empty() && disableInstalled.empty(); }
    };

    ProfilePluginPlan PreviewProfilePlugins(ProfileConfig const& profile, Packages::Manager const& manager,
        std::string const& activeStorage = "local");
    std::string DescribeProfilePlugins(ProfilePluginPlan const& plan);
    // Runtime validation is offline: it never fetches packages or changes folders/settings.
    bool ValidateInstalledProfilePlugins(ProfileConfig const& profile, Packages::Manager const& manager, std::string& error);
    bool ApplyProfilePlugins(ProfileConfig const& profile, Packages::Manager& manager, ProfilePluginPlan const& plan,
        std::string& error, std::function<void(std::string const&)> const& log = {},
        Http::Progress const& progress = {}, std::function<bool()> const& cancelled = {});

    // Call only after successful explicit package changes. An affected list updates just those IDs, keeping an
    // imported desired list intact. Existing settings and unknown per-plugin keys are preserved.
    void SnapshotProfilePlugins(ProfileConfig& profile, Packages::Manager const& manager,
        std::optional<std::vector<std::string>> const& affected = std::nullopt);
}
#endif
