#include "ProfilePlugins.h"
#include "Lang.h"
#include "Platform.h"
#include "VersionRange.h"
#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

using namespace LonelyIce;
namespace fs = std::filesystem;

namespace
{
    std::map<std::string, Packages::Local> Locals(Packages::Manager const& manager)
    {
        std::map<std::string, Packages::Local> result;
        for (auto const& local : manager.Installed())
        {
            auto it = result.find(local.manifest.id);
            if (it == result.end() || (!it->second.enabled && local.enabled))
                result[local.manifest.id] = local;
        }
        return result;
    }

    std::string InstalledStamp(Packages::Manager const& manager)
    {
        std::vector<std::string> lines;
        for (auto const& local : manager.Installed())
            lines.push_back(local.manifest.id + "\t" + local.manifest.version + "\t" +
                (local.enabled ? "1" : "0") + "\t" + Platform::PathToUtf8(local.manifest.dir));
        std::sort(lines.begin(), lines.end());
        std::ostringstream out;
        for (auto const& line : lines)
            out << line.size() << ':' << line;
        return out.str();
    }

    std::string ProfileStamp(ProfileConfig const& profile)
    {
        auto plugins = profile.Get({ "plugins" });
        return plugins ? fkyaml::node::serialize(*plugins) : std::string{};
    }

    bool Compatible(Packages::Local const& local)
    {
        if (!local.manifest.serverLibrary)
            return true;
        try
        {
            std::ifstream input(local.manifest.dir / "plugin.json", std::ios::binary);
            auto root = fkyaml::node::deserialize(input);
            if (!root.contains("core") || !root["core"].is_mapping() || !root["core"].contains("abi")
                || !root["core"]["abi"].is_string() || root["core"]["abi"].get_value<std::string>() != Packages::Manager::CoreAbi())
                return false;
            if (root.contains("platforms"))
            {
                bool found = false;
                if (root["platforms"].is_sequence())
                    for (auto const& platform : root["platforms"].as_seq())
                        found |= platform.is_string() && platform.get_value<std::string>() == Packages::Manager::Platform();
                if (!found)
                    return false;
            }
            auto name = root["server"]["library"].get_value<std::string>();
#ifdef _WIN32
            name += ".dll";
#elif defined(__APPLE__)
            name = "lib" + name + ".dylib";
#else
            name = "lib" + name + ".so";
#endif
            std::error_code ec;
            return fs::is_regular_file(local.manifest.dir / "server" / Packages::Manager::Platform() / Platform::Utf8ToPath(name), ec);
        }
        catch (std::exception const&) { return false; }
    }

    struct Selected
    {
        std::map<std::string, std::string> dependencies;
        std::vector<std::string> conflicts;
        std::optional<Packages::Step> install;
    };
}

