#include "PackageManager.h"
#include "CryptoHash.h"
#include "Lang.h"
#include "PluginApi.h"
#include "PluginMgr.h"
#include "Platform.h"
#include "ServerLock.h"
#include "VersionRange.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <fkYAML/node.hpp>
#include <miniz.h>

namespace fs = std::filesystem;
using namespace LonelyIce::Packages;
using LonelyIce::Tr;

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

    bool SafeId(std::string const& id)
    {
        if (id.empty() || id.front() == '.' || id.back() == '.' || !std::all_of(id.begin(), id.end(), [](unsigned char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        }))
            return false;
        std::string base = id.substr(0, id.find('.'));
        std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return base != "con" && base != "prn" && base != "aux" && base != "nul" &&
            !(base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9');
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
            error = Tr("pkg.error.not_zip");
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
                error = Tr("pkg.error.no_manifest_in_package");
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
                error = Tr("pkg.error.bad_path", n);
                ok = false;
                break;
            }
            fs::path const out = dir / fs::u8path(rel);
            fs::create_directories(out.parent_path(), ec);
            if (!mz_zip_reader_extract_to_file(&a, i, out.string().c_str(), 0))
            {
                error = Tr("pkg.error.unpack_failed", n);
                ok = false;
            }
        }
        mz_zip_reader_end(&a);
        return ok;
    }

    bool IsUrl(std::string const& s)
    {
        return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 || s.rfind("file://", 0) == 0;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string Join(std::vector<std::string> const& items)
    {
        std::string out;
        for (std::string const& s : items)
            out += (out.empty() ? "" : ", ") + s;
        return out;
    }

    bool Contains(std::vector<std::string> const& items, std::string const& s)
    {
        return std::find(items.begin(), items.end(), s) != items.end();
    }

    // conflicts of an installed plugin; the launcher's manifest does not carry them
    std::vector<std::string> ReadConflicts(fs::path const& dir)
    {
        std::vector<std::string> out;
        try
        {
            std::ifstream in(dir / "plugin.json", std::ios::binary);
            fkyaml::node root = fkyaml::node::deserialize(in);
            if (root.is_mapping() && root.contains("conflicts") && root["conflicts"].is_sequence())
                for (auto const& c : root["conflicts"].as_seq())
                    if (c.is_string())
                        out.push_back(c.get_value<std::string>());
        }
        catch (std::exception const&)
        {
        }
        return out;
    }

    // id -> (range, who needs it)
    using Ranges = std::map<std::string, std::vector<std::pair<std::string, std::string>>>;

    bool Fits(std::string const& version, std::vector<std::pair<std::string, std::string>> const& ranges)
    {
        return std::all_of(ranges.begin(), ranges.end(), [&](auto const& r) { return PluginMgr::Satisfies(version, r.first); });
    }

    // Why no version of id fits: a range that cannot be read, or the ranges that exclude every version.
    std::string NoVersion(std::string const& id, std::vector<std::pair<std::string, std::string>> const& ranges)
    {
        std::string need;
        for (auto const& [range, who] : ranges)
        {
            if (std::string bad; !Acore::VersionRange::IsValid(range, &bad))
                return Tr("pkg.error.bad_range", id, who, range, bad);
            need += (need.empty() ? "" : ", ") + range + " (" + who + ")";
        }
        return Tr("pkg.error.no_version", id, need);
    }

    // What the resolver picked for an id: a catalog package, or the installed copy staying (package null).
    struct Choice
    {
        std::string version;
        Package const* package = nullptr;
    };
}

std::vector<std::string> Manager::SplitSources(std::string const& s)
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

