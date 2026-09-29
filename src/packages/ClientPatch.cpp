#include "ClientPatch.h"
#include "ClientArchives.h"
#include "CryptoHash.h"
#include "GameClient.h"
#include "Lang.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>

// StormLib is used for writing archives here; reading goes through ClientArchives. TCHAR paths as there.
#ifdef _WIN32
#  ifndef UNICODE
#    define UNICODE
#  endif
#  ifndef _UNICODE
#    define _UNICODE
#  endif
#endif
#include <StormLib.h>

namespace fs = std::filesystem;
using namespace LonelyIce;
using DbcRecipes::Recipe;
using ClientArchives::Archive;

namespace
{
    char const* const MarkerName = "lonelyice-patch.txt";

    fs::path LocaleDir(fs::path const& data, std::string const& locale)
    {
        return GameClient::Child(data, locale);
    }

    std::string ReadFile(fs::path const& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

    std::string Hex(std::string const& s)
    {
        Acore::Crypto::SHA256 sha;
        sha.UpdateData(s);
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

    bool WriteArchive(fs::path const& path, std::vector<std::pair<std::string, std::vector<uint8_t>>> const& files, std::string& error)
    {
        std::error_code ec;
        fs::path tmp = path;
        tmp += ".tmp";
        fs::remove(tmp, ec);
        HANDLE h = nullptr;
        if (!SFileCreateArchive(tmp.c_str(), MPQ_CREATE_LISTFILE | MPQ_CREATE_ATTRIBUTES | MPQ_CREATE_ARCHIVE_V1,
                DWORD(std::max<std::size_t>(64, files.size() * 2)), &h))
        {
            error = Tr("patch.mpq.create_failed", tmp.string(), GetLastError());
            return false;
        }
        bool ok = true;
        for (auto const& [name, data] : files)
        {
            HANDLE f = nullptr;
            if (!SFileCreateFile(h, name.c_str(), 0, DWORD(data.size()), 0, MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING, &f)
                || !SFileWriteFile(f, data.data(), DWORD(data.size()), MPQ_COMPRESSION_ZLIB) || !SFileFinishFile(f))
            {
                error = Tr("patch.mpq.add_failed", name, GetLastError());
                ok = false;
                break;
            }
        }
        SFileCloseArchive(h);
        if (ok)
        {
            fs::rename(tmp, path, ec);
            if (ec)
            {
                error = Tr("patch.mpq.replace_failed", path.filename().string());
                ok = false;
            }
        }
        if (!ok)
            fs::remove(tmp, ec);
        return ok;
    }
}

std::string ClientPatch::ArchiveName(std::string const& locale)
{
    return "patch-" + locale + "-4.MPQ";
}

ClientPatch::HiddenArchives::HiddenArchives(fs::path const& clientDir)
{
    fs::path const data = ClientArchives::DataDir(clientDir);
    for (std::string const& locale : ClientArchives::Locales(clientDir))
    {
        fs::path const archive = GameClient::Child(LocaleDir(data, locale), ArchiveName(locale));
        std::error_code ec;
        if (!fs::exists(archive, ec) || !Archive(archive).Read(MarkerName))
            continue;
        fs::path hidden = archive;
        hidden += ".hidden";
        fs::rename(archive, hidden, ec);
        if (!ec)
            _moved.push_back(archive);
    }
}

ClientPatch::HiddenArchives::~HiddenArchives()
{
    for (fs::path const& archive : _moved)
    {
        fs::path hidden = archive;
        hidden += ".hidden";
        std::error_code ec;
        fs::rename(hidden, archive, ec);
    }
}

std::vector<uint8_t> ClientPatch::ReadStockTable(fs::path const& clientDir, std::string const& table)
{
    for (std::string const& locale : ClientArchives::Locales(clientDir))
        for (fs::path const& a : ClientArchives::Chain(clientDir, locale))
            if (auto raw = Archive(a).Read("DBFilesClient\\" + table))
                return std::move(*raw);
    return {};
}

ClientPatch::Result ClientPatch::Apply(fs::path const& clientDir, std::vector<Recipe> const& recipes, DbcRecipes::IdMap const& ids)
{
    Result res;
    fs::path data = ClientArchives::DataDir(clientDir);
    std::error_code ec;

    // Everything that goes into the archives, for the stamp.
    std::string stampSource;
    std::vector<std::string> tables;
    for (Recipe const& r : recipes)
    {
        stampSource += r.Plugin() + "@" + r.Version() + "\n" + r.Text() + "\n";
        for (auto const& [to, from] : r.Files())
            stampSource += to + "\n" + ReadFile(from) + "\n";
        for (std::string const& t : r.Tables())
            if (std::find(tables.begin(), tables.end(), t) == tables.end())
                tables.push_back(t);
    }
    for (auto const& [name, id] : ids)
        stampSource += name + "=" + std::to_string(id) + "\n";
    bool const empty = tables.empty() && std::none_of(recipes.begin(), recipes.end(), [](Recipe const& r) { return !r.Files().empty(); });

    for (std::string const& locale : ClientArchives::Locales(clientDir))
    {
        fs::path const target = GameClient::Child(LocaleDir(data, locale), ArchiveName(locale));
        std::vector<fs::path> const sources = ClientArchives::Chain(clientDir, locale);

        std::string stamp;
        if (!empty)
        {
            std::string s = stampSource + locale;
            for (fs::path const& a : sources)
                s += "\n" + a.filename().string() + ":" + std::to_string(fs::file_size(a, ec));
            stamp = Hex(s);
        }

        // What is there now: our archive (with its stamp), someone else's, or nothing.
        std::optional<std::vector<uint8_t>> marker;
        bool exists = fs::exists(target, ec);
        if (exists)
            marker = Archive(target).Read(MarkerName);
        if (exists && !marker)
        {
            fs::path bak = target;
            bak += ".bak";
            if (!fs::exists(bak, ec))
            {
                fs::rename(target, bak, ec);
                res.log.push_back(Tr("patch.client.foreign_backed_up", locale, ArchiveName(locale)));
            }
            else
                fs::remove(target, ec);
            exists = false;
        }

        if (empty)
        {
            if (exists)
            {
                fs::remove(target, ec);
                res.changed = true;
                res.log.push_back(Tr("patch.client.removed", locale));
            }
            continue;
        }
        if (exists && marker)
        {
            std::string const m(marker->begin(), marker->end());
            if (m.substr(0, m.find('\n')) == stamp)
                continue;
        }

        std::vector<std::unique_ptr<Archive>> opened;
        for (fs::path const& a : sources)
            opened.push_back(std::make_unique<Archive>(a));

        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        std::string marks = stamp + "\n";
        for (Recipe const& r : recipes)
            marks += r.Plugin() + " " + r.Version() + "\n";

        for (std::string const& table : tables)
        {
            std::string const name = "DBFilesClient\\" + table;
            std::optional<std::vector<uint8_t>> raw;
            for (auto const& a : opened)
                if ((raw = a->Read(name)))
                    break;
            DbcRecipes::Table t;
            if (!raw || !t.Parse(*raw))
            {
                res.ok = false;
                res.error = Tr("patch.client.table_missing", locale, table);
                return res;
            }
            for (Recipe const& r : recipes)
                if (!r.Patch(table, t, ids, locale, nullptr, res.error))
                {
                    res.ok = false;
                    return res;
                }
            files.emplace_back(name, t.Write());
        }
        for (Recipe const& r : recipes)
            for (auto const& [to, from] : r.Files())
            {
                std::string const content = ReadFile(from);
                files.emplace_back(to, std::vector<uint8_t>(content.begin(), content.end()));
            }
        files.emplace_back(MarkerName, std::vector<uint8_t>(marks.begin(), marks.end()));
        opened.clear();

        if (!WriteArchive(target, files, res.error))
        {
            res.ok = false;
            res.error = locale + ": " + res.error;
            return res;
        }
        res.changed = true;
        res.log.push_back(Tr("patch.client.built", locale, ArchiveName(locale)));
    }
    return res;
}

ClientPatch::Result ClientPatch::SyncAddons(fs::path const& clientDir, std::vector<PluginManifest> const& plugins)
{
    Result res;
    std::error_code ec;
    fs::path const addons = GameClient::Child(GameClient::Child(clientDir, "Interface"), "AddOns");
    fs::path const list = addons / "lonelyice-addons.txt";

    std::vector<std::string> installed;
    for (PluginManifest const& p : plugins)
        for (fs::path const& a : p.addons)
        {
            std::string const name = a.filename().string();
            fs::path const dst = addons / a.filename();
            fs::remove_all(dst, ec);
            fs::create_directories(dst, ec);
            fs::copy(a, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                res.ok = false;
                res.error = Tr("patch.addon.copy_failed", name, ec.message());
                return res;
            }
            installed.push_back(name);
        }

    // Addons a removed plugin left behind.
    {
        std::ifstream in(list);
        for (std::string line; std::getline(in, line);)
            if (!line.empty() && std::find(installed.begin(), installed.end(), line) == installed.end())
            {
                fs::remove_all(addons / fs::u8path(line), ec);
                res.changed = true;
                res.log.push_back(Tr("patch.addon.removed", line));
            }
    }
    if (installed.empty())
        fs::remove(list, ec);
    else
    {
        std::ofstream out(list, std::ios::trunc);
        for (std::string const& n : installed)
            out << n << "\n";
    }
    return res;
}