ProfilePluginPlan LonelyIce::PreviewProfilePlugins(ProfileConfig const& profile, Packages::Manager const& manager,
    std::string const& activeStorage)
{
    ProfilePluginPlan plan;
    try
    {
        plan.installedStamp = InstalledStamp(manager);
        plan.profileStamp = ProfileStamp(profile);
        auto root = profile.Get({ "plugins" });
        if (root && !root->is_mapping())
            throw std::runtime_error(Tr("profile.plugins.mapping"));
        if (root)
            for (auto const& [key, value] : root->as_map())
            {
                if (!key.is_string() || !value.is_mapping())
                    throw std::runtime_error(Tr("profile.plugins.mapping"));
                auto id = key.get_value<std::string>();
                if (!value.contains("version") || !value["version"].is_string() || value["version"].get_value<std::string>().empty())
                    throw std::runtime_error(Tr("profile.plugins.version", id));
                if (value.contains("enabled") && !value["enabled"].is_boolean())
                    throw std::runtime_error(Tr("profile.plugins.enabled", id));
                plan.desired[id] = { value["version"].get_value<std::string>(),
                    !value.contains("enabled") || value["enabled"].get_value<bool>() };
            }

        auto const locals = Locals(manager);
        std::map<std::string, Selected> selected;
        for (auto const& [id, wanted] : plan.desired)
        {
            auto local = locals.find(id);
            auto& selection = selected[id];
            if (local != locals.end() && local->second.manifest.version == wanted.version
                && (!wanted.enabled || Compatible(local->second)))
            {
                selection.dependencies = std::map<std::string, std::string>(local->second.manifest.depends.begin(), local->second.manifest.depends.end());
                selection.conflicts = local->second.conflicts;
            }
            else
            {
                auto package = std::find_if(manager.Available().begin(), manager.Available().end(),
                    [&](auto const& p) { return p.id == id && p.version == wanted.version; });
                if (package == manager.Available().end())
                    throw std::runtime_error(Tr("profile.plugins.unavailable", id, wanted.version, Packages::Manager::Platform()));
                selection.dependencies = package->depends;
                selection.conflicts = package->conflicts;
                selection.install = Packages::Step{ *package, local == locals.end() ? std::string{} : local->second.manifest.version };
            }
            if (wanted.enabled)
                for (auto const& [dependency, range] : selection.dependencies)
                {
                    auto desired = plan.desired.find(dependency);
                    if (desired == plan.desired.end() || !desired->second.enabled)
                        throw std::runtime_error(Tr("profile.plugins.dependency", id, dependency, range));
                    if (!Acore::VersionRange::IsValid(range) || !Acore::VersionRange::Satisfies(desired->second.version, range))
                        throw std::runtime_error(Tr("profile.plugins.range", id, dependency, range, desired->second.version));
                }
        }
        for (auto const& [id, selection] : selected)
            if (plan.desired.at(id).enabled)
                for (auto const& conflict : selection.conflicts)
                    if (auto other = plan.desired.find(conflict); other != plan.desired.end() && other->second.enabled)
                        throw std::runtime_error(Tr("profile.plugins.conflict", id, conflict));

        for (auto const& [id, local] : locals)
        {
            auto desired = plan.desired.find(id);
            bool enabled = desired != plan.desired.end() && desired->second.enabled;
            if (local.manifest.storage && local.manifest.storage->id == activeStorage && !enabled)
                throw std::runtime_error(Tr("profile.plugins.active_storage", id));
            if (desired == plan.desired.end())
                plan.retained.push_back(id);
            if (local.enabled && !enabled)
                plan.disable.push_back(id);
        }
        // Dependency order is also the enable order. Disabled entries do not require their dependencies enabled.
        std::map<std::string, int> visited;
        std::function<void(std::string const&)> visit = [&](std::string const& id)
        {
            if (visited[id] == 2)
                return;
            if (visited[id] == 1)
                throw std::runtime_error(Tr("profile.plugins.cycle", id));
            visited[id] = 1;
            if (plan.desired.at(id).enabled)
                for (auto const& [dep, range] : selected.at(id).dependencies)
                    visit(dep);
            auto const& selection = selected.at(id);
            if (selection.install)
            {
                plan.install.steps.push_back(*selection.install);
                if (!plan.desired.at(id).enabled)
                    plan.disableInstalled.push_back(id);
            }
            else if (plan.desired.at(id).enabled)
            {
                auto local = locals.find(id);
                if (local != locals.end() && !local->second.enabled)
                    plan.enable.push_back(id);
            }
            visited[id] = 2;
        };
        for (auto const& [id, selection] : selected)
            visit(id);
    }
    catch (std::exception const& e)
    {
        plan.error = e.what();
    }
    return plan;
}

std::string LonelyIce::DescribeProfilePlugins(ProfilePluginPlan const& plan)
{
    if (!plan.error.empty())
        return plan.error;
    std::string out = Tr("profile.plugins.review") + "\n";
    for (auto const& step : plan.install.steps)
        out += Tr("profile.plugins.install", step.package.id, step.from.empty() ? Tr("profile.plugins.missing") : step.from, step.package.version) + "\n";
    for (auto const& id : plan.disable)
        out += Tr("profile.plugins.disable", id) + "\n";
    for (auto const& id : plan.enable)
        out += Tr("profile.plugins.enable", id) + "\n";
    for (auto const& id : plan.disableInstalled)
        out += Tr("profile.plugins.disable", id) + "\n";
    for (auto const& id : plan.retained)
        out += Tr("profile.plugins.retain", id) + "\n";
    if (plan.Empty())
        out += Tr("profile.plugins.matches") + "\n";
    return out;
}

