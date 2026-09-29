#include "Plugins.h"
#include <algorithm>
#include <fstream>
#include <fkYAML/node.hpp>

namespace fs = std::filesystem;

namespace
{
    std::string Localized(fkyaml::node const& n)
    {
        if (n.is_string())
            return n.get_value<std::string>();
        if (!n.is_mapping())
            return {};
        for (char const* locale : { "ru", "en" })
            if (n.contains(locale) && n[locale].is_string())
                return n[locale].get_value<std::string>();
        return {};
    }

    std::string Str(fkyaml::node const& n, char const* key)
    {
        return n.is_mapping() && n.contains(key) && n[key].is_string() ? n[key].get_value<std::string>() : std::string();
    }
}

std::vector<LonelyIce::PluginManifest> LonelyIce::ReadPlugins(fs::path const& pluginsDir)
{
    std::vector<PluginManifest> out;
    std::error_code ec;
    for (fs::directory_iterator it(pluginsDir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::ifstream in(it->path() / "plugin.json", std::ios::binary);
        if (!in)
            continue;
        try
        {
            fkyaml::node root = fkyaml::node::deserialize(in);
            PluginManifest m;
            m.dir = it->path();
            m.id = Str(root, "id");
            m.version = Str(root, "version");
            m.name = root.contains("name") ? Localized(root["name"]) : m.id;
            if (m.id.empty())
                continue;
            std::string config = Str(root, "config");
            if (!config.empty())
                m.configDist = m.dir / fs::u8path(config);
            std::string settings = Str(root, "settings");
            if (settings.empty() && fs::exists(m.dir / "settings.json", ec))
                settings = "settings.json";
            if (!settings.empty())
                m.settings = m.dir / fs::u8path(settings);
            if (root.contains("client") && root["client"].is_mapping() && root["client"].contains("addons"))
                for (auto const& a : root["client"]["addons"].as_seq())
                    m.addons.push_back(m.dir / fs::u8path(a.get_value<std::string>()));
            out.push_back(std::move(m));
        }
        catch (std::exception const&)
        {
        }
    }
    std::sort(out.begin(), out.end(), [](PluginManifest const& a, PluginManifest const& b) { return a.id < b.id; });
    return out;
}

std::string LonelyIce::ConfigFileName(PluginManifest const& plugin)
{
    std::string name = plugin.configDist.filename().string();
    if (name.size() > 5 && name.ends_with(".dist"))
        name.resize(name.size() - 5);
    return name;
}
