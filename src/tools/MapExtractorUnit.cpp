// map_extractor linked into LonelyIce. Its sources share global names with vmap4_extractor (MPQFile, DBCFile, input_path, ...),
// so they are compiled here inside their own namespace. Every header they pull from outside src/tools/map_extractor is
// included first, so the include guards keep those at global scope.

#include "MapExtractorUnit.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
// System headers System.cpp includes itself (per platform), pulled in here first so they stay at global scope.
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include "Define.h"
#include "StringFormat.h"
#include "libmpq/mpq.h"

namespace MapExtractor
{
#include "map_extractor/System.cpp"
#include "map_extractor/adt.cpp"
#include "map_extractor/wdt.cpp"
#include "map_extractor/dbcfile.cpp"
#include "map_extractor/loadlib.cpp"
#include "map_extractor/mpq_libmpq.cpp"
}

int MapExtractorMain(int argc, char** argv)
{
    return MapExtractor::main(argc, argv);
}

namespace
{
    // The extractor keeps its state in globals (open archives, map list, conversion buffers): one tile at a time.
    std::mutex _lock;
    bool _opened = false;
    std::unordered_map<uint32, std::string> _mapNames;      // map id -> folder under World\Maps
    std::unordered_map<uint32, std::vector<bool>> _tiles;   // map id -> which of its 64x64 ADTs exist (from the WDT)
    uint32 constexpr ClientBuild = 12340;

    // Whether the map has an ADT at (x, y) in WDT order; the WDT is read once per map. Under _lock.
    bool HasAdt(uint32 mapId, std::string const& name, uint32 x, uint32 y)
    {
        if (x >= WDT_MAP_SIZE || y >= WDT_MAP_SIZE)
            return false;

        auto it = _tiles.find(mapId);
        if (it == _tiles.end())
        {
            std::vector<bool> tiles(WDT_MAP_SIZE * WDT_MAP_SIZE);
            MapExtractor::WDT_file wdt;
            if (wdt.loadFile(Acore::StringFormat(R"(World\Maps\{}\{}.wdt)", name, name), false))
                for (uint32 ty = 0; ty < WDT_MAP_SIZE; ++ty)
                    for (uint32 tx = 0; tx < WDT_MAP_SIZE; ++tx)
                        tiles[ty * WDT_MAP_SIZE + tx] = wdt.main->adt_list[ty][tx].exist != 0;
            it = _tiles.emplace(mapId, std::move(tiles)).first;
        }
        return it->second[y * WDT_MAP_SIZE + x];
    }
}

namespace LonelyIce::AdtMaps
{
    bool Open(std::filesystem::path const& clientDir, std::string const& locale, std::string& error)
    {
        std::lock_guard guard(_lock);
        if (_opened)
            return true;

        int index = -1;
        for (int i = 0; i < LANG_COUNT; ++i)
            if (locale == MapExtractor::langs[i])
                index = i;
        if (index < 0)
        {
            error = "unknown client locale " + locale;
            return false;
        }

        std::string const dir = clientDir.generic_string();
        if (dir.size() >= sizeof(MapExtractor::input_path))
        {
            error = "client path is too long";
            return false;
        }
        std::strcpy(MapExtractor::input_path, dir.c_str());

        // Same order as the extractor: the archive opened last is searched first.
        MapExtractor::LoadLocaleMPQFiles(index);
        MapExtractor::LoadCommonMPQFiles();

        MapExtractor::DBCFile maps("DBFilesClient\\Map.dbc");
        MapExtractor::DBCFile liquids("DBFilesClient\\LiquidType.dbc");
        if (!maps.open() || !liquids.open())
        {
            MapExtractor::CloseMPQFiles();
            error = "Map.dbc or LiquidType.dbc is missing from the client's archives";
            return false;
        }

        for (std::size_t i = 0; i < maps.getRecordCount(); ++i)
            _mapNames[maps.getRecord(i).getUInt(0)] = maps.getRecord(i).getString(1);
        for (std::size_t i = 0; i < liquids.getRecordCount(); ++i)
            MapExtractor::LiquidTypes[liquids.getRecord(i).getUInt(0)].SoundBank = liquids.getRecord(i).getUInt(3);

        _opened = true;
        return true;
    }

    std::optional<std::vector<char>> Build(uint32 mapId, uint32 gridX, uint32 gridY)
    {
        std::lock_guard guard(_lock);
        auto name = _mapNames.find(mapId);
        if (!_opened || name == _mapNames.end())
            return std::nullopt;

        // .map files are named map, grid x, grid y; the ADT (and the WDT's table) the other way round.
        if (!HasAdt(mapId, name->second, gridY, gridX))
            return std::nullopt;

        std::string const adt = Acore::StringFormat(R"(World\Maps\{}\{}_{}_{}.adt)", name->second, name->second, gridY, gridX);
        std::vector<char> image;
        if (!MapExtractor::ConvertADT(adt, image, ClientBuild))
            return std::nullopt;
        return image;
    }

    bool Exists(uint32 mapId, uint32 gridX, uint32 gridY)
    {
        std::lock_guard guard(_lock);
        auto name = _mapNames.find(mapId);
        return _opened && name != _mapNames.end() && HasAdt(mapId, name->second, gridY, gridX);
    }
}
