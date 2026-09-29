#include "PackageManager.h"
#include "CryptoHash.h"
#include "PluginApi.h"
#include "PluginMgr.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <fkYAML/node.hpp>
#include <miniz.h>

namespace fs = std::filesystem;
using namespace LonelyIce::Packages;

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

    std::string Sha256(uint8_t const* data, std::size_t size)
    {
        Acore::Crypto::SHA256 sha;
        sha.UpdateData(data, size);
        sha.Finalize();
        std::string out;
        for (uint8_t b : sha.GetDigest())
        {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", b);
            out += hex;
        }
        return out;
    }

    std::string JsonString(std::string const& s)
    {
        std::string out = "\"";
        for (char c : s)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            if (c == '\n')
            {
                out += "\\n";
                continue;
            }
            out += c;
        }
        return out + "\"";
    }

    // A localized manifest value as JSON: a string, or an object of locale -> string.
    std::string JsonLocalized(fkyaml::node const& n)
    {
        if (n.is_string())
            return JsonString(n.get_value<std::string>());
        std::string out = "{";
        if (n.is_mapping())
            for (auto const& [k, v] : n.as_map())
                if (k.is_string() && v.is_string())
                    out += (out.size() > 1 ? ", " : " ") + JsonString(k.get_value<std::string>()) + ": " + JsonString(v.get_value<std::string>());
        return out + " }";
    }

    bool SafeRelative(std::string const& name)
    {
        if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos)
            return false;
        for (fs::path const& part : fs::u8path(name))
            if (part == "..")
                return false;
        return true;
    }

    // Unpacks a package into dir. The zip holds the plugin folder's contents, at its root or in one top folder.
    bool Unzip(std::vector<uint8_t> const& zip, fs::path const& dir, std::string& error)
    {
        mz_zip_archive a{};
        if (!mz_zip_reader_init_mem(&a, zip.data(), zip.size(), 0))
        {
            error = "это не zip";
            return false;
        }
        mz_uint const count = mz_zip_reader_get_num_files(&a);
        std::vector<std::string> names;
        for (mz_uint i = 0; i < count; ++i)
        {
            char name[1024];
            mz_zip_reader_get_filename(&a, i, name, sizeof(name));
            names.emplace_back(name);
        }

        std::string prefix;
        if (std::find(names.begin(), names.end(), "plugin.json") == names.end())
        {
            for (std::string const& n : names)
                if (std::size_t slash = n.find('/'); slash != std::string::npos && n.substr(slash + 1) == "plugin.json")
                    prefix = n.substr(0, slash + 1);
            if (prefix.empty())
            {
                error = "в пакете нет plugin.json";
                mz_zip_reader_end(&a);
                return false;
            }
        }

        std::error_code ec;
        bool ok = true;
        for (mz_uint i = 0; i < count && ok; ++i)
        {
            std::string const& n = names[i];
            if (n.rfind(prefix, 0) != 0 || mz_zip_reader_is_file_a_directory(&a, i))
                continue;
            std::string const rel = n.substr(prefix.size());
            if (!SafeRelative(rel))
            {
                error = "недопустимый путь в пакете: " + n;
                ok = false;
                break;
            }
            fs::path const out = dir / fs::u8path(rel);
            fs::create_directories(out.parent_path(), ec);
            if (!mz_zip_reader_extract_to_file(&a, i, out.string().c_str(), 0))
            {
                error = "не удалось распаковать " + n;
                ok = false;
            }
        }
        mz_zip_reader_end(&a);
        return ok;
    }

    std::vector<std::string> SplitSources(std::string const& s)
    {
        std::vector<std::string> out;
        std::stringstream ss(s);
        for (std::string part; std::getline(ss, part, ';');)
        {
            part.erase(0, part.find_first_not_of(" \t"));
            part.erase(part.find_last_not_of(" \t") + 1);
            if (!part.empty())
                out.push_back(part);
        }
        return out;
    }
}

Manager::Manager(fs::path pluginsDir) : _dir(std::move(pluginsDir))
{
}

std::string Manager::CoreAbi()
{
    return AC_PLUGIN_ABI;
}

std::string Manager::Platform()
{
    return AC_PLUGIN_PLATFORM;
}

