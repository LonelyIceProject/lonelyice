#include "ClientData.h"
#include "ClientArchives.h"
#include "DataFileSource.h"
#include "DbcTables.h"
#include "MapExtractorUnit.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace
{
    std::string _locale;

    // "maps/0001234.map": map 000, grid 12, 34.
    bool ParseMapFile(std::string const& path, uint32& mapId, uint32& gridX, uint32& gridY)
    {
        if (path.size() != 16 || path.compare(0, 5, "maps/") != 0 || path.compare(12, 4, ".map") != 0)
            return false;

        auto number = [&path](std::size_t pos, std::size_t len, uint32& out)
        {
            char const* begin = path.data() + pos;
            auto [end, ec] = std::from_chars(begin, begin + len, out);
            return ec == std::errc() && end == begin + len;
        };
        return number(5, 3, mapId) && number(8, 2, gridX) && number(10, 2, gridY);
    }

    std::optional<std::vector<char>> ReadFile(fs::path const& path)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
            return std::nullopt;
        std::streamoff const size = in.tellg();
        std::vector<char> data(size > 0 ? std::size_t(size) : 0);
        in.seekg(0);
        if (!in.read(data.data(), size))
            return std::nullopt;
        return data;
    }

    // Terrain tiles are built from the ADT files the first time a grid loads (a few ms each: the ADT is
    // decompressed) and kept in a cache folder, so later loads and later starts read them like extracted files.
    class TileCache
    {
    public:
        TileCache(fs::path dir, std::string const& stamp) : _dir(std::move(dir))
        {
            // Another client, locale or client update: the tiles may differ, start over.
            std::error_code ec;
            fs::path const stampFile = _dir / "stamp.txt";
            std::optional<std::vector<char>> old = ReadFile(stampFile);
            if (!old || std::string(old->begin(), old->end()) != stamp)
            {
                fs::remove_all(_dir, ec);
                fs::create_directories(_dir, ec);
                std::ofstream(stampFile, std::ios::binary) << stamp;
            }
        }

        std::optional<std::vector<char>> Get(std::string const& name, uint32 mapId, uint32 gridX, uint32 gridY) const
        {
            fs::path const file = _dir / name;
            if (std::optional<std::vector<char>> cached = ReadFile(file))
                return cached;

            std::optional<std::vector<char>> built = LonelyIce::AdtMaps::Build(mapId, gridX, gridY);
            if (built)
            {
                // Grids load on several threads: each writes its own temporary file, the rename is atomic.
                std::ostringstream tmpName;
                tmpName << name << '.' << std::this_thread::get_id() << ".tmp";
                fs::path const tmp = _dir / tmpName.str();
                std::error_code ec;
                {
                    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                    out.write(built->data(), std::streamsize(built->size()));
                    if (!out)
                        ec = std::make_error_code(std::errc::io_error);
                }
                if (!ec)
                    fs::rename(tmp, file, ec);
                if (ec)
                    fs::remove(tmp, ec);
            }
            return built;
        }

    private:
        fs::path _dir;
    };

    class ClientFiles final : public DataFileSource
    {
    public:
        ClientFiles(std::shared_ptr<LonelyIce::ClientArchives::Reader const> archives, std::unique_ptr<TileCache> tiles)
            : _archives(std::move(archives)), _tiles(std::move(tiles)) { }

        std::optional<std::vector<char>> Read(std::string const& path) override
        {
            uint32 mapId, gridX, gridY;
            if (ParseMapFile(path, mapId, gridX, gridY))
            {
                if (!LonelyIce::AdtMaps::Exists(mapId, gridX, gridY))
                    return std::nullopt;
                return _tiles->Get(path.substr(5), mapId, gridX, gridY);
            }

            // Everything else (Cameras/*.m2) is a file of the archives under its Windows name.
            std::string name = path;
            std::replace(name.begin(), name.end(), '/', '\\');
            std::optional<std::vector<uint8_t>> data = _archives->Read(name);
            if (!data)
                return std::nullopt;
            return std::vector<char>(data->begin(), data->end());
        }

        bool Exists(std::string const& path) override
        {
            uint32 mapId, gridX, gridY;
            if (ParseMapFile(path, mapId, gridX, gridY))
                return LonelyIce::AdtMaps::Exists(mapId, gridX, gridY);
            return Read(path).has_value();
        }

    private:
        std::shared_ptr<LonelyIce::ClientArchives::Reader const> _archives;
        std::unique_ptr<TileCache> _tiles;
    };

    // What the tiles are built from: the archives (name, size, time) and the locale.
    std::string Stamp(fs::path const& clientDir, std::string const& locale)
    {
        std::string stamp = "map tiles v2\n" + locale + "\n";
        for (fs::path const& archive : LonelyIce::ClientArchives::Chain(clientDir, locale))
        {
            std::error_code ec;
            auto const size = fs::file_size(archive, ec);
            auto const time = fs::last_write_time(archive, ec).time_since_epoch().count();
            stamp += archive.filename().string() + " " + std::to_string(size) + " " + std::to_string(time) + "\n";
        }
        return stamp;
    }
}

namespace LonelyIce::ClientData
{
    bool Enable(fs::path const& clientDir, std::string const& locale, fs::path const& cacheDir, std::string& error)
    {
        std::string l = locale;
        if (l.empty())
        {
            std::vector<std::string> const locales = ClientArchives::Locales(clientDir);
            if (locales.empty())
            {
                error = "no game client with locale archives in " + clientDir.string();
                return false;
            }
            l = locales.front();
        }

        auto archives = std::make_shared<ClientArchives::Reader const>(clientDir, l);
        if (!archives->IsOpen())
        {
            error = "cannot open the " + l + " archives of the game client in " + clientDir.string();
            return false;
        }

        if (!DbcTables::Enable(archives, error) || !AdtMaps::Open(clientDir, l, error))
            return false;

        auto tiles = std::make_unique<TileCache>(cacheDir / "maps", Stamp(clientDir, l));
        DataFiles::SetSource(std::make_shared<ClientFiles>(archives, std::move(tiles)));
        _locale = l;
        return true;
    }

    std::string const& Locale()
    {
        return _locale;
    }
}
