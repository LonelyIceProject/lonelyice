#ifndef LONELYICE_CLIENTDATA_H
#define LONELYICE_CLIENTDATA_H

#include <filesystem>
#include <string>

// The server's game data straight from the client, nothing extracted: the DBC files as dbc_* tables (DbcTables),
// terrain tiles built from the ADT files when a grid first loads (kept in cacheDir/maps) and the cinematic cameras,
// both through the core's DataFiles. Collision (vmaps) and paths (mmaps) still come from DataDir.
namespace LonelyIce::ClientData
{
    // locale empty: the client's first locale. Call once, before the databases are opened.
    bool Enable(std::filesystem::path const& clientDir, std::string const& locale, std::filesystem::path const& cacheDir,
        std::string& error);

    // Locale the data is read in, empty when not enabled.
    std::string const& Locale();
}

#endif