std::string Manager::IndexLocation(std::string const& source)
{
    std::error_code ec;
    if (!IsUrl(source) && fs::is_directory(fs::u8path(source), ec))
    {
        std::u8string const p = (fs::u8path(source) / "index.json").u8string();
        return std::string(p.begin(), p.end());
    }
    return source;
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
    _sources.clear();
    error.clear();
    for (std::string const& location : SplitSources(sources))
    {
        Source& src = _sources.emplace_back();
        src.location = location;
        std::string const source = IndexLocation(location);
        std::vector<uint8_t> data;
        if (!Http::Get(source, data, src.error, progress))
        {
            error += (error.empty() ? "" : "; ") + location + ": " + src.error;
            continue;
        }
        std::vector<Package> found;
        try
        {
            fkyaml::node root = fkyaml::node::deserialize(std::string(data.begin(), data.end()));
            if (!root.contains("packages") || !root["packages"].is_sequence())
                throw std::runtime_error("no packages");
            if (root.contains("name"))
                src.name = Localized(root["name"]);
            for (fkyaml::node const& p : root["packages"].as_seq())
            {
                Package pkg;
                pkg.id = Str(p, "id");
                pkg.version = Str(p, "version");
                pkg.core = Str(p, "core");
                pkg.name = p.contains("name") ? Localized(p["name"]) : pkg.id;
                pkg.description = p.contains("description") ? Localized(p["description"]) : std::string();
                pkg.url = Http::Resolve(source, Str(p, "url"));
                if (std::string const icon = Str(p, "icon"); !icon.empty())
                    pkg.icon = Http::Resolve(source, icon);
                if (std::string const page = Str(p, "page"); !page.empty())
                    pkg.page = Http::Resolve(source, page);
                pkg.source = location;
                pkg.sha256 = Str(p, "sha256");
                if (p.contains("size") && p["size"].is_integer())
                    pkg.size = uint64_t(p["size"].get_value<int64_t>());
                if (p.contains("platforms") && p["platforms"].is_sequence())
                    for (auto const& x : p["platforms"].as_seq())
                        pkg.platforms.push_back(x.get_value<std::string>());
                if (p.contains("locales") && p["locales"].is_sequence())
                    for (auto const& x : p["locales"].as_seq())
                        if (x.is_string())
                            pkg.locales.push_back(x.get_value<std::string>());
                if (p.contains("depends") && p["depends"].is_mapping())
                    for (auto const& [k, v] : p["depends"].as_map())
                        pkg.depends[k.get_value<std::string>()] = v.is_string() ? v.get_value<std::string>() : "*";
                if (p.contains("conflicts") && p["conflicts"].is_sequence())
                    for (auto const& x : p["conflicts"].as_seq())
                        pkg.conflicts.push_back(x.get_value<std::string>());

                // Only packages this build can run: same core (when it has server code) and a build for this platform.
                bool const server = !pkg.platforms.empty();
                if (!SafeId(pkg.id) || pkg.version.empty() || pkg.url.empty())
                    continue;
                if (server && (pkg.core != CoreAbi() || std::find(pkg.platforms.begin(), pkg.platforms.end(), Platform()) == pkg.platforms.end()))
                    continue;
                found.push_back(std::move(pkg));
            }
        }
        catch (std::exception const& e)
        {
            src.error = Tr("pkg.error.not_index", e.what());
            error += (error.empty() ? "" : "; ") + location + ": " + src.error;
            continue;
        }
        src.ok = true;
        src.packages = found.size();
        std::move(found.begin(), found.end(), std::back_inserter(_available));
    }
    return _sources.empty() || std::any_of(_sources.begin(), _sources.end(), [](Source const& s) { return s.ok; });
}

void Manager::FetchIcons()
{
    std::error_code ec;
    fs::create_directories(IconCache(), ec);
    for (Package const& p : _available)
    {
        fs::path const file = IconFile(p);
        if (p.icon.empty() || fs::exists(file, ec))
            continue;
        std::vector<uint8_t> data;
        std::string error;
        if (!Http::Get(p.icon, data, error) || data.size() < 8 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G')
            continue;
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<char const*>(data.data()), std::streamsize(data.size()));
    }
}

