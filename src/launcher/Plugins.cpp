#include "Plugins.h"
#include "Lang.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
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
        std::map<std::string, std::string> texts;
        for (auto const& [k, v] : n.as_map())
            if (k.is_string() && v.is_string())
                texts[k.get_value<std::string>()] = v.get_value<std::string>();
        return LonelyIce::Lang::Pick(texts);
    }

    std::string Str(fkyaml::node const& n, char const* key)
    {
        return n.is_mapping() && n.contains(key) && n[key].is_string() ? n[key].get_value<std::string>() : std::string();
    }

    // Scalar as text: settings values may be written as numbers, bools or strings.
    std::optional<std::string> Scalar(fkyaml::node const& n)
    {
        if (n.is_string())
            return n.get_value<std::string>();
        if (n.is_boolean())
            return n.get_value<bool>() ? "1" : "0";
        if (n.is_integer())
            return std::to_string(n.get_value<int64_t>());
        if (n.is_float_number())
        {
            std::string s = std::to_string(n.get_value<double>());
            s.erase(s.find_last_not_of('0') + 1);
            if (s.back() == '.')
                s.pop_back();
            return s;
        }
        return std::nullopt;
    }

    std::optional<double> Number(fkyaml::node const& n, char const* key)
    {
        if (!n.contains(key))
            return std::nullopt;
        fkyaml::node const& v = n[key];
        if (v.is_integer())
            return double(v.get_value<int64_t>());
        if (v.is_float_number())
            return v.get_value<double>();
        return std::nullopt;
    }

    fkyaml::node ReadJson(fs::path const& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            throw std::runtime_error("cannot open " + file.filename().string());
        return fkyaml::node::deserialize(in);
    }

    // A manifest entry that is either inline or the name of a file in the plugin folder: returns the file,
    // plugin.json itself for inline content.
    fs::path InlineOrFile(fs::path const& dir, fkyaml::node const& n)
    {
        if (n.is_string())
            return dir / fs::u8path(n.get_value<std::string>());
        return dir / "plugin.json";
    }
}

std::vector<LonelyIce::PluginManifest> LonelyIce::ReadPlugins(fs::path const& pluginsDir)
{
    std::vector<PluginManifest> out;
    std::error_code ec;
    for (fs::directory_iterator it(pluginsDir, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!fs::exists(it->path() / "plugin.json", ec))
            continue;
        try
        {
            fkyaml::node root = ReadJson(it->path() / "plugin.json");
            PluginManifest m;
            m.dir = it->path();
            m.id = Str(root, "id");
            m.version = Str(root, "version");
            m.name = root.contains("name") ? Localized(root["name"]) : m.id;
            m.description = root.contains("description") ? Localized(root["description"]) : std::string();
            if (m.id.empty())
                continue;
            if (root.contains("depends") && root["depends"].is_mapping())
                for (auto const& [k, v] : root["depends"].as_map())
                    if (k.is_string() && v.is_string())
                        m.depends.emplace_back(k.get_value<std::string>(), v.get_value<std::string>());
            std::string config = Str(root, "config");
            if (!config.empty())
                m.configDist = m.dir / fs::u8path(config);
            if (root.contains("settings"))
                m.settings = InlineOrFile(m.dir, root["settings"]);
            else if (fs::exists(m.dir / "settings.json", ec))
                m.settings = m.dir / "settings.json";
            if (root.contains("client") && root["client"].is_mapping())
            {
                fkyaml::node const& client = root["client"];
                if (client.contains("addons") && client["addons"].is_sequence())
                    for (auto const& a : client["addons"].as_seq())
                        m.addons.push_back(m.dir / fs::u8path(a.get_value<std::string>()));
            }
            std::string patches = Str(root, "patches");
            if (!patches.empty())
                m.patches = m.dir / fs::u8path(patches);
            m.serverLibrary = root.contains("server") && !Str(root["server"], "library").empty();
            if (root.contains("provides") && root["provides"].is_sequence())
                for (auto const& p : root["provides"].as_seq())
                    if (p.is_string())
                        m.provides.push_back(p.get_value<std::string>());
            if (root.contains("storage") && root["storage"].is_mapping())
            {
                fkyaml::node const& s = root["storage"];
                StorageProvider p;
                p.id = Str(s, "id");
                p.name = s.contains("name") ? Localized(s["name"]) : m.name;
                p.port = s.contains("port") ? Scalar(s["port"]).value_or("") : std::string();
                if (s.contains("config") && s["config"].is_mapping())
                    for (auto const& [k, v] : s["config"].as_map())
                        if (k.is_string() && v.is_string())
                            p.config.emplace_back(k.get_value<std::string>(), v.get_value<std::string>());
                if (!p.id.empty() && p.id != "local")
                    m.storage = std::move(p);
            }
            out.push_back(std::move(m));
        }
        catch (std::exception const&)
        {
        }
    }
    std::sort(out.begin(), out.end(), [](PluginManifest const& a, PluginManifest const& b) { return a.id < b.id; });
    return out;
}