int Manager::CompareVersions(std::string const& a, std::string const& b)
{
    auto parts = [](std::string const& v)
    {
        std::vector<int> p;
        std::stringstream ss(v);
        for (std::string x; std::getline(ss, x, '.');)
            p.push_back(std::atoi(x.c_str()));
        p.resize(3);
        return p;
    };
    std::vector<int> const x = parts(a), y = parts(b);
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

bool Manager::LoadIndex(std::string const& sources, std::string& error, Http::Progress const& progress)
{
    _available.clear();
    for (std::string const& source : SplitSources(sources))
    {
        std::vector<uint8_t> data;
        if (!Http::Get(source, data, error, progress))
            return false;
        try
        {
            fkyaml::node root = fkyaml::node::deserialize(std::string(data.begin(), data.end()));
            if (!root.contains("packages") || !root["packages"].is_sequence())
                throw std::runtime_error("no packages");
            for (fkyaml::node const& p : root["packages"].as_seq())
            {
                Package pkg;
                pkg.id = Str(p, "id");
                pkg.version = Str(p, "version");
                pkg.core = Str(p, "core");
                pkg.name = p.contains("name") ? Localized(p["name"]) : pkg.id;
                pkg.description = p.contains("description") ? Localized(p["description"]) : std::string();
                pkg.url = Http::Resolve(source, Str(p, "url"));
                pkg.sha256 = Str(p, "sha256");
                if (p.contains("size") && p["size"].is_integer())
                    pkg.size = uint64_t(p["size"].get_value<int64_t>());
                if (p.contains("platforms") && p["platforms"].is_sequence())
                    for (auto const& x : p["platforms"].as_seq())
                        pkg.platforms.push_back(x.get_value<std::string>());
                if (p.contains("depends") && p["depends"].is_mapping())
                    for (auto const& [k, v] : p["depends"].as_map())
                        pkg.depends[k.get_value<std::string>()] = v.is_string() ? v.get_value<std::string>() : "*";
                if (p.contains("conflicts") && p["conflicts"].is_sequence())
                    for (auto const& x : p["conflicts"].as_seq())
                        pkg.conflicts.push_back(x.get_value<std::string>());

                // Only packages this build can run: same core (when it has server code) and a build for this platform.
                bool const server = !pkg.platforms.empty();
                if (pkg.id.empty() || pkg.version.empty() || pkg.url.empty())
                    continue;
                if (server && (pkg.core != CoreAbi() || std::find(pkg.platforms.begin(), pkg.platforms.end(), Platform()) == pkg.platforms.end()))
                    continue;
                _available.push_back(std::move(pkg));
            }
        }
        catch (std::exception const& e)
        {
            error = source + ": " + e.what();
            return false;
        }
    }
    return true;
}

std::vector<Local> Manager::Installed() const
{
    std::vector<Local> out;
    for (PluginManifest& m : ReadPlugins(_dir))
        out.push_back({ std::move(m), true, {} });
    for (PluginManifest& m : ReadPlugins(_dir / ".disabled"))
        out.push_back({ std::move(m), false, {} });
    return out;
}

Plan Manager::Resolve(std::map<std::string, std::string> const& requests, bool update) const
{
    Plan plan;
    std::map<std::string, std::string> installed;      // id -> version
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> ranges;   // id -> (range, who needs it)
    for (Local const& l : Installed())
    {
        installed[l.manifest.id] = l.manifest.version;
        // enabled plugins that stay must keep working
        if (l.enabled && !requests.count(l.manifest.id))
            for (auto const& [dep, range] : l.manifest.depends)
                ranges[dep].emplace_back(range, l.manifest.id);
    }

    std::set<std::string> want;
    for (auto const& [id, range] : requests)
    {
        ranges[id].emplace_back(range, "запрос");
        want.insert(id);
    }

    std::map<std::string, Package const*> chosen;
    for (int round = 0; round < 32; ++round)
    {
        bool changed = false;
        for (std::string const& id : std::set<std::string>(want))
        {
            auto satisfiesAll = [&](std::string const& version)
            {
                return std::all_of(ranges[id].begin(), ranges[id].end(), [&](auto const& r) { return PluginMgr::Satisfies(version, r.first); });
            };

            // keep the installed version when it fits and nothing asks for an update
            auto inst = installed.find(id);
            if (inst != installed.end() && !(update && requests.count(id)) && satisfiesAll(inst->second))
            {
                if (chosen.erase(id))
                    changed = true;
                continue;
            }

            Package const* best = nullptr;
            for (Package const& p : _available)
                if (p.id == id && satisfiesAll(p.version) && (!best || CompareVersions(p.version, best->version) > 0))
                    best = &p;
            if (!best)
            {
                std::string need;
                for (auto const& [range, who] : ranges[id])
                    need += (need.empty() ? "" : ", ") + range + " (" + who + ")";
                plan.error = "нет подходящей версии " + id + ": нужно " + need;
                return plan;
            }
            if (chosen[id] != best)
            {
                chosen[id] = best;
                for (auto const& [dep, range] : best->depends)
                {
                    ranges[dep].emplace_back(range, id);
                    want.insert(dep);
                }
                changed = true;
            }
        }
        if (!changed)
            break;
    }

    // conflicts between what will be there
    std::set<std::string> present;
    for (auto const& [id, v] : installed)
        present.insert(id);
    for (auto const& [id, p] : chosen)
        present.insert(id);
    for (auto const& [id, p] : chosen)
        for (std::string const& c : p->conflicts)
            if (present.count(c))
            {
                plan.error = id + " несовместим с " + c;
                return plan;
            }

    // dependencies first
    std::set<std::string> placed;
    auto place = [&](auto&& self, std::string const& id) -> void
    {
        auto it = chosen.find(id);
        if (it == chosen.end() || !placed.insert(id).second)
            return;
        for (auto const& [dep, range] : it->second->depends)
            self(self, dep);
        auto inst = installed.find(id);
        if (inst == installed.end() || inst->second != it->second->version)
            plan.steps.push_back({ *it->second, inst == installed.end() ? std::string() : inst->second });
    };
    for (auto const& [id, p] : chosen)
        place(place, id);
    return plan;
}

Plan Manager::ResolveUpdates() const
{
    std::map<std::string, std::string> requests;
    for (Local const& l : Installed())
        for (Package const& p : _available)
            if (p.id == l.manifest.id && CompareVersions(p.version, l.manifest.version) > 0)
                requests[l.manifest.id] = "*";
    if (requests.empty())
        return {};
    return Resolve(requests, true);
}

bool Manager::Install(Plan const& plan, std::string& error, std::function<void(std::string const&)> const& log, Http::Progress const& progress)
{
    std::error_code ec;
    fs::path const staging = _dir / ".staging";
    for (Step const& step : plan.steps)
    {
        Package const& p = step.package;
        if (log)
            log("Загрузка " + p.id + " " + p.version);
        std::vector<uint8_t> zip;
        if (!Http::Get(p.url, zip, error, progress))
            return false;
        if ((p.size && zip.size() != p.size) || (!p.sha256.empty() && Sha256(zip.data(), zip.size()) != p.sha256))
        {
            error = p.id + ": пакет повреждён (размер или sha256 не совпадают)";
            return false;
        }

        fs::path const dst = staging / p.id;
        fs::remove_all(dst, ec);
        fs::create_directories(dst, ec);
        if (!Unzip(zip, dst, error))
        {
            error = p.id + ": " + error;
            return false;
        }
        std::vector<PluginManifest> check = ReadPlugins(staging);
        auto m = std::find_if(check.begin(), check.end(), [&](PluginManifest const& x) { return x.dir.filename() == p.id; });
        if (m == check.end() || m->id != p.id || m->version != p.version)
        {
            error = p.id + ": plugin.json пакета не совпадает с индексом";
            return false;
        }

        // Replace the installed copy (enabled or disabled); the new one is enabled.
        for (Local const& l : Installed())
            if (l.manifest.id == p.id)
            {
                fs::remove_all(l.manifest.dir, ec);
                if (ec)
                {
                    error = p.id + ": не удалось удалить старую версию, сервер запущен? (" + ec.message() + ")";
                    return false;
                }
            }
        fs::rename(dst, _dir / p.id, ec);
        if (ec)
        {
            error = p.id + ": не удалось установить (" + ec.message() + ")";
            return false;
        }
        if (log)
            log(step.from.empty() ? "Установлен " + p.id + " " + p.version : "Обновлён " + p.id + " " + step.from + " → " + p.version);
    }
    fs::remove_all(staging, ec);
    return true;
}

std::vector<std::string> Manager::Dependents(std::string const& id) const
{
    std::vector<Local> const all = Installed();
    std::set<std::string> out{ id };
    for (bool grew = true; grew;)
    {
        grew = false;
        for (Local const& l : all)
            if (l.enabled && !out.count(l.manifest.id))
                for (auto const& [dep, range] : l.manifest.depends)
                    if (out.count(dep))
                    {
                        out.insert(l.manifest.id);
                        grew = true;
                        break;
                    }
    }
    out.erase(id);
    return std::vector<std::string>(out.begin(), out.end());
}

bool Manager::Remove(std::string const& id, std::string& error)
{
    std::error_code ec;
    for (Local const& l : Installed())
        if (l.manifest.id == id)
        {
            fs::remove_all(l.manifest.dir, ec);
            if (ec)
            {
                error = id + ": не удалось удалить, сервер запущен? (" + ec.message() + ")";
                return false;
            }
            return true;
        }
    error = id + " не установлен";
    return false;
}

bool Manager::SetEnabled(std::string const& id, bool enabled, std::string& error)
{
    std::error_code ec;
    for (Local const& l : Installed())
    {
        if (l.manifest.id != id)
            continue;
        if (l.enabled == enabled)
            return true;
        fs::path const to = enabled ? _dir / l.manifest.dir.filename() : _dir / ".disabled" / l.manifest.dir.filename();
        fs::create_directories(to.parent_path(), ec);
        fs::rename(l.manifest.dir, to, ec);
        if (ec)
        {
            error = id + ": не удалось переместить, сервер запущен? (" + ec.message() + ")";
            return false;
        }
        return true;
    }
    error = id + " не установлен";
    return false;
}

bool Manager::Pack(fs::path const& pluginDir, fs::path const& outDir, std::string& entry, std::string& error)
{
    fkyaml::node root;
    try
    {
        std::ifstream in(pluginDir / "plugin.json", std::ios::binary);
        if (!in)
            throw std::runtime_error("нет plugin.json");
        root = fkyaml::node::deserialize(in);
    }
    catch (std::exception const& e)
    {
        error = e.what();
        return false;
    }
    std::string const id = Str(root, "id"), version = Str(root, "version");
    if (id.empty() || version.empty())
    {
        error = "в plugin.json нет id или version";
        return false;
    }

    // Files under <id>/, without debug files.
    std::vector<std::pair<std::string, fs::path>> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(pluginDir, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        std::string const ext = it->path().extension().string();
        if (ext == ".pdb" || ext == ".ilk" || ext == ".exp" || ext == ".lib")
            continue;
        {
            std::u8string const rel = fs::relative(it->path(), pluginDir, ec).generic_u8string();
            files.emplace_back(id + "/" + std::string(rel.begin(), rel.end()), it->path());
        }
    }
    std::sort(files.begin(), files.end());

    fs::create_directories(outDir, ec);
    std::string const fileName = id + "-" + version + ".zip";
    fs::path const out = outDir / fileName;
    mz_zip_archive a{};
    if (!mz_zip_writer_init_file(&a, out.string().c_str(), 0))
    {
        error = "не удалось создать " + out.string();
        return false;
    }
    for (auto const& [name, path] : files)
        if (!mz_zip_writer_add_file(&a, name.c_str(), path.string().c_str(), nullptr, 0, MZ_BEST_COMPRESSION))
        {
            error = "не удалось добавить " + name;
            mz_zip_writer_end(&a);
            return false;
        }
    mz_zip_writer_finalize_archive(&a);
    mz_zip_writer_end(&a);

    std::ifstream in(out, std::ios::binary);
    std::vector<uint8_t> zip(std::istreambuf_iterator<char>(in), {});

    std::ostringstream e;
    e << "{ \"id\": " << JsonString(id) << ", \"version\": " << JsonString(version);
    if (root.contains("name"))
        e << ", \"name\": " << JsonLocalized(root["name"]);
    if (root.contains("description"))
        e << ", \"description\": " << JsonLocalized(root["description"]);
    if (root.contains("core") && root["core"].is_mapping())
        e << ", \"core\": " << JsonString(Str(root["core"], "abi"));
    if (root.contains("platforms") && root["platforms"].is_sequence())
    {
        e << ", \"platforms\": [";
        bool first = true;
        for (auto const& p : root["platforms"].as_seq())
        {
            e << (first ? " " : ", ") << JsonString(p.get_value<std::string>());
            first = false;
        }
        e << " ]";
    }
    if (root.contains("depends") && root["depends"].is_mapping())
    {
        e << ", \"depends\": {";
        bool first = true;
        for (auto const& [k, v] : root["depends"].as_map())
        {
            e << (first ? " " : ", ") << JsonString(k.get_value<std::string>()) << ": " << JsonString(v.is_string() ? v.get_value<std::string>() : "*");
            first = false;
        }
        e << " }";
    }
    e << ", \"url\": " << JsonString(fileName) << ", \"sha256\": " << JsonString(Sha256(zip.data(), zip.size())) << ", \"size\": " << zip.size() << " }";
    entry = e.str();
    return true;
}