std::vector<Local> Manager::Installed() const
{
    std::vector<Local> out;
    for (PluginManifest& m : ReadPlugins(_dir))
    {
        std::vector<std::string> conflicts = ReadConflicts(m.dir);
        out.push_back({ std::move(m), true, std::move(conflicts) });
    }
    for (PluginManifest& m : ReadPlugins(_dir / ".disabled"))
    {
        std::vector<std::string> conflicts = ReadConflicts(m.dir);
        out.push_back({ std::move(m), false, std::move(conflicts) });
    }
    return out;
}

bool Manager::ServerRunning() const
{
    return ServerLock::IsHeld(_dir);
}

Plan Manager::Resolve(std::map<std::string, std::string> const& requests, bool update) const
{
    Plan plan;
    for (auto const& [id, range] : requests)
        if (std::string bad; !Acore::VersionRange::IsValid(range, &bad))
        {
            plan.error = Tr("pkg.error.bad_range", id, Tr("pkg.resolve.requested"), range, bad);
            return plan;
        }

    // The installed plugins by id; an enabled copy wins over a disabled one of the same id.
    std::map<std::string, Local> installed;
    for (Local& l : Installed())
    {
        auto it = installed.find(l.manifest.id);
        if (it == installed.end() || (!it->second.enabled && l.enabled))
            installed[l.manifest.id] = std::move(l);
    }

    // The ids to decide: the requested ones and everything a decided one needs.
    std::map<std::string, Choice> chosen;
    auto wantedIds = [&]()
    {
        std::set<std::string> wanted;
        for (auto const& [id, range] : requests)
            wanted.insert(id);
        for (auto const& [id, c] : chosen)
        {
            if (c.package)
                for (auto const& [dep, range] : c.package->depends)
                    wanted.insert(dep);
            else
                for (auto const& [dep, range] : installed.at(id).manifest.depends)
                    wanted.insert(dep);
        }
        return wanted;
    };

    // The ranges the current choices bring: the requests, the enabled installed plugins that stay (kept, or not to
    // be decided at all) and the chosen packages. A replaced plugin's old ranges are gone with it; one still to be
    // decided brings its ranges once it is kept.
    auto constraints = [&](std::set<std::string> const& wanted)
    {
        Ranges ranges;
        for (auto const& [id, range] : requests)
            ranges[id].emplace_back(range, Tr("pkg.resolve.requested"));
        for (auto const& [id, l] : installed)
        {
            auto c = chosen.find(id);
            bool const stays = c != chosen.end() ? !c->second.package : !wanted.count(id);
            if (!l.enabled || !stays)
                continue;
            for (auto const& [dep, range] : l.manifest.depends)
                ranges[dep].emplace_back(range, id);
        }
        for (auto const& [id, c] : chosen)
            if (c.package)
                for (auto const& [dep, range] : c.package->depends)
                    ranges[dep].emplace_back(range, id);
        return ranges;
    };

    // Conflicts between what will be enabled afterwards, declared on either side.
    auto conflicts = [&]() -> std::string
    {
        auto present = [&](std::string const& id)
        {
            auto i = installed.find(id);
            return chosen.count(id) || (i != installed.end() && i->second.enabled);
        };
        for (auto const& [id, c] : chosen)
            if (c.package)
                for (std::string const& other : c.package->conflicts)
                    if (other != id && present(other))
                        return Tr("pkg.error.conflict", id, other);
        for (auto const& [id, l] : installed)
        {
            auto c = chosen.find(id);
            if (!l.enabled || (c != chosen.end() && c->second.package))
                continue;
            for (std::string const& other : l.conflicts)
                if (auto n = chosen.find(other); other != id && n != chosen.end() && n->second.package)
                    return Tr("pkg.error.conflict_installed", other, id);
        }
        return {};
    };

    std::string failure;
    std::size_t failureDepth = 0;
    auto fail = [&](std::size_t depth, std::string error)
    {
        if (failure.empty() || depth >= failureDepth)
        {
            failure = std::move(error);
            failureDepth = depth;
        }
    };

    // Depth-first over the ids still to decide, trying their candidates in order of preference; a dead end
    // takes the last choice back.
    int budget = 20000;
    std::function<bool(std::size_t)> search = [&](std::size_t depth) -> bool
    {
        if (--budget < 0)
            return false;
        std::set<std::string> const wanted = wantedIds();
        Ranges ranges = constraints(wanted);
        for (auto const& [id, c] : chosen)
            if (auto r = ranges.find(id); r != ranges.end() && !Fits(c.version, r->second))
            {
                fail(depth, NoVersion(id, r->second));
                return false;
            }

        auto next = std::find_if(wanted.begin(), wanted.end(), [&](std::string const& id) { return !chosen.count(id); });
        if (next == wanted.end())
        {
            if (std::string const c = conflicts(); !c.empty())
            {
                fail(depth, c);
                return false;
            }
            return true;
        }
        std::string const id = *next;
        std::vector<std::pair<std::string, std::string>> const& need = ranges[id];

        // A disabled plugin does not run, so it cannot satisfy anything: the player enables it first.
        auto inst = installed.find(id);
        if (inst != installed.end() && !inst->second.enabled)
        {
            std::vector<std::string> who;
            for (auto const& [range, by] : need)
                if (!Contains(who, by))
                    who.push_back(by);
            fail(depth + 1, Tr("pkg.error.enable_first", id, Join(who)));
            return false;
        }

        // Candidates: the installed copy first (on an update, after the newer versions), then the catalog's
        // versions from the newest; on an update, older versions come last.
        std::vector<Package const*> packages;
        for (Package const& p : _available)
            if (p.id == id && (inst == installed.end() || CompareVersions(p.version, inst->second.manifest.version) != 0))
                packages.push_back(&p);
        std::stable_sort(packages.begin(), packages.end(), [](Package const* a, Package const* b) { return CompareVersions(a->version, b->version) > 0; });
        std::vector<Choice> candidates;
        bool const updating = update && requests.count(id);
        if (inst != installed.end() && !updating)
            candidates.push_back({ inst->second.manifest.version, nullptr });
        for (Package const* p : packages)
            if (!updating || inst == installed.end() || CompareVersions(p->version, inst->second.manifest.version) > 0)
                candidates.push_back({ p->version, p });
        if (inst != installed.end() && updating)
        {
            candidates.push_back({ inst->second.manifest.version, nullptr });
            for (Package const* p : packages)
                if (CompareVersions(p->version, inst->second.manifest.version) < 0)
                    candidates.push_back({ p->version, p });
        }

        bool tried = false;
        for (Choice const& c : candidates)
        {
            if (!Fits(c.version, need))
                continue;
            tried = true;
            chosen[id] = c;
            if (search(depth + 1))
                return true;
            chosen.erase(id);
            if (budget < 0)
                return false;
        }
        if (!tried)
            fail(depth + 1, NoVersion(id, need));
        return false;
    };

    if (!search(0))
    {
        plan.error = budget < 0 ? Tr("pkg.error.resolve_too_complex") : failure;
        return plan;
    }

    // dependencies first; a plugin that stays as installed gets no step
    std::set<std::string> placed;
    auto place = [&](auto&& self, std::string const& id) -> void
    {
        auto it = chosen.find(id);
        if (it == chosen.end() || !it->second.package || !placed.insert(id).second)
            return;
        for (auto const& [dep, range] : it->second.package->depends)
            self(self, dep);
        auto inst = installed.find(id);
        plan.steps.push_back({ *it->second.package, inst == installed.end() ? std::string() : inst->second.manifest.version });
    };
    for (auto const& [id, c] : chosen)
        place(place, id);
    return plan;
}