bool LonelyIce::ValidateInstalledProfilePlugins(ProfileConfig const& profile, Packages::Manager const& manager, std::string& error)
{
    try
    {
        auto const installed = Locals(manager);
        std::set<std::string> enabled;
        auto root = profile.Get({ "plugins" });
        if (root && !root->is_mapping())
            throw std::runtime_error(Tr("profile.plugins.mapping"));
        if (root)
            for (auto const& [key, value] : root->as_map())
            {
                if (!key.is_string() || !value.is_mapping())
                    throw std::runtime_error(Tr("profile.plugins.mapping"));
                auto const id = key.get_value<std::string>();
                if (!value.contains("version") || !value["version"].is_string() || value["version"].get_value<std::string>().empty())
                    throw std::runtime_error(Tr("profile.plugins.version", id));
                if (value.contains("enabled") && !value["enabled"].is_boolean())
                    throw std::runtime_error(Tr("profile.plugins.enabled", id));
                auto const version = value["version"].get_value<std::string>();
                bool const on = !value.contains("enabled") || value["enabled"].get_value<bool>();
                auto local = installed.find(id);
                if (local == installed.end() || local->second.manifest.version != version || local->second.enabled != on
                    || (on && !Compatible(local->second)))
                    throw std::runtime_error(Tr("profile.plugins.mismatch", id, version, Tr(on ? "profile.plugins.on" : "profile.plugins.off")));
                if (on)
                    enabled.insert(id);
            }
        for (auto const& [id, local] : installed)
            if (local.enabled && !enabled.count(id))
                throw std::runtime_error(Tr("profile.plugins.extra", id));
        // All versions already exist, so preview validates dependency ranges/conflicts/cycles without fetching.
        auto validation = PreviewProfilePlugins(profile, manager);
        if (!validation.error.empty())
            throw std::runtime_error(validation.error);
        error.clear();
        return true;
    }
    catch (std::exception const& e)
    {
        error = e.what();
        return false;
    }
}

bool LonelyIce::ApplyProfilePlugins(ProfileConfig const& profile, Packages::Manager& manager, ProfilePluginPlan const& plan,
    std::string& error, std::function<void(std::string const&)> const& log,
    Http::Progress const& progress, std::function<bool()> const& cancelled)
{
    error.clear();
    auto check = [&]
    {
        if (cancelled && cancelled())
            error = Tr("profile.plugins.cancelled");
        else if (manager.ServerRunning())
            error = Tr("profile.plugins.stop_required");
        return error.empty();
    };
    if (!plan.error.empty())
        error = plan.error;
    else if (InstalledStamp(manager) != plan.installedStamp || ProfileStamp(profile) != plan.profileStamp)
        error = Tr("profile.plugins.stale");
    if (!error.empty() || !check())
        return false;
    auto toggle = [&](std::string const& id, bool enabled)
    {
        if (!check() || !manager.SetEnabled(id, enabled, error))
            return false;
        if (log)
            log(Tr(enabled ? "profile.plugins.enable" : "profile.plugins.disable", id));
        return true;
    };
    for (auto const& id : plan.disable)
        if (!toggle(id, false))
            return false;
    if (!plan.install.steps.empty() && (!check() || !manager.Install(plan.install, error, log, progress)))
        return false;
    for (auto const& id : plan.enable)
        if (!toggle(id, true))
            return false;
    for (auto const& id : plan.disableInstalled)
        if (!toggle(id, false))
            return false;
    if (!check())
        return false;
    auto const installed = Locals(manager);
    for (auto const& [id, wanted] : plan.desired)
    {
        auto local = installed.find(id);
        if (local == installed.end() || local->second.manifest.version != wanted.version || local->second.enabled != wanted.enabled
            || (wanted.enabled && !Compatible(local->second)))
        {
            error = Tr("profile.plugins.verify", id);
            return false;
        }
    }
    for (auto const& [id, local] : installed)
        if (local.enabled && !plan.desired.count(id))
        {
            error = Tr("profile.plugins.verify", id);
            return false;
        }
    return true;
}

void LonelyIce::SnapshotProfilePlugins(ProfileConfig& profile, Packages::Manager const& manager,
    std::optional<std::vector<std::string>> const& affected)
{
    auto const installed = Locals(manager);
    std::set<std::string> ids;
    if (affected)
        ids.insert(affected->begin(), affected->end());
    else
    {
        for (auto const& [id, local] : installed)
            ids.insert(id);
        if (auto current = profile.Get({ "plugins" }); current && current->is_mapping())
            for (auto const& [id, value] : current->as_map())
                if (id.is_string())
                    ids.insert(id.get_value<std::string>());
    }
    for (auto const& id : ids)
    {
        auto local = installed.find(id);
        if (local == installed.end())
        {
            profile.Remove({ "plugins", id }, ProfileConfig::Layer::Profile);
            profile.Remove({ "plugins", id }, ProfileConfig::Layer::Local);
        }
        else
        {
            profile.Set({ "plugins", id, "version" }, fkyaml::node(local->second.manifest.version));
            profile.Set({ "plugins", id, "enabled" }, fkyaml::node(local->second.enabled));
            profile.Remove({ "plugins", id, "version" }, ProfileConfig::Layer::Local);
            profile.Remove({ "plugins", id, "enabled" }, ProfileConfig::Layer::Local);
        }
    }
}
