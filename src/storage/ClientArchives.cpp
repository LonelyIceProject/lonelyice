#include "ClientArchives.h"
#include "GameClient.h"
#include <cctype>
#include <type_traits>

// StormLib takes archive paths as TCHAR: wchar_t on Windows (built with STORM_UNICODE), char elsewhere, which is what
// fs::path::c_str() gives on each. HANDLE and DWORD come from StormLib's port header outside Windows.
#ifdef _WIN32
#  ifndef UNICODE
#    define UNICODE
#  endif
#  ifndef _UNICODE
#    define _UNICODE
#  endif
#endif
#include <StormLib.h>

static_assert(std::is_same_v<TCHAR, std::filesystem::path::value_type>, "StormLib's TCHAR must match the path character type");

namespace fs = std::filesystem;

namespace LonelyIce::ClientArchives
{
    fs::path DataDir(fs::path const& clientDir)
    {
        return GameClient::Child(clientDir, "Data");
    }

    std::vector<std::string> Locales(fs::path const& clientDir)
    {
        std::vector<std::string> out;
        std::error_code ec;
        for (fs::directory_iterator it(DataDir(clientDir), ec), end; !ec && it != end; it.increment(ec))
        {
            std::string l = it->path().filename().string();
            if (l.size() != 4)
                continue;
            if (!fs::exists(GameClient::Child(it->path(), "locale-" + l + ".MPQ"), ec))
                continue;
            if (std::islower(static_cast<unsigned char>(l[2])) && std::islower(static_cast<unsigned char>(l[3])))
            {
                l[2] = char(std::toupper(static_cast<unsigned char>(l[2])));
                l[3] = char(std::toupper(static_cast<unsigned char>(l[3])));
            }
            out.push_back(l);
        }
        return out;
    }

    std::vector<fs::path> Chain(fs::path const& clientDir, std::string const& l)
    {
        fs::path const data = DataDir(clientDir);
        fs::path const localeDir = GameClient::Child(data, l);
        std::vector<fs::path> out;
        for (std::string n : { "patch-" + l + "-3", "patch-" + l + "-2", "patch-" + l, "lichking-locale-" + l,
                 "expansion-locale-" + l, "locale-" + l })
            out.push_back(GameClient::Child(localeDir, n + ".MPQ"));
        for (char const* n : { "patch-3", "patch-2", "patch", "lichking", "expansion", "common-2", "common" })
            out.push_back(GameClient::Child(data, std::string(n) + ".MPQ"));
        std::erase_if(out, [](fs::path const& p) { std::error_code ec; return !fs::exists(p, ec); });
        return out;
    }

    Archive::Archive(fs::path const& path)
    {
        HANDLE h = nullptr;
        // Files are looked up by name through the hash table, so the list of names is not needed.
        if (SFileOpenArchive(path.c_str(), 0, STREAM_FLAG_READ_ONLY | MPQ_OPEN_NO_LISTFILE | MPQ_OPEN_NO_ATTRIBUTES, &h))
            _handle = h;
    }

    Archive::~Archive()
    {
        if (_handle)
            SFileCloseArchive(_handle);
    }

    std::optional<std::vector<uint8_t>> Archive::Read(std::string const& name) const
    {
        HANDLE f = nullptr;
        if (!_handle || !SFileHasFile(_handle, name.c_str()) || !SFileOpenFileEx(_handle, name.c_str(), 0, &f))
            return std::nullopt;
        DWORD const size = SFileGetFileSize(f, nullptr);
        std::optional<std::vector<uint8_t>> data;
        if (size != SFILE_INVALID_SIZE)
        {
            data.emplace(size);
            DWORD read = 0;
            SFileReadFile(f, data->data(), size, &read, nullptr);
            data->resize(read);
        }
        SFileCloseFile(f);
        return data;
    }

    Reader::Reader(fs::path const& clientDir, std::string const& locale) : _locale(locale)
    {
        for (fs::path const& p : Chain(clientDir, locale))
        {
            auto archive = std::make_unique<Archive>(p);
            if (archive->IsOpen())
                _archives.push_back(std::move(archive));
        }
    }

    std::optional<std::vector<uint8_t>> Reader::Read(std::string const& name) const
    {
        std::lock_guard guard(_lock);
        for (auto const& a : _archives)
            if (auto data = a->Read(name))
                return data;
        return std::nullopt;
    }
}