Plan Manager::ResolveUpdates() const
{
    std::map<std::string, std::string> requests;
    for (Local const& l : Installed())
        if (l.enabled)
            for (Package const& p : _available)
                if (p.id == l.manifest.id && CompareVersions(p.version, l.manifest.version) > 0)
                    requests[l.manifest.id] = "*";
    if (requests.empty())
        return {};
    return Resolve(requests, true);
}

bool Manager::Install(Plan const& plan, std::string& error, std::function<void(std::string const&)> const& log, Http::Progress const& progress)
{
    ServerLock const operationLock(_dir);
    if (!operationLock.Held())
    {
        error = Tr("pkg.error.server_running", Platform::PathToUtf8(_dir));
        return false;
    }

    for (Step const& step : plan.steps)
        if (!SafeId(step.package.id))
        {
            error = Tr("pkg.error.bad_path", step.package.id);
            return false;
        }

    std::error_code ec;
    fs::path const staging = _dir / ".staging";
    fs::remove_all(staging, ec);

    // 1. Every package is downloaded, checked and unpacked before anything installed changes.
    for (Step const& step : plan.steps)
    {
        Package const& p = step.package;
        if (log)
            log(Tr("pkg.log.downloading", p.id, p.version));
        std::vector<uint8_t> zip;
        bool ok = Http::Get(p.url, zip, error, progress);
        if (ok && ((p.size && zip.size() != p.size) || (!p.sha256.empty() && Sha256(zip.data(), zip.size()) != Lower(p.sha256))))
        {
            error = Tr("pkg.error.corrupt", p.id);
            ok = false;
        }
        fs::path const dst = staging / p.id;
        if (ok)
        {
            fs::create_directories(dst, ec);
            if (!Unzip(zip, dst, error))
            {
                error = p.id + ": " + error;
                ok = false;
            }
        }
        if (ok)
        {
            std::vector<PluginManifest> check = ReadPlugins(staging);
            auto m = std::find_if(check.begin(), check.end(), [&](PluginManifest const& x) { return x.dir.filename() == p.id; });
            if (m == check.end() || m->id != p.id || m->version != p.version)
            {
                error = Tr("pkg.error.manifest_mismatch", p.id);
                ok = false;
            }
        }
        if (!ok)
        {
            fs::remove_all(staging, ec);
            return false;
        }
    }

    // 2. The old folders (enabled or disabled copy, or whatever is in plugins/<id>) are moved aside and the new ones
    // moved in. Every move is recorded; a failure moves everything back, so the plugins are as before.
    fs::path const backupRoot = _dir / ".backup";
    fs::path const backup = backupRoot / std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    std::vector<std::pair<fs::path, fs::path>> moves;      // from, to
    auto relocate = [&](fs::path const& from, fs::path const& to)
    {
        fs::create_directories(to.parent_path(), ec);
        fs::rename(from, to, ec);
        if (ec)
            return false;
        moves.emplace_back(from, to);
        return true;
    };

    std::vector<Local> const before = Installed();
    for (std::size_t i = 0; i < plan.steps.size(); ++i)
    {
        Package const& p = plan.steps[i].package;
        fs::path const target = _dir / fs::u8path(p.id);
        std::vector<fs::path> old;
        for (Local const& l : before)
            if (l.manifest.id == p.id)
                old.push_back(l.manifest.dir);
        if (fs::exists(target, ec) && std::none_of(old.begin(), old.end(), [&](fs::path const& o) { return fs::equivalent(o, target, ec); }))
            old.push_back(target);

        std::string failed;
        for (std::size_t k = 0; k < old.size() && failed.empty(); ++k)
            if (!relocate(old[k], backup / (std::to_string(i) + "-" + std::to_string(k) + "-" + old[k].filename().string())))
                failed = Tr("pkg.error.move_old_failed", p.id, ec.message());
        if (failed.empty() && !relocate(staging / fs::u8path(p.id), target))
            failed = Tr("pkg.error.install_failed", p.id, ec.message());
        if (failed.empty())
            continue;

        std::vector<std::string> stuck;
        for (auto it = moves.rbegin(); it != moves.rend(); ++it)
        {
            std::error_code e;
            fs::rename(it->second, it->first, e);
            if (e)
                stuck.push_back(Platform::PathToUtf8(it->second) + " > " + Platform::PathToUtf8(it->first));
        }
        error = failed + " " + (stuck.empty() ? Tr("pkg.error.rolled_back") : Tr("pkg.error.rollback_failed", Join(stuck)));
        if (stuck.empty())
        {
            fs::remove_all(backup, ec);
            fs::remove(backupRoot, ec);     // only when empty
        }
        fs::remove_all(staging, ec);
        return false;
    }

    for (Step const& step : plan.steps)
        if (log)
            log(step.from.empty() ? Tr("pkg.log.installed", step.package.id, step.package.version)
                : Tr("pkg.log.updated", step.package.id, step.from, step.package.version));
    fs::remove_all(backup, ec);
    fs::remove(backupRoot, ec);
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
    ServerLock const operationLock(_dir);
    if (!operationLock.Held())
    {
        error = Tr("pkg.error.server_running", Platform::PathToUtf8(_dir));
        return false;
    }
    std::error_code ec;
    for (Local const& l : Installed())
        if (l.manifest.id == id)
        {
            fs::remove_all(l.manifest.dir, ec);
            if (ec)
            {
                error = Tr("pkg.error.remove_failed", id, ec.message());
                return false;
            }
            return true;
        }
    error = Tr("pkg.error.not_installed", id);
    return false;
}

