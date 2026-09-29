#ifndef LONELYICE_CLIENTDATA_H
#define LONELYICE_CLIENTDATA_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// The server's game data straight from the client, nothing unpacked: the DBC files as dbc_* tables (DbcTables),
// terrain tiles built from the ADT files when a grid first loads and the cinematic cameras, both through the core's
// DataFiles. Built tiles are kept in <DataDir>/maps like extracted ones (with a stamp of the client they came
// from), so later loads read them from there. Collision (vmaps) and paths (mmaps) still come from DataDir.
namespace LonelyIce::ClientData
{
    // locale empty: the client's first locale. Call once, before the databases are opened.
    bool Enable(std::filesystem::path const& clientDir, std::string const& locale, std::filesystem::path const& dataDir,
        std::string& error);

    // Locale the data is read in, empty when not enabled.
    std::string const& Locale();

    // Builds every terrain tile of the client into <dataDir>/maps (for the mmaps generator, which reads them all).
    bool BuildAllTiles(std::filesystem::path const& clientDir, std::string const& locale, std::filesystem::path const& dataDir,
        std::function<void(uint32_t done, uint32_t total)> const& progress, std::string& error);
}

#endif
