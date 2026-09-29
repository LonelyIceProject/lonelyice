#ifndef LONELYICE_MAPEXTRACTORUNIT_H
#define LONELYICE_MAPEXTRACTORUNIT_H

#include "Define.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

int MapExtractorMain(int argc, char** argv);

// Terrain tiles (.map files) built from the client's ADT files on demand, with the map extractor's code.
namespace LonelyIce::AdtMaps
{
    // Opens the client's archives of a locale ("ruRU") and reads its map list; once per process.
    bool Open(std::filesystem::path const& clientDir, std::string const& locale, std::string& error);

    // The .map image of a grid, std::nullopt when the map has no terrain there. One tile at a time, a few ms each.
    std::optional<std::vector<char>> Build(uint32 mapId, uint32 gridX, uint32 gridY);
    bool Exists(uint32 mapId, uint32 gridX, uint32 gridY);
}

#endif