bool Manager::SetEnabled(std::string const& id, bool enabled, std::string& error)
{
    ServerLock const operationLock(_dir);
    if (!operationLock.Held())
    {
        error = Tr("pkg.error.server_running", Platform::PathToUtf8(_dir));
        return false;
    }
    std::vector<Local> const all = Installed();
    auto self = std::find_if(all.begin(), all.end(), [&](Local const& l) { return l.manifest.id == id && l.enabled == enabled; });
    if (self != all.end())
        return true;
    self = std::find_if(all.begin(), all.end(), [&](Local const& l) { return l.manifest.id == id; });
    if (self == all.end())
    {
        error = Tr("pkg.error.not_installed", id);
        return false;
    }

    if (enabled)
    {
        // Its dependencies must be there, enabled and in range, as the server will check.
        std::vector<std::string> missing;
        for (auto const& [dep, range] : self->manifest.depends)
        {
            if (std::string bad; !Acore::VersionRange::IsValid(range, &bad))
            {
                error = Tr("pkg.error.bad_range", dep, id, range, bad);
                return false;
            }
            auto on = std::find_if(all.begin(), all.end(), [&](Local const& l) { return l.manifest.id == dep && l.enabled; });
            auto off = std::find_if(all.begin(), all.end(), [&](Local const& l) { return l.manifest.id == dep && !l.enabled; });
            if (on != all.end())
            {
                if (!PluginMgr::Satisfies(on->manifest.version, range))
                    missing.push_back(Tr("pkg.error.dep_version", dep, range, on->manifest.version));
            }
            else if (off != all.end())
                missing.push_back(Tr("pkg.error.dep_disabled", dep));
            else
                missing.push_back(Tr("pkg.error.dep_missing", dep, range));
        }
        if (!missing.empty())
        {
            error = Tr("pkg.error.enable_needs", id, Join(missing));
            return false;
        }
        for (Local const& l : all)
        {
            if (!l.enabled || l.manifest.id == id)
                continue;
            if (Contains(self->conflicts, l.manifest.id))
            {
                error = Tr("pkg.error.conflict", id, l.manifest.id);
                return false;
            }
            if (Contains(l.conflicts, id))
            {
                error = Tr("pkg.error.conflict", l.manifest.id, id);
                return false;
            }
        }
    }

    std::error_code ec;
    fs::path const to = enabled ? _dir / self->manifest.dir.filename() : _dir / ".disabled" / self->manifest.dir.filename();
    fs::create_directories(to.parent_path(), ec);
    fs::rename(self->manifest.dir, to, ec);
    if (ec)
    {
        error = Tr("pkg.error.move_failed", id, ec.message());
        return false;
    }
    return true;
}