std::vector<LonelyIce::PluginManifest const*> LonelyIce::OrderByDependencies(std::vector<PluginManifest> const& plugins)
{
    std::map<std::string, PluginManifest const*> byId;
    for (PluginManifest const& p : plugins)
        byId[p.id] = &p;
    std::vector<PluginManifest const*> out;
    std::set<std::string> seen;
    auto visit = [&](auto&& self, PluginManifest const* p) -> void
    {
        if (!seen.insert(p->id).second)
            return;
        for (auto const& [dep, range] : p->depends)
            if (auto it = byId.find(dep); it != byId.end())
                self(self, it->second);
        out.push_back(p);
    };
    for (auto const& [id, p] : byId)
        visit(visit, p);
    return out;
}

LonelyIce::PluginSettings LonelyIce::ReadPluginSettings(PluginManifest const& plugin)
{
    PluginSettings s;
    if (plugin.settings.empty())
        return s;
    try
    {
        fkyaml::node root = ReadJson(plugin.settings);
        if (plugin.settings.filename() == "plugin.json")
            root = root["settings"];
        // a bare array of fields is a group named after the plugin
        fkyaml::node fields = root.is_sequence() ? root : root["fields"];
        s.group = root.is_mapping() && root.contains("group") ? Localized(root["group"]) : plugin.name;
        s.hint = root.is_mapping() && root.contains("hint") ? Localized(root["hint"]) : plugin.description;
        for (fkyaml::node const& f : fields.as_seq())
        {
            PluginSetting d;
            d.key = Str(f, "key");
            d.type = Str(f, "type");
            if (d.key.empty() || (d.type != "bool" && d.type != "int" && d.type != "float" && d.type != "string" && d.type != "choice"))
                continue;
            if (std::string a = Str(f, "apply"); a == "now" || a == "reload" || a == "restart")
                d.apply = a;
            d.label = f.contains("label") ? Localized(f["label"]) : d.key;
            d.hint = f.contains("hint") ? Localized(f["hint"]) : std::string();
            d.min = Number(f, "min");
            d.max = Number(f, "max");
            if (f.contains("default"))
                d.def = Scalar(f["default"]);
            if (f.contains("options") && f["options"].is_sequence())
                for (fkyaml::node const& o : f["options"].as_seq())
                    if (auto v = o.is_mapping() && o.contains("value") ? Scalar(o["value"]) : std::nullopt)
                        d.options.emplace_back(*v, o.contains("label") ? Localized(o["label"]) : *v);
            s.fields.push_back(std::move(d));
        }
    }
    catch (std::exception const& e)
    {
        s.fields.clear();
        s.error = plugin.id + ": " + e.what();
    }
    return s;
}

std::string LonelyIce::ConfigFileName(PluginManifest const& plugin)
{
    std::string name = plugin.configDist.filename().string();
    if (name.size() > 5 && name.ends_with(".dist"))
        name.resize(name.size() - 5);
    return name;
}