bool Manager::Pack(fs::path const& pluginDir, fs::path const& outDir, std::string& entry, std::string& error)
{
    fkyaml::node root;
    try
    {
        std::ifstream in(pluginDir / "plugin.json", std::ios::binary);
        if (!in)
            throw std::runtime_error(Tr("pkg.error.no_manifest"));
        root = fkyaml::node::deserialize(in);
    }
    catch (std::exception const& e)
    {
        error = e.what();
        return false;
    }
    std::string const id = Str(root, "id"), version = Str(root, "version");
    if (!SafeId(id) || version.empty())
    {
        error = Tr("pkg.error.manifest_no_id");
        return false;
    }
    if (root.contains("locales") && root["locales"].is_sequence())
        for (auto const& l : root["locales"].as_seq())
        {
            std::string const locale = l.is_string() ? l.get_value<std::string>() : std::string();
            auto const& known = PluginLocales();
            if (locale != "*" && std::find(known.begin(), known.end(), locale) == known.end())
            {
                error = Tr("pkg.error.bad_locale", locale);
                return false;
            }
        }

    // Ranges the resolver and the server could not read are refused here already.
    std::vector<std::pair<std::string, std::string>> depends;
    if (root.contains("depends") && root["depends"].is_mapping())
        for (auto const& [k, v] : root["depends"].as_map())
        {
            std::string const dep = k.is_string() ? k.get_value<std::string>() : std::string();
            std::string const range = v.is_string() ? v.get_value<std::string>() : "*";
            if (std::string bad; !Acore::VersionRange::IsValid(range, &bad))
            {
                error = Tr("pkg.error.bad_range", dep, id, range, bad);
                return false;
            }
            depends.emplace_back(dep, range);
        }

    // Files under <id>/, without debug files and link-time leftovers (MSVC; .dSYM bundles and static archives elsewhere).
    std::vector<std::pair<std::string, fs::path>> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(pluginDir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::string const ext = it->path().extension().string();
        if (ext == ".dSYM" && it->is_directory(ec))
        {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec))
            continue;
        if (ext == ".pdb" || ext == ".ilk" || ext == ".exp" || ext == ".lib" || ext == ".a")
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
        error = Tr("pkg.error.create_failed", out.string());
        return false;
    }
    for (auto const& [name, path] : files)
        if (!mz_zip_writer_add_file(&a, name.c_str(), path.string().c_str(), nullptr, 0, MZ_BEST_COMPRESSION))
        {
            error = Tr("pkg.error.add_failed", name);
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
    for (char const* key : { "platforms", "locales" })
    {
        if (!root.contains(key) || !root[key].is_sequence())
            continue;
        e << ", \"" << key << "\": [";
        bool first = true;
        for (auto const& p : root[key].as_seq())
            if (p.is_string())
            {
                e << (first ? " " : ", ") << JsonString(p.get_value<std::string>());
                first = false;
            }
        e << " ]";
    }
    if (!depends.empty())
    {
        e << ", \"depends\": {";
        bool first = true;
        for (auto const& [dep, range] : depends)
        {
            e << (first ? " " : ", ") << JsonString(dep) << ": " << JsonString(range);
            first = false;
        }
        e << " }";
    }
    if (root.contains("conflicts") && root["conflicts"].is_sequence())
    {
        e << ", \"conflicts\": [";
        bool first = true;
        for (auto const& c : root["conflicts"].as_seq())
            if (c.is_string())
            {
                e << (first ? " " : ", ") << JsonString(c.get_value<std::string>());
                first = false;
            }
        e << " ]";
    }
    // The icon also goes next to the zip, so the launcher can show it before the package is installed.
    if (fs::exists(pluginDir / "icon.png", ec))
    {
        std::string const iconName = id + "-" + version + ".png";
        fs::copy_file(pluginDir / "icon.png", outDir / iconName, fs::copy_options::overwrite_existing, ec);
        if (!ec)
            e << ", \"icon\": " << JsonString(iconName);
    }
    e << ", \"url\": " << JsonString(fileName) << ", \"sha256\": " << JsonString(Sha256(zip.data(), zip.size())) << ", \"size\": " << zip.size() << " }";
    entry = e.str();
    return true;
}
